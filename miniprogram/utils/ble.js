/**
 * BLE 遥测通道 — 对应固件里的 GATT 服务 (ble_telemetry.h)
 *
 * 看车速不用连 WiFi 热点, 蓝牙直连, 手机照常上网.
 * 推固件才需要连热点 (OTA 走 HTTP, 1MB 多, 蓝牙传太慢).
 *
 * 安卓注意: Android 6 以后扫蓝牙要"定位"权限, 第一次会弹系统授权框, 不给就扫不到设备.
 *
 * ⚠️ **微信基础库版本兼容**: BLE 那套 API 是一批批加进来的, 老的微信上缺 API 会直接抛
 * "wx.xxx is not a function" —— 曾经因为 `wx.onBluetoothDevicesFound` (2.11.0 才有)
 * 在用户手机上不存在, 扫描回调压根没注册上, 表现为"怎么改广播包都扫不到设备".
 * 所以这个文件里凡是用到可能缺失的 API, 都要先 typeof 检查再调, 并留兜底路径.
 */

const SERVICE_UUID = '6e400001-b5a3-f393-e0a9-e50e24dcca9e'
const TELEMETRY_UUID = '6e400002-b5a3-f393-e0a9-e50e24dcca9e'
const FRAME_UUID = '6e400003-b5a3-f393-e0a9-e50e24dcca9e'
const BATTERY_UUID = '6e400004-b5a3-f393-e0a9-e50e24dcca9e'
const CMD_UUID = '6e400005-b5a3-f393-e0a9-e50e24dcca9e'
const NAME_PREFIX = 'NIU-U1'

// 命令字节 —— 对应固件 ble_telemetry.h 的 BleCommand
const CMD_START_AP = 'W'   // 临时开 OTA 热点
const CMD_STOP_AP = 'X'    // 关热点
const CMD_VERSION = 'V'    // 问固件版本号 (回包第一字节还是 'V', 后面跟版本字符串)

// 电池消息类型 —— 对应固件 ble_telemetry.h 的 BLE_BMSG_*
const BMSG_STATUS = 1
const BMSG_CELLS = 2
const BMSG_MODEL = 3

// 把微信的 errMsg 翻译成人能动手解决的一句话.
// BLE 这边最容易卡在权限上, 不翻译的话用户只看到 "openBluetoothAdapter:fail user deny"
function explainBleFail(errMsg, apiName) {
  const m = (errMsg || '').toLowerCase()
  if (m.indexOf('user deny') >= 0 || m.indexOf('user denied') >= 0 ||
      m.indexOf('denied') >= 0 || m.indexOf('not authorized') >= 0) {
    return '蓝牙权限被拒 —— iPhone 上到 设置 → 隐私与安全性 → 蓝牙 → 微信 打开; ' +
           '安卓上到 设置 → 应用 → 微信 → 权限 里开蓝牙(和定位)'
  }
  if (m.indexOf('adapter not ready') >= 0 || m.indexOf('no bluetooth adapter') >= 0 ||
      m.indexOf('bluetooth adapter') >= 0) {
    return '蓝牙没开 —— 把手机蓝牙打开再试'
  }
  if (m.indexOf('timeout') >= 0 || m.indexOf('time out') >= 0) {
    return '超时 —— 设备离手机太远, 或者已经被别的 App 连上了'
  }
  if (m.indexOf('not found') >= 0) {
    return '设备不存在 —— 蓝牙名是 NIU-U1-BLE (WiFi 热点才叫 NIU-U1-OTA), 确认 ESP32 通着电'
  }
  if (m.indexOf('already connected') >= 0) {
    return '设备已经被连上了 —— 先断掉别的 App 再试'
  }
  return (errMsg || (apiName + ' 失败'))
}

// wx.* 的 Promise 化包装
function call(name, options) {
  return new Promise((resolve, reject) => {
    const fn = wx[name]
    if (typeof fn !== 'function') {
      reject(new Error('这个基础库不支持 ' + name))
      return
    }
    fn({
      ...options,
      success: resolve,
      fail: (err) => reject(new Error(explainBleFail(err && err.errMsg, name))),
    })
  })
}

