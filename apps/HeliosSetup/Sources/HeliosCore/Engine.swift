// SPDX-License-Identifier: GPL-2.0-only
//
// Engine: the user-side half of the app, shared by the GUI and the `helios-setup` CLI. It reads (release,
// probes, state), decides (planner, guards) and, for anything that changes the machine, starts
// `helios-setup _root ...` through the PrivilegedRunner (RootOps.swift is that half). So both front ends enforce
// the same rules, and every root action can be driven and tested over SSH without the GUI.
//
//   GUI (AppKit) ─┐                      ┌─ RootOps (as root): step / efi / update-apply / rollback / gpu-apps
//                 ├─ Engine (user) ──────┤
//   CLI ──────────┘   probes, planner,   └─ PrivilegedRunner: osascript prompt | sudo | already root
//                     updater (download)
import Foundation

public struct Environment {
    public var runner: CommandRunner
    public var fs: FileSystem
    public var http: HTTPClient
    public var clock: Clock
    public var privileged: PrivilegedRunner
    public var unzipper: Unzipper
    public var user: UserContext
    /// Contents/Resources: the release scripts, spec.json, the app icon.
    public var resourcesDir: String
    /// Contents/MacOS/helios-setup.
    public var cliPath: String
    /// The .app bundle (nil for a CLI outside one).
    public var appPath: String?
    public var systemStatePath = StatePaths.system
    public var releasesDir = StatePaths.releases

    public init(
        runner: CommandRunner, fs: FileSystem, http: HTTPClient, clock: Clock, privileged: PrivilegedRunner,
        unzipper: Unzipper, user: UserContext, resourcesDir: String, cliPath: String, appPath: String?
    ) {
        self.runner = runner
        self.fs = fs
        self.http = http
        self.clock = clock
        self.privileged = privileged
        self.unzipper = unzipper
        self.user = user
        self.resourcesDir = resourcesDir
        self.cliPath = cliPath
        self.appPath = appPath
    }

    public var store: StateStore { StateStore(fs: fs, home: user.home, systemPath: systemStatePath) }
}

/// The release the app works from.
public struct LoadedRelease {
    public enum Source: String { case chosen, besideApp = "next to the app", installed, embeddedSpec = "built-in spec" }
    public var dir: String?
    public var manifest: Manifest
    public var sha256: String?
    public var source: Source
    public var problem: String?

    /// Steps may run only from a checked release folder.
    public var canAct: Bool { dir != nil && problem == nil }
}

public enum EngineError: Error, CustomStringConvertible, Equatable {
    case noRelease(String)
    case unknownItem(String)
    case blocked(String)
    case cancelled
    case failed(String)

    public var description: String {
        switch self {
        case .noRelease(let why): return "no release folder: \(why)"
        case .unknownItem(let id): return "no step '\(id)' in this release"
        case .blocked(let why): return why
        case .cancelled: return "the administrator prompt was cancelled; nothing ran"
        case .failed(let why): return why
        }
    }
}

public struct Overview {
    public var release: LoadedRelease
    public var items: [SetupItem]
    public var statuses: [String: ItemStatus]
    public var next: SetupItem?
    public var restartPending: String?
    public var bootSource: String
    public var build: String
    public var boot: String
    public var scripts: [(String, Bool)]
}

public struct UpdateOffer {
    public var candidate: ReleaseCandidate
    public var manifest: Manifest
    public var raw: Data
    public var diff: ManifestDiff
    public var plan: UpdatePlan
    public var changelog: String
}

public final class Engine {
    public let env: Environment

    public init(env: Environment) { self.env = env }

    var store: StateStore { env.store }

    public func probes() -> ProbeContext {
        let ctx = ProbeContext(runner: env.runner, fs: env.fs)
        let efi = EFIManager(runner: env.runner, fs: env.fs, clock: env.clock)
        if let esp = try? efi.internalESP() { ctx.internalESPUUID = efi.info(esp).partitionUUID }
        return ctx
    }

    // MARK: release

