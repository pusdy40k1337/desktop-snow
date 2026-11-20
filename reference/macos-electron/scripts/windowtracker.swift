import Foundation
import AppKit
import CoreGraphics

// Трекер окон: раз в 50 мс печатает JSON-строку со списком видимых окон.
// Без разрешения Screen Recording macOS отдаёт только окна своего процесса —
// тогда полей почти нет, и приложение рисует снег только на Dock.

func hasScreenPermission() -> Bool {
    if #available(macOS 10.15, *) {
        return CGPreflightScreenCaptureAccess()
    }
    return true
}

/// Границы всех активных дисплеев — нужны, чтобы узнавать полноэкранные окна.
func activeDisplayBounds() -> [CGRect] {
    var count: UInt32 = 0
    CGGetActiveDisplayList(0, nil, &count)
    var ids = [CGDirectDisplayID](repeating: 0, count: Int(max(count, 1)))
    CGGetActiveDisplayList(count, &ids, &count)
    return ids.prefix(Int(count)).map { CGDisplayBounds($0) }
}

/// Прямоугольник панели Dock через Accessibility: объединяем рамки иконок.
/// Без разрешения Accessibility возвращает nil — тогда снег у панели не копится.
func dockRect() -> [String: Double]? {
    guard AXIsProcessTrusted() else { return nil }
    guard let dockApp = NSRunningApplication.runningApplications(withBundleIdentifier: "com.apple.dock").first else { return nil }
    let app = AXUIElementCreateApplication(dockApp.processIdentifier)

    var childrenRef: CFTypeRef?
    guard AXUIElementCopyAttributeValue(app, kAXChildrenAttribute as CFString, &childrenRef) == .success,
          let children = childrenRef as? [AXUIElement] else { return nil }

    var items: [AXUIElement] = []
    var container: (Double, Double, Double, Double)? = nil
    for child in children {
        var roleRef: CFTypeRef?
        AXUIElementCopyAttributeValue(child, kAXRoleAttribute as CFString, &roleRef)
        guard (roleRef as? String) == "AXList" else { continue }
        // рамка самого контейнера — это и есть полоса панели с иконками:
        // она сразу отражает добавление/удаление приложений и смену размера иконок
        var posRef: CFTypeRef?
        var sizeRef: CFTypeRef?
        if AXUIElementCopyAttributeValue(child, kAXPositionAttribute as CFString, &posRef) == .success,
           AXUIElementCopyAttributeValue(child, kAXSizeAttribute as CFString, &sizeRef) == .success,
           let posValue = posRef, let sizeValue = sizeRef {
            var point = CGPoint.zero
            var size = CGSize.zero
            AXValueGetValue(posValue as! AXValue, .cgPoint, &point)
            AXValueGetValue(sizeValue as! AXValue, .cgSize, &size)
            if size.width > 40 {
                if container == nil || size.width > container!.2 {
                    container = (Double(point.x), Double(point.y), Double(size.width), Double(size.height))
                }
            }
        }
        var innerRef: CFTypeRef?
        if AXUIElementCopyAttributeValue(child, kAXChildrenAttribute as CFString, &innerRef) == .success,
           let inner = innerRef as? [AXUIElement] {
            items = inner
        }
    }
    // у контейнера есть отступы вокруг иконок — берём его, он совпадает с панелью
    if let c = container {
        return ["x": c.0, "y": c.1, "w": c.2, "h": c.3]
    }
    if items.isEmpty { items = children }

    var minX = Double.greatestFiniteMagnitude
    var minY = Double.greatestFiniteMagnitude
    var maxX = -Double.greatestFiniteMagnitude
    var maxY = -Double.greatestFiniteMagnitude

    for item in items {
        var posRef: CFTypeRef?
        var sizeRef: CFTypeRef?
        guard AXUIElementCopyAttributeValue(item, kAXPositionAttribute as CFString, &posRef) == .success,
              AXUIElementCopyAttributeValue(item, kAXSizeAttribute as CFString, &sizeRef) == .success,
              let posValue = posRef, let sizeValue = sizeRef else { continue }
        var point = CGPoint.zero
        var size = CGSize.zero
        AXValueGetValue(posValue as! AXValue, .cgPoint, &point)
        AXValueGetValue(sizeValue as! AXValue, .cgSize, &size)
        guard size.width > 4, size.height > 4 else { continue }
        minX = min(minX, Double(point.x))
        minY = min(minY, Double(point.y))
        maxX = max(maxX, Double(point.x + size.width))
        maxY = max(maxY, Double(point.y + size.height))
    }

    guard minX < maxX, minY < maxY else { return nil }
    return ["x": minX, "y": minY, "w": maxX - minX, "h": maxY - minY]
}

