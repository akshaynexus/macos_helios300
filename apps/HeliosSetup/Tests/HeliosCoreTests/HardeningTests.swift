// SPDX-License-Identifier: GPL-2.0-only
//
// Regression tests for the pre-release review of the app: what happens when work stops partway (every failure point of
// the EFI swap and of an update), what the root side refuses, rollback, the manifest signature.
import XCTest

@testable import HeliosCore

// MARK: - the EFI swap

final class EFISwapTests: XCTestCase {
    let clock = FakeClock(now: Date(timeIntervalSince1970: 1_762_560_000))

    func manager(_ fs: FileSystem) -> EFIManager { EFIManager(runner: FakeRunner(), fs: fs, clock: clock) }

    func text(_ p: String) -> String { (try? String(contentsOfFile: p, encoding: .utf8)) ?? "<MISSING>" }

    /// "old" or "new" when both parts are one generation; anything else is the bug.
    func generation(_ t: String) -> String {
        let boot = text("\(t)/esp/EFI/BOOT/BOOTx64.efi"), kext = text("\(t)/esp/EFI/OC/Kexts/PG106.kext/Contents/MacOS/PG106")
        if boot == "bootx64-old" && kext == "kext-old" { return "old" }
        if boot == "bootx64-new" && kext == "kext-new" { return "new" }
        return "mixed: \(boot) + \(kext)"
    }

    func fresh(_ t: String) {
        try? FileManager.default.removeItem(atPath: "\(t)/esp")
        makeTestEFI("\(t)/esp", marker: "old")
    }

    func assertNoSwapLeftovers(_ t: String, line: UInt = #line) {
        for d in ["EFI.new", "EFI.old", "EFI.old.del"] {
            XCTAssertFalse(FileManager.default.fileExists(atPath: "\(t)/esp/\(d)"), "\(d) left behind", line: line)
        }
        XCTAssertFalse(
            ((try? FileManager.default.contentsOfDirectory(atPath: "\(t)/esp")) ?? []).contains { $0.hasPrefix("EFI-partial-") },
            "partial backup left behind", line: line)
    }

    func testEveryFailurePointEndsFullyOldOrFullyNew() {
        let t = tempDir(self)
        makeTestEFI("\(t)/stick", marker: "new")
        for killed in [false, true] {
            var n = 0
            while true {
                fresh(t)
                let fs = FaultFS(failAt: n, killed: killed)
                let result = Result { try manager(fs).install(from: "\(t)/stick/EFI", esp: "\(t)/esp") { _ in } }
                if !fs.fired {
                    XCTAssertEqual(generation(t), "new")
                    break
                }
                try? manager(LocalFileSystem()).recoverSwap(esp: "\(t)/esp") { _ in }
                let g = generation(t)
                XCTAssertTrue(g == "old" || g == "new", "failure \(n), killed \(killed): \(g)")
                if case .failure = result { XCTAssertEqual(g, "old", "failure \(n): a failed install leaves the old EFI") }
                assertNoSwapLeftovers(t)
                n += 1
            }
            XCTAssertGreaterThan(n, 6, "the loop saw the whole swap")
        }
    }

    func testMoveFailureOnOCPutsBootBackAndSaysSo() throws {
        let t = tempDir(self)
        makeTestEFI("\(t)/stick", marker: "new")
        fresh(t)
        struct FailMove: FileSystem {
            let fs = LocalFileSystem()
            let needle: String
            func exists(_ p: String) -> Bool { fs.exists(p) }
            func isDirectory(_ p: String) -> Bool { fs.isDirectory(p) }
            func isSymlink(_ p: String) -> Bool { fs.isSymlink(p) }
            func read(_ p: String) throws -> Data { try fs.read(p) }
            func write(_ d: Data, to p: String) throws { try fs.write(d, to: p) }
            func list(_ p: String) throws -> [String] { try fs.list(p) }
            func makeDirectory(_ p: String) throws { try fs.makeDirectory(p) }
            func copy(_ a: String, to b: String) throws { try fs.copy(a, to: b) }
            func move(_ a: String, to b: String) throws {
                if b.hasSuffix(needle) { throw FaultFS.Fault() }
                try fs.move(a, to: b)
            }
            func remove(_ p: String) throws { try fs.remove(p) }
            func freeBytes(_ p: String) -> Int64? { fs.freeBytes(p) }
            func size(_ p: String) -> Int64? { fs.size(p) }
            func makeSecureDirectory(in p: String, prefix: String) throws -> String {
                try fs.makeSecureDirectory(in: p, prefix: prefix)
            }
            func ownAsRoot(_ p: String) throws {}
            func makeExecutable(_ p: String) throws {}
        }
        let efi = manager(FailMove(needle: "/EFI/OC"))
        XCTAssertThrowsError(try efi.install(from: "\(t)/stick/EFI", esp: "\(t)/esp") { _ in }) { e in
            guard case .swapFailed = e as? EFIError else { return XCTFail("\(e)") }
            XCTAssertTrue("\(e)".contains("back in place"), "\(e)")
            XCTAssertFalse("\(e)".contains("nothing was swapped"))
        }
        XCTAssertEqual(generation(t), "old")
        assertNoSwapLeftovers(t)
    }

    func testAKilledSwapIsRepairedBeforeTheNextOne() throws {
        let t = tempDir(self)
        makeTestEFI("\(t)/stick", marker: "new")
        fresh(t)
        // the state a kill leaves half way through the renames: BOOT aside, the new OC already in
        try LocalFileSystem().write(Data("BOOT\nOC".utf8), to: "\(t)/esp/EFI.old/.parts")
        try LocalFileSystem().move("\(t)/esp/EFI/BOOT", to: "\(t)/esp/EFI.old/BOOT")
        try LocalFileSystem().move("\(t)/esp/EFI/OC", to: "\(t)/esp/EFI.old/OC")
        makeTestEFI("\(t)/half", marker: "new")
        try LocalFileSystem().move("\(t)/half/EFI/OC", to: "\(t)/esp/EFI/OC")
        var log: [String] = []
        try manager(LocalFileSystem()).recoverSwap(esp: "\(t)/esp") { log.append($0) }
        XCTAssertEqual(generation(t), "old")
        assertNoSwapLeftovers(t)
        XCTAssertTrue(log.contains { $0.contains("interrupted") })
    }

