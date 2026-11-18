import { Container, Graphics, Sprite, Texture } from 'pixi.js'
import type { Effect, EffectContext } from '../../shared/types'

interface Flake {
  sprite: Sprite
  depth: number
  vy: number
  phase: number
  sway: number
  wind: number
  prevY: number
  /** -1 — постоянная хлопья; >0 — осколок от стряхивания, живёт ограниченное время */
  ttl: number
  dead?: boolean
}

/** Сугроб: карта высот по ширине поверхности + заливка. */
class Mound {
  // шаг 3 px вместо 6: на малых высотах сугроб выглядел ступеньками
  static readonly STEP = 3
  heights: Float32Array
  /** 0 — колонку перекрывает окно спереди, снег там не рисуем и не копим */
  visible: Uint8Array
  /** излишек, который не влез по высоте: эффект превращает его в падающие частицы */
  overflow = 0
  graphics = new Graphics()
  /** целевая позиция от трекера: к ней сугроб едет плавно, а не рывками */
  tx: number
  ty: number
  /** скорость окна (px/с) по последнему замеру — снег едет с ней же */
  vx = 0
  vy = 0
  /** момент последнего замера, чтобы экстраполировать позицию между опросами */
  sampleAt = 0
  lastX: number
  lastY: number
  dirty = true
  visibleHeight = 0
  /** диффузия нужна только когда пришёл новый снег, иначе силуэт дёргается */
  private needsSpread = true

  constructor(
    public id: number | string,
    public x: number,
    public y: number,
    public w: number,
    public maxHeight: number,
    /** радиус скругления краёв сугроба — повторяет скругление окна */
    public radius = 12,
  ) {
    this.heights = new Float32Array(Math.ceil(w / Mound.STEP) + 1)
    this.visible = new Uint8Array(this.heights.length).fill(1)
    this.tx = x
    this.ty = y
    this.lastX = x
    this.lastY = y
  }

  indexAt(x: number): number {
    return Math.max(0, Math.min(this.heights.length - 1, Math.floor((x - this.x) / Mound.STEP)))
  }

  heightAt(x: number): number {
    const i = this.indexAt(x)
    return this.visible[i] ? this.heights[i] : 0
  }

  add(x: number, amount: number) {
    const center = this.indexAt(x)
    if (!this.visible[center]) return // это место перекрыто окном спереди
    let lost = 0
    // распределяем осадок по окрестности: иначе сугроб — редкие пики, которые
    // сглаживание при отрисовке превращает в еле заметную полоску
    const kernel: [number, number][] = [[-2, 0.25], [-1, 0.6], [0, 1], [1, 0.6], [2, 0.25]]
    for (const [offset, weight] of kernel) {
      const i = center + offset
      if (i < 0 || i >= this.heights.length || !this.visible[i]) continue
      const target = amount * weight
      const next = Math.min(this.maxHeight, this.heights[i] + target)
      lost += target - (next - this.heights[i])
      this.heights[i] = next
      if (next > this.visibleHeight) this.visibleHeight = next
    }
    // лишний снег не пропадает, а «пересыпается» — эффект потом уронит его вниз
    if (lost > 0) this.overflow += lost
    this.needsSpread = true
    this.dirty = true
  }

  clear() {
    this.heights.fill(0)
    this.visibleHeight = 0
    this.needsSpread = true
    this.dirty = true
    // Перерисовываем сразу: иначе Graphics сохраняет последнюю форму и сугроб
    // остаётся на экране при паузе (анимация-то остановлена)
    this.redraw()
  }

  isEmpty(): boolean {
    return this.visibleHeight < 0.4
  }