var excludedPid = Int(getpid())
var excludedOwner: String? = nil
/// Прозрачные служебные окна: их нельзя считать поверхностью и нельзя прятать за ними снег.
let serviceOwners = ["Cua Driver", "CuaDriver", "Screen Sharing", "Window Server", "Control Center", "Notification Center", "SystemUIServer", "Знімок екрана", "Screenshot", "Снимок экрана", "Dock"]

/// Полноэкранный режим через Accessibility: атрибут AXFullScreen у активного окна.
/// Геометрия окон ненадёжна — macOS отдаёт полноэкранный браузер как 1470x801
/// со смещением по x при переключении Spaces. Кэш 500 мс: AX-запросы дорогие.
var axFullscreenCacheTime = Date.distantPast
var axFullscreenCachePid: pid_t = 0
var axFullscreenCacheValue: Bool? = nil

func axFullscreen(pid: pid_t) -> Bool? {
    let now = Date()
    if axFullscreenCachePid == pid, now.timeIntervalSince(axFullscreenCacheTime) < 0.5 {
        return axFullscreenCacheValue
    }
    axFullscreenCachePid = pid
    axFullscreenCacheTime = now
    var result: Bool? = nil
    let app = AXUIElementCreateApplication(pid)
    var windowRef: CFTypeRef?
    if AXUIElementCopyAttributeValue(app, kAXFocusedWindowAttribute as CFString, &windowRef) == .success,
       let windowRef = windowRef,
       CFGetTypeID(windowRef) == AXUIElementGetTypeID() {
        let window = windowRef as! AXUIElement
        var fullRef: CFTypeRef?
        if AXUIElementCopyAttributeValue(window, "AXFullScreen" as CFString, &fullRef) == .success,
           let value = fullRef as? Bool {
            result = value
        } else {
            // атрибута нет — сравниваем размер окна с экраном
            var sizeRef: CFTypeRef?
            if AXUIElementCopyAttributeValue(window, kAXSizeAttribute as CFString, &sizeRef) == .success,
               let sizeRef = sizeRef, CFGetTypeID(sizeRef) == AXValueGetTypeID() {
                var size = CGSize.zero
                if AXValueGetValue(sizeRef as! AXValue, .cgSize, &size), let screen = NSScreen.main {
                    let sameWidth = abs(size.width - screen.frame.width) < 8
                    let sameHeight = abs(size.height - screen.frame.height) < 8
                    result = sameWidth && sameHeight
                }
            }
        }
    }
    axFullscreenCacheValue = result
    return result
}
var lastDebugLine = ""
func appendDebug(_ line: String) {
    guard line != lastDebugLine else { return }
    lastDebugLine = line
    let stamp = ISO8601DateFormatter().string(from: Date())
    let text = "[\(stamp)] \(line)\n"
    let path = "/tmp/snow-debug.log"
    if let handle = FileHandle(forWritingAtPath: path) {
        handle.seekToEndOfFile()
        handle.write(Data(text.utf8))
        handle.closeFile()
    } else {
        try? text.write(toFile: path, atomically: true, encoding: .utf8)
    }
}
// окна самого приложения (оверлеи) нужно исключить: pid + имя владельца окна
if let index = CommandLine.arguments.firstIndex(of: "--exclude-pid"),
   index + 1 < CommandLine.arguments.count,
   let excluded = Int(CommandLine.arguments[index + 1]) {
    excludedPid = excluded
}
if let index = CommandLine.arguments.firstIndex(of: "--exclude-owner"),
   index + 1 < CommandLine.arguments.count {
    excludedOwner = CommandLine.arguments[index + 1]
}