    func testPartialBackupIsNeverLeftAndOldBackupsArePruned() throws {
        let t = tempDir(self)
        makeTestEFI("\(t)/stick", marker: "new")
        fresh(t)
        for second in 0..<6 {
            let later = FakeClock(now: clock.now + Double(second))
            let efi = EFIManager(runner: FakeRunner(), fs: LocalFileSystem(), clock: later)
            try efi.install(from: "\(t)/stick/EFI", esp: "\(t)/esp") { _ in }
        }
        let backups = manager(LocalFileSystem()).backups(esp: "\(t)/esp")
        XCTAssertEqual(backups.count, EFIManager.backupsKept, "\(backups)")
        XCTAssertEqual(backups.last, "EFI-backup-20251108-000005")
    }

    func testRestoreRefusesANameThatIsNotABackup() {
        let t = tempDir(self)
        makeTestEFI(t, marker: "x")
        XCTAssertThrowsError(try manager(LocalFileSystem()).restore(backup: "../etc", esp: t) { _ in }) { e in
            XCTAssertEqual(e as? EFIError, .badBackupName("../etc"))
        }
    }

    func testTheESPMustBeOnTheDiskThatHoldsMacOS() throws {
        let r = FakeRunner()
        r.set(
            "/usr/sbin/diskutil list -plist internal physical",
            String(decoding: fixtureData("diskutil-list-internal.plist"), as: UTF8.self))
        r.set("/usr/sbin/diskutil info -plist /", String(decoding: fixtureData("diskutil-info-root.plist"), as: UTF8.self))
        let efi = EFIManager(runner: r, fs: LocalFileSystem(), clock: clock)
        XCTAssertEqual(try efi.internalESP(), "disk0s1", "the only one, and on macOS's disk")
        r.set(
            "/usr/sbin/diskutil info -plist /",
            String(decoding: fixtureData("diskutil-info-root.plist"), as: UTF8.self).replacingOccurrences(
                of: "disk0s2", with: "disk4s2"))
        XCTAssertThrowsError(try efi.internalESP(), "macOS is on another disk than the one EFI partition")
    }
}

// MARK: - staged swap, rollback

final class StagedSwapTests: XCTestCase {
    let clock = FakeClock(now: Date(timeIntervalSince1970: 1_762_560_000))

    func applier(_ runner: FakeRunner = FakeRunner(), probes: ProbeContext? = nil) -> UpdateApplier {
        UpdateApplier(
            fs: LocalFileSystem(), runner: runner, clock: clock,
            probes: probes ?? ProbeContext(runner: FakeRunner(), fs: LocalFileSystem()))
    }

    func ctx(_ t: String, old: Manifest?) throws -> UpdateContext {
        UpdateContext(
            new: try manifestFixture("manifest-next.json"), newDir: "\(t)/rel", old: old, oldDir: nil, appPath: nil,
            user: UserContext(name: "u", home: "\(t)/home"), output: nil, mountESP: { "\(t)/esp" })
    }

    func r(_ p: String) -> String { (try? String(contentsOfFile: p, encoding: .utf8)) ?? "<MISSING>" }

    let pg106 = UpdateUnit(id: "PG106.kext", title: "PG106", kind: .opencore, artifacts: ["PG106.kext"], requiresRestart: true)

    /// The reviewer's mixed group: every part applied, not just the OpenCore one.
    func testPG106GroupAppliesEveryInstallKind() throws {
        let t = tempDir(self)
        makeTestEFI("\(t)/esp", marker: "old")
        write("new", "\(t)/rel/x/PG106.kext/Contents/MacOS/PG106")
        var m = try manifestFixture("manifest-next.json")
        let script = "#!/bin/bash\n"
        write(script, "\(t)/rel/install-gpu.sh")
        let i = try XCTUnwrap(m.artifacts.firstIndex { $0.name == "install-gpu.sh" })
        m.artifacts[i].sha256 = Hashing.sha256(Data(script.utf8))
        let unit = UpdatePlanner.unit(
            id: "group-pg106", title: "g", artifacts: ["PG106.kext", "PG106FB.kext", "PG106Accel.kext"], manifest: m)
        let (run, _, probes) = fakeMac(self, files: ["/Library/Extensions/PG106FB.kext", "/Library/Extensions/PG106Accel.kext"])
        var c = try ctx(t, old: m)
        c.new = m
        let record = try applier(run, probes: probes).apply(unit, c) { _ in }
        XCTAssertEqual(r("\(t)/esp/EFI/OC/Kexts/PG106.kext/Contents/MacOS/PG106"), "new")
        XCTAssertEqual(
            run.streamed.map { "\($0.env?["MODE"] ?? "") \($0.env?["ONLY"] ?? "")" },
            ["dry-run PG106FB", "dry-run PG106Accel", "install PG106FB", "install PG106Accel"])
        XCTAssertEqual(record.ops.count, 1)
    }

    func testAFailingPartUndoesTheWholeGroup() throws {
        let t = tempDir(self)
        makeTestEFI("\(t)/esp", marker: "old")
        write("new", "\(t)/rel/x/PG106.kext/Contents/MacOS/PG106")
        write("<plist/>", "\(t)/rel/x/PG106.kext/Contents/Info.plist")
        var m = try manifestFixture("manifest-next.json")
        let script = "#!/bin/bash\n"
        write(script, "\(t)/rel/install-gpu.sh")
        let i = try XCTUnwrap(m.artifacts.firstIndex { $0.name == "install-gpu.sh" })
        m.artifacts[i].sha256 = Hashing.sha256(Data(script.utf8))
        let unit = UpdatePlanner.unit(id: "group-pg106", title: "g", artifacts: ["PG106.kext", "PG106FB.kext"], manifest: m)
        let (run, _, probes) = fakeMac(self, files: ["/Library/Extensions/PG106FB.kext"])
        run.streamStatus = 1  // the dry run fails: nothing may have changed
        var c = try ctx(t, old: m)
        c.new = m
        XCTAssertThrowsError(try applier(run, probes: probes).apply(unit, c) { _ in })
        XCTAssertEqual(r("\(t)/esp/EFI/OC/Kexts/PG106.kext/Contents/MacOS/PG106"), "kext-old")
        XCTAssertFalse(FileManager.default.fileExists(atPath: "\(t)/esp/EFI/OC/Kexts/PG106.kext.new"))
    }

