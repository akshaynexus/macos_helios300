// SPDX-License-Identifier: GPL-2.0-only
//
// GPUApps: which apps render on the GTX 1060 (render offload: the app draws on the GP106 and its frames are copied
// to the Intel HD 630, which drives the internal panel, as prime-run does on Linux). The plugin reads
// /Library/GPUBundles/nvmtl-allow.txt; changes go through the release's gpu-apps.sh (add / remove as root, run
// as the user), so the list is written exactly one way. `decide` mirrors gpu-apps.sh decide and the plugin's
// nvmtl_process_is_allowed: first matching rule wins, "-name" denies, "*" allows every process.
import Foundation

public struct AppEntry: Equatable {
    public var name: String
    public var path: String
    public var executable: String
}

public enum GPUApps {
    public static let allowList = "/Library/GPUBundles/nvmtl-allow.txt"
    public static let script = "gpu-apps.sh"

    /// The rules: non-comment, non-empty lines, trimmed.
    public static func rules(_ text: String) -> [String] {
        text.split(separator: "\n").map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty && !$0.hasPrefix("#") }
    }

    public enum Decision: Equatable { case allowed, denied, notListed }

    public static func decide(_ process: String, rules: [String]) -> Decision {
        for r in rules {
            if r.hasPrefix("-") {
                if r.dropFirst().trimmingCharacters(in: .whitespaces) == process { return .denied }
            } else if r == "*" || r == process {
                return .allowed
            }
        }
        return .notListed
    }

    /// Apps in the given folders (one level deep), with their CFBundleExecutable.
    public static func installedApps(in dirs: [String], fs: FileSystem) -> [AppEntry] {
        var out: [AppEntry] = []
        for dir in dirs {
            for name in (try? fs.list(dir)) ?? [] where name.hasSuffix(".app") {
                let path = (dir as NSString).appendingPathComponent(name)
                let plist = (path as NSString).appendingPathComponent("Contents/Info.plist")
                guard let data = try? fs.read(plist),
                    let info = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any],
                    let exe = info["CFBundleExecutable"] as? String
                else { continue }
                out.append(AppEntry(name: String(name.dropLast(4)), path: path, executable: exe))
            }
        }
        return out.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }

    /// The gpu-apps.sh command line for a change (as root) or a one-off run (as the user).
    public static func command(_ verb: String, app: String, scriptPath: String) -> [String] {
        [Tool.bash, scriptPath, verb, app]
    }
}
