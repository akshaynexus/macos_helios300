// SPDX-License-Identifier: GPL-2.0-only
//
// RootOps: the root half (`helios-setup _root --user NAME --home HOME [--resources DIR] <command>`). It re-checks what
// a root process must not take on trust (the manifest and its signature for updates, the release files, the scripts,
// one driver change per restart), records the change in the system state BEFORE making it, then makes it. The dry-run
// rule stays with the user side: its record lives in the user's own state, which root cannot treat as evidence.
// Anything it starts runs from a root-owned copy (RootScratch). Commands:
//   step <id> <action> --release DIR [--efi-source DIR]   a checklist action (scripts, sudo commands, efi ...)
//   update-apply --staged DIR --sha SHA --baseline-sha SHA|none [--baseline-dir DIR] [--app PATH]   the next batch
//   rollback                                               undo the last applied update unit
//   gpu-apps add|remove APP --release DIR                  edit the GTX 1060 allow list
//   thirdparty-apply --product P --zip Z --sha S --bundle B...
import Foundation

/// Lines for the user: appended to the log file the GUI tails (never through a symbolic link), or printed.
public struct LogSink {
    let path: String?

    public init(path: String?) { self.path = path }

    public func callAsFunction(_ line: String) {
        let data = Data((line + "\n").utf8)
        guard let path else {
            FileHandle.standardOutput.write(data)
            return
        }
        openLogForAppending(path)?.write(data)
    }
}

/// The release an update is planned against.
public struct UpdateBaseline: Equatable {
    public var manifest: Manifest
    public var tag: String?
    public var sha256: String
    public var dir: String
}

public struct RootOps {
    let env: Environment

    public init(env: Environment) { self.env = env }

    /// Rollback records older than the newest this many releases are dropped (their kept copies with them).
    static let releasesKept = 3

    var fs: FileSystem { env.fs }
    var store: StateStore { env.store }
    var efi: EFIManager { EFIManager(runner: env.runner, fs: env.fs, clock: env.clock) }
    var applier: UpdateApplier { UpdateApplier(fs: fs, runner: env.runner, clock: env.clock, probes: probes()) }

    func boot() -> String { ProbeContext(runner: env.runner, fs: env.fs).bootSession }

    func probes() -> ProbeContext { Engine(env: env).probes() }

    /// Runs `body` with a root-owned scratch directory that is removed afterwards.
    func withScratch<T>(_ body: (RootScratch) throws -> T) throws -> T {
        let scratch = try RootScratch.make(fs: fs, parent: env.rootRunDir)
        defer { scratch.remove() }
        return try body(scratch)
    }

    func mountESP(_ log: LogSink) throws -> String {
        let mp = try efi.mount(try efi.internalESP())
        try efi.recoverSwap(esp: mp, log: log.callAsFunction)
        return mp
    }

    /// A run that was killed inside a unit left its undo in the state: finish that first, so nothing else ever sees a
    /// half-done unit. Its change record goes too, unless a script ran (then the kexts may have changed: restart first).
    func recoverInterrupted(_ s: inout SystemState, scratch: RootScratch?, output: String?, log: LogSink) throws {
        guard let rec = s.interrupted else { return }
        log("== an earlier run stopped inside \(rec.unit): undoing it first")
        let esp = rec.needsESP ? try mountESP(log) : nil
        try RollbackExecutor(fs: fs, runner: env.runner, user: env.user, scratch: scratch).undo(
            rec, esp: esp, output: output
        ) { log($0) }
        s.interrupted = nil
        if !rec.ops.contains(where: { $0.kind == .script }) {
            let b = boot()
            s.changes.removeAll { $0.bootSession == b && ($0.item == rec.unit || $0.item == "update:\(rec.unit)") }
        }
        try store.saveSystem(s)
        log("\(rec.unit): undone; what was there before is back in place")
    }

    // MARK: step