  /** Сглаженная заливка сугроба в ЛОКАЛЬНЫХ координатах (0..w, -h..0). */
  redraw() {
    const g = this.graphics
    g.clear()
    this.dirty = false
    if (this.isEmpty()) return

    // лёгкая диффузия: точечные осадки расползаются и заполняют разрывы,
    // иначе сугроб выглядит рваным пунктиром. Только по новому снегу —
    // иначе каждые 50 мс силуэт чуть меняется и сугроб заметно мерцает
    if (this.needsSpread) {
      const src = this.heights
      const spread = new Float32Array(src.length)
      for (let i = 0; i < src.length; i++) {
        const left = src[i - 1] ?? 0
        const right = src[i + 1] ?? 0
        spread[i] = src[i] * 0.76 + left * 0.12 + right * 0.12
      }
      this.heights.set(spread)
      this.needsSpread = false
    }

    // точки профиля в локальных координатах: маска видимости + скругление краёв
    const count = this.heights.length
    const points: { x: number; y: number }[] = []
    for (let i = 0; i < count; i++) {
      const px = i * Mound.STEP
      let h = this.visible[i] ? this.heights[i] : 0
      if (this.radius > 0) {
        const edge = Math.max(0, Math.min(px, this.w - px))
        if (edge < this.radius) {
          const t = edge / this.radius
          h *= Math.sqrt(Math.max(0, 1 - (1 - t) * (1 - t)))
        }
      }
      points.push({ x: px, y: -h })
    }

    // 1. мягкая кромка — ТОЛЬКО по участкам, где реально есть снег и которые не
    // перекрыты окном спереди (иначе по всему окну рисовалась белая полоска)
    let i = 0
    while (i < points.length) {
      if (!this.visible[i] || this.heights[i] < 0.4) {
        i += 1
        continue
      }
      g.moveTo(points[i].x, points[i].y)
      let j = i + 1
      while (j < points.length && this.visible[j] && this.heights[j] >= 0.4) {
        const prev = points[j - 1]
        const cur = points[j]
        g.quadraticCurveTo(prev.x, prev.y, (prev.x + cur.x) / 2, (prev.y + cur.y) / 2)
        j += 1
      }
      i = j
    }
    g.stroke({ color: 0xffffff, alpha: 0.55, width: 1.2 })

    // 2. тело сугроба: квадратичные кривые вместо ломаной
    g.moveTo(points[0].x, 0)
    g.lineTo(points[0].x, points[0].y)
    for (let k = 1; k < points.length; k++) {
      const prev = points[k - 1]
      const cur = points[k]
      g.quadraticCurveTo(prev.x, prev.y, (prev.x + cur.x) / 2, (prev.y + cur.y) / 2)
    }
    g.lineTo(points[points.length - 1].x, 0)
    g.closePath()
    g.fill({ color: 0xffffff, alpha: 0.95 })
  }

  /** Позиция — трансформом: перестраивать путь при каждом движении слишком дорого. */
  applyPosition() {
    this.graphics.position.set(this.x, this.y)
  }
}

function makeTexture(): Texture {
  const size = 64
  const canvas = document.createElement('canvas')
  canvas.width = size
  canvas.height = size
  const ctx = canvas.getContext('2d') as CanvasRenderingContext2D
  // Хлопья двухслойные: плотное ядро + мягкий ореол. Раньше градиент плавно
  // гас к краю — снежинки выглядели как размытые пятна без формы.
  const gradient = ctx.createRadialGradient(size / 2, size / 2, 0, size / 2, size / 2, size / 2)
  // ореол (внешние 55% радиуса) — мягкое свечение вокруг ядра
  gradient.addColorStop(0.0, 'rgba(255,255,255,1)')
  gradient.addColorStop(0.45, 'rgba(255,255,255,1)') // плотное ядро: чёткая форма
  gradient.addColorStop(0.55, 'rgba(255,255,255,0.55)') // быстрый, но плавный спад к ореолу
  gradient.addColorStop(1, 'rgba(255,255,255,0)')
  ctx.fillStyle = gradient
  ctx.beginPath()
  ctx.arc(size / 2, size / 2, size / 2, 0, Math.PI * 2)
  ctx.fill()
  return Texture.from(canvas)
}

function hexToNumber(hex: string): number {
  const clean = String(hex).replace('#', '')
  const value = parseInt(clean.length === 3 ? clean.replace(/(.)/g, '$1$1') : clean, 16)
  return Number.isNaN(value) ? 0xffffff : value
}

