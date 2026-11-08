// SPDX-License-Identifier: GPL-2.0-only
//
// The trust chain, as 1401's p1401/helios300.py checks it (tests/test_helios300.py covers the same rules there).
import XCTest

@testable import HeliosCore

final class ManifestTests: XCTestCase {
    func testRealReleaseManifestDecodesAndPasses() throws {
        let m = try manifestFixture("manifest-0.1.0.json")
        XCTAssertEqual(m.release?.tag, "v0.1.0")
        XCTAssertNoThrow(try ManifestCheck.check(m))
        XCTAssertEqual(m.artifact("PG106.kext")?.opencore?.order, 100)
        XCTAssertEqual(m.artifact("NVK-Pascal")?.contentsSha256?["nvk_icd.json"]?.count, 64, "dictionary keys are not rewritten")
        XCTAssertNil(m.configDelta, "0.1.0 predates config_delta")
    }

    func testNextManifestCarriesTheAppFields() throws {
        let m = try manifestFixture("manifest-next.json")
        XCTAssertNoThrow(try ManifestCheck.check(m, local: true))
        XCTAssertThrowsError(try ManifestCheck.check(m), "a dry-run manifest only for a local test")
        XCTAssertEqual(m.updateGroups?.map(\.name), ["wifi", "pg106"])
        XCTAssertEqual(m.updateGroups?.last?.abi, "1.0")
        XCTAssertEqual(m.artifact("AirportAth10k.kext")?.updateGroup, "wifi")
        XCTAssertEqual(m.artifact("NVMTLDriver.bundle")?.requiresRestart, false)
        XCTAssertEqual(
            m.configDelta?.kernelAdd.map(\.bundlePath),
            ["PG106.kext", "NMHDA.kext", "QCARomePatchRAM.kext", "Ath10kMac.kext", "AirportAth10k.kext"])
        XCTAssertEqual(m.configDelta?.kernelAdd.last?.requires, ["Ath10kMac.kext"])
        XCTAssertEqual(m.configDelta?.deviceProperties["PciRoot(0x0)/Pci(0x2,0x0)"]?["applbkl"], .data(Data([1, 0, 0, 0])))
        XCTAssertFalse(m.health?.isEmpty ?? true)
        XCTAssertNotNil(m.appHelp)
    }

    func testSpecAndManifestDescribeTheSameSteps() throws {
        let spec = try realSpec(), next = try manifestFixture("manifest-next.json")
        XCTAssertEqual(
            StepPlanner.items(spec).map(\.id), StepPlanner.items(next).map(\.id),
            "regenerate Fixtures/manifest-next.json (Tests/regen-fixture-manifest.py) after a spec change")
    }

    func checked(_ edit: (inout Manifest) -> Void) throws {
        var m = try manifestFixture("manifest-0.1.0.json")
        edit(&m)
        try ManifestCheck.check(m)
    }

    func testCheckRefusesWhat1401Refuses() {
        XCTAssertThrowsError(try checked { $0.schema = 2 })
        XCTAssertThrowsError(try checked { $0.repo = "someone/else" })
        XCTAssertThrowsError(try checked { $0.release?.version = "latest" })
        XCTAssertThrowsError(try checked { $0.release?.dryRun = true })
        XCTAssertThrowsError(try checked { $0.artifacts = [] })
        XCTAssertThrowsError(try checked { $0.artifacts[0].sha256 = "abc" })
        XCTAssertThrowsError(try checked { $0.artifacts[0].file = "../evil.zip" })
        XCTAssertThrowsError(try checked { $0.artifacts[0].url = "https://example.com/PG106.kext.zip" })
        XCTAssertThrowsError(try checked { $0.artifacts[0].size = 0 })
        XCTAssertThrowsError(try checked { $0.artifacts[0].install = "somewhere" })
        XCTAssertThrowsError(try checked { $0.artifacts[0].opencore?.plistPath = "Info.plist" })
        XCTAssertThrowsError(try checked { $0.config?.forbidden = nil })
    }

    func testVerifyFileSizeAndHash() throws {
        var a = try manifestFixture("manifest-0.1.0.json").artifacts[0]
        let data = Data("payload".utf8)
        a.size = data.count
        a.sha256 = Hashing.sha256(data)
        XCTAssertNoThrow(try ManifestCheck.verifyFile(data, artifact: a, version: "0.1.0"))
        XCTAssertThrowsError(try ManifestCheck.verifyFile(Data("payloaD".utf8), artifact: a, version: "0.1.0"))
        a.size = 99
        XCTAssertThrowsError(try ManifestCheck.verifyFile(data, artifact: a, version: "0.1.0"))
    }

    func testCovers() throws {
        let m = try manifestFixture("manifest-0.1.0.json")
        XCTAssertTrue(ManifestCheck.covers(m, darwin: "25.6.0"))
        XCTAssertFalse(ManifestCheck.covers(m, darwin: "24.6.0"))
        XCTAssertFalse(ManifestCheck.covers(m, darwin: "26.0.0"))
        XCTAssertEqual(ManifestCheck.darwinTuple("garbage"), [0, 0, 0])
    }

    func testPlistValueRoundTrip() throws {
        let json = #"{"a": {"data": "01000000"}, "b": {"string": "onboard-2"}, "c": {"integer": 7}, "d": {"boolean": true}}"#
        let v = try JSONDecoder().decode([String: PlistValue].self, from: Data(json.utf8))
        XCTAssertEqual(v["a"], .data(Data([1, 0, 0, 0])))
        XCTAssertTrue(v["d"]?.matches(true) ?? false)
        XCTAssertFalse(v["c"]?.matches(true) ?? true, "an integer is not a boolean")
        let back = try JSONDecoder().decode([String: PlistValue].self, from: JSONEncoder().encode(v))
        XCTAssertEqual(back, v)
    }
}
