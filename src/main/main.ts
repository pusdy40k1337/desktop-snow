import { app, BrowserWindow, Menu, Tray, nativeImage, ipcMain, powerMonitor, screen, shell, desktopCapturer } from 'electron'
import * as fs from 'node:fs'
import * as path from 'node:path'
import * as readline from 'node:readline'
import { spawn, type ChildProcess } from 'node:child_process'
import { Store } from './settings'
import type { AppState, EffectSchema, WindowRect } from '../shared/types'

const DIST = __dirname
const APP_NAME = 'Снег на десктопе'

const overlays = new Map<string, BrowserWindow>()
const overlayBounds = new Map<number, Electron.Rectangle>()
// верхняя грань Dock в координатах оверлея — из рабочей области экрана
const overlayDockTop = new Map<number, number>()
let settingsWin: BrowserWindow | null = null
let tray: Tray | null = null
let store: Store
let schemas: EffectSchema[] = []
let systemPaused = false
let fullscreenPaused = false
// счётчик подряд идущих тиков с fullscreen=true (дебаунс ложных вспышек)
let fullscreenStreak = 0

// MARK: эффекты

function loadSchemas(): EffectSchema[] {
  const dir = path.join(DIST, 'effects')
  if (!fs.existsSync(dir)) return []
  return fs
    .readdirSync(dir)
    .filter((id) => fs.existsSync(path.join(dir, id, 'manifest.json')))
    .map((id) => JSON.parse(fs.readFileSync(path.join(dir, id, 'manifest.json'), 'utf8')))
}

// MARK: оверлеи