    public func step(_ id: String, _ action: String, releaseDir: String, efiSource: String?, output: String?) -> Int32 {
        let log = LogSink(path: output)
        do {
            let (m, sha) = try loadChecked(releaseDir)
            guard let item = StepPlanner.items(m).first(where: { $0.id == id }) else { throw EngineError.unknownItem(id) }
            guard let line = item.command(action) else { throw InstallerError.noCommand(id, action) }
            log("== \(item.title): \(action) (\(line))")
            // Only a real root job has a scratch; `status` and plain commands run in the user's own process.
            guard Installer.needsRoot(item, action: action) else {
                return try runStep(item, action, line, m, releaseDir: releaseDir, efiSource: efiSource, scratch: nil, output, log)
            }
            return try withScratch { scratch in
                if RestartGuard.changeActions.contains(action) {
                    var s = try store.readSystem()
                    try recoverInterrupted(&s, scratch: scratch, output: output, log: log)
                    try recordChange(item, action)
                    try adoptIfNone(m, sha: sha, dir: releaseDir)
                }
                return try runStep(
                    item, action, line, m, releaseDir: releaseDir, efiSource: efiSource, scratch: scratch, output, log)
            }
        } catch {
            log("STOP: \(error)")
            return 1
        }
    }

    func runStep(
        _ item: SetupItem, _ action: String, _ line: String, _ m: Manifest, releaseDir: String, efiSource: String?,
        scratch: RootScratch?, _ output: String?, _ log: LogSink
    ) throws -> Int32 {
        if case .cli(let args) = try CommandParser.parse(line) {
            return try cli(args, item: item, releaseDir: releaseDir, efiSource: efiSource, scratch: scratch, log: log)
        }
        let installer = Installer(runner: env.runner, fs: fs, scriptsDir: env.resourcesDir, scratch: scratch)
        guard let plan = try installer.plan(item, action: action, manifest: m, releaseDir: releaseDir, user: env.user)
        else { return 0 }
        let rc = installer.execute(plan, output: output)
        log("== exit \(rc)\(rc == 0 && item.reboot && action != "check" && action != "status" ? ": restart to finish" : "")")
        return rc
    }

    /// Re-reads the release and checks it (the user side did too; this side trusts nothing it was handed).
    func loadChecked(_ dir: String) throws -> (Manifest, String) {
        let raw = try fs.read((dir as NSString).appendingPathComponent(ManifestVerifier.manifestFile))
        let m = try Manifest.decode(raw)
        try ManifestCheck.check(m, local: true)
        return (m, Hashing.sha256(raw))
    }

    func recordChange(_ item: SetupItem, _ action: String) throws {
        var s = try store.readSystem()
        let b = boot()
        if case .restartFirst(let why) = RestartGuard.check(item, action: action, system: s, boot: b) {
            throw EngineError.blocked(why)
        }
        s.record(
            ChangeRecord(
                item: item.id, action: action, bootSession: b, date: env.clock.now, driverChange: item.driverChange,
                needsRestart: item.reboot))
        try store.saveSystem(s)
    }

    /// The first change made from a release makes it the installed one (the baseline for updates).
    func adoptIfNone(_ m: Manifest, sha: String, dir: String) throws {
        var s = try store.readSystem()
        guard s.installed == nil, let rel = m.release, rel.dryRun != true else { return }
        let raw = try fs.read((dir as NSString).appendingPathComponent(ManifestVerifier.manifestFile))
        guard Hashing.sha256(raw) == sha else { throw UpdateError.manifestChanged(dir) }
        let dest = releaseDir(rel.tag)
        if !fs.exists(dest) { try copyRelease(m, raw: raw, signature: nil, from: dir, to: dest) }
        s.installed = InstalledRelease(
            tag: rel.tag, version: rel.version, manifestSha256: sha,
            releaseDir: dest, date: env.clock.now, preRelease: rel.preRelease ?? true)
        try store.saveSystem(s)
    }

    func releaseDir(_ tag: String) -> String { (env.releasesDir as NSString).appendingPathComponent(tag) }