/**
 * Эффект «Снег».
 * Этап 0 — падающие хлопья. Этап 2 — оседание на верхних гранях окон и на Dock,
 * карта высот на каждую поверхность, стряхивание при перетаскивании окна.
 */
export default class SnowEffect implements Effect {
  private layer = new Container()
  private moundLayer = new Container()
  private mounds = new Map<number | string, Mound>()
  private flakes: Flake[] = []
  private texture: Texture | null = null
  private settings: Record<string, any> = {}
  private time = 0
  private w = 1
  private h = 1
  private needsRebuild = false
  private redrawTimer = 0
  private logTimer = 0
  private dockBaseline = 0
  /** метка последнего обработанного замера окон */
  private lastTick = -1
  /** какой слой рисуем: снегопад или сугробы (два оверлея на экран) */
  private showFlakes = true
  private showMounds = true

  init(ctx: EffectContext) {
    this.settings = { ...ctx.settings }
    this.showFlakes = ctx.role !== 'mounds'
    this.showMounds = ctx.role !== 'flakes'
    this.texture = makeTexture()
    ctx.stage.addChild(this.layer)
    if (this.showMounds) ctx.stage.addChild(this.moundLayer)
    this.rebuild(ctx)
  }

  // MARK: частицы

  private flakeCount(ctx: EffectContext): number {
    const area = ctx.screen.w * ctx.screen.h
    const density = Number(this.settings.density ?? 45) / 45
    const quality = ctx.quality === 0.5 ? 0.5 : 1
    return Math.max(0, Math.round((area / 9000) * density * quality))
  }

  private sizeRange(): [number, number] {
    const raw = this.settings.flakeSize
    if (Array.isArray(raw) && raw.length === 2) return [Number(raw[0]), Number(raw[1])]
    return [3, 7]
  }

  private rebuild(ctx: EffectContext) {
    for (const flake of this.flakes) flake.sprite.destroy()
    this.flakes = []
    this.w = ctx.screen.w
    this.h = ctx.screen.h
    if (!this.texture) return

    const count = this.flakeCount(ctx)
    const [minSize, maxSize] = this.sizeRange()
    const color = hexToNumber(this.settings.color ?? '#ffffff')
    const fallSpeed = Number(this.settings.fallSpeed ?? 90)
    const swaySetting = Number(this.settings.sway ?? 35)
    const windSetting = Number(this.settings.windSpeed ?? 5)

    for (let i = 0; i < count; i++) {
      const depth = 0.35 + Math.random() * 0.65 // ближние/дальние хлопья
      const size = (minSize + Math.random() * (maxSize - minSize)) * (0.6 + depth * 0.7)
      const sprite = new Sprite(this.texture)
      sprite.anchor.set(0.5)
      sprite.width = size
      sprite.height = size
      sprite.alpha = 0.35 + depth * 0.6
      sprite.tint = color
      sprite.visible = this.showFlakes
      sprite.x = Math.random() * this.w
      sprite.y = Math.random() * this.h
      this.layer.addChild(sprite)
      this.flakes.push({
        sprite,
        depth,
        vy: fallSpeed * depth * (0.7 + Math.random() * 0.6),
        phase: Math.random() * Math.PI * 2,
        sway: swaySetting * depth,
        wind: windSetting * depth,
        prevY: sprite.y,
        ttl: -1,
      })
    }
    console.log('snow: создано хлопьев', this.flakes.length, 'в', this.w, 'x', this.h)
  }

  // MARK: поверхности

  /** точка попадает внутрь окна, за которым снег рисовать не нужно */
  private insideWindow(x: number, y: number, ctx: EffectContext) {
    // по умолчанию снег идёт поверх окон (иначе при развёрнутых окнах его не видно
    // вовсе), но своё окно настроек остаётся чистым всегда
    const hideOverAll = this.settings.snowOverWindows === false
    for (const w of ctx.windows) {
      if (!hideOverAll && w.id !== 'settings') continue
      if (x >= w.x && x <= w.x + w.w && y >= w.y && y <= w.y + w.h) return true
    }
    return false
  }

