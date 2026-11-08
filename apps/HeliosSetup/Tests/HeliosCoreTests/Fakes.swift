// SPDX-License-Identifier: GPL-2.0-only
//
// Fakes for the ports, and fixture helpers. The file system in tests is the real one, inside a temp directory.
import Foundation
import XCTest

@testable import HeliosCore

let fixtures = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("Fixtures")
let repoRoot = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
    .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()

func fixture(_ name: String) -> String {
    (try? String(contentsOf: fixtures.appendingPathComponent(name), encoding: .utf8)) ?? ""
}

func fixtureData(_ name: String) -> Data {
    (try? Data(contentsOf: fixtures.appendingPathComponent(name))) ?? Data()
}

func realSpec() throws -> Manifest {
    try Manifest.decode(Data(contentsOf: repoRoot.appendingPathComponent("tools/release/spec.json")))
}

func manifestFixture(_ name: String) throws -> Manifest { try Manifest.decode(fixtureData(name)) }

/// Answers commands from a table (argv joined with spaces -> result); records every call.
final class FakeRunner: CommandRunner {
    var answers: [String: CommandResult] = [:]
    var calls: [[String]] = []
    var streamed: [(argv: [String], env: [String: String]?, cwd: String?)] = []
    var streamStatus: Int32 = 0
    /// Called for capture commands without an answer (e.g. ditto -> a real copy).
    var fallback: (([String]) -> CommandResult)?

    func set(_ argv: String, _ stdout: String, status: Int32 = 0) {
        answers[argv] = CommandResult(status: status, stdout: stdout)
    }

    func capture(_ argv: [String], env: [String: String]?, cwd: String?) -> CommandResult {
        calls.append(argv)
        if let a = answers[argv.joined(separator: " ")] { return a }
        return fallback?(argv) ?? CommandResult(status: 1, stderr: "fake: no answer for \(argv.joined(separator: " "))")
    }

    func stream(_ argv: [String], env: [String: String]?, cwd: String?, output: String?) -> Int32 {
        calls.append(argv)
        streamed.append((argv, env, cwd))
        return streamStatus
    }
}

struct FakeClock: Clock {
    var now: Date
}

final class FakeHTTP: HTTPClient {
    var pages: [String: Data] = [:]
    var fetched: [String] = []

    func get(_ url: URL, headers: [String: String]) throws -> Data {
        fetched.append(url.absoluteString)
        guard let d = pages[url.absoluteString] else { throw HTTPError.status(url, 404) }
        return d
    }
}

final class FakePrivileged: PrivilegedRunner {
    var calls: [[String]] = []
    var status: Int32 = 0

    func runAsRoot(_ argv: [String], prompt: String, output: String?) -> Int32 {
        calls.append(argv)
        return status
    }
}

/// A fresh temp directory, removed at the end of the test.
func tempDir(_ test: XCTestCase) -> String {
    let dir = (NSTemporaryDirectory() as NSString).appendingPathComponent("helios-test-\(UUID().uuidString)")
    try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
    test.addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
    return dir
}

func write(_ text: String, _ path: String) {
    try? LocalFileSystem().write(Data(text.utf8), to: path)
}

/// zip -r of `dir`'s `top` into `zip` (deterministic enough for tests).
func makeZip(_ dir: String, top: String, zip: String) {
    let r = ProcessRunner().capture(["/usr/bin/zip", "-q", "-r", "-X", zip, top], env: nil, cwd: dir)
    XCTAssertTrue(r.ok, r.stderr)
}

/// The real file system under a temp root: "/Library/Extensions/X" lives at <root>/Library/Extensions/X.
struct RootedFS: FileSystem {
    let root: String
    let fs = LocalFileSystem()

    func p(_ path: String) -> String { root + (path.hasPrefix("/") ? path : "/" + path) }

    func exists(_ path: String) -> Bool { fs.exists(p(path)) }
    func isDirectory(_ path: String) -> Bool { fs.isDirectory(p(path)) }
    func isSymlink(_ path: String) -> Bool { fs.isSymlink(p(path)) }
    func read(_ path: String) throws -> Data { try fs.read(p(path)) }
    func write(_ data: Data, to path: String) throws { try fs.write(data, to: p(path)) }
    func list(_ path: String) throws -> [String] { try fs.list(p(path)) }
    func makeDirectory(_ path: String) throws { try fs.makeDirectory(p(path)) }
    func copy(_ from: String, to: String) throws { try fs.copy(p(from), to: p(to)) }
    func move(_ from: String, to: String) throws { try fs.move(p(from), to: p(to)) }
    func remove(_ path: String) throws { try fs.remove(p(path)) }
    func freeBytes(_ path: String) -> Int64? { fs.freeBytes(p(path)) }
    func size(_ path: String) -> Int64? { fs.size(p(path)) }

    func touch(_ path: String, _ text: String = "x") { try? write(Data(text.utf8), to: path) }
}

/// A Mac as the probes see it: the commands answered from fixtures, files under a temp root.
func fakeMac(
    _ test: XCTestCase, pg106Ready: Bool = true, loaded: [String] = [], files: [String] = [],
    sleepDisabled: Bool = true, boot: String = "BOOT-A", build: String = "25G332",
    bootArgs: String = "-v keepsyms=1"
) -> (FakeRunner, RootedFS, ProbeContext) {
    let r = FakeRunner()
    let fs = RootedFS(root: tempDir(test))
    for f in files { fs.touch(f) }
    r.set(
        "/usr/sbin/ioreg -r -c PG106 -d 1 -w 0",
        fixture("ioreg-PG106-notready.txt")
            .replacingOccurrences(of: "\"PG106Ready\" = No", with: "\"PG106Ready\" = \(pg106Ready ? "Yes" : "No")"))
    let base = fixture("kmutil-showloaded.txt")
    let extra = loaded.enumerated().map { i, id in
        "  \(200 + i)    0 0xffffff8001a40000 0x6d50     0x6d50     \(id) (1.0.0) BA7EF360-2EE4-3996-8C63-F767B7589077 <19 7 3>"
    }.joined(separator: "\n")
    r.set("/usr/bin/kmutil showloaded", base + "\n" + extra + "\n")
    r.set("/usr/bin/pmset -g", fixture(sleepDisabled ? "pmset-g-sleep-disabled.txt" : "pmset-g-sleep-allowed.txt"))
    r.set("/usr/sbin/nvram boot-args", "boot-args\t\(bootArgs)\n")
    r.set("/usr/bin/sw_vers -buildVersion", build + "\n")
    r.set("/usr/sbin/sysctl -n kern.bootsessionuuid", boot + "\n")
    r.set("/usr/sbin/sysctl -n kern.osrelease", "25.6.0\n")
    r.set("/usr/bin/fdesetup status", "FileVault is Off.\n")
    return (r, fs, ProbeContext(runner: r, fs: fs))
}
