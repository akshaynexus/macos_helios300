// SPDX-License-Identifier: GPL-2.0-only
//
// Third-party kexts (optional, off by default): Lilu, WhateverGreen, AppleALC, VirtualSMC, VoodooPS2, ... from
// Dortania's build repo, the same index 1401's engine and validate.py read
// (https://raw.githubusercontent.com/dortania/build-repo/builds/latest.json: product -> versions[0] with
// links.release and hashes.release.sha256). Which bundles belong to which product comes from the manifest's
// `third_party_kexts.products`. A kext is offered when the index's version is newer than the loaded one; the zip
// must match the index's sha256 before anything is unpacked. Kexts the index does not carry (AMFIPass,
// USBToolBox, ...) are listed as "not from the build repo" and never touched.
import Foundation

public struct ThirdPartyUpdate: Equatable {
    public var product: String
    public var bundles: [KextRef]
    public var installed: String?
    public var available: String
    public var url: String
    public var sha256: String
}

public enum DortaniaIndex {
    /// product -> (version, release zip url, sha256).
    public static func parse(_ data: Data) -> [String: (version: String, url: String, sha256: String)] {
        guard let root = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else { return [:] }
        var out: [String: (String, String, String)] = [:]
        for (product, v) in root {
            guard let entry = v as? [String: Any], let first = (entry["versions"] as? [[String: Any]])?.first,
                let version = first["version"] as? String,
                let url = (first["links"] as? [String: Any])?["release"] as? String,
                let sha = ((first["hashes"] as? [String: Any])?["release"] as? [String: Any])?["sha256"] as? String,
                url.hasPrefix("https://github.com/dortania/build-repo/releases/download/"), sha.count == 64
            else { continue }
            out[product] = (version, url, sha)
        }
        return out.mapValues { (version: $0.0, url: $0.1, sha256: $0.2) }
    }

    /// The products with a newer build than the loaded kext. `loaded`: bundle id -> version (kmutil showloaded).
    public static func updates(
        index: [String: (version: String, url: String, sha256: String)],
        products: [String: [KextRef]], loaded: [String: String]
    ) -> [ThirdPartyUpdate] {
        var out: [ThirdPartyUpdate] = []
        for (product, bundles) in products.sorted(by: { $0.key < $1.key }) {
            guard let e = index[product] else { continue }
            let have = bundles.compactMap { loaded[$0.id] }.first
            guard let have, isNewer(e.version, than: have) else { continue }
            out.append(
                ThirdPartyUpdate(
                    product: product, bundles: bundles, installed: have, available: e.version,
                    url: e.url, sha256: e.sha256))
        }
        return out
    }

    /// Dotted numeric versions ("1.7.3" > "1.7.2").
    public static func isNewer(_ a: String, than b: String) -> Bool {
        let x = a.split(separator: ".").map { Int($0) ?? 0 }, y = b.split(separator: ".").map { Int($0) ?? 0 }
        for i in 0..<max(x.count, y.count) {
            let p = i < x.count ? x[i] : 0, q = i < y.count ? y[i] : 0
            if p != q { return p > q }
        }
        return false
    }

    /// Where a bundle sits in an extracted build-repo zip: at the top or under Kexts/ (VirtualSMC's layout).
    public static func locate(_ bundle: String, in dir: String, fs: FileSystem) -> String? {
        for candidate in [bundle, "Kexts/\(bundle)", "Release/\(bundle)"] {
            let p = (dir as NSString).appendingPathComponent(candidate)
            if fs.isDirectory(p) { return p }
        }
        return nil
    }
}
