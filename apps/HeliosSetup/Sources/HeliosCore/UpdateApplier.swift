// SPDX-License-Identifier: GPL-2.0-only
//
// UpdateApplier (root): applies one update unit from a verified release folder and returns its rollback record.
//   opencore           kexts / SSDTs on the internal ESP: the old copy becomes <name>.prev, the new one is copied
//                      and verified, then the unit's own Kernel/ACPI entries are merged into config.plist (backup
//                      first) and ocvalidate runs; a failed validation undoes the unit at once.
//   config             the release's DeviceProperties / NVRAM / boot-args into config.plist, same backup + check.
//   libraryExtensions  install-gpu.sh of the NEW release (MODE=dry-run, then MODE=install) for the part the
//   userspace          artifact belongs to (post_install `app.parts`), only when that part is installed here.
//   app                the app bundle is replaced, the previous one kept for rollback.
//   files              scripts, docs, licences: they live in the release folder already.
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

    public init(
        new: Manifest, newDir: String, old: Manifest?, oldDir: String?, appPath: String?, user: UserContext,
        output: String?, mountESP: @escaping () throws -> String
    ) {
        self.new = new
        self.newDir = newDir
        self.old = old
        self.oldDir = oldDir
        self.appPath = appPath
        self.user = user
        self.output = output
        self.mountESP = mountESP
    }
}

public enum ApplyError: Error, CustomStringConvertible, Equatable {
    case notInRelease(String)
    case scriptFailed(String, String, Int32)
    case noAppPath

    public var description: String {
        switch self {
        case .notInRelease(let n): return "\(n) is not in the new release"
        case .scriptFailed(let s, let mode, let rc): return "\(s) MODE=\(mode) failed (exit \(rc)); nothing more was changed"
        case .noAppPath: return "the app's own location is not known"
        }
    }
}

public struct UpdateApplier {
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

    public func apply(_ unit: UpdateUnit, _ ctx: UpdateContext, log: (String) -> Void) throws -> RollbackRecord {
        var record = RollbackRecord(
            unit: unit.id, fromTag: ctx.old?.release?.tag, toTag: ctx.new.release?.tag ?? "?",
            date: clock.now)
        switch unit.kind {
        case .files:
            log("\(unit.title): in the release folder \(ctx.newDir)")
        case .app:
            try replaceApp(unit, ctx, &record, log: log)
        case .userspace, .libraryExtensions:
            try runParts(unit, ctx, &record, log: log)
        case .opencore:
            let mp = try ctx.mountESP()
            do {
                try replaceOnESP(unit, ctx, mp: mp, &record, log: log)
                try mergeConfig(names: unit.artifacts, settings: false, ctx, mp: mp, &record, log: log)
            } catch {
                try? undo(record, ctx, esp: mp, log: log)
                throw error
            }
        case .config:
            let mp = try ctx.mountESP()
            do { try mergeConfig(names: [], settings: true, ctx, mp: mp, &record, log: log) } catch {
                try? undo(record, ctx, esp: mp, log: log)
                throw error
            }
        }
        return record
    }

    func undo(_ r: RollbackRecord, _ ctx: UpdateContext, esp: String?, log: (String) -> Void) throws {
        try RollbackExecutor(fs: fs, runner: runner, user: ctx.user).undo(r, esp: esp, output: ctx.output, log: log)
    }

    // MARK: opencore

    func replaceOnESP(
        _ unit: UpdateUnit, _ ctx: UpdateContext, mp: String, _ record: inout RollbackRecord,
        log: (String) -> Void
    ) throws {
        for name in unit.artifacts {
            guard let a = ctx.new.artifact(name) else { throw ApplyError.notInRelease(name) }
            let rel = "EFI/OC/\(a.kind == "ssdt" ? "ACPI" : "Kexts")/\(name)"
            let target = (mp as NSString).appendingPathComponent(rel)
            let existed = fs.exists(target)
            if !existed && ctx.old?.artifact(name) != nil {
                log("\(name): not in this Mac's EFI (left out when it was built): skipped")
                continue
            }
            let src =
                a.isZip
                ? ((ctx.newDir as NSString).appendingPathComponent("x") as NSString).appendingPathComponent(name)
                : (ctx.newDir as NSString).appendingPathComponent(a.file ?? name)
            if existed {
                let prev = rel + ".prev"
                try fs.remove((mp as NSString).appendingPathComponent(prev))
                try fs.move(target, to: (mp as NSString).appendingPathComponent(prev))
                record.ops.append(.restore(rel, from: prev, onESP: true))
            } else {
                record.ops.append(.remove(rel, onESP: true))
            }
            if fs.isDirectory(src) {
                try efi.copyTree(src, target)
                try efi.verify(src, target, what: rel)
            } else {
                try fs.copy(src, to: target)
            }
            log("\(rel): \(existed ? "replaced (previous kept as \(name).prev)" : "added") and verified")
        }
    }

