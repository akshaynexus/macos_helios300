// SPDX-License-Identifier: GPL-2.0-only
//
// helios-setup: the headless CLI over HeliosCore (the GUI is a front end over the same Engine). Run it from
// Terminal or over SSH; commands that change the machine ask for the password through sudo (or run directly
// when already root, e.g. under asroot).
import Foundation
import HeliosCore

let usage = """
    usage: helios-setup <command> [options]
      status [--json] [--show-ssid]        checklist + health (read-only)
      plan                                 what to do next, and whether a restart comes first
      check <step>                         the step's dry run (changes nothing)
      install <step>                       install (needs a passing check in this boot)
      undo <step>                          revert
      efi status|check|install|restore     OpenCore on the internal disk ([--source EFI-folder])
      update --check [--prereleases|--no-prereleases]
      update --apply                       download + verify, then apply the next batch (one driver change per restart)
      rollback                             undo the last applied update unit
      gpu-apps list | add APP | remove APP | run [--log] APP
      third-party --check | --apply PRODUCT   Dortania build-repo kexts (optional)
      diagnostics                          sanitized report for the developer
      release DIR                          remember the release folder (manifest.json + payloads)
    options: --release DIR (the release folder for this run)
    """

struct Args {
    var words: [String] = []
    var options: [String: String] = [:]
    var flags: Set<String> = []
    var multi: [String: [String]] = [:]

    static let valued: Set<String> = [
        "--release", "--user", "--home", "--source", "--efi-source", "--staged", "--sha",
        "--app", "--product", "--zip", "--bundle", "--output", "--resources", "--baseline-sha", "--baseline-dir",
    ]

    init(_ argv: [String]) {
        var i = 0
        while i < argv.count {
            let a = argv[i]
            if Args.valued.contains(a), i + 1 < argv.count {
                options[a] = argv[i + 1]
                multi[a, default: []].append(argv[i + 1])
                i += 2
            } else if a.hasPrefix("--") {
                flags.insert(a)
                i += 1
            } else {
                words.append(a)
                i += 1
            }
        }
    }

    func word(_ n: Int) -> String? { n < words.count ? words[n] : nil }
}

func fail(_ message: String, _ code: Int32 = 1) -> Never {
    FileHandle.standardError.write(Data("helios-setup: \(message)\n".utf8))
    exit(code)
}

func printStatus(_ engine: Engine, json: Bool, showSSID: Bool, release: String?) {
    let o = engine.overview(explicit: release)
    let rows = Health.rows(o.release.manifest, ctx: engine.probes())
    if json {
        let steps = o.items.map { i -> [String: Any] in
            let s = o.statuses[i.id] ?? .notInstalled
            return ["id": i.id, "title": i.title, "status": s.label, "detail": s.detail]
        }
        let health = rows.map { r -> [String: Any] in
            [
                "id": r.id, "ok": r.ok.map { $0 as Any } ?? NSNull(),
                "values": Dictionary(
                    r.lines.map { ($0.label, $0.result.isPrivate && !showSSID ? "<hidden>" : $0.result.value) },
                    uniquingKeysWith: { a, _ in a }),
            ]
        }
        let doc: [String: Any] = [
            "release": o.release.manifest.release?.tag ?? "none", "release_dir": o.release.dir ?? "",
            "build": o.build, "boot_source": o.bootSource, "steps": steps, "health": health,
            "restart_pending": o.restartPending ?? "",
        ]
        if let data = try? JSONSerialization.data(withJSONObject: doc, options: [.prettyPrinted, .sortedKeys]) {
            FileHandle.standardOutput.write(data + Data("\n".utf8))
        }
        return
    }
    print(
        "release: \(o.release.manifest.release?.tag ?? "none") (\(o.release.source.rawValue)\(o.release.dir.map { ", \($0)" } ?? ""))"
    )
    if let p = o.release.problem { print("  ! \(p)") }
    for (name, ok) in o.scripts where !ok { print("  ! \(name) in this app is missing or is not the release's copy (sha256)") }
    if let p = o.stateProblem { print("  ! \(p)") }
    print("macOS \(o.build); this boot came from: \(o.bootSource)")
    if let r = o.restartPending { print("restart pending: \(r)") }
    print("\nSetup:")
    for i in o.items {
        let s = o.statuses[i.id] ?? .notInstalled
        print(
            "  \(i.id.padding(toLength: 18, withPad: " ", startingAt: 0)) \(s.label.padding(toLength: 14, withPad: " ", startingAt: 0)) \(i.title)"
        )
        if !s.detail.isEmpty { print("      \(s.detail)") }
    }
    print("\nStatus:")
    for r in rows {
        print("  [\(r.ok.map { $0 ? " ok " : "FAIL" } ?? "    ")] \(r.title)")
        for l in r.lines {
            print("         \(l.label): \(l.result.isPrivate && !showSSID ? "<hidden: --show-ssid>" : l.result.value)")
        }
    }
}

