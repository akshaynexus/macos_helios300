// SPDX-License-Identifier: GPL-2.0-only
//
// UpdatePlanner: turns a manifest diff into units of work and batches, with the project's restart rule.
//   - One unit per changed artifact, except the members of an `update_groups` entry: `always` -> one unit;
//     `abi-change` -> one unit when the group's `abi` differs between the installed and the new release.
//     A unit may span install kinds (the PG106 group: an OpenCore kext and two /Library/Extensions kexts): the applier
//     does all of its `parts`, or none. `kind` is the unit's rank in the order, not a promise that it is the only one.
//   - An artifact the new release no longer ships is a removal unit of its own.
//   - A changed config_delta (DeviceProperties, NVRAM, boot-args, ACPI) is its own unit; a kext's own Kernel > Add
//     entry travels with the kext.
//   - Units that need no restart (scripts, docs, the Metal userspace when its manifest says so, this app) form the
//     first batch; every unit that needs a restart is a batch of its own, in OpenCore order. "Update all" applies
//     batch after batch, one restart apart.
import Foundation

public struct UpdateUnit: Equatable {
    public enum Kind: String, Codable { case files, app, userspace, libraryExtensions, opencore, config }

    public var id: String
    public var title: String
    public var kind: Kind
    public var artifacts: [String]
    public var requiresRestart: Bool
    /// The artifacts are gone from the release (read them in the old manifest): the unit takes them away.
    public var removal = false
}

public struct UpdatePlan: Equatable {
    public var fromTag: String?
    public var toTag: String
    public var units: [UpdateUnit]
    /// Units applied together; a batch whose units need a restart holds exactly one unit.
    public var batches: [[UpdateUnit]]

    public var isEmpty: Bool { units.isEmpty }
}

public enum UpdatePlanner {
    public static func plan(old: Manifest?, new: Manifest, diff: ManifestDiff) -> UpdatePlan {
        let changed = Set(diff.names(.changed) + diff.names(.added))
        var units: [UpdateUnit] = []
        var grouped = Set<String>()
        for g in new.updateGroups ?? [] where groupTogether(g, old: old) {
            let members = g.members.filter(changed.contains)
            guard members.count > 1 else { continue }
            units.append(
                unit(
                    id: "group-\(g.name)", title: g.what ?? g.members.joined(separator: " + "),
                    artifacts: members, manifest: new))
            grouped.formUnion(members)
        }
        for a in new.artifacts where changed.contains(a.name) && !grouped.contains(a.name) {
            units.append(unit(id: a.name, title: a.name, artifacts: [a.name], manifest: new))
        }
        let dropped = diff.names(.removed).compactMap { old?.artifact($0) }.filter { kind($0) != .app }
        for a in dropped {
            units.append(
                UpdateUnit(
                    id: "remove-\(a.name)", title: "Remove \(a.name)", kind: kind(a), artifacts: [a.name],
                    requiresRestart: requiresRestart(a), removal: true))
        }
        if diff.configChanged {
            units.append(
                UpdateUnit(
                    id: "config", title: "OpenCore config.plist (DeviceProperties, NVRAM, boot-args, ACPI)",
                    kind: .config, artifacts: [], requiresRestart: true))
        }
        units.sort { rank($0, new) < rank($1, new) }
        let quick = units.filter { !$0.requiresRestart }
        let slow = units.filter(\.requiresRestart)
        let batches = (quick.isEmpty ? [] : [quick]) + slow.map { [$0] }
        return UpdatePlan(fromTag: old?.release?.tag, toTag: new.release?.tag ?? "?", units: units, batches: batches)
    }

    /// Whether a group's changed members must go in one unit.
    static func groupTogether(_ g: UpdateGroup, old: Manifest?) -> Bool {
        switch g.together {
        case "always": return true
        case "abi-change":
            let before = old?.updateGroups?.first { $0.name == g.name }?.abi
            return before != g.abi
        default: return true
        }
    }

    static func unit(id: String, title: String, artifacts names: [String], manifest m: Manifest) -> UpdateUnit {
        let arts = names.compactMap(m.artifact)
        let kinds = arts.map(kind)
        let kind =
            kinds.contains(.opencore)
            ? .opencore
            : kinds.contains(.libraryExtensions)
                ? .libraryExtensions
                : kinds.first ?? .files
        return UpdateUnit(
            id: id, title: title, kind: kind, artifacts: names,
            requiresRestart: arts.contains(where: requiresRestart))
    }

    /// A unit's artifacts by install kind, each kind's names in the unit's order. Artifacts the new manifest does not
    /// know stay under `.files`.
    public static func parts(_ unit: UpdateUnit, manifest m: Manifest) -> [UpdateUnit.Kind: [String]] {
        Dictionary(grouping: unit.artifacts) { m.artifact($0).map(kind) ?? .files }
    }

    public static func kind(_ a: Artifact) -> UpdateUnit.Kind {
        switch a.install {
        case "opencore": return .opencore
        case "library-extensions": return .libraryExtensions
        case "gpu-bundles": return .userspace
        case "applications": return .app
        default: return .files
        }
    }

    /// The manifest says; without the field, OpenCore and /Library/Extensions changes need a restart.
    public static func requiresRestart(_ a: Artifact) -> Bool {
        a.requiresRestart ?? ["opencore", "library-extensions"].contains(a.install)
    }

    /// No-restart units first (the app before the files it carries), then config, then OpenCore order, then
    /// /Library/Extensions in manifest order.
    static func rank(_ u: UpdateUnit, _ m: Manifest) -> (Int, Int, Int) {
        let base: Int
        switch (u.requiresRestart, u.kind) {
        case (false, .app): base = 0
        case (false, _): base = 1
        case (true, .config): base = 2
        case (true, .opencore): base = 3
        default: base = 4
        }
        let arts = u.artifacts.compactMap(m.artifact)
        let ocOrder = arts.compactMap { $0.opencore?.order ?? $0.acpi?.order }.min() ?? Int.max
        let index = u.artifacts.compactMap { n in m.artifacts.firstIndex { $0.name == n } }.min() ?? Int.max
        return (base, ocOrder, index)
    }

    /// The next batch to apply, given the units already applied for this release.
    public static func nextBatch(_ plan: UpdatePlan, applied: Set<String>) -> [UpdateUnit]? {
        plan.batches.first { batch in !batch.allSatisfy { applied.contains($0.id) } }
            .map { $0.filter { !applied.contains($0.id) } }
    }
}
