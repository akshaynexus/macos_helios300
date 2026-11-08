// SPDX-License-Identifier: GPL-2.0-only
import XCTest

@testable import HeliosCore

final class UpdateTests: XCTestCase {
    // MARK: release list

    func testReleaseListSkipsDraftsAndReleasesWithoutAManifest() {
        let list = ReleaseList.parse(fixtureData("github-releases.json"))
        XCTAssertEqual(list.map(\.tag), ["v0.1.1", "v0.1.0", "v0.0.9"])
        XCTAssertEqual(list.first?.notes, "Wi-Fi TX fix; Setup.app")
    }

    func testNewerHonoursThePrereleaseChoice() {
        let list = ReleaseList.parse(fixtureData("github-releases.json"))
        XCTAssertEqual(ReleaseList.newer(list, than: "v0.1.0", includePrereleases: true)?.tag, "v0.1.1")
        XCTAssertNil(ReleaseList.newer(list, than: "v0.1.0", includePrereleases: false), "v0.0.9 is older")
        XCTAssertNil(ReleaseList.newer(list, than: "v0.1.1", includePrereleases: true))
        XCTAssertEqual(ReleaseList.newer(list, than: nil, includePrereleases: false)?.tag, "v0.0.9")
    }

    func testVersionKeyAs1401() {
        XCTAssertTrue(ReleaseList.isNewer("v0.1.1", than: "v0.1.0"))
        XCTAssertTrue(ReleaseList.isNewer("v0.2.0", than: "v0.2.0-rc1"), "a final release beats its pre-release")
        XCTAssertFalse(ReleaseList.isNewer("v0.1.10-rc1", than: "v0.1.10"))
        XCTAssertTrue(ReleaseList.isNewer("v0.1.10", than: "v0.1.9"))
        XCTAssertEqual(ReleaseList.versionKey("nonsense").major, -1)
    }

    // MARK: diff and plan (real 0.1.0 -> the current make_manifest output)

    func testDiffRealReleases() throws {
        let old = try manifestFixture("manifest-0.1.0.json"), new = try manifestFixture("manifest-next.json")
        let d = ManifestDiff.between(old, new)
        XCTAssertTrue(d.names(.added).contains("Helios 300 Setup.app"))
        XCTAssertTrue(d.names(.added).contains("install-gpu.sh"))
        XCTAssertTrue(d.names(.changed).contains("Ath10kMac.kext"), "the fixture bakes the Wi-Fi country into 0.1.0's plist")
        XCTAssertFalse(d.names(.changed).contains("PG106.kext"), "same bytes, same entry")
        XCTAssertTrue(d.names(.changed).contains("PG106FB.kext"), "0.1.0 had it as an OpenCore kext; now /Library/Extensions")
        XCTAssertTrue(d.configChanged, "0.1.0 had no config_delta")
        XCTAssertTrue(d.changelog(notes: "notes", from: "v0.1.0", to: "v0.1.1").contains("+ Helios 300 Setup.app"))
    }

    func manifests(oldABI: String?, newABI: String?, changed: [String]) throws -> (Manifest, Manifest) {
        var new = try manifestFixture("manifest-next.json")
        var old = new
        old.release?.tag = "v0.1.0"
        if let i = old.updateGroups?.firstIndex(where: { $0.name == "pg106" }) { old.updateGroups?[i].abi = oldABI }
        if let i = new.updateGroups?.firstIndex(where: { $0.name == "pg106" }) { new.updateGroups?[i].abi = newABI }
        for n in changed {
            if let i = new.artifacts.firstIndex(where: { $0.name == n }) {
                new.artifacts[i].sha256 = String(repeating: "e", count: 64)
            }
        }
        return (old, new)
    }

    func testWiFiPairIsOneUnit() throws {
        let (old, new) = try manifests(oldABI: "1.0", newABI: "1.0", changed: ["Ath10kMac.kext", "AirportAth10k.kext"])
        let plan = UpdatePlanner.plan(old: old, new: new, diff: ManifestDiff.between(old, new))
        XCTAssertEqual(plan.units.map(\.id), ["group-wifi"])
        XCTAssertEqual(plan.units.first?.artifacts, ["Ath10kMac.kext", "AirportAth10k.kext"])
        XCTAssertEqual(plan.batches.count, 1)
    }

