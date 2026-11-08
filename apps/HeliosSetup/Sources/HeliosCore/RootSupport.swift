// SPDX-License-Identifier: GPL-2.0-only
//
// What keeps the root half from running anything a user-level process could change.
//   RootBootstrap  the single command the administrator prompt runs: it copies helios-setup into a new root-owned
//                  directory, checks that copy against the SHA-256 the caller saw, and runs the copy.
//   RootScratch    the same for everything `_root` then starts: release scripts and ocvalidate are copied into a
//                  mkdtemp under /var/root (root:wheel, 0700), re-verified there, and only that copy runs.
//   RootLock       one root job at a time, whatever started it (flock, O_NOFOLLOW).
import Foundation

public enum RootScratchError: Error, CustomStringConvertible, Equatable {
    case symlink(String)

    public var description: String {
        switch self {
        case .symlink(let p): return "\(p) is a symbolic link: refused"
        }
    }
}

/// A private directory only root can enter, for one `_root` command.
public struct RootScratch {
    let fs: FileSystem
    public let dir: String

    public static func make(fs: FileSystem, parent: String) throws -> RootScratch {
        let dir = try fs.makeSecureDirectory(in: parent, prefix: "helios-setup")
        try fs.ownAsRoot(dir)
        return RootScratch(fs: fs, dir: dir)
    }

    /// A root-owned, executable copy of the file at `path`. The caller verifies the copy, never the original.
    public func copy(_ path: String) throws -> String {
        guard !fs.isSymlink(path) else { throw RootScratchError.symlink(path) }
        let dest = (dir as NSString).appendingPathComponent((path as NSString).lastPathComponent)
        try fs.remove(dest)
        try fs.copy(path, to: dest)
        guard !fs.isSymlink(dest) else { throw RootScratchError.symlink(dest) }
        try fs.ownAsRoot(dest)
        try fs.makeExecutable(dest)
        return dest
    }

    public func remove() { try? fs.remove(dir) }
}

/// The root command the prompt approves. `helios-setup` lives in the app bundle, somewhere the logged-in user can
/// write; root runs a private copy of it, so nothing can change the binary after the hash was checked. The hash comes
/// from the user side: this closes the window between that side's check and the run, not a session that is already
/// compromised. The script is one argv word and every value is a further word, so nothing is spliced into shell text.
public enum RootBootstrap {
    public static let script = """
        cli=$1 want=$2 parent=$3
        shift 3
        umask 077
        dir=$(/usr/bin/mktemp -d "$parent/helios-setup.XXXXXX") || exit 126
        trap '/bin/rm -rf "$dir"' EXIT
        if [ -L "$cli" ]; then echo "helios-setup: $cli is a symbolic link" >&2; exit 126; fi
        /bin/cp "$cli" "$dir/helios-setup" || exit 126
        if [ "$(/usr/bin/id -u)" = 0 ]; then /usr/sbin/chown root:wheel "$dir" "$dir/helios-setup" || exit 126; fi
        /bin/chmod 0700 "$dir/helios-setup" || exit 126
        got=$(/usr/bin/shasum -a 256 "$dir/helios-setup") || exit 126
        got=${got%% *}
        if [ "$got" != "$want" ]; then echo "helios-setup: $cli changed after it was checked" >&2; exit 126; fi
        "$dir/helios-setup" "$@"
        exit $?
        """

    public static func argv(cli: String, sha256: String, runDir: String, arguments: [String]) -> [String] {
        ["/bin/sh", "-p", "-c", script, "helios-setup-bootstrap", cli, sha256, runDir] + arguments
    }
}

public enum RootLock {
    public static let path = "/var/run/org.helios300.setup.lock"

    /// The lock's file descriptor (held until the process exits), or nil when another job holds it.
    public static func acquire(at path: String = path) -> Int32? {
        let fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW, 0o600)
        guard fd >= 0 else { return nil }
        guard flock(fd, LOCK_EX | LOCK_NB) == 0 else {
            close(fd)
            return nil
        }
        return fd
    }
}
