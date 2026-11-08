// SPDX-License-Identifier: GPL-2.0-only
//
// Status: one-glance health from the manifest's `health` table (read-only probes: ioreg, kmutil, system_profiler,
// pmset, nvram). The network name stays hidden unless asked. "Copy diagnostics" copies a sanitized report (no MAC
// addresses, serial numbers, UUIDs, network names) for the developer.
#if canImport(AppKit)
    import AppKit
    import HeliosCore

    final class StatusPage {
        unowned let app: AppDelegate
        let view: NSScrollView
        private let body = UI.stack([], spacing: 12)
        private let grid = UI.stack([], spacing: 12)
        private var showPrivate = false
        private var rows: [HealthRow] = []

        init(app: AppDelegate) {
            self.app = app
            view = UI.page(body)
            let refresh = ActionButton.make("Refresh") { [weak self] in self?.refresh() }
            let copy = ActionButton.make("Copy diagnostics") { [weak self] in self?.copyDiagnostics() }
            let reveal = ActionButton.checkbox("Show network name", on: false) { [weak self] on in
                self?.showPrivate = on
                self?.render()
            }
            body.addArrangedSubview(UI.title("Status"))
            body.addArrangedSubview(UI.secondary("Read from the running system; nothing is changed here."))
            body.addArrangedSubview(UI.stack([refresh, copy, reveal], axis: .horizontal))
            body.addArrangedSubview(UI.separator())
            body.addArrangedSubview(grid)
        }

        func refresh() {
            background(
                { () -> [HealthRow] in
                    let e = self.app.engine
                    return Health.rows(e.loadRelease().manifest, ctx: e.probes())
                },
                then: { rows in
                    self.rows = rows
                    self.render()
                })
        }

        private func render() {
            UI.clear(grid)
            for r in rows {
                let mark = r.ok.map { $0 ? "✓" : "✗" } ?? "•"
                let color: NSColor = r.ok.map { $0 ? .systemGreen : .systemRed } ?? .secondaryLabelColor
                var lines: [NSView] = [
                    UI.stack(
                        [
                            UI.label(mark, size: 15, weight: .bold, color: color, wrap: false),
                            UI.label(r.title, size: 14, weight: .semibold, wrap: false),
                        ], axis: .horizontal)
                ]
                for l in r.lines {
                    let value = l.result.isPrivate && !showPrivate ? "hidden" : l.result.value
                    let c: NSColor = l.result.ok == false ? .systemRed : .labelColor
                    lines.append(
                        UI.stack(
                            [
                                UI.label("\(l.label):", size: 12, color: .secondaryLabelColor, wrap: false),
                                UI.label(value, size: 12, color: c, selectable: true),
                            ], axis: .horizontal, spacing: 6))
                }
                grid.addArrangedSubview(UI.stack(lines, spacing: 3))
            }
        }

        private func copyDiagnostics() {
            let version = app.appVersion
            background(
                { self.app.engine.diagnostics(appVersion: version) },
                then: { text in
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(text, forType: .string)
                    UI.alert(
                        "Diagnostics copied",
                        "Paste them into a message to the developer. MAC addresses, serial numbers, "
                            + "UUIDs, the network name and local addresses are removed.", in: self.app.window)
                })
        }
    }
#endif