    /// Copies what `m` lists into a root-owned folder: the manifest bytes this side checked, and every release file that
    /// is not a symbolic link. Replaces `dest` in one rename.
    func copyRelease(_ m: Manifest, raw: Data, signature: Data?, from dir: String, to dest: String) throws {
        let tmp = StagedSwap.staged(dest)
        try fs.remove(tmp)
        try fs.write(raw, to: (tmp as NSString).appendingPathComponent(ManifestVerifier.manifestFile))
        if let signature { try fs.write(signature, to: (tmp as NSString).appendingPathComponent(ManifestVerifier.signatureFile)) }
        for a in m.artifacts {
            guard let file = a.file else { continue }
            let src = (dir as NSString).appendingPathComponent(file)
            guard !fs.isSymlink(src) else { throw RootScratchError.symlink(src) }
            if fs.exists(src) { try fs.copy(src, to: (tmp as NSString).appendingPathComponent(file)) }
        }
        try fs.ownAsRoot(tmp)
        try fs.remove(dest)
        try fs.move(tmp, to: dest)
    }

    // MARK: efi (the OpenCore step's `helios-setup efi ...` commands)

    func cli(
        _ args: [String], item: SetupItem, releaseDir: String, efiSource: String?, scratch: RootScratch?, log: LogSink
    ) throws -> Int32 {
        guard args.first == "efi", args.count >= 2 else {
            throw EngineError.failed("unknown command helios-setup \(args.joined(separator: " "))")
        }
        let source = efiSource ?? defaultEFISource(releaseDir)
        switch args[1] {
        case "status": return efiStatus(log: log)
        case "check": return try efiCheck(source: source, releaseDir: releaseDir, scratch: scratch, log: log)
        case "install": return try efiInstall(source: source, releaseDir: releaseDir, scratch: scratch, log: log)
        case "restore": return try efiRestore(args.count > 2 ? args[2] : nil, log: log)
        default: throw EngineError.failed("unknown efi command \(args[1])")
        }
    }

    /// EFI/Helios300 on the stick -> EFI/.
    func defaultEFISource(_ releaseDir: String) -> String? {
        let parent = (releaseDir as NSString).deletingLastPathComponent
        return (try? efi.sourceEFI(parent))
    }

    func efiStatus(log: LogSink) -> Int32 {
        guard let esp = try? efi.internalESP() else { log("no internal EFI partition found"); return 1 }
        let i = efi.info(esp)
        log("internal EFI partition: \(esp), mounted at \(i.mountPoint ?? "(not mounted)")")
        log("this boot came from: \(probes().bootSource)")
        if let oc = probes().nvram("\(Tool.ocGUID):opencore-version") { log("running OpenCore: \(oc)") }
        if let mp = i.mountPoint { log("backups: \(efi.backups(esp: mp).joined(separator: ", "))") }
        return 0
    }

    func efiCheck(source: String?, releaseDir: String, scratch: RootScratch?, log: LogSink) throws -> Int32 {
        guard let source else { throw EFIError.notAnEFI("(no EFI folder chosen and none next to the release)") }
        let src = try efi.sourceEFI(source)
        log("source: \(src)")
        let esp = try efi.internalESP()
        let mp = try mountESP(log)
        log("internal EFI partition: \(esp) at \(mp); free \(fs.freeBytes(mp).map { "\($0 / 1_048_576) MiB" } ?? "?")")
        let had = fs.exists((mp as NSString).appendingPathComponent("EFI/OC/OpenCore.efi"))
        log(had ? "it has an OpenCore now: install backs it up to EFI-backup-<date> first" : "it has no OpenCore yet")
        try efi.checkSpace(source: src, existing: had ? (mp as NSString).appendingPathComponent("EFI") : nil, esp: mp)
        log("space: enough for the backup and the new copy")
        report(
            efi.validate(
                efi: src, candidates: EFIManager.ocvalidateCandidates(efi: src, releaseDir: releaseDir), scratch: scratch), log)
        log("this boot came from: \(probes().bootSource)")
        log("Check done: nothing was changed")
        return 0
    }