    /// The release folder: the one given, the user's choice, the folder holding the app (EFI/Helios300 on the
    /// stick), the installed one; else the built-in spec (read-only: nothing can run).
    public func loadRelease(explicit: String? = nil) -> LoadedRelease {
        var candidates: [(String, LoadedRelease.Source)] = []
        if let e = explicit { candidates.append((e, .chosen)) }
        if let c = store.loadUser().releaseDir { candidates.append((c, .chosen)) }
        if let app = env.appPath { candidates.append(((app as NSString).deletingLastPathComponent, .besideApp)) }
        if let i = store.loadSystem().installed { candidates.append((i.releaseDir, .installed)) }
        for (dir, source) in candidates {
            let path = (dir as NSString).appendingPathComponent("manifest.json")
            guard let raw = try? env.fs.read(path) else { continue }
            do {
                let m = try Manifest.decode(raw)
                try ManifestCheck.check(m, local: true)
                // A dry-run manifest only when the user chose that folder (1401's rule for a local test).
                let problem =
                    m.release?.dryRun == true && source != .chosen
                    ? "a dry-run manifest (release.sh --dry-run): choose its folder explicitly to test with it" : nil
                return LoadedRelease(dir: dir, manifest: m, sha256: Hashing.sha256(raw), source: source, problem: problem)
            } catch {
                return LoadedRelease(
                    dir: nil, manifest: embeddedSpec(), sha256: nil, source: .embeddedSpec,
                    problem: "\(path): \(error)")
            }
        }
        return LoadedRelease(
            dir: nil, manifest: embeddedSpec(), sha256: nil, source: .embeddedSpec,
            problem: "no manifest.json found: choose the release folder (EFI/Helios300 on the stick)")
    }

    public func embeddedSpec() -> Manifest {
        let path = (env.resourcesDir as NSString).appendingPathComponent("spec.json")
        if let data = try? env.fs.read(path), let m = try? Manifest.decode(data) { return m }
        return Manifest(
            schema: 1, repo: ManifestCheck.repo, macos: MacOSRange(minKernel: "", maxKernel: ""),
            artifacts: [], postInstall: [])
    }

    // MARK: overview

    public func overview(explicit: String? = nil) -> Overview {
        let rel = loadRelease(explicit: explicit)
        let ctx = probes()
        let items = StepPlanner.items(rel.manifest)
        let system = store.loadSystem()
        let statuses = StepPlanner.statuses(items, ctx: ctx, system: system, user: store.loadUser(), boot: ctx.bootSession)
        let restart = system.changes(inBoot: ctx.bootSession).last(where: \.needsRestart)
            .map { "\($0.item) (\($0.action)) waits for a restart" }
        let scripts =
            rel.dir == nil ? [] : ScriptVerifier.report(scriptsDir: env.resourcesDir, manifest: rel.manifest, fs: env.fs)
        return Overview(
            release: rel, items: items, statuses: statuses, next: StepPlanner.next(items, statuses: statuses),
            restartPending: restart, bootSource: ctx.bootSource, build: ctx.build, boot: ctx.bootSession,
            scripts: scripts)
    }

    // MARK: steps

    /// Runs an item's action. Changes go through the administrator prompt; Check must pass before Install.
    @discardableResult
    public func perform(_ id: String, _ action: String, explicit: String? = nil, output: String? = nil) throws -> Int32 {
        let rel = loadRelease(explicit: explicit)
        guard rel.canAct, let dir = rel.dir, let sha = rel.sha256 else {
            throw EngineError.noRelease(rel.problem ?? "choose the release folder")
        }
        guard let item = StepPlanner.items(rel.manifest).first(where: { $0.id == id }) else { throw EngineError.unknownItem(id) }
        let ctx = probes()
        let boot = ctx.bootSession
        var user = store.loadUser()
        if case .restartFirst(let why) = RestartGuard.check(item, action: action, system: store.loadSystem(), boot: boot) {
            throw EngineError.blocked(why)
        }
        if action == "install",
            case .checkFirst(let why) = DryRunRule.check(
                item, user: user, boot: boot,
                manifestSha256: sha, now: env.clock.now)
        {
            throw EngineError.blocked(why)
        }
        var argv = [env.cliPath, "_root", "--user", env.user.name, "--home", env.user.home, "step", id, action, "--release", dir]
        if let src = user.efiSource { argv += ["--efi-source", src] }
        let rc: Int32
        if Installer.needsRoot(item, action: action) {
            rc = env.privileged.runAsRoot(argv, prompt: prompt(action, item.title), output: output)
            if rc == privilegedCancelled { throw EngineError.cancelled }
        } else {
            rc = RootOps(env: env).step(id, action, releaseDir: dir, efiSource: user.efiSource, output: output)
        }
        user = store.loadUser()
        let now = env.clock.now
        user.runs[id] = RunRecord(action: action, status: rc, bootSession: boot, date: now, log: output ?? "terminal")
        if action == "check" { user.checks[id] = CheckRecord(bootSession: boot, date: now, status: rc, manifestSha256: sha) }
        try? store.saveUser(user)
        return rc
    }