    func testPG106SetTogetherOnlyWhenTheABIChanges() throws {
        let names = ["PG106.kext", "PG106FB.kext", "PG106Accel.kext"]
        let (o1, n1) = try manifests(oldABI: "1.0", newABI: "1.0", changed: names)
        let same = UpdatePlanner.plan(old: o1, new: n1, diff: ManifestDiff.between(o1, n1))
        XCTAssertEqual(same.batches.count, 3, "same ABI: one restart each")
        XCTAssertEqual(
            same.batches.map { $0.map(\.id) }, [["PG106.kext"], ["PG106FB.kext"], ["PG106Accel.kext"]],
            "OpenCore first, then /Library/Extensions in manifest order")
        let (o2, n2) = try manifests(oldABI: "1.0", newABI: "2.0", changed: names)
        let abi = UpdatePlanner.plan(old: o2, new: n2, diff: ManifestDiff.between(o2, n2))
        XCTAssertEqual(abi.batches.map { $0.map(\.id) }, [["group-pg106"]])
        XCTAssertEqual(abi.units.first?.kind, .opencore)
    }

    func testNoRestartUnitsFormTheFirstBatchAppFirst() throws {
        let (old, new) = try manifests(
            oldABI: "1.0", newABI: "1.0",
            changed: ["install-gpu.sh", "Helios 300 Setup.app", "NVK-Pascal", "QCARomePatchRAM.kext"])
        let plan = UpdatePlanner.plan(old: old, new: new, diff: ManifestDiff.between(old, new))
        XCTAssertEqual(
            plan.batches.map { $0.map(\.id) },
            [["Helios 300 Setup.app", "NVK-Pascal", "install-gpu.sh"], ["QCARomePatchRAM.kext"]])
        XCTAssertEqual(UpdatePlanner.nextBatch(plan, applied: [])?.count, 3)
        XCTAssertEqual(
            UpdatePlanner.nextBatch(plan, applied: ["Helios 300 Setup.app", "NVK-Pascal", "install-gpu.sh"])?.map(\.id),
            ["QCARomePatchRAM.kext"])
        XCTAssertNil(UpdatePlanner.nextBatch(plan, applied: Set(plan.units.map(\.id))))
    }

    func testConfigSettingsChangeIsItsOwnRestart() throws {
        var (old, new) = try manifests(oldABI: "1.0", newABI: "1.0", changed: [])
        new.configDelta?.bootArgsRequired = ["ath10k_cc=IN"]
        let plan = UpdatePlanner.plan(old: old, new: new, diff: ManifestDiff.between(old, new))
        XCTAssertEqual(plan.batches.map { $0.map(\.id) }, [["config"]])
        old.configDelta = new.configDelta
        XCTAssertTrue(UpdatePlanner.plan(old: old, new: new, diff: ManifestDiff.between(old, new)).isEmpty)
    }

    // MARK: download + verification

    /// A one-artifact release served by FakeHTTP: Foo.kext.zip with two inner files.
    func release(tampered: ((String) -> Void)? = nil) throws -> (Manifest, Data, FakeHTTP, String) {
        let work = tempDir(self)
        let kext = "\(work)/src/Foo.kext/Contents"
        write("<plist/>", "\(kext)/Info.plist")
        write("binary", "\(kext)/MacOS/Foo")
        let contents = [
            "Contents/Info.plist": Hashing.sha256(Data("<plist/>".utf8)),
            "Contents/MacOS/Foo": Hashing.sha256(Data("binary".utf8)),
        ]
        tampered?("\(work)/src")
        makeZip("\(work)/src", top: "Foo.kext", zip: "\(work)/Foo.kext.zip")
        let zip = try Data(contentsOf: URL(fileURLWithPath: "\(work)/Foo.kext.zip"))
        var m = try manifestFixture("manifest-0.1.0.json")
        m.release?.version = "0.1.1"
        m.release?.tag = "v0.1.1"
        var a = m.artifacts[0]
        a.name = "Foo.kext"
        a.file = "Foo.kext.zip"
        a.size = zip.count
        a.sha256 = Hashing.sha256(zip)  // recomputed: only contents_sha256 can catch the tampering
        a.url = ManifestCheck.urlPrefix + "v0.1.1/Foo.kext.zip"
        a.contentsSha256 = contents
        a.opencore = nil
        a.kind = "bundle"
        m.artifacts = [a]
        let raw = try JSONEncoder().encode(m)
        let http = FakeHTTP()
        http.pages[a.url ?? ""] = zip
        http.pages[ManifestCheck.urlPrefix + "v0.1.1/manifest.json"] = raw
        return (m, raw, http, work)
    }

