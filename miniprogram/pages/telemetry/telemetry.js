const ble = require('../../utils/ble')

Page({
  data: {
    statusText: '未连接',
    deviceName: '',
    speed: '0.0',
    rpm: '0',
    voltage: '0.0',
    current: '0.0',
    protocolSpeed: '0',
    errorCode: '',
    unlockedText: '已锁定',
    // 诊断
    qCount: 0,
    aCount: 0,
    pulseCount: 0,
    badCs: 0,
    intervalMs: 0,
    pollHz: '-',
    connected: false,
    busy: false,
    frames: [],
    // 电池 (BMS 报文解析出来的)
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
  },

  onLoad() {
    this.battery = ble.createBatteryState()
    wx.onBLECharacteristicValueChange((res) => {
      if (!res || !res.value) return
      const uuid = (res.characteristicId || '').toLowerCase()
      if (uuid === ble.TELEMETRY_UUID.toLowerCase()) {
        this.onTelemetry(res.value)
      } else if (uuid === ble.FRAME_UUID.toLowerCase()) {
        this.onFrame(res.value)
      } else if (uuid === ble.BATTERY_UUID.toLowerCase()) {
        this.onBattery(res.value)
      }
    })
  },

  onUnload() {
    this.disconnect()
  },

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
      pollHz: pollHz,
    })
  },

  // 485 帧是分块推过来的 (一帧最长 64 字节, 蓝牙一包最多 20 字节).
  // 攒到 0x16 (帧尾) 为止算一整帧.
  onFrame(buffer) {
    if (!buffer || !buffer.byteLength) return
    const bytes = new Uint8Array(buffer)
    const prev = this.frameBuf || []
    const all = prev.concat(Array.from(bytes))
    const last = all[all.length - 1]
    if (last !== 0x16) {
      this.frameBuf = all
      return
    }
    this.frameBuf = []
    let hex = ''
    for (let i = 0; i < all.length; i++) {
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
    })
  },

  async connect() {
    if (this.data.busy) return
    this.setData({ busy: true, statusText: '正在搜索设备…', frames: [] })
    try {
      await ble.openAdapter()
      const devices = await ble.discover(10000)
      const dev = devices[0]
      this.setData({ deviceName: dev.name, statusText: '正在连接 ' + dev.name + ' …' })
      await ble.connect(dev.deviceId)
      this.deviceId = dev.deviceId

      await ble.subscribe(dev.deviceId, ble.TELEMETRY_UUID)
      await ble.subscribe(dev.deviceId, ble.FRAME_UUID)
      try {
        await ble.subscribe(dev.deviceId, ble.BATTERY_UUID)
      } catch (e) {
        // 老固件没有电池特征, 不影响看车速
        console.warn('没有电池特征', e)
      }

      this.setData({ connected: true, busy: false, statusText: '已连接' })
      this.prevQ = undefined
      this.prevQt = 0
      wx.showToast({ title: '已连接', icon: 'none' })
    } catch (e) {
      this.setData({ busy: false, connected: false, statusText: '未连接' })
      wx.showModal({ title: '连不上', content: e.message, showCancel: false })
    }
  },

  disconnect() {
    if (this.deviceId) {
      wx.closeBLEConnection({ deviceId: this.deviceId, fail: () => {} })
      this.deviceId = null
    }
    this.setData({ connected: false, statusText: '未连接' })
  },

  onDisconnect() {
    this.disconnect()
  },
})
