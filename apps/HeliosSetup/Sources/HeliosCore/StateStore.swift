// SPDX-License-Identifier: GPL-2.0-only
//
// StateStore: what the app remembers, in two property lists.
//   System (/Library/Application Support/Helios300/state.plist, root-owned, written only by `helios-setup` running
//   as root): the installed release (tag + manifest sha256), every driver change with the boot session it was made
//   in (the one-change-per-restart guard reads this), the last update's rollback record.
//   User (~/Library/Application Support/Helios 300 Setup/state.plist): the release folder, dry-run results, the
//   last run of each step, update preferences. Nothing secret is stored anywhere.
import Foundation

public struct InstalledRelease: Codable, Equatable {
    public var tag: String
    public var version: String
    public var manifestSha256: String
    public var releaseDir: String
    public var date: Date
    public var preRelease: Bool

    public init(tag: String, version: String, manifestSha256: String, releaseDir: String, date: Date, preRelease: Bool) {
        self.tag = tag
        self.version = version
        self.manifestSha256 = manifestSha256
        self.releaseDir = releaseDir
        self.date = date
        self.preRelease = preRelease
    }
}

/// One change to the machine, recorded before it is made (a failed half install changed the machine too).
public struct ChangeRecord: Codable, Equatable {
    public var item: String
    public var action: String
    public var bootSession: String
    public var date: Date
    public var driverChange: Bool
    public var needsRestart: Bool

    public init(item: String, action: String, bootSession: String, date: Date, driverChange: Bool, needsRestart: Bool) {
        self.item = item
        self.action = action
        self.bootSession = bootSession
        self.date = date
        self.driverChange = driverChange
        self.needsRestart = needsRestart
    }
}

public struct SystemState: Codable, Equatable {
    public var installed: InstalledRelease?
    /// The release installed before the current one (what a full rollback returns to).
    public var previous: InstalledRelease?
    public var changes: [ChangeRecord] = []
    /// Applied update units, newest last: "Roll back last update" pops one.
    public var rollbacks: [RollbackRecord] = []
    /// An update being applied one unit per restart.
    public var pending: PendingUpdate?
    /// EFI backups made by the OpenCore install, newest last (names on the internal ESP).
    public var efiBackups: [String] = []

    public init() {}

    /// The changes made in this boot session.
    public func changes(inBoot boot: String) -> [ChangeRecord] {
        changes.filter { $0.bootSession == boot }
    }

    /// Keeps the log short: the last 200 records.
    public mutating func record(_ c: ChangeRecord) {
        changes.append(c)
        if changes.count > 200 { changes.removeFirst(changes.count - 200) }
    }
}

public struct PendingUpdate: Codable, Equatable {
    public var tag: String
    public var manifestSha256: String
    public var dir: String
    public var applied: [String]

    public init(tag: String, manifestSha256: String, dir: String, applied: [String] = []) {
        self.tag = tag
        self.manifestSha256 = manifestSha256
        self.dir = dir
        self.applied = applied
    }
}

public struct CheckRecord: Codable, Equatable {
    public var bootSession: String
    public var date: Date
    public var status: Int32
    public var manifestSha256: String

    public init(bootSession: String, date: Date, status: Int32, manifestSha256: String) {
        self.bootSession = bootSession
        self.date = date
        self.status = status
        self.manifestSha256 = manifestSha256
    }
}

public struct RunRecord: Codable, Equatable {
    public var action: String
    public var status: Int32
    public var bootSession: String
    public var date: Date
    public var log: String

    public init(action: String, status: Int32, bootSession: String, date: Date, log: String) {
        self.action = action
        self.status = status
        self.bootSession = bootSession
        self.date = date
        self.log = log
    }
}

public struct UserState: Codable, Equatable {
    public var releaseDir: String?
    public var checks: [String: CheckRecord] = [:]
    public var runs: [String: RunRecord] = [:]
    public var resumeAfterLogin = false
    public var includePrereleases: Bool?
    public var dailyUpdateCheck = false
    public var remindLaterUntil: Date?
    public var skippedTag: String?
    public var thirdPartyUpdates = false
    /// The EFI folder "Install OpenCore to this Mac" copies (the stick's EFI, or a release-built one).
    public var efiSource: String?

    public init() {}
}

public enum StatePaths {
    public static let systemDir = "/Library/Application Support/Helios300"
    public static let system = systemDir + "/state.plist"
    public static let releases = systemDir + "/releases"
    public static let backups = systemDir + "/backups"

    public static func userDir(home: String) -> String {
        (home as NSString).appendingPathComponent("Library/Application Support/Helios 300 Setup")
    }

    public static func user(home: String) -> String {
        (userDir(home: home) as NSString).appendingPathComponent("state.plist")
    }

    public static func logs(home: String) -> String {
        (userDir(home: home) as NSString).appendingPathComponent("logs")
    }
}

public struct StateStore {
    let fs: FileSystem
    public let systemPath: String
    public let userPath: String

    public init(fs: FileSystem, home: String, systemPath: String = StatePaths.system) {
        self.fs = fs
        self.systemPath = systemPath
        self.userPath = StatePaths.user(home: home)
    }

    public func loadSystem() -> SystemState { load(systemPath) ?? SystemState() }
    public func loadUser() -> UserState { load(userPath) ?? UserState() }

    public func saveSystem(_ s: SystemState) throws { try save(s, to: systemPath) }
    public func saveUser(_ s: UserState) throws { try save(s, to: userPath) }

    private func load<T: Decodable>(_ path: String) -> T? {
        guard let data = try? fs.read(path) else { return nil }
        return try? PropertyListDecoder().decode(T.self, from: data)
    }

    private func save<T: Encodable>(_ value: T, to path: String) throws {
        let enc = PropertyListEncoder()
        enc.outputFormat = .xml
        try fs.write(try enc.encode(value), to: path)
    }
}
