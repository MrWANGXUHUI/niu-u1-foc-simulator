/**
 * 固件解压 — 解 tools/lz_pack.py 压出来的极简 LZ77
 *
 * 为什么要有这玩意: 小程序主包上限 2MB, 固件 1.6MB 转 base64 是 2.1MB, 装不下.
 * 压缩之后 1.05MB, base64 1.4MB, 就塞得下了.
 *
 * 格式 (一串 group):
 *   flag 字节: bit0..bit7, 1 = 后面跟 1 个字面量字节, 0 = 后面跟 3 字节的匹配
 *   匹配 3 字节: offset (uint16 LE, 往回看多少字节) + len (uint8, 实际长度 = len + 3)
 */

const B64_CHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'

function base64ToArrayBuffer(b64) {
  if (typeof wx !== 'undefined' && typeof wx.base64ToArrayBuffer === 'function') {
    return wx.base64ToArrayBuffer(b64)
  }
  const map = {}
  for (let i = 0; i < B64_CHARS.length; i++) map[B64_CHARS[i]] = i
  let len = b64.length
  while (len > 0 && b64.charAt(len - 1) === '=') len--
  const out = new Uint8Array(Math.floor((len * 3) / 4))
  let acc = 0
  let bits = 0
  let n = 0
  for (let i = 0; i < len; i++) {
    acc = (acc << 6) | map[b64.charAt(i)]
    bits += 6
    if (bits >= 8) {
      bits -= 8
      out[n++] = (acc >> bits) & 0xFF
      acc = bits ? acc & ((1 << bits) - 1) : 0
    }
  }
  return out.buffer
}

// 解压. expectedSize 是原始字节数 (压缩包里单独记着)
function decompress(base64, expectedSize) {
  const src = new Uint8Array(base64ToArrayBuffer(base64))
  const out = new Uint8Array(expectedSize)
  let ip = 0
  let op = 0

  while (ip < src.length) {
    const flag = src[ip++]
    for (let i = 0; i < 8; i++) {
      if (ip >= src.length) break
      if (flag & (1 << i)) {
        // 字面量
        if (op >= out.length) throw new Error('解压越界')
        out[op++] = src[ip++]
      } else {
        // 匹配: 往回看 offset 字节, 复制 len 个
        if (ip + 3 > src.length) throw new Error('压缩数据不完整')
        const off = src[ip] | (src[ip + 1] << 8)
        const len = src[ip + 2] + 3
        ip += 3
        if (off === 0 || off > op) throw new Error('解压出错: 非法偏移 ' + off)
        let p = op - off
        for (let k = 0; k < len; k++) {
          if (op >= out.length) throw new Error('解压越界')
          out[op++] = out[p++]
        }
      }
    }
  }
  if (op !== expectedSize) throw new Error('解压长度不对: ' + op + ' != ' + expectedSize)
  return out.buffer
}

module.exports = { decompress, base64ToArrayBuffer }
