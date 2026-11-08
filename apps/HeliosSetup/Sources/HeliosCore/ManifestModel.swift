// SPDX-License-Identifier: GPL-2.0-only
//
// ManifestModel: the release contract (macos_helios300 docs/MANIFEST.md, tools/release/manifest.schema.json) as
// Codable types. The same types read tools/release/spec.json (the static half the app embeds), where the computed
// fields (release, sha256, size, url, contents_sha256, config_delta) are absent, hence the optionals.
//
// Everything the app shows or does comes from here: the checklist (post_install + its `app` block), the status
// probes (`app.detect`, `health`), update grouping (`update_groups`, `requires_restart`, `update_group`) and
// the OpenCore changes (`config_delta`). Adding a step or artifact to the release needs no app code.
// Coding keys are spelled out: a key strategy would also rewrite dictionary keys such as "nvk_icd.json".
import Foundation

public struct Manifest: Codable, Equatable {
    public var schema: Int
    public var repo: String
    public var release: ReleaseInfo?
    public var macos: MacOSRange
    public var artifacts: [Artifact]
    public var externalKexts: [ExternalKext]?
    public var config: ManifestConfig?
    public var postInstall: [PostInstallStep]
    public var health: [HealthItem]?
    public var updateGroups: [UpdateGroup]?
    public var configDelta: ConfigDelta?
    public var thirdPartyKexts: ThirdPartyKexts?
    public var appHelp: AppHelp?

    enum CodingKeys: String, CodingKey {
        case schema, repo, release, macos, artifacts, config, health
        case externalKexts = "external_kexts"
        case postInstall = "post_install"
        case updateGroups = "update_groups"
        case configDelta = "config_delta"
        case thirdPartyKexts = "third_party_kexts"
        case appHelp = "app_help"
    }

    public static func decode(_ data: Data) throws -> Manifest {
        do {
            return try JSONDecoder().decode(Manifest.self, from: data)
        } catch let error as DecodingError {
            throw ManifestError.unreadable(describe(error))
        }
    }

    public func artifact(_ name: String) -> Artifact? { artifacts.first { $0.name == name } }

    private static func describe(_ error: DecodingError) -> String {
        switch error {
        case .keyNotFound(let key, let ctx): return "missing \(path(ctx.codingPath + [key]))"
        case .typeMismatch(_, let ctx), .valueNotFound(_, let ctx), .dataCorrupted(let ctx):
            return "\(path(ctx.codingPath)): \(ctx.debugDescription)"
        @unknown default: return "\(error)"
        }
    }

    private static func path(_ keys: [CodingKey]) -> String {
        keys.map { $0.intValue.map { "[\($0)]" } ?? $0.stringValue }.joined(separator: ".")
    }
}

public struct ReleaseInfo: Codable, Equatable {
    public var version: String
    public var tag: String
    public var commit: String?
    public var preRelease: Bool?
    public var dryRun: Bool?
    public var built: String?

    enum CodingKeys: String, CodingKey {
        case version, tag, commit, built
        case preRelease = "pre_release"
        case dryRun = "dry_run"
    }
}

public struct MacOSRange: Codable, Equatable {
    public var minKernel: String
    public var maxKernel: String

    enum CodingKeys: String, CodingKey {
        case minKernel = "min_kernel"
        case maxKernel = "max_kernel"
    }
}

public struct Artifact: Codable, Equatable {
    public var name: String
    public var kind: String
    public var install: String
    public var file: String?
    public var sha256: String?
    public var size: Int?
    public var url: String?
    public var version: String?
    public var bundleId: String?
    public var installPath: String?
    public var opencore: OpenCoreKext?
    public var acpi: OpenCoreACPI?
    public var requires: [String]?
    public var contentsSha256: [String: String]?
    public var optional: Bool?
    public var requiresRestart: Bool?
    public var updateGroup: String?
    public var status: String?

    enum CodingKeys: String, CodingKey {
        case name, kind, install, file, sha256, size, url, version, opencore, acpi, requires, optional, status
        case bundleId = "bundle_id"
        case installPath = "install_path"
        case contentsSha256 = "contents_sha256"
        case requiresRestart = "requires_restart"
        case updateGroup = "update_group"
    }

