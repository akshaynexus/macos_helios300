// SPDX-License-Identifier: GPL-2.0-only
//
// The two halves end to end, with fakes: Engine (user side) -> PrivilegedRunner, and RootOps (root side) for a
// step, an update (download -> apply -> rollback) on a fake internal ESP.
import XCTest

@testable import HeliosCore

/// A fake Mac with a real file system under a temp directory, shared by the tests that drive both halves.
class RootFixture: XCTestCase {
    var t = ""
    var runner = FakeRunner()
    var privileged = FakePrivileged()
    var http = FakeHTTP()

    func env(fs: FileSystem = LocalFileSystem(), appPath: String? = nil) -> Environment {
        var e = Environment(
            runner: runner, fs: fs, http: http, clock: FakeClock(now: Date(timeIntervalSince1970: 1_762_560_000)),
            privileged: privileged, unzipper: CommandUnzipper(), user: UserContext(name: "alex", home: "\(t)/home"),
            resourcesDir: "\(t)/res", cliPath: "\(t)/app/helios-setup", appPath: appPath)
        e.systemStatePath = "\(t)/sys/state.plist"
        e.releasesDir = "\(t)/sys/releases"
        e.rootRunDir = "\(t)/rootrun"
        e.manifestVerifier = testVerifier
        return e
    }

    override func setUp() {
        t = tempDir(self)
        runner = FakeRunner()
        privileged = FakePrivileged()
        http = FakeHTTP()
        runner.set("/usr/sbin/sysctl -n kern.bootsessionuuid", "BOOT-A\n")
        runner.set("/usr/sbin/sysctl -n kern.osrelease", "25.6.0\n")
        runner.set("/usr/sbin/diskutil info -plist /", String(decoding: fixtureData("diskutil-info-root.plist"), as: UTF8.self))
        write("the cli binary", "\(t)/app/helios-setup")
        runner.set(
            "/usr/sbin/diskutil list -plist internal physical",
            String(decoding: fixtureData("diskutil-list-internal.plist"), as: UTF8.self))
        let info = try? PropertyListSerialization.data(
            fromPropertyList: [
                "DeviceIdentifier": "disk0s1", "DiskUUID": "8E2D1F5A-3C4B-4D5E-8F90-A1B2C3D4E5F6",
                "MountPoint": "\(t)/esp",
            ], format: .xml, options: 0)
        runner.set("/usr/sbin/diskutil info -plist disk0s1", String(decoding: info ?? Data(), as: UTF8.self))
        runner.fallback = { _ in CommandResult(status: 0) }  // ditto is absent on Linux; other probes answer empty
    }

    /// The release folder: manifest-next.json with install-gpu.sh replaced by a stub the app carries too.
    func releaseFolder() throws -> String {
        let script = "#!/bin/bash\n# stub\n"
        write(script, "\(t)/res/install-gpu.sh")
        var m = try manifestFixture("manifest-next.json")
        if let i = m.artifacts.firstIndex(where: { $0.name == "install-gpu.sh" }) {
            m.artifacts[i].sha256 = Hashing.sha256(Data(script.utf8))
        }
        try LocalFileSystem().write(try JSONEncoder().encode(m), to: "\(t)/rel/manifest.json")
        return "\(t)/rel"
    }

    /// A real (non dry-run) one-kext release, Foo.kext, OpenCore-injected, its zip and manifest served by FakeHTTP.
    func fooRelease(version: String, payload: String, signature: ((Data) -> Data)? = nil) throws -> (Manifest, Data) {
        let src = "\(t)/build-\(version)"
        write("<plist/>", "\(src)/Foo.kext/Contents/Info.plist")
        write(payload, "\(src)/Foo.kext/Contents/MacOS/Foo")
        makeZip(src, top: "Foo.kext", zip: "\(src)/Foo.kext.zip")
        let zip = try Data(contentsOf: URL(fileURLWithPath: "\(src)/Foo.kext.zip"))
        var m = try manifestFixture("manifest-0.1.0.json")
        m.release?.version = version
        m.release?.tag = "v\(version)"
        var a = m.artifacts[0]
        a.name = "Foo.kext"
        a.file = "Foo.kext.zip"
        a.size = zip.count
        a.sha256 = Hashing.sha256(zip)
        a.url = ManifestCheck.urlPrefix + "v\(version)/Foo.kext.zip"
        a.contentsSha256 = [
            "Contents/Info.plist": Hashing.sha256(Data("<plist/>".utf8)),
            "Contents/MacOS/Foo": Hashing.sha256(Data(payload.utf8)),
        ]
        a.opencore = OpenCoreKext(
            order: 100, bundlePath: "Foo.kext", executablePath: "Contents/MacOS/Foo",
            plistPath: "Contents/Info.plist", arch: "x86_64", minKernel: "25.0.0", maxKernel: "",
            enabled: true, comment: "Foo", requires: [])
        a.requiresRestart = true
        m.artifacts = [a]
        m.configDelta = ConfigDelta(kernelAdd: [a.opencore].compactMap { $0 })
        let raw = try JSONEncoder().encode(m)
        http.pages[a.url ?? ""] = zip
        http.pages[ManifestCheck.urlPrefix + "v\(version)/manifest.json"] = raw
        http.pages[ManifestCheck.urlPrefix + "v\(version)/manifest.json.sig"] = (signature ?? testSign)(raw)
        return (m, raw)
    }