function createOverlay(display: Electron.Display, role: 'all' | 'flakes' | 'mounds') {
  const win = new BrowserWindow({
    x: display.bounds.x,
    y: display.bounds.y,
    width: display.bounds.width,
    height: display.bounds.height,
    transparent: true,
    frame: false,
    hasShadow: false,
    resizable: false,
    movable: false,
    minimizable: false,
    maximizable: false,
    fullscreenable: false,
    skipTaskbar: true,
    focusable: false,
    acceptFirstMouse: false,
    enableLargerThanScreen: true,
    backgroundColor: '#00000000',
    show: false,
    webPreferences: {
      preload: path.join(DIST, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
      backgroundThrottling: false,
    },
  })

  // выше обычных окон, ниже Dock (20): хлопья на самих окнах не рисуются —
  // это делает маска в эффекте, поэтому содержимое окон остаётся чистым
  win.setAlwaysOnTop(true, process.platform === 'darwin' ? 'floating' : 'screen-saver')
  try {
    win.setVisibleOnAllWorkspaces(true, { visibleOnFullScreen: true })
  } catch {
    // Windows: метод не поддерживается
  }
  win.setIgnoreMouseEvents(true, { forward: true })

  // диагностика: всё, что пишет рендерер, видно в консоли главного процесса
  win.webContents.on('console-message', (_event, level, message, line, source) => {
    console.log(`[renderer:${level}] ${message} (${source}:${line})`)
    try { fs.appendFileSync('/tmp/snow-render.log', `${new Date().toISOString()} ${message}\n`) } catch {}
  })
  win.webContents.on('did-fail-load', (_event, code, description) => {
    console.error('[overlay] не загрузился:', code, description)
  })
  win.webContents.on('render-process-gone', (_event, details) => {
    console.error('[overlay] рендерер упал:', details.reason)
  })

  win.loadFile(path.join(DIST, 'renderer/index.html'), {
    query: { display: String(display.id), role },
  })
  win.once('ready-to-show', () => win.showInactive())
  overlays.set(`${display.id}:${role}`, win)
  overlayBounds.set(display.id, display.bounds)
  // workArea не включает Dock и меню-бар → её нижняя грань и есть верх Dock
  overlayDockTop.set(
    display.id,
    display.workArea.y + display.workArea.height - display.bounds.y,
  )
}

function rebuildOverlays() {
  for (const [key, win] of [...overlays]) {
    // снимаем с учёта ДО destroy: иначе трекер успевает отправить кадр в мёртвый фрейм
    overlays.delete(key)
    if (!win.isDestroyed()) win.destroy()
  }
  overlayBounds.clear()
  overlayDockTop.clear()
  for (const display of screen.getAllDisplays()) createOverlay(display, 'all')
  lastDisplaySignature = displaySignature()
  broadcast()
}

/**
 * «Отпечаток» набора экранов: геометрия, масштаб, поворот. Изменение рабочей
 * области (Dock меняет размер, меню-бар) в отпечаток не входит — из-за него
 * оверлей пересоздавать нельзя, иначе снегопад каждый раз обнуляется.
 */
function displaySignature(): string {
  return screen
    .getAllDisplays()
    .map(
      (d) =>
        `${d.id}:${d.bounds.width}x${d.bounds.height}@${d.bounds.x},${d.bounds.y}:${d.scaleFactor ?? 1}:${d.rotation ?? 0}`,
    )
    .sort()
    .join('|')
}

let lastDisplaySignature = ''

function onMetricsChanged() {
  if (displaySignature() === lastDisplaySignature) {
    // изменилась только рабочая область (панель Dock) — окна живы, обновим кэш
    for (const display of screen.getAllDisplays()) {
      overlayDockTop.set(
        display.id,
        display.workArea.y + display.workArea.height - display.bounds.y,
      )
    }
    broadcast()
    return
  }
  rebuildOverlays()
}

// MARK: состояние

function currentState(): AppState {
  const state = store.state()
  return { ...state, paused: state.paused || systemPaused || fullscreenPaused }
}

function broadcast() {
  const state = currentState()
  for (const win of overlays.values()) {
    if (win.isDestroyed() || win.webContents.isDestroyed()) continue
    try {
      win.webContents.send('state', state)
    } catch {
      // окно закрывается
    }
  }
  if (settingsWin && !settingsWin.isDestroyed() && !settingsWin.webContents.isDestroyed()) {
    try {
      settingsWin.webContents.send('state', state)
    } catch {
      // окно настроек закрывается
    }
  }
  updateTrayMenu()
}

// MARK: трекер окон (нативный хелпер, 20 Гц)

let tracker: ChildProcess | null = null
let trackerPermission = true
let quitting = false
let trackerDock: { x: number; y: number; w: number; h: number } | null = null
let trackerAxTrusted = false
// режим экономии: на батарее рисуем реже и вдвое меньшим числом частиц
let onBatteryPower = false
let batterySample = 0
let lastSpace = -1

function startWindowTracker() {
  const helper = path.join(DIST, 'bin', 'windowtracker')
  // нативный хелпер есть только под macOS: на Windows/Linux работает заглушка
  if (process.platform !== 'darwin' || !fs.existsSync(helper)) {
    console.warn('[tracker] нативный хелпер недоступен — работает заглушка')
    startFallbackTracker()
    return
  }
  const ownName = path.parse(process.execPath).name // «Electron» в dev, «Снег» в бандле
  tracker = spawn(helper, ['--exclude-pid', String(process.pid), '--exclude-owner', ownName], {
    stdio: ['ignore', 'pipe', 'inherit'],
  })
  if (!tracker.stdout) return
  const lines = readline.createInterface({ input: tracker.stdout })
  lines.on('line', (line) => {
    try {
      handleTrackerPayload(JSON.parse(line))
    } catch {
      // битую строку просто пропускаем
    }
  })
  tracker.on('exit', () => console.warn('[tracker] хелпер завершился'))
}

/** Общая обработка данных трекера: используется и нативным хелпером, и заглушкой. */
function handleTrackerPayload(payload: {
  permission: boolean
  fullscreen?: boolean
  windows: WindowRect[]
  dock?: { x: number; y: number; w: number; h: number } | null
  axTrusted?: boolean
  space?: number
}) {
  const axChanged = Boolean(payload.axTrusted) !== trackerAxTrusted
  trackerAxTrusted = Boolean(payload.axTrusted)
  trackerDock = payload.dock ?? null
  // переключение рабочего стола: чистим снег и приостанавливаем эффект
  if (typeof payload.space === 'number' && payload.space !== lastSpace) {
    const first = lastSpace === -1
    lastSpace = payload.space
    if (!first) {
      sendCommand('clear')
      systemPaused = true
      broadcast()
      setTimeout(() => {
        systemPaused = false
        broadcast()
      }, 1200)
    }
  }
  const permissionChanged = payload.permission !== trackerPermission
  // полноэкранное приложение = пауза (ТЗ, раздел 9).
  // Дебаунс: перезапуск Dock и анимации закрытия окон дают ОДНОТИКОВЫЕ всплески
  // fullscreen=true от транзитных окон WindowServer — без дебаунса каждая такая
  // вспышка стирала весь накопленный снег (очистка при входе в паузу). Настоящий
  // полноэкранный режим держится секундами, задержка паузы ~0.7 с незаметна
  fullscreenStreak = Boolean(payload.fullscreen) ? fullscreenStreak + 1 : 0
  const shouldPause = Boolean(payload.fullscreen) && fullscreenStreak >= 20
  const fullscreenChanged = shouldPause !== fullscreenPaused
  trackerPermission = payload.permission
  fullscreenPaused = shouldPause
  if (fullscreenChanged) {
    console.log('[tracker] fullscreen ->', fullscreenPaused, fullscreenPaused ? ': пауза + очистка' : ': возобновление')
    fs.appendFileSync('/tmp/snow-render.log', `${new Date().toISOString()} [main] fullscreen=${fullscreenPaused} пауза\n`)
  }
  if (fullscreenChanged && fullscreenPaused) sendCommand('clear')
  pushWindows(payload.windows)
  if (permissionChanged || fullscreenChanged || axChanged) broadcast()
}

/**
 * Заглушка вместо нативного хелпера: Windows/Linux и случай отсутствующего бинарника.
 * Снег идёт по всему экрану, панель задач снизу получает сугроб, окна не отслеживаются.
 */
function startFallbackTracker() {
  const emit = () => {
    const display = screen.getPrimaryDisplay()
    const wa = display.workArea
    const bottom = display.bounds.y + display.bounds.height
    const taskbar = Math.max(0, bottom - (wa.y + wa.height))
    handleTrackerPayload({
      permission: true,
      fullscreen: false,
      windows: [],
      dock:
        taskbar > 8
          ? { x: wa.x, y: wa.y + wa.height, w: wa.width, h: taskbar }
          : null,
      axTrusted: true,
      space: 0,
    })
  }
  emit()
  const timer = setInterval(emit, 250)
  timer.unref()
  screen.on('display-metrics-changed', emit)
}

function pushWindows(all: WindowRect[]) {
  if (quitting) return
  for (const [key, win] of overlays) {
    if (win.isDestroyed() || win.webContents.isDestroyed()) continue
    const displayId = Number(key.split(':')[0])
    const bounds = overlayBounds.get(displayId)
    if (!bounds) continue
    const local = all
      .filter(
        (rect) =>
          rect.x + rect.w > bounds.x &&
          rect.x < bounds.x + bounds.width &&
          rect.y + rect.h > bounds.y &&
          rect.y < bounds.y + bounds.height,
      )
      .map((rect) => ({ ...rect, x: rect.x - bounds.x, y: rect.y - bounds.y }))
    // Своё окно настроек трекер не отдаёт (исключено по владельцу), а снег на нём
    // рисоваться не должен — добавляем его прямоугольник в маску вручную.
    if (settingsWin && !settingsWin.isDestroyed()) {
      const sb = settingsWin.getBounds()
      const overlaps =
        sb.x + sb.width > bounds.x &&
        sb.x < bounds.x + bounds.width &&
        sb.y + sb.height > bounds.y &&
        sb.y < bounds.y + bounds.height
      if (overlaps) {
        local.push({
          id: 'settings',
          x: sb.x - bounds.x,
          y: sb.y - bounds.y,
          w: sb.width,
          h: sb.height,
          z: -1, // выше всех: наше окно всегда спереди для маски
          isFullscreen: false,
        })
      }
    }
    let dock: { x: number; y: number; w: number; h: number } | null = null
    if (trackerDock) {
      // рамка панели целиком из AX: и x, и ширина, и верхняя кромка. Раньше y брали
      // из рабочей области (874) — он оказывался на 4 px ниже настоящей кромки (870)
      const x = Math.round(trackerDock.x - bounds.x)
      const w = Math.max(40, Math.round(trackerDock.w))
      const y = Math.round(trackerDock.y - bounds.y)
      if (x + w > 0 && x < bounds.width && y < bounds.height) {
        dock = { x, y, w, h: Math.min(40, bounds.height - y) }
      }
    }
    try {
      win.webContents.send('windows', {
        rects: local,
        dockTop: overlayDockTop.get(displayId) ?? bounds.height,
        dock,
        tick: Date.now(),
        battery: onBatteryPower,
      })
    } catch {
      // фрейм мог умереть между проверкой и отправкой — это штатное закрытие окна
    }
  }
}

function sendCommand(command: string) {
  for (const win of overlays.values()) {
    if (win.isDestroyed() || win.webContents.isDestroyed()) continue
    try {
      win.webContents.send('command', command)
    } catch {
      // окно уже закрывается
    }
  }
}

function openScreenRecordingSettings() {
  shell.openExternal('x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture')
}

function openAccessibilitySettings() {
  shell.openExternal('x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility')
}

/** Просит у macOS доступ к списку окон и открывает нужную панель настроек. */
async function requestScreenPermission() {
  try {
    // этот вызов и вызывает системный запрос Screen Recording
    await desktopCapturer.getSources({ types: ['window'] })
  } catch (error) {
    console.warn('[tracker] запрос доступа не удался:', error)
  }
  openScreenRecordingSettings()
}

// MARK: трей

function createTray() {
  const iconPath = path.join(DIST, 'assets/trayTemplate.png')
  const hasIcon = fs.existsSync(iconPath)
  const image = hasIcon ? nativeImage.createFromPath(iconPath) : nativeImage.createEmpty()
  if (hasIcon) image.setTemplateImage(true)
  tray = new Tray(image)
  if (!hasIcon) tray.setTitle('❄')
  tray.setToolTip(APP_NAME)
  updateTrayMenu()
}

function updateTrayMenu() {
  if (!tray) return
  const state = currentState()
  const template: Electron.MenuItemConstructorOptions[] = [
    { label: 'Эффект', enabled: false },
    ...schemas.map<Electron.MenuItemConstructorOptions>((schema) => ({
      label: schema.name,
      type: 'radio',
      checked: state.activeEffect === schema.id,
      click: () => {
        store.setActive(schema.id)
        broadcast()
      },
    })),
    { type: 'separator' },
    {
      label: state.paused ? 'Продолжить' : 'Пауза',
      type: 'checkbox',
      checked: state.paused,
      click: () => {
        store.setPaused(!state.paused)
        broadcast()
      },
    },
    { label: 'Убрать снег', click: () => sendCommand('clear') },
    ...(trackerPermission
      ? []
      : [{ label: 'Доступ к окнам…', click: () => void requestScreenPermission() }]),
    ...(trackerAxTrusted
      ? []
      : [{ label: 'Доступ к панели Dock…', click: () => openAccessibilitySettings() }]),
    { label: 'Настройки…', accelerator: 'CmdOrCtrl+,', click: () => openSettings() },
    { type: 'separator' },
    { label: 'Выход', accelerator: 'CmdOrCtrl+Q', click: () => app.quit() },
  ]
  tray.setContextMenu(Menu.buildFromTemplate(template))
}

// MARK: окно настроек

function openSettings() {
  if (settingsWin && !settingsWin.isDestroyed()) {
    settingsWin.show()
    settingsWin.focus()
    return
  }
  settingsWin = new BrowserWindow({
    width: 560,
    height: 680,
    minWidth: 460,
    minHeight: 420,
    title: `Настройки — ${APP_NAME}`,
    backgroundColor: '#14161c',
    webPreferences: {
      preload: path.join(DIST, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
    },
  })
  settingsWin.loadFile(path.join(DIST, 'settings.html'))
  settingsWin.on('closed', () => {
    settingsWin = null
  })
}

// MARK: IPC

function registerIpc() {
  ipcMain.handle('state:get', () => currentState())
  ipcMain.handle('schemas:get', () => schemas)
  ipcMain.on('effect:active', (_event, id: string) => {
    store.setActive(id)
    broadcast()
  })
  ipcMain.on('setting:set', (_event, effectId: string, key: string, value: any) => {
    store.setValue(effectId, key, value)
    broadcast()
  })
  ipcMain.on('settings:reset', () => {
    store.reset()
    broadcast()
  })
  ipcMain.on('app:pause', (_event, paused: boolean) => {
    store.setPaused(paused)
    broadcast()
  })
  ipcMain.on('settings:open', () => openSettings())
  ipcMain.on('app:quit', () => app.quit())
}

// MARK: запуск

// один экземпляр приложения: второй запуск открывает настройки, а не второй оверлей
const gotSingleInstanceLock = app.requestSingleInstanceLock()
if (!gotSingleInstanceLock) {
  app.quit()
} else {
  app.on('second-instance', () => openSettings())
}

app.whenReady().then(() => {
  if (!gotSingleInstanceLock) return
  app.setName(APP_NAME)
  app.dock?.hide()

  schemas = loadSchemas()
  store = new Store(path.join(app.getPath('userData'), 'settings.json'), schemas)

  registerIpc()
  rebuildOverlays()
  createTray()
  startWindowTracker()

  screen.on('display-added', rebuildOverlays)
  screen.on('display-removed', rebuildOverlays)
  screen.on('display-metrics-changed', onMetricsChanged)

  // пауза, когда экран заблокирован или система спит
  powerMonitor.on('lock-screen', () => {
    systemPaused = true
    broadcast()
  })
  powerMonitor.on('unlock-screen', () => {
    systemPaused = false
    broadcast()
  })
  powerMonitor.on('suspend', () => {
    systemPaused = true
    broadcast()
  })
  powerMonitor.on('resume', () => {
    systemPaused = false
    broadcast()
  })

  // экономия заряда: на батарее — реже кадры и половина частиц
  onBatteryPower = powerMonitor.isOnBatteryPower()
  powerMonitor.on('on-battery', () => {
    onBatteryPower = true
  })
  powerMonitor.on('on-ac', () => {
    onBatteryPower = false
  })

  // отладочный хук: открыть окно настроек сразу после старта
  if (process.env.DW_OPEN_SETTINGS === '1') openSettings()

  // отладочный хук: корректно завершиться через N секунд (проверка чистого выхода)
  const quitAfter = Number(process.env.DW_QUIT_AFTER ?? 0)
  if (quitAfter > 0) setTimeout(() => app.quit(), quitAfter * 1000)
})

// приложение живёт в строке меню: закрытие окон его не завершает
app.on('window-all-closed', () => {})

app.on('before-quit', () => {
  // сначала гасим трекер: он шлёт 20 кадров в секунду и может опередить закрытие окон
  quitting = true
  tracker?.kill()
  tracker = null
})

// kill по умолчанию не даёт Electron закрыться штатно — превращаем сигналы в app.quit(),
// иначе трекер успевает отправить кадр в уже уничтоженный фрейм
process.on('SIGTERM', () => app.quit())
process.on('SIGINT', () => app.quit())

app.on('will-quit', () => {
  tracker?.kill()
})
