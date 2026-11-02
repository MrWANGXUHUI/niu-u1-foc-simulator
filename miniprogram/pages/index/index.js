/**
 * 旧页面路径的跳板 —— 页面在 2026-11-02 改名了 (index → upgrade).
 *
 * 理由跟 pages/telemetry/telemetry.js 一样: 公众平台的「小程序主页」设置、
 * 老的小程序码/二维码、历史分享链接都可能还指着改名前的 pages/index/index,
 * 那个路径要是没了, 微信就会报 "该页面不存在" —— 用户连小程序都进不去.
 * 留个同名空页跳到新的「升级」tab, 陈旧入口就不会断.
 *
 * 注意: 目标是 tabBar 页面, 必须用 wx.switchTab.
 */

Page({
  onLoad() {
    wx.switchTab({
      url: '/pages/upgrade/upgrade',
      fail: () => wx.reLaunch({ url: '/pages/upgrade/upgrade' }),
    })
  },
})
