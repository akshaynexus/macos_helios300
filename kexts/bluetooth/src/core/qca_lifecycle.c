/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Resident-driver policy for the QCA Rome uploader. See qca_lifecycle.h.
 * Freestanding: no libc calls.
 */
#include "qca_lifecycle.h"

static qca_life_action none(void)
{
    qca_life_action a;
    a.run_setup = 0;
    a.cancel_timer = 0;
    a.arm_timer_ms = 0;
    return a;
}

static qca_life_action arm(uint32_t ms)
{
    qca_life_action a = none();
    a.arm_timer_ms = ms ? ms : 1u;
    return a;
}

void qca_life_init(qca_life *l)
{
    l->state = QCA_LS_PENDING;
    l->powered = 1;
    l->attempts = 0;
    l->runs = 0;
    l->successes = 0;
    l->wakes = 0;
    l->recheck = 0;
    l->last_err = QCA_OK;
}

qca_life_action qca_life_on_event(qca_life *l, qca_life_event ev)
{
    qca_life_action a = none();

    if (l->state == QCA_LS_GONE)
        return a;

    switch (ev) {
    case QCA_EV_START:
        if (l->state != QCA_LS_PENDING || l->runs != 0)
            return a; /* only once per instance */
        l->attempts = 0;
        return arm(QCA_LIFE_START_DELAY_MS);

    case QCA_EV_TIMER:
        /* Stale timers (fired after power-off, or after a result already
         * moved us on) do nothing. */
        if (l->state != QCA_LS_PENDING || !l->powered)
            return a;
        l->state = QCA_LS_RUNNING;
        l->attempts++;
        l->runs++;
        a.run_setup = 1;
        return a;

    case QCA_EV_POWER_OFF:
        if (!l->powered)
            return a;
        l->powered = 0;
        a.cancel_timer = 1;
        /* A run in progress finishes (the USB stack fails it if the device
         * really went away); its result is then folded into SLEEPING. */
        if (l->state != QCA_LS_DEAD && l->state != QCA_LS_RUNNING)
            l->state = QCA_LS_SLEEPING;
        return a;

    case QCA_EV_POWER_ON:
        if (l->powered)
            return a; /* initial PM "on" or duplicate: nothing changed */
        l->powered = 1;
        if (l->state == QCA_LS_DEAD) {
            /* A "permanent" error can come from one garbage read (e.g. a
             * version of zeros). Allow a single attempt per wake; a real
             * mismatch fails the same way and stays dead. */
            l->wakes++;
            l->state = QCA_LS_PENDING;
            l->attempts = QCA_LIFE_MAX_ATTEMPTS - 1u;
            return arm(QCA_LIFE_WAKE_DELAY_MS);
        }
        if (l->state == QCA_LS_RUNNING) {
            /* The run straddled a sleep: its result says nothing about the
             * chip after wake, so check again once it is done. */
            l->wakes++;
            l->recheck = 1;
            return a;
        }
        /* Wake: the chip may have lost power without re-enumerating (a
         * re-enumeration gives a new device and a new instance instead).
         * Re-run the setup; it only downloads if the status says so. */
        l->wakes++;
        l->state = QCA_LS_PENDING;
        l->attempts = 0;
        return arm(QCA_LIFE_WAKE_DELAY_MS);

    case QCA_EV_TERMINATE:
        l->state = QCA_LS_GONE;
        a.cancel_timer = 1;
        return a;
    }
    return a;
}

qca_life_action qca_life_on_result(qca_life *l, qca_err err)
{
    qca_life_action a = none();

    if (l->state == QCA_LS_GONE)
        return a;
    if (l->state != QCA_LS_RUNNING)
        return a; /* result without a run: ignore */

    l->last_err = err;
    if (err != QCA_OK && !qca_err_is_transient(err)) {
        int woke_during_run = l->recheck && l->powered;
        l->recheck = 0;
        if (woke_during_run) {
            /* Read while the chip was going down or coming back, so it says
             * nothing about the chip now. The usual one attempt per wake. */
            l->state = QCA_LS_PENDING;
            l->attempts = QCA_LIFE_MAX_ATTEMPTS - 1u;
            return arm(QCA_LIFE_WAKE_DELAY_MS);
        }
        l->state = QCA_LS_DEAD;
        return a;
    }
    if (err == QCA_OK)
        l->successes++;
    if (l->recheck && l->powered) {
        l->recheck = 0;
        l->attempts = 0;
        l->state = QCA_LS_PENDING;
        return arm(QCA_LIFE_WAKE_DELAY_MS);
    }
    l->recheck = 0;
    if (err == QCA_OK) {
        l->attempts = 0;
        l->state = l->powered ? QCA_LS_READY : QCA_LS_SLEEPING;
        return a;
    }
    if (!l->powered) {
        /* Failed because we went to sleep mid-run: retry on wake. */
        l->state = QCA_LS_SLEEPING;
        return a;
    }
    if (l->attempts >= QCA_LIFE_MAX_ATTEMPTS) {
        l->state = QCA_LS_FAILED;
        return a;
    }
    l->state = QCA_LS_PENDING;
    return arm(QCA_LIFE_RETRY_BASE_MS << (l->attempts - 1u));
}

qca_life_action qca_life_on_skipped(qca_life *l)
{
    qca_life_action a = none();

    if (l->state != QCA_LS_RUNNING)
        return a;
    l->last_err = QCA_OK;
    if (l->recheck && l->powered) {
        l->recheck = 0;
        l->attempts = 0;
        l->state = QCA_LS_PENDING;
        return arm(QCA_LIFE_WAKE_DELAY_MS);
    }
    l->recheck = 0;
    l->attempts = 0;
    l->state = l->powered ? QCA_LS_READY : QCA_LS_SLEEPING;
    return a;
}

const char *qca_life_state_name(qca_life_state s)
{
    switch (s) {
    case QCA_LS_PENDING:  return "pending";
    case QCA_LS_RUNNING:  return "running";
    case QCA_LS_READY:    return "ready";
    case QCA_LS_SLEEPING: return "sleeping";
    case QCA_LS_FAILED:   return "failed";
    case QCA_LS_DEAD:     return "dead";
    case QCA_LS_GONE:     return "gone";
    }
    return "unknown";
}
