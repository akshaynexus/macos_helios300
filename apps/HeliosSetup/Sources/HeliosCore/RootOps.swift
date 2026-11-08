// SPDX-License-Identifier: GPL-2.0-only
//
// RootOps: the root half (`helios-setup _root --user NAME --home HOME <command>`). It re-checks every rule the
// user side checked (release + scripts verified, one driver change per restart), records the change in the system
// state BEFORE making it, then makes it. Commands:
//   step <id> <action> --release DIR [--efi-source DIR]   a checklist action (scripts, sudo commands, efi ...)
//   update-apply --staged DIR --sha SHA [--app PATH]       the next batch of an update
//   rollback                                               undo the last applied update unit
//   gpu-apps add|remove APP --release DIR                  edit the GTX 1060 allow list
//   thirdparty-apply --product P --zip Z --sha S --bundle B...
import Foundation

/// Lines for the user: appended to the log file the GUI tails, or printed.
public struct LogSink {
    let path: String?

    public init(path: String?) { self.path = path }

    public func callAsFunction(_ line: String) {
        let data = Data((line + "\n").utf8)
        guard let path else {
            FileHandle.standardOutput.write(data)
            return
        }
        if !FileManager.default.fileExists(atPath: path) { _ = FileManager.default.createFile(atPath: path, contents: nil) }
        if let h = FileHandle(forWritingAtPath: path) {
            h.seekToEndOfFile()
            h.write(data)
            h.closeFile()
        }
    }
}

public struct RootOps {
    let env: Environment

    public init(env: Environment) { self.env = env }

    var fs: FileSystem { env.fs }
    var store: StateStore { env.store }
    var efi: EFIManager { EFIManager(runner: env.runner, fs: env.fs, clock: env.clock) }

    func boot() -> String { ProbeContext(runner: env.runner, fs: env.fs).bootSession }

    func probes() -> ProbeContext { Engine(env: env).probes() }

    // MARK: step

    public func step(_ id: String, _ action: String, releaseDir: String, efiSource: String?, output: String?) -> Int32 {
        let log = LogSink(path: output)
        do {
            let (m, sha) = try loadChecked(releaseDir)
            guard let item = StepPlanner.items(m).first(where: { $0.id == id }) else { throw EngineError.unknownItem(id) }
            guard let line = item.command(action) else { throw InstallerError.noCommand(id, action) }
            log("== \(item.title): \(action) (\(line))")
            if RestartGuard.changeActions.contains(action) {
                try recordChange(item.id, action, driver: item.driverChange, restart: item.reboot)
                adoptIfNone(m, sha: sha, dir: releaseDir)
            }
            if case .cli(let args) = try CommandParser.parse(line) {
                return try cli(args, item: item, releaseDir: releaseDir, efiSource: efiSource, log: log)
            }
            let installer = Installer(runner: env.runner, fs: fs, scriptsDir: env.resourcesDir)
            guard let plan = try installer.plan(item, action: action, manifest: m, releaseDir: releaseDir, user: env.user)
            else { return 0 }
            let rc = installer.execute(plan, output: output)
            log("== exit \(rc)\(rc == 0 && item.reboot && action != "check" && action != "status" ? ": restart to finish" : "")")
            return rc
        } catch {
            log("STOP: \(error)")
            return 1
        }
    }

    /// Re-reads the release and checks it (the user side did too; this side trusts nothing it was handed).
    func loadChecked(_ dir: String) throws -> (Manifest, String) {
        let raw = try fs.read((dir as NSString).appendingPathComponent("manifest.json"))
        let m = try Manifest.decode(raw)
        try ManifestCheck.check(m, local: true)
        return (m, Hashing.sha256(raw))
    }

    func recordChange(_ item: String, _ action: String, driver: Bool, restart: Bool) throws {
        var s = store.loadSystem()
        let b = boot()
        if driver, case .restartFirst(let why) = RestartGuard.checkDriverChange(system: s, boot: b) {
            throw EngineError.blocked(why)
        }
        s.record(
            ChangeRecord(
                item: item, action: action, bootSession: b, date: env.clock.now, driverChange: driver,
                needsRestart: restart))
        try store.saveSystem(s)
    }