    func efiInstall(source: String?, releaseDir: String, scratch: RootScratch?, log: LogSink) throws -> Int32 {
        guard let source else { throw EFIError.notAnEFI("(no EFI folder chosen)") }
        let src = try efi.sourceEFI(source)
        if case .failed(let out) = efi.validate(
            efi: src, candidates: EFIManager.ocvalidateCandidates(efi: src, releaseDir: releaseDir), scratch: scratch)
        {
            throw ConfigError.validationFailed(out)
        }
        let mp = try mountESP(log)
        let backup = try efi.install(from: src, esp: mp) { log($0) }
        var s = try store.readSystem()
        s.efiBackups = (s.efiBackups + [backup].compactMap { $0 }).filter(efi.backups(esp: mp).contains)
        try store.saveSystem(s)
        if let backup { log("previous EFI kept as \(backup) (Undo puts it back)") }
        _ = env.runner.capture([Tool.diskutil, "unmount", mp])  // as OCLP's install.py does afterwards
        log("Installed. Restart; if the Mac still starts from the stick, see Help > Boot order.")
        return 0
    }

    func efiRestore(_ name: String?, log: LogSink) throws -> Int32 {
        let mp = try mountESP(log)
        var s = try store.readSystem()
        guard let b = name ?? s.efiBackups.last(where: efi.backups(esp: mp).contains) ?? efi.backups(esp: mp).last else {
            throw EFIError.noBackup("(none)")
        }
        let kept = try efi.restore(backup: b, esp: mp) { log($0) }
        s.efiBackups = (s.efiBackups + [kept].compactMap { $0 }).filter(efi.backups(esp: mp).contains)
        try store.saveSystem(s)
        _ = env.runner.capture([Tool.diskutil, "unmount", mp])
        log("\(b) is back in place\(kept.map { "; what was there is kept as \($0)" } ?? ""). Restart.")
        return 0
    }

    func report(_ v: ValidationResult, _ log: LogSink) {
        switch v {
        case .passed: log("ocvalidate: no problems")
        case .failed(let out): log("ocvalidate reports problems:\n\(out)")
        case .skipped(let why): log("WARNING: \(why)")
        }
    }

    // MARK: updates

    /// What the update is measured against on this side: the installed release, from the root-owned releases folder.
    func installedBaseline(_ s: SystemState) -> UpdateBaseline? {
        guard let i = s.installed else { return nil }
        let dir = releaseDir(i.tag)
        guard let raw = try? fs.read((dir as NSString).appendingPathComponent(ManifestVerifier.manifestFile)),
            let m = try? Manifest.decode(raw)
        else { return nil }
        return UpdateBaseline(manifest: m, tag: i.tag, sha256: Hashing.sha256(raw), dir: dir)
    }

    /// `baselineSha`: what the user saw the plan against ("none" for no installed release). `baselineDir`: where it came
    /// from when nothing is recorded yet (the release folder the app works from), which then becomes the installed one.
    public func updateApply(
        staged: String, sha: String, baselineSha: String, baselineDir: String?, appPath: String?, output: String?
    ) -> Int32 {
        let log = LogSink(path: output)
        do {
            if let appPath, !isAppBundlePath(appPath) { throw ApplyError.noAppPath }
            return try withScratch { scratch in
                let raw = try fs.read((staged as NSString).appendingPathComponent(ManifestVerifier.manifestFile))
                guard Hashing.sha256(raw) == sha else { throw UpdateError.manifestChanged(staged) }
                guard let signature = try? fs.read((staged as NSString).appendingPathComponent(ManifestVerifier.signatureFile))
                else {
                    throw SignatureError.missing(staged)
                }
                try env.manifestVerifier.verify(raw, signature: signature, what: "the manifest in \(staged)")
                let m = try Manifest.decode(raw)
                try ManifestCheck.check(m)
                guard let tag = m.release?.tag else { throw UpdateError.noRelease }
                let darwin = probes().darwin
                guard ManifestCheck.covers(m, darwin: darwin) else { throw UpdateError.notForThisMacOS(tag, darwin) }
                var s = try store.readSystem()
                try recoverInterrupted(&s, scratch: scratch, output: output, log: log)
                if s.installed == nil, let dir = baselineDir, baselineSha != "none" {
                    let (bm, bsha) = try loadChecked(dir)
                    guard bsha == baselineSha else { throw UpdateError.baselineChanged(baselineSha, bsha) }
                    try adoptIfNone(bm, sha: bsha, dir: dir)
                    s = try store.readSystem()
                }
                let baseline = installedBaseline(s)
                guard (baseline?.sha256 ?? "none") == baselineSha else {
                    throw UpdateError.baselineChanged(baselineSha, baseline?.sha256 ?? "none")
                }
                let dest = releaseDir(tag)
                let updater = Updater(http: env.http, fs: fs, unzipper: env.unzipper, verifier: env.manifestVerifier)
                if (try? fs.read((dest as NSString).appendingPathComponent(ManifestVerifier.manifestFile))) != raw {
                    try copyRelease(m, raw: raw, signature: signature, from: staged, to: dest)
                }
                try updater.verifyStaged(dest, manifest: m)
                try fs.ownAsRoot(dest)  // the bundles extracted by verifyStaged too
                log("release \(tag): every file verified in \(dest)")
                return try applyBatch(
                    m, sha: sha, dir: dest, baseline: baseline, appPath: appPath, scratch: scratch, state: &s, output: output,
                    log: log)
            }
        } catch {
            log("STOP: \(error)")
            return 1
        }
    }

