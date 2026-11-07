/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Resident-driver policy for the QCA Rome uploader: when to (re)run
 * qca_rome_setup(), how often to retry, and what to do across system
 * sleep/wake and hot-unplug. Pure C, no I/O, so the kext and the host tests
 * run exactly the same decisions.
 *
 * The kext feeds events (start, timer fired, power off/on, terminate) and
 * setup results in; the policy answers with what to do next: run the setup
 * now, arm the timer for N ms, or nothing. One timer, one setup at a time.
 *
 * Measured on the Helios 300 under Linux (kexts/bluetooth/evidence/): the
 * chip keeps its rampatch across a warm reboot, a USB port reset, a
 * deauthorize/authorize cycle and a USB port power-off; it loses it on a cold
 * boot and after a firmware crash. So a setup run on every start and every
 * wake is cheap when nothing was lost (four control reads, no download) and
 * repairs the controller when something was.
 */
#ifndef QCA_LIFECYCLE_H
#define QCA_LIFECYCLE_H

#include <stdint.h>

#include "qca_rome.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QCA_LIFE_START_DELAY_MS   10u   /* leave start() before doing I/O     */
#define QCA_LIFE_WAKE_DELAY_MS    800u  /* let the bus settle after wake      */
#define QCA_LIFE_RETRY_BASE_MS    250u  /* 250, 500, 1000 ms between retries  */
#define QCA_LIFE_MAX_ATTEMPTS     4u    /* per start / per wake               */

typedef enum qca_life_state {
    QCA_LS_PENDING = 0,  /* a setup run is due (timer armed or waiting power) */
    QCA_LS_RUNNING,      /* setup in progress                                 */
    QCA_LS_READY,        /* last run succeeded                                */
    QCA_LS_SLEEPING,     /* powered off; re-check on wake                     */
    QCA_LS_FAILED,       /* gave up until the next wake                       */
    QCA_LS_DEAD,         /* permanent error: one attempt per wake, else none  */
    QCA_LS_GONE          /* terminated (unplug / unload): ignore everything   */
} qca_life_state;

typedef enum qca_life_event {
    QCA_EV_START = 0,
    QCA_EV_TIMER,
    QCA_EV_POWER_OFF,
    QCA_EV_POWER_ON,
    QCA_EV_TERMINATE
} qca_life_event;

typedef struct qca_life_action {
    int      run_setup;     /* call qca_rome_setup() now, then qca_life_result */
    int      cancel_timer;  /* cancel a pending timer                          */
    uint32_t arm_timer_ms;  /* arm the timer (0 = do not arm)                  */
} qca_life_action;

typedef struct qca_life {
    qca_life_state state;
    int      powered;
    uint32_t attempts;      /* attempts in the current start/wake round */
    uint32_t runs;          /* total setup runs                         */
    uint32_t successes;
    uint32_t wakes;
    int      recheck;       /* slept and woke while a run was in progress */
    qca_err  last_err;
} qca_life;

void qca_life_init(qca_life *l);
qca_life_action qca_life_on_event(qca_life *l, qca_life_event ev);
/* Report the outcome of a run the policy asked for (run_setup was set). */
qca_life_action qca_life_on_result(qca_life *l, qca_err err);

const char *qca_life_state_name(qca_life_state s);

#ifdef __cplusplus
}
#endif

#endif /* QCA_LIFECYCLE_H */
