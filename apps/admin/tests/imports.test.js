/**
 * 静态门禁：调用了没导入的标识符。
 *
 * 这一类缺陷在 JS 里没有任何编译期保护，构建和单元测试也拦不住——函数体只在
 * 特定运行时数据下才执行。本项目已经踩过三次：
 *
 *   1. views/StationsView.vue 的 formatInt：费率表渲染那一刻抛 ReferenceError，
 *      渲染半途失败把 vnode 树留在半成品，从此每次路由切换都抛错、内容区不更新；
 *   2. api/account.js 的 unsupported：本该显示"接口尚未迁移"，却因为 ReferenceError
 *      落进兜底文案，显示成"加载失败（需要 OWNER 权限）"；
 *   3. api/ml.js 的 unsupported：同上。
 *
 * 因此这里做一个不依赖任何静态分析工具的白名单式检查：
 * 收集项目内所有本地模块的导出符号，凡是"在某文件里被当作函数调用、但既没有
 * import 进来、也没有在该文件里定义"的，一律报错。
 *
 * tests/ 也一起扫：测试里漏一个 import 同样只会炸在特定用例上（这个门禁写完当天
 * 就在 tests/chargers.test.js 里抓到过一次）。
 *
 * 有意做成测试而不是别的东西：`npm test` 本来就会跑，不需要额外的工具链与网络安装。
 */
import { readFileSync, readdirSync, statSync } from 'node:fs'
import { join, relative, resolve } from 'node:path'
import { describe, expect, it } from 'vitest'

const SOURCE_ROOT = resolve(process.cwd(), 'src')
const TESTS_ROOT = resolve(process.cwd(), 'tests')

function collectFiles(dir, found = []) {
  for (const entry of readdirSync(dir)) {
    const full = join(dir, entry)
    if (statSync(full).isDirectory()) collectFiles(full, found)
    else if (/\.(vue|js)$/.test(entry)) found.push(full)
  }
  return found
}

/** SFC 只看 <script> 块，模板里的名字由 vue 编译器处理，不参与本检查。 */
function scriptOf(text) {
  const matched = text.match(/<script[^>]*>([\s\S]*?)<\/script>/)
  return matched ? matched[1] : text
}

/**
 * 再去掉注释：注释里写到 `foo(` 只是举例，不是调用。
 * （这个门禁自己就被自己的注释误报过一次。）
 */
function codeOf(text) {
  return scriptOf(text)
    .replace(/\/\*[\s\S]*?\*\//g, ' ')
    .replace(/(^|[^:])\/\/[^\n]*/gm, '$1 ')
}

/** 项目内所有导出符号（含 `export { a, b }` 与 `export { a as b }`）。 */
function collectExports(files) {
  const exports = new Map()
  for (const file of files) {
    const text = codeOf(readFileSync(file, 'utf8'))
    const moduleName = relative(process.cwd(), file).split('\\').join('/')
    for (const match of text.matchAll(/^export\s+(?:async\s+)?(?:function|const|let|class)\s+([A-Za-z_$][\w$]*)/gm)) {
      exports.set(match[1], moduleName)
    }
    for (const match of text.matchAll(/^export\s*\{([^}]+)\}/gm)) {
      for (const part of match[1].split(',')) {
        const name = part.trim().split(/\s+as\s+/).pop().trim()
        if (/^[A-Za-z_$][\w$]*$/.test(name)) exports.set(name, moduleName)
      }
    }
  }
  return exports
}

/** 该文件里"名字已经有着落"的集合：导入的、命名空间导入的、本地声明的。 */
function collectBoundNames(text) {
  const bound = new Set()

  for (const match of text.matchAll(/import\s+([A-Za-z_$][\w$]*)\s*,\s*\{([^}]*)\}\s*from/g)) {
    bound.add(match[1])
    for (const part of match[2].split(',')) {
      const name = part.trim().split(/\s+as\s+/).pop().trim()
      if (name) bound.add(name)
    }
  }
  for (const match of text.matchAll(/import\s*\{([^}]*)\}\s*from/g)) {
    for (const part of match[1].split(',')) {
      const name = part.trim().split(/\s+as\s+/).pop().trim()
      if (name) bound.add(name)
    }
  }
  for (const match of text.matchAll(/import\s+([A-Za-z_$][\w$]*)\s+from/g)) bound.add(match[1])

  for (const match of text.matchAll(/(?:const|let|var|function|class)\s+([A-Za-z_$][\w$]*)/g)) bound.add(match[1])

  // Pinia store 的 action 也是同名定义：`async login({ account, password }) {`。
  // 只在"对象字面量里的方法定义"形态上认（后面跟 `(...) {`），普通调用语句不会命中。
  for (const match of text.matchAll(/(?:^|[{,])\s*(?:async\s+)?([A-Za-z_$][\w$]*)\s*\([^)]*\)\s*\{/gm)) {
    bound.add(match[1])
  }

  // `import * as ns from '...'`：以 ns. 形式调用，本检查用不上，但要避免误报
  const namespaces = [...text.matchAll(/import\s*\*\s*as\s+([A-Za-z_$][\w$]*)\s+from/g)].map(m => m[1])
  for (const namespace of namespaces) {
    const nsCall = new RegExp(`(?<![\\w.$])${namespace}\\s*\\.`)
    if (nsCall.test(text)) bound.add(namespace)
  }

  return bound
}

/** 仅在这些目录之外调用才算问题：本文件自己的导出当然不需要导入。 */
describe('静态门禁：没有"调用未导入的符号"', () => {
  it('src 与 tests 下每个被调用的项目内导出符号都已导入或定义', () => {
    const files = [...collectFiles(SOURCE_ROOT), ...collectFiles(TESTS_ROOT)]
    const exports = collectExports(files)
    expect(files.length).toBeGreaterThan(20)
    expect(exports.size).toBeGreaterThan(20)

    const problems = []
    for (const file of files) {
      const text = codeOf(readFileSync(file, 'utf8'))
      const self = relative(process.cwd(), file).split('\\').join('/')
      const bound = collectBoundNames(text)

      for (const [name, moduleName] of exports) {
        if (moduleName === self || bound.has(name)) continue
        if (new RegExp(`(?<![\\w.$'"/])${name}\\s*\\(`).test(text)) {
          problems.push(`${self} 调用了 ${name}()，但既没有导入（定义在 ${moduleName}）也没有本地定义`)
        }
      }
    }

    expect(problems).toEqual([])
  })
})
