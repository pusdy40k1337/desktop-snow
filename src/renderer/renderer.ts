import { Application, Container } from 'pixi.js'
import { EFFECTS } from './.effects.generated'
import { ipc } from './ipc'
import type { AppState, Effect, EffectContext, WindowRect } from '../shared/types'

async function main() {
  // слой отрисовки: 'all' — снегопад и сугробы, 'mounds'/'flakes' — только часть (диагностика)
  const role = ((new URLSearchParams(location.search).get('role') ?? 'all') as 'all' | 'flakes' | 'mounds')
  const app = new Application()
  await app.init({
    backgroundAlpha: 0,
    resizeTo: window,
    antialias: true,
    // рендер в 1.5x вместо 1x: хлопья и кромки сугробов заметно чётче на Retina,
    // а нагрузка растёт несильно (2.25x пикселей против 4x у полного dpr 2)
    resolution: 1.5,
    autoDensity: true,
    powerPreference: 'high-performance',
  })
  document.body.appendChild(app.canvas)
  // слой сугробов двигается медленно — ему хватает 24 кадров, снегопаду нужно 30
  const baseFps = role === 'mounds' ? 24 : 30
  app.ticker.maxFPS = baseFps

  const stage = new Container()
  app.stage.addChild(stage)

  const cursor = { x: 0, y: 0, idleMs: 0 }
  window.addEventListener('mousemove', (event) => {
    cursor.x = event.clientX
    cursor.y = event.clientY
    cursor.idleMs = 0
  })

  let state: AppState = await ipc.getState()
  let windows: WindowRect[] = []
  let dockTop = window.innerHeight
  let dock: { x: number; y: number; w: number; h: number } | null = null
  let tick = 0
  let battery = false
  /** время последнего реального смещения окна: пока движется — рисуем 60 fps */
  let lastMotionAt = 0
  ipc.onWindows((payload: {
    rects: WindowRect[]
    dockTop: number
    dock: typeof dock
    tick: number
    battery?: boolean
  }) => {
    // движение фиксируем по фактическому смещению координат, а не по факту замера:
    // пока окно стоит на месте, слой работает на 24 fps и не греет машину
    const prevWindows = windows
    const rects = payload.rects ?? []
    const moved = rects.some((rect) => {
      const old = prevWindows.find((item) => item.id === rect.id)
      if (!old) return false
      return Math.abs(old.x - rect.x) > 0.5 || Math.abs(old.y - rect.y) > 0.5
    })
    if (moved) lastMotionAt = performance.now()
    windows = rects
    if (typeof payload.dockTop === 'number') dockTop = payload.dockTop
    dock = payload.dock ?? null
    if (typeof payload.tick === 'number') tick = payload.tick
    if (typeof payload.battery === 'boolean' && payload.battery !== battery) {
      // экономия заряда: на батарее вдвое меньше частиц (частоту кадров задаёт настройка)
      battery = payload.battery
    }
  })
  let current: Effect | null = null
  let currentId: string | null = null

  const makeContext = (): EffectContext => ({
    stage,
    windows, // из WindowTracker (этап 2)
    cursor,
    screen: { w: window.innerWidth, h: window.innerHeight, dpr: window.devicePixelRatio || 1, dockTop },
    dock,
    tick,
    role,
    quality: battery ? 0.5 : 1,
    settings: currentId ? (state.values[currentId] ?? {}) : {},
  })

  function activate(id: string | null) {
    if (current) {
      current.destroy()
      current = null
    }
    stage.removeChildren().forEach((child) => child.destroy({ children: true }))
    currentId = id
    if (!id) return

    const entry = EFFECTS.find((effect) => effect.manifest.id === id)
    if (!entry) {
      console.warn('эффект не найден:', id, 'доступны:', EFFECTS.map((e) => e.manifest.id).join(','))
      return
    }
    const Constructor = entry.module.default ?? entry.module.Effect
    const instance: Effect = new Constructor()
    instance.init(makeContext())
    current = instance
    const canvas = document.querySelector('canvas') as HTMLCanvasElement | null
    console.log(
      'эффект запущен:', id,
      'слой детей:', stage.children.length,
      'canvas:', canvas?.width, canvas?.height,
      'window:', window.innerWidth, window.innerHeight,
      'dpr:', window.devicePixelRatio,
    )
  }

  function applyState(next: AppState) {
    const previousSettings = currentId ? JSON.stringify(state.values[currentId] ?? {}) : ''
    const previousId = currentId
    state = next

    // частота кадров из настроек эффекта: 20–60
    const fpsValue = Number(next.values[next.activeEffect ?? '']?.fps ?? 30)
    const fps = Number.isFinite(fpsValue) ? Math.min(60, Math.max(20, Math.round(fpsValue))) : 30
    if (app.ticker.maxFPS !== fps) {
      app.ticker.maxFPS = fps
      console.log('fps эффекта:', fps)
    }

    if (next.activeEffect !== previousId) {
      activate(next.activeEffect)
    } else if (current && currentId) {
      const fresh = JSON.stringify(next.values[currentId] ?? {})
      if (fresh !== previousSettings) current.onSettingsChange(next.values[currentId] ?? {})
    }

    if (next.paused) {
      if (app.ticker.started) {
        current?.clear?.()
        app.render()
        app.ticker.stop()
        console.log('эффект на паузе: снег убран с экрана')
      }
    } else if (!app.ticker.started) {
      app.ticker.start()
      console.log('эффект возобновлён')
    }
  }

  app.ticker.add((ticker) => {
    // частоту кадров задаёт настройка эффекта (20–60), см. applyState
    const dt = Math.min(ticker.deltaMS / 1000, 0.05)
    cursor.idleMs += ticker.deltaMS
    if (!current) return
    const ctx = makeContext()
    current.update(dt, ctx)
    current.render(ctx)
  })

  ipc.onState(applyState)
  ipc.onCommand((command) => {
    if (command === 'clear') current?.clear?.()
  })
  applyState(state)
}

main().catch((error) => console.error(error))
