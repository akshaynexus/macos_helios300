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
    /// The update unit being applied: its undo so far, written before each change. Set only while a unit runs, so
    /// finding it on the next run means that run was killed, and the unit is undone first.
    public var interrupted: RollbackRecord?

    public init() {}

    /// Every key optional: a state written by an older build still reads.
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        installed = try c.decodeIfPresent(InstalledRelease.self, forKey: .installed)
        previous = try c.decodeIfPresent(InstalledRelease.self, forKey: .previous)
        changes = try c.decodeIfPresent([ChangeRecord].self, forKey: .changes) ?? []
        rollbacks = try c.decodeIfPresent([RollbackRecord].self, forKey: .rollbacks) ?? []
        pending = try c.decodeIfPresent(PendingUpdate.self, forKey: .pending)
        efiBackups = try c.decodeIfPresent([String].self, forKey: .efiBackups) ?? []
        interrupted = try c.decodeIfPresent(RollbackRecord.self, forKey: .interrupted)
    }

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

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        releaseDir = try c.decodeIfPresent(String.self, forKey: .releaseDir)
        checks = try c.decodeIfPresent([String: CheckRecord].self, forKey: .checks) ?? [:]
        runs = try c.decodeIfPresent([String: RunRecord].self, forKey: .runs) ?? [:]
        resumeAfterLogin = try c.decodeIfPresent(Bool.self, forKey: .resumeAfterLogin) ?? false
        includePrereleases = try c.decodeIfPresent(Bool.self, forKey: .includePrereleases)
        dailyUpdateCheck = try c.decodeIfPresent(Bool.self, forKey: .dailyUpdateCheck) ?? false
        remindLaterUntil = try c.decodeIfPresent(Date.self, forKey: .remindLaterUntil)
        skippedTag = try c.decodeIfPresent(String.self, forKey: .skippedTag)
        thirdPartyUpdates = try c.decodeIfPresent(Bool.self, forKey: .thirdPartyUpdates) ?? false
        efiSource = try c.decodeIfPresent(String.self, forKey: .efiSource)
    }
}

public enum StateError: Error, CustomStringConvertible, Equatable {
    case corrupt(String, String)

    public var description: String {
        switch self {
        case .corrupt(let path, let why): return "\(path) cannot be read (\(why)): it was left as it is"
        }
    }
}

public enum StatePaths {
    public static let systemDir = "/Library/Application Support/Helios300"
    public static let system = systemDir + "/state.plist"
    public static let releases = systemDir + "/releases"
    /// Where root keeps its private copies while it works: /var/root, not the admin-writable /Library/Application Support.
    public static let rootRun = "/var/root"

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

    /// The system state for showing. A missing file is an empty state; so is a corrupt one here, and `readSystem`
    /// tells the difference for anything that is about to write.
    public func loadSystem() -> SystemState { (try? readSystem()) ?? SystemState() }
    public func loadUser() -> UserState { (try? readUser()) ?? UserState() }

    /// A missing file is an empty state; a file that does not decode throws, so it is never overwritten with one.
    public func readSystem() throws -> SystemState { try read(systemPath, missing: SystemState()) }
    public func readUser() throws -> UserState { try read(userPath, missing: UserState()) }

    public func saveSystem(_ s: SystemState) throws { try save(s, to: systemPath) }
    public func saveUser(_ s: UserState) throws { try save(s, to: userPath) }

    private func read<T: Decodable>(_ path: String, missing: T) throws -> T {
        guard fs.exists(path) else { return missing }
        do {
            return try PropertyListDecoder().decode(T.self, from: try fs.read(path))
        } catch {
            throw StateError.corrupt(path, "\(error)")
        }
    }

    private func save<T: Encodable>(_ value: T, to path: String) throws {
        let enc = PropertyListEncoder()
        enc.outputFormat = .xml
        try fs.write(try enc.encode(value), to: path)
    }
}
