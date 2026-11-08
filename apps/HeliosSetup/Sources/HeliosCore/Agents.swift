// SPDX-License-Identifier: GPL-2.0-only
//
// LaunchAgents the app installs for the user and removes again (the same pattern as OCLP's post-install agents:
// a per-user plist in ~/Library/LaunchAgents, loaded by launchd at login).
//   resume  after "Restart now": opens the app at the next login with --resume, so it continues where it was;
//           the app deletes it as soon as it has started in the new boot.
//   update  the daily check (`--background-check`): looks for a newer release and posts a notification; it never
//           installs anything (that needs the user's click in the app).
import Foundation

public struct LaunchAgent: Equatable {
    public var label: String
    public var program: [String]
    public var startInterval: Int?
    public var runAtLoad: Bool

    public static let resumeLabel = "org.helios300.setup.resume"
    public static let updateLabel = "org.helios300.setup.updatecheck"

    public static func resume(bundleId: String) -> LaunchAgent {
        LaunchAgent(
            label: resumeLabel, program: [Tool.open, "-b", bundleId, "--args", "--resume"], startInterval: nil,
            runAtLoad: true)
    }

    public static func updateCheck(appExecutable: String) -> LaunchAgent {
        LaunchAgent(
            label: updateLabel, program: [appExecutable, "--background-check"], startInterval: 86_400,
            runAtLoad: false)
    }

    public func plist() throws -> Data {
        var d: [String: Any] = [
            "Label": label, "ProgramArguments": program, "RunAtLoad": runAtLoad,
            "ProcessType": "Background",
        ]
        if let s = startInterval { d["StartInterval"] = s }
        return try PropertyListSerialization.data(fromPropertyList: d, format: .xml, options: 0)
    }

    public static func path(_ label: String, home: String) -> String {
        (home as NSString).appendingPathComponent("Library/LaunchAgents/\(label).plist")
    }
}

public struct AgentManager {
    let fs: FileSystem
    let runner: CommandRunner
    let home: String
    let uid: UInt32

    public init(fs: FileSystem, runner: CommandRunner, home: String, uid: UInt32) {
        self.fs = fs
        self.runner = runner
        self.home = home
        self.uid = uid
    }

    /// Writes the agent; `load` also starts it now (the update check), otherwise launchd reads it at next login.
    public func install(_ a: LaunchAgent, load: Bool) throws {
        let p = LaunchAgent.path(a.label, home: home)
        try fs.write(try a.plist(), to: p)
        if load {
            _ = runner.capture([Tool.launchctl, "bootout", "gui/\(uid)/\(a.label)"])
            _ = runner.capture([Tool.launchctl, "bootstrap", "gui/\(uid)", p])
        }
    }

    public func remove(_ label: String) {
        _ = runner.capture([Tool.launchctl, "bootout", "gui/\(uid)/\(label)"])
        try? fs.remove(LaunchAgent.path(label, home: home))
    }

    public func isInstalled(_ label: String) -> Bool { fs.exists(LaunchAgent.path(label, home: home)) }
}
