// SPDX-License-Identifier: GPL-2.0-only
//
// The trust chain, verified the same way on both sides. This is a port of 1401's p1401/helios300.py
// (check_manifest, Release.blob, Release.covers; akshaynexus/1401 branch linux-port) and of install-gpu.sh's
// `verify` for the zips (every inner file listed, none extra, no symlink). docs/MANIFEST.md "Trust chain":
//   1. the manifest comes from the release (its sha256 is recorded in the state once installed);
//   2. every release file must match the manifest's sha256 and size;
//   3. a zipped bundle must extract to exactly the files in contents_sha256, each with its sha256.
// Keep the rules identical to 1401's; a change there must be made here too (ManifestCheckTests mirror its tests).
import Foundation

public enum ManifestCheck {
    public static let repo = "akshaynexus/macos_helios300"
    public static let urlPrefix = "https://github.com/\(repo)/releases/download/"
    static let installKinds: Set<String> = ["opencore", "library-extensions", "gpu-bundles", "applications", "stick-only"]

    /// helios300.check_manifest: structural checks. `local`: a manifest on disk the user chose (dry runs allowed).
    public static func check(_ m: Manifest, local: Bool = false) throws {
        func need(_ cond: Bool, _ what: String) throws {
            if !cond { throw ManifestError.notUsable(what) }
        }
        try need(m.schema == 1, "schema is not 1")
        try need(m.repo == repo, "repo is '\(m.repo)', not \(repo)")
        guard let rel = m.release else { throw ManifestError.notUsable("no release version") }
        try need(isVersion(rel.version), "no release version")
        try need(local || rel.dryRun != true, "it is a dry-run manifest (release.sh --dry-run); only a local test may use one")
        try need(!m.artifacts.isEmpty, "no artifacts")
        for a in m.artifacts {
            try need(isSha256(a.sha256 ?? ""), "artifact without sha256: \(a.name)")
            try need(isFileName(a.file ?? ""), "bad file name '\(a.file ?? "")'")
            try need(
                (a.url ?? "").hasPrefix(urlPrefix + "v" + rel.version + "/"),
                "\(a.file ?? a.name) is not served from \(urlPrefix)v\(rel.version)/")
            try need((a.size ?? 0) > 0, "\(a.file ?? a.name) has no size")
            try need(installKinds.contains(a.install), "\(a.name): unknown install '\(a.install)'")
            if a.install == "opencore" && a.kind == "kext" {
                let oc = a.opencore
                try need(
                    oc?.bundlePath == a.name && !(oc?.executablePath ?? "").isEmpty
                        && oc?.plistPath == "Contents/Info.plist", "\(a.name): incomplete Kernel > Add entry")
            }
            if a.kind == "ssdt" {
                try need(a.acpi?.path == a.name, "\(a.name): incomplete ACPI > Add entry")
            }
        }
        let cfg = m.config
        try need(
            cfg?.nvramAdd != nil && cfg?.nvramDelete != nil && cfg?.deviceProperties != nil && cfg?.forbidden != nil,
            "config.nvram_add, nvram_delete, device_properties or forbidden missing")
    }

    /// helios300.Release.blob: the bytes of a release file must match its size and sha256.
    public static func verifyFile(_ data: Data, artifact a: Artifact, version: String) throws {
        guard data.count == a.size, Hashing.sha256(data) == a.sha256 else {
            throw VerifyError.fileMismatch(a.file ?? a.name, version)
        }
    }

    /// install-gpu.sh verify: an extracted zip holds `<name>/` with exactly the contents_sha256 files.
    public static func verifyExtracted(root: String, artifact a: Artifact, fs: FileSystem) throws {
        let top = (root as NSString).appendingPathComponent(a.name)
        guard fs.isDirectory(top) else { throw VerifyError.notInZip(a.file ?? a.name, a.name) }
        guard let want = a.contentsSha256, !want.isEmpty else { throw VerifyError.noContents(a.name) }
        let tree = try Hashing.tree(top, fs: fs)
        if let link = tree.symlinks.first { throw VerifyError.symlink(a.name, link) }
        for (path, sha) in want.sorted(by: { $0.key < $1.key }) {
            guard let got = tree.files[path] else { throw VerifyError.missingInner(a.name, path) }
            guard got == sha else { throw VerifyError.innerMismatch(a.name, path) }
        }
        let extra = Set(tree.files.keys).subtracting(want.keys).sorted()
        if let first = extra.first { throw VerifyError.extraInner(a.name, first) }
    }

    /// helios300.Release.covers: the running kernel version is within macos.min_kernel..max_kernel.
    public static func covers(_ m: Manifest, darwin: String) -> Bool {
        let v = darwinTuple(darwin)
        let lo = m.macos.minKernel, hi = m.macos.maxKernel
        return (lo.isEmpty || !v.lexicographicallyPrecedes(darwinTuple(lo)))
            && (hi.isEmpty || !darwinTuple(hi).lexicographicallyPrecedes(v))
    }

    /// helios300._darwin: "25.6.0" -> [25, 6, 0]; anything unparsable is [0, 0, 0].
    public static func darwinTuple(_ v: String) -> [Int] {
        let parts = (v.split(separator: ".").map(String.init) + ["0", "0"]).prefix(3)
        let nums = parts.compactMap { Int($0) }
        return nums.count == 3 ? nums : [0, 0, 0]
    }

    static func isVersion(_ s: String) -> Bool {
        s.range(of: #"^\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?$"#, options: .regularExpression) != nil
    }

    static func isSha256(_ s: String) -> Bool {
        s.range(of: "^[0-9a-f]{64}$", options: .regularExpression) != nil
    }

    static func isFileName(_ s: String) -> Bool {
        s.range(of: "^[A-Za-z0-9_.+-]+$", options: .regularExpression) != nil
    }
}

public enum VerifyError: Error, CustomStringConvertible, Equatable {
    case fileMismatch(String, String)
    case notInZip(String, String)
    case noContents(String)
    case symlink(String, String)
    case missingInner(String, String)
    case innerMismatch(String, String)
    case extraInner(String, String)
    case missingFile(String, String)

    public var description: String {
        switch self {
        case .fileMismatch(let f, let v):
            return "\(f) does not match the SHA-256 in the macos_helios300 \(v) manifest"
        case .notInZip(let f, let n): return "\(f) does not hold \(n)/ at its top"
        case .noContents(let n): return "\(n): the manifest lists no files inside it"
        case .symlink(let n, let p): return "\(n): holds a symbolic link (\(p))"
        case .missingInner(let n, let p): return "\(n): \(p) is in manifest.json but not in the zip"
        case .innerMismatch(let n, let p): return "\(n): \(p) does not match its manifest.json sha256"
        case .extraInner(let n, let p): return "\(n): \(p) is in the zip but not in manifest.json"
        case .missingFile(let f, let dir): return "\(f) is listed in manifest.json but not in \(dir)"
        }
    }
}
