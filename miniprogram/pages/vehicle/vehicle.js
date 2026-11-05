const ble = require('../../utils/ble')
const bleLink = require('../../utils/bleLink')
const chart = require('../../utils/chart')

Page({
  data: {
    // 连接
    statusText: '未连接',
    deviceName: '',
    fwVersion: '',
    busy: false,
    connected: false,
    connectError: '',

    // 车速
    speed: '0.0',
    rpm: '0',
    voltage: '0.0',
    current: '0.0',
    protocolSpeed: '0',
    errorCode: '',
    unlockedText: '已锁定',

    // 诊断 (折叠)
    showDiag: false,
    qCount: 0,
    aCount: 0,
    pulseCount: 0,
    badCs: 0,
    intervalMs: 0,
    glitchCount: 0,
    pollHz: '-',
    speedJitter: '-',   // 最近 5 秒车速的波动幅度 (km/h)

    // 电池
    bmsHasData: false,
    soc: 0,
    socWidth: '0%',
    bmsVoltage: '-',
    bmsCurrent: '-',
    bmsTemps: '',
    cellCount: 0,
    cellRange: '',
    cells: [],
    cellBars: [],
    bmsModel: '',
    // BMS 诊断 —— 回答"为什么没有电池数据"
    bmsDiagText: '',
    bmsDiagDetail: '',
    bmsAgeText: '',

    // 485 原始帧 (折叠)
    showFrames: false,
    frames: [],

    // 485 日志
    logBusy: false,
    logStatus: '',
    logSupported: true,    // 固件里有没有日志特征 (1.5.0 才有)
    logError: '',
  },

  onLoad() {
    this.battery = ble.createBatteryState()
    // 电池趋势图的数据点: 每 5 秒一个, 最多 120 个 (10 分钟)
    this.bmsPoints = []
    this.lastPointAt = 0
    this.chartTimer = null

    // 连接状态: 订阅全局管理器, 两个页面共享同一条连接
    this.unsubState = bleLink.onState((s) => {
      this.setData({
        statusText: s.status === 'connected' ? '已连接'
          : s.status === 'connecting' ? '连接中…'
          : s.status === 'error' ? '连接失败' : '未连接',
        deviceName: s.deviceName,
        fwVersion: s.fwVersion || '',
        connected: s.status === 'connected',
        busy: s.status === 'connecting',
        connectError: s.error || '',
        logSupported: s.hasLog !== false,
      })
    })

    // 遥测数据
    this.unsubData = bleLink.onData((kind, value) => {
      if (kind === 'telemetry') this.onTelemetry(value)
      else if (kind === 'frame') this.onFrame(value)
      else if (kind === 'battery') this.onBattery(value)
      else if (kind === 'log') this.onLogPacket(value)
    })
  },

  onShow() {
    // canvas 的内容切页面回来就没了, 重画一次
    if (this.data.bmsHasData) this.scheduleChart()
  },

  onUnload() {
    if (this.unsubState) this.unsubState()
    if (this.unsubData) this.unsubData()
    this.stopLogWatchdog()
  },

  // ---------- 连接 ----------
  async onConnect() {
    if (this.data.busy || this.data.connected) return
    this.setData({ busy: true, connectError: '' })
    try {
      await bleLink.connect()
    } catch (e) {
      // 错误信息 bleLink 里已经存好了, 状态订阅会带出来
      console.warn('连接失败', e.message)
    } finally {
      this.setData({ busy: false })
    }
  },

  onDisconnect() {
    bleLink.disconnect()
  },

  // ---------- 数据 ----------
  onTelemetry(buffer) {
    if (!buffer || buffer.byteLength < 12) return
    const t = ble.parseTelemetry(buffer)
    const now = Date.now()
    // 轮询速率 —— 计数是 16 位会回绕, 差值算负数就补一圈
    let pollHz = this.data.pollHz
    if (this.prevQ !== undefined) {
      let d = t.qCount - this.prevQ
      if (d < 0) d += 65536
      const dt = (now - this.prevQt) / 1000
      if (dt >= 0.5 && d >= 0) pollHz = (d / dt).toFixed(1) + ' Hz'
    }
    this.prevQ = t.qCount
    this.prevQt = now

    // 测速波动: 最近 5 秒 (25 次通知) 里车速的最大最小差.
    // 骑车时一眼看出稳不稳 —— 数值大就是传感器在抖/在漏脉冲.
    this.speedHist = this.speedHist || []
    this.speedHist.push(t.speed)
    if (this.speedHist.length > 25) this.speedHist.shift()
    let jitter = '-'
    if (this.speedHist.length >= 5) {
      let lo = Infinity, hi = -Infinity
      for (const v of this.speedHist) {
        if (v < lo) lo = v
        if (v > hi) hi = v
      }
      jitter = (hi - lo).toFixed(1)
    }

    this.setData({
      speed: t.speed.toFixed(1),
      rpm: String(t.rpm),
      voltage: t.voltage.toFixed(1),
      current: t.current.toFixed(1),
      protocolSpeed: String(t.protocolSpeed),
      errorCode: t.error ? '0x' + t.error.toString(16).toUpperCase().padStart(2, '0') : '',
      unlockedText: t.unlocked ? '已解锁' : '已锁定',
      qCount: t.qCount,
      aCount: t.aCount,
      pulseCount: t.pulseCount,
      badCs: t.badCs,
      intervalMs: t.intervalMs,
      glitchCount: t.glitchCount,
      pollHz: pollHz,
      speedJitter: jitter,
    })
  },

  // 485 帧是分块推过来的 (固件按 16 字节一块, 每块第 1 个字节是块序号).
  // 靠块序号拼, 不用"攒到 0x16 为止" —— 后者在丢块时会把两帧混成一坨,
  // 拼出来的十六进制根本不是一帧, 拿去分析只会误导人.
  onFrame(buffer) {
    if (!buffer || !buffer.byteLength) return
    const bytes = new Uint8Array(buffer)
    const idx = bytes[0]
    const data = bytes.subarray(1)
    if (!this.frameParts) this.frameParts = []
    if (idx === 0) {
      this.frameParts = [data]                 // 新的一帧
    } else if (this.frameParts.length === idx) {
      this.frameParts.push(data)              // 接着上一块
    } else {
      this.frameParts = []                     // 序号对不上 → 中间丢块了, 这帧作废
      return
    }
    let total = 0
    for (const p of this.frameParts) total += p.length
    const all = new Uint8Array(total)
    let off = 0
    for (const p of this.frameParts) { all.set(p, off); off += p.length }
    if (all[total - 1] !== 0x16) return        // 还没收完
    this.frameParts = []
    let hex = ''
    for (let i = 0; i < total; i++) {
      hex += (i ? ' ' : '') + ('0' + all[i].toString(16).toUpperCase()).slice(-2)
    }
    const frames = this.data.frames.concat([hex]).slice(-20)
    this.setData({ frames })
  },

  // 电池信息 —— 固件那边是分片消息, 这里攒齐了再显示
  onBattery(buffer) {
    ble.handleBatteryChunk(this.battery, buffer, (state) => this.applyBattery(state))
  },

  applyBattery(state) {
    const s = state.status
    const cells = state.cells || []
    const d = state.diag
    // 趋势图: 每 5 秒收一个点. 电量变化很慢, 不用每个通知都收.
    if (s && s.hasData) {
      const now = Date.now()
      if (now - this.lastPointAt > 5000) {
        this.lastPointAt = now
        this.bmsPoints.push({ t: now, soc: s.soc, volt: s.voltage })
        while (this.bmsPoints.length > 120) this.bmsPoints.shift()
        this.scheduleChart()
      }
    }
    // 每节单体画一根柱子, 高度按最低/最高归一化
    const bars = []
    if (cells.length) {
      const min = Math.min.apply(null, cells)
      const max = Math.max.apply(null, cells)
      const span = Math.max(max - min, 0.02)   // 防止全一样时除零
      for (let i = 0; i < cells.length; i++) {
        const h = 20 + Math.round(((cells[i] - min) / span) * 60)
        bars.push({ i: i, v: cells[i].toFixed(3), h: h + 'rpx' })
      }
    }
    this.setData({
      bmsHasData: !!(s && s.hasData),
      soc: s ? s.soc : 0,
      socWidth: (s ? s.soc : 0) + '%',
      bmsVoltage: s ? s.voltage.toFixed(1) : '-',
      bmsCurrent: s ? s.current.toFixed(1) : '-',
      bmsTemps: s ? s.temps.map((t) => t + '°').join(' / ') : '',
      cellCount: s ? s.cellCount : 0,
      cellRange: s && s.cellCount
        ? (s.minCellMv / 1000).toFixed(3) + ' ~ ' + (s.maxCellMv / 1000).toFixed(3) + ' V'
        : '',
      cells: cells,
      cellBars: bars,
      bmsModel: state.model || '',
      // 诊断 —— 电池没数据时, 这几行就是答案:
      //   总线 0 帧 = BMS 压根没在总线上回话
      //   总线有帧但成功 0 = 收到了但被干扰/撞坏 (校验和错)
      //   成功 > 0 = 固件这边正常, 那是蓝牙这一段的问题
      bmsDiagText: d ? `总线 ${d.rxFrames} 帧 · 解析成功 ${d.okFrames}` : '',
      bmsDiagDetail: d ? `校验错 ${d.csErr} · 长度错 ${d.lenErr} · 未知长度 ${d.unknownLen}` : '',
      bmsAgeText: d
        ? (d.ageMs < 0 ? '从没收到过 BMS 应答' : `距上次 BMS 应答 ${(d.ageMs / 1000).toFixed(1)} 秒`)
        : '',
    })
  },

  // ---------- 485 日志 ----------
  // 手机上看到的 485 帧只是"每 200ms 最后一帧"的采样, 总线实际每秒几十帧,
  // 采样存下来没法分析. 所以日志是**设备端**记的 (板子上是全知视角),
  // 点保存的时候通过蓝牙把整份日志拉回来, 存成文件发到电脑.

  async onSaveLog() {
    if (this.data.logBusy) return
    if (!this.data.connected) {
      wx.showToast({ title: '还没连上设备', icon: 'none' })
      return
    }
    if (!this.data.logSupported) {
      // 固件里没有日志特征 —— 推上去的固件太老
      wx.showModal({
        title: '固件太老',
        content: '车上的固件没有日志功能 (1.5.0 才有). 先去「升级」页把固件推上去.',
        showCancel: false,
      })
      return
    }
    this.logBuf = ''
    this.logTotal = 0
    this.logDone = false
    this.setData({ logBusy: true, logStatus: '正在从设备拉取 485 日志…', logError: '' })
    this.startLogWatchdog()
    try {
      await bleLink.requestLog(0, false)
    } catch (e) {
      this.stopLogWatchdog()
      this.setData({ logBusy: false, logStatus: '拉取失败: ' + e.message })
    }
  },

  // 看门狗: 迟迟收不到包就报错, 别让按钮一直转
  // (以前就是"永远在导出中", 不知道卡在哪一步 —— 这次至少要说清楚)
  startLogWatchdog() {
    this.stopLogWatchdog()
    this.logTimer = setTimeout(() => {
      this.logTimer = null
      if (this.logDone) return
      const got = this.logBuf ? this.logBuf.length : 0
      this.setData({
        logBusy: false,
        logError: got
          ? `只收到 ${got} 字节就没下文了 (设备可能中途断开了)`
          : '设备一个包都没发过来 —— 检查固件版本是不是 1.5.0 以上',
      })
      this.setData({ logStatus: '' })
    }, 12000)
  },

  stopLogWatchdog() {
    if (this.logTimer) {
      clearTimeout(this.logTimer)
      this.logTimer = null
    }
  },

  // 一个日志包
  onLogPacket(buffer) {
    const pkt = ble.parseLogPacket(buffer)
    if (!pkt) return
    if (this.logDone) return          // 已经拉完了, 别重复弹保存
    if (this.logBuf === undefined) this.logBuf = ''
    if (pkt.offset !== this.logBuf.length) {
      // 中间丢了一块 —— 让设备从我们收到的位置重发 (日志在设备那边是冻结的, 补得回来)
      console.warn('485 日志有缺口', pkt.offset, this.logBuf.length)
      bleLink.requestLog(this.logBuf.length, false).catch(() => {})
      return
    }
    this.logBuf += pkt.text
    this.logTotal = pkt.total
    this.stopLogWatchdog()   // 有动静了, 重新计时
    this.setData({
      logStatus: `拉取中 ${(this.logBuf.length / 1024).toFixed(1)} / ${(pkt.total / 1024).toFixed(1)} KB`,
    })
    if (pkt.last) {
      this.logDone = true
      this.finishLog()
    }
  },

  async finishLog() {
    this.stopLogWatchdog()
    const body = this.logBuf || ''
    const lines = body.split('\n').filter((l) => l.length)
    const header = [
      '# NIU-U1 485 总线日志',
      '# 固件 ' + (this.data.fwVersion || '未知') + '   导出 ' + new Date().toLocaleString(),
      '# 帧数 ' + lines.length,
      '# 格式: RX/TX <毫秒时间戳> <帧字节...>    RX = 总线上收到的, TX = 我们自己回的',
      '',
    ].join('\n')
    const text = header + body
    const name = 'niu485-' + this.fileStamp() + '.log'
    this.setData({ logStatus: '正在保存到手机…' })
    try {
      const path = await ble.saveLogFile(text, name)
      this.logFilePath = path
      this.setData({
        logBusy: false,
        logStatus: `已保存 ${lines.length} 帧 (${(text.length / 1024).toFixed(1)} KB)`,
      })
      // 告诉设备拉完了, 让它解冻日志恢复记录
      bleLink.requestLog(0, true).catch(() => {})
      this.offerExport(text, name, path)
    } catch (e) {
      this.setData({ logBusy: false, logStatus: '保存失败: ' + e.message })
    }
  },

  // 日志躺在小程序的沙箱里, 电脑上是没有的 —— 唯一能弄出去的办法是发进微信聊天.
  offerExport(text, name, path) {
    wx.showActionSheet({
      itemList: ['发送到微信 (发到文件传输助手)', '复制文本到剪贴板'],
      success: (res) => {
        if (res.tapIndex === 0) {
          ble.shareLogFile(path, name).catch((e) => {
            wx.showModal({
              title: '分享失败',
              content: e.message + '\n\n可以改用「复制文本到剪贴板」, 一样能粘到电脑上分析.',
              showCancel: false,
            })
          })
        } else if (res.tapIndex === 1) {
          ble.copyText(text)
            .then(() => wx.showToast({ title: '已复制', icon: 'success' }))
            .catch((e) => wx.showToast({ title: e.message, icon: 'none' }))
        }
      },
      fail: () => {},
    })
  },

  fileStamp() {
    const d = new Date()
    const p = (n) => ('0' + n).slice(-2)
    return '' + d.getFullYear() + p(d.getMonth() + 1) + p(d.getDate()) +
      '-' + p(d.getHours()) + p(d.getMinutes()) + p(d.getSeconds())
  },

  // ---------- 电池趋势图 ----------
  scheduleChart() {
    if (this.chartTimer) return
    this.chartTimer = setTimeout(() => {
      this.chartTimer = null
      this.drawChart()
    }, 300)
  },

  drawChart() {
    if (!this.data.bmsHasData) return
    // 先问画布有多大 (rpx → px 要按实际布局算), 再画
    wx.createSelectorQuery().in(this).select('#bmsChart').boundingClientRect((rect) => {
      if (!rect || !rect.width || !rect.height) return
      const ctx = wx.createCanvasContext('bmsChart', this)
      chart.drawBmsChart(ctx, {
        width: rect.width,
        height: rect.height,
        points: this.bmsPoints || [],
      })
      ctx.draw()
    }).exec()
  },

  // ---------- 折叠 ----------
  toggleDiag() {
    this.setData({ showDiag: !this.data.showDiag })
  },

  toggleFrames() {
    this.setData({ showFrames: !this.data.showFrames })
  },
})
