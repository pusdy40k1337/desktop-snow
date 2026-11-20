import { ipc } from '../renderer/ipc'
import type { AppState, EffectSchema, SettingSpec } from '../shared/types'

let schemas: EffectSchema[] = []
let state: AppState

const effectSelect = document.getElementById('effect') as HTMLSelectElement
const controls = document.getElementById('controls') as HTMLDivElement
const pauseButton = document.getElementById('pause') as HTMLButtonElement

function activeSchema(): EffectSchema | undefined {
  return schemas.find((schema) => schema.id === state.activeEffect)
}

function values(): Record<string, any> {
  return state.activeEffect ? (state.values[state.activeEffect] ?? {}) : {}
}

function push(key: string, value: any) {
  if (!state.activeEffect) return
  ipc.setSetting(state.activeEffect, key, value)
}

function makeRow(spec: SettingSpec): HTMLDivElement {
  const row = document.createElement('div')
  row.className = 'row'

  const label = document.createElement('div')
  label.className = 'label'
  label.textContent = spec.label
  row.appendChild(label)

  if (spec.type === 'slider') {
    const input = document.createElement('input')
    input.type = 'range'
    input.id = `ctl-${spec.key}`
    input.min = String(spec.min ?? 0)
    input.max = String(spec.max ?? 100)
    input.step = String(spec.step ?? 1)
    input.value = String(values()[spec.key] ?? spec.default)
    const value = document.createElement('div')
    value.className = 'value'
    value.textContent = input.value
    input.addEventListener('input', () => {
      value.textContent = input.value
      push(spec.key, Number(input.value))
    })
    row.appendChild(input)
    row.appendChild(value)
  } else if (spec.type === 'range') {
    const current = (values()[spec.key] ?? spec.default) as number[]
    const from = document.createElement('input')
    from.type = 'number'
    from.id = `ctl-${spec.key}-min`
    from.min = String(spec.min ?? 0)
    from.max = String(spec.max ?? 100)
    from.value = String(current[0])
    const to = document.createElement('input')
    to.type = 'number'
    to.id = `ctl-${spec.key}-max`
    to.min = String(spec.min ?? 0)
    to.max = String(spec.max ?? 100)
    to.value = String(current[1])
    const commit = () => {
      const a = Math.min(Number(from.value), Number(to.value))
      const b = Math.max(Number(from.value), Number(to.value))
      push(spec.key, [a, b])
    }
    from.addEventListener('change', commit)
    to.addEventListener('change', commit)
    row.appendChild(from)
    row.appendChild(document.createTextNode('—'))
    row.appendChild(to)
  } else if (spec.type === 'color') {
    const input = document.createElement('input')
    input.type = 'color'
    input.id = `ctl-${spec.key}`
    input.value = String(values()[spec.key] ?? spec.default)
    input.addEventListener('input', () => push(spec.key, input.value))
    row.appendChild(input)
  } else if (spec.type === 'toggle') {
    const input = document.createElement('input')
    input.type = 'checkbox'
    input.id = `ctl-${spec.key}`
    input.checked = Boolean(values()[spec.key] ?? spec.default)
    input.addEventListener('change', () => push(spec.key, input.checked))
    row.appendChild(input)
  } else if (spec.type === 'select') {
    const input = document.createElement('select')
    input.id = `ctl-${spec.key}`
    for (const option of spec.options ?? []) {
      const element = document.createElement('option')
      element.value = option.value
      element.textContent = option.label
      input.appendChild(element)
    }
    input.value = String(values()[spec.key] ?? spec.default)
    input.addEventListener('change', () => push(spec.key, input.value))
    row.appendChild(input)
  }

  return row
}

function renderControls() {
  controls.replaceChildren()
  const schema = activeSchema()
  if (!schema) return
  for (const spec of schema.settings) controls.appendChild(makeRow(spec))
}

function syncValues() {
  const schema = activeSchema()
  if (!schema) return
  const current = values()

  for (const spec of schema.settings) {
    if (spec.type === 'slider') {
      const input = document.getElementById(`ctl-${spec.key}`) as HTMLInputElement | null
      if (input && document.activeElement !== input) {
        input.value = String(current[spec.key] ?? spec.default)
        const value = input.parentElement?.querySelector('.value')
        if (value) value.textContent = input.value
      }
    } else if (spec.type === 'color' || spec.type === 'select') {
      const input = document.getElementById(`ctl-${spec.key}`) as HTMLInputElement | null
      if (input && document.activeElement !== input) {
        input.value = String(current[spec.key] ?? spec.default)
      }
    } else if (spec.type === 'toggle') {
      const input = document.getElementById(`ctl-${spec.key}`) as HTMLInputElement | null
      if (input) input.checked = Boolean(current[spec.key] ?? spec.default)
    } else if (spec.type === 'range') {
      const list = (current[spec.key] ?? spec.default) as number[]
      const from = document.getElementById(`ctl-${spec.key}-min`) as HTMLInputElement | null
      const to = document.getElementById(`ctl-${spec.key}-max`) as HTMLInputElement | null
      if (from && document.activeElement !== from) from.value = String(list[0])
      if (to && document.activeElement !== to) to.value = String(list[1])
    }
  }

  pauseButton.textContent = state.paused ? 'Продолжить' : 'Пауза'
}

function buildEffectSelect() {
  effectSelect.replaceChildren()
  for (const schema of schemas) {
    const option = document.createElement('option')
    option.value = schema.id
    option.textContent = schema.name
    effectSelect.appendChild(option)
  }
  effectSelect.value = state.activeEffect ?? ''
  effectSelect.addEventListener('change', () => ipc.setActive(effectSelect.value))
}

async function main() {
  schemas = await ipc.getSchemas()
  state = await ipc.getState()

  buildEffectSelect()
  renderControls()
  syncValues()

  ipc.onState((next) => {
    const switched = next.activeEffect !== state.activeEffect
    state = next
    if (switched) {
      effectSelect.value = state.activeEffect ?? ''
      renderControls()
    }
    syncValues()
  })

  pauseButton.addEventListener('click', () => ipc.setPaused(!state.paused))
  document.getElementById('reset')?.addEventListener('click', () => ipc.resetSettings())
  document.getElementById('close')?.addEventListener('click', () => window.close())
}

main().catch((error) => console.error(error))
