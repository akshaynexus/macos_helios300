// SPDX-License-Identifier: GPL-2.0-only
//
// UpdateApplier (root): applies one update unit from a verified release folder and returns its rollback record.
// A unit is applied whole or not at all: every part is done, and any failure undoes the parts already done.
//   opencore           kexts / SSDTs on the internal ESP: a StagedSwap per file (<name>.new -> verify -> old copy to
//                      <name>.prev-N -> rename in), then the unit's own Kernel/ACPI entries, for the files actually on
//                      this ESP, are merged into a config.plist.new that ocvalidate checks BEFORE it replaces config.plist.
//   config             the release's DeviceProperties / NVRAM / boot-args into config.plist, same staging + check.
//   libraryExtensions  install-gpu.sh of the NEW release (MODE=dry-run for every part first, then MODE=install) for the
//   userspace          part the artifact belongs to (post_install `app.parts`), only when that part is installed here.
//   app                the app bundle, by the same StagedSwap next to itself; the running app keeps working until the
//                      user reopens it (the Updates page offers to).
//   files              scripts, docs, licences: they live in the release folder already.
//   removal            a dropped kext or SSDT leaves the ESP (kept for rollback) and its config entries go; other kinds
//                      stay until the user undoes their step.
// `journal` is called with the record so far BEFORE each change, so a run that gets killed can be undone from it.
import Foundation

public struct UpdateContext {
    public var new: Manifest
    public var newDir: String
    public var old: Manifest?
    public var oldDir: String?
    public var appPath: String?
    public var user: UserContext
    public var output: String?
    public var mountESP: () throws -> String
    /// Set on the root side: scripts and ocvalidate run from root-owned copies here.
    public var scratch: RootScratch?

    public init(
        new: Manifest, newDir: String, old: Manifest?, oldDir: String?, appPath: String?, user: UserContext,
        output: String?, mountESP: @escaping () throws -> String, scratch: RootScratch? = nil
    ) {
        self.new = new
        self.newDir = newDir
        self.old = old
        self.oldDir = oldDir
        self.appPath = appPath
        self.user = user
        self.output = output
        self.mountESP = mountESP
        self.scratch = scratch
    }
}

public enum ApplyError: Error, CustomStringConvertible, Equatable {
    case notInRelease(String)
    case scriptFailed(String, String, Int32)
    case noAppPath
    case keptCopyWithoutTarget(String)
    case undoFailed(String, String)

    public var description: String {
        switch self {
        case .notInRelease(let n): return "\(n) is not in the new release"
        case .scriptFailed(let s, let mode, let rc): return "\(s) MODE=\(mode) failed (exit \(rc))"
        case .noAppPath: return "the app's own location is not known"
        case .keptCopyWithoutTarget(let p):
            return "\(p) is missing but a kept copy of it exists: use Roll back, or restore it by hand"
        case .undoFailed(let why, let undo):
            return "\(why), and undoing what was already done also failed (\(undo)); the next run tries the undo again"
        }
    }
}

public struct UpdateApplier {
    public typealias Journal = (RollbackRecord) throws -> Void

    let fs: FileSystem
    let runner: CommandRunner
    let clock: Clock
    let probes: ProbeContext

    public init(fs: FileSystem, runner: CommandRunner, clock: Clock, probes: ProbeContext) {
        self.fs = fs
        self.runner = runner
        self.clock = clock
        self.probes = probes
    }

    var efi: EFIManager { EFIManager(runner: runner, fs: fs, clock: clock) }

    /// Applies a whole unit. On any failure the parts already done are undone and the first error is thrown.
    public func apply(
        _ unit: UpdateUnit, _ ctx: UpdateContext, journal: Journal = { _ in }, log: (String) -> Void
    ) throws -> RollbackRecord {
        var record = RollbackRecord(
            unit: unit.id, fromTag: ctx.old?.release?.tag, toTag: ctx.new.release?.tag ?? "?",
            date: clock.now)
        var esp: String?
        do {
            try applyParts(unit, ctx, &record, &esp, journal, log)
        } catch {
            do { try undo(record, ctx, esp: esp, log: log) } catch let undoError {
                throw ApplyError.undoFailed("\(error)", "\(undoError)")
            }
            throw error
        }
        return record
    }

