// SPDX-License-Identifier: GPL-2.0-only
import XCTest

@testable import HeliosCore

final class SanitizerTests: XCTestCase {
    func testMACsSerialsUUIDsAndNames() {
        let text = """
            Address: 9C:B6:D0:12:34:56
            "IOMACAddress" = <9cb6d0123456>
            "Ath10kRxSecurityExpectedBSSID" = <000000000000>
            broadcast ff:ff:ff:ff:ff:ff zero 00:00:00:00:00:00
            Serial Number (system): C02WT0NUHTD5
            "IOPlatformSerialNumber" = "C02WT0NUHTD5"
            Hardware UUID: C1697F77-8D41-4B54-922C-850207706222
            inet6 2403:a080:c04:2729:1483:5a13:4369:6cb2 and fe80::1c2b:3d4e:5f60:7182
            at 12:07:31.874
            inet 192.168.1.45 on alexs-MacBook-Pro at /Users/alex/Desktop
            joined ssid="Home Net 5G" as Home Net 5G
            """
        let out = Sanitizer.clean(text, secrets: ["Home Net 5G"])
        for leak in [
            "9C:B6:D0:12:34:56", "9cb6d0123456", "C02WT0NUHTD5", "C1697F77", "2403:a080", "fe80::1c2b", "192.168.1.45",
            "alexs-MacBook-Pro",
            "/Users/alex", "Home Net",
        ] {
            XCTAssertFalse(out.contains(leak), leak)
        }
        XCTAssertTrue(out.contains("ff:ff:ff:ff:ff:ff") && out.contains("00:00:00:00:00:00"), "not addresses: kept\n\(out)")
        XCTAssertTrue(out.contains("<000000000000>"))
        XCTAssertTrue(out.contains("12:07:31.874"), "a time is not an address")
    }

    func testDiagnosticsHidePrivateValues() throws {
        let (r, _, ctx) = fakeMac(self)
        r.set("/usr/sbin/networksetup -listallhardwareports", "Hardware Port: Wi-Fi\nDevice: en1\n")
        r.set("/usr/sbin/ipconfig getsummary en1", "  SSID : Secret Cafe\n")
        r.set("/usr/sbin/ipconfig getifaddr en1", "192.168.1.25\n")
        let m = try realSpec()
        let rows = Health.rows(m, ctx: ctx)
        let wifi = try XCTUnwrap(rows.first { $0.id == "wifi" })
        XCTAssertEqual(wifi.lines.first { $0.label == "network" }?.result.isPrivate, true)
        let text = Health.diagnostics(m, rows: rows, statuses: [], ctx: ctx, appVersion: "test")
        XCTAssertFalse(text.contains("Secret Cafe"))
        XCTAssertFalse(text.contains("192.168.1.25"))
        XCTAssertTrue(text.contains("org.helios300.pg106 (0.1.0)"))
    }

    func testHealthRowsFromTheSpec() throws {
        let (_, _, ctx) = fakeMac(self, pg106Ready: true)
        let rows = Health.rows(try realSpec(), ctx: ctx)
        XCTAssertEqual(rows.first { $0.id == "gpu" }?.ok, true)
        XCTAssertEqual(rows.first { $0.id == "wifi" }?.ok, false, "AirportAth10k not loaded in the fixture")
        let audio = try XCTUnwrap(rows.first { $0.id == "audio" })
        XCTAssertNil(audio.ok, "audio rows only report (format info)")
    }
}

final class InstallerTests: XCTestCase {
    func testCommandParser() throws {
        XCTAssertEqual(
            try CommandParser.parse("MODE=install ONLY=\"plugin xlate\" bash install-gpu.sh"),
            .script(name: "install-gpu.sh", args: [], env: ["MODE": "install", "ONLY": "plugin xlate"]))
        XCTAssertEqual(
            try CommandParser.parse("sudo pmset -a disablesleep 0"),
            .exec(argv: ["pmset", "-a", "disablesleep", "0"], root: true, env: [:]))
        XCTAssertEqual(try CommandParser.parse("helios-setup efi install"), .cli(args: ["efi", "install"]))
        XCTAssertEqual(try CommandParser.parse("pmset -g"), .exec(argv: ["pmset", "-g"], root: false, env: [:]))
        XCTAssertThrowsError(try CommandParser.parse("bash 'unterminated"))
        XCTAssertThrowsError(try CommandParser.parse("MODE=x"))
    }