    func espWithOldFoo() {
        write(String(decoding: fixtureData("config-internal.plist"), as: UTF8.self), "\(t)/esp/EFI/OC/config.plist")
        write("foo-old", "\(t)/esp/EFI/OC/Kexts/Foo.kext/Contents/MacOS/Foo")
        write("<plist/>", "\(t)/esp/EFI/OC/Kexts/Foo.kext/Contents/Info.plist")
    }

    func read(_ path: String) -> String { (try? String(contentsOfFile: path, encoding: .utf8)) ?? "<MISSING>" }

    func fooOnESP() -> String { read("\(t)/esp/EFI/OC/Kexts/Foo.kext/Contents/MacOS/Foo") }
}

final class EngineTests: RootFixture {
    // MARK: steps

    func testRootSideRecordsTheChangeFirstAndRefusesASecondDriverChange() throws {
        let rel = try releaseFolder()
        let ops = RootOps(env: env())
        XCTAssertEqual(ops.step("gpu-fb", "install", releaseDir: rel, efiSource: nil, output: nil), 0)
        let run = try XCTUnwrap(runner.streamed.last)
        XCTAssertEqual(run.argv.first, "/bin/bash")
        XCTAssertEqual(run.argv.count, 2)
        XCTAssertTrue(run.argv[1].hasPrefix("\(t)/rootrun/helios-setup."), "a root-owned copy, not \(run.argv[1])")
        XCTAssertTrue(run.argv[1].hasSuffix("/install-gpu.sh"))
        XCTAssertEqual(run.env?["MODE"], "install")
        XCTAssertEqual(run.env?["ONLY"], "PG106FB")
        XCTAssertEqual(run.cwd, rel)
        let s = env().store.loadSystem()
        XCTAssertEqual(s.changes.map(\.item), ["gpu-fb"])
        XCTAssertNil(s.installed, "a dry-run manifest is never adopted as the installed release")
        let before = runner.streamed.count
        XCTAssertEqual(ops.step("gpu-accel", "install", releaseDir: rel, efiSource: nil, output: nil), 1)
        XCTAssertEqual(runner.streamed.count, before, "refused before anything ran")
        XCTAssertEqual(ops.step("gpu-accel", "check", releaseDir: rel, efiSource: nil, output: nil), 0, "a dry run is fine")
    }

    func testRootSideRefusesAScriptThatIsNotTheRelease() throws {
        let rel = try releaseFolder()
        write("#!/bin/bash\n# changed\n", "\(t)/res/install-gpu.sh")
        XCTAssertEqual(RootOps(env: env()).step("gpu-fb", "check", releaseDir: rel, efiSource: nil, output: nil), 1)
        XCTAssertTrue(runner.streamed.isEmpty)
    }

    func testUserSideNeedsTheDryRunThenGoesThroughThePrivilegedRunner() throws {
        let rel = try releaseFolder()
        let engine = Engine(env: env())
        XCTAssertThrowsError(try engine.perform("gpu-fb", "install", explicit: rel)) { e in
            guard case .blocked(let why) = e as? EngineError else { return XCTFail("\(e)") }
            XCTAssertTrue(why.contains("Check"))
        }
        XCTAssertTrue(privileged.calls.isEmpty)
        XCTAssertEqual(try engine.perform("gpu-fb", "check", explicit: rel), 0)
        XCTAssertEqual(try engine.perform("gpu-fb", "install", explicit: rel), 0)
        let argv = try XCTUnwrap(privileged.calls.last)
        XCTAssertEqual(Array(argv.prefix(3)), ["/bin/sh", "-p", "-c"])
        XCTAssertEqual(
            Array(argv.dropFirst(4).prefix(4)),
            ["helios-setup-bootstrap", "\(t)/app/helios-setup", Hashing.sha256(Data("the cli binary".utf8)), "\(t)/rootrun"])
        XCTAssertEqual(Array(argv.dropFirst(8).prefix(3)), ["_root", "--user", "alex"])
        XCTAssertTrue(argv.contains("install") && argv.contains("gpu-fb") && argv.contains(rel))
        XCTAssertEqual(
            argv[(argv.firstIndex(of: "--resources") ?? 0) + 1], "\(t)/res", "the root copy still finds the app's scripts")
        let u = env().store.loadUser()
        XCTAssertEqual(u.runs["gpu-fb"]?.action, "install")
        XCTAssertEqual(u.checks["gpu-fb"]?.status, 0)
    }

