import AppKit
import Foundation

guard CommandLine.arguments.count == 2 else {
    fputs("usage: generate-app-icon.swift ICONSET_DIRECTORY\n", stderr)
    exit(2)
}

let iconsetURL = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
try FileManager.default.createDirectory(at: iconsetURL, withIntermediateDirectories: true)

func roundRect(_ rect: NSRect, radius: CGFloat) -> NSBezierPath {
    NSBezierPath(roundedRect: rect, xRadius: radius, yRadius: radius)
}

func strokeLine(_ from: NSPoint, _ to: NSPoint, color: NSColor, width: CGFloat) {
    let path = NSBezierPath()
    path.lineWidth = width
    path.lineCapStyle = .round
    path.move(to: from)
    path.line(to: to)
    color.setStroke()
    path.stroke()
}

func drawArc(radius: CGFloat, center: NSPoint, color: NSColor) {
    let path = NSBezierPath()
    path.lineWidth = 28
    path.lineCapStyle = .round
    path.appendArc(withCenter: center, radius: radius, startAngle: 45, endAngle: 135, clockwise: false)
    color.setStroke()
    path.stroke()
}

func renderIcon(pixelSize: Int) -> Data {
    let bitmap = NSBitmapImageRep(
        bitmapDataPlanes: nil,
        pixelsWide: pixelSize,
        pixelsHigh: pixelSize,
        bitsPerSample: 8,
        samplesPerPixel: 4,
        hasAlpha: true,
        isPlanar: false,
        colorSpaceName: .deviceRGB,
        bytesPerRow: 0,
        bitsPerPixel: 0
    )!
    let graphics = NSGraphicsContext(bitmapImageRep: bitmap)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = graphics
    graphics.imageInterpolation = .high
    graphics.cgContext.scaleBy(x: CGFloat(pixelSize) / 1024, y: CGFloat(pixelSize) / 1024)

    NSColor.clear.setFill()
    NSRect(x: 0, y: 0, width: 1024, height: 1024).fill()

    let tile = roundRect(NSRect(x: 24, y: 24, width: 976, height: 976), radius: 218)
    NSGradient(colors: [
        NSColor(calibratedRed: 0.08, green: 0.19, blue: 0.42, alpha: 1),
        NSColor(calibratedRed: 0.025, green: 0.075, blue: 0.20, alpha: 1)
    ])!.draw(in: tile, angle: -45)

    let rim = roundRect(NSRect(x: 26, y: 26, width: 972, height: 972), radius: 216)
    NSColor(calibratedRed: 0.42, green: 0.70, blue: 0.98, alpha: 0.26).setStroke()
    rim.lineWidth = 5
    rim.stroke()

    let glow = NSBezierPath(ovalIn: NSRect(x: 275, y: 255, width: 474, height: 474))
    NSColor(calibratedRed: 0.06, green: 0.62, blue: 0.91, alpha: 0.13).setFill()
    glow.fill()

    let board = roundRect(NSRect(x: 235, y: 225, width: 554, height: 574), radius: 94)
    let shadow = NSShadow()
    shadow.shadowColor = NSColor(calibratedRed: 0, green: 0.015, blue: 0.07, alpha: 0.65)
    shadow.shadowBlurRadius = 34
    shadow.shadowOffset = NSSize(width: 0, height: -16)
    shadow.set()
    NSGradient(colors: [
        NSColor(calibratedRed: 0.10, green: 0.23, blue: 0.40, alpha: 1),
        NSColor(calibratedRed: 0.045, green: 0.105, blue: 0.23, alpha: 1)
    ])!.draw(in: board, angle: 90)
    NSShadow().set()
    NSColor(calibratedRed: 0.36, green: 0.76, blue: 0.98, alpha: 0.82).setStroke()
    board.lineWidth = 12
    board.stroke()

    let pad = NSColor(calibratedRed: 0.40, green: 0.81, blue: 0.98, alpha: 0.95)
    let warmPad = NSColor(calibratedRed: 0.99, green: 0.69, blue: 0.35, alpha: 0.95)
    let positions: [CGFloat] = [330, 452, 574, 696]
    for (index, position) in positions.enumerated() {
        let color = index == 1 ? warmPad : pad
        strokeLine(NSPoint(x: position, y: 800), NSPoint(x: position, y: 858), color: color, width: 24)
        strokeLine(NSPoint(x: position, y: 166), NSPoint(x: position, y: 225), color: color, width: 24)
        strokeLine(NSPoint(x: 176, y: position + 98), NSPoint(x: 235, y: position + 98), color: color, width: 24)
        strokeLine(NSPoint(x: 789, y: position + 98), NSPoint(x: 848, y: position + 98), color: color, width: 24)
    }

    let signal = NSColor(calibratedRed: 0.34, green: 0.88, blue: 1.0, alpha: 1)
    let center = NSPoint(x: 512, y: 380)
    drawArc(radius: 115, center: center, color: signal)
    drawArc(radius: 195, center: center, color: signal)
    drawArc(radius: 275, center: center, color: signal)
    let dot = NSBezierPath(ovalIn: NSRect(x: 478, y: 346, width: 68, height: 68))
    signal.setFill()
    dot.fill()

    let lowerAccent = NSBezierPath(roundedRect: NSRect(x: 426, y: 274, width: 172, height: 13), xRadius: 6, yRadius: 6)
    NSColor(calibratedRed: 1, green: 0.70, blue: 0.36, alpha: 0.95).setFill()
    lowerAccent.fill()

    NSGraphicsContext.restoreGraphicsState()
    return bitmap.representation(using: .png, properties: [:])!
}

let files: [(String, Int)] = [
    ("icon_16x16.png", 16),
    ("icon_16x16@2x.png", 32),
    ("icon_32x32.png", 32),
    ("icon_32x32@2x.png", 64),
    ("icon_128x128.png", 128),
    ("icon_128x128@2x.png", 256),
    ("icon_256x256.png", 256),
    ("icon_256x256@2x.png", 512),
    ("icon_512x512.png", 512),
    ("icon_512x512@2x.png", 1024)
]

for (name, size) in files {
    try renderIcon(pixelSize: size).write(to: iconsetURL.appendingPathComponent(name), options: .atomic)
}