    func updater(_ http: FakeHTTP) -> Updater {
        Updater(http: http, fs: LocalFileSystem(), unzipper: CommandUnzipper())
    }

    func testStageVerifiesEveryFileAndInnerFile() throws {
        let (m, raw, http, work) = try release()
        let staged = try updater(http).stage(m, raw: raw, into: "\(work)/staged", previousDir: nil)
        XCTAssertEqual(staged.manifestSha256, Hashing.sha256(raw))
        XCTAssertTrue(FileManager.default.fileExists(atPath: "\(work)/staged/x/Foo.kext/Contents/MacOS/Foo"))
    }

    func testTamperedZipWithAnExtraFileIsRefused() throws {
        let (m, raw, http, work) = try release { src in write("evil", "\(src)/Foo.kext/Contents/MacOS/evil") }
        XCTAssertThrowsError(try updater(http).stage(m, raw: raw, into: "\(work)/staged", previousDir: nil)) { e in
            XCTAssertEqual(e as? VerifyError, .extraInner("Foo.kext", "Contents/MacOS/evil"))
        }
    }

    func testTamperedZipWithAChangedFileIsRefused() throws {
        let (m, raw, http, work) = try release { src in write("other binary", "\(src)/Foo.kext/Contents/MacOS/Foo") }
        XCTAssertThrowsError(try updater(http).stage(m, raw: raw, into: "\(work)/staged", previousDir: nil)) { e in
            XCTAssertEqual(e as? VerifyError, .innerMismatch("Foo.kext", "Contents/MacOS/Foo"))
        }
    }

    func testCorruptDownloadIsRefused() throws {
        let (m, raw, http, work) = try release()
        let url = m.artifacts[0].url ?? ""
        http.pages[url] = (http.pages[url] ?? Data()) + Data([0])
        XCTAssertThrowsError(try updater(http).stage(m, raw: raw, into: "\(work)/staged", previousDir: nil)) { e in
            guard case .fileMismatch = e as? VerifyError else { return XCTFail("\(e)") }
        }
    }

    func testUnchangedFilesComeFromTheInstalledRelease() throws {
        let (m, raw, http, work) = try release()
        _ = try updater(http).stage(m, raw: raw, into: "\(work)/old", previousDir: nil)
        http.pages = [:]
        http.fetched = []
        XCTAssertNoThrow(try updater(http).stage(m, raw: raw, into: "\(work)/new", previousDir: "\(work)/old"))
        XCTAssertTrue(http.fetched.isEmpty)
    }

    func testManifestTagMustMatch() throws {
        let (_, raw, http, _) = try release()
        http.pages[ManifestCheck.urlPrefix + "v0.1.2/manifest.json"] = raw
        XCTAssertThrowsError(try updater(http).manifest(tag: "v0.1.2")) { e in
            XCTAssertEqual(e as? UpdateError, .tagMismatch("v0.1.2", "0.1.1"))
        }
        XCTAssertNoThrow(try updater(http).manifest(tag: "v0.1.1"))
    }

    // MARK: Dortania build repo (optional third-party kexts)

    func testDortaniaIndexAndUpdates() {
        let index = DortaniaIndex.parse(fixtureData("dortania-latest-subset.json"))
        XCTAssertEqual(index["Lilu"]?.version, "1.7.3")
        XCTAssertEqual(index["Lilu"]?.sha256.count, 64)
        let products = [
            "Lilu": [KextRef(bundle: "Lilu.kext", id: "as.vit9696.Lilu")],
            "VoodooPS2": [KextRef(bundle: "VoodooPS2Controller.kext", id: "as.acidanthera.voodoo.driver.PS2Controller")],
            "AMFIPass": [KextRef(bundle: "AMFIPass.kext", id: "com.dhinakg.AMFIPass")],
        ]
        let ups = DortaniaIndex.updates(
            index: index, products: products,
            loaded: [
                "as.vit9696.Lilu": "1.7.1", "as.acidanthera.voodoo.driver.PS2Controller": "9.9.9",
                "com.dhinakg.AMFIPass": "1.0.0",
            ])
        XCTAssertEqual(ups.map(\.product), ["Lilu"], "newer only, and never a kext the index does not carry")
        XCTAssertTrue(DortaniaIndex.isNewer("1.10.0", than: "1.9.9"))
    }
}