func printPlan(_ engine: Engine, release: String?) {
    let o = engine.overview(explicit: release)
    if let r = o.restartPending { print("Restart first: \(r)."); return }
    guard let n = o.next else { print("Nothing to do: every step is installed or not available here."); return }
    print("Next: \(n.id) - \(n.title)")
    print("  1. helios-setup check \(n.id)")
    print("  2. helios-setup install \(n.id)\(n.reboot ? "\n  3. restart" : "")")
}

func step(_ engine: Engine, _ id: String?, _ action: String, release: String?) -> Int32 {
    guard let id else { fail("which step? (helios-setup status lists them)") }
    do { return try engine.perform(id, action, explicit: release) } catch { fail("\(error)") }
}

func update(_ engine: Engine, _ a: Args) -> Int32 {
    let pre: Bool? = a.flags.contains("--prereleases") ? true : a.flags.contains("--no-prereleases") ? false : nil
    do {
        guard let offer = try engine.checkForUpdates(includePrereleases: pre) else {
            print("No newer macos_helios300 release.")
            return 0
        }
        print(offer.changelog)
        print("\nPlan (\(offer.plan.batches.count) step(s), a restart between driver changes):")
        for (n, b) in offer.plan.batches.enumerated() {
            print(
                "  \(n + 1). \(b.map(\.title).joined(separator: ", "))\(b.contains(where: \.requiresRestart) ? "  [restart]" : "")"
            )
        }
        guard a.flags.contains("--apply") else { return 0 }
        let staged = try engine.download(offer)
        print("\nDownloaded and verified: \(staged.dir)")
        return try engine.applyNextUpdate(staged, baseline: offer.baseline)
    } catch {
        fail("\(error)")
    }
}

func gpuApps(_ engine: Engine, _ a: Args) -> Int32 {
    let (apps, present) = engine.gpuApps()
    switch a.word(1) ?? "list" {
    case "list":
        if !present { print("No \(GPUApps.allowList) yet (the GPU step's Metal userspace part installs it).") }
        for (app, d) in apps {
            print("  [\(d == .allowed ? "x" : " ")] \(app.name) (\(app.executable))\(d == .denied ? " denied" : "")")
        }
        return 0
    case "add", "remove", "run":
        guard let name = a.word(2),
            let app = apps.map(\.0).first(where: { $0.name == name || $0.path == name || $0.executable == name })
        else { fail("no app '\(a.word(2) ?? "")' in /Applications or ~/Applications") }
        do {
            if a.word(1) == "run" { return try engine.runOnGPU(app, withLog: a.flags.contains("--log"), output: nil) }
            return try engine.setGPU(app, use: a.word(1) == "add")
        } catch { fail("\(error)") }
    default:
        fail("gpu-apps list | add APP | remove APP | run [--log] APP")
    }
}

func thirdParty(_ engine: Engine, _ a: Args) -> Int32 {
    do {
        let ups = try engine.thirdPartyUpdates(engine.loadRelease().manifest)
        if ups.isEmpty { print("Every third-party kext from Dortania's build repo is current (or not loaded).") }
        for u in ups { print("  \(u.product): \(u.installed ?? "?") -> \(u.available)") }
        if let p = a.options["--product"] ?? (a.flags.contains("--apply") ? a.word(1) : nil) {
            guard let u = ups.first(where: { $0.product == p }) else { fail("no update for \(p)") }
            return try engine.applyThirdParty(u)
        }
        return 0
    } catch { fail("\(error)") }
}

