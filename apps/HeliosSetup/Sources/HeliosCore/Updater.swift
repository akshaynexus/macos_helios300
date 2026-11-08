// SPDX-License-Identifier: GPL-2.0-only
//
// Updater: finds a newer macos_helios300 release, downloads it and verifies every byte before anything is applied.
//   - Releases come from the GitHub API (the list 1401's resolve_latest reads); drafts are skipped, pre-releases
//     only when allowed, and only a release that carries manifest.json counts.
//   - Versions compare as 1401's _vkey: (major, minor, patch, is-final).
//   - The manifest is fetched from the release download URL, checked with ManifestCheck (1401's rules) and must
//     say the version its tag names. Each file must match its sha256 and size; zips are extracted and must hold
//     exactly their contents_sha256 files. Unchanged files are copied from the installed release (re-verified).
//   - The changelog is the release notes plus the per-artifact diff.
// No update is ever installed from here: the UI (Sparkle-style: notes, Remind me later, Skip, Install) or the CLI
// asks, then UpdateApplier does it as root. OCLP's support/updates.py does the same split (check, then offer).
import Foundation

public struct ReleaseCandidate: Equatable {
    public var tag: String
    public var version: String
    public var preRelease: Bool
    public var notes: String
    public var htmlURL: String
    public var published: String
}

public enum ReleaseList {
    public static let api = "https://api.github.com/repos/\(ManifestCheck.repo)/releases?per_page=30"

    /// The GitHub API's release list -> candidates that carry a manifest.json, newest first.
    public static func parse(_ data: Data) -> [ReleaseCandidate] {
        guard let list = (try? JSONSerialization.jsonObject(with: data)) as? [[String: Any]] else { return [] }
        let found: [ReleaseCandidate] = list.compactMap { r in
            guard (r["draft"] as? Bool) != true, let tag = r["tag_name"] as? String,
                versionKey(tag).major >= 0,
                (r["assets"] as? [[String: Any]] ?? []).contains(where: { ($0["name"] as? String) == "manifest.json" })
            else { return nil }
            return ReleaseCandidate(
                tag: tag, version: String(tag.dropFirst(tag.hasPrefix("v") ? 1 : 0)),
                preRelease: (r["prerelease"] as? Bool) ?? false, notes: r["body"] as? String ?? "",
                htmlURL: r["html_url"] as? String ?? "", published: r["published_at"] as? String ?? "")
        }
        return found.sorted { isNewer($0.tag, than: $1.tag) }
    }

    /// The newest candidate newer than `installed` (a version or tag), honouring the pre-release choice.
    public static func newer(_ list: [ReleaseCandidate], than installed: String?, includePrereleases: Bool) -> ReleaseCandidate? {
        list.first { c in
            (includePrereleases || !c.preRelease) && (installed.map { isNewer(c.tag, than: $0) } ?? true)
        }
    }

    /// helios300._vkey.
    public static func versionKey(_ tag: String) -> (major: Int, minor: Int, patch: Int, final: Bool) {
        let pattern = #"^v?(\d+)\.(\d+)\.(\d+)(-.*)?$"#
        guard tag.range(of: pattern, options: .regularExpression) != nil else { return (-1, -1, -1, false) }
        let core = String(tag.drop { $0 == "v" })
        let dash = core.firstIndex(of: "-")
        let nums = core[..<(dash ?? core.endIndex)].split(separator: ".").compactMap { Int($0) }
        guard nums.count == 3 else { return (-1, -1, -1, false) }
        return (nums[0], nums[1], nums[2], dash == nil)
    }

    public static func isNewer(_ a: String, than b: String) -> Bool {
        let x = versionKey(a), y = versionKey(b)
        return (x.major, x.minor, x.patch, x.final ? 1 : 0) > (y.major, y.minor, y.patch, y.final ? 1 : 0)
    }
}

/// What changed between two manifests.
public struct ManifestDiff: Equatable {
    public enum Change: String { case added, changed, removed }
    public var artifacts: [(name: String, change: Change, from: String?, to: String?)]
    public var configChanged: Bool

