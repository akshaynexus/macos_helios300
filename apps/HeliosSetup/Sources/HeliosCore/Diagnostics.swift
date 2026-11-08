// SPDX-License-Identifier: GPL-2.0-only
//
// Diagnostics: the Status page's "Copy diagnostics" text, for sending to the developer. It is built from the same
// probes the page shows plus the release's kexts in kmutil, then sanitized: MAC addresses (aa:bb:..., and 6-byte
// <...> registry data), IPv6 addresses, serial numbers, hardware/platform UUIDs and the network name are replaced.
// The rules follow kexts/wifi/evidence/macos/sanitize.py (the repo's evidence sanitizer).
import Foundation

public enum Sanitizer {
    static let rules: [(String, String)] = [
        (#"(?i)\b(?!00:00:00:00:00:00\b)(?!ff:ff:ff:ff:ff:ff\b)([0-9a-f]{2}[:-]){5}[0-9a-f]{2}\b"#, "xx:xx:xx:xx:xx:xx"),
        (#"ssid="[^"]+""#, "ssid=\"<redacted>\""),
        (#"\b[A-Za-z0-9]+s-MacBook-Pro\b"#, "HOST"),
        (#"/Users/[A-Za-z0-9._-]+"#, "/Users/USER"),
        (#"\b(192\.168|10\.\d+|172\.(1[6-9]|2\d|3[01]))\.\d+\.\d+\b"#, "192.0.2.x"),
        (
            #"(?i)("(?:IOMACAddress|MACAddress|BSSID|[A-Za-z0-9]*(?:MAC|BSSID)[A-Za-z0-9]*)" = )<(?!0{12}>)[0-9a-f]{12}>"#,
            "$1<xxxxxxxxxxxx>"
        ),
        // IPv6 (also "::" forms), but not a MAC-shaped string (left as is above: all-zero / broadcast)
        (
            #"(?i)(?<![0-9a-f:])(?!(?:[0-9a-f]{2}:){5}[0-9a-f]{2}(?![0-9a-f:]))[0-9a-f]{1,4}(?::[0-9a-f]{0,4}){3,7}(?![0-9a-f:])"#,
            "<ipv6>"
        ),
        (
            #"(?i)((?:serial(?: number)?|IOPlatformSerialNumber|SerialNumber|kUSBSerialNumberString)[^:=\n]*[:=]\s*"?)[A-Za-z0-9-]{4,}"#,
            "$1<serial>"
        ),
        (#"(?i)\b[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\b"#, "<uuid>"),
    ]

    /// `secrets`: exact strings to remove as well (the SSID the probes read, the user name).
    public static func clean(_ text: String, secrets: [String] = []) -> String {
        var out = text
        for s in secrets where s.count >= 2 {
            out = out.replacingOccurrences(of: s, with: "<redacted>")
        }
        for (pattern, template) in rules {
            guard let re = try? NSRegularExpression(pattern: pattern) else { continue }
            out = re.stringByReplacingMatches(in: out, range: NSRange(out.startIndex..., in: out), withTemplate: template)
        }
        return out
    }
}

public struct HealthRow: Equatable {
    public var id: String
    public var title: String
    public var lines: [(label: String, result: ProbeResult)]
    /// true all expectations met, false one failed, nil only values.
    public var ok: Bool?

    public static func == (a: HealthRow, b: HealthRow) -> Bool {
        a.id == b.id && a.ok == b.ok
            && a.lines.map { "\($0.label)=\($0.result.value)" } == b.lines.map { "\($0.label)=\($0.result.value)" }
    }
}

public enum Health {
    /// Evaluates the manifest's `health` table.
    public static func rows(_ m: Manifest, ctx: ProbeContext) -> [HealthRow] {
        (m.health ?? []).map { item in
            let lines = item.probes.map { p -> (label: String, result: ProbeResult) in
                var r = ProbeEvaluator.evaluate(p, in: ctx)
                if !p.hasExpectation { r.ok = nil }
                return (label: p.label ?? p.key ?? p.probe, result: r)
            }
            let checks = lines.compactMap(\.result.ok)
            return HealthRow(id: item.id, title: item.title, lines: lines, ok: checks.isEmpty ? nil : !checks.contains(false))
        }
    }

    /// The text for "Copy diagnostics": health, macOS, the release's kexts as loaded, steps; sanitized.
    public static func diagnostics(
        _ m: Manifest, rows: [HealthRow], statuses: [(SetupItem, ItemStatus)],
        ctx: ProbeContext, appVersion: String
    ) -> String {
        var out = [
            "Helios 300 Setup \(appVersion) diagnostics", "release: \(m.release?.tag ?? "spec (no release folder)")",
            "macOS build \(ctx.build), Darwin \(ctx.darwin)", "boot-args: \(ctx.bootArgs.joined(separator: " "))",
            "boot source: \(ctx.bootSource)", "",
        ]
        for r in rows {
            out.append("[\(r.ok.map { $0 ? "ok" : "PROBLEM" } ?? "info")] \(r.title)")
            for l in r.lines { out.append("    \(l.label): \(l.result.isPrivate ? "<hidden>" : l.result.value)") }
        }
        out += ["", "Steps:"]
        for (item, st) in statuses { out.append("    \(item.id): \(st.label)\(st.detail.isEmpty ? "" : " - \(st.detail)")") }
        out += ["", "Kexts of this release (kmutil showloaded):"]
        let ids = Set(m.artifacts.compactMap(\.bundleId))
        for (id, v) in ctx.loadedKexts.sorted(by: { $0.key < $1.key }) where ids.contains(id) { out.append("    \(id) (\(v))") }
        let secrets = rows.flatMap(\.lines).filter(\.result.isPrivate).map(\.result.value).filter { $0 != "absent" }
        return Sanitizer.clean(out.joined(separator: "\n"), secrets: secrets)
    }
}