function openAdapter() {
  return call('openBluetoothAdapter', {})
}

function closeAdapter() {
  return call('closeBluetoothAdapter', {}).catch(() => {})
}

// 扫设备, 只留名字像我们的. 返回 [{deviceId, name, rssi}]
//
// ⚠️ 三处 iOS / 低版本微信的坑, 都踩过:
//
// 1) **services 参数**: 微信文档说"只搜索广播包有对应 UUID 的主服务的设备",
//    但 iOS 实现里传空数组会被当成"过滤条件非空", 只上报广播里带服务 UUID 的设备.
//    我们主广播包只有 Flags + 名字 (名字必须放主广播包, 见固件 ble_telemetry.h),
//    128 位 UUID 在扫描回应里 —— 正好被过滤掉. **所以不传 services**.
//
// 2) **onBluetoothDevicesFound 可能不存在**: 这个 API 是基础库 2.11.0 才加的,
//    更老的微信只有单数的 wx.onBluetoothDeviceFound. 用户手机上报的错就是
//    "wx.onBluetoothDevicesFound is not a function" —— 扫描回调压根没注册上,
//    所以之前怎么改广播包都没用. **三套 API 都兼容**: 新版事件 / 旧版单数事件 /
//    getBluetoothDevices 轮询兜底 (最老的微信只有这个).
//
// 3) **iOS 上微信的 BLE 扫描在后台会被系统限制**, 所以窗口给足 (>=10s).
//
// 顺带记下扫到的**所有**设备 (不只是匹配上的), 失败时报错里全列出来 ——
// "扫不到"的原因有好几个 (权限 / iOS 缓存 / 名字没广播出来 / 基础库太老),
// 光看一句"没找到"没法区分, 看到具体扫到了什么才行.
function discover(timeoutMs) {
  return new Promise((resolve, reject) => {
    let done = false
    const found = {}
    const seen = {}   // 扫到的所有设备 (不管名字对不对)
    let pollTimer = null
    let timer = null

    const onFound = (res) => {
      const list = (res && res.devices) || []
      for (const d of list) {
        const name = d.name || d.localName || ''
        seen[d.deviceId] = {
          name: name,
          rssi: d.RSSI,
          uuids: d.advertisServiceUUIDs || [],
        }
        if (!name || name.indexOf(NAME_PREFIX) !== 0) continue
        found[d.deviceId] = { deviceId: d.deviceId, name: name, rssi: d.RSSI }
      }
      // 一找到就直接收工, 不用等满整个窗口
      if (Object.keys(found).length) finish(true)
    }

    // 老版 API 的回调参数形状不一样 (单个设备, 不是 devices 数组)
    const onFoundOld = (res) => onFound({ devices: res ? [res] : [] })

    function finish() {
      if (done) return
      done = true
      if (timer) clearTimeout(timer)
      if (pollTimer) clearInterval(pollTimer)
      if (wx.offBluetoothDevicesFound) wx.offBluetoothDevicesFound(onFound)
      if (wx.offBluetoothDeviceFound) wx.offBluetoothDeviceFound(onFoundOld)
      call('stopBluetoothDevicesDiscovery', {}).catch(() => {})

      const list = Object.keys(found).map((k) => found[k])
      if (list.length) {
        resolve(list)
        return
      }
      // 一个都没匹配上 —— 把实际扫到的设备全列出来, 一眼就能看出是"权限没给"还是"名字变了"
      const all = Object.keys(seen).map((k) => {
        const s = seen[k]
        const uuids = (s.uuids && s.uuids.length) ? ('[' + s.uuids.join(',') + ']') : ''
        return (s.name || '(无名字)') + ' ' + uuids
      })
      if (all.length) {
        reject(new Error('扫到了 ' + all.length + ' 个蓝牙设备, 但没有叫 ' + NAME_PREFIX +
          ' 开头的:\n' + all.slice(0, 8).join('\n')))
      } else {
        reject(new Error('一个蓝牙设备都没扫到 —— 检查手机蓝牙是否开着, ' +
          '微信有没有蓝牙权限 (设置→隐私与安全性→蓝牙→微信), 安卓还要给定位权限'))
      }
    }

    // 注册扫描回调 —— 新版优先, 没有就用老版单数 API
    if (typeof wx.onBluetoothDevicesFound === 'function') {
      wx.onBluetoothDevicesFound(onFound)
    } else if (typeof wx.onBluetoothDeviceFound === 'function') {
      wx.onBluetoothDeviceFound(onFoundOld)
    }
    // 兜底: 定时拉一次已知设备列表. 有些基础库事件回调不可靠, 但这个接口一直都在.
    pollTimer = setInterval(() => {
      wx.getBluetoothDevices({
        success: (res) => {
          const list = (res && res.devices) || []
          if (list.length) onFound({ devices: list })
        },
        fail: () => {},
      })
    }, 1500)

    timer = setTimeout(finish, timeoutMs || 8000)

    wx.startBluetoothDevicesDiscovery({
      // 不传 services! iOS 上传空数组会把没广播服务 UUID 的设备全过滤掉
      allowDuplicatesKey: true,
      interval: 1000,
      success: () => {},
      fail: (err) => {
        if (done) return
        done = true
        if (timer) clearTimeout(timer)
        if (pollTimer) clearInterval(pollTimer)
        if (wx.offBluetoothDevicesFound) wx.offBluetoothDevicesFound(onFound)
        if (wx.offBluetoothDeviceFound) wx.offBluetoothDeviceFound(onFoundOld)
        reject(new Error(explainBleFail(err && err.errMsg, '开始扫描')))
      },
    })
  })
}

