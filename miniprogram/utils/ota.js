/**
 * ESP32 OTA 通道 — 对应固件里的 /ota/* 接口
 *
 * 走的是微信小程序的「局域网例外」: url 是局域网 IP 时不做安全域校验, 也不用 https.
 * 前提是手机和 ESP32 在同一网段 —— 所以手机要先连 ESP32 的热点 NIU-U1-OTA.
 */

const CHUNK_SIZE = 4096   // 原始字节/块, 和固件里的 OTA_CHUNK_SIZE 一致

function baseUrl(ip, port) {
  return 'http://' + ip + ':' + port
}

function request(url, options) {
  return new Promise((resolve, reject) => {
    wx.request({
      url: url,
      method: 'GET',
      timeout: 20000,
      ...options,
      success: (res) => {
        if (res.statusCode >= 200 && res.statusCode < 300) {
          resolve(res.data)
        } else {
          reject(new Error('HTTP ' + res.statusCode + ' ' + JSON.stringify(res.data || '')))
        }
      },
      fail: (err) => {
        reject(new Error(err.errMsg || '请求失败'))
      },
    })
  })
}

// 读设备状态
async function getInfo(ip, port) {
  const data = await request(baseUrl(ip, port) + '/ota/info', { method: 'GET' })
  if (!data || !data.ok) throw new Error('设备返回异常')
  return data
}

// 告诉设备"准备接收 N 字节", 设备开始擦写备用分区
async function begin(ip, port, size, md5) {
  const body = { size: size }
  if (md5) body.md5 = md5
  const data = await request(baseUrl(ip, port) + '/ota/begin', {
    method: 'POST',
    header: { 'content-type': 'application/json' },
    data: JSON.stringify(body),
  })
  if (!data || !data.ok) throw new Error((data && data.error) || 'begin 失败')
  return data
}

// 发一块 (base64 文本)
async function chunk(ip, port, offset, base64) {
  const data = await request(baseUrl(ip, port) + '/ota/chunk', {
    method: 'POST',
    header: { 'content-type': 'text/plain', 'X-Offset': String(offset) },
    data: base64,
  })
  if (!data || !data.ok) throw new Error((data && data.error) || '传输中断')
  return data
}

// 收尾: 设备校验 + 切换启动分区 + 重启
async function end(ip, port) {
  const data = await request(baseUrl(ip, port) + '/ota/end', { method: 'POST' })
  if (!data || !data.ok) throw new Error((data && data.error) || '校验失败')
  return data
}

async function abort(ip, port) {
  try {
    await request(baseUrl(ip, port) + '/ota/abort', { method: 'POST' })
  } catch (e) {
    // 设备可能已经断了, 无所谓
  }
}

module.exports = { CHUNK_SIZE, getInfo, begin, chunk, end, abort }