    /// `--app` comes from the user side and is where root renames things.
    func isAppBundlePath(_ p: String) -> Bool {
        p.hasPrefix("/") && p.hasSuffix(".app") && !p.split(separator: "/").contains("..") && fs.isDirectory(p)
    }

    func applyBatch(
        _ m: Manifest, sha: String, dir: String, baseline: UpdateBaseline?, appPath: String?, scratch: RootScratch,
        state s: inout SystemState, output: String?, log: LogSink
    ) throws -> Int32 {
        let plan = UpdatePlanner.plan(old: baseline?.manifest, new: m, diff: ManifestDiff.between(baseline?.manifest, m))
        let tag = m.release?.tag ?? "?"
        if let p = s.pending, p.tag != tag || p.manifestSha256 != sha {
            guard p.applied.isEmpty else { throw UpdateError.otherUpdatePending(p.tag) }
            s.pending = nil
        }
        if s.pending == nil { s.pending = PendingUpdate(tag: tag, manifestSha256: sha, dir: dir) }
        let applied = Set(s.pending?.applied ?? [])
        guard let batch = UpdatePlanner.nextBatch(plan, applied: applied) else {
            finish(&s, m, sha: sha, dir: dir, log: log)
            try store.saveSystem(s)
            log("\(tag) is fully installed")
            return 0
        }
        if batch.contains(where: \.requiresRestart),
            case .restartFirst(let why) = RestartGuard.checkDriverChange(system: s, boot: boot())
        {
            throw EngineError.blocked(why)
        }
        let ctx = UpdateContext(
            new: m, newDir: dir, old: baseline?.manifest, oldDir: baseline?.dir, appPath: appPath, user: env.user,
            output: output, mountESP: { try mountESP(log) }, scratch: scratch)
        let applier = self.applier
        for unit in batch {
            s.record(
                ChangeRecord(
                    item: "update:\(unit.id)", action: "update \(tag)", bootSession: boot(), date: env.clock.now,
                    driverChange: unit.requiresRestart, needsRestart: unit.requiresRestart))
            s.interrupted = RollbackRecord(unit: unit.id, fromTag: baseline?.tag, toTag: tag, date: env.clock.now)
            try store.saveSystem(s)
            log("== \(unit.title)")
            let record: RollbackRecord
            do {
                var state = s
                record = try applier.apply(
                    unit, ctx,
                    journal: { journalled in
                        state.interrupted = journalled
                        try store.saveSystem(state)
                    }
                ) { log($0) }
            } catch {
                // apply undid the unit itself, unless it says the undo failed: then the journal has to stay.
                if !isUndoFailure(error) {
                    s.interrupted = nil
                    try? store.saveSystem(s)
                }
                throw error
            }
            s.rollbacks.append(record)
            s.pending?.applied.append(unit.id)
            s.interrupted = nil
            try store.saveSystem(s)
        }
        if UpdatePlanner.nextBatch(plan, applied: Set(s.pending?.applied ?? [])) == nil {
            finish(&s, m, sha: sha, dir: dir, log: log)
            log("\(tag) is fully installed\(batch.contains(where: \.requiresRestart) ? ": restart" : "")")
        } else {
            log("restart, then continue the update (one driver change per restart)")
        }
        try store.saveSystem(s)
        return 0
    }

