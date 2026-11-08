// SPDX-License-Identifier: GPL-2.0-only
//
// EFIManager: OpenCore on the internal disk's EFI system partition (ESP).
//   find      `diskutil list -plist internal physical` -> the EFI partition(s); with several, the one on the
//             disk that holds the boot volume's APFS physical store.
//   mount     `diskutil mount <dev>` (root), then `diskutil info -plist` for the mount point.
//   install   1. free space is checked (backup + staged copy + 1 MiB);
//             2. an existing EFI/BOOT + EFI/OC is copied to EFI-backup-<date>/ on the same ESP and verified;
//             3. the new BOOT + OC are copied to EFI.new/ and verified against the source, file by file;
//             4. only then swapped in, and verified again in place.
//             A stop before 4 leaves the old bootloader untouched.
//   restore   the same swap from a backup (the current EFI is backed up first).
//   validate  ocvalidate from <EFI>/OC/Tools (or next to the release), only when it is for the same OpenCore
//             version as OpenCore.efi; otherwise skipped with a warning (OpenCore's own rule: ocvalidate checks one
//             OpenCore version's schema).
// Backups are named EFI-backup-<yyyyMMdd-HHmmss> and hold BOOT/ and OC/ directly (not EFI/BOOT), so no firmware or
// OpenCore scan mistakes a backup for a bootable \EFI\BOOT\BOOTx64.efi.
// Copies use `ditto --norsrc --noextattr` on the Mac, so FAT32 gets no AppleDouble ._ files.
import Foundation

public enum EFIError: Error, CustomStringConvertible, Equatable {
    case noInternalESP
    case severalESPs([String])
    case notMounted(String)
    case notAnEFI(String)
    case noSpace(need: Int64, free: Int64)
    case verifyFailed(String, String)
    case noBackup(String)
    case command(String, String)

    public var description: String {
        switch self {
        case .noInternalESP: return "no EFI partition on the internal disk"
        case .severalESPs(let l): return "several internal EFI partitions (\(l.joined(separator: ", "))): choose one with --esp"
        case .notMounted(let d): return "\(d) could not be mounted"
        case .notAnEFI(let p):
            return "\(p) is not an OpenCore EFI folder (needs BOOT/BOOTx64.efi, OC/OpenCore.efi, OC/config.plist)"
        case .noSpace(let need, let free):
            return "the EFI partition has \(free / 1_048_576) MiB free, \(need / 1_048_576) MiB are needed (backup + new copy)"
        case .verifyFailed(let what, let path): return "\(what) does not match its source (\(path)); nothing was swapped"
        case .noBackup(let b): return "no EFI backup \(b)"
        case .command(let c, let why): return "\(c) failed: \(why)"
        }
    }
}

public enum ValidationResult: Equatable {
    case passed(String)
    case failed(String)
    case skipped(String)
}

public struct EFIManager {
    let runner: CommandRunner
    let fs: FileSystem
    let clock: Clock
    static let parts = ["BOOT", "OC"]
    static let ignored: Set<String> = [".DS_Store", ".fseventsd", ".Spotlight-V100", ".Trashes", ".TemporaryItems"]

    public init(runner: CommandRunner, fs: FileSystem, clock: Clock) {
        self.runner = runner
        self.fs = fs
        self.clock = clock
    }

    // MARK: find and mount

    public func internalESP() throws -> String {
        let list = runner.capture([Tool.diskutil, "list", "-plist", "internal", "physical"])
        let esps = DiskutilParser.efiPartitions(Data(list.stdout.utf8))
        if esps.count == 1, let only = esps.first { return only }
        if esps.isEmpty { throw EFIError.noInternalESP }
        let root = runner.capture([Tool.diskutil, "info", "-plist", "/"])
        if let store = physicalStore(Data(root.stdout.utf8)), let disk = wholeDisk(store),
            let match = esps.first(where: { wholeDisk($0) == disk })
        {
            return match
        }
        throw EFIError.severalESPs(esps)
    }

