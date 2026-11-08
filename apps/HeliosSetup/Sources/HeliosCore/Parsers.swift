// SPDX-License-Identifier: GPL-2.0-only
//
// Parsers for the text macOS tools print. Each takes the captured text and nothing else, so the tests feed them
// real (sanitized) captures from the Helios: Tests/HeliosCoreTests/Fixtures. The formats are the ones
// tools/macos/*.sh already rely on (ioprop's `"Key" = value`, kmutil's `<id> (<version>)`).
import Foundation

public enum IORegParser {
    /// `ioreg -r -c <class> -d 1` (or -l) text -> {key: value}. The first occurrence of a key wins (the class's
    /// own node is printed first); string values lose their quotes, others stay as printed (Yes, 4, <0a>, {...}).
    public static func properties(_ text: String) -> [String: String] {
        var out: [String: String] = [:]
        for line in text.split(separator: "\n", omittingEmptySubsequences: true) {
            guard let (key, value) = property(String(line)), out[key] == nil else { continue }
            out[key] = value
        }
        return out
    }

    /// True when the text shows at least one registry node (`+-o Name  <class ...>`).
    public static func hasNode(_ text: String) -> Bool {
        text.contains("+-o ")
    }

    static func property(_ line: String) -> (String, String)? {
        var rest = Substring(line)
        while let c = rest.first, c == " " || c == "|" || c == "\t" { rest = rest.dropFirst() }
        guard rest.hasPrefix("\"") else { return nil }
        rest = rest.dropFirst()
        guard let close = rest.firstIndex(of: "\"") else { return nil }
        let key = String(rest[..<close])
        let after = rest[rest.index(after: close)...]
        guard after.hasPrefix(" = ") else { return nil }
        var value = String(after.dropFirst(3)).trimmingCharacters(in: .whitespaces)
        if value.count >= 2, value.hasPrefix("\""), value.hasSuffix("\"") {
            value = String(value.dropFirst().dropLast())
        }
        return (key, value)
    }
}

public enum KmutilParser {
    /// `kmutil showloaded` -> {bundle id: version}. Lines: index refs address size wired <id> (<version>) UUID <deps>.
    public static func loaded(_ text: String) -> [String: String] {
        var out: [String: String] = [:]
        for line in text.split(separator: "\n") {
            let fields = line.split(separator: " ", omittingEmptySubsequences: true)
            guard fields.count >= 7, Int(fields[0]) != nil, fields[2].hasPrefix("0x") else { continue }
            let id = String(fields[5])
            var version = String(fields[6])
            guard version.hasPrefix("("), version.hasSuffix(")") else { continue }
            version = String(version.dropFirst().dropLast())
            out[id] = version
        }
        return out
    }
}

public enum PmsetParser {
    /// `pmset -g` -> {setting: value}, e.g. SleepDisabled -> "1", sleep -> "0 (sleep prevented by ...)".
    public static func settings(_ text: String) -> [String: String] {
        var out: [String: String] = [:]
        for raw in text.split(separator: "\n") {
            let line = raw.trimmingCharacters(in: .whitespaces)
            guard !line.isEmpty, !line.hasSuffix(":") else { continue }
            let parts = line.split(maxSplits: 1, whereSeparator: { $0 == " " || $0 == "\t" })
            guard parts.count == 2 else { continue }
            let key = String(parts[0])
            if out[key] == nil { out[key] = parts[1].trimmingCharacters(in: .whitespaces) }
        }
        return out
    }
}

public enum NVRAMParser {
    /// `nvram <name>` -> the value (after the first tab), or nil when the variable is absent.
    public static func value(_ text: String) -> String? {
        guard let line = text.split(separator: "\n").first, let tab = line.firstIndex(of: "\t") else { return nil }
        return String(line[line.index(after: tab)...])
    }

    /// The words of boot-args.
    public static func bootArgs(_ text: String) -> [String] {
        (value(text) ?? "").split(separator: " ").map(String.init)
    }

    /// The GPT partition UUID in an OpenCore boot-path device path (`.../HD(1,GPT,<uuid>,0x28,0x64000)/\EFI\...`).
    public static func gptPartition(inDevicePath path: String) -> String? {
        guard let r = path.range(of: #"HD\([0-9]+,GPT,([0-9A-Fa-f-]{36})"#, options: .regularExpression) else { return nil }
        let match = String(path[r])
        return match.split(separator: ",").dropFirst(2).first.map { $0.uppercased() }
    }

    /// True when the device path goes through a USB node.
    public static func isUSB(_ path: String) -> Bool { path.contains("/USB(") || path.contains("/UsbClass(") }
}

/// `system_profiler <Type>` text as a tree: each line "Name:" opens a node, "Key: value" is a value of the node
/// above it (by indentation). With -detailLevel mini macOS leaves out serial numbers.
public final class ProfilerNode {
    public let name: String
    public var value: String?
    public var children: [ProfilerNode] = []
    let indent: Int

    init(name: String, value: String?, indent: Int) {
        self.name = name
        self.value = value
        self.indent = indent
    }