    func testAMissingTargetWithAKeptCopyIsNotSilentlySkipped() throws {
        let t = tempDir(self)
        makeTestEFI("\(t)/esp", marker: "old")
        write("new", "\(t)/rel/x/PG106.kext/Contents/MacOS/PG106")
        try LocalFileSystem().move("\(t)/esp/EFI/OC/Kexts/PG106.kext", to: "\(t)/esp/EFI/OC/Kexts/PG106.kext.prev-1")
        let m = try manifestFixture("manifest-next.json")
        XCTAssertThrowsError(try applier().apply(pg106, try ctx(t, old: m)) { _ in }) { e in
            XCTAssertEqual(e as? ApplyError, .keptCopyWithoutTarget("EFI/OC/Kexts/PG106.kext"))
        }
    }

    func testAnEarlierKeptCopyIsNeverOverwritten() throws {
        let t = tempDir(self)
        makeTestEFI("\(t)/esp", marker: "old")
        write("another unit's copy", "\(t)/esp/EFI/OC/Kexts/PG106.kext.prev-1/Contents/MacOS/PG106")
        write("a stale partial copy", "\(t)/esp/EFI/OC/Kexts/PG106.kext.new/Contents/MacOS/PG106")
        write("new", "\(t)/rel/x/PG106.kext/Contents/MacOS/PG106")
        let m = try manifestFixture("manifest-next.json")
        let rec = try applier().apply(pg106, try ctx(t, old: m)) { _ in }
        XCTAssertEqual(rec.ops.first?.backup, "EFI/OC/Kexts/PG106.kext.prev-2")
        XCTAssertEqual(r("\(t)/esp/EFI/OC/Kexts/PG106.kext.prev-1/Contents/MacOS/PG106"), "another unit's copy")
        XCTAssertEqual(r("\(t)/esp/EFI/OC/Kexts/PG106.kext/Contents/MacOS/PG106"), "new", "not the stale .new")
    }

    func testRollbackChainGoesBackOneVersionAtATimeAndUndoIsIdempotent() throws {
        let t = tempDir(self)
        makeTestEFI("\(t)/esp", marker: "v1")
        let m = try manifestFixture("manifest-next.json")
        write("v2", "\(t)/rel/x/PG106.kext/Contents/MacOS/PG106")
        let rec1 = try applier().apply(pg106, try ctx(t, old: m)) { _ in }
        try LocalFileSystem().remove("\(t)/rel/x")
        write("v3", "\(t)/rel/x/PG106.kext/Contents/MacOS/PG106")
        let rec2 = try applier().apply(pg106, try ctx(t, old: m)) { _ in }
        let ex = RollbackExecutor(fs: LocalFileSystem(), runner: FakeRunner(), user: UserContext(name: "u", home: t))
        try ex.undo(rec2, esp: "\(t)/esp", output: nil) { _ in }
        XCTAssertEqual(r("\(t)/esp/EFI/OC/Kexts/PG106.kext/Contents/MacOS/PG106"), "v2")
        try ex.undo(rec2, esp: "\(t)/esp", output: nil) { _ in }
        XCTAssertEqual(r("\(t)/esp/EFI/OC/Kexts/PG106.kext/Contents/MacOS/PG106"), "v2", "again: nothing to do")
        try ex.undo(rec1, esp: "\(t)/esp", output: nil) { _ in }
        XCTAssertEqual(r("\(t)/esp/EFI/OC/Kexts/PG106.kext/Contents/MacOS/PG106"), "kext-v1")
    }

    func testUndoRefusesWhenTheKeptCopyIsGoneOrChanged() throws {
        let t = tempDir(self)
        makeTestEFI("\(t)/esp", marker: "v1")
        write("v2", "\(t)/rel/x/PG106.kext/Contents/MacOS/PG106")
        let rec = try applier().apply(pg106, try ctx(t, old: try manifestFixture("manifest-next.json"))) { _ in }
        let ex = RollbackExecutor(fs: LocalFileSystem(), runner: FakeRunner(), user: UserContext(name: "u", home: t))
        write("tampered", "\(t)/esp/EFI/OC/Kexts/PG106.kext.prev-1/Contents/MacOS/PG106")
        XCTAssertThrowsError(try ex.undo(rec, esp: "\(t)/esp", output: nil) { _ in }) { e in
            guard case .backupChanged = e as? RollbackError else { return XCTFail("\(e)") }
        }
        try LocalFileSystem().remove("\(t)/esp/EFI/OC/Kexts/PG106.kext.prev-1")
        XCTAssertThrowsError(try ex.undo(rec, esp: "\(t)/esp", output: nil) { _ in }) { e in
            guard case .backupMissing = e as? RollbackError else { return XCTFail("\(e)") }
        }
        XCTAssertEqual(r("\(t)/esp/EFI/OC/Kexts/PG106.kext/Contents/MacOS/PG106"), "v2", "left alone")
    }

    func testSelfUpdateIsNeverLeftWithoutAnAppAndTheUndoIsJournalledFirst() throws {
        let t = tempDir(self)
        let m = try manifestFixture("manifest-next.json")
        let name = "Helios 300 Setup.app"
        let unit = UpdateUnit(id: name, title: name, kind: .app, artifacts: [name], requiresRestart: false)
        write("app-new", "\(t)/rel/x/\(name)/Contents/MacOS/HeliosSetup")
        let app = "\(t)/Applications/\(name)"
        func appText() -> String { r("\(app)/Contents/MacOS/HeliosSetup") }
        var n = 0
        while true {
            try? FileManager.default.removeItem(atPath: "\(t)/Applications")
            write("app-old", "\(app)/Contents/MacOS/HeliosSetup")
            var c = try ctx(t, old: m)
            c.appPath = app
            let fs = FaultFS(failAt: n, killed: true)
            let faulty = UpdateApplier(
                fs: fs, runner: FakeRunner(), clock: clock, probes: ProbeContext(runner: FakeRunner(), fs: fs))
            var journalled: RollbackRecord?
            let done = try? faulty.apply(unit, c, journal: { journalled = $0 }) { _ in }
            if !fs.fired {
                XCTAssertEqual(appText(), "app-new")
                XCTAssertEqual(done?.ops.first?.backup, "\(app).prev-1")
                break
            }
            if let journalled {
                try RollbackExecutor(fs: LocalFileSystem(), runner: FakeRunner(), user: UserContext(name: "u", home: t)).undo(
                    journalled, esp: nil, output: nil
                ) { _ in }
            }
            XCTAssertEqual(appText(), "app-old", "at failure point \(n)")
            XCTAssertFalse(FileManager.default.fileExists(atPath: "\(app).new"))
            n += 1
        }
        XCTAssertGreaterThan(n, 2)
    }

