import AppKit

// Иконки: 1024 — иконка приложения (icns), 22/44 — template-иконка в трее.

func isTemplate(prefix: String) -> Bool { prefix.contains("tray") }

func drawSnowflake(in size: CGFloat, lineWidth: CGFloat, color: NSColor) {
    let center = NSPoint(x: size / 2, y: size / 2)
    let radius = size / 2 - lineWidth
    color.setStroke()
    for i in 0..<6 {
        let angle = CGFloat(i) * .pi / 3
        let path = NSBezierPath()
        path.lineWidth = lineWidth
        path.lineCapStyle = .round
        path.move(to: NSPoint(x: center.x + cos(angle) * radius * 0.18,
                              y: center.y + sin(angle) * radius * 0.18))
        path.line(to: NSPoint(x: center.x + cos(angle) * radius,
                              y: center.y + sin(angle) * radius))
        // боковые веточки
        for (offset, length) in [(CGFloat(0.55), CGFloat(0.26)), (0.78, 0.18)] {
            let base = NSPoint(x: center.x + cos(angle) * radius * offset,
                               y: center.y + sin(angle) * radius * offset)
            for branch in [-1.0, 1.0] {
                let branchAngle = angle + CGFloat(branch) * .pi / 3
                path.move(to: base)
                path.line(to: NSPoint(x: base.x + cos(branchAngle) * radius * length,
                                      y: base.y + sin(branchAngle) * radius * length))
            }
        }
        path.stroke()
    }
}

func renderAppIcon(size: CGFloat, to path: String) throws {
    let image = NSImage(size: NSSize(width: size, height: size))
    image.lockFocus()
    NSGraphicsContext.current?.imageInterpolation = .high
    let rect = NSRect(x: 0, y: 0, width: size, height: size)

    // фон: скруглённый квадрат с зимним градиентом
    let inset = size * 0.08
    let bg = NSBezierPath(roundedRect: rect.insetBy(dx: inset, dy: inset),
                          xRadius: size * 0.22, yRadius: size * 0.22)
    bg.addClip()
    NSGradient(colors: [
        NSColor(srgbRed: 0.05, green: 0.10, blue: 0.25, alpha: 1),
        NSColor(srgbRed: 0.12, green: 0.32, blue: 0.58, alpha: 1),
        NSColor(srgbRed: 0.42, green: 0.68, blue: 0.92, alpha: 1),
    ])?.draw(in: rect, angle: -60)

    // снежинка
    drawSnowflake(in: size, lineWidth: size * 0.035, color: NSColor.white.withAlphaComponent(0.96))

    // пара «снежных» точек
    for (x, y, r) in [(0.24, 0.28, 0.022), (0.76, 0.72, 0.018), (0.34, 0.78, 0.014)] {
        NSColor.white.withAlphaComponent(0.7).setFill()
        NSBezierPath(ovalIn: NSRect(x: size * x, y: size * y,
                                    width: size * r, height: size * r)).fill()
    }

    image.unlockFocus()
    guard let tiff = image.tiffRepresentation,
          let rep = NSBitmapImageRep(data: tiff),
          let png = rep.representation(using: .png, properties: [:]) else {
        throw NSError(domain: "icon", code: 1)
    }
    try png.write(to: URL(fileURLWithPath: path))
}

func renderTemplate(size: CGFloat, to path: String) throws {
    let image = NSImage(size: NSSize(width: size, height: size))
    image.lockFocus()
    drawSnowflake(in: size, lineWidth: max(1.1, size * 0.09), color: NSColor.black)
    image.unlockFocus()
    guard let tiff = image.tiffRepresentation,
          let rep = NSBitmapImageRep(data: tiff),
          let png = rep.representation(using: .png, properties: [:]) else {
        throw NSError(domain: "icon", code: 2)
    }
    try png.write(to: URL(fileURLWithPath: path))
}

let args = CommandLine.arguments
guard args.count >= 3 else {
    FileHandle.standardError.write(Data("usage: icongen <out.png> <size> [tray]\n".utf8))
    exit(1)
}
let out = args[1]
let size = CGFloat(Double(args[2]) ?? 1024)
if args.count > 3 && args[3] == "tray" {
    try renderTemplate(size: size, to: out)
} else {
    try renderAppIcon(size: size, to: out)
}
print("icon: \(out) \(Int(size))px")
