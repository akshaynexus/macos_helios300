// SPDX-License-Identifier: GPL-2.0-only
//
// The real Environment for the GUI and the CLI: Foundation ports, the app bundle's own paths, the logged-in user.
import Foundation

extension Environment {
    /// `executable`: this process's binary (Contents/MacOS/HeliosSetup or Contents/MacOS/helios-setup).
    /// `user`: who the work is for (the `_root` options); defaults to the caller (SUDO_USER under sudo).
    public static func live(gui: Bool, executable: String, user: UserContext? = nil) -> Environment {
        let exe = URL(fileURLWithPath: executable).resolvingSymlinksInPath()
        let macos = exe.deletingLastPathComponent()
        let contents = macos.deletingLastPathComponent()
        let isBundle = contents.lastPathComponent == "Contents"
        let resources =
            isBundle
            ? contents.appendingPathComponent("Resources").path
            : ProcessInfo.processInfo.environment["HELIOS_SETUP_RESOURCES"] ?? macos.path
        let app = isBundle ? contents.deletingLastPathComponent().path : nil
        return Environment(
            runner: ProcessRunner(), fs: LocalFileSystem(), http: URLSessionHTTPClient(), clock: SystemClock(),
            privileged: defaultPrivilegedRunner(gui: gui), unzipper: CommandUnzipper(),
            user: user ?? currentUser(), resourcesDir: resources,
            cliPath: macos.appendingPathComponent("helios-setup").path, appPath: app)
    }

    /// The caller, or under sudo the user who ran sudo.
    public static func currentUser() -> UserContext {
        let env = ProcessInfo.processInfo.environment
        if geteuid() == 0, let name = env["SUDO_USER"], let home = NSHomeDirectoryForUser(name) {
            return UserContext(name: name, home: home)
        }
        return UserContext(name: NSUserName(), home: NSHomeDirectory())
    }
}
