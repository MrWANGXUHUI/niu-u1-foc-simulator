/**
 * 本地跑一遍 miniprogram-ci 的编译, 把生成的 app.json 和文件清单打出来.
 *
 * 用途: 上传之后小程序报"页面不存在", 但 miniprogram-ci 的日志只列 .js/.wxml/.wxss,
 * 看不到它**重新生成**的 app.json. 这个脚本直接把编译产物 dump 出来.
 *
 * 用法: node tools/dump_appjson.js
 */
const path = require('path')
const fs = require('fs')

const ROOT = path.join(__dirname, '..')
const MP = process.argv[2] || path.join(ROOT, 'miniprogram')
const OUT = process.argv[3] || '/tmp/appjson_dump.txt'

const lines = []
function log(...args) {
  const line = args
    .map((a) => (typeof a === 'string' ? a : JSON.stringify(a, null, 2)))
    .join(' ')
  lines.push(line)
  console.log(line)
  fs.writeFileSync(OUT, lines.join('\n'))
}

const { Project } = require(path.join(ROOT, 'node_modules/miniprogram-ci'))
const { getCompiler } = require(path.join(ROOT, 'node_modules/miniprogram-ci/dist/ci/getcompiler'))

async function main() {
  const project = new Project({
    appid: 'wx302c7b5ea803ba9e',
    type: 'miniProgram',
    projectPath: MP,
    privateKeyPath: process.env.NIU_MINI_KEY || '/home/wangxuhui/key/private.wx302c7b5ea803ba9e.key',
    ignores: [],
    setting: {},
  })

  const compiler = await getCompiler(project, project.setting)
  const compiled = await compiler.compile({
    setting: project.setting,
    onProgressUpdate: () => {},
    resultType: 'prod',
    disableSpreadingUsingComponents: true,
  })

  const files = Object.keys(compiled).sort()
  log('=== 编译产物共 ' + files.length + ' 个文件 ===')
  for (const f of files) {
    const size = compiled[f] && compiled[f].length ? compiled[f].length : 0
    log('  ' + f + '  (' + size + ')')
  }

  const appJson = compiled['app.json']
  log('')
  log('=== 生成的 app.json ===')
  if (!appJson) {
    log('!!! 没有 app.json !!!')
    return
  }
  log(typeof appJson === 'string' ? appJson : JSON.stringify(appJson, null, 2))

  log('')
  log('=== app.json 声明的页面 vs 实际文件 ===')
  let conf
  try {
    conf = typeof appJson === 'string' ? JSON.parse(appJson) : appJson
  } catch (e) {
    log('app.json 解析失败: ' + e.message)
    return
  }
  const pages = (conf.pages || []).concat(
    (conf.subPackages || []).flatMap((sp) => sp.pages.map((p) => sp.root + p))
  )
  for (const p of pages) {
    const hasJs = files.includes(p + '.js')
    const hasWxml = files.includes(p + '.wxml')
    log('  ' + (hasJs && hasWxml ? 'OK ' : 'BAD') + ' ' + p +
      '  js=' + hasJs + ' wxml=' + hasWxml + ' json=' + files.includes(p + '.json'))
  }
  if (conf.tabBar) {
    log('')
    log('=== tabBar ===')
    for (const t of conf.tabBar.list || []) {
      log('  ' + (pages.includes(t.pagePath) ? 'OK ' : 'BAD') + ' ' + t.pagePath)
    }
  }
}

main().catch((e) => {
  log('编译失败: ' + (e && e.stack ? e.stack : e))
  process.exit(1)
})
