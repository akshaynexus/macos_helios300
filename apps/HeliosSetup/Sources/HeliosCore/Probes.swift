// SPDX-License-Identifier: GPL-2.0-only
//
// Probes: evaluate the manifest's declarative status checks against this Mac. Each probe kind maps to one
// read-only command (ioreg, kmutil, pmset, nvram, system_profiler, ...) run through the injected CommandRunner;
// outputs are cached per ProbeContext, so one Status refresh runs each command once. Nothing here needs root.
//
// Probe kinds (spec.json `app.detect` and `health`):
//   file        path exists                         kext       bundle_id is loaded (kmutil showloaded)
//   ioreg       class + key [equals|contains]       pmset      key [equals]          bootarg  arg in boot-args
//   nvram       var [equals|contains]               profiler   type + key, or type + items (child names)
//   filevault-off  FileVault is off                 boot-source  internal | usb | other | unknown [equals]
//   ip / ssid   the address / network name of a hardware port (port: "Wi-Fi"); ssid is private
//   build       sw_vers -buildVersion [equals]
// `negate` inverts the result; `format: band` shows a channel number as its band.
import Foundation

/// Where the macOS tools live. Facts about macOS, not about the release.
public enum Tool {
    public static let ioreg = "/usr/sbin/ioreg"
    public static let kmutil = "/usr/bin/kmutil"
    public static let pmset = "/usr/bin/pmset"
    public static let nvram = "/usr/sbin/nvram"
    public static let systemProfiler = "/usr/sbin/system_profiler"
    public static let networksetup = "/usr/sbin/networksetup"
    public static let ipconfig = "/usr/sbin/ipconfig"
    public static let fdesetup = "/usr/bin/fdesetup"
    public static let diskutil = "/usr/sbin/diskutil"
    public static let swVers = "/usr/bin/sw_vers"
    public static let sysctl = "/usr/sbin/sysctl"
    public static let bash = "/bin/bash"
    public static let ditto = "/usr/bin/ditto"
    public static let open = "/usr/bin/open"
    public static let launchctl = "/bin/launchctl"
    /// OpenCore's NVRAM vendor GUID (opencore-version, boot-path).
    public static let ocGUID = "4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102"
}

public struct ProbeResult: Equatable {
    /// true / false for a probe with an expectation; nil for one that only reports a value.
    public var ok: Bool?
    public var value: String
    public var isPrivate: Bool

    public init(ok: Bool?, value: String, isPrivate: Bool = false) {
        self.ok = ok
        self.value = value
        self.isPrivate = isPrivate
    }
}

/// Reads this Mac once per refresh. `internalESPUUID` (from EFIManager) lets boot-source tell the internal
/// OpenCore from the stick.
public final class ProbeContext {
    let runner: CommandRunner
    let fs: FileSystem
    public var internalESPUUID: String?
    private var cache: [String: CommandResult] = [:]

    public init(runner: CommandRunner, fs: FileSystem, internalESPUUID: String? = nil) {
        self.runner = runner
        self.fs = fs
        self.internalESPUUID = internalESPUUID
    }

    /// One command's output, cached.
    public func output(_ argv: [String]) -> CommandResult {
        let key = argv.joined(separator: "\u{1f}")
        if let hit = cache[key] { return hit }
        let result = runner.capture(argv, env: nil, cwd: nil)
        cache[key] = result
        return result
    }

    public func ioreg(_ ioClass: String) -> [String: String] {
        IORegParser.properties(output([Tool.ioreg, "-r", "-c", ioClass, "-d", "1", "-w", "0"]).stdout)
    }

    public lazy var loadedKexts: [String: String] = KmutilParser.loaded(output([Tool.kmutil, "showloaded"]).stdout)
    public lazy var pmset: [String: String] = PmsetParser.settings(output([Tool.pmset, "-g"]).stdout)
    public lazy var bootArgs: [String] = NVRAMParser.bootArgs(output([Tool.nvram, "boot-args"]).stdout)
    public lazy var build: String = output([Tool.swVers, "-buildVersion"]).stdout
        .trimmingCharacters(in: .whitespacesAndNewlines)
    public lazy var darwin: String = output([Tool.sysctl, "-n", "kern.osrelease"]).stdout
        .trimmingCharacters(in: .whitespacesAndNewlines)
    public lazy var bootSession: String = output([Tool.sysctl, "-n", "kern.bootsessionuuid"]).stdout
        .trimmingCharacters(in: .whitespacesAndNewlines)

    public func nvram(_ name: String) -> String? {
        NVRAMParser.value(output([Tool.nvram, name]).stdout)
    }

