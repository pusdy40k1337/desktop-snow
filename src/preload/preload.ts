import { contextBridge, ipcRenderer } from 'electron'

contextBridge.exposeInMainWorld('ipc', {
  getState: () => ipcRenderer.invoke('state:get'),
  onState: (cb: (state: any) => void) => {
    ipcRenderer.on('state', (_event, state) => cb(state))
  },
  getSchemas: () => ipcRenderer.invoke('schemas:get'),
  setActive: (id: string) => ipcRenderer.send('effect:active', id),
  setSetting: (effectId: string, key: string, value: any) =>
    ipcRenderer.send('setting:set', effectId, key, value),
  resetSettings: () => ipcRenderer.send('settings:reset'),
  setPaused: (paused: boolean) => ipcRenderer.send('app:pause', paused),
  openSettings: () => ipcRenderer.send('settings:open'),
  quit: () => ipcRenderer.send('app:quit'),
  onWindows: (cb: (rects: any[]) => void) => {
    ipcRenderer.on('windows', (_event, rects) => cb(rects))
  },
  onCommand: (cb: (command: string) => void) => {
    ipcRenderer.on('command', (_event, command) => cb(command))
  },
})
