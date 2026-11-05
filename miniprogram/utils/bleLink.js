/**
 * 全局 BLE 连接管理器 (单例)
 *
 * 为什么要有这个: 小程序有两个页面 (车辆 / 升级), 两个都要蓝牙.
 * 升级页要通过蓝牙开关设备的 WiFi 热点 —— 如果连接状态只存在页面里,
 * 两个页面各连一次、互相踢掉, 用户一脸懵. 所以连接提到全局, 页面只订阅状态.
 *
 * 另外顺带修掉一个老 bug: 通知回调 (wx.onBLECharacteristicValueChange) 原来是在
 * 页面 onLoad 里注册的, 但 onUnload 没注销 —— 页面来回进出就会重复注册,
 * 同一批数据被处理多次. 现在统一在这里注册一次, 按类型广播给订阅者.
 */

const ble = require('./ble')

const stateListeners = []
const dataListeners = []

let state = {
  status: 'idle',      // idle | connecting | connected | error
  deviceId: '',
  deviceName: '',
  error: '',
  hasCommand: false,   // 固件里有没有命令特征 (1.4.0 才有)
  hasLog: false,       // 固件里有没有 485 日志特征 (1.5.0 才有)
  fwVersion: '',       // 固件版本号 (1.4.0 才有, 通过命令特征问出来)
}

function notifyState() {
  for (const cb of stateListeners) {
    try { cb(state) } catch (e) { console.warn('bleLink 状态回调出错', e) }
  }
}

function setState(patch) {
  state = Object.assign({}, state, patch)
  notifyState()
}

function getState() {
  return state
}

// 订阅状态变化. 立刻回调一次当前状态, 页面不用额外处理"初次渲染"
// 返回取消订阅的函数
function onState(cb) {
  stateListeners.push(cb)
  cb(state)
  return () => {
    const i = stateListeners.indexOf(cb)
    if (i >= 0) stateListeners.splice(i, 1)
  }
}

// 订阅遥测数据. kind = 'telemetry' | 'frame' | 'battery' | 'cmd'
function onData(cb) {
  dataListeners.push(cb)
  return () => {
    const i = dataListeners.indexOf(cb)
    if (i >= 0) dataListeners.splice(i, 1)
  }
}

function emitData(kind, value) {
  for (const cb of dataListeners) {
    try { cb(kind, value) } catch (e) { console.warn('bleLink 数据回调出错', e) }
  }
}

let notifyRegistered = false
let stateHandlerRegistered = false

// 只注册一次通知回调, 之后按特征 UUID 分发
function registerNotifyHandler() {
  if (notifyRegistered) return
  if (typeof wx.onBLECharacteristicValueChange !== 'function') return
  notifyRegistered = true
  wx.onBLECharacteristicValueChange((res) => {
    if (!res || !res.value) return
    const uuid = (res.characteristicId || '').toLowerCase()
    if (uuid === ble.TELEMETRY_UUID.toLowerCase()) emitData('telemetry', res.value)
    else if (uuid === ble.FRAME_UUID.toLowerCase()) emitData('frame', res.value)
    else if (uuid === ble.BATTERY_UUID.toLowerCase()) emitData('battery', res.value)
    else if (uuid === ble.LOG_UUID.toLowerCase()) emitData('log', res.value)
    else if (uuid === ble.CMD_UUID.toLowerCase()) {
      // 版本号直接进状态里 —— 页面只要看 state 就行, 不用自己去解包
      const reply = ble.parseCmdReply(res.value)
      if (reply && reply.version) setState({ fwVersion: reply.version })
      emitData('cmd', reply)
    }
  })
}

// 连接状态变化 —— 设备重启(比如 OTA 完)、走远、关机都会断, 得让页面知道.
// 不注册的话, 页面会一直显示"已连接", 车速却停在原地, 用户会以为车坏了.
function registerStateHandler() {
  if (stateHandlerRegistered) return
  if (typeof wx.onBLEConnectionStateChange !== 'function') return
  stateHandlerRegistered = true
  wx.onBLEConnectionStateChange((res) => {
    if (!res || !res.connected) {
      if (state.status === 'connected') {
        setState({
          status: 'idle',
          deviceId: '',
          deviceName: '',
          fwVersion: '',
          error: '设备断开了',
        })
      }
    }
  })
}

// 连接设备. 已连接就直接返回, 正在连就返回当前状态
async function connect() {
  if (state.status === 'connected' || state.status === 'connecting') return state
  setState({ status: 'connecting', error: '' })
  try {
    await ble.openAdapter()
    const devices = await ble.discover(10000)
    if (!devices || !devices.length) {
      throw new Error('没找到 ' + ble.NAME_PREFIX + ' 开头的设备')
    }
    const dev = devices[0]
    await ble.connect(dev.deviceId)

    registerNotifyHandler()
    registerStateHandler()
    await ble.subscribe(dev.deviceId, ble.TELEMETRY_UUID)
    await ble.subscribe(dev.deviceId, ble.FRAME_UUID)
    // 电池 / 命令 / 日志特征是后来加的, 老固件没有 —— 缺了不影响看车速
    try { await ble.subscribe(dev.deviceId, ble.BATTERY_UUID) } catch (e) {}
    let hasCommand = false
    try {
      await ble.subscribe(dev.deviceId, ble.CMD_UUID)
      hasCommand = true
    } catch (e) {}
    let hasLog = false
    try {
      await ble.subscribe(dev.deviceId, ble.LOG_UUID)
      hasLog = true
    } catch (e) {}

    setState({
      status: 'connected',
      deviceId: dev.deviceId,
      deviceName: dev.name,
      error: '',
      hasCommand: hasCommand,
      hasLog: hasLog,
      fwVersion: '',
    })

    // 问一嘴固件版本 —— 平时 WiFi 是关的, 只有蓝牙这条路能问.
    // 老固件没有命令特征, 问不了, 版本号就空着 (页面显示 "未知").
    if (hasCommand) {
      try {
        await sendCommand(ble.CMD_VERSION)
      } catch (e) {}
    }
    return state
  } catch (e) {
    setState({ status: 'error', error: e.message, deviceId: '', deviceName: '' })
    throw e
  }
}

function disconnect() {
  if (state.deviceId) {
    wx.closeBLEConnection({ deviceId: state.deviceId, fail: () => {} })
  }
  setState({ status: 'idle', deviceId: '', deviceName: '', error: '', fwVersion: '' })
}

// 往命令特征写一个字节 ('W' 开热点 / 'X' 关热点). 需要已连接
async function sendCommand(byte) {
  if (state.status !== 'connected' || !state.deviceId) {
    throw new Error('还没连上设备')
  }
  const cmdChar = await ble.findCharacteristic(state.deviceId, ble.CMD_UUID)
  await ble.writeCommand(state.deviceId, cmdChar, byte)
}

/**
 * 让设备把 485 总线日志发过来 (设备端冻结日志, 一块一块往回推).
 * offset = 从整份日志的第几个字节开始 —— 收到缺口时用这个让设备重发缺的部分.
 * release = true 表示拉完了, 让设备解冻恢复记录.
 */
async function requestLog(offset, release) {
  if (state.status !== 'connected' || !state.deviceId) {
    throw new Error('还没连上设备')
  }
  const cmdChar = await ble.findCharacteristic(state.deviceId, ble.CMD_UUID)
  const arg = release ? 0xFFFF : (offset | 0)
  await ble.writeCommandWithArg(state.deviceId, cmdChar, ble.CMD_LOG, arg)
}

module.exports = {
  getState,
  onState,
  onData,
  connect,
  disconnect,
  sendCommand,
  requestLog,
}
