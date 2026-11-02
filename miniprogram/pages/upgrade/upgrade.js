const ota = require('../../utils/ota')
const firmwareUtil = require('../../utils/firmware')
const ble = require('../../utils/ble')
const bleLink = require('../../utils/bleLink')
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

    // 三步的状态
    stepAp: 'todo',       // todo | doing | done
    stepWifi: 'todo',     // todo | doing | done
    stepSend: 'todo',     // todo | doing | done
    bleConnected: false,   // 蓝牙连上了没有 (第 1 步要用)
    hasCommand: false,     // 固件有没有命令特征 (1.4.0 才有; 没有的话热点本来就开着)
    bleVersion: '',        // 蓝牙问来的固件版本 (不用连热点也能看)

    // 设备信息 (通过 HTTP 拿到)
    info: null,
    heapText: '',
    deviceVersion: '',
    upgraded: false,       // 刚升级完 (设备重启后热点会关, 只能靠蓝牙确认)

    // 固件
    firmware: null,
    sizeText: '',
    useCustom: false,

    // 传输进度
    busy: false,
    progress: 0,
    sentText: '',
    speedText: '',
    phaseText: '',

    // 日志 (折叠)
    showLog: false,
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

    // 蓝牙连接状态 —— 步骤①要用
    this.unsubState = bleLink.onState((s) => {
      this.setData({
        bleConnected: s.status === 'connected',
        hasCommand: !!s.hasCommand,
        bleVersion: s.fwVersion || '',
      })
      // 老固件 (1.3.x) 没有命令特征, 热点本来就是开的 —— 直接跳到第②步.
      // 这条路是给"第一次用新小程序"的用户准备的: 车上的固件还没升到 1.4.0,
      // 只能靠老办法 (手机直接连 NIU-U1-OTA 热点) 推第一个新固件.
      if (s.status === 'connected' && !s.hasCommand && this.data.stepAp === 'todo') {
        this.setData({ stepAp: 'done', stepWifi: 'doing' })
        this.log('设备固件较老, 热点默认就是开的, 跳过开热点这一步')
      }
    })

    // 设备有没有连上热点, 靠 HTTP 探测 (小程序读不到手机的 WiFi 状态).
    // 只在第①步完成之后才探 —— 之前探也探不通 (手机还没连热点, 没有 192.168.4.x 网段),
    // 白白发请求.
    this.probeTimer = setInterval(() => {
      if (this.data.stepAp === 'done' && this.data.stepWifi !== 'done') {
        this.probe(true)
      }
    }, 2000)
  },

  onUnload() {
    if (this.probeTimer) clearInterval(this.probeTimer)
    if (this.unsubState) this.unsubState()
  },

  // ---------- 日志 ----------
  log(msg) {    const logs = this.data.logs.concat(['[' + new Date().toLocaleTimeString() + '] ' + msg]).slice(-50)
    this.setData({ logs: logs, logAnchor: 'log' + (logs.length - 1) })
  },

  toggleLog() {
    this.setData({ showLog: !this.data.showLog })
  },

  // 去车辆页连蓝牙 —— 第①步要用
  goVehicle() {
    wx.switchTab({ url: '/pages/vehicle/vehicle' })
  },

  // ---------- 步骤①: 用蓝牙开热点 ----------

  // 设备的 WiFi 默认是关的 (省电, 而且手机连热点会断网).
  // 通过蓝牙发一个字节把热点临时打开 —— 蓝牙这条路本来就是通的 (看车速就走它).
  //
  // 判定"热点开好了"的依据是**蓝牙命令写成功**, 不是 HTTP 探测:
  // 手机这时还没连热点, 没有 192.168.4.x 网段, HTTP 必然不通, 分不清是
  // "热点没开"还是"手机没连热点". 命令写成功 = 固件收到并执行了 = 热点会开.
  async startAp() {
    if (this.data.stepAp === 'doing') return
    this.setData({ stepAp: 'doing' })
    this.log('正在通过蓝牙让设备打开热点…')
    try {
      // 没连蓝牙就先连上 (和车辆页共享同一条连接)
      const s = bleLink.getState()
      if (s.status !== 'connected') {
        this.log('还没连设备, 先连蓝牙…')
        await bleLink.connect()
      }
      // 老固件没有命令特征 —— 热点本来就是开的, 不用开
      if (!bleLink.getState().hasCommand) {
        this.log('设备固件较老, 热点默认就是开的')
        this.setData({ stepAp: 'done', stepWifi: 'doing' })
        return
      }
      await bleLink.sendCommand(ble.CMD_START_AP)
      this.log('命令已发出, 设备正在起热点…')
      this.setData({ stepAp: 'done', stepWifi: 'doing' })
      wx.showToast({ title: '热点已开, 去连 WiFi', icon: 'none' })
    } catch (e) {
      this.setData({ stepAp: 'todo' })
      this.log('开热点失败: ' + e.message)
      wx.showModal({ title: '开热点失败', content: e.message, showCancel: false })
    }
  },

  // ---------- 步骤②: 手机连热点 ----------

  copyApInfo() {
    const text = this.data.apSsid + '\n' + this.data.apPassword
    wx.setClipboardData({
      data: text,
      success: () => wx.showToast({ title: '已复制', icon: 'none' }),
    })
  },

  // 手动再探一次 —— 自动探测每 2 秒一次, 但用户刚连上 WiFi 时想立刻看到反馈
  onProbe() {
    this.probe(false)
  },

  // ---------- 步骤③: 传固件 ----------

  // 探设备在不在. 手机的 WiFi 连上热点之后这里就会通
  async probe(silent) {
    const { ip, port } = this.data
    try {
      const info = await ota.getInfo(ip, port)
      const first = !this.data.info
      this.setData({
        info: info,
        deviceVersion: info.version || '',
        heapText: info.freeHeap ? fmtSize(info.freeHeap) : '',
        stepWifi: 'done',
        stepSend: this.data.stepSend === 'done' ? 'done' : 'doing',
      })
      if (first && !silent) {
        this.log('设备在线: v' + info.version + ' (' + info.slot + ' 分区)')
      }
    } catch (e) {
      if (!silent) {
        this.log('还没连上设备: ' + e.message)
      }
    }
  },

  // ---------- 固件来源 ----------
  loadBuiltin() {
    try {
      const fw = firmwareUtil.readBuiltin()
      this.firmwareBuffer = fw.buffer
      this.setData({
        firmware: fw.meta,
        sizeText: fmtSize(fw.meta.size),
        useCustom: false,
      })
    } catch (e) {
      this.log(e.message)
    }
  },

  async pickFile() {
    try {
      const fw = await firmwareUtil.chooseFromChat()
      this.firmwareBuffer = fw.buffer
      this.setData({
        firmware: fw.meta,
        sizeText: fmtSize(fw.meta.size),
        useCustom: true,
      })
      this.log('已选择 ' + fw.meta.name + ' (' + fmtSize(fw.meta.size) + ')')
    } catch (e) {
      if (e.message !== 'cancelled') this.log('选择文件失败: ' + e.message)
    }
  },

  // ---------- 升级 ----------
  async startUpdate() {
    const { firmware, ip, port } = this.data
    if (!firmware || !this.firmwareBuffer) {
      wx.showToast({ title: '没有固件', icon: 'none' })
      return
    }
    if (!this.data.info) {
      wx.showToast({ title: '设备还没连上', icon: 'none' })
      return
    }
    const buffer = this.firmwareBuffer
    const size = buffer.byteLength

    this.setData({
      busy: true,
      progress: 0, sentText: '0 B', speedText: '', phaseText: '准备中',
      stepSend: 'doing',
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

      // ota.end() 返回就说明: 固件已写入、校验通过、启动分区已切好、设备正在重启.
      // 不能再靠 HTTP 探测确认 —— 重启后设备的 WiFi 是关的 (默认关), 探测必然失败.
      await ota.end(ip, port)
      // 设备重启 → 热点自动关 → 之前那份 info 作废了 (HTTP 探测不再通).
      // 把 info 清掉, "开始升级"按钮自动变灰, 免得用户点了撞一鼻子灰.
      this.setData({
        phaseText: '设备重启中', stepSend: 'done', stepWifi: 'done',
        upgraded: true, info: null,
      })
      this.log('传输完成, 设备正在重启, 新固件马上生效')
      wx.showModal({
        title: '升级完成',
        content: '设备正在重启, 新固件已生效.\n\n手机的 WiFi 还连着 NIU-U1-OTA, ' +
          '而设备的热点已经关掉了 —— 记得切回自己的 WiFi.\n蓝牙不受影响, 随时可以看车速.',
        showCancel: false,
        confirmText: '知道了',
      })
    } catch (e) {
      this.log('失败: ' + e.message)
      this.setData({ phaseText: '失败: ' + e.message, stepSend: 'todo' })
      ota.abort(ip, port)
      wx.showModal({
        title: '升级失败',
        content: e.message + '\n\n设备会继续跑原来的固件, 可以重试。',
        showCancel: false,
      })
    } finally {
      this.setData({ busy: false })
    }
  },
})