    public var isZip: Bool { (file ?? "").hasSuffix(".zip") }
}

/// One OpenCore Kernel > Add entry (OpenCore Configuration.pdf, Sample.plist key names).
public struct OpenCoreKext: Codable, Equatable {
    public var order: Int?
    public var bundlePath: String
    public var executablePath: String?
    public var plistPath: String?
    public var arch: String?
    public var minKernel: String?
    public var maxKernel: String?
    public var enabled: Bool?
    public var comment: String?
    public var requires: [String]?

    enum CodingKeys: String, CodingKey {
        case order, requires
        case bundlePath = "BundlePath"
        case executablePath = "ExecutablePath"
        case plistPath = "PlistPath"
        case arch = "Arch"
        case minKernel = "MinKernel"
        case maxKernel = "MaxKernel"
        case enabled = "Enabled"
        case comment = "Comment"
    }
}

/// One OpenCore ACPI > Add entry.
public struct OpenCoreACPI: Codable, Equatable {
    public var order: Int?
    public var path: String
    public var enabled: Bool?
    public var comment: String?

    enum CodingKeys: String, CodingKey {
        case order
        case path = "Path"
        case enabled = "Enabled"
        case comment = "Comment"
    }
}

/// A plist value in the manifest's JSON spelling: {"data": "<hex>"}, {"string": …}, {"integer": …}, {"boolean": …}.
public enum PlistValue: Codable, Equatable {
    case data(Data)
    case string(String)
    case integer(Int)
    case boolean(Bool)

    enum CodingKeys: String, CodingKey { case data, string, integer, boolean }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        if let hex = try c.decodeIfPresent(String.self, forKey: .data) {
            guard let data = Data(hex: hex) else {
                throw DecodingError.dataCorruptedError(forKey: .data, in: c, debugDescription: "not hex: \(hex)")
            }
            self = .data(data)
        } else if let s = try c.decodeIfPresent(String.self, forKey: .string) {
            self = .string(s)
        } else if let i = try c.decodeIfPresent(Int.self, forKey: .integer) {
            self = .integer(i)
        } else if let b = try c.decodeIfPresent(Bool.self, forKey: .boolean) {
            self = .boolean(b)
        } else {
            throw DecodingError.dataCorrupted(.init(codingPath: c.codingPath, debugDescription: "no plist value"))
        }
    }

    public func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        switch self {
        case .data(let d): try c.encode(d.hexString, forKey: .data)
        case .string(let s): try c.encode(s, forKey: .string)
        case .integer(let i): try c.encode(i, forKey: .integer)
        case .boolean(let b): try c.encode(b, forKey: .boolean)
        }
    }

    /// The Foundation object PropertyListSerialization writes.
    public var plistObject: Any {
        switch self {
        case .data(let d): return d
        case .string(let s): return s
        case .integer(let i): return NSNumber(value: i)
        case .boolean(let b): return NSNumber(value: b)
        }
    }

    /// True when a value read from a config.plist is this value.
    public func matches(_ object: Any?) -> Bool {
        switch self {
        case .data(let d): return (object as? Data) == d
        case .string(let s): return (object as? String) == s
        case .integer(let i): return !PlistValue.isBool(object) && (object as? Int) == i
        case .boolean(let b): return PlistValue.isBool(object) && (object as? Bool) == b
        }
    }

    /// A plist boolean (on Darwin a CFBoolean NSNumber, which would otherwise also read as 0/1).
    public static func isBool(_ object: Any?) -> Bool {
        #if canImport(Darwin)
            guard let n = object as? NSNumber else { return false }
            return CFGetTypeID(n) == CFBooleanGetTypeID()
        #else
            return object is Bool
        #endif
    }

    public var display: String {
        switch self {
        case .data(let d): return "<\(d.hexString)>"
        case .string(let s): return "\"\(s)\""
        case .integer(let i): return String(i)
        case .boolean(let b): return b ? "true" : "false"
        }
    }
}

public struct ManifestConfig: Codable, Equatable {
    public var bootArgs: [String]?
    public var nvramAdd: [String: [String: PlistValue]]?
    public var nvramDelete: [String: [String]]?
    public var deviceProperties: [String: [String: PlistValue]]?
    public var forbidden: Forbidden?

