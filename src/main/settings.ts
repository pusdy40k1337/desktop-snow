import * as fs from 'node:fs'
import * as path from 'node:path'
import type { AppState, EffectSchema, SettingSpec } from '../shared/types'

/** Настройки приложения: активный эффект, пауза, значения по каждому эффекту. */
export class Store {
  private values: Record<string, Record<string, any>> = {}
  private data: { activeEffect: string | null; paused: boolean }

  constructor(
    private file: string,
    private schemas: EffectSchema[],
  ) {
    this.data = { activeEffect: schemas[0]?.id ?? null, paused: false }
    this.applyDefaults()
    this.load()
  }

  private applyDefaults() {
    for (const schema of this.schemas) {
      const values: Record<string, any> = {}
      for (const spec of schema.settings) values[spec.key] = spec.default
      this.values[schema.id] = values
    }
  }

  load() {
    try {
      const raw = JSON.parse(fs.readFileSync(this.file, 'utf8'))
      if (raw.activeEffect && this.schemas.some((s) => s.id === raw.activeEffect)) {
        this.data.activeEffect = raw.activeEffect
      }
      // paused намеренно не восстанавливаем: приложение декоративное и должно
      // стартовать включённым, иначе «снег пропал» после каждого перезапуска
      for (const [id, values] of Object.entries(raw.values ?? {})) {
        if (this.values[id] && values && typeof values === 'object') {
          this.values[id] = { ...this.values[id], ...(values as object) }
        }
      }
    } catch {
      // первый запуск — файла ещё нет
    }
  }

  save() {
    fs.mkdirSync(path.dirname(this.file), { recursive: true })
    fs.writeFileSync(this.file, JSON.stringify({ ...this.data, values: this.values }, null, 2))
  }

  setValue(effectId: string, key: string, value: any) {
    if (!this.values[effectId]) return
    this.values[effectId][key] = value
    this.save()
  }

  setActive(id: string) {
    this.data.activeEffect = id
    this.save()
  }

  setPaused(paused: boolean) {
    this.data.paused = paused
    this.save()
  }

  reset() {
    this.applyDefaults()
    this.data.paused = false
    this.save()
  }

  state(): AppState {
    return { activeEffect: this.data.activeEffect, paused: this.data.paused, values: this.values }
  }

  specs(effectId: string): SettingSpec[] {
    return this.schemas.find((s) => s.id === effectId)?.settings ?? []
  }
}