    func testPlanRunsTheVerifiedScriptWithACleanEnvironmentAndStdinIsNeverThePipe() throws {
        let t = tempDir(self)
        let script = "#!/bin/bash\necho hi\n"
        write(script, "\(t)/res/install-gpu.sh")
        var m = try manifestFixture("manifest-next.json")
        let i = try XCTUnwrap(m.artifacts.firstIndex { $0.name == "install-gpu.sh" })
        m.artifacts[i].sha256 = Hashing.sha256(Data(script.utf8))
        let item = try XCTUnwrap(StepPlanner.items(m).first { $0.id == "gpu-fb" })
        let inst = Installer(runner: FakeRunner(), fs: LocalFileSystem(), scriptsDir: "\(t)/res")
        let plan = try XCTUnwrap(
            try inst.plan(
                item, action: "install", manifest: m, releaseDir: "\(t)/rel",
                user: UserContext(name: "alex", home: "/Users/alex")))
        XCTAssertEqual(plan.argv, ["/bin/bash", "\(t)/res/install-gpu.sh"])
        XCTAssertEqual(plan.env["MODE"], "install")
        XCTAssertEqual(plan.env["ONLY"], "PG106FB")
        XCTAssertEqual(plan.env["HOME"], "/Users/alex")
        XCTAssertNil(plan.env["SUDO_ASKPASS"])
        XCTAssertEqual(plan.cwd, "\(t)/rel")
        write(script + "# changed\n", "\(t)/res/install-gpu.sh")
        XCTAssertThrowsError(
            try inst.plan(
                item, action: "install", manifest: m, releaseDir: "\(t)/rel",
                user: UserContext(name: "alex", home: "/Users/alex"))
        ) { e in
            XCTAssertEqual(e as? InstallerError, .scriptMismatch("install-gpu.sh", "0.1.1"))
        }
    }

    func testNeedsRoot() throws {
        let items = StepPlanner.items(try realSpec())
        let fb = try XCTUnwrap(items.first { $0.id == "gpu-fb" })
        XCTAssertTrue(Installer.needsRoot(fb, action: "check"))
        XCTAssertFalse(Installer.needsRoot(fb, action: "status"))
        let sleep = try XCTUnwrap(items.first { $0.id == "sleep" })
        XCTAssertTrue(Installer.needsRoot(sleep, action: "install"))
        XCTAssertFalse(Installer.needsRoot(sleep, action: "check"))
        let oc = try XCTUnwrap(items.first { $0.id == "opencore" })
        XCTAssertTrue(Installer.needsRoot(oc, action: "check"), "mounting the ESP needs root")
    }

    func testOsascriptGetsArgumentsNotShellText() {
        let args = OsascriptRunner.osascriptArguments(
            ["/App/helios-setup", "_root", "it's; rm -rf /"], prompt: "p", log: "/tmp/l", rcFile: "/tmp/r")
        XCTAssertEqual(args.first, "/usr/bin/osascript")
        XCTAssertEqual(Array(args.suffix(6)), ["p", "/tmp/l", "/tmp/r", "/App/helios-setup", "_root", "it's; rm -rf /"])
        XCTAssertTrue(args.contains { $0.contains("quoted form of") })
        XCTAssertTrue(args.contains { $0.contains("</dev/null") })
    }

    func testOsascriptRunnerReadsTheExitStatusOrReportsCancel() {
        let t = tempDir(self)
        let r = FakeRunner()
        r.fallback = { argv in
            if let rc = argv.firstIndex(of: "/tmp/never") { _ = rc }
            return CommandResult(status: 1, stderr: "User canceled. (-128)")
        }
        XCTAssertEqual(
            OsascriptRunner(runner: r, fs: LocalFileSystem(), tempDir: t).runAsRoot(["/bin/true"], prompt: "p", output: nil),
            privilegedCancelled)
        r.fallback = { argv in
            let rcFile = argv[argv.count - 2]
            write("3\n", rcFile)
            return CommandResult(status: 0)
        }
        XCTAssertEqual(
            OsascriptRunner(runner: r, fs: LocalFileSystem(), tempDir: t).runAsRoot(["/bin/false"], prompt: "p", output: nil), 3)
    }
}

