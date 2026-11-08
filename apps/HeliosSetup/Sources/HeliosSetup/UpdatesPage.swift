// SPDX-License-Identifier: GPL-2.0-only
//
// Updates, Sparkle-style: check, read the release notes and the per-file changes, then Install (the next batch),
// Update all (batch after batch, a restart between driver changes, the app reopening after each), Remind me later
// or Skip this version. Nothing installs without a click. "Roll back last update" undoes one applied unit.
#if canImport(AppKit)
    import AppKit
    import HeliosCore

    final class UpdatesPage {
        unowned let app: AppDelegate
        let view: NSScrollView
        private let body = UI.stack([], spacing: 12)
        private let installedLine = UI.label("")
        private let stateLine = UI.secondary("")
        private let (notesScroll, notes) = UI.logView(height: 220)
        private let planLine = UI.label("", size: 12)
        private let actions = UI.stack([], axis: .horizontal)
        private let thirdParty = UI.stack([], spacing: 6)
        private let (logScroll, logText) = UI.logView(height: 140)
        private var offer: UpdateOffer?
        private var staged: StagedRelease?

        init(app: AppDelegate) {
            self.app = app
            view = UI.page(body)
            let u = app.store.loadUser()
            let pre = ActionButton.checkbox("Include pre-releases", on: u.includePrereleases ?? true) { [weak self] on in
                self?.editUser { $0.includePrereleases = on }
            }
            let daily = ActionButton.checkbox("Check once a day (a notification only, never an install)", on: u.dailyUpdateCheck)
            { [weak self] on in
                self?.setDaily(on)
            }
            let check = ActionButton.make("Check now") { [weak self] in self?.checkNow() }
            let rollback = ActionButton.make("Roll back last update") { [weak self] in self?.confirmRollback() }
            let tp = ActionButton.checkbox(
                "Also offer third-party kexts from Dortania's build repo (Lilu, WhateverGreen, AppleALC, "
                    + "VirtualSMC, VoodooPS2, ...)", on: u.thirdPartyUpdates
            ) { [weak self] on in
                self?.editUser { $0.thirdPartyUpdates = on }
                self?.renderThirdParty([])
            }
            body.addArrangedSubview(UI.title("Updates"))
            body.addArrangedSubview(installedLine)
            body.addArrangedSubview(UI.stack([check, pre, daily], axis: .horizontal))
            body.addArrangedSubview(stateLine)
            body.addArrangedSubview(notesScroll)
            body.addArrangedSubview(planLine)
            body.addArrangedSubview(actions)
            body.addArrangedSubview(UI.separator())
            body.addArrangedSubview(
                UI.stack(
                    [
                        rollback,
                        UI.secondary(
                            "Restores the .prev kexts and the config.plist backup of the "
                                + "last applied update. If macOS does not start: Shift+Enter in "
                                + "OpenCore (safe mode), or F12 and the stick; Help has the details."),
                    ],
                    axis: .horizontal))
            body.addArrangedSubview(UI.separator())
            body.addArrangedSubview(tp)
            body.addArrangedSubview(thirdParty)
            body.addArrangedSubview(UI.label("Log", weight: .semibold, wrap: false))
            body.addArrangedSubview(logScroll)
            for v in [notesScroll, logScroll] {
                v.translatesAutoresizingMaskIntoConstraints = false
                v.widthAnchor.constraint(equalTo: body.widthAnchor, constant: -40).isActive = true
            }
        }

        private func editUser(_ change: (inout UserState) -> Void) {
            var u = app.store.loadUser()
            change(&u)
            try? app.store.saveUser(u)
        }

        func refresh() {
            let s = app.store.loadSystem()
            if let i = s.installed {
                installedLine.stringValue =
                    "Installed: macos_helios300 \(i.tag)\(i.preRelease ? " (pre-release)" : ""), manifest \(i.manifestSha256.prefix(16))…"
            } else {
                installedLine.stringValue = "Installed: not recorded yet (the first step run from a release records it)"
            }
            if let p = s.pending {
                stateLine.stringValue =
                    "Update \(p.tag) is half applied (\(p.applied.count) unit(s) done): Check now, then continue."
            }
            renderActions()
        }

        // MARK: check / download / apply

        func checkNow() {
            stateLine.stringValue = "Checking GitHub releases…"
            let pre = app.store.loadUser().includePrereleases
            background(
                { Result { try self.app.engine.checkForUpdates(includePrereleases: pre) } },
                then: { r in
                    switch r {
                    case .success(let o?):
                        self.offer = o
                        self.notes.string = o.changelog
                        self.stateLine.stringValue =
                            "\(o.candidate.tag) is available\(o.candidate.preRelease ? " (pre-release)" : "")."
                        self.planLine.stringValue =
                            "Plan: "
                            + o.plan.batches.enumerated().map { n, b in
                                "\(n + 1). \(b.map(\.title).joined(separator: ", "))\(b.contains(where: \.requiresRestart) ? " (restart)" : "")"
                            }.joined(separator: "   ")
                    case .success(nil):
                        self.offer = nil
                        self.notes.string = ""
                        self.stateLine.stringValue = "Up to date."
                        self.planLine.stringValue = ""
                    case .failure(let e):
                        self.stateLine.stringValue = "Could not check: \(e)"
                    }
                    self.renderActions()
                    if self.app.store.loadUser().thirdPartyUpdates { self.checkThirdParty() }
                })
        }

        private func renderActions() {
            UI.clear(actions)
            guard let o = offer else { return }
            actions.addArrangedSubview(ActionButton.make("Install next step") { [weak self] in self?.apply(all: false) })
            actions.addArrangedSubview(
                ActionButton.make("Update all (one driver change per restart)") { [weak self] in self?.apply(all: true) })
            actions.addArrangedSubview(
                ActionButton.make("Remind me later") { [weak self] in
                    self?.editUser { $0.remindLaterUntil = Date().addingTimeInterval(86_400) }
                    self?.stateLine.stringValue = "Reminder in a day."
                })
            actions.addArrangedSubview(
                ActionButton.make("Skip this version") { [weak self] in
                    self?.editUser { $0.skippedTag = o.candidate.tag }
                    self?.offer = nil
                    self?.renderActions()
                })
        }

        private func apply(all: Bool) {
            guard let o = offer else { return }
            let log = logFile("update")
            let t = LogTail(path: log, view: logText)
            t.start()
            stateLine.stringValue = "Downloading and verifying every file of \(o.candidate.tag)…"
            background(
                { () -> Result<Int32, Error> in
                    Result {
                        let s = try self.staged.flatMap { $0.manifest == o.manifest ? $0 : nil } ?? self.app.engine.download(o)
                        self.staged = s
                        return try self.app.engine.applyNextUpdate(s, output: log)
                    }
                },
                then: { r in
                    t.stop()
                    switch r {
                    case .success(0):
                        self.editUser { $0.resumeAfterLogin = all }
                        let pending = self.app.store.loadSystem().pending
                        if pending == nil {
                            self.stateLine.stringValue = "\(o.candidate.tag) is installed."
                        }
                        if o.plan.units.contains(where: \.requiresRestart) {
                            self.app.offerRestart(
                                "Part of \(o.candidate.tag) is applied; a restart loads it.\(pending != nil && all ? " After the restart the app offers the next part." : "")"
                            )
                        }
                    case .success(let rc):
                        self.stateLine.stringValue =
                            "The update stopped (exit \(rc)): see the log. What was applied can be rolled back."
                    case .failure(let e):
                        self.stateLine.stringValue = "\(e)"
                    }
                    self.app.refreshAll()
                })
        }

        private func confirmRollback() {
            guard let last = app.store.loadSystem().rollbacks.last else {
                UI.alert("Nothing to roll back", "No update has been applied by this app yet.", in: app.window)
                return
            }
            UI.alert(
                "Roll back \(last.unit)?", "Puts back what \(last.toTag) replaced (applied \(last.date)). A restart follows.",
                buttons: ["Roll back", "Cancel"], style: .warning, in: app.window
            ) { [weak self] c in
                guard c == 0, let self else { return }
                let log = self.logFile("rollback")
                let t = LogTail(path: log, view: self.logText)
                t.start()
                background(
                    { Result { try self.app.engine.rollbackLast(output: log) } },
                    then: { r in
                        t.stop()
                        if case .success(0) = r { self.app.offerRestart("Rolled back \(last.unit).") }
                        self.app.refreshAll()
                    })
            }
        }

        private func setDaily(_ on: Bool) {
            editUser { $0.dailyUpdateCheck = on }
            if on {
                app.requestNotifications { _ in }
                let exe = Bundle.main.executablePath ?? ""
                try? app.agents.install(.updateCheck(appExecutable: exe), load: true)
            } else {
                app.agents.remove(LaunchAgent.updateLabel)
            }
        }

        // MARK: third-party kexts (optional)

        private func checkThirdParty() {
            let m = app.engine.loadRelease().manifest
            background(
                { Result { try self.app.engine.thirdPartyUpdates(m) } },
                then: { r in
                    if case .success(let ups) = r { self.renderThirdParty(ups) }
                })
        }

        private func renderThirdParty(_ ups: [ThirdPartyUpdate]) {
            UI.clear(thirdParty)
            guard app.store.loadUser().thirdPartyUpdates else { return }
            if ups.isEmpty { thirdParty.addArrangedSubview(UI.secondary("Third-party kexts: current (Check now looks again).")) }
            for u in ups {
                let b = ActionButton.make("Update \(u.product)") { [weak self] in self?.applyThirdParty(u) }
                thirdParty.addArrangedSubview(
                    UI.stack(
                        [UI.label("\(u.product): \(u.installed ?? "?") → \(u.available)", wrap: false), b],
                        axis: .horizontal))
            }
        }

        private func applyThirdParty(_ u: ThirdPartyUpdate) {
            let log = logFile("third-party")
            let t = LogTail(path: log, view: logText)
            t.start()
            background(
                { Result { try self.app.engine.applyThirdParty(u, output: log) } },
                then: { r in
                    t.stop()
                    if case .success(0) = r { self.app.offerRestart("\(u.product) \(u.available) is on the EFI partition.") }
                    self.app.refreshAll()
                })
        }

        private func logFile(_ what: String) -> String {
            let p = (StatePaths.logs(home: NSHomeDirectory()) as NSString).appendingPathComponent(
                "\(what)-\(Int(Date().timeIntervalSince1970)).log")
            try? LocalFileSystem().write(Data(), to: p)
            return p
        }
    }
#endif