    public func profiler(_ type: String) -> ProfilerNode {
        ProfilerNode.parse(output([Tool.systemProfiler, "-detailLevel", "mini", type]).stdout)
    }

    public func device(ofPort port: String) -> String? {
        NetworkParser.ports(output([Tool.networksetup, "-listallhardwareports"]).stdout)[port]
    }

    /// internal (this Mac's ESP), usb, other, or unknown (no OpenCore boot-path).
    public var bootSource: String {
        guard let path = nvram("\(Tool.ocGUID):boot-path") else { return "unknown" }
        if let esp = internalESPUUID, let part = NVRAMParser.gptPartition(inDevicePath: path), part == esp.uppercased() {
            return "internal"
        }
        if NVRAMParser.isUSB(path) { return "usb" }
        return internalESPUUID == nil ? "unknown" : "other"
    }

    public var fileVaultOff: Bool {
        output([Tool.fdesetup, "status"]).stdout.contains("FileVault is Off")
    }

    public func exists(_ path: String) -> Bool { fs.exists(path) }
}

public enum ProbeEvaluator {
    /// Evaluates one probe on this Mac.
    public static func evaluate(_ p: Probe, in ctx: ProbeContext) -> ProbeResult {
        var r = raw(p, ctx)
        if p.format == "band", r.value != "absent" { r.value = NetworkParser.band(channel: r.value) }
        if p.negate == true, let ok = r.ok { r.ok = !ok }
        r.isPrivate = p.isPrivate ?? (p.probe == "ssid")
        return r
    }

    /// All probes true (an empty list is true).
    public static func all(_ probes: [Probe]?, in ctx: ProbeContext) -> Bool {
        (probes ?? []).allSatisfy { evaluate($0, in: ctx).ok ?? false }
    }

    private static func raw(_ p: Probe, _ ctx: ProbeContext) -> ProbeResult {
        switch p.probe {
        case "file":
            let present = ctx.exists(p.path ?? "")
            return ProbeResult(ok: present, value: present ? "present" : "absent")
        case "kext":
            let version = ctx.loadedKexts[p.bundleId ?? ""]
            return ProbeResult(ok: version != nil, value: version.map { "loaded (\($0))" } ?? "not loaded")
        case "ioreg":
            return compare(p, ctx.ioreg(p.ioClass ?? "")[p.key ?? ""])
        case "pmset":
            return compare(p, ctx.pmset[p.key ?? ""])
        case "bootarg":
            let has = ctx.bootArgs.contains(p.arg ?? "")
            return ProbeResult(ok: has, value: has ? "set" : "not set")
        case "nvram":
            return compare(p, ctx.nvram(p.variable ?? ""))
        case "profiler":
            return profiler(p, ctx)
        case "filevault-off":
            let off = ctx.fileVaultOff
            return ProbeResult(ok: off, value: off ? "FileVault off" : "FileVault on")
        case "boot-source":
            return compare(p, ctx.bootSource)
        case "build":
            return compare(p, ctx.build)
        case "ip":
            let dev = ctx.device(ofPort: p.port ?? "")
            let ip = dev.map { ctx.output([Tool.ipconfig, "getifaddr", $0]).stdout }?
                .trimmingCharacters(in: .whitespacesAndNewlines)
            return compare(p, (ip ?? "").isEmpty ? nil : ip)
        case "ssid":
            let dev = ctx.device(ofPort: p.port ?? "")
            return compare(p, dev.flatMap { NetworkParser.ssid(ctx.output([Tool.ipconfig, "getsummary", $0]).stdout) })
        default:
            return ProbeResult(ok: false, value: "unknown probe kind '\(p.probe)' (a newer release needs a newer app)")
        }
    }

    private static func profiler(_ p: Probe, _ ctx: ProbeContext) -> ProbeResult {
        let tree = ctx.profiler(p.type ?? "")
        if let items = p.items {
            let names = tree.find(items)?.children.map(\.name) ?? []
            return compare(p, names.isEmpty ? nil : names.joined(separator: ", "))
        }
        return compare(p, tree.firstValue(p.key ?? ""))
    }

    /// equals / contains against a value; without either, "present" is the expectation.
    static func compare(_ p: Probe, _ value: String?) -> ProbeResult {
        guard let value else { return ProbeResult(ok: false, value: "absent") }
        if let want = p.equals { return ProbeResult(ok: value == want, value: value) }
        if let want = p.contains { return ProbeResult(ok: value.contains(want), value: value) }
        return ProbeResult(ok: true, value: value)
    }
}