    public static func == (a: ManifestDiff, b: ManifestDiff) -> Bool {
        a.configChanged == b.configChanged
            && a.artifacts.map { "\($0.name)|\($0.change)|\($0.from ?? "")|\($0.to ?? "")" }
                == b.artifacts.map { "\($0.name)|\($0.change)|\($0.from ?? "")|\($0.to ?? "")" }
    }

    public static func between(_ old: Manifest?, _ new: Manifest) -> ManifestDiff {
        let before = Dictionary((old?.artifacts ?? []).map { ($0.name, $0) }, uniquingKeysWith: { a, _ in a })
        var out: [(String, Change, String?, String?)] = []
        for a in new.artifacts {
            if let b = before[a.name] {
                if b.sha256 != a.sha256 || b.opencore != a.opencore || b.acpi != a.acpi {
                    out.append((a.name, .changed, b.version, a.version))
                }
            } else {
                out.append((a.name, .added, nil, a.version))
            }
        }
        let now = Set(new.artifacts.map(\.name))
        for b in old?.artifacts ?? [] where !now.contains(b.name) { out.append((b.name, .removed, b.version, nil)) }
        let configChanged = settingsPart(old?.configDelta) != settingsPart(new.configDelta)
        return ManifestDiff(artifacts: out.map { (name: $0.0, change: $0.1, from: $0.2, to: $0.3) }, configChanged: configChanged)
    }

    /// The part of a config delta that is not tied to an artifact (Kernel and ACPI entries travel with theirs).
    static func settingsPart(_ d: ConfigDelta?) -> ConfigDelta {
        var s = d ?? ConfigDelta()
        s.kernelAdd = []
        s.acpiAdd = []
        s.forbidden = nil
        return s
    }

    public func names(_ c: Change) -> [String] { artifacts.filter { $0.change == c }.map(\.name) }

    /// The changelog text: release notes, then one line per artifact.
    public func changelog(notes: String, from: String?, to: String) -> String {
        var lines = ["macos_helios300 \(from.map { "\($0) -> " } ?? "")\(to)", ""]
        if !notes.isEmpty { lines += [notes.trimmingCharacters(in: .whitespacesAndNewlines), ""] }
        lines.append("Changed files:")
        for a in artifacts {
            switch a.change {
            case .added: lines.append("  + \(a.name) \(a.to ?? "")")
            case .changed: lines.append("  ~ \(a.name) \(a.from ?? "?") -> \(a.to ?? "?")")
            case .removed: lines.append("  - \(a.name)")
            }
        }
        if artifacts.isEmpty { lines.append("  (none)") }
        if configChanged { lines.append("  ~ OpenCore config.plist changes (config_delta)") }
        return lines.joined(separator: "\n")
    }
}

/// Extracts a zip (ditto on the Mac, as install-gpu.sh does; unzip elsewhere).
public protocol Unzipper {
    func extract(_ zip: String, into dir: String) throws
}

public struct CommandUnzipper: Unzipper {
    let runner: CommandRunner
    let fs: FileSystem

    public init(runner: CommandRunner = ProcessRunner(), fs: FileSystem = LocalFileSystem()) {
        self.runner = runner
        self.fs = fs
    }

    public func extract(_ zip: String, into dir: String) throws {
        try fs.makeDirectory(dir)
        let argv = fs.exists(Tool.ditto) ? [Tool.ditto, "-x", "-k", zip, dir] : ["/usr/bin/unzip", "-q", "-o", zip, "-d", dir]
        let r = runner.capture(argv, env: nil, cwd: nil)
        if !r.ok { throw UpdateError.unzip(zip, r.stderr) }
    }
}

public enum UpdateError: Error, CustomStringConvertible, Equatable {
    case noRelease
    case tagMismatch(String, String)
    case unzip(String, String)
    case download(String, String)
    case manifestChanged(String)