// 微信缓存里已知的蓝牙设备 (含之前连过的).
// iOS 对同一个设备地址的 GATT 缓存很顽固 —— 之前连接失败过的话, 之后可能一直连不上.
// 把这个列出来, 就能看出"设备在缓存里但连不上"这种 iOS 特有的坑.
function getKnownDevices() {
  return new Promise((resolve) => {
    if (typeof wx.getBluetoothDevices !== 'function') {
      resolve([])
      return
    }
    wx.getBluetoothDevices({
      success: (res) => resolve((res && res.devices) || []),
      fail: () => resolve([]),
    })
  })
}

// 当前微信支持哪些 BLE API —— 出问题时先看这个, 一眼就知道是不是版本太老.
// 返回缺失的 API 名字列表 (空数组 = 都支持)
function missingApis() {
  const need = [
    'openBluetoothAdapter',
    'startBluetoothDevicesDiscovery',
    'stopBluetoothDevicesDiscovery',
    'createBLEConnection',
    'getBLEDeviceServices',
    'getBLEDeviceCharacteristics',
    'notifyBLECharacteristicValueChange',
    'onBLECharacteristicValueChange',
    'writeBLECharacteristicValue',
  ]
  const missing = []
  for (const name of need) {
    if (typeof wx[name] !== 'function') missing.push(name)
  }
  return missing
}

// 基础库版本 (比如 "3.0.0"). 老版本缺 API 时报出来, 用户能看懂该升级微信.
function libVersion() {
  try {
    if (wx.getSystemInfoSync) {
      const info = wx.getSystemInfoSync()
      return info.SDKVersion || info.version || '未知'
    }
  } catch (e) {
    // 老基础库可能没有 getSystemInfoSync
  }
  return '未知'
}

async function connect(deviceId) {
  await call('createBLEConnection', { deviceId, timeout: 10000 })
  // iOS 上必须先问一次 MTU, 否则后面写不进去 (通知不需要, 但问一次更稳)
  try {
    await call('getBLEMTU', { deviceId, writeType: 'write' })
  } catch (e) {
    // 安卓不支持 getBLEMTU, 忽略
  }
}

