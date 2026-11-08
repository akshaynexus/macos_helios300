// SPDX-License-Identifier: GPL-2.0-only
//
// Root commands. The GUI never stores or sees a password: it asks macOS for an administrator prompt through
// `osascript -e 'do shell script … with administrator privileges'` (the same mechanism Apple documents for
// AppleScript; OCLP's GUI also elevates through an authorization prompt rather than holding credentials). The
// command line is built inside AppleScript from `quoted form of` each argument, so no argument is ever spliced
// into shell text by this code. stdin of the root command is /dev/null.
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
    let fs: FileSystem
    let tempDir: String

    public init(
        runner: CommandRunner = ProcessRunner(), fs: FileSystem = LocalFileSystem(),
        tempDir: String = NSTemporaryDirectory()
    ) {
        self.runner = runner
        self.fs = fs
        self.tempDir = tempDir
    }

    /// The AppleScript: argv items are (prompt, log, rc file, command...). The exit status goes to the rc file,
    /// so a failing command and a cancelled prompt can be told apart.
    public static let script = [
        "on run argv",
        "set cmd to \"\"",
        "repeat with i from 4 to count of argv",
        "set cmd to cmd & quoted form of (item i of argv) & \" \"",
        "end repeat",
        "set cmd to cmd & \"</dev/null >>\" & quoted form of (item 2 of argv) & \" 2>&1; echo $? >\" & quoted form of (item 3 of argv)",
        "do shell script cmd with prompt (item 1 of argv) with administrator privileges",
        "end run",
    ]

    /// The full osascript argv for one root command.
    public static func osascriptArguments(_ argv: [String], prompt: String, log: String, rcFile: String) -> [String] {
        var out = ["/usr/bin/osascript"]
        for line in script { out += ["-e", line] }
        return out + [prompt, log, rcFile] + argv
    }

    public func runAsRoot(_ argv: [String], prompt: String, output: String?) -> Int32 {
        let token = UUID().uuidString
        let log = output ?? (tempDir as NSString).appendingPathComponent("helios-setup-\(token).log")
        let rcFile = (tempDir as NSString).appendingPathComponent("helios-setup-\(token).rc")
        if !fs.exists(log) { try? fs.write(Data(), to: log) }
        let args = Self.osascriptArguments(argv, prompt: prompt, log: log, rcFile: rcFile)
        _ = runner.capture(args, env: nil, cwd: nil)
        defer { try? fs.remove(rcFile) }
        guard let data = try? fs.read(rcFile),
            let rc = Int32(String(decoding: data, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines))
        else { return privilegedCancelled }
        return rc
    }
}

/// The runner that fits this process: direct when root, otherwise the given interactive one.
public func defaultPrivilegedRunner(gui: Bool) -> PrivilegedRunner {
    if geteuid() == 0 { return DirectRootRunner() }
    return gui ? OsascriptRunner() : SudoRunner()
}
