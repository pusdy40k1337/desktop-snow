// Общие типы ядра и плагинов эффектов (см. ТЗ, раздел 5).

export interface SettingSpec {
  key: string
  label: string
  type: 'slider' | 'range' | 'color' | 'toggle' | 'select'
  default: any
  min?: number
  max?: number
  step?: number
  options?: { value: string; label: string }[]
}

export interface EffectSchema {
  id: string
  name: string
  version: string
  settings: SettingSpec[]
}

export interface AppState {
  activeEffect: string | null
  paused: boolean
  values: Record<string, Record<string, any>>
}

/** Прямоугольник видимого окна ОС (этап 2 — оседание снега). */
export interface WindowRect {
  id: string
  x: number
  y: number
  w: number
  h: number
  /** порядок наложения: 0 — верхнее окно, дальше вглубь */
  z: number
  isFullscreen: boolean
}

export interface EffectContext {
  stage: any // PIXI.Container, куда рисует эффект
  windows: WindowRect[]
  cursor: { x: number; y: number; idleMs: number }
  /** dockTop — верхняя грань Dock (из рабочей области экрана) в локальных координатах */
  screen: { w: number; h: number; dpr: number; dockTop: number }
  /** настоящие границы панели Dock; null — неизвестны (нет доступа Accessibility) */
  dock: { x: number; y: number; w: number; h: number } | null
  /** метка замера окон: меняется только при новом кадре трекера */
  tick: number
  /** слой отрисовки: flakes — только снегопад, mounds — только сугробы, all — всё */
  role?: 'all' | 'flakes' | 'mounds'
  quality: 0.5 | 1
  settings: Record<string, any>
}

export interface Effect {
  init(ctx: EffectContext): void
  update(dt: number, ctx: EffectContext): void
  render(ctx: EffectContext): void
  onSettingsChange(settings: Record<string, any>): void
  serialize(): unknown
  restore(state: unknown): void
  destroy(): void
  /** необязательно: убрать накопленное (пункт трея «Убрать снег») */
  clear?(): void
}
