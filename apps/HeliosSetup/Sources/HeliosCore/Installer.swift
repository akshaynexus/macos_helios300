// SPDX-License-Identifier: GPL-2.0-only
//
// Installer: runs a step's command exactly as the manifest spells it (post_install.run), so the app and a person
// typing it in Terminal do the same thing. The command grammar is the one the spec uses:
//     [NAME=value ...] bash <script> [args]      a release script (tools/macos), from the app's verified copy
//     [NAME=value ...] sudo <program> [args]      a root command
//     helios-setup <args>                         this app's own CLI (OpenCore install, ...)
//     <program> [args]                            a plain command
// Scripts run with stdin from /dev/null, MODE/ONLY etc. in the environment (never piped), in the release folder
// (install-gpu.sh checks the payloads next to manifest.json). As root, because the scripts expect a user that can
// sudo; root can, without a password. HOME stays the user's (the scripts' caches are under ~/Library/Caches).
import Foundation

public enum ParsedCommand: Equatable {
    case script(name: String, args: [String], env: [String: String])
    case exec(argv: [String], root: Bool, env: [String: String])
    case cli(args: [String])
}

public enum CommandParseError: Error, CustomStringConvertible, Equatable {
    case empty
    case unbalancedQuote(String)

    public var description: String {
        switch self {
        case .empty: return "empty command"
        case .unbalancedQuote(let s): return "unbalanced quote in: \(s)"
        }
    }
}

public enum CommandParser {
    public static let cliName = "helios-setup"

    public static func parse(_ line: String) throws -> ParsedCommand {
        var words = try split(line)
        var env: [String: String] = [:]
        while let w = words.first, let eq = w.firstIndex(of: "="), isName(w[..<eq]) {
            env[String(w[..<eq])] = String(w[w.index(after: eq)...])
            words.removeFirst()
        }
        guard let first = words.first else { throw CommandParseError.empty }
        switch first {
        case "bash" where words.count >= 2:
            return .script(name: words[1], args: Array(words.dropFirst(2)), env: env)
        case "sudo" where words.count >= 2:
            return .exec(argv: Array(words.dropFirst()), root: true, env: env)
        case cliName:
            return .cli(args: Array(words.dropFirst()))
        default:
            return .exec(argv: words, root: false, env: env)
        }
    }

    /// Shell-style words: spaces separate, '...' and "..." group, backslash escapes inside "..." and bare words.
    public static func split(_ line: String) throws -> [String] {
        var words: [String] = []
        var cur = ""
        var inWord = false
        var quote: Character?
        var escape = false
        for c in line {
            if escape { cur.append(c); escape = false; continue }
            if let q = quote {
                if c == q { quote = nil } else if c == "\\" && q == "\"" { escape = true } else { cur.append(c) }
                continue
            }
            switch c {
            case " ", "\t":
                if inWord { words.append(cur); cur = ""; inWord = false }
            case "'", "\"":
                quote = c; inWord = true
            case "\\":
                escape = true; inWord = true
            default:
                cur.append(c); inWord = true
            }
        }
        if quote != nil || escape { throw CommandParseError.unbalancedQuote(line) }
        if inWord { words.append(cur) }
        return words
    }

    static func isName(_ s: Substring) -> Bool {
        !s.isEmpty && s.allSatisfy { $0 == "_" || $0.isLetter || $0.isNumber } && !(s.first?.isNumber ?? true)
    }
}

/// Who the work is for: the logged-in user, also when the CLI runs as root on their behalf.
public struct UserContext: Equatable {
    public var name: String
    public var home: String

    public init(name: String, home: String) {
        self.name = name
        self.home = home
    }
}

public enum InstallerError: Error, CustomStringConvertible, Equatable {
    case noCommand(String, String)
    case scriptMissing(String)
    case scriptMismatch(String, String)
    case scriptNotInRelease(String)
    case programNotFound(String)
    case needsRoot

    public var description: String {
        switch self {
        case .noCommand(let item, let action): return "\(item) has no \(action) command in this release"
        case .scriptMissing(let s): return "\(s) is not in this app (Contents/Resources)"
        case .scriptMismatch(let s, let v):
            return "\(s) in this app does not match the release \(v) manifest: use the app from the same release"
        case .scriptNotInRelease(let s): return "\(s) is not a release artifact"
        case .programNotFound(let p): return "\(p) was not found"
        case .needsRoot: return "this must run as root (helios-setup asks for the administrator password itself)"
        }
    }
}

/// The scripts in the app's Resources must be the release's (sha256 against the manifest).
public enum ScriptVerifier {
    public static func path(of script: String, scriptsDir: String, manifest: Manifest, fs: FileSystem) throws -> String {
        let path = (scriptsDir as NSString).appendingPathComponent(script)
        guard fs.exists(path) else { throw InstallerError.scriptMissing(script) }
        guard let art = manifest.artifact(script), let want = art.sha256 else {
            throw InstallerError.scriptNotInRelease(script)
        }
        guard Hashing.sha256(file: path, fs: fs) == want else {
            throw InstallerError.scriptMismatch(script, manifest.release?.version ?? "?")
        }
        return path
    }

