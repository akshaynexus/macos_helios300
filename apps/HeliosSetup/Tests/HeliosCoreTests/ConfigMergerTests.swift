// SPDX-License-Identifier: GPL-2.0-only
//
// config_delta merges into the Helios's real (sanitized) internal config.plist, and never drops the user's keys.
import XCTest

@testable import HeliosCore

final class ConfigMergerTests: XCTestCase {
    func config() throws -> [String: Any] { try ConfigMerger.read(fixtureData("config-internal.plist")) }
    func delta() throws -> ConfigDelta {
        guard let d = try manifestFixture("manifest-next.json").configDelta else { throw ConfigError.notADictionary }
        return d
    }

    func kexts(_ c: [String: Any]) -> [[String: Any]] { (c["Kernel"] as? [String: Any])?["Add"] as? [[String: Any]] ?? [] }
    func bundlePaths(_ c: [String: Any]) -> [String] { kexts(c).compactMap { $0["BundlePath"] as? String } }
    func bootArgs(_ c: [String: Any]) -> String {
        (((c["NVRAM"] as? [String: Any])?["Add"] as? [String: Any])?[ConfigMerger.bootGUID] as? [String: Any])?["boot-args"]
            as? String ?? ""
    }

    func testRealConfigKeepsEveryUserEntryAndSetting() throws {
        let before = try config()
        let (after, changes) = ConfigMerger.merge(before, delta: try delta())
        XCTAssertEqual(bundlePaths(after), bundlePaths(before), "every kext is already there: nothing added, nothing dropped")
        XCTAssertEqual(bootArgs(after), bootArgs(before), "no required boot-args: the user's stay as they are")
        XCTAssertTrue(bootArgs(after).contains("ath10k_native_delay=150"))
        XCTAssertEqual(
            (after["PlatformInfo"] as? [String: Any])?.keys.sorted(), (before["PlatformInfo"] as? [String: Any])?.keys.sorted())
        XCTAssertEqual((after["Misc"] as? [String: Any])?.count, (before["Misc"] as? [String: Any])?.count)
        let props = ((after["DeviceProperties"] as? [String: Any])?["Add"] as? [String: Any]) ?? [:]
        XCTAssertNotNil(props["PciRoot(0x0)/Pci(0x1c,0x3)/Pci(0x0,0x1)"], "a user device path stays")
        XCTAssertEqual((props["PciRoot(0x0)/Pci(0x2,0x0)"] as? [String: Any])?["applbkl"] as? Data, Data([1, 0, 0, 0]))
        XCTAssertFalse(changes.contains { $0.path.hasPrefix("Kernel/Add/WhateverGreen") })
    }

    func testMergeIsIdempotent() throws {
        let (once, _) = ConfigMerger.merge(try config(), delta: try delta())
        let (_, again) = ConfigMerger.merge(once, delta: try delta())
        XCTAssertEqual(again.filter { $0.kind != .warning }, [], "a second merge changes nothing")
    }

    func testExistingEntryTakesTheReleaseRangeAndKeepsUserEnabledAndComment() throws {
        var c = try config()
        var add = kexts(c)
        let i = try XCTUnwrap(add.firstIndex { ($0["BundlePath"] as? String) == "Ath10kMac.kext" })
        add[i]["MaxKernel"] = "26.99.99"
        add[i]["Comment"] = "my comment"
        add[i]["Enabled"] = false
        c["Kernel"] = ["Add": add]
        let (after, changes) = ConfigMerger.merge(c, delta: try delta())
        let e = try XCTUnwrap(kexts(after).first { ($0["BundlePath"] as? String) == "Ath10kMac.kext" })
        XCTAssertEqual(e["MaxKernel"] as? String, "25.99.99")
        XCTAssertEqual(e["Comment"] as? String, "my comment")
        XCTAssertEqual(e["Enabled"] as? Bool, false)
        XCTAssertTrue(changes.contains { $0.kind == .warning && $0.path == "Kernel/Add/Ath10kMac.kext" })
    }

    func testNewKextGoesAfterWhatItRequires() throws {
        var c = try config()
        c["Kernel"] = [
            "Add": kexts(c).filter { !["AirportAth10k.kext", "NMHDA.kext"].contains($0["BundlePath"] as? String ?? "") }
        ]
        let (after, changes) = ConfigMerger.merge(c, delta: try delta())
        let paths = bundlePaths(after)
        let ath = try XCTUnwrap(paths.firstIndex(of: "Ath10kMac.kext"))
        XCTAssertEqual(paths[ath + 1], "AirportAth10k.kext")
        let nmhda = try XCTUnwrap(paths.firstIndex(of: "NMHDA.kext"))
        XCTAssertGreaterThan(nmhda, try XCTUnwrap(paths.firstIndex(of: "AppleALC.kext")))
        XCTAssertGreaterThan(nmhda, try XCTUnwrap(paths.firstIndex(of: "Lilu.kext")))
        let entry = try XCTUnwrap(kexts(after).first { ($0["BundlePath"] as? String) == "AirportAth10k.kext" })
        XCTAssertEqual(entry.keys.sorted(), ConfigMerger.kextKeys, "a new entry has every Sample.plist key")
        XCTAssertEqual(changes.filter { $0.kind == .added && $0.path.hasPrefix("Kernel/Add/") }.count, 2)
    }

    func testRequiredBootArgsAddOrUpdateButKeepTheRest() throws {
        var d = try delta()
        d.bootArgsRequired = ["ath10k_native_delay=0", "pg106_selftest=2"]
        let (after, _) = ConfigMerger.merge(try config(), delta: d)
        let args = bootArgs(after).split(separator: " ").map(String.init)
        XCTAssertTrue(args.contains("ath10k_native_delay=0"))
        XCTAssertFalse(args.contains("ath10k_native_delay=150"))
        XCTAssertTrue(args.contains("pg106_selftest=2"))
        XCTAssertTrue(args.contains("-amfipassbeta"))
    }

    func testNVRAMDeleteIsAUnion() throws {
        var c = try config()
        var nv = c["NVRAM"] as? [String: Any] ?? [:]
        nv["Delete"] = [ConfigMerger.bootGUID: ["mine", "bluetoothExternalDongleFailed"]]
        c["NVRAM"] = nv
        let (after, _) = ConfigMerger.merge(c, delta: try delta())
        let del = (((after["NVRAM"] as? [String: Any])?["Delete"] as? [String: Any])?[ConfigMerger.bootGUID] as? [String]) ?? []
        XCTAssertEqual(del, ["mine", "bluetoothExternalDongleFailed", "bluetoothInternalControllerInfo"])
    }

    func testForbiddenIsReportedNotEdited() throws {
        var c = try config()
        var add = kexts(c)
        add.append(["BundlePath": "NVRM.kext", "Enabled": true])
        c["Kernel"] = ["Add": add]
        let (after, changes) = ConfigMerger.merge(c, delta: try delta())
        XCTAssertTrue(bundlePaths(after).contains("NVRM.kext"))
        XCTAssertTrue(changes.contains { $0.kind == .warning && $0.path == "Kernel/Add/NVRM.kext" })
    }

    func testPlistRoundTripKeepsTypes() throws {
        let (after, _) = ConfigMerger.merge(try config(), delta: try delta())
        let back = try ConfigMerger.read(try ConfigMerger.write(after))
        let e = try XCTUnwrap(kexts(back).first)
        XCTAssertTrue(PlistValue.isBool(e["Enabled"]))
        XCTAssertTrue(((back["PlatformInfo"] as? [String: Any])?["Generic"] as? [String: Any])?["ROM"] is Data)
    }
}
