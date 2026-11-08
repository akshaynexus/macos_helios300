// SPDX-License-Identifier: GPL-2.0-only
//
// Setup: the checklist. One row per manifest step (or part), in order, with its live status, Check (the dry run,
// always first), the primary action (enabled only after a passing Check in this boot), and Undo. The log below
// shows the running script's output as it comes.
#if canImport(AppKit)
    import AppKit
    import HeliosCore

    final class SetupPage {
        unowned let app: AppDelegate
        let view: NSScrollView
        private let body = UI.stack([], spacing: 12)
        private let rows = UI.stack([], spacing: 14)
        private let banners = UI.stack([], spacing: 6)
        private let releaseLine = UI.secondary("")
        private let (logScroll, logText) = UI.logView(height: 200)
        private var tail: LogTail?
        private var running = false
        private var overview: Overview?

        init(app: AppDelegate) {
            self.app = app
            view = UI.page(body)
            let choose = ActionButton.make("Choose release folder…") { [weak self] in self?.chooseRelease() }
            let refresh = ActionButton.make("Refresh") { [weak self] in self?.refresh() }
            body.addArrangedSubview(UI.title("Set up this Mac, one step at a time"))
            body.addArrangedSubview(
                UI.secondary(
                    "Each step runs its dry run first (Check). Install is enabled once the Check "
                        + "passed in this session. One driver change per restart: the app refuses a second one."))
            body.addArrangedSubview(UI.stack([releaseLine, choose, refresh], axis: .horizontal))
            body.addArrangedSubview(banners)
            body.addArrangedSubview(UI.separator())
            body.addArrangedSubview(rows)
            body.addArrangedSubview(UI.separator())
            let copy = ActionButton.make("Copy log") { [weak self] in
                NSPasteboard.general.clearContents()
                NSPasteboard.general.setString(self?.logText.string ?? "", forType: .string)
            }
            body.addArrangedSubview(UI.stack([UI.label("Log", weight: .semibold, wrap: false), copy], axis: .horizontal))
            body.addArrangedSubview(logScroll)
            for v in [rows, logScroll, banners] {
                v.translatesAutoresizingMaskIntoConstraints = false
                v.widthAnchor.constraint(equalTo: body.widthAnchor, constant: -40).isActive = true
            }
        }

        // MARK: refresh

        func refresh() {
            background(
                { self.app.engine.overview() },
                then: { o in
                    self.overview = o
                    self.render(o)
                })
        }

        private func render(_ o: Overview) {
            let rel = o.release
            releaseLine.stringValue =
                rel.dir.map { "Release \(rel.manifest.release?.tag ?? "?") from \($0) (\(rel.source.rawValue))" }
                ?? "No release folder: the checklist is read-only"
            UI.clear(banners)
            renderBanners(o)
            UI.clear(rows)
            for item in o.items { rows.addArrangedSubview(row(item, o.statuses[item.id] ?? .notInstalled, o)) }
        }

        private func renderBanners(_ o: Overview) {
            if let p = o.release.problem { banners.addArrangedSubview(banner("⚠︎ \(p)", .systemOrange)) }
            for (name, ok) in o.scripts where !ok {
                banners.addArrangedSubview(
                    banner("⚠︎ \(name) in this app is not the release's: use the app from the same release.", .systemRed))
            }
            if let r = o.restartPending {
                let b = ActionButton.make("Restart now") { [weak self] in self?.app.restartNow() }
                banners.addArrangedSubview(UI.stack([banner("⟳ \(r).", .systemOrange), b], axis: .horizontal))
            }
            if o.bootSource == "usb" {
                banners.addArrangedSubview(
                    banner(
                        "This Mac started from the USB stick. After “Install OpenCore to this Mac” "
                            + "it can start without it (Help > Boot order).", .secondaryLabelColor))
            }
            if !app.runsFromApplications {
                let b = ActionButton.make("Copy to Applications") { [weak self] in self?.app.copyToApplications() }
                banners.addArrangedSubview(
                    UI.stack(
                        [
                            banner(
                                "The app runs from \((Bundle.main.bundlePath as NSString).deletingLastPathComponent). "
                                    + "Copy it to Applications so it can continue after a restart.",
                                .secondaryLabelColor), b,
                        ], axis: .horizontal))
            }
            if let n = o.next, o.restartPending == nil {
                banners.addArrangedSubview(banner("Next: \(n.title)", .controlAccentColor))
            }
        }

        private func banner(_ text: String, _ color: NSColor) -> NSTextField {
            UI.label(text, weight: .medium, color: color)
        }

        private func row(_ item: SetupItem, _ status: ItemStatus, _ o: Overview) -> NSView {
            let canAct = o.release.canAct && !running && !(status.isNotAvailable)
            let checked =
                DryRunRule.check(
                    item, user: app.store.loadUser(), boot: o.boot,
                    manifestSha256: o.release.sha256 ?? "", now: Date()) == .allowed
            let check = ActionButton.make("Check") { [weak self] in self?.run(item, "check") }
            let primary = ActionButton.make(primaryTitle(item, status)) { [weak self] in self?.confirmInstall(item) }
            let undo = ActionButton.make("Undo") { [weak self] in self?.confirmUndo(item) }
            check.isEnabled = canAct && item.command("check") != nil
            primary.isEnabled = canAct && checked && status != .installed && item.command("install") != nil
            primary.keyEquivalent = o.next?.id == item.id && primary.isEnabled ? "\r" : ""
            undo.isEnabled =
                o.release.canAct && !running && item.command("revert") != nil
                && (status == .installed || status.isFailed || status.label == "Needs restart")
            primary.toolTip = checked ? nil : "Run Check first: Install is enabled after a passing dry run in this session"
            var buttons: [NSView] = [check, primary, undo]
            if (item.command("install") ?? "").hasPrefix("helios-setup efi") {
                buttons.insert(ActionButton.make("EFI folder…") { [weak self] in self?.chooseEFI() }, at: 0)
            }
            let head = UI.stack(
                [UI.badge(status), UI.label(item.title, size: 14, weight: .semibold, wrap: false), NSView()]
                    + buttons, axis: .horizontal)
            var lines: [NSView] = [head, UI.secondary(item.what)]
            if !status.detail.isEmpty { lines.append(UI.label(status.detail, size: 12, color: UI.color(status))) }
            if !item.needs.isEmpty { lines.append(UI.secondary("Needs: " + item.needs.joined(separator: "; "))) }
            if item.reboot { lines.append(UI.secondary("A restart follows Install and Undo.")) }
            let box = UI.stack(lines, spacing: 4)
            head.translatesAutoresizingMaskIntoConstraints = false
            head.widthAnchor.constraint(equalTo: box.widthAnchor).isActive = true
            return box
        }

        private func primaryTitle(_ item: SetupItem, _ status: ItemStatus) -> String {
            if (item.command("install") ?? "").hasPrefix("sudo ") { return "Enable" }
            return status == .installed ? "Installed" : "Install"
        }

        // MARK: actions

        private func confirmInstall(_ item: SetupItem) {
            let restart = item.reboot ? " A restart is needed afterwards; one driver change per restart." : ""
            UI.alert(
                "Install \(item.title)?", "The dry run passed (its output is in the log).\(restart)",
                buttons: ["Install", "Cancel"], in: app.window
            ) { [weak self] c in
                if c == 0 { self?.run(item, "install") }
            }
        }

        private func confirmUndo(_ item: SetupItem) {
            let note = item.undoNote.map { "\n\n\($0)." } ?? ""
            UI.alert(
                "Undo \(item.title)?", "This runs the step's revert.\(note)", buttons: ["Undo", "Cancel"],
                style: .warning, in: app.window
            ) { [weak self] c in
                if c == 0 { self?.run(item, "revert") }
            }
        }

        func run(_ item: SetupItem, _ action: String) {
            let log = logPath(item.id, action)
            try? LocalFileSystem().write(Data(), to: log)
            let t = LogTail(path: log, view: logText)
            tail = t
            t.start()
            running = true
            refreshButtonsOnly()
            background(
                { () -> Result<Int32, Error> in
                    Result { try self.app.engine.perform(item.id, action, output: log) }
                },
                then: { result in
                    t.stop()
                    self.running = false
                    self.finished(item, action, result)
                    self.app.refreshAll()
                })
        }

        private func finished(_ item: SetupItem, _ action: String, _ result: Result<Int32, Error>) {
            switch result {
            case .failure(let e):
                UI.alert("\(item.title): \(action) did not run", "\(e)", in: app.window)
            case .success(let rc) where rc != 0:
                UI.alert(
                    "\(item.title): \(action) failed (exit \(rc))",
                    "The log shows where it stopped. Nothing after that "
                        + "point was changed by the script.", style: .warning, in: app.window)
            case .success where (action == "install" || action == "revert") && item.reboot:
                app.offerRestart(
                    "\(item.title): \(action == "install" ? "installed" : "undone"). It takes effect after a restart.")
            default:
                break
            }
        }

        private func refreshButtonsOnly() {
            if let o = overview { render(o) }
        }

        private func logPath(_ id: String, _ action: String) -> String {
            let f = DateFormatter()
            f.dateFormat = "yyyyMMdd-HHmmss"
            return (StatePaths.logs(home: NSHomeDirectory()) as NSString)
                .appendingPathComponent("\(id)-\(action)-\(f.string(from: Date())).log")
        }

        private func chooseRelease() {
            let p = NSOpenPanel()
            p.canChooseDirectories = true
            p.canChooseFiles = false
            p.message = "The folder with manifest.json and the release files (EFI/Helios300 on the stick)"
            if p.runModal() == .OK, let url = p.url {
                app.engine.setReleaseDir(url.path)
                app.refreshAll()
            }
        }

        private func chooseEFI() {
            let p = NSOpenPanel()
            p.canChooseDirectories = true
            p.canChooseFiles = false
            p.message = "The OpenCore EFI folder to install (holds BOOT and OC), e.g. the stick's EFI"
            if p.runModal() == .OK, let url = p.url {
                app.engine.setEFISource(url.path)
                refresh()
            }
        }
    }

    extension ItemStatus {
        var isNotAvailable: Bool { if case .notAvailable = self { return true } else { return false } }
        var isFailed: Bool { if case .failed = self { return true } else { return false } }
    }
#endif
