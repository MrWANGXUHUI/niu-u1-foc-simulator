/**
 * 电池趋势图 (canvas 2D)
 *
 * 画两块面板, **各自一个纵轴**:
 *   上: 电量 %   (固定 0~100)
 *   下: 总电压 V (按数据自动缩放)
 *
 * 为什么上下两块而不是叠一个图: 电量是 %, 电压是 V, 量纲不一样.
 * 硬塞进一张图就得搞双 Y 轴 —— 那是图表里最容易骗人的做法 (两条线的交叉点
 * 一点意义都没有). 分成两块小图 (small multiples) 就没这个问题.
 *
 * 其它几条规矩 (照着 dataviz 的方法走):
 *   - 每个面板只有一条线 → 不要图例, 面板标题就是它的名字
 *   - 线宽 2px, 网格和坐标轴是弱化的 (退到背景里)
 *   - 只标**最新值** (选择性直接标注), 不是每个点都标数字
 *   - 文字用中性色, 不用线的颜色
 *
 * 纯函数, 不依赖 wx —— 可以在 node 上测 (tools/test_vehicle_page.js 里用得上).
 */

const COLORS = {
  surface: '#232329',     // 卡片底色 (跟 app.wxss 一致)
  grid: '#2f2f36',        // 网格线: 弱化
  text: '#8e8e96',        // 次要文字
  textStrong: '#e8e8ea',  // 主要文字
  soc: '#3987e5',         // 电量 (分类色 slot 1, 深色模式)
  volt: '#199e70',        // 电压 (分类色 slot 3, 深色模式)
}

// 取一个"好看"的刻度上界: 向上取整到 1/2/5 × 10^n
function niceCeil(v) {
  if (v <= 0) return 1
  const exp = Math.floor(Math.log10(v))
  const base = Math.pow(10, exp)
  const n = v / base
  let step
  if (n <= 1) step = 1
  else if (n <= 2) step = 2
  else if (n <= 5) step = 5
  else step = 10
  return step * base
}

// 电压的纵轴范围: 包住所有点, 但至少留 ±1V 的余量, 免得电压一动一点就抖
function autoRange(values, minSpan) {
  if (!values.length) return { min: 0, max: 1 }
  let lo = Infinity, hi = -Infinity
  for (const v of values) {
    if (v < lo) lo = v
    if (v > hi) hi = v
  }
  let span = hi - lo
  if (span < minSpan) {
    const mid = (hi + lo) / 2
    lo = mid - minSpan / 2
    hi = mid + minSpan / 2
    span = minSpan
  }
  const pad = span * 0.15
  return { min: lo - pad, max: hi + pad }
}

/**
 * 画一个面板.
 * o = {x, y, w, h, title, unit, color, values:[], times:[], min, max, fmt}
 */