    func testRemovalUnitIsPlannedAppliedAndUndone() throws {
        let t = tempDir(self)
        makeTestEFI("\(t)/esp", marker: "old")
        var old = try manifestFixture("manifest-next.json")
        var new = old
        new.artifacts.removeAll { $0.name == "PG106.kext" }
        old.release?.tag = "v0.1.0"
        let plan = UpdatePlanner.plan(old: old, new: new, diff: ManifestDiff.between(old, new))
        let unit = try XCTUnwrap(plan.units.first { $0.id == "remove-PG106.kext" })
        XCTAssertTrue(unit.requiresRestart)
        var c = try ctx(t, old: old)
        c.new = new
        var config = try ConfigMerger.read(try Data(contentsOf: URL(fileURLWithPath: "\(t)/esp/EFI/OC/config.plist")))
        var kernel = config["Kernel"] as? [String: Any] ?? [:]
        var add = kernel["Add"] as? [[String: Any]] ?? []
        add.append(["BundlePath": "PG106.kext", "Enabled": true])
        kernel["Add"] = add
        config["Kernel"] = kernel
        try LocalFileSystem().write(try ConfigMerger.write(config), to: "\(t)/esp/EFI/OC/config.plist")
        let before = try Data(contentsOf: URL(fileURLWithPath: "\(t)/esp/EFI/OC/config.plist"))
        let record = try applier().apply(unit, c) { _ in }
        XCTAssertFalse(FileManager.default.fileExists(atPath: "\(t)/esp/EFI/OC/Kexts/PG106.kext"))
        let after = try ConfigMerger.read(try Data(contentsOf: URL(fileURLWithPath: "\(t)/esp/EFI/OC/config.plist")))
        let entries = (after["Kernel"] as? [String: Any])?["Add"] as? [[String: Any]] ?? []
        XCTAssertFalse(entries.contains { ($0["BundlePath"] as? String) == "PG106.kext" }, "the config entry goes with the file")
        try RollbackExecutor(fs: LocalFileSystem(), runner: FakeRunner(), user: UserContext(name: "u", home: t)).undo(
            record, esp: "\(t)/esp", output: nil
        ) { _ in }
        XCTAssertEqual(r("\(t)/esp/EFI/OC/Kexts/PG106.kext/Contents/MacOS/PG106"), "kext-old")
        XCTAssertEqual(try Data(contentsOf: URL(fileURLWithPath: "\(t)/esp/EFI/OC/config.plist")), before)
    }

    func testOnlyKextsActuallyOnTheESPAreMergedIntoConfig() throws {
        let t = tempDir(self)
        makeTestEFI("\(t)/esp", marker: "old")
        write("new", "\(t)/rel/x/PG106.kext/Contents/MacOS/PG106")
        var old = try manifestFixture("manifest-next.json")
        old.configDelta?.kernelAdd = []
        var c = try ctx(t, old: old)
        let group = UpdateUnit(
            id: "group", title: "g", kind: .opencore, artifacts: ["PG106.kext", "NMHDA.kext"], requiresRestart: true)
        let kexts = (c.new.configDelta?.kernelAdd ?? []).filter { ["PG106.kext", "NMHDA.kext"].contains($0.bundlePath) }
        c.new.configDelta?.kernelAdd = kexts
        var log: [String] = []
        _ = try applier().apply(group, c) { log.append($0) }
        XCTAssertFalse(
            log.contains { $0.contains("Kernel/Add/NMHDA") }, "left out of this EFI, so left out of the config: \(log)")
    }
}

// MARK: - both halves

final class RootHardeningTests: RootFixture {
    func staged(_ m: Manifest, _ raw: Data, into dir: String) throws -> StagedRelease {
        try Updater(http: http, fs: LocalFileSystem(), unzipper: CommandUnzipper(), verifier: testVerifier)
            .stage(m, raw: raw, signature: testSign(raw), into: dir, previousDir: nil)
    }

    func apply(_ s: StagedRelease, baseline: String = "none", dir: String? = nil, fs: FileSystem = LocalFileSystem()) -> Int32 {
        RootOps(env: env(fs: fs)).updateApply(
            staged: s.dir, sha: s.manifestSha256, baselineSha: baseline, baselineDir: dir, appPath: nil, output: nil)
    }

    func testAKilledUpdateIsFinishedOnRetryWhereverItStopped() throws {
        let (m, raw) = try fooRelease(version: "0.1.2", payload: "foo-new")
        var n = 0
        while true {
            for d in ["esp", "sys", "rootrun"] { try? FileManager.default.removeItem(atPath: "\(t)/\(d)") }
            espWithOldFoo()
            let s = try staged(m, raw, into: "\(t)/staged")
            let fs = FaultFS(failAt: n, killed: true)
            let rc = apply(s, fs: fs)
            if !fs.fired {
                XCTAssertEqual(rc, 0)
                break
            }
            if rc != 0 { XCTAssertEqual(apply(s), 0, "the retry after stopping at step \(n)") }
            XCTAssertEqual(fooOnESP(), "foo-new", "at step \(n)")
            XCTAssertEqual(read("\(t)/esp/EFI/OC/Kexts/Foo.kext.prev-1/Contents/MacOS/Foo"), "foo-old", "at step \(n)")
            XCTAssertFalse(FileManager.default.fileExists(atPath: "\(t)/esp/EFI/OC/Kexts/Foo.kext.new"), "at step \(n)")
            let state = env().store.loadSystem()
            XCTAssertNil(state.interrupted, "at step \(n)")
            XCTAssertEqual(state.installed?.tag, "v0.1.2", "at step \(n)")
            n += 1
        }
        XCTAssertGreaterThan(n, 10)
    }

