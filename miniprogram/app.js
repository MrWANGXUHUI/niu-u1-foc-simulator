App({
  globalData: {
    // ESP32 的 SoftAP 固定 IP (ota_config.h 里的 OTA_AP_IP)
    deviceIp: '192.168.4.1',
    port: 8080,
    apSsid: 'NIU-U1-OTA',
    apPassword: 'niu12345',
  },
})