// 找到目标特征并订阅通知. 返回 serviceId / characteristicId
async function subscribe(deviceId, charUuid) {
  const services = await call('getBLEDeviceServices', { deviceId })
  const svc = (services.services || []).find(
    (s) => (s.uuid || '').toLowerCase() === SERVICE_UUID.toLowerCase()
  )
  if (!svc) throw new Error('设备上没有预期的服务, 可能不是我们的固件')

  const chars = await call('getBLEDeviceCharacteristics', { deviceId, serviceId: svc.uuid })
  const ch = (chars.characteristics || []).find(
    (c) => (c.uuid || '').toLowerCase() === charUuid.toLowerCase()
  )
  if (!ch) throw new Error('设备上没有预期的特征值')

  await call('notifyBLECharacteristicValueChange', {
    deviceId,
    serviceId: svc.uuid,
    characteristicId: ch.uuid,
    state: true,
  })
  return { serviceId: svc.uuid, characteristicId: ch.uuid }
}

// 找到特征值但不订阅 —— 用于只写不听的场合 (命令特征)
async function findCharacteristic(deviceId, charUuid) {
  const services = await call('getBLEDeviceServices', { deviceId })
  const svc = (services.services || []).find(
    (s) => (s.uuid || '').toLowerCase() === SERVICE_UUID.toLowerCase()
  )
  if (!svc) throw new Error('设备上没有预期的服务, 可能不是我们的固件')

  const chars = await call('getBLEDeviceCharacteristics', { deviceId, serviceId: svc.uuid })
  const ch = (chars.characteristics || []).find(
    (c) => (c.uuid || '').toLowerCase() === charUuid.toLowerCase()
  )
  if (!ch) throw new Error('设备上没有预期的特征值')
  return { serviceId: svc.uuid, characteristicId: ch.uuid }
}

// 往命令特征写一个字节 ('W' 开热点 / 'X' 关热点)
async function writeCommand(deviceId, cmdChar, byte) {
  const buf = new Uint8Array(1)
  buf[0] = byte.charCodeAt(0)
  await call('writeBLECharacteristicValue', {
    deviceId,
    serviceId: cmdChar.serviceId,
    characteristicId: cmdChar.characteristicId,
    value: buf.buffer,
    writeType: 'write',
  })
}

// 解析遥测载荷 (20 字节小端; 前 12 字节是旧格式, 老固件也认)
function parseTelemetry(buffer) {
  const v = new DataView(buffer)
  const t = {
    speed: v.getUint16(0, true) / 10,     // km/h
    rpm: v.getUint16(2, true),
    voltage: v.getUint16(4, true) / 10,   // V
    current: v.getInt16(6, true) / 10,    // A
    protocolSpeed: v.getUint8(8),
    error: v.getUint8(9),
    unlocked: v.getUint8(10),
    flags: v.getUint8(11),
    // 诊断字段 —— 骑车时用它们分清"霍尔没脉冲"还是"485 没应答"
    qCount: 0,        // 收到的 V35 状态查询次数
    aCount: 0,        // 我们回出的应答次数
    pulseCount: 0,    // 霍尔脉冲累计数
    badCs: 0,         // 校验和错的帧数
    intervalMs: 0,    // 最近一次脉冲间隔
  }
  if (buffer.byteLength >= 20) {
    t.qCount = v.getUint16(12, true)
    t.aCount = v.getUint16(14, true)
    t.pulseCount = v.getUint16(16, true)
    t.badCs = v.getUint8(18)
    t.intervalMs = v.getUint8(19) * 10
  }
  return t
}

function bufferToHex(buffer) {
  const bytes = new Uint8Array(buffer)
  let s = ''
  for (let i = 0; i < bytes.length; i++) {
    s += (i ? ' ' : '') + ('0' + bytes[i].toString(16).toUpperCase()).slice(-2)
  }
  return s
}

/**
 * 解析命令特征的回执.
 *
 * 固件有两种回包 (ble_telemetry.h 的 notifyReply / notifyVersion):
 *   [cmd][state]      —— 普通命令, state: 0x01 已执行 / 0xFF 不认识
 *   ['V'][ASCII 版本] —— 版本号
 *
 * 返回 {cmd, state, version}. 版本那条的 state 恒为 null.
 */