    func testAnUnfinishedThirdPartyUpdateIsUndoneFirst() throws {
        espWithOldFoo()
        write("foo-half", "\(t)/esp/EFI/OC/Kexts/Foo.kext.new/Contents/MacOS/Foo")
        var s = SystemState()
        s.interrupted = RollbackRecord(
            unit: "third-party:Foo", fromTag: nil, toTag: "Foo", date: Date(),
            ops: [.remove("EFI/OC/Kexts/Added.kext", onESP: true, staged: "EFI/OC/Kexts/Foo.kext.new")])
        try env().store.saveSystem(s)
        XCTAssertEqual(RootOps(env: env()).rollback(output: nil), 1, "nothing to roll back, but the interrupted unit is undone")
        XCTAssertFalse(FileManager.default.fileExists(atPath: "\(t)/esp/EFI/OC/Kexts/Foo.kext.new"))
        XCTAssertNil(env().store.loadSystem().interrupted)
    }

    func testThirdPartyUpdateKeepsAVersionedCopyAndRollsBack() throws {
        espWithOldFoo()
        let build = "\(t)/dortania"
        write("foo-new", "\(build)/Foo.kext/Contents/MacOS/Foo")
        write("<plist/>", "\(build)/Foo.kext/Contents/Info.plist")
        makeZip(build, top: "Foo.kext", zip: "\(build)/Foo.zip")
        let zip = try Data(contentsOf: URL(fileURLWithPath: "\(build)/Foo.zip"))
        let ops = RootOps(env: env())
        XCTAssertEqual(
            ops.thirdPartyApply(
                product: "Foo", zip: "\(build)/Foo.zip", sha: Hashing.sha256(zip), bundles: ["Foo.kext"], output: nil), 0)
        XCTAssertEqual(fooOnESP(), "foo-new")
        XCTAssertEqual(read("\(t)/esp/EFI/OC/Kexts/Foo.kext.prev-1/Contents/MacOS/Foo"), "foo-old")
        XCTAssertEqual(ops.rollback(output: nil), 0)
        XCTAssertEqual(fooOnESP(), "foo-old")
        XCTAssertEqual(
            ops.thirdPartyApply(
                product: "../x", zip: "\(build)/Foo.zip", sha: Hashing.sha256(zip), bundles: ["Foo.kext"], output: nil), 1)
        XCTAssertEqual(
            ops.thirdPartyApply(
                product: "Foo", zip: "\(build)/Foo.zip", sha: Hashing.sha256(zip), bundles: ["../Foo.kext"], output: nil), 1)
    }

    // MARK: what the manifest may say

    func testATagThatClimbsOutIsRefusedAndTheVictimSurvives() throws {
        let (m0, _) = try fooRelease(version: "0.1.2", payload: "foo-new")
        var m = m0
        write("precious", "\(t)/victim/keep.txt")
        m.release?.tag = "../../victim"
        let raw = try JSONEncoder().encode(m)
        write(String(decoding: raw, as: UTF8.self), "\(t)/staged/manifest.json")
        try LocalFileSystem().write(testSign(raw), to: "\(t)/staged/manifest.json.sig")
        let rc = RootOps(env: env()).updateApply(
            staged: "\(t)/staged", sha: Hashing.sha256(raw), baselineSha: "none", baselineDir: nil, appPath: nil, output: nil)
        XCTAssertEqual(rc, 1)
        XCTAssertEqual(read("\(t)/victim/keep.txt"), "precious")
    }

    func testPathLikeManifestFieldsAreRefused() throws {
        func checked(_ edit: (inout Manifest) -> Void) throws {
            var m = try manifestFixture("manifest-0.1.0.json")
            edit(&m)
            try ManifestCheck.check(m)
        }
        XCTAssertNoThrow(try checked { _ in })
        XCTAssertThrowsError(try checked { $0.release?.tag = "v0.1.0-x" })
        XCTAssertThrowsError(try checked { $0.release?.tag = "../x" })
        for bad in ["../evil.kext", "/etc/passwd", "a/b.kext", "..", "."] {
            XCTAssertThrowsError(try checked { $0.artifacts[0].name = bad }, bad)
        }
        XCTAssertTrue(ManifestCheck.isPathComponent("Helios 300 Setup.app"))
    }

    func testRegexesDoNotAcceptATrailingNewline() {
        XCTAssertFalse(ManifestCheck.isVersion("1.0.0\n"))
        XCTAssertFalse(ManifestCheck.isFileName("a.zip\n"))
        XCTAssertFalse(ManifestCheck.isSha256(String(repeating: "a", count: 64) + "\n"))
        XCTAssertEqual(ReleaseList.versionKey("v1.0.0\n").major, -1)
    }

    // MARK: the plan shown is the plan applied

    func testRootRefusesWhenTheInstalledReleaseMovedSinceThePlan() throws {
        espWithOldFoo()
        let (m, raw) = try fooRelease(version: "0.1.2", payload: "foo-new")
        let s = try staged(m, raw, into: "\(t)/staged")
        XCTAssertEqual(apply(s, baseline: String(repeating: "b", count: 64)), 1)
        XCTAssertEqual(fooOnESP(), "foo-old")
    }

    func testBaselineIsTheSameOnBothSides() throws {
        espWithOldFoo()
        let (base, baseRaw) = try fooRelease(version: "0.1.1", payload: "foo-base")
        write(String(decoding: baseRaw, as: UTF8.self), "\(t)/rel/manifest.json")
        var u = UserState()
        u.releaseDir = "\(t)/rel"
        try env().store.saveUser(u)
        let engine = Engine(env: env())
        let baseline = try XCTUnwrap(engine.updateBaseline())
        XCTAssertEqual(baseline.manifest, base)
        XCTAssertEqual(baseline.sha256, Hashing.sha256(baseRaw))

        let (m, raw) = try fooRelease(version: "0.1.2", payload: "foo-new")
        let s = try staged(m, raw, into: "\(t)/staged")
        XCTAssertEqual(apply(s, baseline: baseline.sha256, dir: baseline.dir), 0, "root records it, then finds it the baseline")
        XCTAssertEqual(env().store.loadSystem().installed?.tag, "v0.1.2")
        XCTAssertEqual(env().store.loadSystem().previous?.tag, "v0.1.1")
    }

    // MARK: the signature