func snapshot() -> [String: Any] {
    var windows: [[String: Any]] = []
    let displays = activeDisplayBounds()
    // полноэкранным считаем только окно активного приложения: иначе любой прозрачный
    // оверлей чужой утилиты (демонстрация экрана, драйвер автоматизации) ставил бы паузу
    let frontmostPid = NSWorkspace.shared.frontmostApplication?.processIdentifier
    let options: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]

    /// Окно занимает почти весь экран (≥80 % площади дисплея). Пауза нужна на
    /// ЛЮБОЕ окно, заполнившее экран, — и на нативный fullscreen, и на «окно
    /// на весь экран» (рамочное/развёрнутое), по явной просьбе пользователя:
    ///   • окно вне дисплея — полноэкранный Space отдаёт виртуальные координаты
    ///     (x=1533, x=−1531); совпадение размеров ±10 %/±20 % отсеивает чужие;
    ///   • верхний зазор ≤5 px — окно перекрыло меню-бар (нативный fullscreen);
    ///   • окно ДОХОДИТ ДО НИЗА экрана (±20 px) — развёрнутое окно (Hermes,
    ///     Discord: 1470x923 при y=33, ratio 0.97) или рамочный fullscreen
    ///     (Chrome: 1470x801 при y=155). Раньше развёрнутые окна не паузили —
    ///     пользователь считал это поломкой («снег идёт при окне на весь экран»).
    func coversDisplay(_ x: Double, _ y: Double, _ w: Double, _ h: Double) -> Bool {
        // мусорные размеры CGWindowList (окна-призраки с гигантскими bounds)
        guard w > 0, h > 0, w < 20000, h < 20000 else { return false }
        for display in displays {
            guard display.width > 100, display.height > 100 else { continue }
            if (w * h) / (display.width * display.height) < 0.80 { continue }
            let onThisDisplay = x >= display.minX - 50 && x + w <= display.maxX + 50
                && y >= display.minY - 50 && y + h <= display.maxY + 50
            if !onThisDisplay {
                // окно этого размера вне дисплея — полноэкранный Space с виртуальными
                // координатами; чужое окно соседнего дисплея отсеивается размерами ниже
                if abs(w - display.width) <= display.width * 0.10,
                   abs(h - display.height) <= display.height * 0.20 { return true }
                continue
            }
            let topGap = y - display.minY
            let bottomFill = (y + h) >= display.maxY - 20
            if topGap <= 5 || bottomFill { return true }
        }
        return false
    }

    var geometryFullscreen = false
    var debugBest = ""
    let displayArea = displays.first.map { $0.width * $0.height } ?? 1
    // pid'ы приложений, у которых есть окно «на всю ширину и почти весь низ» —
    // кандидаты на полноэкранный Space: проверяем у них AXFullScreen напрямую
    var fullscreenCandidates: Set<Int> = []
    if let list = CGWindowListCopyWindowInfo(options, kCGNullWindowID) as? [[String: Any]] {
        // 1) Пауза. У активного приложения проверяем ВСЕ окна, независимо от слоя:
        //    полноэкранный браузер отдаёт окно не на layer 0, и раньше он не ловился.
        for entry in list {
            let pid = entry[kCGWindowOwnerPID as String] as? Int ?? -1
            guard pid != excludedPid else { continue }
            if let owner = entry[kCGWindowOwnerName as String] as? String,
               let skip = excludedOwner,
               owner.caseInsensitiveCompare(skip) == .orderedSame { continue }
            guard let bounds = entry[kCGWindowBounds as String] as? [String: Any],
                  let x = bounds["X"] as? Double,
                  let y = bounds["Y"] as? Double,
                  let width = bounds["Width"] as? Double,
                  let height = bounds["Height"] as? Double,
                  width >= 140, height >= 100 else { continue }
            let alpha = entry[kCGWindowAlpha as String] as? Double ?? 1
            let layer = entry[kCGWindowLayer as String] as? Int ?? -999
            // служебные окна (драйверы, панель скриншота) — не признаки fullscreen
            let ownerName = entry[kCGWindowOwnerName as String] as? String ?? ""
            if serviceOwners.contains(where: { ownerName.localizedCaseInsensitiveContains($0) }) { continue }
            // полноэкранный Space ловим по ЛЮБОМУ окну на экране, не только активного
            // приложения: Chrome в fullscreen на другом Space — frontmost остаётся прежним,
            // а окно-то на экране. Это и была причина «снег идёт в полноэкранном браузере».
            // layer==0: Dock (20), скриншот-панель (24) и оверлеи не в счёт.
            if alpha > 0.5, layer == 0, coversDisplay(x, y, width, height) {
                geometryFullscreen = true
                // лог виновника: какое окно сочтено «на весь экран» — иначе
                // ложные паузы не разобрать (ratio выше пишется только для frontmost)
                appendDebug("geom-culprit owner=\(ownerName) \(Int(width))x\(Int(height)) y=\(Int(y))")
            }
            // окно во всю ширину и до низа экрана — кандидат на полноэкранный Space
            // (у полноэкранного контент доходит до дна; у рамочного «зума» — тоже, поэтому
            // решает только AXFullScreen у владельца, геометрия лишь отбирает кандидатов)
            if alpha > 0.5, layer == 0, width >= 1400, y + height >= 930, y > 60 {
                fullscreenCandidates.insert(pid)
            }
            if pid == Int(frontmostPid ?? 0), alpha > 0.5 {
                let line = "ratio=\(String(format: "%.2f", (width * height) / displayArea)) layer=\(layer) \(Int(x)),\(Int(y)) \(Int(width))x\(Int(height))"
                if line != debugBest { debugBest = line }
            }
        }
        if !debugBest.isEmpty { appendDebug(debugBest) }

        // 2) Список поверхностей для сугробов и маски — только обычные окна.
        for (index, entry) in list.enumerated() {
            guard (entry[kCGWindowLayer as String] as? Int) == 0 else { continue }
            let pid = entry[kCGWindowOwnerPID as String] as? Int ?? -1
            if pid == excludedPid { continue }
            // оверлей принадлежит другому pid, поэтому ловим его ещё и по имени владельца
            if let owner = entry[kCGWindowOwnerName as String] as? String,
               let skip = excludedOwner,
               owner.caseInsensitiveCompare(skip) == .orderedSame { continue }
            guard let bounds = entry[kCGWindowBounds as String] as? [String: Any],
                  let x = bounds["X"] as? Double,
                  let y = bounds["Y"] as? Double,
                  let width = bounds["Width"] as? Double,
                  let height = bounds["Height"] as? Double else { continue }
            // служебные оверлеи (драйверы автоматизации, демонстрация экрана, панели)
            // невидимы пользователю, но перекрывают весь экран — из маски их убираем,
            // иначе снег прятался за ними целиком
            let ownerName = entry[kCGWindowOwnerName as String] as? String ?? ""
            if serviceOwners.contains(where: { ownerName.localizedCaseInsensitiveContains($0) }) {
                continue
            }
            // мелочь вроде теней, тултипов и панелей не считаем поверхностью
            guard width >= 140, height >= 100 else { continue }
            let alpha = entry[kCGWindowAlpha as String] as? Double ?? 1
            let isFullscreen = false // полноэкранность выше считается по активному приложению
            _ = alpha
            let number = entry[kCGWindowNumber as String] as? Int ?? 0
            windows.append([
                "id": number,
                "x": x,
                "y": y,
                "w": width,
                "h": height,
                "z": index, // список CGWindowList идёт спереди назад
                "fullscreen": isFullscreen,
                "isFullscreen": isFullscreen,
            ])
        }
    }
    // AX надёжнее геометрии; если приложение не отдаёт атрибут — работает эвристика
    let axValue = frontmostPid.flatMap { axFullscreen(pid: $0) }
    // AX у активного окна + AX-опрос полноэкранноподобных окон других приложений:
    // Chrome в fullscreen Space может быть не активным приложением (фокус у Hermes),
    // а его окно геометрически неотличимо от рамочного (панель 0,33 1470x158 + контент
    // 0,155 1470x801). Поэтому спросим AXFullScreen напрямую у владельца такого окна.
    var anyAxFullscreen = false
    if !fullscreenCandidates.isEmpty {
        for candidate in fullscreenCandidates {
            if axFullscreen(pid: pid_t(candidate)) == true { anyAxFullscreen = true; break }
        }
    }
    let fullscreen = anyAxFullscreen || (axValue ?? false) || geometryFullscreen
    // dock в той же строке в ФИКСИРОВАННОМ формате: словарь Swift сериализуется
    // с рандомным порядком ключей, из-за чего дедуп appendDebug пропускал каждую
    // строку и лог раздувался. Формат отсортирован — печать только при изменении
    let dockLine = dockPayload()
    var dockText = "nil"
    if let d = dockLine as? [String: Double] {
        dockText = "x=\(d["x"] ?? -1) y=\(d["y"] ?? -1) w=\(d["w"] ?? -1) h=\(d["h"] ?? -1)"
    }
    appendDebug("fullscreen=\(fullscreen) ax=\(axValue.map(String.init) ?? "nil") anyAX=\(anyAxFullscreen) geometry=\(geometryFullscreen) dock=\(dockText)")
    return ["permission": hasScreenPermission(), "fullscreen": fullscreen, "windows": windows, "dock": dockLine, "axTrusted": AXIsProcessTrusted(), "space": spaceCounter]
}