    func prompt(_ action: String, _ title: String) -> String {
        let verb = ["check": "check (dry run, changes nothing)", "install": "install", "revert": "undo"][action] ?? action
        return "Helios 300 Setup needs an administrator password to \(verb): \(title)."
    }

    public func setReleaseDir(_ dir: String?) {
        var u = store.loadUser()
        u.releaseDir = dir
        try? store.saveUser(u)
    }

    public func setEFISource(_ dir: String?) {
        var u = store.loadUser()
        u.efiSource = dir
        try? store.saveUser(u)
    }

    // MARK: updates

    var updater: Updater { Updater(http: env.http, fs: env.fs, unzipper: env.unzipper) }

    /// The installed release's manifest (the baseline for a diff), else the one the app works from.
    public func installedManifest() -> (Manifest?, String?) {
        if let i = store.loadSystem().installed {
            let path = ((env.releasesDir as NSString).appendingPathComponent(i.tag) as NSString).appendingPathComponent(
                "manifest.json")
            if let d = try? env.fs.read(path), let m = try? Manifest.decode(d) { return (m, i.tag) }
        }
        let rel = loadRelease()
        return rel.dir == nil ? (nil, nil) : (rel.manifest, rel.manifest.release?.tag)
    }

    /// Looks for a newer release; nil when there is none. Pre-releases follow the installed release's channel
    /// unless the user chose (every release so far is a pre-release).
    public func checkForUpdates(includePrereleases: Bool? = nil) throws -> UpdateOffer? {
        let (old, tag) = installedManifest()
        let pre = includePrereleases ?? store.loadUser().includePrereleases ?? (old?.release?.preRelease ?? true)
        guard let c = ReleaseList.newer(try updater.releases(), than: tag, includePrereleases: pre) else { return nil }
        let (m, raw) = try updater.manifest(tag: c.tag)
        let diff = ManifestDiff.between(old, m)
        return UpdateOffer(
            candidate: c, manifest: m, raw: raw, diff: diff, plan: UpdatePlanner.plan(old: old, new: m, diff: diff),
            changelog: diff.changelog(notes: c.notes, from: tag, to: c.tag))
    }

    public func downloadsDir(_ tag: String) -> String {
        ((StatePaths.userDir(home: env.user.home) as NSString).appendingPathComponent("downloads") as NSString)
            .appendingPathComponent(tag)
    }

    /// Downloads and verifies the whole release (unchanged files are copied from the installed one).
    public func download(_ offer: UpdateOffer) throws -> StagedRelease {
        let prev = store.loadSystem().installed?.releaseDir ?? loadRelease().dir
        return try updater.stage(offer.manifest, raw: offer.raw, into: downloadsDir(offer.candidate.tag), previousDir: prev)
    }

    /// Applies the next batch of a staged update as root (one driver unit per restart).
    @discardableResult
    public func applyNextUpdate(_ staged: StagedRelease, output: String? = nil) throws -> Int32 {
        var argv = [
            env.cliPath, "_root", "--user", env.user.name, "--home", env.user.home, "update-apply",
            "--staged", staged.dir, "--sha", staged.manifestSha256,
        ]
        if let app = env.appPath { argv += ["--app", app] }
        let rc = env.privileged.runAsRoot(
            argv,
            prompt: "Helios 300 Setup needs an administrator password to apply "
                + "macos_helios300 \(staged.manifest.release?.tag ?? "")", output: output)
        if rc == privilegedCancelled { throw EngineError.cancelled }
        return rc
    }

    @discardableResult
    public func rollbackLast(output: String? = nil) throws -> Int32 {
        let argv = [env.cliPath, "_root", "--user", env.user.name, "--home", env.user.home, "rollback"]
        let rc = env.privileged.runAsRoot(
            argv,
            prompt: "Helios 300 Setup needs an administrator password to roll back "
                + "the last update", output: output)
        if rc == privilegedCancelled { throw EngineError.cancelled }
        return rc
    }