    enum CodingKeys: String, CodingKey {
        case forbidden
        case bootArgs = "boot_args"
        case nvramAdd = "nvram_add"
        case nvramDelete = "nvram_delete"
        case deviceProperties = "device_properties"
    }
}

public struct Forbidden: Codable, Equatable {
    public var kexts: [String]?
    public var bootArgs: [String]?
    public var acpi: [String]?
    /// PCI path -> property names that must not be set on it (`disable-gpu` on the GP106).
    public var deviceProperties: [String: [String]]?

    enum CodingKeys: String, CodingKey {
        case kexts, acpi
        case bootArgs = "boot_args"
        case deviceProperties = "device_properties"
    }
}

/// What the release changes in OpenCore's config.plist, computed by make_manifest.py from the artifacts'
/// `opencore` / `acpi` entries and `config` (one source; the app never derives it again).
public struct ConfigDelta: Codable, Equatable {
    public var kernelAdd: [OpenCoreKext]
    public var acpiAdd: [OpenCoreACPI]
    public var deviceProperties: [String: [String: PlistValue]]
    public var nvramAdd: [String: [String: PlistValue]]
    public var nvramDelete: [String: [String]]
    public var bootArgsRequired: [String]
    public var forbidden: Forbidden?

    public init(
        kernelAdd: [OpenCoreKext] = [], acpiAdd: [OpenCoreACPI] = [],
        deviceProperties: [String: [String: PlistValue]] = [:], nvramAdd: [String: [String: PlistValue]] = [:],
        nvramDelete: [String: [String]] = [:], bootArgsRequired: [String] = [], forbidden: Forbidden? = nil
    ) {
        self.kernelAdd = kernelAdd
        self.acpiAdd = acpiAdd
        self.deviceProperties = deviceProperties
        self.nvramAdd = nvramAdd
        self.nvramDelete = nvramDelete
        self.bootArgsRequired = bootArgsRequired
        self.forbidden = forbidden
    }

    enum CodingKeys: String, CodingKey {
        case forbidden
        case kernelAdd = "kernel_add"
        case acpiAdd = "acpi_add"
        case deviceProperties = "device_properties"
        case nvramAdd = "nvram_add"
        case nvramDelete = "nvram_delete"
        case bootArgsRequired = "boot_args_required"
    }
}

public struct ExternalKext: Codable, Equatable {
    public var name: String
    public var source: String?
    public var minVersion: String?

    enum CodingKeys: String, CodingKey {
        case name, source
        case minVersion = "min_version"
    }
}

/// Artifacts that must be updated in the same restart: `always`, or `abi-change` (only when `abi` differs
/// between the installed and the new release, e.g. the PG106 kernel interface PG106FB/PG106Accel call).
public struct UpdateGroup: Codable, Equatable {
    public var name: String
    public var members: [String]
    public var together: String
    public var abi: String?
    public var what: String?
}

/// The third-party kexts the EFI uses that come from Dortania's build repo (the source 1401 uses), by product.
public struct ThirdPartyKexts: Codable, Equatable {
    public var source: String
    public var products: [String: [KextRef]]
}

/// The Help sheet's text: the ways back when a change keeps macOS from starting, and boot-order advice.
public struct AppHelp: Codable, Equatable {
    public var sections: [HelpSection]
}

public struct HelpSection: Codable, Equatable {
    public var title: String
    public var lines: [String]
}

/// A kext bundle and its CFBundleIdentifier (how kmutil reports its loaded version).
public struct KextRef: Codable, Equatable {
    public var bundle: String
    public var id: String
}

public struct PostInstallStep: Codable, Equatable {
    public var step: String
    public var order: Int?
    public var what: String
    public var artifacts: [String]
    public var group: String?
    public var choice: Int?
    public var recommended: Bool?
    public var script: String?
    public var run: [String: String]?
    public var cwd: String?
    public var reboot: Bool?
    public var needs: [String]?
    public var rerunAfterMacOSUpdate: Bool?
    public var notes: [String]?
    public var app: StepApp?

    enum CodingKeys: String, CodingKey {
        case step, order, what, artifacts, group, choice, recommended, script, run, cwd, reboot, needs, notes, app
        case rerunAfterMacOSUpdate = "rerun_after_macos_update"
    }
}

