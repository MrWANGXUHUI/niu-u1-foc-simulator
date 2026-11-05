// 车辆页逻辑自测: 485 帧的分块拼装 + 日志包的重组与保存
//
//   node tools/test_vehicle_page.js
//
// 直接把 pages/vehicle/vehicle.js 拿来跑 (Page 和 wx 都用假的),
// 所以测的是**页面里真正的那段代码**, 不是复制一遍的逻辑.

const path = require('path')
const Module = require('module')

let fail = 0
function check(name, got, want) {
  const ok = JSON.stringify(got) === JSON.stringify(want)
  if (!ok) fail++
  console.log((ok ? '✓' : '✗') + ' ' + name +
    (ok ? '' : '  实际=' + JSON.stringify(got) + ' 期望=' + JSON.stringify(want)))
}

// ---- 假的 wx / Page ----
const shown = []
const toasts = []
global.wx = {
  getFileSystemManager: () => ({
    writeFile: (o) => { o.success && o.success(); },
  }),
  env: { USER_DATA_PATH: '/tmp' },
  setClipboardData: (o) => { o.success && o.success(); },
  showToast: (o) => { toasts.push(o.title); o.success && o.success(); },
  showActionSheet: (o) => { shown.push(o.itemList); o.fail && o.fail({}); },
  showModal: (o) => { shown.push(o.content); o.fail && o.fail({}); },
}
let pageDef = null
global.Page = (def) => { pageDef = def }

// ---- 把 bleLink 换成假的 (它要真连蓝牙) ----
const bleLinkPath = require.resolve(path.join(__dirname, '..', 'miniprogram', 'utils', 'bleLink.js'))
const fakeBleLink = {
  onState: () => () => {},
  onData: () => () => {},
  requestLog: async () => {},
  sendCommand: async () => {},
}
require.cache[bleLinkPath] = {
  id: bleLinkPath, filename: bleLinkPath, loaded: true, exports: fakeBleLink,
}

require(path.join(__dirname, '..', 'miniprogram', 'pages', 'vehicle', 'vehicle.js'))
const page = Object.assign({}, pageDef)
page.data = Object.assign({}, pageDef.data)
page.setData = function (patch) { Object.assign(this.data, patch) }

async function main() {
// ---- 1) 485 帧分块拼装 ----
// 固件按 16 字节一块推, 每块第 1 个字节是块序号
function chunks(frame) {
  const out = []
  for (let off = 0, idx = 0; off < frame.length; off += 16, idx++) {
    const part = frame.slice(off, off + 16)
    out.push(Buffer.concat([Buffer.from([idx]), Buffer.from(part)]))
  }
  return out
}

// 40 字节, 结尾是 0x16 (帧尾) → 固件会分成 3 块推 (16 + 16 + 8)
const frame = Buffer.from([
  0x68, 0x40, 0xBF, 0x68, 0x02, 0x02, 0x05, 0x08,
  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
  0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
  0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
  0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x16,
])
function hexOf(buf) {
  return Array.from(buf).map((b) => ('0' + b.toString(16).toUpperCase()).slice(-2)).join(' ')
}

for (const c of chunks(frame)) page.onFrame(c.buffer.slice(c.byteOffset, c.byteOffset + c.length))
check('485 帧: 分块拼回来', page.data.frames.length, 1)
check('485 帧: 拼出来的十六进制对', page.data.frames[0], hexOf(frame))

// 丢一块 → 这帧要作废, 不能拼出乱七八糟的东西
page.data.frames = []
const cs = chunks(frame)
page.onFrame(cs[0].buffer.slice(cs[0].byteOffset, cs[0].byteOffset + cs[0].length))
// 中间那块 (序号 1) 丢了, 只收到序号 0 和 2
page.onFrame(cs[2].buffer.slice(cs[2].byteOffset, cs[2].byteOffset + cs[2].length))
check('485 帧: 中间丢块时不产生半截帧', page.data.frames.length, 0)

// ---- 2) 日志包重组 ----
// 包格式: [0]'L' [1]flags [2..5]偏移 [6..9]总长 [10..]文本
function logPacket(offset, total, text, last) {
  const body = Buffer.from(text, 'ascii')
  const b = Buffer.alloc(10 + body.length)
  b[0] = 0x4C
  b[1] = last ? 1 : 0
  b.writeUInt32LE(offset, 2)
  b.writeUInt32LE(total, 6)
  body.copy(b, 10)
  return b.buffer.slice(b.byteOffset, b.byteOffset + b.length)
}

let savedText = null
let savedName = null
global.wx.getFileSystemManager = () => ({
  writeFile: (o) => { savedText = o.data; savedName = o.filePath; o.success && o.success(); },
})
page.setData = function (patch) { Object.assign(this.data, patch) }

const body = 'RX 1000 68 20 DF 68 02 02 05 08 1A 16\nTX 1012 68 20 DF 68 82 0C\n'
page.onLogPacket(logPacket(0, body.length, body.slice(0, 30), false))
check('日志: 中间状态显示进度', page.data.logStatus.indexOf('拉取中') === 0, true)
page.onLogPacket(logPacket(30, body.length, body.slice(30), true))
// finishLog 是异步的 (要 await 写文件), 等它跑完
await new Promise((r) => setTimeout(r, 10))
check('日志: 拉完了状态', page.data.logStatus.indexOf('已保存') === 0, true)
check('日志: 存下来的内容带设备侧的原文', savedText.indexOf(body) >= 0, true)
check('日志: 文件名带时间戳', /niu485-\d{8}-\d{6}\.log$/.test(savedName), true)
check('日志: 弹出分享选项', shown.length > 0 && shown[0].join(' ').indexOf('发送到微信') >= 0, true)

// ---- 3) 有缺口 → 让设备从缺口重发 ----
let requested = null
fakeBleLink.requestLog = async (offset, release) => { requested = { offset, release } }
page.logBuf = ''
page.logDone = false
page.onLogPacket(logPacket(0, 100, 'AAAA', false))
page.onLogPacket(logPacket(50, 100, 'BBBB', false))   // 缺口: 我们只有 4 个字节
check('日志: 有缺口时按缺口位置要重发', requested && requested.offset === 4, true)
check('日志: 有缺口时不误判成拉完', page.data.logStatus.indexOf('拉取中') === 0, true)

console.log('\n' + (fail ? `✗ ${fail} 项没过` : '✓ 全部通过'))
process.exit(fail ? 1 : 0)
}

main()
