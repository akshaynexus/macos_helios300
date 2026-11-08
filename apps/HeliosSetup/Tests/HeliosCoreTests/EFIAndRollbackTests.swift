// SPDX-License-Identifier: GPL-2.0-only
//
// OpenCore install/restore on a fake ESP (a temp directory), and update apply + rollback of an OpenCore unit.
import XCTest

@testable import HeliosCore

final class EFIAndRollbackTests: XCTestCase {
    let clock = FakeClock(now: Date(timeIntervalSince1970: 1_762_560_000))  // 2025-11-08 00:00:00 UTC

    func makeEFI(_ root: String, marker: String) {
        write("bootx64-\(marker)", "\(root)/EFI/BOOT/BOOTx64.efi")
        write("xxREL-105-2025-05-05xx", "\(root)/EFI/OC/OpenCore.efi")
        write(String(decoding: fixtureData("config-internal.plist"), as: UTF8.self), "\(root)/EFI/OC/config.plist")
        write("kext-\(marker)", "\(root)/EFI/OC/Kexts/PG106.kext/Contents/MacOS/PG106")
    }

    func manager() -> (EFIManager, FakeRunner) {
        let r = FakeRunner()
        r.set(
            "/usr/sbin/diskutil list -plist internal physical",
            String(decoding: fixtureData("diskutil-list-internal.plist"), as: UTF8.self))
        return (EFIManager(runner: r, fs: LocalFileSystem(), clock: clock), r)
    }

    func testFindsTheInternalESP() throws {
        let (efi, _) = manager()
        XCTAssertEqual(try efi.internalESP(), "disk0s1")
        XCTAssertEqual(efi.backupName(), "EFI-backup-20251108-000000")
    }

    func testInstallBacksUpVerifiesAndSwaps() throws {
        let t = tempDir(self)
        makeEFI("\(t)/stick", marker: "new")
        makeEFI("\(t)/esp", marker: "old")
        let (efi, _) = manager()
        let src = try efi.sourceEFI("\(t)/stick")
        XCTAssertEqual(src, "\(t)/stick/EFI")
        var log: [String] = []
        let backup = try efi.install(from: src, esp: "\(t)/esp") { log.append($0) }
        XCTAssertEqual(backup, "EFI-backup-20251108-000000")
        XCTAssertEqual(try String(contentsOfFile: "\(t)/esp/EFI/BOOT/BOOTx64.efi", encoding: .utf8), "bootx64-new")
        XCTAssertEqual(
            try String(contentsOfFile: "\(t)/esp/EFI-backup-20251108-000000/BOOT/BOOTx64.efi", encoding: .utf8), "bootx64-old")
        XCTAssertFalse(
            FileManager.default.fileExists(atPath: "\(t)/esp/EFI-backup-20251108-000000/EFI"),
            "a backup never looks like \\EFI\\BOOT to the firmware")
        XCTAssertFalse(FileManager.default.fileExists(atPath: "\(t)/esp/EFI.new"))
        XCTAssertEqual(efi.backups(esp: "\(t)/esp"), ["EFI-backup-20251108-000000"])
        XCTAssertTrue(log.contains { $0.contains("verified") })
    }

    func testRestorePutsTheBackupBack() throws {
        let t = tempDir(self)
        makeEFI("\(t)/stick", marker: "new")
        makeEFI("\(t)/esp", marker: "old")
        let (efi, _) = manager()
        let backup = try XCTUnwrap(try efi.install(from: "\(t)/stick/EFI", esp: "\(t)/esp") { _ in })
        let (later, _) = (EFIManager(runner: FakeRunner(), fs: LocalFileSystem(), clock: FakeClock(now: clock.now + 60)), 0)
        let kept = try later.restore(backup: backup, esp: "\(t)/esp") { _ in }
        XCTAssertEqual(try String(contentsOfFile: "\(t)/esp/EFI/BOOT/BOOTx64.efi", encoding: .utf8), "bootx64-old")
        XCTAssertEqual(kept, "EFI-backup-20251108-000100-before-restore")
    }

    func testNotAnEFIIsRefusedAndSpaceIsChecked() throws {
        let t = tempDir(self)
        let (efi, _) = manager()
        XCTAssertThrowsError(try efi.sourceEFI(t))
        makeEFI("\(t)/stick", marker: "new")
        XCTAssertNoThrow(try efi.checkSpace(source: "\(t)/stick/EFI", existing: nil, esp: t))
    }

    func testVerifyIgnoresFATHousekeepingButNotRealDifferences() throws {
        let t = tempDir(self)
        makeEFI("\(t)/a", marker: "x")
        makeEFI("\(t)/b", marker: "x")
        write("", "\(t)/b/EFI/OC/._config.plist")
        write("", "\(t)/b/EFI/.DS_Store")
        let (efi, _) = manager()
        XCTAssertNoThrow(try efi.verify("\(t)/a/EFI", "\(t)/b/EFI", what: "EFI"))
        write("changed", "\(t)/b/EFI/OC/Kexts/PG106.kext/Contents/MacOS/PG106")
        XCTAssertThrowsError(try efi.verify("\(t)/a/EFI", "\(t)/b/EFI", what: "EFI"))
    }