    func testUnsignedOrBadlySignedUpdatesAreRefusedOnBothSides() throws {
        http.pages[ReleaseList.api] = Data(
            #"[{"tag_name": "v0.1.2", "prerelease": true, "body": "", "assets": [{"name": "manifest.json"}]}]"#.utf8)
        espWithOldFoo()
        let engine = Engine(env: env())
        let (m, raw) = try fooRelease(version: "0.1.2", payload: "foo-new") { _ in Data(repeating: 7, count: 64) }
        XCTAssertThrowsError(try engine.checkForUpdates(includePrereleases: true)) { e in
            guard case .invalid = e as? SignatureError else { return XCTFail("\(e)") }
        }
        http.pages[ManifestCheck.urlPrefix + "v0.1.2/manifest.json.sig"] = nil
        XCTAssertThrowsError(try engine.checkForUpdates(includePrereleases: true)) { e in
            guard case .missing = e as? SignatureError else { return XCTFail("\(e)") }
        }
        let s = try staged(m, raw, into: "\(t)/staged")
        try LocalFileSystem().remove("\(t)/staged/manifest.json.sig")
        XCTAssertEqual(apply(s), 1)
        XCTAssertEqual(fooOnESP(), "foo-old")
        try LocalFileSystem().write(Data(repeating: 7, count: 64), to: "\(t)/staged/manifest.json.sig")
        XCTAssertEqual(apply(s), 1, "the root side checks the signature for itself")
        XCTAssertEqual(fooOnESP(), "foo-old")
        http.pages[ManifestCheck.urlPrefix + "v0.1.2/manifest.json.sig"] = testSign(raw)
        XCTAssertNotNil(try engine.checkForUpdates(includePrereleases: true))
    }

    func testTheCommittedKeyIsTheOneCompiledIn() throws {
        let committed = try String(
            contentsOf: repoRoot.appendingPathComponent("tools/release/manifest-ed25519.pub"), encoding: .utf8)
        XCTAssertEqual(committed.trimmingCharacters(in: .whitespacesAndNewlines), ManifestVerifier.releaseKeyHex)
    }

    func testSignManifestScriptProducesWhatTheAppAccepts() throws {
        guard FileManager.default.fileExists(atPath: "/usr/bin/openssl") else { throw XCTSkip("no openssl") }
        write("{}", "\(t)/manifest.json")
        let script = repoRoot.appendingPathComponent("tools/release/sign-manifest.sh").path
        let env = ["HELIOS_SIGNING_KEY": "\(t)/key/sign", "PATH": "/usr/bin:/bin", "HOME": t]
        let runner = ProcessRunner()
        let signed = runner.capture(["/bin/bash", script, "\(t)/manifest.json"], env: env, cwd: nil)
        XCTAssertTrue(signed.ok, signed.stdout + signed.stderr)
        XCTAssertTrue(signed.stderr.contains("NOTICE"), "a new key is announced")
        let mode = try XCTUnwrap(
            FileManager.default.attributesOfItem(atPath: "\(t)/key/sign")[.posixPermissions] as? NSNumber)
        XCTAssertEqual(mode.intValue, 0o600)
        XCTAssertFalse(signed.stdout.contains("verifies against the committed"), "this is not the release key")
        let hex = runner.capture(["/bin/bash", script, "--public"], env: env, cwd: nil).stdout.trimmingCharacters(
            in: .whitespacesAndNewlines)
        let verifier = ManifestVerifier(publicKey: try XCTUnwrap(Data(hex: hex)))
        let sig = try Data(contentsOf: URL(fileURLWithPath: "\(t)/manifest.json.sig"))
        XCTAssertNoThrow(try verifier.verify(Data("{}".utf8), signature: sig, what: "test"))
        XCTAssertThrowsError(try verifier.verify(Data("{} ".utf8), signature: sig, what: "test"))
    }

    // MARK: root runs no user-writable code

    func testScriptsRunFromAVerifiedCopyAndSymlinksAreRefused() throws {
        let fs = LocalFileSystem()
        let script = "#!/bin/bash\n"
        write(script, "\(t)/res/install-gpu.sh")
        var m = try manifestFixture("manifest-next.json")
        let i = try XCTUnwrap(m.artifacts.firstIndex { $0.name == "install-gpu.sh" })
        m.artifacts[i].sha256 = Hashing.sha256(Data(script.utf8))
        let scratch = try RootScratch.make(fs: fs, parent: "\(t)/rootrun")
        defer { scratch.remove() }
        let copy = try ScriptVerifier.verifiedCopy(
            of: "install-gpu.sh", scriptsDir: "\(t)/res", manifest: m, fs: fs, scratch: scratch)
        XCTAssertTrue(copy.hasPrefix(scratch.dir))
        let mode = try XCTUnwrap(FileManager.default.attributesOfItem(atPath: scratch.dir)[.posixPermissions] as? NSNumber)
        XCTAssertEqual(mode.intValue, 0o700)
        m.artifacts[i].sha256 = String(repeating: "a", count: 64)
        XCTAssertThrowsError(
            try ScriptVerifier.verifiedCopy(of: "install-gpu.sh", scriptsDir: "\(t)/res", manifest: m, fs: fs, scratch: scratch))
        try FileManager.default.createSymbolicLink(atPath: "\(t)/res/gpu-apps.sh", withDestinationPath: "\(t)/res/install-gpu.sh")
        XCTAssertThrowsError(try scratch.copy("\(t)/res/gpu-apps.sh")) { e in
            XCTAssertEqual(e as? RootScratchError, .symlink("\(t)/res/gpu-apps.sh"))
        }
    }

    func testBootstrapRunsAPrivateCheckedCopyAndRefusesAChangedBinary() throws {
        let cli = "\(t)/cli/helios-setup"
        write("#!/bin/sh\necho \"args: $*\"\nexit 3\n", cli)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: cli)
        let sha = try XCTUnwrap(Hashing.sha256(file: cli, fs: LocalFileSystem()))
        let run = "\(t)/rootrun"
        try LocalFileSystem().makeDirectory(run)
        // shasum is on every Mac; on Linux the same check with sha256sum.
        let script = RootBootstrap.script.replacingOccurrences(of: "/usr/bin/shasum -a 256", with: "/usr/bin/sha256sum")
        func go(_ want: String) -> CommandResult {
            var argv = RootBootstrap.argv(cli: cli, sha256: want, runDir: run, arguments: ["_root", "it's; a test"])
            argv[3] = script
            return ProcessRunner().capture(argv, env: nil, cwd: nil)
        }
        let ok = go(sha)
        XCTAssertEqual(ok.status, 3, ok.stderr)
        XCTAssertEqual(ok.stdout, "args: _root it's; a test\n")
        let bad = go(String(repeating: "0", count: 64))
        XCTAssertEqual(bad.status, 126)
        XCTAssertTrue(bad.stderr.contains("changed after it was checked"))
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: run), [], "the private copy is gone")
        try FileManager.default.createSymbolicLink(atPath: "\(t)/cli/link", withDestinationPath: cli)
        var linked = RootBootstrap.argv(cli: "\(t)/cli/link", sha256: sha, runDir: run, arguments: [])
        linked[3] = script
        XCTAssertEqual(ProcessRunner().capture(linked, env: nil, cwd: nil).status, 126)
    }

    func testLogsNeverFollowALink() throws {
        write("do not touch", "\(t)/victim")
        try FileManager.default.createSymbolicLink(atPath: "\(t)/log", withDestinationPath: "\(t)/victim")
        LogSink(path: "\(t)/log")("line")
        XCTAssertEqual(read("\(t)/victim"), "do not touch")
        XCTAssertEqual(ProcessRunner().stream(["/bin/echo", "hi"], env: nil, cwd: nil, output: "\(t)/log"), 126)
        XCTAssertEqual(read("\(t)/victim"), "do not touch")
    }

    func testACopiedReleaseRefusesSymlinks() throws {
        let (m, raw) = try fooRelease(version: "0.1.2", payload: "foo-new")
        let s = try staged(m, raw, into: "\(t)/staged")
        try LocalFileSystem().remove("\(t)/staged/Foo.kext.zip")
        try FileManager.default.createSymbolicLink(atPath: "\(t)/staged/Foo.kext.zip", withDestinationPath: "\(t)/res/x")
        espWithOldFoo()
        XCTAssertEqual(apply(s), 1)
        XCTAssertEqual(fooOnESP(), "foo-old")
    }

    func testAnAppPathThatIsNotAnAppBundleIsRefused() throws {
        espWithOldFoo()
        let (m, raw) = try fooRelease(version: "0.1.2", payload: "foo-new")
        let s = try staged(m, raw, into: "\(t)/staged")
        for bad in ["\(t)/esp", "/Applications/../etc.app", "relative.app"] {
            XCTAssertEqual(
                RootOps(env: env()).updateApply(
                    staged: s.dir, sha: s.manifestSha256, baselineSha: "none", baselineDir: nil, appPath: bad, output: nil), 1,
                bad)
        }
        XCTAssertEqual(fooOnESP(), "foo-old")
    }

    func testOnlyOneRootJobAtATime() throws {
        let path = "\(t)/lock"
        let first = try XCTUnwrap(RootLock.acquire(at: path))
        XCTAssertNil(RootLock.acquire(at: path), "held")
        close(first)
        let again = try XCTUnwrap(RootLock.acquire(at: path), "released when the holder exits")
        close(again)
        try FileManager.default.createSymbolicLink(atPath: "\(t)/link", withDestinationPath: path)
        XCTAssertNil(RootLock.acquire(at: "\(t)/link"), "never through a link")
    }

    // MARK: rollback

    func testRollbackOnlyMovesTheBaselineWithTheReleasesLastRecord() throws {
        var s = SystemState()
        s.installed = InstalledRelease(
            tag: "v0.1.2", version: "0.1.2", manifestSha256: "a", releaseDir: "", date: Date(), preRelease: true)
        s.previous = InstalledRelease(
            tag: "v0.1.1", version: "0.1.1", manifestSha256: "b", releaseDir: "", date: Date(), preRelease: true)
        s.rollbacks = [
            RollbackRecord(unit: "a", fromTag: "v0.1.1", toTag: "v0.1.2", date: Date()),
            RollbackRecord(unit: "b", fromTag: "v0.1.1", toTag: "v0.1.2", date: Date()),
        ]
        try env().store.saveSystem(s)
        let ops = RootOps(env: env())
        XCTAssertEqual(ops.rollback(output: nil), 0)
        XCTAssertEqual(env().store.loadSystem().installed?.tag, "v0.1.2", "one record of that release is left")
        XCTAssertEqual(ops.rollback(output: nil), 1, "one driver change per restart")
        // the next boot
        runner.set("/usr/sbin/sysctl -n kern.bootsessionuuid", "BOOT-B\n")
        XCTAssertEqual(ops.rollback(output: nil), 0)
        XCTAssertEqual(env().store.loadSystem().installed?.tag, "v0.1.1", "its last record: the baseline moves back")
    }

    func testRollbackRespectsTheRestartGuard() throws {
        var s = SystemState()
        s.rollbacks = [RollbackRecord(unit: "Foo.kext", fromTag: nil, toTag: "v0.1.2", date: Date())]
        s.record(
            ChangeRecord(
                item: "gpu-fb", action: "install", bootSession: "BOOT-A", date: Date(), driverChange: true, needsRestart: true))
        try env().store.saveSystem(s)
        XCTAssertEqual(RootOps(env: env()).rollback(output: nil), 1)
        s.record(
            ChangeRecord(
                item: "update:Foo.kext", action: "update", bootSession: "BOOT-A", date: Date(), driverChange: true,
                needsRestart: true))
        try env().store.saveSystem(s)
        XCTAssertEqual(RootOps(env: env()).rollback(output: nil), 0, "taking back this boot's own change")
    }

    func testKeptCopiesAreBoundedToTheNewestReleases() throws {
        let ops = RootOps(env: env())
        var s = SystemState()
        espWithOldFoo()
        for v in 1...5 {
            let backup = "EFI/OC/Kexts/Foo.kext.prev-\(v)"
            write("kept", "\(t)/esp/\(backup)/x")
            s.rollbacks.append(
                RollbackRecord(
                    unit: "Foo", fromTag: nil, toTag: "v0.1.\(v)", date: Date(),
                    ops: [.restore("EFI/OC/Kexts/Foo.kext", from: backup, onESP: true)]))
        }
        ops.pruneRollbacks(&s, log: LogSink(path: "\(t)/log"))
        XCTAssertEqual(s.rollbacks.map(\.toTag), ["v0.1.3", "v0.1.4", "v0.1.5"])
        XCTAssertFalse(FileManager.default.fileExists(atPath: "\(t)/esp/EFI/OC/Kexts/Foo.kext.prev-1"))
        XCTAssertTrue(FileManager.default.fileExists(atPath: "\(t)/esp/EFI/OC/Kexts/Foo.kext.prev-3"))
    }

    // MARK: smaller findings

    func testPendingMatchesTagAndSha() throws {
        var s = SystemState()
        s.pending = PendingUpdate(tag: "v0.1.2", manifestSha256: "other", dir: "", applied: ["unit"])
        try env().store.saveSystem(s)
        espWithOldFoo()
        let (m, raw) = try fooRelease(version: "0.1.2", payload: "foo-new")
        XCTAssertEqual(apply(try staged(m, raw, into: "\(t)/staged")), 1, "the same tag with other bytes is another update")
        XCTAssertEqual(fooOnESP(), "foo-old")
    }

    func testARunningMacOSOutsideTheManifestRangeIsRefused() throws {
        http.pages[ReleaseList.api] = Data(
            #"[{"tag_name": "v0.1.2", "prerelease": true, "body": "", "assets": [{"name": "manifest.json"}]}]"#.utf8)
        _ = try fooRelease(version: "0.1.2", payload: "foo-new")
        runner.set("/usr/sbin/sysctl -n kern.osrelease", "24.6.0\n")
        XCTAssertThrowsError(try Engine(env: env()).checkForUpdates(includePrereleases: true)) { e in
            XCTAssertEqual(e as? UpdateError, .notForThisMacOS("v0.1.2", "24.6.0"))
        }
    }

    func testCorruptStateIsToldApartFromMissingState() throws {
        let store = env().store
        XCTAssertEqual(try store.readSystem(), SystemState(), "missing is empty")
        write("not a plist", store.systemPath)
        XCTAssertThrowsError(try store.readSystem()) { e in
            guard case .corrupt = e as? StateError else { return XCTFail("\(e)") }
        }
        let (m, raw) = try fooRelease(version: "0.1.2", payload: "foo-new")
        espWithOldFoo()
        XCTAssertEqual(apply(try staged(m, raw, into: "\(t)/staged")), 1, "a corrupt state is not overwritten with an empty one")
        XCTAssertEqual(read(store.systemPath), "not a plist")
        XCTAssertNotNil(Engine(env: env()).overview().stateProblem)
    }

    func testAStateFromAnOlderBuildStillReads() throws {
        let store = env().store
        let older = try PropertyListSerialization.data(
            fromPropertyList: ["efiBackups": ["EFI-backup-1"]], format: .xml, options: 0)
        try LocalFileSystem().write(older, to: store.systemPath)
        XCTAssertEqual(try store.readSystem().efiBackups, ["EFI-backup-1"])
        XCTAssertNil(try store.readSystem().interrupted)
    }

    func testForbiddenDevicePropertiesAreReported() throws {
        var delta = ConfigDelta()
        delta.forbidden = Forbidden(deviceProperties: ["PciRoot(0x0)": ["disable-gpu"]])
        let config: [String: Any] = ["DeviceProperties": ["Add": ["PciRoot(0x0)": ["disable-gpu": Data([1])]]]]
        let (_, changes) = ConfigMerger.merge(config, delta: delta)
        XCTAssertTrue(changes.contains { $0.kind == .warning && $0.path.contains("disable-gpu") })
        let m = try manifestFixture("manifest-next.json")
        XCTAssertNotNil(m.config?.forbidden?.deviceProperties, "and the release's own list decodes")
    }

    func testZipStrayEntriesAreRefusedAs1401Does() throws {
        let work = tempDir(self)
        write("binary", "\(work)/Foo.kext/Contents/MacOS/Foo")
        write("stray", "\(work)/stray.txt")
        let r = ProcessRunner().capture(
            ["/usr/bin/zip", "-q", "-r", "-X", "\(work)/a.zip", "Foo.kext", "stray.txt"], env: nil, cwd: work)
        XCTAssertTrue(r.ok, r.stderr)
        var a = try manifestFixture("manifest-0.1.0.json").artifacts[0]
        a.name = "Foo.kext"
        a.file = "a.zip"
        let names = try ZipListing.names(try Data(contentsOf: URL(fileURLWithPath: "\(work)/a.zip")), file: "a.zip")
        XCTAssertTrue(names.contains("stray.txt"))
        XCTAssertThrowsError(try ManifestCheck.checkZipEntries(names, artifact: a)) { e in
            XCTAssertEqual(e as? VerifyError, .strayEntry("a.zip", "stray.txt"))
        }
        XCTAssertThrowsError(try ManifestCheck.checkZipEntries(["Foo.kext/../../x"], artifact: a))
        XCTAssertThrowsError(try ManifestCheck.checkZipEntries(["/Foo.kext/x"], artifact: a))
        XCTAssertNoThrow(try ManifestCheck.checkZipEntries(["Foo.kext/", "Foo.kext/Contents/x"], artifact: a))
        XCTAssertThrowsError(try ZipListing.names(Data("no zip here".utf8), file: "a.zip"))
    }

    func testResumeOnlyContinuesAStartedUpdateAll() throws {
        let store = env().store
        let engine = Engine(env: env())
        XCTAssertFalse(engine.shouldResumeUpdate(arguments: ["--resume"]))
        var s = SystemState()
        s.pending = PendingUpdate(tag: "v0.1.2", manifestSha256: "a", dir: "")
        try store.saveSystem(s)
        XCTAssertFalse(engine.shouldResumeUpdate(arguments: ["--resume"]), "the user did not ask for Update all")
        var u = UserState()
        u.resumeAfterLogin = true
        try store.saveUser(u)
        XCTAssertTrue(engine.shouldResumeUpdate(arguments: ["--resume"]))
        XCTAssertFalse(engine.shouldResumeUpdate(arguments: []))
    }

    func testInstallAppReplacesTheBundleAndRelaunchUsesTheInjectedRunner() throws {
        let fs = LocalFileSystem()
        write("new", "\(t)/stick/App.app/Contents/MacOS/App")
        write("old", "\(t)/Applications/App.app/Contents/MacOS/App")
        let engine = Engine(env: env(appPath: "\(t)/Applications/App.app"))
        try engine.installApp(from: "\(t)/stick/App.app", to: "\(t)/Applications/App.app")
        XCTAssertEqual(read("\(t)/Applications/App.app/Contents/MacOS/App"), "new")
        XCTAssertFalse(fs.exists("\(t)/Applications/App.app.prev-1"))
        XCTAssertFalse(fs.exists("\(t)/Applications/App.app.new"))
        XCTAssertTrue(engine.relaunch())
        XCTAssertTrue(runner.calls.contains(["/usr/bin/open", "-n", "\(t)/Applications/App.app"]))
    }
}
