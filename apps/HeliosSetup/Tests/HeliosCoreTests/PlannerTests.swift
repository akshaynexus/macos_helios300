// SPDX-License-Identifier: GPL-2.0-only
import XCTest

@testable import HeliosCore

final class PlannerTests: XCTestCase {
    let fb = "/Library/Extensions/PG106FB.kext"
    let accel = "/Library/Extensions/PG106Accel.kext"

    func items() throws -> [SetupItem] { StepPlanner.items(try realSpec()) }

    func item(_ id: String) throws -> SetupItem {
        guard let i = try items().first(where: { $0.id == id }) else { throw EngineError.unknownItem(id) }
        return i
    }

    // MARK: ordering (from the real spec.json)

    func testOrderFromSpecWithGPUPartsInPlaybookOrder() throws {
        XCTAssertEqual(
            try items().map(\.id),
            ["opencore", "gpu-fb", "gpu-metal", "gpu-accel", "audio-applehda", "audio-voodoohda", "sleep"])
    }

    func testPartsInheritTheStepCommandsAndAddOnlyTheirEnv() throws {
        let fb = try item("gpu-fb")
        XCTAssertEqual(fb.command("install"), "MODE=install bash install-gpu.sh")
        XCTAssertEqual(fb.env, ["ONLY": "PG106FB"])
        XCTAssertTrue(fb.reboot && fb.driverChange)
        let metal = try item("gpu-metal")
        XCTAssertFalse(metal.reboot)
        XCTAssertTrue(metal.driverChange, "the userspace part needs no restart but is still one change per boot")
        XCTAssertNotNil(metal.undoNote)
        XCTAssertFalse(try item("sleep").driverChange)
    }

    func testOrderTiesKeepManifestOrder() throws {
        var m = try realSpec()
        m.postInstall = m.postInstall.map {
            var s = $0; s.order = 5; return s
        }
        XCTAssertEqual(StepPlanner.items(m).map(\.id).prefix(2), ["opencore", "gpu-fb"], "equal orders keep the spec's order")
    }

    // MARK: status

    func status(
        _ id: String, _ ctx: ProbeContext, system: SystemState = SystemState(), user: UserState = UserState(),
        boot: String = "BOOT-A"
    ) throws -> ItemStatus {
        let all = try items()
        return StepPlanner.status(try item(id), all: all, ctx: ctx, system: system, user: user, boot: boot)
    }

    func testNotAvailableWhenPG106IsNotUp() throws {
        let (_, _, ctx) = fakeMac(self, pg106Ready: false)
        guard case .notAvailable(let why) = try status("gpu-fb", ctx) else { return XCTFail("want notAvailable") }
        XCTAssertTrue(why.contains("PG106Ready"))
    }

    func testNotAvailableOnAnotherBuild() throws {
        let (_, _, ctx) = fakeMac(self, build: "25H100")
        guard case .notAvailable(let why) = try status("audio-voodoohda", ctx) else { return XCTFail() }
        XCTAssertTrue(why.contains("25G332"))
    }

    func testInstalledNeedsFileAndLoadedKext() throws {
        let (_, _, ctx) = fakeMac(self, loaded: ["org.helios300.pg106fb"], files: [fb])
        XCTAssertEqual(try status("gpu-fb", ctx), .installed)
        XCTAssertEqual(try status("gpu-accel", ctx), .notInstalled)
    }

    func testInstalledButNotLoadedAfterARestartIsFailedWithTheHint() throws {
        let (_, _, ctx) = fakeMac(self, files: [fb])
        guard case .failed(let why) = try status("gpu-fb", ctx) else { return XCTFail() }
        XCTAssertTrue(why.contains("Privacy & Security"))
    }

    func testChangeInThisBootNeedsRestart() throws {
        let (_, _, ctx) = fakeMac(self, files: [fb])
        var s = SystemState()
        s.record(
            ChangeRecord(
                item: "gpu-fb", action: "install", bootSession: "BOOT-A", date: Date(), driverChange: true, needsRestart: true))
        XCTAssertEqual(try status("gpu-fb", ctx, system: s).label, "Needs restart")
        XCTAssertEqual(try status("gpu-fb", ctx, system: s, boot: "BOOT-B").label, "Failed", "after the restart: not loaded")
    }

    func testAppleHDAInstallWaitsForTheRestartEvenWithoutFiles() throws {
        let (_, _, ctx) = fakeMac(self)
        var s = SystemState()
        s.record(
            ChangeRecord(
                item: "audio-applehda", action: "install", bootSession: "BOOT-A", date: Date(), driverChange: true,
                needsRestart: true))
        XCTAssertEqual(try status("audio-applehda", ctx, system: s).label, "Needs restart")
    }