/// Оценка панели Dock по её настройкам — если Accessibility не выдан.
/// Ширина приблизительная (учитываем иконки и запас), высота не важна: её берём
/// из рабочей области экрана.
func estimatedDockRect() -> [String: Double]? {
    guard let dockPrefs = UserDefaults(suiteName: "com.apple.dock") else { return nil }
    let tilesize = dockPrefs.double(forKey: "tilesize")
    guard tilesize > 10 else { return nil }
    let apps = dockPrefs.array(forKey: "persistent-apps")?.count ?? 0
    let others = dockPrefs.array(forKey: "persistent-others")?.count ?? 0
    let items = max(2, apps + others + 2) // + Корзина и небольшой запас
    let bounds = CGDisplayBounds(CGMainDisplayID())
    let width = min(bounds.width * 0.92, Double(items) * tilesize + 28)
    let height = max(48, tilesize + 24)
    return [
        "x": Double(bounds.midX) - width / 2,
        "y": Double(bounds.maxY) - height - 8,
        "w": width,
        "h": height,
    ]
}

var cachedDock: [String: Double]? = nil
var dockCounter = 0
var dockMisses = 0

/// Счётчик смены рабочих столов: при переключении Space снег очищается.
var spaceCounter = 0
NSWorkspace.shared.notificationCenter.addObserver(
    forName: NSWorkspace.activeSpaceDidChangeNotification,
    object: nil,
    queue: .main
) { _ in
    spaceCounter += 1
}

