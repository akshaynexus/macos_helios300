// SPDX-License-Identifier: GPL-2.0-only
//
// Ports: every side effect the core has (running a command, the file system, the network, the clock, a root
// command) goes through one of these protocols. The real implementations below use Foundation only, so the same
// code runs on the Mac and in `swift test` on Linux; tests swap in fakes (Tests/HeliosCoreTests/Fakes.swift).
import Foundation

#if canImport(FoundationNetworking)
    import FoundationNetworking
#endif

/// What a finished command returned.
public struct CommandResult: Equatable {
    public var status: Int32
    public var stdout: String
    public var stderr: String

    public init(status: Int32, stdout: String = "", stderr: String = "") {
        self.status = status
        self.stdout = stdout
        self.stderr = stderr
    }

    public var ok: Bool { status == 0 }
}

/// Runs a command. stdin is always /dev/null: the release scripts must never read their own body (or anything
/// else) from stdin (tools/macos/test-stdin.sh; the asroot stdin hazard).
public protocol CommandRunner {
    /// Runs argv and captures stdout and stderr.
    func capture(_ argv: [String], env: [String: String]?, cwd: String?) -> CommandResult
    /// Runs argv with stdout and stderr going to `output` (a file path, or nil for this process's own), returns
    /// the exit status.
    func stream(_ argv: [String], env: [String: String]?, cwd: String?, output: String?) -> Int32
}

extension CommandRunner {
    public func capture(_ argv: [String]) -> CommandResult { capture(argv, env: nil, cwd: nil) }
}

/// The file system operations the core needs. Paths are absolute strings.
public protocol FileSystem {
    func exists(_ path: String) -> Bool
    func isDirectory(_ path: String) -> Bool
    func isSymlink(_ path: String) -> Bool
    func read(_ path: String) throws -> Data
    func write(_ data: Data, to path: String) throws
    func list(_ path: String) throws -> [String]
    func makeDirectory(_ path: String) throws
    func copy(_ from: String, to: String) throws
    func move(_ from: String, to: String) throws
    func remove(_ path: String) throws
    func freeBytes(_ path: String) -> Int64?
    func size(_ path: String) -> Int64?
}

/// HTTPS GET. Every download is checked against a hash afterwards, so this only has to fetch bytes.
public protocol HTTPClient {
    func get(_ url: URL, headers: [String: String]) throws -> Data
}

/// The current time (state records, backup names, check freshness).
public protocol Clock {
    var now: Date { get }
}

/// Runs a command as root. The GUI asks for an administrator password (osascript), the CLI uses sudo, and a
/// process that is already root runs it directly. stdin is /dev/null in every case.
public protocol PrivilegedRunner {
    /// `output`: a file the command's stdout and stderr are appended to, or nil for the caller's own.
    func runAsRoot(_ argv: [String], prompt: String, output: String?) -> Int32
}

// MARK: - Real implementations

public struct ProcessRunner: CommandRunner {
    public init() {}

    public func capture(_ argv: [String], env: [String: String]?, cwd: String?) -> CommandResult {
        guard let process = makeProcess(argv, env: env, cwd: cwd) else {
            return CommandResult(status: 127, stderr: "no such program: \(argv.first ?? "")")
        }
        let out = Pipe()
        let err = Pipe()
        process.standardOutput = out
        process.standardError = err
        do { try process.run() } catch {
            return CommandResult(status: 127, stderr: "\(argv.first ?? ""): \(error.localizedDescription)")
        }
        // Read both pipes concurrently so a chatty command cannot fill one and block.
        let errData = Box(Data())
        let group = DispatchGroup()
        group.enter()
        DispatchQueue.global().async {
            errData.value = err.fileHandleForReading.readDataToEndOfFile()
            group.leave()
        }
        let outData = out.fileHandleForReading.readDataToEndOfFile()
        group.wait()
        process.waitUntilExit()
        return CommandResult(
            status: process.terminationStatus,
            stdout: String(decoding: outData, as: UTF8.self),
            stderr: String(decoding: errData.value, as: UTF8.self))
    }

    public func stream(_ argv: [String], env: [String: String]?, cwd: String?, output: String?) -> Int32 {
        guard let process = makeProcess(argv, env: env, cwd: cwd) else { return 127 }
        if let output {
            guard let handle = appendHandle(output) else { return 126 }
            process.standardOutput = handle
            process.standardError = handle
        }
        do { try process.run() } catch { return 127 }
        process.waitUntilExit()
        return process.terminationStatus
    }