    public static func parse(_ text: String) -> ProfilerNode {
        let root = ProfilerNode(name: "", value: nil, indent: -1)
        var stack = [root]
        for raw in text.split(separator: "\n") {
            let line = String(raw)
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            guard !trimmed.isEmpty, let colon = trimmed.firstIndex(of: ":") else { continue }
            let indent = line.prefix { $0 == " " }.count
            let name = String(trimmed[..<colon])
            let rest = trimmed[trimmed.index(after: colon)...].trimmingCharacters(in: .whitespaces)
            while let top = stack.last, top.indent >= indent, stack.count > 1 { stack.removeLast() }
            let node = ProfilerNode(name: name, value: rest.isEmpty ? nil : rest, indent: indent)
            stack.last?.children.append(node)
            if rest.isEmpty { stack.append(node) }
        }
        return root
    }

    /// The first value of `key` anywhere below this node (depth first, in print order).
    public func firstValue(_ key: String) -> String? {
        for c in children {
            if c.name == key, let v = c.value { return v }
            if let v = c.firstValue(key) { return v }
        }
        return nil
    }

    /// The first node named `name` below this one.
    public func find(_ name: String) -> ProfilerNode? {
        for c in children {
            if c.name == name { return c }
            if let n = c.find(name) { return n }
        }
        return nil
    }
}

public enum NetworkParser {
    /// `networksetup -listallhardwareports` -> {hardware port: device}, e.g. "Wi-Fi" -> "en1".
    public static func ports(_ text: String) -> [String: String] {
        var out: [String: String] = [:]
        var port: String?
        for raw in text.split(separator: "\n") {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("Hardware Port: ") {
                port = String(line.dropFirst("Hardware Port: ".count))
            } else if line.hasPrefix("Device: "), let p = port {
                out[p] = String(line.dropFirst("Device: ".count))
                port = nil
            }
        }
        return out
    }

    /// `ipconfig getsummary <if>` -> the SSID line's value.
    public static func ssid(_ text: String) -> String? {
        for raw in text.split(separator: "\n") {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("SSID : ") { return String(line.dropFirst("SSID : ".count)) }
        }
        return nil
    }

    /// A Wi-Fi channel as its band.
    public static func band(channel: String) -> String {
        guard let ch = Int(channel), ch > 0 else { return channel }
        return ch <= 14 ? "2.4 GHz (channel \(ch))" : "5 GHz (channel \(ch))"
    }
}

/// `diskutil list -plist ...` and `diskutil info -plist ...`.
public enum DiskutilParser {
    /// The EFI system partitions in `diskutil list -plist internal physical`: their device identifiers (disk0s1).
    public static func efiPartitions(_ plist: Data) -> [String] {
        guard let root = try? PropertyListSerialization.propertyList(from: plist, format: nil) as? [String: Any],
            let disks = root["AllDisksAndPartitions"] as? [[String: Any]]
        else { return [] }
        var out: [String] = []
        for disk in disks {
            for part in disk["Partitions"] as? [[String: Any]] ?? [] {
                if (part["Content"] as? String) == "EFI", let id = part["DeviceIdentifier"] as? String { out.append(id) }
            }
        }
        return out
    }

    /// `diskutil info -plist <dev>` -> (mount point or nil, partition UUID or nil, total size).
    public static func info(_ plist: Data) -> (mountPoint: String?, partitionUUID: String?, size: Int64?) {
        guard let root = try? PropertyListSerialization.propertyList(from: plist, format: nil) as? [String: Any] else {
            return (nil, nil, nil)
        }
        let mp = (root["MountPoint"] as? String).flatMap { $0.isEmpty ? nil : $0 }
        let uuid = (root["DiskUUID"] as? String)?.uppercased()
        let size = (root["TotalSize"] as? NSNumber)?.int64Value ?? (root["Size"] as? NSNumber)?.int64Value
        return (mp, uuid, size)
    }
}

/// OpenCore version strings: `opencore-version` NVRAM / OpenCore.efi ("REL-105-2025-05-05") and ocvalidate's
/// "compatible with OpenCore version 1.0.5".
public enum OpenCoreVersion {
    /// "REL-105-..." or "DBG-105-..." -> "1.0.5".
    public static func fromBuildString(_ s: String) -> String? {
        guard let r = s.range(of: #"(REL|DBG|NPT)-([0-9]{3})-[0-9]{4}-[0-9]{2}-[0-9]{2}"#, options: .regularExpression)
        else { return nil }
        let digits = Array(s[r].split(separator: "-")[1])
        return "\(digits[0]).\(digits[1]).\(digits[2])"
    }

    /// The OpenCore version an ocvalidate binary is for, from its output.
    public static func fromOcvalidate(_ output: String) -> String? {
        guard let r = output.range(of: #"OpenCore version ([0-9]+\.[0-9]+\.[0-9]+)"#, options: .regularExpression)
        else { return nil }
        return output[r].split(separator: " ").last.map(String.init)
    }

    /// The build string embedded in an OpenCore.efi binary (it carries "REL-105-2025-05-05" as ASCII).
    public static func fromBinary(_ data: Data) -> String? {
        let ascii = String(decoding: data.map { (32...126).contains($0) ? $0 : 10 }, as: UTF8.self)
        return fromBuildString(ascii)
    }
}
