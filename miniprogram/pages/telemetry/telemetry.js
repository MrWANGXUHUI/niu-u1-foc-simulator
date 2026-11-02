/**
 * 旧页面路径的跳板 —— 页面在 2026-11-02 改名了 (telemetry → vehicle).
 *
 * 为什么还留着这个空壳:
 *   小程序可以带着一个**页面路径**被打开, 而那个路径可能记在很多地方 ——
 *   公众平台的「小程序主页」设置、老的小程序码/二维码、微信里的历史分享链接、
 *   收藏夹里的旧入口. 这些路径指向的是**改名前的** pages/telemetry/telemetry.
 *   如果那个页面不存在, 微信会直接报 "该页面不存在, 你可以进入XXX首页获取更多服务",
 *   用户连小程序都进不去 —— 但点左上角「首页」又完全正常 (那个走的是 pages[0]).
 *
 *   所以这里留一个同名空页, 进来就跳到新的「车辆」tab.
 *   只要旧路径还在, 任何陈旧的入口都不会断.
 *
 * 注意: 目标是 tabBar 页面, 必须用 wx.switchTab (redirectTo/navigateTo 跳不过去).
 */

Page({
  onLoad() {
    wx.switchTab({
      url: '/pages/vehicle/vehicle',
      fail: () => wx.reLaunch({ url: '/pages/vehicle/vehicle' }),
    })
  },
})
