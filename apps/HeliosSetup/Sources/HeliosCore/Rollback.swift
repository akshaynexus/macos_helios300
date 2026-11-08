// SPDX-License-Identifier: GPL-2.0-only
//
// Rollback: every update unit leaves a record of how to undo it, written before the unit's first change.
//   restore  put a kept copy back (a kext's or config.plist's <name>.prev-N, the previous app bundle)
//   remove   take away something the update added (a new kext)
//   script   re-run a release script from the PREVIOUS release folder (install-gpu.sh for PG106FB/PG106Accel/
//            the Metal userspace: it reinstalls the previous files and rebuilds the auxiliary collection)
// Paths on the internal ESP are stored relative to its mount point (`onESP`), which can differ per boot.
// Ops are undone newest first. "Roll back last update" pops one record off SystemState.rollbacks.
// Undo is idempotent: every op is written BEFORE its change (StagedSwap), so it may describe a change that never
// happened or was already undone. A missing kept copy is fine when the target already is that copy (`digest`).
import Foundation

public struct RollbackOp: Codable, Equatable {
    public enum Kind: String, Codable { case restore, remove, script }

    public var kind: Kind
    public var onESP: Bool
    public var target: String
    public var backup: String?
    public var releaseDir: String?
    public var env: [String: String]?
    /// The staged `<target>.new`, removed by the undo.
    public var staged: String?
    /// Hashing.digest of the copy `backup` keeps.
    public var digest: String?

    public static func restore(
        _ target: String, from backup: String, onESP: Bool, digest: String? = nil, staged: String? = nil
    ) -> RollbackOp {
        RollbackOp(kind: .restore, onESP: onESP, target: target, backup: backup, staged: staged, digest: digest)
    }

    public static func remove(_ target: String, onESP: Bool, staged: String? = nil) -> RollbackOp {
        RollbackOp(kind: .remove, onESP: onESP, target: target, staged: staged)
    }

    /// target: the script file name in `releaseDir`.
    public static func script(_ name: String, releaseDir: String, env: [String: String]) -> RollbackOp {
        RollbackOp(kind: .script, onESP: false, target: name, releaseDir: releaseDir, env: env)
    }
}

public struct RollbackRecord: Codable, Equatable {
    public var unit: String
    public var fromTag: String?
    public var toTag: String
    public var date: Date
    public var ops: [RollbackOp]

    public init(unit: String, fromTag: String?, toTag: String, date: Date, ops: [RollbackOp] = []) {
        self.unit = unit
        self.fromTag = fromTag
        self.toTag = toTag
        self.date = date
        self.ops = ops
    }

    public var needsESP: Bool { ops.contains(where: \.onESP) }
}

public enum RollbackError: Error, CustomStringConvertible, Equatable {
    case nothingToRollBack
    case backupMissing(String)
    case backupChanged(String)
    case scriptFailed(String, Int32)
    case scriptNotVerified(String)

    public var description: String {
        switch self {
        case .nothingToRollBack: return "no update to roll back"
        case .backupMissing(let p): return "the kept copy \(p) is gone: cannot put it back"
        case .backupChanged(let p): return "the kept copy \(p) is not what was kept (it changed): not putting it back"
        case .scriptNotVerified(let p): return "\(p) cannot be checked against that release's manifest: not running it"
        case .scriptFailed(let s, let rc): return "\(s) (previous release) failed with exit \(rc)"
        }
    }
}

public struct RollbackExecutor {
    let fs: FileSystem
    let runner: CommandRunner
    let user: UserContext
    let scratch: RootScratch?

    public init(fs: FileSystem, runner: CommandRunner, user: UserContext, scratch: RootScratch? = nil) {
        self.fs = fs
        self.runner = runner
        self.user = user
        self.scratch = scratch
    }

    /// Undoes a record, newest op first. `esp` is the ESP mount point when the record needs it.
    public func undo(_ record: RollbackRecord, esp: String?, output: String?, log: (String) -> Void) throws {
        for op in record.ops.reversed() {
            let target = resolve(op.target, op.onESP, esp)
            if let staged = op.staged { try fs.remove(resolve(staged, op.onESP, esp)) }
            switch op.kind {
            case .restore:
                guard let b = op.backup else { continue }
                try restore(target, from: resolve(b, op.onESP, esp), digest: op.digest, log: log)
            case .remove:
                try fs.remove(target)
                log("removed \(target)")
            case .script:
                try runScript(op, output: output, log: log)
            }
        }
    }

    /// Deletes what a record keeps (rollback pruning); the record is not undone.
    public func discard(_ record: RollbackRecord, esp: String?) {
        for op in record.ops {
            if let b = op.backup { try? fs.remove(resolve(b, op.onESP, esp)) }
        }
    }

    func restore(_ target: String, from backup: String, digest: String?, log: (String) -> Void) throws {
        guard fs.exists(backup) else {
            // The op is written before its change, so the rename aside may never have happened; or this ran before.
            if let digest, fs.exists(target), (try? Hashing.digest(target, fs: fs)) == digest {
                log("\(target) already is the kept copy")
                return
            }
            throw RollbackError.backupMissing(backup)
        }
        if let digest, (try? Hashing.digest(backup, fs: fs)) != digest { throw RollbackError.backupChanged(backup) }
        try fs.remove(target)
        try fs.move(backup, to: target)
        log("restored \(target)")
    }

    func runScript(_ op: RollbackOp, output: String?, log: (String) -> Void) throws {
        guard let dir = op.releaseDir else { return }
        guard let raw = try? fs.read((dir as NSString).appendingPathComponent(ManifestVerifier.manifestFile)),
            let manifest = try? Manifest.decode(raw)
        else { throw RollbackError.scriptNotVerified("\(dir)/\(op.target)") }
        let script = try ScriptVerifier.runnable(op.target, scriptsDir: dir, manifest: manifest, fs: fs, scratch: scratch)
        let env = Installer(runner: runner, fs: fs, scriptsDir: dir).baseEnvironment(user).merging(op.env ?? [:]) { $1 }
        log(
            "running \(op.target) from \(dir) (\(env.filter { $0.key == "MODE" || $0.key == "ONLY" }.map { "\($0.key)=\($0.value)" }.sorted().joined(separator: " ")))"
        )
        let rc = runner.stream([Tool.bash, script], env: env, cwd: dir, output: output)
        if rc != 0 { throw RollbackError.scriptFailed(op.target, rc) }
    }

    func resolve(_ path: String, _ onESP: Bool, _ esp: String?) -> String {
        onESP ? ((esp ?? "") as NSString).appendingPathComponent(path) : path
    }
}
