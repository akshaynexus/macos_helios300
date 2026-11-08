// SPDX-License-Identifier: GPL-2.0-only
//
// StepPlanner: the checklist and its rules.
//   - Items come from the manifest's post_install, in `order` (ties keep the manifest's order); a step with
//     `app.parts` becomes one item per part (the GPU stack: PG106FB, then the Metal userspace, then PG106Accel),
//     because the project rule is one driver change per restart.
//   - Status is evaluated from the step's declarative probes plus what was done in this boot session.
//   - RestartGuard: a second driver change in the same boot session is refused (the change log is the system
//     state, so the GUI and the CLI enforce the same rule). Taking back that same change is not a second change.
//   - DryRunRule: Install needs a passing Check of the same item, in this boot, against the same manifest.
import Foundation

public struct SetupItem: Equatable {
    public var id: String
    public var stepId: String
    public var title: String
    public var what: String
    public var order: Int
    public var group: String?
    public var choice: Int?
    public var recommended: Bool
    public var script: String?
    public var run: [String: String]
    public var env: [String: String]
    public var reboot: Bool
    public var driverChange: Bool
    public var detect: Detect?
    public var needs: [String]
    public var notes: [String]
    public var artifacts: [String]
    public var undoNote: String?

    /// The command line for an action (check, install, revert, status) as the manifest spells it.
    public func command(_ action: String) -> String? { run[action] }
}

public enum ItemStatus: Equatable {
    case notInstalled
    case installed
    case needsRestart(String)
    case failed(String)
    case notAvailable(String)

    public var label: String {
        switch self {
        case .notInstalled: return "Not installed"
        case .installed: return "Installed"
        case .needsRestart: return "Needs restart"
        case .failed: return "Failed"
        case .notAvailable: return "Not available"
        }
    }

    public var detail: String {
        switch self {
        case .notInstalled, .installed: return ""
        case .needsRestart(let s), .failed(let s), .notAvailable(let s): return s
        }
    }
}

public enum StepPlanner {
    /// The checklist, in order.
    public static func items(_ m: Manifest) -> [SetupItem] {
        let steps = m.postInstall.enumerated().sorted { a, b in
            (a.element.order ?? 0, a.offset) < (b.element.order ?? 0, b.offset)
        }.map(\.element)
        return steps.flatMap(expand)
    }

    static func expand(_ st: PostInstallStep) -> [SetupItem] {
        let base = SetupItem(
            id: st.step, stepId: st.step, title: st.app?.title ?? st.step, what: st.what, order: st.order ?? 0,
            group: st.group, choice: st.choice, recommended: st.recommended ?? false, script: st.script,
            run: st.run ?? [:], env: [:], reboot: st.reboot ?? false,
            driverChange: st.app?.driverChange ?? (st.reboot ?? false), detect: st.app?.detect,
            needs: st.needs ?? [], notes: st.notes ?? [], artifacts: st.artifacts, undoNote: st.app?.undoNote)
        guard let parts = st.app?.parts, !parts.isEmpty else { return [base] }
        return parts.map { part in
            var item = base
            item.id = part.id
            item.title = part.title
            item.env = part.env ?? [:]
            item.reboot = part.reboot ?? base.reboot
            item.driverChange = part.driverChange ?? item.reboot
            item.detect = part.detect ?? base.detect
            item.artifacts = part.artifacts ?? base.artifacts
            return item
        }
    }

    /// The status of every item, evaluated together (group alternatives look at each other).
    public static func statuses(
        _ items: [SetupItem], ctx: ProbeContext, system: SystemState, user: UserState,
        boot: String
    ) -> [String: ItemStatus] {
        var out: [String: ItemStatus] = [:]
        for item in items {
            out[item.id] = status(item, all: items, ctx: ctx, system: system, user: user, boot: boot)
        }
        return out
    }

    public static func status(
        _ item: SetupItem, all: [SetupItem], ctx: ProbeContext, system: SystemState,
        user: UserState, boot: String
    ) -> ItemStatus {
        if let run = user.runs[item.id], run.bootSession == boot, run.status != 0, run.action != "check",
            run.action != "status"
        {
            return .failed("the last \(run.action) failed (exit \(run.status)); its log: \(run.log)")
        }
        if let last = system.changes(inBoot: boot).last(where: { $0.item == item.id }), last.needsRestart {
            return .needsRestart("\(last.action) done in this session: restart to finish it")
        }
        let inst = installState(item, ctx: ctx)
        let active = item.detect?.active.map { ProbeEvaluator.all($0, in: ctx) } ?? inst.all
        switch (inst.all, inst.any) {
        case (true, _) where active: return .installed
        case (true, _): return .failed(item.detect?.failedHint ?? "installed, but not in effect after the restart")
        case (false, true): return .failed("partly installed: \(inst.missing.joined(separator: ", ")) missing")
        default: break
        }
        if let other = all.first(where: {
            $0.id != item.id && $0.group != nil && $0.group == item.group
                && installState($0, ctx: ctx).all
        }) {
            return .notAvailable("\(other.title) is installed: only one \(item.group ?? "") option at a time")
        }
        for p in item.detect?.available ?? [] where ProbeEvaluator.evaluate(p, in: ctx).ok != true {
            return .notAvailable(p.whyNot ?? "\(p.probe) \(p.key ?? p.path ?? p.arg ?? "") is not as required")
        }
        return .notInstalled
    }

