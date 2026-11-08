// SPDX-License-Identifier: GPL-2.0-only
//
// The update trust root: every manifest the updater acts on carries `manifest.json.sig`, an Ed25519 signature (64 raw
// bytes) over the exact bytes of manifest.json, made by tools/release.sh with the owner's offline key. The public half
// is compiled in here; `tools/release/manifest-ed25519.pub` is the same value for humans and for the release tooling
// (ManifestSignatureTests keep the two equal). GitHub and the network are not trusted: without a valid signature an
// update is refused before anything in the manifest is believed. Both sides check it: the user side before showing an
// offer, and the root side again on the bytes it is about to apply. docs/MANIFEST.md "Trust chain".
// The tag and manifest SHA-256 1401 pins are a separate root of trust for its own builds and are unchanged.
import Foundation

#if canImport(CryptoKit)
    import CryptoKit
#else
    import Crypto
#endif

public enum SignatureError: Error, CustomStringConvertible, Equatable {
    case missing(String)
    case invalid(String)
    case badKey

    public var description: String {
        switch self {
        case .missing(let what): return "\(what) has no \(ManifestVerifier.signatureFile): refusing an unsigned release"
        case .invalid(let what): return "\(what): the Ed25519 signature does not verify against this app's release key"
        case .badKey: return "this app's release key is not a valid Ed25519 public key"
        }
    }
}

public struct ManifestVerifier {
    public static let manifestFile = "manifest.json"
    public static let signatureFile = "manifest.json.sig"
    /// The release key, hex (tools/release/manifest-ed25519.pub).
    public static let releaseKeyHex = "3dd3bb3786d82f4964cc3868c1cc7aaf9428f2f1b642ee25883dc94ac09631b1"

    /// The key this app was built to trust.
    public static var release: ManifestVerifier { ManifestVerifier(publicKey: Data(hex: releaseKeyHex) ?? Data()) }

    let publicKey: Data

    public init(publicKey: Data) { self.publicKey = publicKey }

    /// Throws unless `signature` is a valid signature of exactly `manifest`. `what` names it in the error.
    public func verify(_ manifest: Data, signature: Data, what: String) throws {
        guard let key = try? Curve25519.Signing.PublicKey(rawRepresentation: publicKey) else { throw SignatureError.badKey }
        guard key.isValidSignature(signature, for: manifest) else { throw SignatureError.invalid(what) }
    }
}