/// `_root`: the half that runs as root (started by Engine through sudo / osascript / directly).
func root(_ a: Args) -> Int32 {
    guard geteuid() == 0 else { fail("_root must run as root", 77) }
    // `do shell script … with administrator privileges` can leave the real UID the user's; scripts and sudo must see root.
    guard setuid(0) == 0 else { fail("_root could not become root fully (setuid)", 77) }
    // Held until this process exits.
    guard RootLock.acquire() != nil else { fail("another Helios 300 Setup job is running; try again when it has finished", 75) }
    guard let name = a.options["--user"], let home = a.options["--home"] else { fail("_root needs --user and --home") }
    let env = Environment.live(
        gui: false, executable: CommandLine.arguments[0], user: UserContext(name: name, home: home),
        resources: a.options["--resources"])
    let ops = RootOps(env: env)
    let out = a.options["--output"]
    switch a.word(1) {
    case "step":
        guard let id = a.word(2), let action = a.word(3), let rel = a.options["--release"] else {
            fail("_root step ID ACTION --release DIR")
        }
        return ops.step(id, action, releaseDir: rel, efiSource: a.options["--efi-source"], output: out)
    case "update-apply":
        guard let staged = a.options["--staged"], let sha = a.options["--sha"], let baseline = a.options["--baseline-sha"] else {
            fail("_root update-apply --staged DIR --sha SHA --baseline-sha SHA|none")
        }
        return ops.updateApply(
            staged: staged, sha: sha, baselineSha: baseline, baselineDir: a.options["--baseline-dir"],
            appPath: a.options["--app"],
            output: out)
    case "rollback":
        return ops.rollback(output: out)
    case "gpu-apps":
        guard let verb = a.word(2), let app = a.word(3), let rel = a.options["--release"] else {
            fail("_root gpu-apps VERB APP --release DIR")
        }
        return ops.gpuApps(verb, app: app, releaseDir: rel, output: out)
    case "thirdparty-apply":
        guard let p = a.options["--product"], let z = a.options["--zip"], let s = a.options["--sha"] else {
            fail("_root thirdparty-apply ...")
        }
        return ops.thirdPartyApply(product: p, zip: z, sha: s, bundles: a.multi["--bundle"] ?? [], output: out)
    default:
        fail("unknown _root command")
    }
}

let args = Args(Array(CommandLine.arguments.dropFirst()))
if args.word(0) == "_root" { exit(root(args)) }
let engine = Engine(env: .live(gui: false, executable: CommandLine.arguments[0]))
let release = args.options["--release"]

switch args.word(0) {
case "status":
    printStatus(engine, json: args.flags.contains("--json"), showSSID: args.flags.contains("--show-ssid"), release: release)
case "plan": printPlan(engine, release: release)
case "check": exit(step(engine, args.word(1), "check", release: release))
case "install": exit(step(engine, args.word(1), "install", release: release))
case "undo", "revert": exit(step(engine, args.word(1), "revert", release: release))
case "efi":
    if let src = args.options["--source"] { engine.setEFISource(src) }
    let action = ["status": "status", "check": "check", "install": "install", "restore": "revert"][args.word(1) ?? "status"]
    guard let action else { fail("efi status|check|install|restore") }
    guard
        let item = engine.overview(explicit: release).items.first(where: {
            ($0.command(action) ?? "").hasPrefix("helios-setup efi")
        })
    else { fail("this release has no OpenCore step") }
    exit(step(engine, item.id, action, release: release))
case "update": exit(update(engine, args))
case "rollback":
    do { exit(try engine.rollbackLast()) } catch { fail("\(error)") }
case "gpu-apps": exit(gpuApps(engine, args))
case "third-party": exit(thirdParty(engine, args))
case "diagnostics":
    print(engine.diagnostics(appVersion: Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "dev"))
case "release":
    guard let dir = args.word(1) else { fail("release DIR") }
    engine.setReleaseDir(dir)
    print("release folder: \(dir)")
case nil, "help", "-h", "--help": print(usage)
default: fail("unknown command \(args.word(0) ?? "")\n\(usage)", 2)
}