    func applyParts(
        _ unit: UpdateUnit, _ ctx: UpdateContext, _ record: inout RollbackRecord, _ esp: inout String?,
        _ journal: Journal, _ log: (String) -> Void
    ) throws {
        if unit.removal { return try remove(unit, ctx, &record, &esp, journal, log) }
        if unit.kind == .config {
            let mp = try ctx.mountESP()
            esp = mp
            return try mergeConfig(names: [], settings: true, ctx, mp: mp, &record, journal, log)
        }
        let parts = UpdatePlanner.parts(unit, manifest: ctx.new)
        // Every dry run before the first change.
        let scripts = try dryRunScripts((parts[.userspace] ?? []) + (parts[.libraryExtensions] ?? []), ctx, log)
        if let names = parts[.opencore] {
            let mp = try ctx.mountESP()
            esp = mp
            let onESP = try replaceOnESP(names, ctx, mp: mp, &record, journal, log)
            try mergeConfig(names: onESP, settings: false, ctx, mp: mp, &record, journal, log)
        }
        try installScripts(scripts, ctx, &record, journal, log)
        if let names = parts[.app] { try replaceApp(names, ctx, &record, journal, log) }
        for name in parts[.files] ?? [] { log("\(name): in the release folder \(ctx.newDir)") }
    }

    func undo(_ r: RollbackRecord, _ ctx: UpdateContext, esp: String?, log: (String) -> Void) throws {
        try RollbackExecutor(fs: fs, runner: runner, user: ctx.user, scratch: ctx.scratch).undo(
            r, esp: esp, output: ctx.output, log: log)
    }

    // MARK: staged swap

    /// Replaces `rel` on the ESP from `source`: staged and verified first, the undo persisted, then the two renames.
    /// Returns the backup's path relative to the ESP (nil when there was nothing to keep).
    @discardableResult
    public func swapOntoESP(
        _ source: String, rel: String, mp: String, _ record: inout RollbackRecord, _ journal: Journal
    ) throws -> String? {
        let target = (mp as NSString).appendingPathComponent(rel)
        return try swap(source, target: target, what: rel, label: rel, onESP: true, &record, journal)
    }

    /// `label`: how the op names the target (relative on the ESP, absolute elsewhere).
    func swap(
        _ source: String, target: String, what: String, label: String, onESP: Bool, _ record: inout RollbackRecord,
        _ journal: Journal
    ) throws -> String? {
        let efi = self.efi
        let swap = StagedSwap(fs: fs, copy: efi.copyTree, verify: efi.verify)
        try swap.stage(source, target: target, what: what)
        let staged = label + StagedSwap.stagedSuffix
        var keptAs: String?
        var backup: String?
        if fs.exists(target) {
            let b = swap.nextBackup(target)
            backup = b
            keptAs = label + String(b.dropFirst(target.count))
            record.ops.append(
                .restore(label, from: keptAs ?? b, onESP: onESP, digest: try Hashing.digest(target, fs: fs), staged: staged))
        } else {
            record.ops.append(.remove(label, onESP: onESP, staged: staged))
        }
        try journal(record)
        try swap.commit(source, target: target, backup: backup, what: what)
        return keptAs
    }

    /// The names actually on this ESP afterwards.
    func replaceOnESP(
        _ names: [String], _ ctx: UpdateContext, mp: String, _ record: inout RollbackRecord, _ journal: Journal,
        _ log: (String) -> Void
    ) throws -> [String] {
        var done: [String] = []
        let efi = self.efi
        for name in names {
            guard let a = ctx.new.artifact(name) else { throw ApplyError.notInRelease(name) }
            let rel = EFIManager.espPath(of: a)
            let target = (mp as NSString).appendingPathComponent(rel)
            if !fs.exists(target) && ctx.old?.artifact(name) != nil {
                guard !StagedSwap(fs: fs, copy: efi.copyTree, verify: efi.verify).hasBackup(target) else {
                    throw ApplyError.keptCopyWithoutTarget(rel)
                }
                log("\(name): not in this Mac's EFI (left out when it was built): skipped")
                continue
            }
            let src =
                a.isZip
                ? ((ctx.newDir as NSString).appendingPathComponent("x") as NSString).appendingPathComponent(name)
                : (ctx.newDir as NSString).appendingPathComponent(a.file ?? name)
            let kept = try swapOntoESP(src, rel: rel, mp: mp, &record, journal)
            log(
                "\(rel): \(kept.map { "replaced (previous kept as \(($0 as NSString).lastPathComponent))" } ?? "added") and verified"
            )
            done.append(name)
        }
        return done
    }