final class StateAndAgentTests: XCTestCase {
    func testStateRoundTrip() throws {
        let t = tempDir(self)
        let store = StateStore(fs: LocalFileSystem(), home: "\(t)/home", systemPath: "\(t)/system/state.plist")
        var s = store.loadSystem()
        s.record(
            ChangeRecord(
                item: "gpu-fb", action: "install", bootSession: "A", date: Date(timeIntervalSince1970: 5), driverChange: true,
                needsRestart: true))
        s.rollbacks.append(
            RollbackRecord(
                unit: "x", fromTag: "v0.1.0", toTag: "v0.1.1", date: Date(timeIntervalSince1970: 6),
                ops: [.remove("EFI/OC/Kexts/X.kext", onESP: true)]))
        try store.saveSystem(s)
        XCTAssertEqual(store.loadSystem(), s)
        var u = store.loadUser()
        u.releaseDir = "/Volumes/EFI/EFI/Helios300"
        try store.saveUser(u)
        XCTAssertEqual(store.loadUser().releaseDir, "/Volumes/EFI/EFI/Helios300")
        XCTAssertTrue(store.userPath.hasSuffix("Library/Application Support/Helios 300 Setup/state.plist"))
    }

    func testChangeLogIsBounded() {
        var s = SystemState()
        for i in 0..<250 {
            s.record(
                ChangeRecord(
                    item: "\(i)", action: "install", bootSession: "A", date: Date(), driverChange: false, needsRestart: false))
        }
        XCTAssertEqual(s.changes.count, 200)
        XCTAssertEqual(s.changes.first?.item, "50")
    }

    func testLaunchAgents() throws {
        let resume =
            try PropertyListSerialization.propertyList(
                from: LaunchAgent.resume(bundleId: "org.helios300.setup").plist(), format: nil) as? [String: Any]
        XCTAssertEqual(
            resume?["ProgramArguments"] as? [String], ["/usr/bin/open", "-b", "org.helios300.setup", "--args", "--resume"])
        XCTAssertEqual(resume?["RunAtLoad"] as? Bool, true)
        let daily =
            try PropertyListSerialization.propertyList(
                from: LaunchAgent.updateCheck(appExecutable: "/A/HeliosSetup").plist(), format: nil) as? [String: Any]
        XCTAssertEqual(daily?["StartInterval"] as? Int, 86_400)
        XCTAssertEqual(daily?["ProgramArguments"] as? [String], ["/A/HeliosSetup", "--background-check"])
        let t = tempDir(self)
        let r = FakeRunner()
        let agents = AgentManager(fs: LocalFileSystem(), runner: r, home: t, uid: 501)
        try agents.install(.updateCheck(appExecutable: "/A/HeliosSetup"), load: true)
        XCTAssertTrue(agents.isInstalled(LaunchAgent.updateLabel))
        XCTAssertTrue(
            r.calls.contains([
                "/bin/launchctl", "bootstrap", "gui/501", "\(t)/Library/LaunchAgents/\(LaunchAgent.updateLabel).plist",
            ]))
        agents.remove(LaunchAgent.updateLabel)
        XCTAssertFalse(agents.isInstalled(LaunchAgent.updateLabel))
    }
}

final class GPUAppsTests: XCTestCase {
    func testDecideMirrorsTheScript() {
        let rules = GPUApps.rules("# comment\n-Finder\n  Blender  \n*\n")
        XCTAssertEqual(rules, ["-Finder", "Blender", "*"])
        XCTAssertEqual(GPUApps.decide("Finder", rules: rules), .denied)
        XCTAssertEqual(GPUApps.decide("Blender", rules: rules), .allowed)
        XCTAssertEqual(GPUApps.decide("Other", rules: rules), .allowed, "*")
        XCTAssertEqual(GPUApps.decide("Other", rules: ["Blender"]), .notListed)
    }

    func testInstalledApps() {
        let t = tempDir(self)
        write(
            "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>CFBundleExecutable</key><string>Blender</string></dict></plist>",
            "\(t)/Blender.app/Contents/Info.plist")
        write("x", "\(t)/Broken.app/Contents/MacOS/x")
        XCTAssertEqual(GPUApps.installedApps(in: [t], fs: LocalFileSystem()).map(\.executable), ["Blender"])
    }
}