    public var description: String {
        switch self {
        case .noRelease: return "no newer macos_helios300 release"
        case .tagMismatch(let tag, let v): return "the \(tag) manifest says v\(v)"
        case .unzip(let z, let why): return "cannot unzip \(z): \(why)"
        case .download(let f, let why): return "\(f) could not be downloaded: \(why)"
        case .manifestChanged(let p): return "\(p) is not the manifest that was checked (its sha256 changed)"
        }
    }
}

/// A downloaded, verified release on disk: manifest.json + every file + `x/<name>` extracted bundles.
public struct StagedRelease: Equatable {
    public var dir: String
    public var manifest: Manifest
    public var manifestSha256: String
}

public struct Updater {
    let http: HTTPClient
    let fs: FileSystem
    let unzipper: Unzipper

    public init(http: HTTPClient, fs: FileSystem, unzipper: Unzipper) {
        self.http = http
        self.fs = fs
        self.unzipper = unzipper
    }

    public func releases() throws -> [ReleaseCandidate] {
        guard let url = URL(string: ReleaseList.api) else { return [] }
        return ReleaseList.parse(try http.get(url, headers: ["Accept": "application/vnd.github+json"]))
    }

    /// helios300.load_release for one tag: the manifest bytes, checked.
    public func manifest(tag: String) throws -> (Manifest, Data) {
        guard let url = URL(string: "\(ManifestCheck.urlPrefix)\(tag)/manifest.json") else { throw UpdateError.noRelease }
        let raw = try http.get(url, headers: [:])
        let m = try Manifest.decode(raw)
        try ManifestCheck.check(m)
        guard "v" + (m.release?.version ?? "") == tag else { throw UpdateError.tagMismatch(tag, m.release?.version ?? "?") }
        return (m, raw)
    }

    /// Downloads (or reuses from `previousDir`) every artifact into `dir`, verifying each, then extracts and
    /// verifies the zips. A failure leaves `dir` incomplete; nothing outside it is touched.
    public func stage(_ m: Manifest, raw: Data, into dir: String, previousDir: String?) throws -> StagedRelease {
        let version = m.release?.version ?? "?"
        try fs.makeDirectory(dir)
        try fs.write(raw, to: (dir as NSString).appendingPathComponent("manifest.json"))
        for a in m.artifacts {
            guard let file = a.file else { continue }
            let dest = (dir as NSString).appendingPathComponent(file)
            let data = try fetch(a, previousDir: previousDir)
            try ManifestCheck.verifyFile(data, artifact: a, version: version)
            try fs.write(data, to: dest)
        }
        try verifyStaged(dir, manifest: m)
        return StagedRelease(dir: dir, manifest: m, manifestSha256: Hashing.sha256(raw))
    }

    /// Re-checks a release folder against its manifest (used again by the root side before applying).
    public func verifyStaged(_ dir: String, manifest m: Manifest) throws {
        let version = m.release?.version ?? "?"
        let x = (dir as NSString).appendingPathComponent("x")
        try fs.remove(x)
        for a in m.artifacts {
            guard let file = a.file else { continue }
            let path = (dir as NSString).appendingPathComponent(file)
            guard let data = try? fs.read(path) else { throw VerifyError.missingFile(file, dir) }
            try ManifestCheck.verifyFile(data, artifact: a, version: version)
            if a.isZip {
                try unzipper.extract(path, into: x)
                try ManifestCheck.verifyExtracted(root: x, artifact: a, fs: fs)
            }
        }
    }

    private func fetch(_ a: Artifact, previousDir: String?) throws -> Data {
        if let prev = previousDir, let file = a.file {
            let p = (prev as NSString).appendingPathComponent(file)
            if let data = try? fs.read(p), data.count == a.size, Hashing.sha256(data) == a.sha256 { return data }
        }
        guard let s = a.url, let url = URL(string: s) else { throw UpdateError.download(a.name, "no url") }
        do { return try http.get(url, headers: [:]) } catch { throw UpdateError.download(a.file ?? a.name, "\(error)") }
    }
}
