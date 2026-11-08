// SPDX-License-Identifier: GPL-2.0-only
//
// ConfigMerger: applies a release's config_delta to an OpenCore config.plist and never drops what the user set.
//   Kernel > Add   an entry with the same BundlePath keeps the user's Enabled and Comment, takes the release's
//                  ExecutablePath / PlistPath / Arch / MinKernel / MaxKernel; a new entry gets every Sample.plist
//                  key and goes after the kexts it requires and the release's earlier entries (docs/MANIFEST.md:
//                  "in order, after every kext they require", as 1401's apply() inserts them).
//   ACPI > Add     the same by Path.
//   DeviceProperties > Add, NVRAM > Add   the release's keys are set; every other key is left as it is.
//   NVRAM > Delete the release's names are added to the list; none is removed.
//   boot-args      required args are added (or their value updated); the user's other args stay.
//   forbidden      reported as warnings, never edited here (the user decides).
// The result lists each change, so the GUI can show it before anything is written and ocvalidate runs after.
import Foundation

public struct ConfigChange: Equatable, CustomStringConvertible {
    public enum Kind: String { case added, changed, removed, warning }
    public var kind: Kind
    public var path: String
    public var detail: String

    public var description: String { "\(kind.rawValue): \(path)\(detail.isEmpty ? "" : " (\(detail))")" }
}

public enum ConfigMerger {
    public static let bootGUID = "7C436110-AB2A-4BBB-A880-FE41995C9F82"
    static let kextKeys = ["Arch", "BundlePath", "Comment", "Enabled", "ExecutablePath", "MaxKernel", "MinKernel", "PlistPath"]

    /// The merged config and what changed. `config` is the parsed config.plist.
    public static func merge(_ config: [String: Any], delta: ConfigDelta) -> ([String: Any], [ConfigChange]) {
        var cfg = config
        var changes: [ConfigChange] = []
        mergeKernel(&cfg, delta.kernelAdd, &changes)
        mergeACPI(&cfg, delta.acpiAdd, &changes)
        mergeKeyed(&cfg, section: "DeviceProperties", delta.deviceProperties, &changes)
        mergeKeyed(&cfg, section: "NVRAM", delta.nvramAdd, &changes)
        mergeNVRAMDelete(&cfg, delta.nvramDelete, &changes)
        mergeBootArgs(&cfg, delta.bootArgsRequired, &changes)
        warnForbidden(cfg, delta.forbidden, &changes)
        return (cfg, changes)
    }

    /// Takes a dropped kext's Kernel > Add entries and a dropped SSDT's ACPI > Add entry out of `config`. Nothing else is
    /// touched. (`merge` never removes; a release that stops shipping a file must, or OpenCore would look for it.)
    public static func remove(_ config: [String: Any], kexts: [String], ssdts: [String]) -> ([String: Any], [ConfigChange]) {
        var cfg = config
        var changes: [ConfigChange] = []
        if var kernel = cfg["Kernel"] as? [String: Any], var add = kernel["Add"] as? [[String: Any]] {
            for k in add where kexts.contains(bundleName(k)) {
                changes.append(ConfigChange(kind: .removed, path: "Kernel/Add/\(k["BundlePath"] as? String ?? "")", detail: ""))
            }
            add.removeAll { kexts.contains(bundleName($0)) }
            kernel["Add"] = add
            cfg["Kernel"] = kernel
        }
        if var acpi = cfg["ACPI"] as? [String: Any], var add = acpi["Add"] as? [[String: Any]] {
            for s in add where ssdts.contains(s["Path"] as? String ?? "") {
                changes.append(ConfigChange(kind: .removed, path: "ACPI/Add/\(s["Path"] as? String ?? "")", detail: ""))
            }
            add.removeAll { ssdts.contains($0["Path"] as? String ?? "") }
            acpi["Add"] = add
            cfg["ACPI"] = acpi
        }
        return (cfg, changes)
    }

    // MARK: Kernel > Add

    static func mergeKernel(_ cfg: inout [String: Any], _ wanted: [OpenCoreKext], _ changes: inout [ConfigChange]) {
        var kernel = cfg["Kernel"] as? [String: Any] ?? [:]
        var add = kernel["Add"] as? [[String: Any]] ?? []
        var lastOurs: Int?
        for k in wanted {
            if let i = add.firstIndex(where: { ($0["BundlePath"] as? String) == k.bundlePath }) {
                update(&add[i], with: k, &changes)
                lastOurs = max(lastOurs ?? i, i)
                warnOrder(add, at: i, requires: k.requires ?? [], &changes)
            } else {
                let pos = insertPosition(add, requires: k.requires ?? [], after: lastOurs)
                add.insert(newEntry(k), at: pos)
                lastOurs = pos
                changes.append(ConfigChange(kind: .added, path: "Kernel/Add/\(k.bundlePath)", detail: "at position \(pos)"))
            }
        }
        kernel["Add"] = add
        cfg["Kernel"] = kernel
    }