  private syncSurfaces(ctx: EffectContext) {
    if (!this.showMounds) return // в слое снегопада сугробов нет
    const accumulate = Boolean(this.settings.accumulate ?? true)
    // новый замер от трекера: только по нему считаем скорость окна
    const newSample = ctx.tick !== this.lastTick
    if (newSample) this.lastTick = ctx.tick

    if (!accumulate) {
      for (const [id, mound] of this.mounds) {
        mound.graphics.destroy()
        this.mounds.delete(id)
      }
      return
    }

    const seen = new Set<number | string>([-1])
    // радиус скругления окна: снег не должен свисать с закруглённых углов
    const ROUND = 14

    for (const rect of ctx.windows) {
      if (rect.y < 40) continue // окна под меню-баром не трогаем
      if (rect.isFullscreen) continue // в полноэкранном режиме эффект на паузе
      seen.add(rect.id)

      const inset = Math.min(ROUND, rect.w / 6)
      const targetX = rect.x + inset
      const targetW = Math.max(24, rect.w - inset * 2)

      let mound = this.mounds.get(rect.id)
      if (!mound) {
        mound = new Mound(rect.id, targetX, rect.y, targetW, 40, ROUND)
        mound.lastX = rect.x
        mound.lastY = rect.y
        mound.sampleAt = this.time
        this.mounds.set(rect.id, mound)
        this.moundLayer.addChild(mound.graphics)
        mound.applyPosition()
      } else {
        mound.tx = targetX
        mound.ty = rect.y

        if (newSample) {
          // скорость окна = смещение / время с прошлого замера
          const dx = rect.x - mound.lastX
          const dy = rect.y - mound.lastY
          const dtSample = Math.max(0.008, this.time - mound.sampleAt)
          mound.vx = dx / dtSample
          mound.vy = dy / dtSample
          mound.sampleAt = this.time
          mound.lastX = rect.x
          mound.lastY = rect.y

          // превышен предел скорости — снег сдувает ветром.
          // Резкое движение вниз срывает снег вдвое легче (vy > 60 → порог вдвое ниже)
          const speed = Math.hypot(mound.vx, mound.vy)
          const baseThreshold = Number(this.settings.blowSpeed ?? 1200)
          const threshold = mound.vy > 60 ? baseThreshold * 0.5 : baseThreshold
          if (speed > threshold && !mound.isEmpty()) {
            const fraction = Math.min(1, (speed - threshold) / (threshold * 1.2))
            // осколки отстают от окна — летят против его движения
            this.shakeOff(mound, fraction, -mound.vx * 0.12)
          }
        }
        if (Math.abs(targetW - mound.w) > 8) {
          mound.w = targetW
          mound.heights = new Float32Array(Math.ceil(targetW / Mound.STEP) + 1)
          mound.visible = new Uint8Array(mound.heights.length).fill(1)
          mound.visibleHeight = 0
          mound.dirty = true
        }
        mound.lastX = rect.x
        mound.lastY = rect.y
      }

      // маска видимости пересчитывается только по новым данным трекера:
      // 500 колонок × все окна на каждом кадре — лишняя нагрузка
      if (newSample) {
        const cols = mound.heights.length
        const bandTop = mound.y - mound.maxHeight
        for (let i = 0; i < cols; i++) {
          const px = mound.x + i * Mound.STEP
          let hidden = false
          for (const front of ctx.windows) {
            if (front.z >= rect.z) continue
            if (px < front.x || px > front.x + front.w) continue
            // окно спереди перекрывает полосу, где лежит сугроб
            if (front.y < mound.y && front.y + front.h > bandTop) {
              hidden = true
              break
            }
          }
          const flag = hidden ? 0 : 1
          if (mound.visible[i] !== flag) {
            mound.visible[i] = flag
            if (!flag) mound.heights[i] = 0
            mound.dirty = true
          }
        }
      }
    }

    for (const [id, mound] of this.mounds) {
      if (seen.has(id)) continue
      this.shakeOff(mound)
      mound.graphics.destroy()
      this.mounds.delete(id)
    }

    // Dock: снег лежит только по фактическим границам панели (границы приходят из AX).
    // Если панель неизвестна — сугроба нет, снег просто падает на «землю».
    const dockRect = ctx.dock
    const dockMound = this.mounds.get(-1)
    // настройки высоты и сдвига снега на панели Dock (слайдеры в окне настроек)
    const dockMaxH = Math.max(8, Math.min(80, Number(this.settings.dockMoundHeight ?? 40)))
    // Сдвиг сугроба относительно кромки панели подобран пользователем (−10 px:
    // панель «съедает» ~10 px низа сугроба из-за скругления и обводки) и запечён
    // в код: ползунок из настроек убран, значение применяется автоматически
    // при любых изменениях Dock — сугроб двигается вместе с панелью,
    // всегда оставаясь опущенным на эту величину
    const DOCK_MOUND_SHIFT = -10
    const dockShift = DOCK_MOUND_SHIFT
    if (dockRect && dockRect.h > 24) {
      // по ширине панели Dock, основание — на её верхней кромке.
      // -1 px: иначе нижняя строка сугроба совпадает с краем панели и мерцает
      const targetX = Math.round(dockRect.x)
      const targetW = Math.max(80, Math.round(dockRect.w))
      const baseline = Math.round(dockRect.y) - 1 - dockShift
      if (!dockMound) {
        const fresh = new Mound(-1, targetX, baseline, targetW, dockMaxH, 12)
        fresh.lastX = targetX
        fresh.lastY = baseline
        fresh.sampleAt = this.time
        this.mounds.set(-1, fresh)
        this.moundLayer.addChild(fresh.graphics)
        fresh.applyPosition()
      } else {
        dockMound.tx = targetX
        dockMound.ty = baseline
        // панель меняет размер (увеличение иконок, число приложений) — профиль
        // переливаем в новую ширину: снег распластывается или осыпается вниз
        if (Math.abs(targetW - dockMound.w) > 4) this.reshapeMound(dockMound, targetW)
        // пользователь подтянул высоту/сдвиг слайдером — применяем на лету
        if (dockMound.maxHeight !== dockMaxH) {
          dockMound.maxHeight = dockMaxH
          for (let i = 0; i < dockMound.heights.length; i++) {
            if (dockMound.heights[i] > dockMaxH) dockMound.heights[i] = dockMaxH
          }
          if (dockMound.visibleHeight > dockMaxH) dockMound.visibleHeight = dockMaxH
          dockMound.dirty = true
        }
      }
    } else if (dockMound && !dockRect) {
      // границы панели временно неизвестны — оставляем сугроб как есть,
      // иначе разовый сбой AX сбрасывал накопленный снег
      void dockMound
    }

    // Земля: только когда панель Dock недоступна (скрыта или нет прав AX) —
    // тогда падающий снег не исчезает бесследно, а копится на нижней кромке.
    // При видимой панели землю не рисуем: полоса под Dock мерцает на стыке
    // (это и просил убрать пользователь), а снег, не попавший на панель,
    // просто уходит за нижнюю кромку.
    if (dockRect) {
      const stale = this.mounds.get(-2)
      if (stale) {
        stale.clear()
        stale.graphics.destroy()
        this.mounds.delete(-2)
      }
      return
    }
    seen.add(-2)
    let ground = this.mounds.get(-2)
    if (!ground) {
      ground = new Mound(-2, 0, this.h, this.w, 30, 0)
      this.mounds.set(-2, ground)
      this.moundLayer.addChild(ground.graphics)
      ground.applyPosition()
    } else if (Math.abs(this.w - ground.w) > 8) {
      ground.w = this.w
      ground.heights = new Float32Array(Math.ceil(this.w / Mound.STEP) + 1)
      ground.visible = new Uint8Array(ground.heights.length).fill(1)
      ground.visibleHeight = 0
      ground.dirty = true
    }
  }