    func testOcvalidateOnlyForTheSameOpenCoreVersion() throws {
        let t = tempDir(self)
        makeEFI(t, marker: "x")
        write("tool", "\(t)/EFI/OC/Tools/ocvalidate")
        let (efi, r) = manager()
        let candidates = EFIManager.ocvalidateCandidates(efi: "\(t)/EFI", releaseDir: nil)
        r.set(
            "\(t)/EFI/OC/Tools/ocvalidate \(t)/EFI/OC/config.plist",
            "NOTE: This version of ocvalidate is only compatible with OpenCore version 1.0.4!\n")
        guard case .skipped(let why) = efi.validate(efi: "\(t)/EFI", candidates: candidates) else { return XCTFail() }
        XCTAssertTrue(why.contains("1.0.4"))
        r.set(
            "\(t)/EFI/OC/Tools/ocvalidate \(t)/EFI/OC/config.plist",
            "NOTE: This version of ocvalidate is only compatible with OpenCore version 1.0.5!\nCompleted validating", status: 0)
        guard case .passed = efi.validate(efi: "\(t)/EFI", candidates: candidates) else { return XCTFail() }
        r.set(
            "\(t)/EFI/OC/Tools/ocvalidate \(t)/EFI/OC/config.plist",
            "NOTE: ... OpenCore version 1.0.5!\nKernel->Add[3]: bad", status: 1)
        guard case .failed = efi.validate(efi: "\(t)/EFI", candidates: candidates) else { return XCTFail() }
        guard case .skipped = efi.validate(efi: "\(t)/EFI", candidates: ["\(t)/none"]) else { return XCTFail() }
    }

    // MARK: update apply + rollback

    func context(_ t: String, newDir: String, old: Manifest?) throws -> UpdateContext {
        UpdateContext(
            new: try manifestFixture("manifest-next.json"), newDir: newDir, old: old, oldDir: nil, appPath: nil,
            user: UserContext(name: "user", home: "\(t)/home"), output: nil, mountESP: { "\(t)/esp" })
    }

    func testOpenCoreUnitKeepsPrevAndRollbackRestoresTheExactBytes() throws {
        let t = tempDir(self)
        makeEFI("\(t)/esp", marker: "old")
        let configBefore = try Data(contentsOf: URL(fileURLWithPath: "\(t)/esp/EFI/OC/config.plist"))
        write("kext-new", "\(t)/rel/x/PG106.kext/Contents/MacOS/PG106")
        var oldManifest = try manifestFixture("manifest-next.json")
        oldManifest.configDelta?.kernelAdd = []
        var ctx = try context(t, newDir: "\(t)/rel", old: oldManifest)
        if let i = ctx.new.configDelta?.kernelAdd.firstIndex(where: { $0.bundlePath == "PG106.kext" }) {
            ctx.new.configDelta?.kernelAdd[i].maxKernel = "25.99.99"  // so config.plist is written (and backed up)
        }
        let probes = ProbeContext(runner: FakeRunner(), fs: LocalFileSystem())
        let applier = UpdateApplier(fs: LocalFileSystem(), runner: FakeRunner(), clock: clock, probes: probes)
        let unit = UpdateUnit(id: "PG106.kext", title: "PG106", kind: .opencore, artifacts: ["PG106.kext"], requiresRestart: true)
        var log: [String] = []
        let record = try applier.apply(unit, ctx) { log.append($0) }
        XCTAssertEqual(
            try String(contentsOfFile: "\(t)/esp/EFI/OC/Kexts/PG106.kext/Contents/MacOS/PG106", encoding: .utf8), "kext-new")
        XCTAssertEqual(
            try String(contentsOfFile: "\(t)/esp/EFI/OC/Kexts/PG106.kext.prev/Contents/MacOS/PG106", encoding: .utf8), "kext-old")
        XCTAssertEqual(record.ops.first, .restore("EFI/OC/Kexts/PG106.kext", from: "EFI/OC/Kexts/PG106.kext.prev", onESP: true))
        XCTAssertTrue(log.contains { $0.contains("WARNING: no ocvalidate") }, "skipped, with a warning")

        try RollbackExecutor(fs: LocalFileSystem(), runner: FakeRunner(), user: ctx.user).undo(
            record, esp: "\(t)/esp", output: nil
        ) { _ in }
        XCTAssertEqual(
            try String(contentsOfFile: "\(t)/esp/EFI/OC/Kexts/PG106.kext/Contents/MacOS/PG106", encoding: .utf8), "kext-old")
        XCTAssertFalse(FileManager.default.fileExists(atPath: "\(t)/esp/EFI/OC/Kexts/PG106.kext.prev"))
        XCTAssertEqual(try Data(contentsOf: URL(fileURLWithPath: "\(t)/esp/EFI/OC/config.plist")), configBefore)
    }