    /// A root-owned copy of the script, checked against the manifest after it was copied: what runs is what was hashed,
    /// whatever happens to the original in a user-writable place.
    public static func verifiedCopy(
        of script: String, scriptsDir: String, manifest: Manifest, fs: FileSystem, scratch: RootScratch
    ) throws -> String {
        let source = (scriptsDir as NSString).appendingPathComponent(script)
        guard fs.exists(source) else { throw InstallerError.scriptMissing(script) }
        guard let want = manifest.artifact(script)?.sha256 else { throw InstallerError.scriptNotInRelease(script) }
        let copy = try scratch.copy(source)
        guard Hashing.sha256(file: copy, fs: fs) == want else {
            throw InstallerError.scriptMismatch(script, manifest.release?.version ?? "?")
        }
        return copy
    }

    /// The path to run: the verified root-owned copy under `scratch`, or on the user side the file in place.
    public static func runnable(
        _ script: String, scriptsDir: String, manifest: Manifest, fs: FileSystem, scratch: RootScratch?
    ) throws -> String {
        if let scratch {
            return try verifiedCopy(of: script, scriptsDir: scriptsDir, manifest: manifest, fs: fs, scratch: scratch)
        }
        return try path(of: script, scriptsDir: scriptsDir, manifest: manifest, fs: fs)
    }

    /// Every script artifact the app carries, checked: [(name, ok)].
    public static func report(scriptsDir: String, manifest: Manifest, fs: FileSystem) -> [(String, Bool)] {
        manifest.artifacts.filter { $0.kind == "script" }.map { a in
            ((try? path(of: a.name, scriptsDir: scriptsDir, manifest: manifest, fs: fs)) != nil
                ? (a.name, true) : (a.name, false))
        }
    }
}

public struct Installer {
    let runner: CommandRunner
    let fs: FileSystem
    public let scriptsDir: String
    /// Set on the root side: scripts run from a root-owned, re-verified copy.
    let scratch: RootScratch?

    public static let searchPath = ["/usr/bin", "/bin", "/usr/sbin", "/sbin", "/usr/libexec"]

    public init(runner: CommandRunner, fs: FileSystem, scriptsDir: String, scratch: RootScratch? = nil) {
        self.runner = runner
        self.fs = fs
        self.scriptsDir = scriptsDir
        self.scratch = scratch
    }

    /// The process to start for an item's action: argv, environment, working directory. `.cli` commands are
    /// returned as nil argv for the caller (the CLI dispatches them itself).
    public func plan(
        _ item: SetupItem, action: String, manifest: Manifest, releaseDir: String,
        user: UserContext
    ) throws -> (argv: [String], env: [String: String], cwd: String)? {
        guard let line = item.command(action) else { throw InstallerError.noCommand(item.id, action) }
        let env = baseEnvironment(user)
        switch try CommandParser.parse(line) {
        case .script(let name, let args, let extra):
            let path = try ScriptVerifier.runnable(name, scriptsDir: scriptsDir, manifest: manifest, fs: fs, scratch: scratch)
            return ([Tool.bash, path] + args, env.merging(extra) { $1 }.merging(item.env) { $1 }, releaseDir)
        case .exec(let argv, _, let extra):
            return ([try resolve(argv[0])] + argv.dropFirst(), env.merging(extra) { $1 }, releaseDir)
        case .cli:
            return nil
        }
    }

    /// True when the action has to run as root: every script action except status, and `sudo` commands.
    public static func needsRoot(_ item: SetupItem, action: String) -> Bool {
        guard let line = item.command(action), let cmd = try? CommandParser.parse(line) else { return false }
        switch cmd {
        case .script: return action != "status"
        case .exec(_, let root, _): return root
        case .cli: return action != "status"
        }
    }

    /// Runs a planned process with its output on this process's stdout (or appended to `output`).
    public func execute(_ plan: (argv: [String], env: [String: String], cwd: String), output: String?) -> Int32 {
        runner.stream(plan.argv, env: plan.env, cwd: plan.cwd, output: output)
    }

    /// A clean environment: the user's identity and a fixed PATH, nothing inherited from the caller.
    public func baseEnvironment(_ user: UserContext) -> [String: String] {
        [
            "HOME": user.home, "USER": user.name, "LOGNAME": user.name, "PATH": Self.searchPath.joined(separator: ":"),
            "LANG": "en_US.UTF-8", "TMPDIR": "/tmp/",
        ]
    }

    func resolve(_ program: String) throws -> String {
        if program.hasPrefix("/") { return program }
        for dir in Self.searchPath {
            let p = (dir as NSString).appendingPathComponent(program)
            if fs.exists(p) { return p }
        }
        throw InstallerError.programNotFound(program)
    }
}