    /// Merges the release's config delta (the entries of `names`, or the settings part) with a backup first.
    func mergeConfig(
        names: [String], settings: Bool, _ ctx: UpdateContext, mp: String,
        _ record: inout RollbackRecord, log: (String) -> Void
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
        let rel = "EFI/OC/config.plist"
        let path = (mp as NSString).appendingPathComponent(rel)
        let (merged, changes) = ConfigMerger.merge(try ConfigMerger.read(try fs.read(path)), delta: delta)
        for c in changes { log("config.plist \(c)") }
        guard changes.contains(where: { $0.kind != .warning }) else { return }
        let backupRel = "EFI/OC/config-backup-\(efi.backupName().dropFirst("EFI-backup-".count)).plist"
        try fs.copy(path, to: (mp as NSString).appendingPathComponent(backupRel))
        record.ops.append(.restore(rel, from: backupRel, onESP: true))
        try fs.write(try ConfigMerger.write(merged), to: path)
        let efiDir = (mp as NSString).appendingPathComponent("EFI")
        switch efi.validate(efi: efiDir, candidates: EFIManager.ocvalidateCandidates(efi: efiDir, releaseDir: ctx.newDir)) {
        case .passed: log("ocvalidate: no problems")
        case .skipped(let why): log("WARNING: \(why)")
        case .failed(let out): throw ConfigError.validationFailed(out)
        }
    }

    // MARK: install-gpu.sh parts

    func runParts(_ unit: UpdateUnit, _ ctx: UpdateContext, _ record: inout RollbackRecord, log: (String) -> Void) throws {
        let items = StepPlanner.items(ctx.new)
        var done = Set<String>()
        for name in unit.artifacts {
            guard let item = items.first(where: { $0.artifacts.contains(name) && !$0.env.isEmpty }),
                !done.contains(item.id), let script = item.script
            else { continue }
            done.insert(item.id)
            guard StepPlanner.installState(item, ctx: probes).all else {
                log("\(item.title): not installed on this Mac: skipped (install it from the Setup checklist)")
                continue
            }
            let env = item.env
            try run(script, dir: ctx.newDir, env: env.merging(["MODE": "dry-run"]) { $1 }, ctx, log: log)
            if let oldDir = ctx.oldDir, ctx.old?.artifact(script) != nil {
                record.ops.append(.script(script, releaseDir: oldDir, env: env.merging(["MODE": "install"]) { $1 }))
            }
            try run(script, dir: ctx.newDir, env: env.merging(["MODE": "install"]) { $1 }, ctx, log: log)
        }
    }

    func run(_ script: String, dir: String, env: [String: String], _ ctx: UpdateContext, log: (String) -> Void) throws {
        let installer = Installer(runner: runner, fs: fs, scriptsDir: dir)
        let path = try ScriptVerifier.path(of: script, scriptsDir: dir, manifest: ctx.new, fs: fs)
        let mode = env["MODE"] ?? ""
        log("\(script) MODE=\(mode) \(env["ONLY"].map { "ONLY=\"\($0)\"" } ?? "")")
        let rc = runner.stream(
            [Tool.bash, path], env: installer.baseEnvironment(ctx.user).merging(env) { $1 },
            cwd: dir, output: ctx.output)
        if rc != 0 { throw ApplyError.scriptFailed(script, mode, rc) }
    }

    // MARK: app

    func replaceApp(_ unit: UpdateUnit, _ ctx: UpdateContext, _ record: inout RollbackRecord, log: (String) -> Void) throws {
        guard let target = ctx.appPath, let name = unit.artifacts.first else { throw ApplyError.noAppPath }
        let src = ((ctx.newDir as NSString).appendingPathComponent("x") as NSString).appendingPathComponent(name)
        let backup = ((StatePaths.backups as NSString).appendingPathComponent(efi.backupName()) as NSString)
            .appendingPathComponent(name)
        try fs.makeDirectory((backup as NSString).deletingLastPathComponent)
        try fs.move(target, to: backup)
        record.ops.append(.restore(target, from: backup, onESP: false))
        try fs.copy(src, to: target)
        log("\(name) replaced (previous kept in \(backup)); it restarts itself")
    }
}