    /// Optional: newer third-party kexts from Dortania's build repo (only when the user turned it on).
    public func thirdPartyUpdates(_ m: Manifest) throws -> [ThirdPartyUpdate] {
        guard let tp = m.thirdPartyKexts, let url = URL(string: tp.source) else { return [] }
        let index = DortaniaIndex.parse(try env.http.get(url, headers: [:]))
        return DortaniaIndex.updates(index: index, products: tp.products, loaded: probes().loadedKexts)
    }

    /// Downloads one third-party zip (sha256 checked) and installs it on the ESP as root.
    @discardableResult
    public func applyThirdParty(_ u: ThirdPartyUpdate, output: String? = nil) throws -> Int32 {
        guard let url = URL(string: u.url) else { throw EngineError.failed("bad url \(u.url)") }
        let data = try env.http.get(url, headers: [:])
        guard Hashing.sha256(data) == u.sha256 else {
            throw EngineError.failed("\(u.url) does not match the sha256 in Dortania's build index")
        }
        let zip = (downloadsDir("third-party") as NSString).appendingPathComponent("\(u.product)-\(u.available).zip")
        try env.fs.write(data, to: zip)
        let argv =
            [
                env.cliPath, "_root", "--user", env.user.name, "--home", env.user.home, "thirdparty-apply",
                "--product", u.product, "--zip", zip, "--sha", u.sha256,
            ] + u.bundles.flatMap { ["--bundle", $0.bundle] }
        let rc = env.privileged.runAsRoot(
            argv,
            prompt: "Helios 300 Setup needs an administrator password to update "
                + "\(u.product) on the EFI partition", output: output)
        if rc == privilegedCancelled { throw EngineError.cancelled }
        return rc
    }

    // MARK: GPU apps

    public func gpuApps() -> (apps: [(AppEntry, GPUApps.Decision)], listPresent: Bool) {
        let text = (try? env.fs.read(GPUApps.allowList)).map { String(decoding: $0, as: UTF8.self) }
        let rules = GPUApps.rules(text ?? "")
        let dirs = ["/Applications", (env.user.home as NSString).appendingPathComponent("Applications")]
        let apps = GPUApps.installedApps(in: dirs, fs: env.fs).map { ($0, GPUApps.decide($0.executable, rules: rules)) }
        return (apps, text != nil)
    }

    /// gpu-apps.sh add / remove as root.
    @discardableResult
    public func setGPU(_ app: AppEntry, use: Bool, output: String? = nil) throws -> Int32 {
        let rel = loadRelease()
        guard let dir = rel.dir else { throw EngineError.noRelease(rel.problem ?? "") }
        let argv = [
            env.cliPath, "_root", "--user", env.user.name, "--home", env.user.home, "gpu-apps",
            use ? "add" : "remove", app.path, "--release", dir,
        ]
        let rc = env.privileged.runAsRoot(
            argv,
            prompt: "Helios 300 Setup needs an administrator password to "
                + "\(use ? "give" : "take") the GTX 1060 \(use ? "to" : "from") \(app.name)", output: output)
        if rc == privilegedCancelled { throw EngineError.cancelled }
        return rc
    }

    /// gpu-apps.sh run as the user: one launch on the GTX 1060 without changing the list.
    public func runOnGPU(_ app: AppEntry, withLog: Bool, output: String?) throws -> Int32 {
        let rel = loadRelease()
        let script = try ScriptVerifier.path(of: GPUApps.script, scriptsDir: env.resourcesDir, manifest: rel.manifest, fs: env.fs)
        let env0 = Installer(runner: env.runner, fs: env.fs, scriptsDir: env.resourcesDir).baseEnvironment(env.user)
        return env.runner.stream(
            [Tool.bash, script, "run"] + (withLog ? ["--log"] : []) + [app.path], env: env0,
            cwd: env.user.home, output: output)
    }

    // MARK: diagnostics

    public func diagnostics(appVersion: String) -> String {
        let o = overview()
        let ctx = probes()
        return Health.diagnostics(
            o.release.manifest, rows: Health.rows(o.release.manifest, ctx: ctx),
            statuses: o.items.compactMap { i in o.statuses[i.id].map { (i, $0) } },
            ctx: ctx, appVersion: appVersion)
    }
}