function drawPanel(ctx, o) {
  const vals = o.values

  // 网格 (弱化): 三条横线, 上下各一条, 中间一条
  ctx.setStrokeStyle(COLORS.grid)
  ctx.setLineWidth(1)
  for (let i = 0; i <= 2; i++) {
    const y = Math.round(o.y + (o.h * i) / 2) + 0.5
    ctx.beginPath()
    ctx.moveTo(o.x, y)
    ctx.lineTo(o.x + o.w, y)
    ctx.stroke()
  }

  // 标题 + 单位
  ctx.setFillStyle(COLORS.text)
  ctx.setFontSize(11)
  ctx.fillText(o.title + ' (' + o.unit + ')', o.x, o.y - 5)

  // 刻度 (只标上下两个, 中间不标 —— 少即是多)
  ctx.setFillStyle(COLORS.text)
  ctx.setFontSize(10)
  ctx.setTextAlign('right')
  ctx.fillText(o.fmt(o.max), o.x - 4, o.y + 8)
  ctx.fillText(o.fmt(o.min), o.x - 4, o.y + o.h + 3)
  ctx.setTextAlign('left')

  if (vals.length < 2) {
    ctx.setFillStyle(COLORS.text)
    ctx.fillText('还在收数据…', o.x + 6, o.y + o.h / 2)
    return
  }

  const t0 = o.times[0]
  const t1 = o.times[o.times.length - 1]
  const span = Math.max(t1 - t0, 1)
  const yOf = (v) => o.y + o.h * (1 - (v - o.min) / (o.max - o.min))
  const xOf = (t) => o.x + o.w * ((t - t0) / span)

  // 线
  ctx.setStrokeStyle(o.color)
  ctx.setLineWidth(2)
  ctx.beginPath()
  for (let i = 0; i < vals.length; i++) {
    const x = xOf(o.times[i])
    const y = yOf(vals[i])
    if (i === 0) ctx.moveTo(x, y)
    else ctx.lineTo(x, y)
  }
  ctx.stroke()

  // 只标最后一个点 + 它的值 (选择性直接标注)
  const lx = xOf(o.times[vals.length - 1])
  const ly = yOf(vals[vals.length - 1])
  ctx.setFillStyle(o.color)
  ctx.beginPath()
  ctx.arc(lx, ly, 3, 0, Math.PI * 2)
  ctx.fill()
  ctx.setFillStyle(COLORS.textStrong)
  ctx.setFontSize(11)
  const label = o.fmt(vals[vals.length - 1])
  const tw = label.length * 6 + 4
  // 值贴在点的左边 (点在最右边, 往右写会出界)
  ctx.fillText(label, Math.max(o.x, lx - tw - 6), Math.min(o.y + o.h, Math.max(o.y + 10, ly - 6)))
}

/**
 * 画整张图.
 * opt = {width, height, points: [{t, soc, volt}]}
 */
function drawBmsChart(ctx, opt) {
  const w = opt.width
  const h = opt.height
  ctx.clearRect(0, 0, w, h)
  ctx.setFillStyle(COLORS.surface)
  ctx.fillRect(0, 0, w, h)

  const pts = opt.points || []
  const padL = 42
  const padR = 12
  const padT = 20
  const padB = 14
  const plotW = Math.max(w - padL - padR, 10)
  const plotH = Math.max(h - padT - padB, 10)
  const socH = plotH * 0.56
  const gap = 26
  const voltY = padT + socH + gap
  const voltH = Math.max(plotH - socH - gap, 10)

  // 上: 电量 % (固定 0~100 —— 电量天生就是百分比, 缩放反而看不懂)
  drawPanel(ctx, {
    x: padL, y: padT, w: plotW, h: socH,
    title: '电量', unit: '%', color: COLORS.soc,
    values: pts.map((p) => p.soc),
    times: pts.map((p) => p.t),
    min: 0, max: 100,
    fmt: (v) => Math.round(v) + '',
  })

  // 下: 总电压 V (自动缩放)
  const volts = pts.map((p) => p.volt)
  const r = autoRange(volts, 2)
  drawPanel(ctx, {
    x: padL, y: voltY, w: plotW, h: voltH,
    title: '总电压', unit: 'V', color: COLORS.volt,
    values: volts,
    times: pts.map((p) => p.t),
    min: r.min, max: r.max,
    fmt: (v) => v.toFixed(1),
  })

  // 时间轴: 左端和右端各标一个时间
  if (pts.length >= 2) {
    ctx.setFillStyle(COLORS.text)
    ctx.setFontSize(10)
    const fmtT = (t) => {
      const d = new Date(t)
      const p = (n) => ('0' + n).slice(-2)
      return p(d.getHours()) + ':' + p(d.getMinutes())
    }
    ctx.fillText(fmtT(pts[0].t), padL, h - 3)
    ctx.setTextAlign('right')
    ctx.fillText(fmtT(pts[pts.length - 1].t), w - padR, h - 3)
    ctx.setTextAlign('left')
  }
}

module.exports = {
  COLORS,
  drawBmsChart,
  drawPanel,
  autoRange,
  niceCeil,
}