  /** Переливает профиль сугроба в новую ширину; при сужении излишек осыпается. */
  private reshapeMound(mound: Mound, targetW: number) {
    const oldHeights = mound.heights
    const oldLen = oldHeights.length
    const newLen = Math.ceil(targetW / Mound.STEP) + 1
    if (newLen < 2) return
    let oldMass = 0
    for (let i = 0; i < oldLen; i++) oldMass += oldHeights[i]
    const next = new Float32Array(newLen)
    for (let i = 0; i < newLen; i++) {
      const rel = i / (newLen - 1)
      const src = Math.min(oldLen - 1, Math.round(rel * (oldLen - 1)))
      next[i] = oldHeights[src]
    }
    let newMass = 0
    let peak = 0
    for (let i = 0; i < newLen; i++) {
      newMass += next[i]
      if (next[i] > peak) peak = next[i]
    }
    mound.heights = next
    mound.visible = new Uint8Array(newLen).fill(1)
    mound.w = targetW
    // обязательно переносим высоту пика: если оставить 0, isEmpty() сочтёт
    // сугроб пустым и он исчезнет с экрана до прихода нового снега
    mound.visibleHeight = peak
    mound.dirty = true
    // сужение панели: лишний снег не исчезает, а падает вниз на кромку экрана
    const lost = oldMass - newMass
    if (lost > 4) this.spillMass(mound.x, mound.x + targetW, mound.y, lost)
    console.log('snow: сугроб Dock перелит под новую панель:', oldLen * Mound.STEP, '->', targetW, 'px, осыпь:', Math.round(lost))
  }

