// SPDX-License-Identifier: GPL-2.0-only
//
// StagedSwap: replacing one file or bundle without ever leaving the target missing or half written.
//   1. the new copy is written to the sibling `<target>.new` and verified against its source;
//   2. the caller persists how to undo it (the RollbackOp this returns), BEFORE the first change;
//   3. the old copy is renamed aside to `<target>.prev-N`, the first N not in use, so a backup that belongs to another
//      unit or to an earlier update is never overwritten;
//   4. `.new` is renamed to the target and verified in place.
// Only 3 and 4 touch the target and both are renames on one volume. Anything that stops the work is repaired from the
// persisted op by RollbackExecutor, which is idempotent, so the same undo also finishes after the process was killed.
import Foundation

public struct StagedSwap {
    let fs: FileSystem
    /// Copies a file or directory tree (ditto on the ESP, so FAT gets no AppleDouble files).
    let copy: (String, String) throws -> Void
    /// Throws unless the two paths hold the same file or tree.
    let verify: (_ source: String, _ copy: String, _ what: String) throws -> Void

    public init(
        fs: FileSystem, copy: @escaping (String, String) throws -> Void,
        verify: @escaping (_ source: String, _ copy: String, _ what: String) throws -> Void
    ) {
        self.fs = fs
        self.copy = copy
        self.verify = verify
    }

    public static let stagedSuffix = ".new"
    static let backupInfix = ".prev-"

    public static func staged(_ target: String) -> String { target + stagedSuffix }

    /// `<target>.prev-N` with the lowest N that is free.
    public func nextBackup(_ target: String) -> String {
        var n = 1
        while fs.exists("\(target)\(Self.backupInfix)\(n)") { n += 1 }
        return "\(target)\(Self.backupInfix)\(n)"
    }

    /// True when an earlier kept copy of `target` exists.
    public func hasBackup(_ target: String) -> Bool { fs.exists("\(target)\(Self.backupInfix)1") }

    /// Writes `<target>.new` from `source` and verifies it. The target is not touched.
    public func stage(_ source: String, target: String, what: String) throws {
        let staged = Self.staged(target)
        try fs.remove(staged)
        try copy(source, staged)
        try verify(source, staged, "\(what) (staged)")
    }

    /// The old copy aside (when there is one), the staged copy in, verified against `source`.
    public func commit(_ source: String, target: String, backup: String?, what: String) throws {
        if let backup { try fs.move(target, to: backup) }
        try fs.move(Self.staged(target), to: target)
        try verify(source, target, what)
    }
}

extension Hashing {
    /// One SHA-256 for a file or a whole tree (paths and contents), to recognise a kept copy later.
    public static func digest(_ path: String, fs: FileSystem) throws -> String {
        guard fs.isDirectory(path) else { return sha256(try fs.read(path)) }
        let tree = try Self.tree(path, fs: fs)
        // Not the FAT housekeeping files macOS may add between keeping a copy and looking at it.
        let kept = tree.files.filter { !$0.key.split(separator: "/").contains { EFIManager.isHousekeeping($0) } }
        let text = kept.sorted { $0.key < $1.key }.map { "\($0.key)\t\($0.value)\n" }.joined()
        return sha256(Data((text + tree.symlinks.joined(separator: "\n")).utf8))
    }
}