    /// The partition UUID and mount point of a partition (no root needed).
    public func info(_ dev: String) -> (mountPoint: String?, partitionUUID: String?) {
        let r = runner.capture([Tool.diskutil, "info", "-plist", dev])
        let i = DiskutilParser.info(Data(r.stdout.utf8))
        return (i.mountPoint, i.partitionUUID)
    }

    /// Mounts the ESP if needed (root) and returns its mount point.
    public func mount(_ dev: String) throws -> String {
        if let mp = info(dev).mountPoint { return mp }
        let r = runner.capture([Tool.diskutil, "mount", dev])
        guard r.ok, let mp = info(dev).mountPoint else { throw EFIError.notMounted(dev) }
        return mp
    }

    func physicalStore(_ plist: Data) -> String? {
        guard let d = try? PropertyListSerialization.propertyList(from: plist, format: nil) as? [String: Any],
            let stores = d["APFSPhysicalStores"] as? [[String: Any]]
        else { return nil }
        return stores.first?["APFSPhysicalStore"] as? String
    }

    /// disk0s1 -> disk0.
    func wholeDisk(_ dev: String) -> String? {
        guard let r = dev.range(of: #"^disk[0-9]+"#, options: .regularExpression) else { return nil }
        return String(dev[r])
    }

    // MARK: source

    /// `path` itself or its EFI/ child, when it holds an OpenCore EFI.
    public func sourceEFI(_ path: String) throws -> String {
        for candidate in [path, (path as NSString).appendingPathComponent("EFI")] where isEFI(candidate) {
            return candidate
        }
        throw EFIError.notAnEFI(path)
    }

    func isEFI(_ dir: String) -> Bool {
        ["BOOT/BOOTx64.efi", "OC/OpenCore.efi", "OC/config.plist"].allSatisfy {
            fs.exists((dir as NSString).appendingPathComponent($0))
        }
    }

    // MARK: install / restore

    public func backupName() -> String {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.timeZone = TimeZone(identifier: "UTC")
        f.dateFormat = "yyyyMMdd-HHmmss"
        return "EFI-backup-\(f.string(from: clock.now))"
    }

    /// Copies `source` (an EFI folder) onto the ESP mounted at `mp`. Returns the backup's name, or nil when the ESP
    /// had no OpenCore before.
    @discardableResult
    public func install(from source: String, esp mp: String, log: (String) -> Void) throws -> String? {
        let efi = (mp as NSString).appendingPathComponent("EFI")
        let hadOC = isEFI(efi) || Self.parts.contains { fs.exists((efi as NSString).appendingPathComponent($0)) }
        try checkSpace(source: source, existing: hadOC ? efi : nil, esp: mp)
        var backup: String?
        if hadOC {
            let name = backupName()
            try copyParts(from: efi, to: (mp as NSString).appendingPathComponent(name), what: "backup", log: log)
            backup = name
        }
        try swapIn(source: source, esp: mp, log: log)
        return backup
    }

    /// Puts a backup back (after backing up what is there now).
    public func restore(backup name: String, esp mp: String, log: (String) -> Void) throws -> String? {
        let dir = (mp as NSString).appendingPathComponent(name)
        guard fs.isDirectory((dir as NSString).appendingPathComponent("OC")) else { throw EFIError.noBackup(name) }
        let efi = (mp as NSString).appendingPathComponent("EFI")
        var current: String?
        if isEFI(efi) {
            try checkSpace(source: dir, existing: efi, esp: mp)
            let n = backupName() + "-before-restore"
            try copyParts(from: efi, to: (mp as NSString).appendingPathComponent(n), what: "backup", log: log)
            current = n
        }
        try swapIn(source: dir, esp: mp, log: log)
        return current
    }

    public func backups(esp mp: String) -> [String] {
        ((try? fs.list(mp)) ?? []).filter { $0.hasPrefix("EFI-backup-") }.sorted()
    }

    func swapIn(source: String, esp mp: String, log: (String) -> Void) throws {
        let efi = (mp as NSString).appendingPathComponent("EFI")
        let staged = (mp as NSString).appendingPathComponent("EFI.new")
        try fs.remove(staged)
        try copyParts(from: source, to: staged, what: "new EFI (staged)", log: log)
        try fs.makeDirectory(efi)
        for p in Self.parts {
            let target = (efi as NSString).appendingPathComponent(p)
            try fs.remove(target)
            try fs.move((staged as NSString).appendingPathComponent(p), to: target)
        }
        try fs.remove(staged)
        for p in Self.parts {
            try verify(
                (source as NSString).appendingPathComponent(p), (efi as NSString).appendingPathComponent(p), what: "EFI/\(p)")
        }
        log("EFI/BOOT and EFI/OC are in place and verified")
    }

    func copyParts(from src: String, to dst: String, what: String, log: (String) -> Void) throws {
        try fs.makeDirectory(dst)
        for p in Self.parts {
            let s = (src as NSString).appendingPathComponent(p)
            guard fs.exists(s) else { continue }
            let d = (dst as NSString).appendingPathComponent(p)
            try copyTree(s, d)
            try verify(s, d, what: "\(what) \(p)")
        }
        log("\(what): copied to \(dst) and verified")
    }

    func copyTree(_ s: String, _ d: String) throws {
        try fs.remove(d)
        if fs.exists(Tool.ditto) {
            let r = runner.capture([Tool.ditto, "--norsrc", "--noextattr", "--noacl", s, d])
            if !r.ok { throw EFIError.command("ditto \(s)", r.stderr) }
        } else {
            try fs.copy(s, to: d)
        }
    }

    /// Same files with the same SHA-256 (FAT housekeeping files ignored).
    public func verify(_ a: String, _ b: String, what: String) throws {
        let x = try filtered(Hashing.tree(a, fs: fs).files)
        let y = try filtered(Hashing.tree(b, fs: fs).files)
        if x != y { throw EFIError.verifyFailed(what, b) }
    }

    func filtered(_ files: [String: String]) -> [String: String] {
        files.filter { path, _ in
            !path.split(separator: "/").contains { Self.ignored.contains(String($0)) || $0.hasPrefix("._") }
        }
    }

    func treeSize(_ dir: String) -> Int64 {
        guard let files = try? Hashing.tree(dir, fs: fs).files else { return 0 }
        return files.keys.reduce(0) { $0 + (fs.size((dir as NSString).appendingPathComponent($1)) ?? 0) }
    }

    func checkSpace(source: String, existing: String?, esp mp: String) throws {
        let need = Self.parts.reduce(Int64(1 << 20)) { acc, p in
            acc + treeSize((source as NSString).appendingPathComponent(p))
                + (existing.map { treeSize(($0 as NSString).appendingPathComponent(p)) } ?? 0)
        }
        if let free = fs.freeBytes(mp), free < need { throw EFIError.noSpace(need: need, free: free) }
    }

    // MARK: ocvalidate

    /// Runs ocvalidate on `<efi>/OC/config.plist` when a matching one is found in `candidates` (paths to try).
    public func validate(efi: String, candidates: [String]) -> ValidationResult {
        let config = (efi as NSString).appendingPathComponent("OC/config.plist")
        let ocFile = (efi as NSString).appendingPathComponent("OC/OpenCore.efi")
        guard let oc = (try? fs.read(ocFile)).flatMap(OpenCoreVersion.fromBinary) else {
            return .skipped("the OpenCore version of \(ocFile) is not readable: ocvalidate not run")
        }
        guard let tool = candidates.first(where: fs.exists) else {
            return .skipped("no ocvalidate next to OpenCore \(oc) (EFI/OC/Tools or the release folder): config not validated")
        }
        let r = runner.capture([tool, config])
        let out = r.stdout + r.stderr
        if let v = OpenCoreVersion.fromOcvalidate(out), v != oc {
            return .skipped("\(tool) is for OpenCore \(v), the EFI has \(oc): config not validated")
        }
        return r.ok ? .passed(out) : .failed(out)
    }

    /// Where to look for ocvalidate for an EFI folder.
    public static func ocvalidateCandidates(efi: String, releaseDir: String?) -> [String] {
        var c = [(efi as NSString).appendingPathComponent("OC/Tools/ocvalidate")]
        if let r = releaseDir { c.append((r as NSString).appendingPathComponent("ocvalidate")) }
        return c
    }
}
