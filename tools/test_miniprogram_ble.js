// 小程序侧自测: BLE 消息的解析 (日志包 / 电池状态 / 电池诊断) + 设备侧包的还原
//
//   node tools/test_miniprogram_ble.js [packets.bin] [expect.txt]
//
// 配套的设备侧测试在 tools/test_bus_log.cpp —— 它生成包, 这个脚本负责还原,
// 两个凑一起才说明"固件和小程序两边格式对得上".

global.wx = {
  getFileSystemManager: () => ({ writeFile: (o) => { o.success && o.success(); } }),
  env: { USER_DATA_PATH: '/tmp' },
  setClipboardData: (o) => { o.success && o.success(); },
}
const fs = require('fs')
const path = require('path')
const ble = require(path.join(__dirname, '..', 'miniprogram', 'utils', 'ble.js'))

let fail = 0
function check(name, got, want) {
  const ok = JSON.stringify(got) === JSON.stringify(want)
  if (!ok) fail++
  console.log((ok ? '✓' : '✗') + ' ' + name +
    (ok ? '' : '  实际=' + JSON.stringify(got) + ' 期望=' + JSON.stringify(want)))
}

// ---- 1) 日志包解析 ----
// 格式: [0]'L' [1]flags [2..5]偏移 [6..9]总长 [10..]文本
function makeLogPacket(offset, total, text, last) {
  const body = Buffer.from(text, 'ascii')
  const buf = Buffer.alloc(10 + body.length)
  buf[0] = 0x4C
  buf[1] = last ? 1 : 0
  buf.writeUInt32LE(offset, 2)
  buf.writeUInt32LE(total, 6)
  body.copy(buf, 10)
  return buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.length)
}

let pkt = ble.parseLogPacket(makeLogPacket(0, 25, 'RX 1000 68 20 DF\n', false))
check('日志包: 偏移/总长/文本',
  { off: pkt.offset, total: pkt.total, text: pkt.text, last: pkt.last },
  { off: 0, total: 25, text: 'RX 1000 68 20 DF\n', last: false })
check('日志包: 最后一个包 last=true',
  ble.parseLogPacket(makeLogPacket(20, 25, 'TX 2000 16\n', true)).last, true)
check('日志包: 不是日志包返回 null',
  ble.parseLogPacket(Buffer.from([1, 2, 3]).buffer), null)

// ---- 2) 电池诊断消息 (固件 ble_battery_msg.h 的 BMSG_DIAG) ----
function makeBatteryMsg(type, payload) {
  const buf = Buffer.alloc(3 + payload.length)
  buf[0] = type
  buf[1] = 0
  buf[2] = 1
  payload.copy(buf, 3)
  return buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.length)
}

const diag = Buffer.alloc(16)
diag.writeUInt16LE(42, 0)          // rxFrames
diag.writeUInt16LE(40, 2)          // okFrames
diag[4] = 1                        // csErr
diag[5] = 1                        // lenErr
diag[6] = 0                        // unknownLen
diag[7] = 23                       // lastLen
diag.writeUInt32LE(0xFFFFFFFF, 8)  // ageMs = 从来没有
diag.writeUInt16LE(100, 12)        // qCount
diag.writeUInt16LE(99, 14)         // aCount

const state = ble.createBatteryState()
ble.handleBatteryChunk(state, makeBatteryMsg(4, diag), () => {})
check('诊断: 帧数/成功数/错误数',
  [state.diag.rxFrames, state.diag.okFrames, state.diag.csErr, state.diag.lenErr],
  [42, 40, 1, 1])
check('诊断: 从没见过 BMS → ageMs = -1', state.diag.ageMs, -1)
check('诊断: qCount/aCount', [state.diag.qCount, state.diag.aCount], [100, 99])

diag.writeUInt32LE(3400, 8)
const state2 = ble.createBatteryState()
ble.handleBatteryChunk(state2, makeBatteryMsg(4, diag), () => {})
check('诊断: ageMs 有值', state2.diag.ageMs, 3400)

// ---- 3) 电池状态消息 (老格式不能被改坏) ----
const st = Buffer.alloc(17)
st[0] = 0x01; st[1] = 80
st.writeUInt16LE(537, 2)
st.writeInt16LE(0, 4)
st[6] = 5
st[7] = 22; st[8] = 21; st[9] = 22; st[10] = 21; st[11] = 20
st[12] = 13
st.writeUInt16LE(3700, 13)
st.writeUInt16LE(3790, 15)
const state3 = ble.createBatteryState()
ble.handleBatteryChunk(state3, makeBatteryMsg(1, st), () => {})
check('电池状态: 电量/电压/电流/温度/串数',
  [state3.status.soc, state3.status.voltage, state3.status.current,
   state3.status.temps.join('/'), state3.status.cellCount],
  [80, 53.7, 0, '22/21/22/21/20', 13])

// ---- 4) 命令带参数 (取日志用) ----
let written = null
global.wx.writeBLECharacteristicValue = (o) => {
  written = Buffer.from(o.value); o.success && o.success()
}
global.wx.getBLEDeviceServices = (o) => o.success({ services: [{ uuid: ble.SERVICE_UUID }] })
global.wx.getBLEDeviceCharacteristics = (o) =>
  o.success({ characteristics: [{ uuid: ble.CMD_UUID }] })

;(async () => {
  const cmdChar = await ble.findCharacteristic('devid', ble.CMD_UUID)
  await ble.writeCommandWithArg('devid', cmdChar, ble.CMD_LOG, 0x1234)
  check('命令带参数: [L][偏移小端]', Array.from(written), [0x4C, 0x34, 0x12])
  await ble.writeCommand('devid', cmdChar, ble.CMD_VERSION)
  check('命令单字节: [V]', Array.from(written), [0x56])

  // ---- 5) 还原设备侧生成的包 (跨语言的那一环) ----
  const pktPath = process.argv[2] || '/tmp/niu485_test/packets.bin'
  const txtPath = process.argv[3] || '/tmp/niu485_test/expect.txt'
  if (fs.existsSync(pktPath) && fs.existsSync(txtPath)) {
    const raw = fs.readFileSync(pktPath)
    const expect = fs.readFileSync(txtPath, 'ascii')
    const packets = []
    let p = 0
    while (p + 4 <= raw.length) {
      const len = raw.readUInt32LE(p)
      p += 4
      packets.push(raw.slice(p, p + len))
      p += len
    }
    let buf = ''
    let total = 0
    let ok = true
    for (const raw of packets) {
      const parsed = ble.parseLogPacket(
        raw.buffer.slice(raw.byteOffset, raw.byteOffset + raw.length))
      if (!parsed || parsed.offset !== buf.length) { ok = false; break }
      buf += parsed.text
      total = parsed.total
      if (parsed.last) break
    }
    check('设备侧的包: 偏移首尾相接没缺口', ok, true)
    check('设备侧的包: 还原出来的文本跟原文一致', buf === expect.toString('ascii'), true)
    check('设备侧的包: 总长字段对得上', total === buf.length, true)
  } else {
    console.log('(跳过跨语言还原测试: 先跑 tools/test_bus_log.cpp 生成包)')
  }

  console.log('\n' + (fail ? `✗ ${fail} 项没过` : '✓ 全部通过'))
  process.exit(fail ? 1 : 0)
})()