    func testFailedRunInThisBootIsFailed() throws {
        let (_, _, ctx) = fakeMac(self)
        var u = UserState()
        u.runs["gpu-fb"] = RunRecord(action: "install", status: 1, bootSession: "BOOT-A", date: Date(), log: "/tmp/x.log")
        XCTAssertEqual(try status("gpu-fb", ctx, user: u).label, "Failed")
        u.runs["gpu-fb"]?.action = "check"
        XCTAssertEqual(try status("gpu-fb", ctx, user: u), .notInstalled, "a failed dry run is not a failed install")
    }

    func testPartlyInstalledMetalUserspace() throws {
        let (_, _, ctx) = fakeMac(self, files: ["/Library/GPUBundles/NVMTLDriver.bundle"])
        guard case .failed(let why) = try status("gpu-metal", ctx) else { return XCTFail() }
        XCTAssertTrue(why.contains("partly"))
    }

    func testAudioGroupAlternativeIsNotAvailableOnceOneIsInstalled() throws {
        let (_, _, ctx) = fakeMac(
            self, loaded: ["com.apple.driver.AppleHDA"],
            files: ["/System/Library/CoreServices/OpenCore-Legacy-Patcher.plist"])
        XCTAssertEqual(try status("audio-applehda", ctx), .installed)
        guard case .notAvailable(let why) = try status("audio-voodoohda", ctx) else { return XCTFail() }
        XCTAssertTrue(why.contains("only one audio option"))
    }

    func testFileVaultBlocksAppleHDA() throws {
        let (r, _, ctx) = fakeMac(self)
        r.set("/usr/bin/fdesetup status", "FileVault is On.\n")
        guard case .notAvailable(let why) = try status("audio-applehda", ctx) else { return XCTFail() }
        XCTAssertTrue(why.contains("FileVault"))
    }

    func testSleepStep() throws {
        XCTAssertEqual(try status("sleep", fakeMac(self, sleepDisabled: true).2), .notInstalled)
        XCTAssertEqual(try status("sleep", fakeMac(self, sleepDisabled: false).2), .installed)
        guard case .notAvailable = try status("sleep", fakeMac(self, bootArgs: "-v pg106_pm=0").2) else { return XCTFail() }
    }

    func testOpenCoreInstalledOnlyWhenBootedFromTheInternalESP() throws {
        let (r, _, ctx) = fakeMac(self)
        r.set("/usr/sbin/nvram \(Tool.ocGUID):boot-path", fixture("nvram-boot-path-usb.txt"))
        ctx.internalESPUUID = "8E2D1F5A-3C4B-4D5E-8F90-A1B2C3D4E5F6"
        XCTAssertEqual(try status("opencore", ctx), .notInstalled)
        let (r2, _, ctx2) = fakeMac(self)
        r2.set("/usr/sbin/nvram \(Tool.ocGUID):boot-path", fixture("nvram-boot-path-internal.txt"))
        ctx2.internalESPUUID = "8e2d1f5a-3c4b-4d5e-8f90-a1b2c3d4e5f6"
        XCTAssertEqual(try status("opencore", ctx2), .installed)
    }

    func testNextPrefersTheRecommendedAudioOption() throws {
        let all = try items()
        var st = Dictionary(uniqueKeysWithValues: all.map { ($0.id, ItemStatus.installed) })
        st["audio-applehda"] = .notInstalled
        st["audio-voodoohda"] = .notInstalled
        XCTAssertEqual(StepPlanner.next(all, statuses: st)?.id, "audio-applehda")
        st["audio-applehda"] = .notAvailable("FileVault")
        XCTAssertEqual(StepPlanner.next(all, statuses: st)?.id, "audio-voodoohda")
        st["audio-voodoohda"] = .installed
        st["sleep"] = .notInstalled
        XCTAssertEqual(StepPlanner.next(all, statuses: st)?.id, "sleep")
    }

    // MARK: guards

