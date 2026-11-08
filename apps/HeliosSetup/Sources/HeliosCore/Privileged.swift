// SPDX-License-Identifier: GPL-2.0-only
//
// Root commands. The GUI never stores or sees a password: it asks macOS for an administrator prompt through
// `osascript -e 'do shell script … with administrator privileges'` (the same mechanism Apple documents for
// AppleScript; OCLP's GUI also elevates through an authorization prompt rather than holding credentials). The
// command line is built inside AppleScript from `quoted form of` each argument, so no argument is ever spliced
// into shell text by this code. stdin of the root command is /dev/null. The root shell writes no file of its own:
// `_root` opens the log itself (never through a link), and the exit status comes back as the last line of the
// command's output, so no root redirection ever points into a user-writable folder.
import Foundation

/// Exit status used when the administrator prompt was cancelled (no command ran).
public let privilegedCancelled: Int32 = 125

/// Already root (CLI under sudo or asroot): runs the command directly.
public struct DirectRootRunner: PrivilegedRunner {
    let runner: CommandRunner

    public init(runner: CommandRunner = ProcessRunner()) { self.runner = runner }

    public func runAsRoot(_ argv: [String], prompt: String, output: String?) -> Int32 {
        runner.stream(argv, env: nil, cwd: nil, output: output)
    }
}

/// CLI in a terminal: sudo asks for the password on the terminal (it reads /dev/tty, not stdin).
public struct SudoRunner: PrivilegedRunner {
    let runner: CommandRunner

    public init(runner: CommandRunner = ProcessRunner()) { self.runner = runner }

    public func runAsRoot(_ argv: [String], prompt: String, output: String?) -> Int32 {
        runner.stream(["/usr/bin/sudo", "--"] + argv, env: nil, cwd: nil, output: output)
    }
}

/// GUI: the macOS administrator prompt.
public struct OsascriptRunner: PrivilegedRunner {
    let runner: CommandRunner

    public init(runner: CommandRunner = ProcessRunner()) { self.runner = runner }

    /// The AppleScript: argv items are (prompt, command...). The command's exit status is the last line it prints,
    /// so a failing command and a cancelled prompt (no output at all) can be told apart.
    public static let script = [
        "on run argv",
        "set cmd to \"\"",
        "repeat with i from 2 to count of argv",
        "set cmd to cmd & quoted form of (item i of argv) & \" \"",
        "end repeat",
        "set cmd to cmd & \"</dev/null 2>&1; echo $?\"",
        "do shell script cmd with prompt (item 1 of argv) with administrator privileges",
        "end run",
    ]

    /// The full osascript argv for one root command.
    public static func osascriptArguments(_ argv: [String], prompt: String) -> [String] {
        var out = ["/usr/bin/osascript"]
        for line in script { out += ["-e", line] }
        return out + [prompt] + argv
    }

    /// Lines the command printed before its status (the status is the last one).
    static func parse(_ stdout: String) -> (status: Int32, text: String)? {
        var lines = stdout.components(separatedBy: .newlines).filter { !$0.isEmpty }
        guard let last = lines.popLast(), let status = Int32(last) else { return nil }
        return (status, lines.joined(separator: "\n"))
    }

    public func runAsRoot(_ argv: [String], prompt: String, output: String?) -> Int32 {
        let result = runner.capture(Self.osascriptArguments(argv, prompt: prompt), env: nil, cwd: nil)
        guard result.ok, let parsed = Self.parse(result.stdout) else { return privilegedCancelled }
        if !parsed.text.isEmpty, let output, let log = openLogForAppending(output) {
            log.write(Data((parsed.text + "\n").utf8))
        }
        return parsed.status
    }
}

/// The runner that fits this process: direct when root, otherwise the given interactive one.
public func defaultPrivilegedRunner(gui: Bool) -> PrivilegedRunner {
    if geteuid() == 0 { return DirectRootRunner() }
    return gui ? OsascriptRunner() : SudoRunner()
}
