import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

// vitest 的 jsdom 环境把 import.meta.url 解析成 http URL，node:fs 无法直接消费；
// 改为从 apps/user（vitest 的运行目录）出发解析：剥掉第一段 ../，
// '../src/...' 落在 apps/user 内，'../../admin|shared/...' 落在 apps/ 下。
const read = (path) => readFileSync(join(process.cwd(), path.replace('../', '')), 'utf8')

describe('两端共享冰蓝主题', () => {
  it('两端在基础样式之后、动效降级之前加载相同主题', () => {
    for (const path of ['../src/main.js', '../../admin/src/main.js']) {
      const source = read(path)
      const theme = source.indexOf("import '../../shared/styles/ice-theme.css'")
      expect(theme).toBeGreaterThan(source.indexOf("import './style.css'"))
      expect(theme).toBeLessThan(source.indexOf("import './styles/motion.css'"))
    }
  })

  it('装饰不接收点击且保留键盘焦点提示，不新增无限动画', () => {
    const theme = read('../../shared/styles/ice-theme.css')
    expect(theme).toContain('pointer-events: none')
    expect(theme).toContain(':focus-visible')
    expect(theme).not.toContain('animation:')
    for (const path of ['../src/styles/ice-layout.css', '../../admin/src/styles/ice-layout.css']) {
      expect(read(path)).not.toContain('animation:')
    }
  })

  it('电量装饰复用真实格式化值，不伪造进度且不重复朗读', () => {
    const source = read('../src/views/ChargingView.vue')
    expect(source).toContain('class="charge-energy" aria-hidden="true"')
    expect(source).toContain('<strong>{{ formatEnergy(energyMwh) }}</strong>')
    expect(source).toContain('data-testid="progress-energy"')
  })
})
