/**
 * BLE 遥测通道 — 对应固件里的 GATT 服务 (ble_telemetry.h)
 *
 * 看车速不用连 WiFi 热点, 蓝牙直连, 手机照常上网.
 * 推固件才需要连热点 (OTA 走 HTTP, 1MB 多, 蓝牙传太慢).
 *
 * 安卓注意: Android 6 以后扫蓝牙要"定位"权限, 第一次会弹系统授权框, 不给就扫不到设备.
 */

const SERVICE_UUID = '6e400001-b5a3-f393-e0a9-e50e24dcca9e'
const TELEMETRY_UUID = '6e400002-b5a3-f393-e0a9-e50e24dcca9e'
const FRAME_UUID = '6e400003-b5a3-f393-e0a9-e50e24dcca9e'
const BATTERY_UUID = '6e400004-b5a3-f393-e0a9-e50e24dcca9e'
const NAME_PREFIX = 'NIU-U1'

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
function discover(timeoutMs) {
  return new Promise((resolve, reject) => {
    let done = false
    const found = {}
    const seen = {}   // 扫到的所有设备 (不管名字对不对) —— 用来在失败时说明到底扫到了什么

    const onFound = (res) => {
      const list = (res && res.devices) || []
      for (const d of list) {
        const name = d.name || d.localName || ''
        seen[d.deviceId] = name || '(名字为空)'
        if (!name || name.indexOf(NAME_PREFIX) !== 0) continue
        found[d.deviceId] = { deviceId: d.deviceId, name: name, rssi: d.RSSI }
      }
    }

    wx.onBluetoothDevicesFound(onFound)

    const timer = setTimeout(() => {
      if (done) return
      done = true
      wx.offBluetoothDevicesFound && wx.offBluetoothDevicesFound(onFound)
      call('stopBluetoothDevicesDiscovery', {}).catch(() => {})
      const list = Object.keys(found).map((k) => found[k])
      if (list.length) {
        resolve(list)
        return
      }
      // 一个都没匹配上 —— 把实际扫到的名字列出来, 一眼就能看出是"权限没给"还是"名字变了"
      const names = Object.keys(seen).map((k) => seen[k])
      if (names.length) {
        reject(new Error('扫到了 ' + names.length + ' 个蓝牙设备但没一个叫 ' + NAME_PREFIX +
          ' 开头的: ' + names.slice(0, 5).join(', ')))
      } else {
        reject(new Error('一个蓝牙设备都没扫到 —— 检查手机蓝牙是否开着, ' +
          '微信有没有蓝牙权限 (设置→隐私与安全性→蓝牙→微信), 安卓还要给定位权限'))
      }
    }, timeoutMs || 8000)

    wx.startBluetoothDevicesDiscovery({
      services: [],
      allowDuplicatesKey: true,
      interval: 500,
      success: () => {},
      fail: (err) => {
        if (done) return
        done = true
        clearTimeout(timer)
        wx.offBluetoothDevicesFound && wx.offBluetoothDevicesFound(onFound)
        reject(new Error(explainBleFail(err && err.errMsg, '开始扫描')))
      },
    })
  })
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
  NAME_PREFIX,
  openAdapter,
  closeAdapter,
  discover,
  connect,
  subscribe,
  parseTelemetry,
  bufferToHex,
  createBatteryState,
  handleBatteryChunk,
}