    // MARK: config.plist

    /// Merges the release's config delta (the entries of `names`, or the settings part).
    func mergeConfig(
        names: [String], settings: Bool, _ ctx: UpdateContext, mp: String,
        _ record: inout RollbackRecord, _ journal: Journal, _ log: (String) -> Void
    ) throws {
        guard let full = ctx.new.configDelta else { return }
        var delta = ConfigDelta()
        if settings {
            delta = full
            delta.kernelAdd = []
            delta.acpiAdd = []
        } else {
            delta.kernelAdd = full.kernelAdd.filter { names.contains($0.bundlePath.components(separatedBy: "/")[0]) }
            delta.acpiAdd = full.acpiAdd.filter { names.contains($0.path) }
        }
        try updateConfig(ctx, mp: mp, &record, journal, log) { ConfigMerger.merge($0, delta: delta) }
    }

    /// config.plist.new is written and validated while config.plist is untouched, then swapped in.
    func updateConfig(
        _ ctx: UpdateContext, mp: String, _ record: inout RollbackRecord, _ journal: Journal, _ log: (String) -> Void,
        change: ([String: Any]) -> ([String: Any], [ConfigChange])
    ) throws {
        let rel = EFIManager.configPath
        let path = (mp as NSString).appendingPathComponent(rel)
        let (merged, changes) = change(try ConfigMerger.read(try fs.read(path)))
        for c in changes { log("config.plist \(c)") }
        guard changes.contains(where: { $0.kind != .warning }) else { return }
        let data = try ConfigMerger.write(merged)
        let staged = StagedSwap.staged(path)
        try fs.write(data, to: staged)
        let efiDir = (mp as NSString).appendingPathComponent("EFI")
        let candidates = EFIManager.ocvalidateCandidates(efi: efiDir, releaseDir: ctx.newDir)
        switch efi.validate(efi: efiDir, config: staged, candidates: candidates, scratch: ctx.scratch) {
        case .passed: log("ocvalidate: no problems")
        case .skipped(let why): log("WARNING: \(why)")
        case .failed(let out):
            try? fs.remove(staged)
            throw ConfigError.validationFailed(out)
        }
        let fs = self.fs
        let swap = StagedSwap(fs: fs, copy: { _, _ in }) { _, copy, what in
            guard try fs.read(copy) == data else { throw EFIError.verifyFailed(what, copy) }
        }
        let backup = swap.nextBackup(path)
        record.ops.append(
            .restore(
                rel, from: rel + String(backup.dropFirst(path.count)), onESP: true, digest: try Hashing.digest(path, fs: fs),
                staged: rel + StagedSwap.stagedSuffix))
        try journal(record)
        try swap.commit(staged, target: path, backup: backup, what: rel)
    }

    // MARK: install-gpu.sh parts

    struct ScriptRun {
        let item: SetupItem
        let script: String
    }

    /// One dry run per installed part; parts this Mac does not have are skipped.
    func dryRunScripts(_ names: [String], _ ctx: UpdateContext, _ log: (String) -> Void) throws -> [ScriptRun] {
        let items = StepPlanner.items(ctx.new)
        var seen = Set<String>()
        var runs: [ScriptRun] = []
        for name in names {
            guard let item = items.first(where: { $0.artifacts.contains(name) && !$0.env.isEmpty }),
                !seen.contains(item.id), let script = item.script
            else { continue }
            seen.insert(item.id)
            guard StepPlanner.installState(item, ctx: probes).all else {
                log("\(item.title): not installed on this Mac: skipped (install it from the Setup checklist)")
                continue
            }
            try run(script, dir: ctx.newDir, env: item.env.merging(["MODE": "dry-run"]) { $1 }, ctx, log: log)
            runs.append(ScriptRun(item: item, script: script))
        }
        return runs
    }