/// Прямоугольник Dock пересчитываем ~6 раз в секунду: при добавлении приложений
/// панель меняет ширину, и сугроб должен успеть за ней.
func dockPayload() -> Any {
    dockCounter += 1
    if dockCounter % 10 == 1 {
        if let fresh = dockRect() ?? estimatedDockRect() {
            cachedDock = fresh
            dockMisses = 0
        } else {
            dockMisses += 1
            if dockMisses > 3 { cachedDock = nil } // пропал по-настоящему (Dock скрыт)
        }
    }
    return cachedDock ?? NSNull()
}

let queue = DispatchQueue(label: "windowtracker")
let timer = DispatchSource.makeTimerSource(queue: queue)

// отладка: показать оценку Dock без Accessibility и выйти
if CommandLine.arguments.contains("--estimate-dock") {
    print(estimatedDockRect() ?? [:])
    exit(0)
}
/// Частота опроса: 60 Гц, пока окна двигаются, и 25 Гц в покое — иначе трекер
/// впустую жжёт процессор, когда ничего не происходит.
var currentIntervalMs = 16
var idleTicks = 0
var lastSignature = ""

timer.schedule(deadline: .now(), repeating: .milliseconds(16))
timer.setEventHandler {
    let snap = snapshot()
    // подпись = позиции окон: меняется только при реальном движении
    let signature = (snap["windows"] as? [[String: Any]])?
        .map { "\($0["id"] as? Int ?? 0):\($0["x"] as? Double ?? 0):\($0["y"] as? Double ?? 0)" }
        .joined() ?? ""
    if signature == lastSignature {
        idleTicks += 1
    } else {
        idleTicks = 0
        lastSignature = signature
    }
    if idleTicks > 25, currentIntervalMs == 16 {
        currentIntervalMs = 40
        timer.schedule(deadline: .now() + .milliseconds(40), repeating: .milliseconds(40))
    } else if idleTicks == 0, currentIntervalMs == 40 {
        currentIntervalMs = 16
        timer.schedule(deadline: .now() + .milliseconds(16), repeating: .milliseconds(16))
    }
    guard let data = try? JSONSerialization.data(withJSONObject: snap),
          let line = String(data: data, encoding: .utf8) else { return }
    print(line)
    fflush(stdout)
}
timer.resume()
RunLoop.main.run()
