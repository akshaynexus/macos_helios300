// SPDX-License-Identifier: GPL-2.0-only
//
// EFIManager: OpenCore on the internal disk's EFI system partition (ESP).
//   find      `diskutil list -plist internal physical` -> the EFI partition(s); with several, the one on the
//             disk that holds the boot volume's APFS physical store.
//   mount     `diskutil mount <dev>` (root), then `diskutil info -plist` for the mount point.
//   install   1. free space is checked (backup + staged copy + 1 MiB), oldest backups pruned if that is what it takes;
//             2. an existing EFI/BOOT + EFI/OC is copied to EFI-backup-<date>/ on the same ESP and verified (it is
//                written under a partial name first, so a stopped backup never looks like a finished one);
//             3. the new BOOT + OC are copied to EFI.new/ and verified against the source, file by file;
//             4. the swap: EFI.old/.parts records what was there, BOOT and OC are renamed into EFI.old/, the staged
//                copies are renamed in and verified in place, then EFI.old is renamed to EFI.old.del (the commit
//                point) and the old copies are deleted last.
//             A throw anywhere in 4 puts the old copies back. EFI.old's presence on the next run says a swap was killed:
//             recoverSwap puts them back; EFI.old.del says it had committed and only the deletion is left.
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
    case swapFailed(String)
    case rollbackFailed(String, String)
    case noBackup(String)
    case badBackupName(String)
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
        case .verifyFailed(let what, let path): return "\(what) does not match its source (\(path))"
        case .swapFailed(let why): return "the EFI swap stopped (\(why)); the previous EFI/BOOT and EFI/OC are back in place"
        case .rollbackFailed(let why, let undo):
            return "the EFI swap stopped (\(why)) and the previous EFI could not be put back (\(undo)): "
                + "start from the stick and use Undo, or copy an EFI-backup-<date> folder back by hand"
        case .noBackup(let b): return "no EFI backup \(b)"
        case .badBackupName(let b): return "\(b) is not an EFI backup name"
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
    static let stagedDir = "EFI.new"
    static let asideDir = "EFI.old"
    static let doomedDir = "EFI.old.del"
    static let asideMarker = ".parts"
    static let backupPrefix = "EFI-backup-"
    static let partialPrefix = "EFI-partial-"
    /// EFI backups kept on the ESP after a successful install or restore.
    public static let backupsKept = 3
    static let ignored: Set<String> = [".DS_Store", ".fseventsd", ".Spotlight-V100", ".Trashes", ".TemporaryItems"]

    /// Where an artifact lives on the ESP, relative to its mount point.
    public static func kextPath(_ name: String) -> String { "EFI/OC/Kexts/\(name)" }
    public static func acpiPath(_ name: String) -> String { "EFI/OC/ACPI/\(name)" }
    public static let configPath = "EFI/OC/config.plist"

    public static func espPath(of artifact: Artifact) -> String {
        artifact.kind == "ssdt" ? acpiPath(artifact.name) : kextPath(artifact.name)
    }

    public init(runner: CommandRunner, fs: FileSystem, clock: Clock) {
        self.runner = runner
        self.fs = fs
        self.clock = clock
    }

    // MARK: find and mount

    /// The EFI partition on the disk that holds the running macOS (its APFS physical store), even when the internal
    /// disk list shows only one: that one could belong to another disk than the macOS being set up.
    public func internalESP() throws -> String {
        let list = runner.capture([Tool.diskutil, "list", "-plist", "internal", "physical"])
        let esps = DiskutilParser.efiPartitions(Data(list.stdout.utf8))
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
        try recoverSwap(esp: mp, log: log)
        let efi = (mp as NSString).appendingPathComponent("EFI")
        let hadOC = isEFI(efi) || Self.parts.contains { fs.exists((efi as NSString).appendingPathComponent($0)) }
        try makeRoom(source: source, existing: hadOC ? efi : nil, esp: mp, log: log)
        let backup = hadOC ? backupName() : nil
        if let backup { try makeBackup(from: efi, named: backup, esp: mp, log: log) }
        try swapIn(source: source, esp: mp, log: log)
        pruneBackups(esp: mp, log: log)
        return backup
    }

    /// Puts a backup back (after backing up what is there now).
    public func restore(backup name: String, esp mp: String, log: (String) -> Void) throws -> String? {
        guard ManifestCheck.isPathComponent(name), name.hasPrefix(Self.backupPrefix) else { throw EFIError.badBackupName(name) }
        try recoverSwap(esp: mp, log: log)
        let dir = (mp as NSString).appendingPathComponent(name)
        guard fs.isDirectory((dir as NSString).appendingPathComponent("OC")) else { throw EFIError.noBackup(name) }
        let efi = (mp as NSString).appendingPathComponent("EFI")
        var current: String?
        if isEFI(efi) {
            try makeRoom(source: dir, existing: efi, esp: mp, keeping: name, log: log)
            let n = backupName() + "-before-restore"
            try makeBackup(from: efi, named: n, esp: mp, log: log)
            current = n
        }
        try swapIn(source: dir, esp: mp, log: log)
        pruneBackups(esp: mp, keeping: name, log: log)
        return current
    }

    public func backups(esp mp: String) -> [String] {
        ((try? fs.list(mp)) ?? []).filter { $0.hasPrefix(Self.backupPrefix) }.sorted()
    }

    /// Writes a backup under a partial name and renames it when complete; a failed copy leaves nothing behind.
    func makeBackup(from efi: String, named name: String, esp mp: String, log: (String) -> Void) throws {
        let partial = (mp as NSString).appendingPathComponent(Self.partialPrefix + name)
        try fs.remove(partial)
        do {
            try copyParts(from: efi, to: partial, what: "backup", log: log)
            try fs.move(partial, to: (mp as NSString).appendingPathComponent(name))
        } catch {
            try? fs.remove(partial)
            throw error
        }
        log("backup kept as \(name)")
    }

    /// Frees the ESP by deleting the oldest backups, keeping at least the newest, when that is what it takes.
    func makeRoom(source: String, existing: String?, esp mp: String, keeping: String? = nil, log: (String) -> Void) throws {
        while true {
            do { return try checkSpace(source: source, existing: existing, esp: mp) } catch EFIError.noSpace(let need, let free) {
                let spare = backups(esp: mp).dropLast().filter { $0 != keeping }
                guard let oldest = spare.first else { throw EFIError.noSpace(need: need, free: free) }
                try fs.remove((mp as NSString).appendingPathComponent(oldest))
                log("removed the oldest backup \(oldest) to make room")
            }
        }
    }

    /// Keeps the newest `backupsKept` (and the one being used); a failure here costs only disk space.
    func pruneBackups(esp mp: String, keeping: String? = nil, log: (String) -> Void) {
        for old in backups(esp: mp).dropLast(Self.backupsKept) where old != keeping {
            try? fs.remove((mp as NSString).appendingPathComponent(old))
            log("removed the old backup \(old)")
        }
    }

    func swapIn(source: String, esp mp: String, log: (String) -> Void) throws {
        let efi = (mp as NSString).appendingPathComponent("EFI")
        let staged = (mp as NSString).appendingPathComponent(Self.stagedDir)
        let aside = (mp as NSString).appendingPathComponent(Self.asideDir)
        do {
            try copyParts(from: source, to: staged, what: "new EFI (staged)", log: log)
            try fs.makeDirectory(efi)
            try fs.makeDirectory(aside)
        } catch {
            try? fs.remove(staged)
            try? fs.remove(aside)
            throw error
        }
        let incoming = Self.parts.filter { fs.exists((staged as NSString).appendingPathComponent($0)) }
        let had = Self.parts.filter { fs.exists((efi as NSString).appendingPathComponent($0)) }
        try fs.write(Data(had.joined(separator: "\n").utf8), to: (aside as NSString).appendingPathComponent(Self.asideMarker))
        do {
            for p in incoming where had.contains(p) {
                try fs.move((efi as NSString).appendingPathComponent(p), to: (aside as NSString).appendingPathComponent(p))
            }
            for p in incoming {
                try fs.move((staged as NSString).appendingPathComponent(p), to: (efi as NSString).appendingPathComponent(p))
            }
            for p in incoming {
                try verify(
                    (source as NSString).appendingPathComponent(p), (efi as NSString).appendingPathComponent(p), what: "EFI/\(p)")
            }
        } catch {
            do { try undoSwap(esp: mp) } catch let undo { throw EFIError.rollbackFailed("\(error)", "\(undo)") }
            throw EFIError.swapFailed("\(error)")
        }
        // The commit point: from here on the new EFI stays, whatever happens to the deletions.
        try fs.move(aside, to: (mp as NSString).appendingPathComponent(Self.doomedDir))
        finishSwap(esp: mp, log: log)
        log("EFI/BOOT and EFI/OC are in place and verified")
    }

    /// Deletes what a committed swap left behind. A failure only costs space until the next run tries again.
    func finishSwap(esp mp: String, log: (String) -> Void) {
        for dir in [Self.doomedDir, Self.stagedDir] {
            let path = (mp as NSString).appendingPathComponent(dir)
            do { try fs.remove(path) } catch { log("WARNING: could not delete \(path): \(error)") }
        }
    }

    /// Puts the parts in EFI.old back, removes parts that only the swap brought in, and deletes EFI.old and EFI.new.
    func undoSwap(esp mp: String) throws {
        let efi = (mp as NSString).appendingPathComponent("EFI")
        let aside = (mp as NSString).appendingPathComponent(Self.asideDir)
        let marker = (aside as NSString).appendingPathComponent(Self.asideMarker)
        // No marker: the swap had not started moving anything.
        if let data = try? fs.read(marker) {
            let had = Set(String(decoding: data, as: UTF8.self).split(separator: "\n").map(String.init))
            for p in Self.parts {
                let kept = (aside as NSString).appendingPathComponent(p)
                let target = (efi as NSString).appendingPathComponent(p)
                if fs.exists(kept) {
                    try fs.remove(target)
                    do { try fs.move(kept, to: target) } catch { try copyTree(kept, target) }  // a rename that fails twice
                } else if !had.contains(p) {
                    try fs.remove(target)
                }
            }
        }
        try fs.remove(aside)
        try fs.remove((mp as NSString).appendingPathComponent(Self.stagedDir))
    }

    /// Repairs what a killed process left on the ESP: an uncommitted swap goes back, a committed one is tidied up,
    /// and partial backups are removed. Run before anything else touches the ESP.
    public func recoverSwap(esp mp: String, log: (String) -> Void) throws {
        if fs.exists((mp as NSString).appendingPathComponent(Self.doomedDir)) {
            log("an earlier EFI swap had finished: removing its old copies")
            finishSwap(esp: mp, log: log)
        }
        if fs.exists((mp as NSString).appendingPathComponent(Self.asideDir)) {
            log("an earlier EFI swap was interrupted: putting the previous EFI back")
            do { try undoSwap(esp: mp) } catch { throw EFIError.rollbackFailed("an earlier run was interrupted", "\(error)") }
        }
        try fs.remove((mp as NSString).appendingPathComponent(Self.stagedDir))
        for partial in ((try? fs.list(mp)) ?? []) where partial.hasPrefix(Self.partialPrefix) {
            try fs.remove((mp as NSString).appendingPathComponent(partial))
            log("removed the partial backup \(partial)")
        }
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
        guard fs.isDirectory(a) else {
            guard Hashing.sha256(file: a, fs: fs) == Hashing.sha256(file: b, fs: fs) else { throw EFIError.verifyFailed(what, b) }
            return
        }
        let x = try filtered(Hashing.tree(a, fs: fs).files)
        let y = try filtered(Hashing.tree(b, fs: fs).files)
        if x != y { throw EFIError.verifyFailed(what, b) }
    }

    func filtered(_ files: [String: String]) -> [String: String] {
        files.filter { path, _ in !path.split(separator: "/").contains { Self.isHousekeeping($0) } }
    }

    static func isHousekeeping(_ name: Substring) -> Bool { ignored.contains(String(name)) || name.hasPrefix("._") }

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

    /// Runs ocvalidate on `config` (default `<efi>/OC/config.plist`) when a matching one is found in `candidates`. With
    /// `scratch` (the root side) it runs a root-owned copy, never the file in the place it was found.
    public func validate(efi: String, config: String? = nil, candidates: [String], scratch: RootScratch? = nil)
        -> ValidationResult
    {
        let config = config ?? (efi as NSString).appendingPathComponent("OC/config.plist")
        let ocFile = (efi as NSString).appendingPathComponent("OC/OpenCore.efi")
        guard let oc = (try? fs.read(ocFile)).flatMap(OpenCoreVersion.fromBinary) else {
            return .skipped("the OpenCore version of \(ocFile) is not readable: ocvalidate not run")
        }
        guard let found = candidates.first(where: fs.exists) else {
            return .skipped("no ocvalidate for OpenCore \(oc) in EFI/OC/Tools or the release folder: config not validated")
        }
        let tool: String
        do { tool = try scratch?.copy(found) ?? found } catch {
            return .skipped("ocvalidate could not be copied to a root-owned place (\(error)): config not validated")
        }
        let r = runner.capture([tool, config])
        let out = r.stdout + r.stderr
        if let v = OpenCoreVersion.fromOcvalidate(out), v != oc {
            return .skipped("\(found) is for OpenCore \(v), the EFI has \(oc): config not validated")
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