    /// (all installed probes true, any true, the ones that are false). No installed probes: nothing installed.
    static func installState(_ item: SetupItem, ctx: ProbeContext) -> (all: Bool, any: Bool, missing: [String]) {
        let probes = item.detect?.installed ?? []
        guard !probes.isEmpty else { return (false, false, []) }
        let results = probes.map { ($0, ProbeEvaluator.evaluate($0, in: ctx).ok == true) }
        let missing = results.filter { !$0.1 }.map { $0.0.path ?? $0.0.bundleId ?? $0.0.key ?? $0.0.probe }
        return (missing.isEmpty, results.contains { $0.1 }, missing)
    }

    /// What to do next: the first item that is not installed and can be done, preferring a group's recommended
    /// option; nil when everything is done or the next one waits for a restart.
    public static func next(_ items: [SetupItem], statuses: [String: ItemStatus]) -> SetupItem? {
        var doneGroups = Set<String>()
        for item in items {
            if let g = item.group, statuses[item.id] == .installed { doneGroups.insert(g) }
        }
        let candidates = items.filter { item in
            statuses[item.id] == .notInstalled && !(item.group.map(doneGroups.contains) ?? false)
        }
        return candidates.first { $0.group == nil || $0.recommended }
            ?? candidates.first
    }
}

public enum GuardDecision: Equatable {
    case allowed
    case restartFirst(String)
    case checkFirst(String)
}

/// One driver change per restart (the project rule since the 2026-11-08 hangs that left no logs).
public enum RestartGuard {
    public static let changeActions: Set<String> = ["install", "revert"]

    public static func check(_ item: SetupItem, action: String, system: SystemState, boot: String) -> GuardDecision {
        guard changeActions.contains(action), item.driverChange else { return .allowed }
        if action == "revert", let last = system.changes(inBoot: boot).last(where: { $0.driverChange }), last.item == item.id,
            last.action == "install"
        {
            return .allowed  // Undo of the install this boot is waiting on
        }
        return checkDriverChange(system: system, boot: boot)
    }

    /// Rolling back an update unit is a driver change, except that it may take back, or finish taking back, the one
    /// change this boot already holds.
    public static func checkRollback(_ record: RollbackRecord, system: SystemState, boot: String) -> GuardDecision {
        let own = ["update:\(record.unit)", record.unit, "rollback:\(record.unit)"]
        if let last = system.changes(inBoot: boot).last(where: { $0.driverChange }), own.contains(last.item) { return .allowed }
        return checkDriverChange(system: system, boot: boot)
    }

    /// For any driver change (a step, an update unit, the OpenCore install).
    public static func checkDriverChange(system: SystemState, boot: String) -> GuardDecision {
        if let prior = system.changes(inBoot: boot).last(where: { $0.driverChange }) {
            return .restartFirst(
                "\(prior.item) (\(prior.action)) was already changed since this Mac started: "
                    + "restart first, one driver change per restart")
        }
        return .allowed
    }
}

/// Install only after a passing dry run of the same item, in this boot, against the same manifest.
public enum DryRunRule {
    public static let maxAge: TimeInterval = 4 * 3600

    public static func check(
        _ item: SetupItem, user: UserState, boot: String, manifestSha256: String,
        now: Date
    ) -> GuardDecision {
        guard item.command("check") != nil else { return .allowed }
        guard let c = user.checks[item.id] else { return .checkFirst("run Check (the dry run) first") }
        if c.bootSession != boot { return .checkFirst("the last Check was before the restart: run it again") }
        if c.status != 0 { return .checkFirst("the last Check failed (exit \(c.status)): fix what it reports first") }
        if c.manifestSha256 != manifestSha256 { return .checkFirst("the release changed since the Check: run it again") }
        if now.timeIntervalSince(c.date) > maxAge { return .checkFirst("the last Check is over 4 hours old: run it again") }
        return .allowed
    }
}