    func testCancelledPromptIsReported() throws {
        let rel = try releaseFolder()
        privileged.status = privilegedCancelled
        XCTAssertThrowsError(try Engine(env: env()).perform("gpu-fb", "check", explicit: rel)) { e in
            XCTAssertEqual(e as? EngineError, .cancelled)
        }
    }

    func testOverviewWithoutAReleaseIsReadOnly() {
        write(
            String(
                decoding: (try? Data(contentsOf: repoRoot.appendingPathComponent("tools/release/spec.json"))) ?? Data(),
                as: UTF8.self),
            "\(t)/res/spec.json")
        let o = Engine(env: env()).overview()
        XCTAssertFalse(o.release.canAct)
        XCTAssertEqual(o.release.source, .embeddedSpec)
        XCTAssertEqual(o.items.first?.id, "opencore")
        XCTAssertThrowsError(try Engine(env: env()).perform("gpu-fb", "check"))
    }

    // MARK: updates

    func testUpdateDownloadApplyAndRollBack() throws {
        espWithOldFoo()
        let (m, raw) = try fooRelease(version: "0.1.2", payload: "foo-new")
        http.pages[ReleaseList.api] = Data(
            #"[{"tag_name": "v0.1.2", "prerelease": true, "body": "Foo fix", "assets": [{"name": "manifest.json"}]}]"#.utf8)
        let engine = Engine(env: env())
        let offer = try XCTUnwrap(try engine.checkForUpdates(includePrereleases: true))
        XCTAssertEqual(offer.candidate.tag, "v0.1.2")
        XCTAssertEqual(offer.manifest, m)
        XCTAssertTrue(offer.changelog.contains("Foo fix"))
        let staged = try engine.download(offer)
        XCTAssertEqual(staged.manifestSha256, Hashing.sha256(raw))

        let ops = RootOps(env: env())
        XCTAssertEqual(
            ops.updateApply(
                staged: staged.dir, sha: staged.manifestSha256, baselineSha: "none", baselineDir: nil, appPath: nil, output: nil),
            0)
        XCTAssertEqual(
            try String(contentsOfFile: "\(t)/esp/EFI/OC/Kexts/Foo.kext/Contents/MacOS/Foo", encoding: .utf8), "foo-new")
        XCTAssertEqual(
            try String(contentsOfFile: "\(t)/esp/EFI/OC/Kexts/Foo.kext.prev-1/Contents/MacOS/Foo", encoding: .utf8), "foo-old")
        var s = env().store.loadSystem()
        XCTAssertEqual(s.installed?.tag, "v0.1.2")
        XCTAssertEqual(s.installed?.manifestSha256, Hashing.sha256(raw))
        XCTAssertEqual(s.rollbacks.count, 1)
        XCTAssertTrue(
            FileManager.default.fileExists(atPath: "\(t)/sys/releases/v0.1.2/Foo.kext.zip"), "kept root-owned for rollback")

        XCTAssertEqual(
            ops.updateApply(
                staged: staged.dir, sha: "0" + staged.manifestSha256.dropFirst(), baselineSha: "none", baselineDir: nil,
                appPath: nil,
                output: nil), 1,
            "a manifest that is not the checked one is refused")

        XCTAssertEqual(ops.rollback(output: nil), 0)
        XCTAssertEqual(
            try String(contentsOfFile: "\(t)/esp/EFI/OC/Kexts/Foo.kext/Contents/MacOS/Foo", encoding: .utf8), "foo-old")
        s = env().store.loadSystem()
        XCTAssertTrue(s.rollbacks.isEmpty)
        XCTAssertEqual(s.changes.map(\.item), ["update:Foo.kext", "rollback:Foo.kext"])
        XCTAssertEqual(ops.rollback(output: nil), 1, "nothing left to roll back")
    }

    func testUpdateRefusesASecondDriverChangeInTheSameBoot() throws {
        espWithOldFoo()
        let (m, raw) = try fooRelease(version: "0.1.2", payload: "foo-new")
        var s = SystemState()
        s.record(
            ChangeRecord(
                item: "gpu-fb", action: "install", bootSession: "BOOT-A", date: Date(), driverChange: true, needsRestart: true))
        try env().store.saveSystem(s)
        let staged = try Updater(http: http, fs: LocalFileSystem(), unzipper: CommandUnzipper())
            .stage(m, raw: raw, signature: testSign(raw), into: "\(t)/staged", previousDir: nil)
        XCTAssertEqual(
            RootOps(env: env()).updateApply(
                staged: staged.dir, sha: staged.manifestSha256, baselineSha: "none", baselineDir: nil, appPath: nil, output: nil),
            1)
        XCTAssertEqual(
            try String(contentsOfFile: "\(t)/esp/EFI/OC/Kexts/Foo.kext/Contents/MacOS/Foo", encoding: .utf8), "foo-old")
    }

    func testNoNewerReleaseIsNil() throws {
        http.pages[ReleaseList.api] = Data("[]".utf8)
        XCTAssertNil(try Engine(env: env()).checkForUpdates(includePrereleases: true))
    }
}