    /// The first change made from a release makes it the installed one (the baseline for updates).
    func adoptIfNone(_ m: Manifest, sha: String, dir: String) {
        var s = store.loadSystem()
        guard s.installed == nil, let rel = m.release, rel.dryRun != true else { return }
        let dest = (env.releasesDir as NSString).appendingPathComponent(rel.tag)
        if !fs.exists(dest) { try? fs.copy(dir, to: dest) }
        s.installed = InstalledRelease(
            tag: rel.tag, version: rel.version, manifestSha256: sha,
            releaseDir: fs.exists(dest) ? dest : dir, date: env.clock.now,
            preRelease: rel.preRelease ?? true)
        try? store.saveSystem(s)
    }

    // MARK: efi (the OpenCore step's `helios-setup efi ...` commands)

    func cli(_ args: [String], item: SetupItem, releaseDir: String, efiSource: String?, log: LogSink) throws -> Int32 {
        guard args.first == "efi", args.count >= 2 else {
            throw EngineError.failed("unknown command helios-setup \(args.joined(separator: " "))")
        }
        let source = efiSource ?? defaultEFISource(releaseDir)
        switch args[1] {
        case "status": return efiStatus(log: log)
        case "check": return try efiCheck(source: source, releaseDir: releaseDir, log: log)
        case "install": return try efiInstall(source: source, releaseDir: releaseDir, log: log)
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

    func efiCheck(source: String?, releaseDir: String, log: LogSink) throws -> Int32 {
        guard let source else { throw EFIError.notAnEFI("(no EFI folder chosen and none next to the release)") }
        let src = try efi.sourceEFI(source)
        log("source: \(src)")
        let esp = try efi.internalESP()
        let mp = try efi.mount(esp)
        log("internal EFI partition: \(esp) at \(mp); free \(fs.freeBytes(mp).map { "\($0 / 1_048_576) MiB" } ?? "?")")
        let had = fs.exists((mp as NSString).appendingPathComponent("EFI/OC/OpenCore.efi"))
        log(had ? "it has an OpenCore now: install backs it up to EFI-backup-<date> first" : "it has no OpenCore yet")
        try efi.checkSpace(source: src, existing: had ? (mp as NSString).appendingPathComponent("EFI") : nil, esp: mp)
        log("space: enough for the backup and the new copy")
        report(efi.validate(efi: src, candidates: EFIManager.ocvalidateCandidates(efi: src, releaseDir: releaseDir)), log)
        log("this boot came from: \(probes().bootSource)")
        log("Check done: nothing was changed")
        return 0
    }

    func efiInstall(source: String?, releaseDir: String, log: LogSink) throws -> Int32 {
        guard let source else { throw EFIError.notAnEFI("(no EFI folder chosen)") }
        let src = try efi.sourceEFI(source)
        if case .failed(let out) = efi.validate(
            efi: src, candidates: EFIManager.ocvalidateCandidates(efi: src, releaseDir: releaseDir))
        {
            throw ConfigError.validationFailed(out)
        }
        let mp = try efi.mount(try efi.internalESP())
        let backup = try efi.install(from: src, esp: mp) { log($0) }
        if let backup {
            var s = store.loadSystem()
            s.efiBackups.append(backup)
            try store.saveSystem(s)
            log("previous EFI kept as \(backup) (Undo puts it back)")
        }
        _ = env.runner.capture([Tool.diskutil, "unmount", mp])  // as OCLP's install.py does afterwards
        log("Installed. Restart; if the Mac still starts from the stick, see Help > Boot order.")
        return 0
    }

    func efiRestore(_ name: String?, log: LogSink) throws -> Int32 {
        let mp = try efi.mount(try efi.internalESP())
        guard let b = name ?? store.loadSystem().efiBackups.last ?? efi.backups(esp: mp).last else {
            throw EFIError.noBackup("(none)")
        }
        let kept = try efi.restore(backup: b, esp: mp) { log($0) }
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

    public func updateApply(staged: String, sha: String, appPath: String?, output: String?) -> Int32 {
        let log = LogSink(path: output)
        do {
            let raw = try fs.read((staged as NSString).appendingPathComponent("manifest.json"))
            guard Hashing.sha256(raw) == sha else { throw UpdateError.manifestChanged(staged) }
            let m = try Manifest.decode(raw)
            try ManifestCheck.check(m)
            guard let tag = m.release?.tag else { throw UpdateError.noRelease }
            let dest = (env.releasesDir as NSString).appendingPathComponent(tag)
            let updater = Updater(http: env.http, fs: fs, unzipper: env.unzipper)
            if (try? fs.read((dest as NSString).appendingPathComponent("manifest.json"))) != raw {
                try fs.remove(dest)
                try fs.copy(staged, to: dest)
            }
            try updater.verifyStaged(dest, manifest: m)
            log("release \(tag): every file verified in \(dest)")
            return try applyBatch(m, sha: sha, dir: dest, appPath: appPath, output: output, log: log)
        } catch {
            log("STOP: \(error)")
            return 1
        }
    }

    func applyBatch(_ m: Manifest, sha: String, dir: String, appPath: String?, output: String?, log: LogSink) throws -> Int32 {
        var s = store.loadSystem()
        let old = s.installed.flatMap { i -> (Manifest, String)? in
            let d = (env.releasesDir as NSString).appendingPathComponent(i.tag)
            guard let raw = try? fs.read((d as NSString).appendingPathComponent("manifest.json")),
                let om = try? Manifest.decode(raw)
            else { return nil }
            return (om, d)
        }
        let plan = UpdatePlanner.plan(old: old?.0, new: m, diff: ManifestDiff.between(old?.0, m))
        let tag = m.release?.tag ?? "?"
        if s.pending?.tag != tag { s.pending = PendingUpdate(tag: tag, manifestSha256: sha, dir: dir) }
        let applied = Set(s.pending?.applied ?? [])
        guard let batch = UpdatePlanner.nextBatch(plan, applied: applied) else {
            finish(&s, m, sha: sha, dir: dir)
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
            new: m, newDir: dir, old: old?.0, oldDir: old?.1, appPath: appPath, user: env.user,
            output: output, mountESP: { try efi.mount(try efi.internalESP()) })
        let applier = UpdateApplier(fs: fs, runner: env.runner, clock: env.clock, probes: probes())
        for unit in batch {
            s.record(
                ChangeRecord(
                    item: "update:\(unit.id)", action: "update \(tag)", bootSession: boot(), date: env.clock.now,
                    driverChange: unit.requiresRestart, needsRestart: unit.requiresRestart))
            try store.saveSystem(s)
            log("== \(unit.title)")
            let record = try applier.apply(unit, ctx) { log($0) }
            s.rollbacks.append(record)
            s.pending?.applied.append(unit.id)
            try store.saveSystem(s)
        }
        if UpdatePlanner.nextBatch(plan, applied: Set(s.pending?.applied ?? [])) == nil {
            finish(&s, m, sha: sha, dir: dir)
            log("\(tag) is fully installed\(batch.contains(where: \.requiresRestart) ? ": restart" : "")")
        } else {
            log("restart, then continue the update (one driver change per restart)")
        }
        try store.saveSystem(s)
        return 0
    }

    func finish(_ s: inout SystemState, _ m: Manifest, sha: String, dir: String) {
        guard let rel = m.release else { return }
        if s.installed?.tag != rel.tag { s.previous = s.installed }
        s.installed = InstalledRelease(
            tag: rel.tag, version: rel.version, manifestSha256: sha, releaseDir: dir,
            date: env.clock.now, preRelease: rel.preRelease ?? true)
        s.pending = nil
    }

    public func rollback(output: String?) -> Int32 {
        let log = LogSink(path: output)
        do {
            var s = store.loadSystem()
            guard let rec = s.rollbacks.last else { throw RollbackError.nothingToRollBack }
            let esp = rec.needsESP ? try efi.mount(try efi.internalESP()) : nil
            s.record(
                ChangeRecord(
                    item: "rollback:\(rec.unit)", action: "rollback", bootSession: boot(), date: env.clock.now,
                    driverChange: true, needsRestart: true))
            try store.saveSystem(s)
            try RollbackExecutor(fs: fs, runner: env.runner, user: env.user).undo(rec, esp: esp, output: output) { log($0) }
            s.rollbacks.removeLast()
            if s.installed?.tag == rec.toTag, let prev = s.previous {
                s.installed = prev
                s.previous = nil
            }
            if s.pending?.tag == rec.toTag { s.pending?.applied.removeAll { $0 == rec.unit } }
            try store.saveSystem(s)
            log("rolled back \(rec.unit) (\(rec.toTag)). Restart.")
            return 0
        } catch {
            log("STOP: \(error)")
            return 1
        }
    }

    // MARK: GPU apps

    public func gpuApps(_ verb: String, app: String, releaseDir: String, output: String?) -> Int32 {
        let log = LogSink(path: output)
        do {
            guard ["add", "remove", "deny"].contains(verb) else { throw EngineError.failed("gpu-apps \(verb)?") }
            let (m, _) = try loadChecked(releaseDir)
            let script = try ScriptVerifier.path(of: GPUApps.script, scriptsDir: env.resourcesDir, manifest: m, fs: fs)
            let base = Installer(runner: env.runner, fs: fs, scriptsDir: env.resourcesDir).baseEnvironment(env.user)
            return env.runner.stream(GPUApps.command(verb, app: app, scriptPath: script), env: base, cwd: "/", output: output)
        } catch {
            log("STOP: \(error)")
            return 1
        }
    }

    // MARK: third-party kexts

    public func thirdPartyApply(product: String, zip: String, sha: String, bundles: [String], output: String?) -> Int32 {
        let log = LogSink(path: output)
        do {
            let data = try fs.read(zip)
            guard Hashing.sha256(data) == sha else { throw VerifyError.fileMismatch(zip, "Dortania build index") }
            var s = store.loadSystem()
            if case .restartFirst(let why) = RestartGuard.checkDriverChange(system: s, boot: boot()) {
                throw EngineError.blocked(why)
            }
            let x = zip + ".x"
            try fs.remove(x)
            try env.unzipper.extract(zip, into: x)
            let mp = try efi.mount(try efi.internalESP())
            s.record(
                ChangeRecord(
                    item: "third-party:\(product)", action: "update", bootSession: boot(), date: env.clock.now,
                    driverChange: true, needsRestart: true))
            try store.saveSystem(s)
            var record = RollbackRecord(unit: "third-party:\(product)", fromTag: nil, toTag: product, date: env.clock.now)
            for b in bundles {
                let rel = "EFI/OC/Kexts/\(b)"
                let target = (mp as NSString).appendingPathComponent(rel)
                guard fs.exists(target), let src = DortaniaIndex.locate(b, in: x, fs: fs) else {
                    log("\(b): not in this EFI or not in the zip: skipped")
                    continue
                }
                try fs.remove(target + ".prev")
                try fs.move(target, to: target + ".prev")
                record.ops.append(.restore(rel, from: rel + ".prev", onESP: true))
                try efi.copyTree(src, target)
                try efi.verify(src, target, what: rel)
                log("\(rel): updated (previous kept as \(b).prev)")
            }
            s.rollbacks.append(record)
            try store.saveSystem(s)
            log("Restart to load the new \(product).")
            return 0
        } catch {
            log("STOP: \(error)")
            return 1
        }
    }
}
