/**
 * 旧页面路径的跳板 —— 页面在 2026-11-02 改名了 (index → upgrade).
 *
 * 为什么还留着这个空壳:
 *   小程序可以带着一个**页面路径**被打开, 而那个路径可能记在很多地方 ——
 *   公众平台的「小程序主页」设置、老的小程序码/二维码、微信里的历史分享链接、
 *   收藏夹里的旧入口.
 *
 *   ⚠️ 关键: **旧版 app.json 里 pages[0] 就是 pages/index/index** ——
 *   所以公众平台记下来的「小程序主页」正是这个路径. 改完名之后, 小程序默认打开
 *   会报 "该页面不存在, 你可以进入XXX首页获取更多服务", 用户连进都进不来
 *   (点左上角「首页」倒是正常, 因为那个走 pages[0]).
 *
 *   所以这里留个同名空页, 进来就跳到**新的默认首页** (车辆 tab).
 *   注意: 跳车辆不是跳升级 —— 新版 app.json 的 pages[0] 是 pages/vehicle/vehicle,
 *   这个跳板要跟新的默认首页保持一致, 否则用户默认打开会落在「升级」上.
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