  /** Сыплет частицы с линии сугроба: они долетают до земли и оседают там. */
  private spillMass(x0: number, x1: number, y: number, mass: number) {
    if (!this.texture) return
    const count = Math.min(140, Math.max(6, Math.round(mass / 5)))
    for (let i = 0; i < count; i++) {
      const sprite = new Sprite(this.texture)
      sprite.anchor.set(0.5)
      const size = 2.5 + Math.random() * 3
      sprite.width = size
      sprite.height = size
      sprite.alpha = 0.8
      sprite.visible = this.showFlakes
      sprite.x = x0 + Math.random() * Math.max(1, x1 - x0)
      sprite.y = y - Math.random() * 4
      this.layer.addChild(sprite)
      this.flakes.push({
        sprite,
        depth: 0.85,
        vy: 70 + Math.random() * 110,
        phase: Math.random() * Math.PI * 2,
        sway: 4,
        wind: 0,
        prevY: sprite.y,
        ttl: 6,
      })
    }
  }

  /** Сугроб рассыпается в частицы. fraction — доля срыва, windX — снос ветром. */
  private shakeOff(mound: Mound, fraction = 1, windX = 0) {
    if (mound.isEmpty() || !this.texture || fraction <= 0) return
    // бюджет частиц пропорционален доле срыва: при полном осыпании нужно больше
    const budget = Math.round(Math.max(80, this.w / 10) * Math.max(0.35, fraction))
    let spawned = 0

    for (let i = 0; i < mound.heights.length && spawned < budget; i++) {
      const height = mound.heights[i]
      if (height < 0.8) continue
      const released = height * fraction
      // остаток остаётся лежать на окне и едет вместе с ним
      mound.heights[i] = height - released
      if (released < 0.5) continue
      const pieces = Math.min(3, 1 + Math.round(released / 8))
      for (let p = 0; p < pieces && spawned < budget; p++) {
        const sprite = new Sprite(this.texture)
        sprite.anchor.set(0.5)
        const size = 2.5 + Math.random() * 3.5
        sprite.width = size
        sprite.height = size
        sprite.alpha = 0.85
        sprite.x = mound.x + i * Mound.STEP + (Math.random() - 0.5) * Mound.STEP
        sprite.y = mound.y - released * (0.2 + Math.random() * 0.8)
        this.layer.addChild(sprite)
        this.flakes.push({
          sprite,
          depth: 0.85,
          // осколки сразу падают оторванным куском, а не висят на прежнем месте
          vy: 140 + Math.random() * 120,
          phase: Math.random() * Math.PI * 2,
          sway: 8 + Math.random() * 14,
          wind: windX,
          prevY: sprite.y,
          ttl: 2 + Math.random() * 1.5,
        })
        spawned++
      }
    }

    // при полном срыве высоты обнуляются, при частичном — остаются на месте
    if (fraction >= 0.999) mound.clear()
    else mound.dirty = true
  }