function parseCmdReply(buffer) {
  if (!buffer || !buffer.byteLength) return null
  const bytes = new Uint8Array(buffer)
  const cmd = String.fromCharCode(bytes[0])
  if (cmd === CMD_VERSION) {
    let ver = ''
    for (let i = 1; i < bytes.length; i++) ver += String.fromCharCode(bytes[i])
    return { cmd: cmd, state: null, version: ver.replace(/\0/g, '').trim() }
  }
  return { cmd: cmd, state: bytes.length > 1 ? bytes[1] : null, version: '' }
}

/**
 * 电池信息 —— 一条特征上跑多种消息, 每条通知带 3 字节头 [类型][片号][总片数].
 * 分片是因为单条通知最多 20 字节, 而 13 串单体电压就有 26 字节.
 *
 * state 是个累加器: 每收到一片就往里存, 收齐了再回调 onUpdate.
 */
function createBatteryState() {
  return {
    status: null,
    cells: [],
    model: '',
    pending: {},     // 类型 → {total, chunks:[]}
  }
}

function handleBatteryChunk(state, buffer, onUpdate) {
  const bytes = new Uint8Array(buffer)
  if (bytes.length < 3) return
  const type = bytes[0]
  const idx = bytes[1]
  const total = bytes[2]
  if (total < 1 || total > 16 || idx >= total) return

  let slot = state.pending[type]
  if (!slot || slot.total !== total) {
    slot = { total: total, chunks: new Array(total), got: 0 }
    state.pending[type] = slot
  }
  slot.chunks[idx] = bytes.subarray(3)
  // 按"已收齐的片数"算, 不能按"收到的次数"算 —— 蓝牙重传同一片时
  // 用次数算会提前凑够 total, 拼出来少一片, 整条消息就废了
  let got = 0
  for (let i = 0; i < total; i++) if (slot.chunks[i]) got++
  slot.got = got

  // 收齐了才拼
  if (slot.got < total) return
  delete state.pending[type]

  let payload = new Uint8Array(0)
  for (let i = 0; i < total; i++) {
    const c = slot.chunks[i]
    if (!c) return
    const merged = new Uint8Array(payload.length + c.length)
    merged.set(payload, 0)
    merged.set(c, payload.length)
    payload = merged
  }

  if (type === BMSG_STATUS && payload.length >= 17) {
    const v = new DataView(payload.buffer, payload.byteOffset, payload.byteLength)
    const temps = []
    const n = v.getUint8(6)
    for (let i = 0; i < n && i < 5; i++) temps.push(v.getInt8(7 + i))
    state.status = {
      hasData: (v.getUint8(0) & 0x01) !== 0,
      soc: v.getUint8(1),
      voltage: v.getUint16(2, true) / 10,      // V
      current: v.getInt16(4, true) / 10,       // A
      temps: temps,
      cellCount: v.getUint8(12),
      minCellMv: v.getUint16(13, true),
      maxCellMv: v.getUint16(15, true),
    }
  } else if (type === BMSG_CELLS) {
    const cells = []
    for (let i = 0; i + 1 < payload.length; i += 2) {
      cells.push((payload[i] | (payload[i + 1] << 8)) / 1000)   // V
    }
    state.cells = cells
  } else if (type === BMSG_MODEL) {
    state.model = String.fromCharCode.apply(null, payload)
  }

  if (onUpdate) onUpdate(state)
}

module.exports = {
  SERVICE_UUID,
  TELEMETRY_UUID,
  FRAME_UUID,
  BATTERY_UUID,
  CMD_UUID,
  NAME_PREFIX,
  CMD_START_AP,
  CMD_STOP_AP,
  CMD_VERSION,
  openAdapter,
  closeAdapter,
  discover,
  getKnownDevices,
  missingApis,
  libVersion,
  connect,
  subscribe,
  findCharacteristic,
  writeCommand,
  parseTelemetry,
  bufferToHex,
  parseCmdReply,
  createBatteryState,
  handleBatteryChunk,
}
