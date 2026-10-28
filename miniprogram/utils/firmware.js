/**
 * 取固件 —— 两种来源
 *
 *   1) 包内内置固件 (默认): tools/build_ota.sh 生成的 firmware/firmware.js
 *      完全离线可用, 手机不联网也能升级.
 *   2) 微信聊天里选的 .bin: 临时刷任意版本用.
 *
 * 为什么内置固件是 .js 而不是 .bin:
 * 小程序代码包只允许打包白名单里的扩展名 (.wxml/.wxss/.js/.json/图片/音频/…),
 * .bin / .txt 不在里面 —— 直接放 .bin 会在上传时被悄悄丢掉, 小程序里读不到。
 * 所以构建时把固件转成 base64 塞进一个 .js 模块里, .js 是白名单扩展名, 一定进包。
 *
 * 为什么还要先压缩:
 * 主包上限 2MB, 固件 1.6MB 转 base64 是 2.1MB, 超限。
 * 构建时用 tools/lz_pack.py 压到 ~1.05MB 再 base64, 这里负责解压 (utils/lz.js)。
 *
 * 固件必须 base64 后再发 —— ESP32 那边用 Arduino WebServer 收 body,
 * 存成 String(plainBuf) 按 C 字符串处理, 遇 0x00 就截断, 原始二进制会坏掉.
 */

const lz = require('./lz')

function readBuiltin() {
  let mod
  try {
    mod = require('../firmware/firmware.js')
  } catch (e) {
    throw new Error('包内没有固件, 先跑 tools/build_ota.sh 再重新上传小程序')
  }
  if (!mod || !mod.b64) throw new Error('包内固件模块是空的, 重新跑 tools/build_ota.sh')

  // 包里存的是压缩过的, 这里解压回原始固件
  const buffer = lz.decompress(mod.b64, mod.size)
  return {
    buffer: buffer,
    meta: {
      name: '内置固件',
      version: mod.version || '未知',
      size: mod.size || buffer.byteLength,
      md5: mod.md5 || '',
    },
  }
}

// 从微信聊天记录里选一个 .bin 文件
function chooseFromChat() {
  return new Promise((resolve, reject) => {
    wx.chooseMessageFile({
      count: 1,
      type: 'file',
      extension: ['.bin'],
      success: (res) => {
        const file = res.tempFiles[0]
        wx.getFileSystemManager().readFile({
          filePath: file.path,
          success: (r) => {
            resolve({
              buffer: r.data,
              meta: {
                name: file.name,
                version: '未知 (手动选择)',
                size: file.size,
                md5: '',      // 手动选的文件没有 md5, 靠 bootloader 的镜像校验兜底
              },
            })
          },
          fail: reject,
        })
      },
      fail: (err) => {
        if (err && err.errMsg && err.errMsg.indexOf('cancel') >= 0) {
          reject(new Error('cancelled'))
        } else {
          reject(new Error((err && err.errMsg) || '选择文件失败'))
        }
      },
    })
  })
}

// ArrayBuffer 的 [offset, offset+len) 转 base64
function toBase64(buffer, offset, len) {
  const view = new Uint8Array(buffer, offset, len)
  const slice = view.buffer.slice(view.byteOffset, view.byteOffset + view.byteLength)
  return wx.arrayBufferToBase64(slice)
}

module.exports = { readBuiltin, chooseFromChat, toBase64 }