    func isUndoFailure(_ error: Error) -> Bool {
        if case .undoFailed = error as? ApplyError { return true }
        return false
    }

    func finish(_ s: inout SystemState, _ m: Manifest, sha: String, dir: String, log: LogSink) {
        guard let rel = m.release else { return }
        if s.installed?.tag != rel.tag { s.previous = s.installed }
        s.installed = InstalledRelease(
            tag: rel.tag, version: rel.version, manifestSha256: sha, releaseDir: dir,
            date: env.clock.now, preRelease: rel.preRelease ?? true)
        s.pending = nil
        pruneRollbacks(&s, log: log)
    }

    /// Kept copies are bounded: the records of the newest `releasesKept` releases stay, older ones are forgotten.
    func pruneRollbacks(_ s: inout SystemState, log: LogSink) {
        var seen: [String] = []
        for r in s.rollbacks.reversed() where !seen.contains(r.toTag) { seen.append(r.toTag) }
        let keep = Set(seen.prefix(Self.releasesKept))
        let old = s.rollbacks.filter { !keep.contains($0.toTag) }
        guard !old.isEmpty else { return }
        let esp = old.contains(where: \.needsESP) ? try? mountESP(log) : nil
        let executor = RollbackExecutor(fs: fs, runner: env.runner, user: env.user)
        for r in old where !r.needsESP || esp != nil {
            executor.discard(r, esp: esp)
            log("forgot the kept copies of \(r.unit) (\(r.toTag))")
        }
        s.rollbacks.removeAll { r in !keep.contains(r.toTag) && (!r.needsESP || esp != nil) }
    }

    public func rollback(output: String?) -> Int32 {
        let log = LogSink(path: output)
        do {
            return try withScratch { scratch in
                var s = try store.readSystem()
                try recoverInterrupted(&s, scratch: scratch, output: output, log: log)
                guard let rec = s.rollbacks.last else { throw RollbackError.nothingToRollBack }
                let b = boot()
                if case .restartFirst(let why) = RestartGuard.checkRollback(rec, system: s, boot: b) {
                    throw EngineError.blocked(why)
                }
                let esp = rec.needsESP ? try mountESP(log) : nil
                s.record(
                    ChangeRecord(
                        item: "rollback:\(rec.unit)", action: "rollback", bootSession: b, date: env.clock.now,
                        driverChange: true, needsRestart: true))
                try store.saveSystem(s)
                try RollbackExecutor(fs: fs, runner: env.runner, user: env.user, scratch: scratch).undo(
                    rec, esp: esp, output: output
                ) { log($0) }
                s.rollbacks.removeLast()
                if !s.rollbacks.contains(where: { $0.toTag == rec.toTag }), s.installed?.tag == rec.toTag {
                    s.installed = s.previous?.tag == rec.fromTag ? s.previous : rec.fromTag.flatMap(installedRelease)
                    s.previous = nil
                }
                if s.pending?.tag == rec.toTag {
                    s.pending?.applied.removeAll { $0 == rec.unit }
                    if s.pending?.applied.isEmpty == true { s.pending = nil }
                }
                try store.saveSystem(s)
                log("rolled back \(rec.unit) (\(rec.toTag)). Restart.")
                return 0
            }
        } catch {
            log("STOP: \(error)")
            return 1
        }
    }

    /// An installed-release record for a release still in the releases folder.
    func installedRelease(_ tag: String) -> InstalledRelease? {
        let dir = releaseDir(tag)
        guard let raw = try? fs.read((dir as NSString).appendingPathComponent(ManifestVerifier.manifestFile)),
            let m = try? Manifest.decode(raw), let rel = m.release
        else { return nil }
        return InstalledRelease(
            tag: rel.tag, version: rel.version, manifestSha256: Hashing.sha256(raw), releaseDir: dir, date: env.clock.now,
            preRelease: rel.preRelease ?? true)
    }

    // MARK: GPU apps