  /** Излишек снега с переполненных сугробов сползает вниз частицами. */
  private spillOverflow() {
    if (!this.texture) return
    for (const mound of this.mounds.values()) {
      if (mound.overflow < 0.5) continue
      const pieces = Math.min(3, Math.floor(mound.overflow))
      mound.overflow -= pieces
      for (let k = 0; k < pieces; k++) {
        const i = Math.floor(Math.random() * mound.heights.length)
        if (!mound.visible[i] || mound.heights[i] < mound.maxHeight * 0.7) continue
        const sprite = new Sprite(this.texture)
        sprite.anchor.set(0.5)
        const size = 2 + Math.random() * 3
        sprite.width = size
        sprite.height = size
        sprite.alpha = 0.85
        sprite.x = mound.x + i * Mound.STEP
        sprite.y = mound.y - mound.heights[i]
        this.layer.addChild(sprite)
        this.flakes.push({
          sprite,
          depth: 0.9,
          vy: 90 + Math.random() * 70,
          phase: Math.random() * Math.PI * 2,
          sway: 6 + Math.random() * 10,
          wind: 0,
          prevY: sprite.y,
          ttl: 3 + Math.random() * 3,
        })
      }
    }
  }

  clear() {
    for (const mound of this.mounds.values()) mound.clear()
    // пауза/смена стола должны убирать с экрана и сами хлопья, а не только сугробы
    for (const flake of this.flakes) flake.sprite.visible = false
  }

  // MARK: кадр

  update(dt: number, ctx: EffectContext) {
    this.time += dt

    if (ctx.screen.w !== this.w || ctx.screen.h !== this.h || this.needsRebuild) {
      this.needsRebuild = false
      this.rebuild(ctx)
    }

    // верхняя грань Dock больше не нужна: границы панели приходят из AX (ctx.dock)
    this.dockBaseline = this.h

    this.syncSurfaces(ctx)

    // Позиции сугробов двигаем только в слое сугробов
    if (this.showMounds) {
      for (const mound of this.mounds.values()) {
        const elapsed = Math.min(0.04, Math.max(0, this.time - mound.sampleAt))
        const nx = mound.tx + mound.vx * elapsed
        const ny = mound.ty + mound.vy * elapsed
        if (Math.abs(nx - mound.x) > 0.05 || Math.abs(ny - mound.y) > 0.05) {
          mound.x = nx
          mound.y = ny
          mound.applyPosition() // только трансформ, путь не перестраиваем
        }
      }
    }

    const damping = dt * 60 // стабильность при просадках FPS
    for (const flake of this.flakes) {
      // осколки от стряхивания живут ограниченное время
      if (flake.ttl >= 0) {
        flake.ttl -= dt
        if (flake.ttl <= 0) {
          flake.sprite.destroy()
          flake.dead = true
          continue
        }
      }

      flake.prevY = flake.sprite.y
      flake.sprite.y += flake.vy * dt
      flake.sprite.x += flake.wind * dt + Math.sin(this.time * 1.4 + flake.phase) * flake.sway * dt * damping

      // Снег не рисуется поверх содержимого окон: хлопья внутри окна считаются
      // ушедшими «за окно». Так содержимое окон остаётся чистым при одном слое.
      flake.sprite.visible = this.showFlakes && !this.insideWindow(flake.sprite.x, flake.sprite.y, ctx)

      // попадание на поверхность сугроба
      let deposited = false
      for (const mound of this.mounds.values()) {
        if (flake.sprite.x < mound.x || flake.sprite.x > mound.x + mound.w) continue
        const surface = mound.y - mound.heightAt(flake.sprite.x)
        if (flake.prevY <= surface && flake.sprite.y >= surface) {
          if (flake.ttl >= 0) {
            // снег с закрытого/сдвинутого окна оседает на поверхность ниже,
            // а не пропадает (частично — доля меньше, чем у обычной хлопьи)
            mound.add(flake.sprite.x, Math.max(0.2, flake.sprite.width * 0.2))
            flake.sprite.destroy()
            flake.dead = true
          } else {
            mound.add(flake.sprite.x, Math.max(0.25, flake.sprite.width * 0.28))
            flake.sprite.y = -24
            flake.sprite.x = Math.random() * this.w
            flake.prevY = flake.sprite.y
          }
          deposited = true
          break
        }
      }
      if (deposited) continue

      if (flake.sprite.y > this.h + 24) {
        if (flake.ttl >= 0) {
          flake.sprite.destroy()
          flake.dead = true
          continue
        }
        flake.sprite.y = -24
        flake.sprite.x = Math.random() * this.w
        flake.prevY = flake.sprite.y
      }
      if (flake.sprite.x < -30) flake.sprite.x = this.w + 20
      if (flake.sprite.x > this.w + 30) flake.sprite.x = -20
    }

    if (this.flakes.some((flake) => flake.dead)) {
      this.flakes = this.flakes.filter((flake) => !flake.dead)
    }

    // пересыпание: сугроб заполнен доверху — излишек сползает вниз частицами
    this.spillOverflow()

    // Перестраиваем форму только при изменении снега, не чаще 20 раз в секунду:
    // движение сугроба идёт трансформом и от этого не зависит
    this.redrawTimer += dt
    if (this.redrawTimer > 0.05) {
      this.redrawTimer = 0
      for (const mound of this.mounds.values()) {
        if (mound.dirty) mound.redraw()
      }
    }

    // диагностика накопления (раз в 8 секунд)
    this.logTimer += dt
    if (this.logTimer > 8) {
      this.logTimer = 0
      const dock = this.mounds.get(-1)
      const windows = [...this.mounds.values()].filter((mound) => mound.id !== -1)
      const top = windows.reduce((max, mound) => Math.max(max, mound.visibleHeight), 0)
      const hidden = windows.reduce((sum, mound) => {
        let count = 0
        for (let i = 0; i < mound.visible.length; i++) if (!mound.visible[i]) count++
        return sum + count
      }, 0)
      const mass = [...this.mounds.values()].reduce((sum, mound) => {
        let total = 0
        for (let i = 0; i < mound.heights.length; i++) total += mound.heights[i]
        return sum + total
      }, 0)
      console.log(
        'snow: поверхностей', windows.length,
        '| Dock', (dock?.visibleHeight ?? 0).toFixed(1),
        '| макс. окно', top.toFixed(1),
        '| скрыто колонок', hidden,
        '| масса', Math.round(mass),
        '| хлопьев', this.flakes.length,
      )
    }
  }

