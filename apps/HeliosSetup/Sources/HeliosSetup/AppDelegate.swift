// SPDX-License-Identifier: GPL-2.0-only
//
// The app: one window with four pages (Setup, Status, GPU apps, Updates) and a Help sheet. Everything it shows
// and does goes through HeliosCore's Engine, the same code as the helios-setup CLI.
//   --resume             started by the resume LaunchAgent after "Restart now": the agent is removed, the window
//                        opens on Setup (or Updates when an update is half applied, and "Update all" then asks to go on)
//   --background-check   started daily by the update LaunchAgent: checks, posts a notification, quits.
#if canImport(AppKit)
    import AppKit
    import HeliosCore
    import UserNotifications

    final class AppDelegate: NSObject, NSApplicationDelegate, UNUserNotificationCenterDelegate {
        var window: NSWindow?
        var tabs = NSTabView()
        var setup: SetupPage?
        var status: StatusPage?
        var gpu: GPUAppsPage?
        var updates: UpdatesPage?
        /// One job at a time across every page (the root side holds a lock too; this one says so before the prompt).
        private(set) var busy = false
        /// Created once, on the main thread, before any page uses it from a background queue.
        let engine = Engine(env: .live(gui: true, executable: Bundle.main.executablePath ?? CommandLine.arguments[0]))

        var appVersion: String { Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "dev" }
        var bundleId: String { Bundle.main.bundleIdentifier ?? "org.helios300.setup" }
        var agents: AgentManager {
            AgentManager(fs: LocalFileSystem(), runner: ProcessRunner(), home: NSHomeDirectory(), uid: getuid())
        }

        func applicationDidFinishLaunching(_ note: Notification) {
            UNUserNotificationCenter.current().delegate = self
            if CommandLine.arguments.contains("--background-check") {
                NSApp.setActivationPolicy(.accessory)
                backgroundCheck()
                return
            }
            NSApp.setActivationPolicy(.regular)
            agents.remove(LaunchAgent.resumeLabel)  // it has done its job once the app runs in the new boot
            buildMenu()
            buildWindow()
            if store.loadSystem().pending != nil { tabs.selectTabViewItem(at: 3) }
            NSApp.activate(ignoringOtherApps: true)
            if engine.shouldResumeUpdate(arguments: CommandLine.arguments) { updates?.resumeUpdate() }
        }

        func applicationShouldTerminateAfterLastWindowClosed(_ app: NSApplication) -> Bool { true }

        var store: StateStore { engine.env.store }

        // MARK: window

        func buildWindow() {
            let w = NSWindow(
                contentRect: NSRect(x: 0, y: 0, width: 860, height: 720),
                styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
            w.title = "Helios 300 Setup"
            w.minSize = NSSize(width: 720, height: 560)
            let setup = SetupPage(app: self)
            let status = StatusPage(app: self)
            let gpu = GPUAppsPage(app: self)
            let updates = UpdatesPage(app: self)
            for (title, page) in [
                ("Setup", setup.view), ("Status", status.view), ("GPU apps", gpu.view), ("Updates", updates.view),
            ] {
                let item = NSTabViewItem(identifier: title)
                item.label = title
                item.view = page
                tabs.addTabViewItem(item)
            }
            self.setup = setup
            self.status = status
            self.gpu = gpu
            self.updates = updates
            let help = ActionButton.make("Help…") { [weak self] in self?.showHelp() }
            let root = UI.stack([tabs, UI.stack([NSView(), help], axis: .horizontal)], spacing: 6, alignment: .trailing)
            root.edgeInsets = NSEdgeInsets(top: 8, left: 8, bottom: 10, right: 12)
            tabs.translatesAutoresizingMaskIntoConstraints = false
            tabs.widthAnchor.constraint(equalTo: root.widthAnchor, constant: -20).isActive = true
            w.contentView = root
            w.center()
            w.setFrameAutosaveName("HeliosSetupMain")
            w.makeKeyAndOrderFront(nil)
            window = w
            refreshAll()
        }

        func buildMenu() {
            let main = NSMenu()
            let appItem = NSMenuItem()
            let appMenu = NSMenu()
            appMenu.addItem(
                withTitle: "About Helios 300 Setup", action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)),
                keyEquivalent: "")
            appMenu.addItem(.separator())
            appMenu.addItem(
                withTitle: "Quit Helios 300 Setup", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
            appItem.submenu = appMenu
            main.addItem(appItem)
            let editItem = NSMenuItem()
            let edit = NSMenu(title: "Edit")
            edit.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
            edit.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
            editItem.submenu = edit
            main.addItem(editItem)
            NSApp.mainMenu = main
        }

        func refreshAll() {
            setup?.refresh()
            status?.refresh()
            gpu?.refresh()
            updates?.refresh()
        }

        // MARK: one job at a time

        /// False (and says so) while another job runs.
        func beginWork(in what: String) -> Bool {
            guard !busy else {
                UI.alert("Please wait", "Another job is running; \(what) can start when it has finished.", in: window)
                return false
            }
            busy = true
            return true
        }

        func endWork() { busy = false }

        // MARK: help

        func showHelp() {
            guard let window else { return }
            HelpSheet.show(engine.loadRelease().manifest.appHelp, in: window)
        }

        // MARK: restart

        /// Offers a restart; with yes, installs the resume agent and asks macOS to restart (apps may save first).
        func offerRestart(_ why: String) {
            UI.alert(
                "Restart needed",
                "\(why)\n\nAfter the restart this app opens again by itself and shows the result. "
                    + "If the Mac does not start: Shift+Enter in the OpenCore picker starts safe mode; Help has the way back.",
                buttons: ["Restart now", "Later"], in: window
            ) { [weak self] choice in
                guard choice == 0, let self else { return }
                self.restartNow()
            }
        }

        func restartNow() {
            do {
                try agents.install(.resume(bundleId: bundleId), load: false)
                try Restarter.restart()
            } catch {
                UI.alert(
                    "Could not restart", "\(error). Restart from the Apple menu; the app opens again after login.",
                    in: window)
            }
        }

        // MARK: copy to Applications

        /// The stick's EFI partition is not mounted after a restart: the app must live on the Mac to continue.
        var runsFromApplications: Bool { Bundle.main.bundlePath.hasPrefix("/Applications/") }

        func copyToApplications() {
            let src = Bundle.main.bundlePath
            let dst = "/Applications/Helios 300 Setup.app"
            let rel = engine.loadRelease()
            do {
                try engine.installApp(from: src, to: dst)
                if let dir = rel.dir { engine.setReleaseDir(dir) }
                engine.relaunch(at: dst)
                NSApp.terminate(nil)
            } catch {
                UI.alert("Could not copy the app", "\(error)", in: window)
            }
        }

        // MARK: background update check

        func backgroundCheck() {
            background(
                { () -> UpdateOffer? in
                    let u = self.store.loadUser()
                    if let later = u.remindLaterUntil, later > Date() { return nil }
                    guard let offer = try? self.engine.checkForUpdates() else { return nil }
                    return offer.candidate.tag == u.skippedTag ? nil : offer
                },
                then: { offer in
                    guard let offer else { NSApp.terminate(nil); return }
                    let content = UNMutableNotificationContent()
                    content.title = "macos_helios300 \(offer.candidate.tag) is available"
                    content.body =
                        "\(offer.diff.artifacts.count) file(s) changed. Open Helios 300 Setup to read the changes; "
                        + "nothing is installed until you click Install."
                    content.userInfo = ["tag": offer.candidate.tag]
                    let req = UNNotificationRequest(identifier: "update-\(offer.candidate.tag)", content: content, trigger: nil)
                    UNUserNotificationCenter.current().add(req) { _ in
                        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { NSApp.terminate(nil) }
                    }
                })
        }

        func userNotificationCenter(
            _ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse,
            withCompletionHandler done: @escaping () -> Void
        ) {
            if window == nil {
                NSApp.setActivationPolicy(.regular)
                buildMenu()
                buildWindow()
            }
            tabs.selectTabViewItem(at: 3)
            updates?.checkNow()
            done()
        }

        func requestNotifications(_ done: @escaping (Bool) -> Void) {
            UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { ok, _ in
                DispatchQueue.main.async { done(ok) }
            }
        }
    }

    /// Restart through loginwindow (kAERestart), the documented way an app asks for a restart: apps get to save
    /// and macOS can refuse. No root needed.
    enum Restarter {
        static func restart() throws {
            let target = NSAppleEventDescriptor(bundleIdentifier: "com.apple.loginwindow")
            let event = NSAppleEventDescriptor(
                eventClass: 0x6165_7674 /* 'aevt' */, eventID: 0x7265_7374 /* 'rest' */,
                targetDescriptor: target, returnID: -1, transactionID: 0)
            _ = try event.sendEvent(options: [.noReply], timeout: 10)
        }
    }

    /// The manifest's app_help, as a sheet.
    enum HelpSheet {
        static func show(_ help: AppHelp?, in window: NSWindow) {
            let panel = NSPanel(
                contentRect: NSRect(x: 0, y: 0, width: 640, height: 520), styleMask: [.titled, .resizable],
                backing: .buffered, defer: false)
            let body = UI.stack([], spacing: 10)
            for s in help?.sections ?? [] {
                body.addArrangedSubview(UI.label(s.title, size: 14, weight: .semibold))
                for line in s.lines { body.addArrangedSubview(UI.label("• " + line, selectable: true)) }
            }
            if help == nil { body.addArrangedSubview(UI.label("No help in this release's manifest.")) }
            let close = ActionButton.make("Close") { [weak window, weak panel] in
                if let panel { window?.endSheet(panel) }
            }
            close.keyEquivalent = "\r"
            let page = UI.page(body)
            let root = UI.stack([page, close], spacing: 8, alignment: .trailing)
            root.edgeInsets = NSEdgeInsets(top: 8, left: 8, bottom: 12, right: 12)
            page.translatesAutoresizingMaskIntoConstraints = false
            page.widthAnchor.constraint(equalTo: root.widthAnchor, constant: -20).isActive = true
            page.heightAnchor.constraint(greaterThanOrEqualToConstant: 420).isActive = true
            body.widthAnchor.constraint(equalTo: page.widthAnchor).isActive = true
            panel.contentView = root
            window.beginSheet(panel)
        }
    }
#endif