/// The app's view of a step: its title, status probes and, for a step installed in parts (the GPU stack, one
/// part per boot), the parts.
public struct StepApp: Codable, Equatable {
    public var title: String
    public var driverChange: Bool?
    public var undoNote: String?
    public var detect: Detect?
    public var parts: [StepPart]?

    enum CodingKeys: String, CodingKey {
        case title, detect, parts
        case driverChange = "driver_change"
        case undoNote = "undo_note"
    }
}

public struct StepPart: Codable, Equatable {
    public var id: String
    public var title: String
    public var env: [String: String]?
    public var artifacts: [String]?
    public var reboot: Bool?
    public var driverChange: Bool?
    public var detect: Detect?

    enum CodingKeys: String, CodingKey {
        case id, title, env, artifacts, reboot, detect
        case driverChange = "driver_change"
    }
}

/// Declarative status checks: all `installed` probes true = installed; all `active` true = in effect now (a
/// kext loaded, a patch booted); any `available` probe false = cannot be done here (with its `why_not`).
public struct Detect: Codable, Equatable {
    public var installed: [Probe]?
    public var active: [Probe]?
    public var available: [Probe]?
    public var failedHint: String?

    enum CodingKeys: String, CodingKey {
        case installed, active, available
        case failedHint = "failed_hint"
    }
}

/// One status probe. `probe` is the kind; the other fields are its parameters (see Probes.swift).
public struct Probe: Codable, Equatable {
    public var probe: String
    public var label: String?
    public var path: String?
    public var bundleId: String?
    public var ioClass: String?
    public var key: String?
    public var type: String?
    public var items: String?
    public var arg: String?
    public var variable: String?
    public var port: String?
    public var equals: String?
    public var contains: String?
    public var negate: Bool?
    public var format: String?
    public var isPrivate: Bool?
    public var whyNot: String?

    public init(
        probe: String, path: String? = nil, bundleId: String? = nil, ioClass: String? = nil, key: String? = nil,
        equals: String? = nil, negate: Bool? = nil, whyNot: String? = nil
    ) {
        self.probe = probe
        self.path = path
        self.bundleId = bundleId
        self.ioClass = ioClass
        self.key = key
        self.equals = equals
        self.negate = negate
        self.whyNot = whyNot
    }

    enum CodingKeys: String, CodingKey {
        case probe, label, path, key, type, items, arg, port, equals, contains, negate, format
        case bundleId = "bundle_id"
        case ioClass = "class"
        case variable = "var"
        case isPrivate = "private"
        case whyNot = "why_not"
    }

    /// True when the probe states an expectation (otherwise it only reports a value).
    public var hasExpectation: Bool {
        if format == "info" { return false }
        return equals != nil || contains != nil || ["file", "kext", "bootarg", "filevault-off"].contains(probe)
    }
}

/// One row of the Status page: a component and the probes that describe it.
public struct HealthItem: Codable, Equatable {
    public var id: String
    public var title: String
    public var probes: [Probe]
}

public enum ManifestError: Error, CustomStringConvertible, Equatable {
    case unreadable(String)
    case notUsable(String)

    public var description: String {
        switch self {
        case .unreadable(let why): return "manifest.json cannot be read: \(why)"
        case .notUsable(let why): return "the macos_helios300 manifest is not usable: \(why)"
        }
    }
}

extension Data {
    /// Bytes from an even-length hex string; nil for anything else.
    public init?(hex: String) {
        let chars = Array(hex.utf8)
        guard chars.count % 2 == 0 else { return nil }
        var bytes = [UInt8]()
        bytes.reserveCapacity(chars.count / 2)
        var i = 0
        while i < chars.count {
            guard let hi = Self.nibble(chars[i]), let lo = Self.nibble(chars[i + 1]) else { return nil }
            bytes.append(hi << 4 | lo)
            i += 2
        }
        self.init(bytes)
    }

    private static func nibble(_ c: UInt8) -> UInt8? {
        switch c {
        case 48...57: return c - 48
        case 65...70: return c - 55
        case 97...102: return c - 87
        default: return nil
        }
    }

    public var hexString: String { map { String(format: "%02x", $0) }.joined() }
}