  render(_ctx: EffectContext) {}

  onSettingsChange(settings: Record<string, any>) {
    const previous = this.settings
    this.settings = { ...settings }

    const structural =
      previous.density !== this.settings.density ||
      JSON.stringify(previous.flakeSize) !== JSON.stringify(this.settings.flakeSize)

    if (structural) {
      this.needsRebuild = true
      return
    }

    const color = hexToNumber(this.settings.color ?? '#ffffff')
    const fallSpeed = Number(this.settings.fallSpeed ?? 90)
    const swaySetting = Number(this.settings.sway ?? 35)
    const windSetting = Number(this.settings.windSpeed ?? 5)

    for (const flake of this.flakes) {
      flake.sprite.tint = color
      flake.vy = fallSpeed * flake.depth * 0.9
      flake.sway = swaySetting * flake.depth
      flake.wind = windSetting * flake.depth
    }

    if (!Boolean(this.settings.accumulate ?? true)) {
      for (const mound of this.mounds.values()) mound.clear()
    }
  }

  serialize(): unknown {
    return {
      mounds: [...this.mounds.values()]
        .filter((mound) => mound.id !== -1 && !mound.isEmpty())
        .map((mound) => ({ id: mound.id, heights: Array.from(mound.heights) })),
    }
  }

  restore(_state: unknown) {
    // восстановление сугробов между запусками — на этапе 3 вместе со снеговиком
  }

  destroy() {
    this.layer.destroy({ children: true })
    this.moundLayer.destroy({ children: true })
    this.texture?.destroy(true)
    this.texture = null
    this.flakes = []
    this.mounds.clear()
  }
}