    func testFailedOcvalidateUndoesTheUnitAtOnce() throws {
        let t = tempDir(self)
        makeEFI("\(t)/esp", marker: "old")
        write("tool", "\(t)/esp/EFI/OC/Tools/ocvalidate")
        let configBefore = try Data(contentsOf: URL(fileURLWithPath: "\(t)/esp/EFI/OC/config.plist"))
        write("kext-new", "\(t)/rel/x/PG106.kext/Contents/MacOS/PG106")
        var oldManifest = try manifestFixture("manifest-next.json")
        oldManifest.configDelta?.kernelAdd = []
        let ctx = try context(t, newDir: "\(t)/rel", old: oldManifest)
        // the release changes PG106's range, so config.plist is written, and the validation fails
        var new = ctx.new
        if let i = new.configDelta?.kernelAdd.firstIndex(where: { $0.bundlePath == "PG106.kext" }) {
            new.configDelta?.kernelAdd[i].maxKernel = "25.99.99"
        }
        var c2 = ctx
        c2.new = new
        let r = FakeRunner()
        r.set("\(t)/esp/EFI/OC/Tools/ocvalidate \(t)/esp/EFI/OC/config.plist", "OpenCore version 1.0.5!\nbroken", status: 1)
        let applier = UpdateApplier(
            fs: LocalFileSystem(), runner: r, clock: clock, probes: ProbeContext(runner: r, fs: LocalFileSystem()))
        let unit = UpdateUnit(id: "PG106.kext", title: "PG106", kind: .opencore, artifacts: ["PG106.kext"], requiresRestart: true)
        XCTAssertThrowsError(try applier.apply(unit, c2) { _ in })
        XCTAssertEqual(
            try String(contentsOfFile: "\(t)/esp/EFI/OC/Kexts/PG106.kext/Contents/MacOS/PG106", encoding: .utf8), "kext-old")
        XCTAssertEqual(try Data(contentsOf: URL(fileURLWithPath: "\(t)/esp/EFI/OC/config.plist")), configBefore)
    }

    func testKextTheUserLeftOutIsSkipped() throws {
        let t = tempDir(self)
        makeEFI("\(t)/esp", marker: "old")
        write("new", "\(t)/rel/x/NMHDA.kext/Contents/MacOS/NMHDA")
        let ctx = try context(t, newDir: "\(t)/rel", old: try manifestFixture("manifest-next.json"))
        let applier = UpdateApplier(
            fs: LocalFileSystem(), runner: FakeRunner(), clock: clock,
            probes: ProbeContext(runner: FakeRunner(), fs: LocalFileSystem()))
        let unit = UpdateUnit(id: "NMHDA.kext", title: "NMHDA", kind: .opencore, artifacts: ["NMHDA.kext"], requiresRestart: true)
        var log: [String] = []
        let record = try applier.apply(unit, ctx) { log.append($0) }
        XCTAssertTrue(record.ops.isEmpty)
        XCTAssertFalse(FileManager.default.fileExists(atPath: "\(t)/esp/EFI/OC/Kexts/NMHDA.kext"))
        XCTAssertTrue(log.contains { $0.contains("skipped") })
    }

    func testLibraryExtensionsUnitRunsTheNewScriptDryRunFirstAndOnlyWhenInstalled() throws {
        let t = tempDir(self)
        var new = try manifestFixture("manifest-next.json")
        let script = "#!/bin/bash\n# the new release's install-gpu.sh\n"
        write(script, "\(t)/rel/install-gpu.sh")
        if let i = new.artifacts.firstIndex(where: { $0.name == "install-gpu.sh" }) {
            new.artifacts[i].sha256 = Hashing.sha256(Data(script.utf8))
        }
        let (r, fs, probes) = fakeMac(self, files: ["/Library/Extensions/PG106FB.kext"])
        _ = fs
        let ctx = UpdateContext(
            new: new, newDir: "\(t)/rel", old: new, oldDir: "\(t)/old", appPath: nil,
            user: UserContext(name: "user", home: "/Users/user"), output: nil, mountESP: { "" })
        let applier = UpdateApplier(fs: LocalFileSystem(), runner: r, clock: clock, probes: probes)
        let units = [
            UpdateUnit(
                id: "PG106FB.kext", title: "PG106FB", kind: .libraryExtensions, artifacts: ["PG106FB.kext"], requiresRestart: true
            ),
            UpdateUnit(
                id: "PG106Accel.kext", title: "PG106Accel", kind: .libraryExtensions, artifacts: ["PG106Accel.kext"],
                requiresRestart: true),
        ]
        var records: [RollbackRecord] = []
        for u in units { records.append(try applier.apply(u, ctx) { _ in }) }
        let modes = r.streamed.map { "\($0.env?["MODE"] ?? "") \($0.env?["ONLY"] ?? "")" }
        XCTAssertEqual(modes, ["dry-run PG106FB", "install PG106FB"], "PG106Accel is not installed here: skipped")
        XCTAssertEqual(r.streamed.first?.cwd, "\(t)/rel")
        XCTAssertEqual(
            records.first?.ops, [.script("install-gpu.sh", releaseDir: "\(t)/old", env: ["ONLY": "PG106FB", "MODE": "install"])])
    }
}