    func installScripts(
        _ runs: [ScriptRun], _ ctx: UpdateContext, _ record: inout RollbackRecord, _ journal: Journal, _ log: (String) -> Void
    ) throws {
        for r in runs {
            let env = r.item.env.merging(["MODE": "install"]) { $1 }
            if let oldDir = ctx.oldDir, ctx.old?.artifact(r.script) != nil {
                record.ops.append(.script(r.script, releaseDir: oldDir, env: env))
                try journal(record)
            }
            try run(r.script, dir: ctx.newDir, env: env, ctx, log: log)
        }
    }

    func run(_ script: String, dir: String, env: [String: String], _ ctx: UpdateContext, log: (String) -> Void) throws {
        let installer = Installer(runner: runner, fs: fs, scriptsDir: dir, scratch: ctx.scratch)
        let path = try ScriptVerifier.runnable(script, scriptsDir: dir, manifest: ctx.new, fs: fs, scratch: ctx.scratch)
        let mode = env["MODE"] ?? ""
        log("\(script) MODE=\(mode) \(env["ONLY"].map { "ONLY=\"\($0)\"" } ?? "")")
        let rc = runner.stream(
            [Tool.bash, path], env: installer.baseEnvironment(ctx.user).merging(env) { $1 },
            cwd: dir, output: ctx.output)
        if rc != 0 { throw ApplyError.scriptFailed(script, mode, rc) }
    }

    // MARK: app

    func replaceApp(
        _ names: [String], _ ctx: UpdateContext, _ record: inout RollbackRecord, _ journal: Journal, _ log: (String) -> Void
    ) throws {
        guard let target = ctx.appPath, let name = names.first else { throw ApplyError.noAppPath }
        let src = ((ctx.newDir as NSString).appendingPathComponent("x") as NSString).appendingPathComponent(name)
        let kept = try swap(src, target: target, what: name, label: target, onESP: false, &record, journal)
        log("\(name) replaced\(kept.map { " (previous kept as \($0))" } ?? ""). Quit and reopen the app to use the new version.")
    }

    // MARK: removal

    func remove(
        _ unit: UpdateUnit, _ ctx: UpdateContext, _ record: inout RollbackRecord, _ esp: inout String?, _ journal: Journal,
        _ log: (String) -> Void
    ) throws {
        var kexts: [String] = []
        var ssdts: [String] = []
        for name in unit.artifacts {
            guard let a = ctx.old?.artifact(name) else { continue }
            guard UpdatePlanner.kind(a) == .opencore else {
                log("\(name) is no longer part of the release; it stays until you undo its step in Setup")
                continue
            }
            let mp: String
            if let known = esp { mp = known } else { mp = try ctx.mountESP() }
            esp = mp
            let rel = EFIManager.espPath(of: a)
            let target = (mp as NSString).appendingPathComponent(rel)
            guard fs.exists(target) else {
                log("\(name): not in this Mac's EFI: nothing to remove")
                continue
            }
            let swap = StagedSwap(fs: fs, copy: { _, _ in }, verify: { _, _, _ in })
            let backup = swap.nextBackup(target)
            record.ops.append(
                .restore(
                    rel, from: rel + String(backup.dropFirst(target.count)), onESP: true,
                    digest: try Hashing.digest(target, fs: fs))
            )
            try journal(record)
            try fs.move(target, to: backup)
            log("\(rel): removed (kept as \((backup as NSString).lastPathComponent))")
            if a.kind == "ssdt" { ssdts.append(a.name) } else { kexts.append(a.name) }
        }
        if let esp, !kexts.isEmpty || !ssdts.isEmpty {
            try updateConfig(ctx, mp: esp, &record, journal, log) { ConfigMerger.remove($0, kexts: kexts, ssdts: ssdts) }
        }
    }
}
