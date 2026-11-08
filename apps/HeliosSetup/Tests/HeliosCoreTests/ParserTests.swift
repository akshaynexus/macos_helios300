// SPDX-License-Identifier: GPL-2.0-only
import XCTest

@testable import HeliosCore

final class ParserTests: XCTestCase {
    func testIORegPG106RealCapture() {
        let p = IORegParser.properties(fixture("ioreg-PG106-notready.txt"))
        XCTAssertEqual(p["PG106Ready"], "No")
        XCTAssertEqual(p["CFBundleIdentifier"], "org.helios300.pg106")
        XCTAssertEqual(p["PG106KifABI"], "65536")
        XCTAssertEqual(p["PG106RenderUsers"], "Yes")
        XCTAssertTrue(IORegParser.hasNode(fixture("ioreg-PG106-notready.txt")))
    }

    func testIORegWiFiRun7() {
        let front = IORegParser.properties(fixture("ioreg-AirportAth10k-run7.txt"))
        XCTAssertEqual(front["AirportAth10kJoinState"], "connected")
        let core = IORegParser.properties(fixture("ioreg-Ath10kPCIDevice-run7.txt"))
        XCTAssertEqual(core["Ath10kConnectionState"], "connected-wpa2")
        XCTAssertEqual(core["Ath10kConnectionPHY"], "HT40-2SS")
        XCTAssertEqual(core["Ath10kRxSecurityExpectedBSSID"], "<000000000000>")
    }

    func testIORegTreePrefixedLinesFirstWins() {
        let p = IORegParser.properties(fixture("ioreg-tree-run6.txt"))
        XCTAssertEqual(p["AirportAth10kJoinState"], "ready")
        XCTAssertEqual(p["AirportAth10kLastDisconnectReason"], "3")
        XCTAssertEqual(p["Ath10kConnectionState"], "disabled")
    }

    func testIORegIgnoresNonProperties() {
        XCTAssertTrue(IORegParser.properties("+-o Root  <class IORegistryEntry>\n    {\n    }\n").isEmpty)
        XCTAssertEqual(
            IORegParser.properties(#"  "brightness" = {"min"=0,"max"=1024,"value"=512}"#)["brightness"],
            #"{"min"=0,"max"=1024,"value"=512}"#)
    }

    func testKmutilRealCapture() {
        let k = KmutilParser.loaded(fixture("kmutil-showloaded.txt"))
        XCTAssertEqual(k["org.helios300.pg106"], "0.1.0")
        XCTAssertEqual(k["io.github.akshaynexus.helios300.QCARomePatchRAM"], "0.2.0")
        XCTAssertEqual(k["com.apple.iokit.IOAcceleratorFamily2"], "487.4.4")
        XCTAssertNil(k["org.helios300.pg106fb"])
        XCTAssertEqual(k.count, 11)
    }

    func testPmset() {
        XCTAssertEqual(PmsetParser.settings(fixture("pmset-g-sleep-disabled.txt"))["SleepDisabled"], "1")
        XCTAssertEqual(PmsetParser.settings(fixture("pmset-g-sleep-disabled.txt"))["sleep"], "0 (sleep prevented by powerd)")
        XCTAssertNil(PmsetParser.settings(fixture("pmset-g-sleep-allowed.txt"))["SleepDisabled"])
    }

    func testNVRAM() {
        XCTAssertTrue(NVRAMParser.bootArgs(fixture("nvram-boot-args.txt")).contains("-amfipassbeta"))
        XCTAssertNil(NVRAMParser.value("nvram: Error getting variable - 'x': (iokit/common) data was not found\n"))
        let onDisk = NVRAMParser.value(fixture("nvram-boot-path-internal.txt")) ?? ""
        XCTAssertEqual(NVRAMParser.gptPartition(inDevicePath: onDisk), "8E2D1F5A-3C4B-4D5E-8F90-A1B2C3D4E5F6")
        XCTAssertFalse(NVRAMParser.isUSB(onDisk))
        XCTAssertTrue(NVRAMParser.isUSB(NVRAMParser.value(fixture("nvram-boot-path-usb.txt")) ?? ""))
    }

    func testProfilerTree() {
        let audio = ProfilerNode.parse(fixture("sp-audio.txt"))
        XCTAssertEqual(audio.find("Devices")?.children.map(\.name), ["Built-in Microphone", "Built-in Output"])
        XCTAssertEqual(audio.firstValue("Output Source"), "Internal Speakers")
        let bt = ProfilerNode.parse(fixture("sp-bluetooth.txt"))
        XCTAssertEqual(bt.firstValue("State"), "On")
        XCTAssertEqual(bt.find("Bluetooth Controller")?.firstValue("Product ID"), "0x3016")
    }

    func testDiskutil() {
        XCTAssertEqual(DiskutilParser.efiPartitions(fixtureData("diskutil-list-internal.plist")), ["disk0s1"])
        let i = DiskutilParser.info(fixtureData("diskutil-info-esp.plist"))
        XCTAssertNil(i.mountPoint)
        XCTAssertEqual(i.partitionUUID, "8E2D1F5A-3C4B-4D5E-8F90-A1B2C3D4E5F6")
        XCTAssertEqual(i.size, 209_715_200)
    }

    func testNetwork() {
        let ports = NetworkParser.ports(
            "Hardware Port: Ethernet\nDevice: en0\nEthernet Address: x\n\nHardware Port: Wi-Fi\nDevice: en1\n")
        XCTAssertEqual(ports["Wi-Fi"], "en1")
        XCTAssertEqual(NetworkParser.ssid("<dictionary> {\n  SSID : Home Net\n  BSSID : x\n}"), "Home Net")
        XCTAssertEqual(NetworkParser.band(channel: "4"), "2.4 GHz (channel 4)")
        XCTAssertEqual(NetworkParser.band(channel: "36"), "5 GHz (channel 36)")
    }

    func testOpenCoreVersions() {
        XCTAssertEqual(OpenCoreVersion.fromBuildString("REL-105-2025-05-05"), "1.0.5")
        XCTAssertEqual(OpenCoreVersion.fromBinary(Data([0, 1] + Array("xxDBG-106-2025-08-04\0".utf8))), "1.0.6")
        XCTAssertEqual(
            OpenCoreVersion.fromOcvalidate("NOTE: This version of ocvalidate is only compatible with OpenCore version 1.0.5!\n"),
            "1.0.5")
        XCTAssertNil(OpenCoreVersion.fromBuildString("nothing"))
    }

    func testSHA256Vectors() {
        var h = SHA256Hasher()
        h.update(Data("abc".utf8))
        XCTAssertEqual(h.finalize(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        var e = SHA256Hasher()
        XCTAssertEqual(e.finalize(), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
        var long = SHA256Hasher()
        for _ in 0..<1000 { long.update(Data(repeating: 0x61, count: 1000)) }
        XCTAssertEqual(long.finalize(), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")
        XCTAssertEqual(Hashing.sha256(Data("abc".utf8)), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
    }
}
