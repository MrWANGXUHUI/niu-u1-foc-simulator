const ota = require('../../utils/ota')
const firmwareUtil = require('../../utils/firmware')
const app = getApp()

function fmtSize(n) {
  if (!n && n !== 0) return ''
  if (n < 1024) return n + ' B'
  if (n < 1024 * 1024) return (n / 1024).toFixed(0) + ' KB'
  return (n / 1024 / 1024).toFixed(2) + ' MB'
}

Page({
  data: {
    ip: '192.168.4.1',
    port: 8080,
    apSsid: '',
    apPassword: '',
    statusText: '未连接',
    info: null,
    heapText: '',
    firmware: null,
    sizeText: '',
    busy: false,
    canUpdate: false,
    progress: 0,
    sentText: '',
    speedText: '',
    phaseText: '',
    logAnchor: '',
    logs: [],
  },

  onLoad() {
    const g = app.globalData
    this.setData({
      ip: g.deviceIp,
      port: g.port,
      apSsid: g.apSsid,
      apPassword: g.apPassword,
    })
    this.loadBuiltin()
    this.probe()
    // 小程序没法自动连 WiFi (微信没有这个 API), 所以只能一直探:
    // 用户在手机上连上热点的那一刻, 页面自己就连上了, 不用再点"重新检测"
    this.probeTimer = setInterval(() => {
      if (this.data.info) return
      this.probe(false, true)
    }, 2000)
  },

  onUnload() {
    if (this.probeTimer) clearInterval(this.probeTimer)
  },

  // ---------- 日志 ----------
  log(msg) {
    const logs = this.data.logs.concat(['[' + new Date().toLocaleTimeString() + '] ' + msg]).slice(-50)
    this.setData({ logs: logs, logAnchor: 'log' + (logs.length - 1) })
  },

  // ---------- 设备 ----------
  async probe(isAfterUpdate, silent) {
    const { ip, port } = this.data
    if (!silent) this.setData({ info: null, statusText: '连接中…', canUpdate: false })
    try {
      const info = await ota.getInfo(ip, port)
      this.setData({
        info: info,
        statusText: '已连接',
        heapText: info.freeHeap ? fmtSize(info.freeHeap) : '',
        canUpdate: !!this.data.firmware,
      })
      if (!silent) {
        this.log('设备在线: v' + info.version + ' (' + info.slot + ' 分区, ' + info.state + ')')
      }
      if (isAfterUpdate) {
        this.log('升级完成, 现在跑的是 v' + info.version)
        wx.showToast({ title: '升级完成 v' + info.version, icon: 'none' })
      }
    } catch (e) {
      this.setData({ info: null, statusText: '未连接', canUpdate: false })
      if (!silent) {
        this.log('连不上设备: ' + e.message + ' (手机连上 ' + this.data.apSsid + ' 了吗?)')
      }
    }
  },

  onProbe() {
    this.probe()
  },

  // 复制热点名和密码 —— 连热点得手动点, 这一步能省一次手打
  copyApInfo() {
    const text = 'WiFi: ' + this.data.apSsid + '\n密码: ' + this.data.apPassword
    wx.setClipboardData({
      data: text,
      success: () => wx.showToast({ title: '已复制', icon: 'none' }),
    })
  },

  goTelemetry() {
    wx.navigateTo({ url: '/pages/telemetry/telemetry' })
  },

  onIpInput(e) {
    this.setData({ ip: e.detail.value })
  },

  // ---------- 固件 ----------
  loadBuiltin() {
    try {
      const fw = firmwareUtil.readBuiltin()
      this.firmwareBuffer = fw.buffer
      this.setData({
        firmware: fw.meta,
        sizeText: fmtSize(fw.meta.size),
        canUpdate: !!this.data.info,
      })
      this.log('已载入内置固件 v' + fw.meta.version + ' (' + fmtSize(fw.meta.size) + ')')
    } catch (e) {
      this.log(e.message)
    }
  },

  useBuiltin() {
    this.loadBuiltin()
  },

  async pickFile() {
    try {
      const fw = await firmwareUtil.chooseFromChat()
      this.firmwareBuffer = fw.buffer
      this.setData({
        firmware: fw.meta,
        sizeText: fmtSize(fw.meta.size),
        canUpdate: !!this.data.info,
      })
      this.log('已选择 ' + fw.meta.name + ' (' + fmtSize(fw.meta.size) + ')')
    } catch (e) {
      if (e.message !== 'cancelled') this.log('选择文件失败: ' + e.message)
    }
  },

  // ---------- 升级 ----------
  async startUpdate() {
    const { firmware, ip, port } = this.data
    if (!firmware) return
    if (!this.data.info) {
      wx.showToast({ title: '设备没连上', icon: 'none' })
      return
    }
    const buffer = this.firmwareBuffer
    if (!buffer) {
      this.log('没有固件内容, 重新选一次固件')
      return
    }
    const size = buffer.byteLength

    this.setData({
      busy: true, canUpdate: false,
      progress: 0, sentText: '0 B', speedText: '', phaseText: '准备中',
    })
    this.log('开始升级: ' + firmware.name + ' (' + fmtSize(size) + ')')

    const t0 = Date.now()
    try {
      await ota.begin(ip, port, size, firmware.md5)
      this.log('设备已就绪, 传数据中')

      let off = 0
      while (off < size) {
        const len = Math.min(ota.CHUNK_SIZE, size - off)
        const b64 = firmwareUtil.toBase64(buffer, off, len)
        await ota.chunk(ip, port, off, b64)
        off += len

        const elapsed = (Date.now() - t0) / 1000
        const speed = elapsed > 0 ? off / elapsed : 0
        this.setData({
          sentText: fmtSize(off),
          progress: Math.floor((off * 100) / size),
          speedText: speed > 0 ? fmtSize(speed) + '/s' : '',
        })
      }

      await ota.end(ip, port)
      this.setData({ phaseText: '设备重启中' })
      this.log('传输完成, 设备正在重启…')
      setTimeout(() => this.probe(true), 6000)
    } catch (e) {
      this.log('失败: ' + e.message)
      this.setData({ phaseText: '失败: ' + e.message })
      ota.abort(ip, port)
      wx.showModal({
        title: '升级失败',
        content: e.message + '\n\n设备会继续跑原来的固件, 可以重试。',
        showCancel: false,
      })
    } finally {
      this.setData({ busy: false, canUpdate: !!this.data.info && !!this.data.firmware })
    }
  },
})