    public func gpuApps(_ verb: String, app: String, releaseDir: String, output: String?) -> Int32 {
        let log = LogSink(path: output)
        do {
            guard ["add", "remove", "deny"].contains(verb) else { throw EngineError.failed("gpu-apps \(verb)?") }
            let (m, _) = try loadChecked(releaseDir)
            return try withScratch { scratch in
                let script = try ScriptVerifier.verifiedCopy(
                    of: GPUApps.script, scriptsDir: env.resourcesDir, manifest: m, fs: fs, scratch: scratch)
                let base = Installer(runner: env.runner, fs: fs, scriptsDir: env.resourcesDir).baseEnvironment(env.user)
                return env.runner.stream(GPUApps.command(verb, app: app, scriptPath: script), env: base, cwd: "/", output: output)
            }
        } catch {
            log("STOP: \(error)")
            return 1
        }
    }

    // MARK: third-party kexts

    public func thirdPartyApply(product: String, zip: String, sha: String, bundles: [String], output: String?) -> Int32 {
        let log = LogSink(path: output)
        do {
            guard ManifestCheck.isPathComponent(product), bundles.allSatisfy(ManifestCheck.isPathComponent) else {
                throw EngineError.failed("thirdparty-apply: bad product or bundle name")
            }
            let data = try fs.read(zip)
            guard Hashing.sha256(data) == sha else { throw VerifyError.fileMismatch(zip, "Dortania build index") }
            return try withScratch { scratch in
                var s = try store.readSystem()
                try recoverInterrupted(&s, scratch: scratch, output: output, log: log)
                let b = boot()
                if case .restartFirst(let why) = RestartGuard.checkDriverChange(system: s, boot: b) {
                    throw EngineError.blocked(why)
                }
                // Only bytes that were hashed here, unpacked where only root can reach them.
                let localZip = (scratch.dir as NSString).appendingPathComponent("third-party.zip")
                try fs.write(data, to: localZip)
                let x = (scratch.dir as NSString).appendingPathComponent("x")
                try env.unzipper.extract(localZip, into: x)
                let mp = try mountESP(log)
                let unit = "third-party:\(product)"
                s.record(
                    ChangeRecord(
                        item: unit, action: "update", bootSession: b, date: env.clock.now, driverChange: true, needsRestart: true)
                )
                var record = RollbackRecord(unit: unit, fromTag: nil, toTag: product, date: env.clock.now)
                s.interrupted = record
                try store.saveSystem(s)
                do {
                    try replaceBundles(bundles, in: x, esp: mp, &record, state: s, log: log)
                } catch {
                    do {
                        try RollbackExecutor(fs: fs, runner: env.runner, user: env.user, scratch: scratch).undo(
                            record, esp: mp, output: output
                        ) { log($0) }
                    } catch let undo {
                        throw ApplyError.undoFailed("\(error)", "\(undo)")
                    }
                    s.interrupted = nil
                    try? store.saveSystem(s)
                    throw error
                }
                s.rollbacks.append(record)
                s.interrupted = nil
                try store.saveSystem(s)
                log("Restart to load the new \(product).")
                return 0
            }
        } catch {
            log("STOP: \(error)")
            return 1
        }
    }

    func replaceBundles(
        _ bundles: [String], in x: String, esp mp: String, _ record: inout RollbackRecord, state: SystemState, log: LogSink
    ) throws {
        let applier = self.applier
        var s = state
        for b in bundles {
            let rel = EFIManager.kextPath(b)
            guard fs.exists((mp as NSString).appendingPathComponent(rel)), let src = DortaniaIndex.locate(b, in: x, fs: fs) else {
                log("\(b): not in this EFI or not in the zip: skipped")
                continue
            }
            let links = try Hashing.tree(src, fs: fs).symlinks
            if let link = links.first { throw VerifyError.symlink(b, link) }
            let kept = try applier.swapOntoESP(src, rel: rel, mp: mp, &record) { journalled in
                s.interrupted = journalled
                try store.saveSystem(s)
            }
            log("\(rel): updated (previous kept as \(kept.map { ($0 as NSString).lastPathComponent } ?? "nothing"))")
        }
    }
}