    static func update(_ entry: inout [String: Any], with k: OpenCoreKext, _ changes: inout [ConfigChange]) {
        let fields: [(String, String?)] = [
            ("ExecutablePath", k.executablePath), ("PlistPath", k.plistPath),
            ("Arch", k.arch), ("MinKernel", k.minKernel), ("MaxKernel", k.maxKernel),
        ]
        for (key, value) in fields {
            guard let value, (entry[key] as? String) != value else { continue }
            let old = entry[key] as? String
            entry[key] = value
            changes.append(
                ConfigChange(
                    kind: .changed, path: "Kernel/Add/\(k.bundlePath)/\(key)",
                    detail: "\(old.map { "'\($0)'" } ?? "absent") -> '\(value)'"))
        }
        if (entry["Enabled"] as? Bool) == false {
            changes.append(
                ConfigChange(
                    kind: .warning, path: "Kernel/Add/\(k.bundlePath)",
                    detail: "disabled in your config; left disabled"))
        }
    }

    static func newEntry(_ k: OpenCoreKext) -> [String: Any] {
        [
            "Arch": k.arch ?? "x86_64", "BundlePath": k.bundlePath, "Comment": k.comment ?? "",
            "Enabled": k.enabled ?? true, "ExecutablePath": k.executablePath ?? "", "MaxKernel": k.maxKernel ?? "",
            "MinKernel": k.minKernel ?? "", "PlistPath": k.plistPath ?? "Contents/Info.plist",
        ]
    }

    /// After every kext it requires and after the release's previous entry; at the end when neither is there.
    static func insertPosition(_ add: [[String: Any]], requires: [String], after lastOurs: Int?) -> Int {
        let reqIdx = requires.compactMap { r in add.lastIndex { bundleName($0) == r } }
        let anchor = (reqIdx + [lastOurs].compactMap { $0 }).max()
        return anchor.map { $0 + 1 } ?? add.count
    }

    static func warnOrder(_ add: [[String: Any]], at i: Int, requires: [String], _ changes: inout [ConfigChange]) {
        for r in requires {
            if let j = add.lastIndex(where: { bundleName($0) == r }), j > i {
                changes.append(
                    ConfigChange(
                        kind: .warning, path: "Kernel/Add/\(bundleName(add[i]))",
                        detail: "listed before \(r), which it needs: move it below \(r)"))
            }
        }
    }

    /// "Lilu.kext" for "Lilu.kext" and for "Lilu.kext/Contents/PlugIns/...".
    static func bundleName(_ entry: [String: Any]) -> String {
        String((entry["BundlePath"] as? String ?? "").split(separator: "/").first ?? "")
    }

    // MARK: ACPI > Add

    static func mergeACPI(_ cfg: inout [String: Any], _ wanted: [OpenCoreACPI], _ changes: inout [ConfigChange]) {
        guard !wanted.isEmpty else { return }
        var acpi = cfg["ACPI"] as? [String: Any] ?? [:]
        var add = acpi["Add"] as? [[String: Any]] ?? []
        for a in wanted where !add.contains(where: { ($0["Path"] as? String) == a.path }) {
            add.append(["Comment": a.comment ?? "", "Enabled": a.enabled ?? true, "Path": a.path])
            changes.append(ConfigChange(kind: .added, path: "ACPI/Add/\(a.path)", detail: ""))
        }
        acpi["Add"] = add
        cfg["ACPI"] = acpi
    }

    // MARK: DeviceProperties / NVRAM Add

    static func mergeKeyed(
        _ cfg: inout [String: Any], section: String, _ wanted: [String: [String: PlistValue]],
        _ changes: inout [ConfigChange]
    ) {
        guard !wanted.isEmpty else { return }
        var sec = cfg[section] as? [String: Any] ?? [:]
        var add = sec["Add"] as? [String: Any] ?? [:]
        for (path, keys) in wanted.sorted(by: { $0.key < $1.key }) {
            var node = add[path] as? [String: Any] ?? [:]
            for (key, value) in keys.sorted(by: { $0.key < $1.key }) where !value.matches(node[key]) {
                let had = node[key] != nil
                node[key] = value.plistObject
                changes.append(
                    ConfigChange(
                        kind: had ? .changed : .added, path: "\(section)/Add/\(path)/\(key)",
                        detail: value.display))
            }
            add[path] = node
        }
        sec["Add"] = add
        cfg[section] = sec
    }

