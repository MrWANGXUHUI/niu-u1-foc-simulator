// 趋势图的坐标换算自测 (node): 画布是假的, 只看算出来的坐标对不对
//
//   node tools/test_chart.js

const chart = require('../miniprogram/utils/chart.js')

let fail = 0
function check(name, got, want) {
  const ok = JSON.stringify(got) === JSON.stringify(want)
  if (!ok) fail++
  console.log((ok ? '✓' : '✗') + ' ' + name +
    (ok ? '' : '  实际=' + JSON.stringify(got) + ' 期望=' + JSON.stringify(want)))
}

// 假的 canvas 上下文: 把画线记下来
function mockCtx() {
  const c = {
    strokes: [], moves: [], lines: [], texts: [], fills: [],
    setStrokeStyle(s) { c._stroke = s },
    setLineWidth(w) { c._lw = w },
    setFillStyle(s) { c._fill = s },
    setFontSize(s) { c._fs = s },
    setTextAlign(s) { c._align = s },
    beginPath() { c._cur = { color: c._stroke, lw: c._lw, pts: [] } },
    moveTo(x, y) { c._cur.pts.push([x, y]) },
    lineTo(x, y) { c._cur.pts.push([x, y]) },
    stroke() { c.strokes.push(c._cur) },
    arc(x, y, r) { c.fills.push({ x, y, r, color: c._fill }) },
    fill() { if (c._cur && c._cur.pts.length) c.fills.push({ dot: c._cur.pts[0], color: c._fill }) },
    fillRect(x, y, w, h) { c.fills.push({ rect: [x, y, w, h], color: c._fill }) },
    fillText(t, x, y) { c.texts.push({ t, x, y }) },
    clearRect() {},
  }
  return c
}

const W = 320
const H = 140
const now = Date.now()
const points = []
for (let i = 0; i < 20; i++) {
  points.push({ t: now + i * 5000, soc: 80 - i * 0.5, volt: 48.6 - i * 0.05 })
}

const ctx = mockCtx()
chart.drawBmsChart(ctx, { width: W, height: H, points })

// 1) 折线: 找那条点数最多的 (20 个点的就是数据线)
const dataLine = ctx.strokes.filter((s) => s.pts.length >= 20).sort((a, b) => b.pts.length - a.pts.length)[0]
check('画了两条数据线', ctx.strokes.filter((s) => s.pts.length >= 20).length, 2)

// 2) 电量线: 80% 应该在图的**上半** (0~100 的刻度), 且从左到右 y 变小 (电量在降)
const socLine = dataLine
const first = socLine.pts[0]
const last = socLine.pts[socLine.pts.length - 1]
check('电量线: 从左到右向下走 (电量在降)', last[1] > first[1], true)
check('电量线: 线宽 2px', socLine.lw, 2)
check('电量线: 颜色是分类色 slot 1', socLine.color, chart.COLORS.soc)

// 3) 所有坐标都在画布内
const allPts = ctx.strokes.flatMap((s) => s.pts)
const inBounds = allPts.every((p) => p[0] >= 0 && p[0] <= W && p[1] >= 0 && p[1] <= H)
check('所有坐标都在画布内', inBounds, true)

// 4) 面板标题都在 (两个面板各一个标题 + 时间轴两个)
const titles = ctx.texts.map((t) => t.t)
check('有电量面板标题', titles.indexOf('电量 (%)') >= 0, true)
check('有电压面板标题', titles.indexOf('总电压 (V)') >= 0, true)

// 5) 电量轴是 0~100 固定刻度
check('电量刻度: 上限 100', titles.indexOf('100') >= 0, true)
check('电量刻度: 下限 0', titles.indexOf('0') >= 0, true)

// 6) 自动缩放: 电压全一样时也要留出余量 (免得一动就抖)
const r1 = chart.autoRange([48.6, 48.6, 48.6], 2)
// 相同值: 先撑开 ±1V, 再各留 15% 余量 → 2 + 2*0.3 = 2.6
check('电压范围: 相同值时留 ±1V (再加 15% 余量)', (r1.max - r1.min).toFixed(1), '2.6')
const r2 = chart.autoRange([48.0, 49.0], 2)
check('电压范围: 包住所有点', r2.min < 48.0 && r2.max > 49.0, true)

// 7) 数据点少的时候别画崩
const ctx2 = mockCtx()
chart.drawBmsChart(ctx2, { width: W, height: H, points: [] })
check('没数据时不崩', ctx2.texts.length > 0, true)
const ctx3 = mockCtx()
chart.drawBmsChart(ctx3, { width: W, height: H, points: [{ t: now, soc: 50, volt: 48 }] })
check('只有一个点时不崩', ctx3.texts.length > 0, true)

console.log('\n' + (fail ? `✗ ${fail} 项没过` : '✓ 全部通过'))
process.exit(fail ? 1 : 0)