    private func makeProcess(_ argv: [String], env: [String: String]?, cwd: String?) -> Process? {
        guard let program = argv.first else { return nil }
        let process = Process()
        process.executableURL = URL(fileURLWithPath: program)
        process.arguments = Array(argv.dropFirst())
        if let env { process.environment = env }
        if let cwd { process.currentDirectoryURL = URL(fileURLWithPath: cwd) }
        process.standardInput = FileHandle(forReadingAtPath: "/dev/null")
        return process
    }

    private func appendHandle(_ path: String) -> FileHandle? {
        if !FileManager.default.fileExists(atPath: path) {
            _ = FileManager.default.createFile(atPath: path, contents: nil)
        }
        guard let handle = FileHandle(forWritingAtPath: path) else { return nil }
        handle.seekToEndOfFile()
        return handle
    }
}

public struct LocalFileSystem: FileSystem {
    public init() {}
    private var fm: FileManager { FileManager.default }

    public func exists(_ path: String) -> Bool {
        isSymlink(path) || fm.fileExists(atPath: path)
    }

    public func isDirectory(_ path: String) -> Bool {
        var dir: ObjCBool = false
        return fm.fileExists(atPath: path, isDirectory: &dir) && dir.boolValue && !isSymlink(path)
    }

    public func isSymlink(_ path: String) -> Bool {
        (try? fm.destinationOfSymbolicLink(atPath: path)) != nil
    }

    public func read(_ path: String) throws -> Data {
        try Data(contentsOf: URL(fileURLWithPath: path))
    }

    public func write(_ data: Data, to path: String) throws {
        try makeDirectory((path as NSString).deletingLastPathComponent)
        try data.write(to: URL(fileURLWithPath: path), options: .atomic)
    }

    public func list(_ path: String) throws -> [String] {
        try fm.contentsOfDirectory(atPath: path).sorted()
    }

    public func makeDirectory(_ path: String) throws {
        try fm.createDirectory(atPath: path, withIntermediateDirectories: true)
    }

    public func copy(_ from: String, to: String) throws {
        try makeDirectory((to as NSString).deletingLastPathComponent)
        try fm.copyItem(atPath: from, toPath: to)
    }

    public func move(_ from: String, to: String) throws {
        try makeDirectory((to as NSString).deletingLastPathComponent)
        try fm.moveItem(atPath: from, toPath: to)
    }

    public func remove(_ path: String) throws {
        if exists(path) { try fm.removeItem(atPath: path) }
    }

    public func freeBytes(_ path: String) -> Int64? {
        guard let attrs = try? fm.attributesOfFileSystem(forPath: path) else { return nil }
        return (attrs[.systemFreeSize] as? NSNumber)?.int64Value
    }

    public func size(_ path: String) -> Int64? {
        guard let attrs = try? fm.attributesOfItem(atPath: path) else { return nil }
        return (attrs[.size] as? NSNumber)?.int64Value
    }
}

/// A value handed from a completion handler to the waiting thread (the wait orders the accesses).
final class Box<T>: @unchecked Sendable {
    var value: T
    init(_ value: T) { self.value = value }
}

public struct SystemClock: Clock {
    public init() {}
    public var now: Date { Date() }
}

public enum HTTPError: Error, CustomStringConvertible {
    case status(URL, Int)
    case transport(URL, String)

    public var description: String {
        switch self {
        case .status(let url, let code): return "\(url.absoluteString) answered HTTP \(code)"
        case .transport(let url, let why): return "\(url.absoluteString) could not be fetched: \(why)"
        }
    }
}

/// URLSession, synchronous: the core is driven from a background thread (GUI) or a CLI.
public struct URLSessionHTTPClient: HTTPClient {
    public var timeout: TimeInterval

    public init(timeout: TimeInterval = 120) { self.timeout = timeout }

    public func get(_ url: URL, headers: [String: String]) throws -> Data {
        var request = URLRequest(url: url, timeoutInterval: timeout)
        request.setValue("helios300-setup", forHTTPHeaderField: "User-Agent")
        for (k, v) in headers { request.setValue(v, forHTTPHeaderField: k) }
        let semaphore = DispatchSemaphore(value: 0)
        let result = Box<Result<Data, Error>>(.failure(HTTPError.transport(url, "no answer")))
        let task = URLSession.shared.dataTask(with: request) { data, response, error in
            if let error {
                result.value = .failure(HTTPError.transport(url, error.localizedDescription))
            } else if let http = response as? HTTPURLResponse, !(200..<300).contains(http.statusCode) {
                result.value = .failure(HTTPError.status(url, http.statusCode))
            } else {
                result.value = .success(data ?? Data())
            }
            semaphore.signal()
        }
        task.resume()
        semaphore.wait()
        return try result.value.get()
    }
}