    static func mergeNVRAMDelete(_ cfg: inout [String: Any], _ wanted: [String: [String]], _ changes: inout [ConfigChange]) {
        guard !wanted.isEmpty else { return }
        var nv = cfg["NVRAM"] as? [String: Any] ?? [:]
        var del = nv["Delete"] as? [String: Any] ?? [:]
        for (guid, names) in wanted.sorted(by: { $0.key < $1.key }) {
            var list = del[guid] as? [String] ?? []
            for n in names where !list.contains(n) {
                list.append(n)
                changes.append(ConfigChange(kind: .added, path: "NVRAM/Delete/\(guid)/\(n)", detail: ""))
            }
            del[guid] = list
        }
        nv["Delete"] = del
        cfg["NVRAM"] = nv
    }

    // MARK: boot-args

    static func mergeBootArgs(_ cfg: inout [String: Any], _ required: [String], _ changes: inout [ConfigChange]) {
        guard !required.isEmpty else { return }
        var nv = cfg["NVRAM"] as? [String: Any] ?? [:]
        var add = nv["Add"] as? [String: Any] ?? [:]
        var boot = add[bootGUID] as? [String: Any] ?? [:]
        var args = (boot["boot-args"] as? String ?? "").split(separator: " ").map(String.init)
        for r in required {
            let name = argName(r)
            if let i = args.firstIndex(where: { argName($0) == name }) {
                if args[i] != r {
                    changes.append(ConfigChange(kind: .changed, path: "boot-args/\(name)", detail: "'\(args[i])' -> '\(r)'"))
                    args[i] = r
                }
            } else {
                args.append(r)
                changes.append(ConfigChange(kind: .added, path: "boot-args/\(name)", detail: r))
            }
        }
        boot["boot-args"] = args.joined(separator: " ")
        add[bootGUID] = boot
        nv["Add"] = add
        cfg["NVRAM"] = nv
    }

    static func argName(_ arg: String) -> String { String(arg.split(separator: "=", maxSplits: 1).first ?? "") }

    // MARK: forbidden

    static func warnForbidden(_ cfg: [String: Any], _ f: Forbidden?, _ changes: inout [ConfigChange]) {
        guard let f else { return }
        let kexts = (cfg["Kernel"] as? [String: Any])?["Add"] as? [[String: Any]] ?? []
        for k in kexts where (k["Enabled"] as? Bool) == true && (f.kexts ?? []).contains(bundleName(k)) {
            changes.append(
                ConfigChange(
                    kind: .warning, path: "Kernel/Add/\(bundleName(k))",
                    detail: "enabled, but the release forbids it on this machine: disable it"))
        }
        let boot = (((cfg["NVRAM"] as? [String: Any])?["Add"] as? [String: Any])?[bootGUID] as? [String: Any])?["boot-args"]
        let args = (boot as? String ?? "").split(separator: " ").map(String.init)
        for a in f.bootArgs ?? [] where args.contains(a) {
            changes.append(ConfigChange(kind: .warning, path: "boot-args/\(a)", detail: "forbidden by the release: remove it"))
        }
        let ssdts = (cfg["ACPI"] as? [String: Any])?["Add"] as? [[String: Any]] ?? []
        for s in ssdts where (s["Enabled"] as? Bool) == true && (f.acpi ?? []).contains(s["Path"] as? String ?? "") {
            changes.append(
                ConfigChange(
                    kind: .warning, path: "ACPI/Add/\(s["Path"] as? String ?? "")",
                    detail: "forbidden by the release: disable it"))
        }
        let properties = (cfg["DeviceProperties"] as? [String: Any])?["Add"] as? [String: Any] ?? [:]
        for (path, names) in (f.deviceProperties ?? [:]).sorted(by: { $0.key < $1.key }) {
            for name in names where (properties[path] as? [String: Any])?[name] != nil {
                changes.append(
                    ConfigChange(
                        kind: .warning, path: "DeviceProperties/Add/\(path)/\(name)",
                        detail: "forbidden by the release: remove it"))
            }
        }
    }

    // MARK: plist I/O

    public static func read(_ data: Data) throws -> [String: Any] {
        guard let d = try PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any] else {
            throw ConfigError.notADictionary
        }
        return d
    }

    public static func write(_ config: [String: Any]) throws -> Data {
        try PropertyListSerialization.data(fromPropertyList: config, format: .xml, options: 0)
    }
}

public enum ConfigError: Error, CustomStringConvertible, Equatable {
    case notADictionary
    case validationFailed(String)

    public var description: String {
        switch self {
        case .notADictionary: return "config.plist is not a property list dictionary"
        case .validationFailed(let out): return "ocvalidate reports problems:\n\(out)"
        }
    }
}
