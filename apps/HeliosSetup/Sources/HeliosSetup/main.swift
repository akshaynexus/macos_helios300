// SPDX-License-Identifier: GPL-2.0-only
//
// Helios 300 Setup (AppKit). Elsewhere (Linux `swift build` / `swift test`) this target is an empty stub: the
// window code is macOS-only, everything it calls is HeliosCore.
#if canImport(AppKit)
    import AppKit

    let app = NSApplication.shared
    let delegate = AppDelegate()
    app.delegate = delegate
    app.run()
#else
    print("Helios 300 Setup runs on macOS; on this system use the helios-setup CLI or `swift test`.")
#endif
