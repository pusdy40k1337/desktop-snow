// Мост между окнами (renderer / settings) и главным процессом.
import type { AppState, EffectSchema } from '../shared/types'

export interface IpcBridge {
  getState(): Promise<AppState>
  onState(cb: (state: AppState) => void): void
  onWindows(cb: (payload: {
    rects: any[]
    dockTop: number
    dock: { x: number; y: number; w: number; h: number } | null
    tick: number
    battery?: boolean
  }) => void): void
  onCommand(cb: (command: string) => void): void
  getSchemas(): Promise<EffectSchema[]>
  setActive(id: string): void
  setSetting(effectId: string, key: string, value: any): void
  resetSettings(): void
  setPaused(paused: boolean): void
  openSettings(): void
  quit(): void
}

declare global {
  interface Window {
    ipc: IpcBridge
  }
}

export const ipc: IpcBridge = (window as any).ipc
