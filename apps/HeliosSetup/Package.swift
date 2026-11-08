// swift-tools-version:5.9
// SPDX-License-Identifier: GPL-2.0-only
//
// Helios 300 Setup: the post-install app for the Acer Predator Helios 300 G3-571 on macOS 26.
//   HeliosCore    Foundation (+ CryptoKit on macOS): manifest model, probes, planner, installer, EFI, config merge,
//                 updater, state.
//                 Builds and tests on Linux (`swift test`) as well as on macOS.
//   helios-setup  the headless CLI over HeliosCore (status, plan, install, update, rollback, efi, ...).
//   HeliosSetup   the AppKit front end over the same core (macOS only; an empty stub elsewhere).
// Cross-built on Linux by tools/release/cross-setup-app.sh (darwin Swift SDK, x86_64-apple-macosx, minos 26.0).
import PackageDescription

let linuxCrypto: Target.Dependency = .product(name: "Crypto", package: "swift-crypto", condition: .when(platforms: [.linux]))

let package = Package(
    name: "HeliosSetup",
    platforms: [.macOS("26.0")],
    products: [
        .library(name: "HeliosCore", targets: ["HeliosCore"]),
        .executable(name: "helios-setup", targets: ["helios-setup"]),
        .executable(name: "HeliosSetup", targets: ["HeliosSetup"]),
    ],
    dependencies: [
        // Linux only (swift test): on macOS HeliosCore uses Apple's CryptoKit, the same Ed25519 API, and the Mach-O links
        // nothing from this package. See ManifestSignature.swift.
        .package(url: "https://github.com/apple/swift-crypto.git", "3.0.0"..<"5.0.0")
    ],
    targets: [
        .target(name: "HeliosCore", dependencies: [linuxCrypto]),
        .executableTarget(name: "helios-setup", dependencies: ["HeliosCore"]),
        .executableTarget(name: "HeliosSetup", dependencies: ["HeliosCore"]),
        .testTarget(name: "HeliosCoreTests", dependencies: ["HeliosCore", linuxCrypto], exclude: ["Fixtures"]),
    ],
    swiftLanguageVersions: [.v5]
)
