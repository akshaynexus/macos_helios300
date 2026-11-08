// SPDX-License-Identifier: GPL-2.0-only
//
// GPU apps: which apps render on the GTX 1060, through the release's gpu-apps.sh (add/remove as root, run as you).
#if canImport(AppKit)
    import AppKit
    import HeliosCore

    final class GPUAppsPage: NSObject, NSTableViewDataSource, NSTableViewDelegate {
        unowned let app: AppDelegate
        let view: NSScrollView
        private let body = UI.stack([], spacing: 12)
        private let table = NSTableView()
        private let note = UI.secondary("")
        private var apps: [(AppEntry, GPUApps.Decision)] = []
        private var withLog = false

        static let explanation = """
            Render offload, like prime-run on Linux: an app ticked here draws on the GTX 1060, and its frames are copied \
            to the Intel HD 630, which drives the screen. 3D-heavy and Metal apps can run much faster; the copy adds a \
            little latency and some power draw. System apps (Finder, Dock, the login window) always stay on the HD 630. \
            A change applies the next time the app starts. The GTX 1060 runs at boot clocks (no reclocking yet).
            """

        init(app: AppDelegate) {
            self.app = app
            view = UI.page(body)
            super.init()
            let col = { (id: String, title: String, w: CGFloat) -> NSTableColumn in
                let c = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(id))
                c.title = title
                c.width = w
                return c
            }
            table.addTableColumn(col("use", "Use GTX 1060", 110))
            table.addTableColumn(col("name", "App", 260))
            table.addTableColumn(col("exe", "Process", 200))
            table.dataSource = self
            table.delegate = self
            table.usesAlternatingRowBackgroundColors = true
            let scroll = NSScrollView()
            scroll.documentView = table
            scroll.hasVerticalScroller = true
            scroll.translatesAutoresizingMaskIntoConstraints = false
            scroll.heightAnchor.constraint(equalToConstant: 340).isActive = true
            let runOnce = ActionButton.make("Run once on GTX 1060") { [weak self] in self?.runSelected() }
            let log = ActionButton.checkbox("with the plugin log", on: false) { [weak self] on in self?.withLog = on }
            let refresh = ActionButton.make("Refresh") { [weak self] in self?.refresh() }
            body.addArrangedSubview(UI.title("Apps on the GTX 1060"))
            body.addArrangedSubview(UI.label(Self.explanation, size: 12))
            body.addArrangedSubview(note)
            body.addArrangedSubview(scroll)
            body.addArrangedSubview(UI.stack([runOnce, log, NSView(), refresh], axis: .horizontal))
            scroll.widthAnchor.constraint(equalTo: body.widthAnchor, constant: -40).isActive = true
        }

        func refresh() {
            background(
                { self.app.engine.gpuApps() },
                then: { r in
                    self.apps = r.apps
                    self.note.stringValue =
                        r.listPresent
                        ? "Ticking a box asks for the administrator password (the list is root-owned)."
                        : "No allow list yet: install the Setup step “Metal plugin, translator and NVK” first."
                    self.table.reloadData()
                })
        }

        func numberOfRows(in tableView: NSTableView) -> Int { apps.count }

        func tableView(_ tableView: NSTableView, viewFor column: NSTableColumn?, row: Int) -> NSView? {
            let (entry, decision) = apps[row]
            switch column?.identifier.rawValue {
            case "use":
                let b = ActionButton.checkbox("", on: decision == .allowed) { [weak self] on in self?.set(entry, on) }
                b.isEnabled = decision != .denied
                b.toolTip = decision == .denied ? "denied by a rule in the allow list (system process)" : nil
                return b
            case "name": return UI.label(entry.name, wrap: false)
            default: return UI.label(entry.executable, size: 12, color: .secondaryLabelColor, wrap: false)
            }
        }

        private func set(_ entry: AppEntry, _ on: Bool) {
            guard app.beginWork(in: "this change") else {
                refresh()
                return
            }
            background(
                { Result { try self.app.engine.setGPU(entry, use: on) } },
                then: { r in
                    self.app.endWork()
                    if case .failure(let e) = r { UI.alert("Not changed", "\(e)", in: self.app.window) }
                    self.refresh()
                })
        }

        private func runSelected() {
            guard table.selectedRow >= 0, table.selectedRow < apps.count else {
                UI.alert("Choose an app", "Select an app in the list first.", in: app.window)
                return
            }
            let entry = apps[table.selectedRow].0
            let log = withLog
            DispatchQueue.global().async {
                _ = try? self.app.engine.runOnGPU(entry, withLog: log, output: "/dev/null")
            }
            if log {
                UI.alert(
                    "Started", "\(entry.name) runs on the GTX 1060 once. Plugin log: /tmp/nvmtl-\(entry.executable).log",
                    in: app.window)
            }
        }
    }
#endif
