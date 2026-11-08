// SPDX-License-Identifier: GPL-2.0-only
//
// Small AppKit helpers: labels, buttons with closures, stacks, the scrolling page container and the log view.
// Plain programmatic AppKit (no storyboards, no nibs), so the whole UI cross-builds from source on Linux.
#if canImport(AppKit)
    import AppKit
    import HeliosCore

    /// A button that runs a closure.
    final class ActionButton: NSButton {
        var handler: (() -> Void)?

        static func make(_ title: String, style: NSButton.BezelStyle = .rounded, _ handler: @escaping () -> Void) -> ActionButton
        {
            let b = ActionButton(title: title, target: nil, action: nil)
            b.bezelStyle = style
            b.handler = handler
            b.target = b
            b.action = #selector(fire)
            return b
        }

        static func checkbox(_ title: String, on: Bool, _ handler: @escaping (Bool) -> Void) -> ActionButton {
            let b = ActionButton(checkboxWithTitle: title, target: nil, action: nil)
            b.state = on ? .on : .off
            b.handler = { [weak b] in handler(b?.state == .on) }
            b.target = b
            b.action = #selector(fire)
            return b
        }

        @objc func fire() { handler?() }
    }

    enum UI {
        static func label(
            _ text: String, size: CGFloat = 13, weight: NSFont.Weight = .regular,
            color: NSColor = .labelColor, wrap: Bool = true, selectable: Bool = false
        ) -> NSTextField {
            let l = wrap ? NSTextField(wrappingLabelWithString: text) : NSTextField(labelWithString: text)
            l.font = .systemFont(ofSize: size, weight: weight)
            l.textColor = color
            l.isSelectable = selectable
            l.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
            return l
        }

        static func title(_ text: String) -> NSTextField { label(text, size: 17, weight: .semibold, wrap: false) }

        static func secondary(_ text: String) -> NSTextField { label(text, size: 12, color: .secondaryLabelColor) }

        static func stack(
            _ views: [NSView], axis: NSUserInterfaceLayoutOrientation = .vertical, spacing: CGFloat = 8,
            alignment: NSLayoutConstraint.Attribute? = nil
        ) -> NSStackView {
            let s = NSStackView(views: views)
            s.orientation = axis
            s.spacing = spacing
            s.alignment = alignment ?? (axis == .vertical ? .leading : .centerY)
            return s
        }

        /// A vertical page that scrolls; `content` gets the page's full width.
        static func page(_ content: NSStackView) -> NSScrollView {
            let scroll = NSScrollView()
            scroll.hasVerticalScroller = true
            scroll.drawsBackground = false
            let clip = FlippedClipView()
            clip.drawsBackground = false
            scroll.contentView = clip
            content.translatesAutoresizingMaskIntoConstraints = false
            content.edgeInsets = NSEdgeInsets(top: 16, left: 20, bottom: 16, right: 20)
            scroll.documentView = content
            NSLayoutConstraint.activate([
                content.leadingAnchor.constraint(equalTo: clip.leadingAnchor),
                content.trailingAnchor.constraint(equalTo: clip.trailingAnchor),
                content.topAnchor.constraint(equalTo: clip.topAnchor),
            ])
            return scroll
        }

        /// A monospaced, read-only text view in a scroll view.
        static func logView(height: CGFloat = 180) -> (NSScrollView, NSTextView) {
            let scroll = NSTextView.scrollableTextView()
            guard let text = scroll.documentView as? NSTextView else { return (scroll, NSTextView()) }
            text.isEditable = false
            text.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
            text.textContainerInset = NSSize(width: 6, height: 6)
            scroll.heightAnchor.constraint(equalToConstant: height).isActive = true
            return (scroll, text)
        }

        static func separator() -> NSBox {
            let b = NSBox()
            b.boxType = .separator
            return b
        }

        static func badge(_ status: ItemStatus) -> NSTextField {
            let l = label(" \(status.label) ", size: 11, weight: .semibold, color: .white, wrap: false)
            l.drawsBackground = true
            l.backgroundColor = color(status)
            l.wantsLayer = true
            l.layer?.cornerRadius = 4
            return l
        }

        static func color(_ s: ItemStatus) -> NSColor {
            switch s {
            case .installed: return .systemGreen
            case .notInstalled: return .systemGray
            case .needsRestart: return .systemOrange
            case .failed: return .systemRed
            case .notAvailable: return .tertiaryLabelColor
            }
        }

        static func alert(
            _ title: String, _ text: String, buttons: [String] = ["OK"], style: NSAlert.Style = .informational,
            in window: NSWindow?, _ done: ((Int) -> Void)? = nil
        ) {
            let a = NSAlert()
            a.messageText = title
            a.informativeText = text
            a.alertStyle = style
            for b in buttons { a.addButton(withTitle: b) }
            if let window {
                a.beginSheetModal(for: window) { r in
                    done?(r.rawValue - NSApplication.ModalResponse.alertFirstButtonReturn.rawValue)
                }
            } else {
                done?(a.runModal().rawValue - NSApplication.ModalResponse.alertFirstButtonReturn.rawValue)
            }
        }

        static func clear(_ stack: NSStackView) {
            for v in stack.arrangedSubviews {
                stack.removeArrangedSubview(v)
                v.removeFromSuperview()
            }
        }
    }

    final class FlippedClipView: NSClipView {
        override var isFlipped: Bool { true }
    }

    /// Follows a log file the root job appends to, and shows it live.
    final class LogTail {
        private var timer: Timer?
        private var offset: UInt64 = 0
        let path: String
        weak var view: NSTextView?

        init(path: String, view: NSTextView) {
            self.path = path
            self.view = view
            view.string = ""
        }

        func start() {
            timer = Timer.scheduledTimer(withTimeInterval: 0.3, repeats: true) { [weak self] _ in self?.poll() }
        }

        func stop() {
            poll()
            timer?.invalidate()
            timer = nil
        }

        func poll() {
            guard let h = FileHandle(forReadingAtPath: path) else { return }
            defer { h.closeFile() }
            h.seek(toFileOffset: offset)
            let data = h.readDataToEndOfFile()
            guard !data.isEmpty, let view else { return }
            offset += UInt64(data.count)
            view.textStorage?.append(
                NSAttributedString(
                    string: String(decoding: data, as: UTF8.self),
                    attributes: [
                        .font: NSFont.monospacedSystemFont(ofSize: 11, weight: .regular),
                        .foregroundColor: NSColor.textColor,
                    ]))
            view.scrollToEndOfDocument(nil)
        }
    }

    /// Runs work off the main thread and comes back to it.
    func background<T>(_ work: @escaping () -> T, then done: @escaping (T) -> Void) {
        DispatchQueue.global(qos: .userInitiated).async {
            let r = work()
            DispatchQueue.main.async { done(r) }
        }
    }
#endif