    func testOneDriverChangePerRestart() throws {
        var s = SystemState()
        XCTAssertEqual(RestartGuard.check(try item("gpu-fb"), action: "install", system: s, boot: "A"), .allowed)
        s.record(
            ChangeRecord(
                item: "gpu-fb", action: "install", bootSession: "A", date: Date(), driverChange: true, needsRestart: true))
        guard case .restartFirst(let why) = RestartGuard.check(try item("gpu-accel"), action: "install", system: s, boot: "A")
        else { return XCTFail("a second kext install in the same boot must be refused") }
        XCTAssertTrue(why.contains("gpu-fb"))
        XCTAssertEqual(RestartGuard.check(try item("gpu-accel"), action: "install", system: s, boot: "B"), .allowed)
        XCTAssertEqual(
            RestartGuard.check(try item("gpu-accel"), action: "check", system: s, boot: "A"), .allowed,
            "a dry run is never blocked")
        XCTAssertEqual(
            RestartGuard.check(try item("sleep"), action: "install", system: s, boot: "A"), .allowed,
            "pmset is not a driver change")
        XCTAssertEqual(
            RestartGuard.check(try item("gpu-fb"), action: "revert", system: s, boot: "A"), .allowed,
            "Undo of the install this boot is waiting on is not a second change")
        guard case .restartFirst = RestartGuard.check(try item("gpu-accel"), action: "revert", system: s, boot: "A")
        else { return XCTFail("undo of something else is a change too") }
    }

    func testUserspacePartCountsAsTheChangeOfThisBoot() throws {
        var s = SystemState()
        s.record(
            ChangeRecord(
                item: "gpu-metal", action: "install", bootSession: "A", date: Date(), driverChange: true, needsRestart: false))
        guard case .restartFirst = RestartGuard.check(try item("gpu-accel"), action: "install", system: s, boot: "A") else {
            return XCTFail()
        }
    }

    func testDryRunFirst() throws {
        let fb = try item("gpu-fb")
        let now = Date(timeIntervalSince1970: 1_000_000)
        var u = UserState()
        XCTAssertEqual(DryRunRule.check(fb, user: u, boot: "A", manifestSha256: "m", now: now).isAllowed, false)
        u.checks["gpu-fb"] = CheckRecord(bootSession: "A", date: now, status: 0, manifestSha256: "m")
        XCTAssertEqual(DryRunRule.check(fb, user: u, boot: "A", manifestSha256: "m", now: now), .allowed)
        XCTAssertFalse(DryRunRule.check(fb, user: u, boot: "B", manifestSha256: "m", now: now).isAllowed)
        XCTAssertFalse(DryRunRule.check(fb, user: u, boot: "A", manifestSha256: "other", now: now).isAllowed)
        XCTAssertFalse(DryRunRule.check(fb, user: u, boot: "A", manifestSha256: "m", now: now + 5 * 3600).isAllowed)
        u.checks["gpu-fb"]?.status = 1
        XCTAssertFalse(DryRunRule.check(fb, user: u, boot: "A", manifestSha256: "m", now: now).isAllowed)
    }

    // MARK: the spec stays consistent with the scripts it names

    func testEveryCommandParsesAndEveryScriptIsAnArtifact() throws {
        let m = try realSpec()
        for item in StepPlanner.items(m) {
            for (action, line) in item.run {
                let cmd = try CommandParser.parse(line)
                if case .script(let name, _, let env) = cmd {
                    XCTAssertNotNil(m.artifact(name), "\(item.id) \(action): \(name)")
                    XCTAssertEqual(env["MODE"], action == "check" ? "dry-run" : action, "\(item.id) \(action)")
                }
            }
        }
    }

    /// The build each step's `build` probe names must be the one its script pins (PINNED_BUILD / KDK_BUILD /
    /// BUILD_WANT), so the app's "not available on this build" and the script's own refusal agree.
    func testBuildProbesMatchTheScriptPins() throws {
        let m = try realSpec()
        for item in StepPlanner.items(m) {
            guard let script = item.script else { continue }
            let text = try String(contentsOf: repoRoot.appendingPathComponent("tools/macos/\(script)"), encoding: .utf8)
            let pins = text.split(separator: "\n").compactMap { line -> String? in
                guard let r = line.range(of: #"^[A-Z_]*BUILD[A-Z_]*=(25[A-Z][0-9]+[a-z]?)\b"#, options: .regularExpression) else {
                    return nil
                }
                return String(line[r].split(separator: "=")[1])
            }
            let probes = (item.detect?.available ?? []).filter { $0.probe == "build" }.compactMap(\.equals)
            XCTAssertFalse(pins.isEmpty, "\(script) pins no build")
            XCTAssertEqual(Set(probes), Set(pins), "\(item.id): spec build probe vs \(script) pin")
        }
    }
}

extension GuardDecision {
    var isAllowed: Bool { self == .allowed }
}
