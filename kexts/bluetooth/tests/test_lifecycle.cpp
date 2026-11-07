// SPDX-License-Identifier: GPL-2.0-or-later
//
// Host tests for the resident-driver policy (src/core/qca_lifecycle.c) and
// for the policy driving the real setup state machine against a simulated
// chip across start, retries, sleep/wake (with and without power loss),
// re-enumeration and hot-unplug — the decisions the kext makes on macOS.
#include "qca_lifecycle.h"
#include "qca_rome.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

namespace {

int g_failures = 0, g_checks = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        ++g_checks;                                                               \
        if (!(c)) {                                                               \
            ++g_failures;                                                         \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);   \
        }                                                                         \
    } while (0)
#define CHECK_EQ(a, b)                                                            \
    do {                                                                          \
        ++g_checks;                                                               \
        auto va_ = (a);                                                           \
        auto vb_ = (b);                                                           \
        if (!(va_ == vb_)) {                                                      \
            ++g_failures;                                                         \
            std::fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n",       \
                         __FILE__, __LINE__, #a, #b, (long long)va_,              \
                         (long long)vb_);                                         \
        }                                                                         \
    } while (0)

// ---- pure policy ---------------------------------------------------------

void test_start_and_success()
{
    std::puts("[policy: start -> timer -> run -> ready; start/power-on once]");
    qca_life l;
    qca_life_init(&l);
    qca_life_action a = qca_life_on_event(&l, QCA_EV_POWER_ON); // initial PM "on"
    CHECK(!a.run_setup && !a.arm_timer_ms);
    a = qca_life_on_event(&l, QCA_EV_START);
    CHECK_EQ(a.arm_timer_ms, QCA_LIFE_START_DELAY_MS);
    CHECK(!a.run_setup);
    a = qca_life_on_event(&l, QCA_EV_TIMER);
    CHECK(a.run_setup);
    CHECK_EQ(l.state, QCA_LS_RUNNING);
    a = qca_life_on_event(&l, QCA_EV_TIMER); // stale/duplicate timer
    CHECK(!a.run_setup);
    a = qca_life_on_result(&l, QCA_OK);
    CHECK(!a.run_setup && !a.arm_timer_ms);
    CHECK_EQ(l.state, QCA_LS_READY);
    CHECK_EQ(l.runs, 1u);
    CHECK_EQ(l.successes, 1u);
    a = qca_life_on_event(&l, QCA_EV_START); // never twice
    CHECK(!a.arm_timer_ms);
    a = qca_life_on_event(&l, QCA_EV_TIMER); // timer while ready: nothing
    CHECK(!a.run_setup);
    a = qca_life_on_result(&l, QCA_OK); // result without a run: ignored
    CHECK_EQ(l.successes, 1u);
}

void test_retry_backoff()
{
    std::puts("[policy: transient errors retry 250/500/1000 ms, then fail until wake]");
    qca_life l;
    qca_life_init(&l);
    qca_life_on_event(&l, QCA_EV_START);
    const uint32_t delays[] = {250, 500, 1000};
    for (int i = 0; i < 3; i++) {
        qca_life_action a = qca_life_on_event(&l, QCA_EV_TIMER);
        CHECK(a.run_setup);
        a = qca_life_on_result(&l, QCA_ERR_BODY_IO);
        CHECK_EQ(a.arm_timer_ms, delays[i]);
        CHECK_EQ(l.state, QCA_LS_PENDING);
    }
    qca_life_action a = qca_life_on_event(&l, QCA_EV_TIMER);
    CHECK(a.run_setup);
    a = qca_life_on_result(&l, QCA_ERR_GET_VERSION_IO);
    CHECK(!a.arm_timer_ms);
    CHECK_EQ(l.state, QCA_LS_FAILED);
    CHECK_EQ(l.runs, QCA_LIFE_MAX_ATTEMPTS);
    // No further runs until a wake; a wake gives a fresh round.
    CHECK(!qca_life_on_event(&l, QCA_EV_TIMER).run_setup);
    qca_life_on_event(&l, QCA_EV_POWER_OFF);
    a = qca_life_on_event(&l, QCA_EV_POWER_ON);
    CHECK_EQ(a.arm_timer_ms, QCA_LIFE_WAKE_DELAY_MS);
    CHECK(qca_life_on_event(&l, QCA_EV_TIMER).run_setup);
    CHECK_EQ(l.attempts, 1u);
    qca_life_on_result(&l, QCA_OK);
    CHECK_EQ(l.state, QCA_LS_READY);
}

void test_permanent_errors()
{
    std::puts("[policy: permanent errors: no retry in the round, exactly one attempt per wake]");
    const qca_err perm[] = {QCA_ERR_UNSUPPORTED_ROM, QCA_ERR_UNSUPPORTED_BOARD,
                            QCA_ERR_RAMPATCH_ROM_MISMATCH, QCA_ERR_RAMPATCH_PATCH_OLDER,
                            QCA_ERR_RAMPATCH_MISSING, QCA_ERR_NVM_TOO_SHORT,
                            QCA_ERR_ROM_CHANGED, QCA_ERR_BAD_ARGUMENT};
    for (qca_err e : perm) {
        CHECK(!qca_err_is_transient(e));
        qca_life l;
        qca_life_init(&l);
        qca_life_on_event(&l, QCA_EV_START);
        qca_life_on_event(&l, QCA_EV_TIMER);
        qca_life_action a = qca_life_on_result(&l, e);
        CHECK(!a.arm_timer_ms);
        CHECK_EQ(l.state, QCA_LS_DEAD);
        CHECK(!qca_life_on_event(&l, QCA_EV_TIMER).run_setup);
        qca_life_on_event(&l, QCA_EV_POWER_OFF);
        a = qca_life_on_event(&l, QCA_EV_POWER_ON);
        CHECK_EQ(a.arm_timer_ms, QCA_LIFE_WAKE_DELAY_MS);
        CHECK(qca_life_on_event(&l, QCA_EV_TIMER).run_setup);
        a = qca_life_on_result(&l, QCA_ERR_GET_VERSION_IO); // even a transient one: no 2nd try
        CHECK(!a.arm_timer_ms);
        CHECK_EQ(l.state, QCA_LS_FAILED);
        qca_life_on_event(&l, QCA_EV_POWER_OFF);
        qca_life_on_event(&l, QCA_EV_POWER_ON);
        CHECK(qca_life_on_event(&l, QCA_EV_TIMER).run_setup);
        qca_life_on_result(&l, e); // real mismatch: dead again
        CHECK_EQ(l.state, QCA_LS_DEAD);
        CHECK_EQ(l.runs, 3u);
    }
    {
        // A one-off garbage read (e.g. zeros -> unsupported ROM) recovers on wake.
        qca_life l;
        qca_life_init(&l);
        qca_life_on_event(&l, QCA_EV_START);
        qca_life_on_event(&l, QCA_EV_TIMER);
        qca_life_on_result(&l, QCA_ERR_UNSUPPORTED_ROM);
        qca_life_on_event(&l, QCA_EV_POWER_OFF);
        qca_life_on_event(&l, QCA_EV_POWER_ON);
        qca_life_on_event(&l, QCA_EV_TIMER);
        qca_life_on_result(&l, QCA_OK);
        CHECK_EQ(l.state, QCA_LS_READY);
    }
    const qca_err trans[] = {QCA_ERR_GET_VERSION_IO, QCA_ERR_GET_VERSION_SHORT,
                             QCA_ERR_CHECK_STATUS_IO, QCA_ERR_CHECK_STATUS_SHORT,
                             QCA_ERR_HEADER_IO, QCA_ERR_HEADER_SHORT, QCA_ERR_BODY_IO,
                             QCA_ERR_BODY_SHORT, QCA_ERR_FINAL_STATUS,
                             QCA_ERR_PREPARE_DOWNLOAD};
    for (qca_err e : trans)
        CHECK(qca_err_is_transient(e));
    CHECK(!qca_err_is_transient(QCA_OK));
}

void test_sleep_wake()
{
    std::puts("[policy: sleep cancels timers, wake re-checks after 800 ms]");
    qca_life l;
    qca_life_init(&l);
    qca_life_on_event(&l, QCA_EV_START);
    qca_life_on_event(&l, QCA_EV_TIMER);
    qca_life_on_result(&l, QCA_OK);
    for (uint32_t i = 1; i <= 3; i++) {
        qca_life_action a = qca_life_on_event(&l, QCA_EV_POWER_OFF);
        CHECK(a.cancel_timer);
        CHECK_EQ(l.state, QCA_LS_SLEEPING);
        CHECK(!qca_life_on_event(&l, QCA_EV_TIMER).run_setup); // stale timer while asleep
        CHECK(!qca_life_on_event(&l, QCA_EV_POWER_OFF).cancel_timer); // duplicate off
        a = qca_life_on_event(&l, QCA_EV_POWER_ON);
        CHECK_EQ(a.arm_timer_ms, QCA_LIFE_WAKE_DELAY_MS);
        CHECK_EQ(l.wakes, i);
        CHECK(qca_life_on_event(&l, QCA_EV_TIMER).run_setup);
        qca_life_on_result(&l, QCA_OK);
        CHECK_EQ(l.state, QCA_LS_READY);
    }
    CHECK_EQ(l.runs, 4u);

    // Sleep while the start timer is still pending: the start run moves to wake.
    qca_life m;
    qca_life_init(&m);
    qca_life_on_event(&m, QCA_EV_START);
    qca_life_on_event(&m, QCA_EV_POWER_OFF);
    CHECK(!qca_life_on_event(&m, QCA_EV_TIMER).run_setup);
    CHECK_EQ(qca_life_on_event(&m, QCA_EV_POWER_ON).arm_timer_ms, QCA_LIFE_WAKE_DELAY_MS);
    CHECK(qca_life_on_event(&m, QCA_EV_TIMER).run_setup);
}

void test_run_straddles_sleep()
{
    std::puts("[policy: a run that straddles sleep/wake is followed by a re-check]");
    qca_life l;
    qca_life_init(&l);
    qca_life_on_event(&l, QCA_EV_START);
    CHECK(qca_life_on_event(&l, QCA_EV_TIMER).run_setup);
    qca_life_on_event(&l, QCA_EV_POWER_OFF);
    CHECK_EQ(l.state, QCA_LS_RUNNING);
    qca_life_on_event(&l, QCA_EV_POWER_ON);
    qca_life_action a = qca_life_on_result(&l, QCA_OK);
    CHECK_EQ(a.arm_timer_ms, QCA_LIFE_WAKE_DELAY_MS);
    CHECK_EQ(l.state, QCA_LS_PENDING);
    CHECK(qca_life_on_event(&l, QCA_EV_TIMER).run_setup);

    // A run that fails because the machine went to sleep under it: retried on
    // wake, not counted against the retry budget of the sleeping round.
    qca_life m;
    qca_life_init(&m);
    qca_life_on_event(&m, QCA_EV_START);
    qca_life_on_event(&m, QCA_EV_TIMER);
    qca_life_on_event(&m, QCA_EV_POWER_OFF);
    a = qca_life_on_result(&m, QCA_ERR_BODY_IO);
    CHECK(!a.arm_timer_ms);
    CHECK_EQ(m.state, QCA_LS_SLEEPING);
    CHECK_EQ(qca_life_on_event(&m, QCA_EV_POWER_ON).arm_timer_ms, QCA_LIFE_WAKE_DELAY_MS);
}

void test_terminate()
{
    std::puts("[policy: terminate (unplug/unload) silences everything]");
    qca_life l;
    qca_life_init(&l);
    qca_life_on_event(&l, QCA_EV_START);
    qca_life_on_event(&l, QCA_EV_TIMER);
    qca_life_action a = qca_life_on_event(&l, QCA_EV_TERMINATE);
    CHECK(a.cancel_timer);
    CHECK_EQ(l.state, QCA_LS_GONE);
    a = qca_life_on_result(&l, QCA_ERR_BODY_IO); // the aborted run reports back
    CHECK(!a.arm_timer_ms && !a.run_setup);
    const qca_life_event evs[] = {QCA_EV_START, QCA_EV_TIMER, QCA_EV_POWER_OFF,
                                  QCA_EV_POWER_ON, QCA_EV_TERMINATE};
    for (qca_life_event ev : evs) {
        a = qca_life_on_event(&l, ev);
        CHECK(!a.arm_timer_ms && !a.run_setup && !a.cancel_timer);
    }
    CHECK_EQ(l.state, QCA_LS_GONE);
    CHECK(std::strcmp(qca_life_state_name(QCA_LS_GONE), "gone") == 0);
    for (int s = QCA_LS_PENDING; s <= QCA_LS_GONE; s++)
        CHECK(std::strcmp(qca_life_state_name(qca_life_state(s)), "unknown") != 0);
}

// ---- policy + real setup against a simulated chip --------------------------

// Rome 3.2 as measured on the Helios: ROM patch 0x111, rampatch 0x3e8,
// status 0x20 at ROM, 0xe0 when patched + NVM applied.
struct Chip {
    bool patched = false, nvm = false, present = true;
    int downloads = 0;
    uint32_t ctrl_reads = 0;
    int fail_next_bulk = 0;     // transient bulk errors to inject
    uint32_t expect = 0, got = 0;
    bool in_rampatch = false;

    static int in(void *c, uint8_t req, void *buf, uint16_t len, uint32_t *act, uint32_t)
    {
        Chip *d = static_cast<Chip *>(c);
        *act = 0;
        if (!d->present)
            return int(0xe00002c0); // kIOReturnNoDevice
        d->ctrl_reads++;
        uint8_t *b = static_cast<uint8_t *>(buf);
        if (req == QCA_REQ_GET_TARGET_VERSION && len == 20) {
            std::memset(b, 0, 20);
            uint32_t patch = d->patched ? 0x3e8 : 0x111;
            b[0] = 0x02; b[1] = 0x03;
            b[4] = uint8_t(patch); b[5] = uint8_t(patch >> 8);
            b[12] = 0x07;
            *act = 20;
            return 0;
        }
        if (req == QCA_REQ_CHECK_STATUS && len == 1) {
            b[0] = uint8_t(0x20 | (d->patched ? 0x80 : 0) | (d->nvm ? 0x40 : 0));
            *act = 1;
            return 0;
        }
        return -1;
    }
    static int out(void *c, uint8_t, const void *, uint16_t len, uint32_t *act, uint32_t)
    {
        Chip *d = static_cast<Chip *>(c);
        *act = 0;
        if (!d->present)
            return int(0xe00002c0);
        d->in_rampatch = (len == QCA_ROME32_RAMPATCH_HDR);
        d->expect = d->in_rampatch ? 68644u : 1998u;
        d->got = len;
        *act = len;
        return 0;
    }
    static int bulk(void *c, uint8_t, const void *, uint32_t len, uint32_t *act, uint32_t)
    {
        Chip *d = static_cast<Chip *>(c);
        *act = 0;
        if (!d->present)
            return int(0xe00002c0);
        if (d->fail_next_bulk > 0) {
            d->fail_next_bulk--;
            d->got = 0; // the chip discards a broken download
            return int(0xe00002d6); // kIOReturnTimeout
        }
        d->got += len;
        *act = len;
        if (d->got == d->expect) {
            d->downloads++;
            if (d->in_rampatch)
                d->patched = true;
            else
                d->nvm = true;
        }
        return 0;
    }
    static void nap(void *, uint32_t) {}
    void power_loss() { patched = nvm = false; }
};

struct SimKext {
    Chip chip;
    qca_life life;
    std::vector<uint8_t> rp, nv;
    uint32_t timer_ms = 0; // armed timer (0 = none)

    SimKext() : rp(68644), nv(1998)
    {
        for (size_t i = 0; i < rp.size(); i++)
            rp[i] = uint8_t(i * 13 + 1);
        rp[16] = 0x08; rp[17] = 0; rp[18] = 0x02; rp[19] = 0x03; rp[20] = 0xe8; rp[21] = 0x03;
        qca_life_init(&life);
    }
    void apply(qca_life_action a)
    {
        for (;;) {
            if (a.cancel_timer)
                timer_ms = 0;
            if (a.arm_timer_ms)
                timer_ms = a.arm_timer_ms;
            if (!a.run_setup)
                return;
            qca_transport t{};
            t.ctx = &chip;
            t.control_in = Chip::in;
            t.control_out = Chip::out;
            t.bulk_out = Chip::bulk;
            t.sleep_ms = Chip::nap;
            qca_firmware f1{rp.data(), rp.size()}, f2{nv.data(), nv.size()};
            qca_result r;
            qca_err e = qca_rome_setup(&t, &f1, &f2, nullptr, &r);
            a = qca_life_on_result(&life, e);
        }
    }
    void event(qca_life_event ev) { apply(qca_life_on_event(&life, ev)); }
    // Fire the armed timer until none is armed (time passes).
    void settle()
    {
        for (int i = 0; i < 20 && timer_ms; i++) {
            timer_ms = 0;
            event(QCA_EV_TIMER);
        }
    }
    bool chip_ready() const { return chip.patched && chip.nvm; }
};

void test_sim_cold_boot_and_warm_wake()
{
    std::puts("[sim: cold boot downloads once; wake with power kept = 4 reads, no download]");
    SimKext k;
    k.event(QCA_EV_POWER_ON);
    k.event(QCA_EV_START);
    k.settle();
    CHECK(k.chip_ready());
    CHECK_EQ(k.chip.downloads, 2);
    CHECK_EQ(k.life.state, QCA_LS_READY);
    uint32_t reads = k.chip.ctrl_reads;
    k.event(QCA_EV_POWER_OFF);
    k.event(QCA_EV_POWER_ON);
    k.settle();
    CHECK_EQ(k.chip.downloads, 2);
    CHECK_EQ(k.chip.ctrl_reads - reads, 4u);
    CHECK_EQ(k.life.state, QCA_LS_READY);
}

void test_sim_wake_after_power_loss()
{
    std::puts("[sim: 20 sleep cycles, power lost on some: re-download exactly then]");
    SimKext k;
    k.event(QCA_EV_START);
    k.settle();
    int losses = 0;
    for (int i = 0; i < 20; i++) {
        k.event(QCA_EV_POWER_OFF);
        if (i % 3 == 1) {
            k.chip.power_loss();
            losses++;
        }
        k.event(QCA_EV_POWER_ON);
        k.settle();
        CHECK(k.chip_ready());
        CHECK_EQ(k.life.state, QCA_LS_READY);
    }
    CHECK_EQ(k.chip.downloads, 2 + 2 * losses);
    CHECK_EQ(k.life.wakes, 20u);
}

void test_sim_transient_bulk_errors()
{
    std::puts("[sim: two failed downloads are retried and the third succeeds]");
    SimKext k;
    k.chip.fail_next_bulk = 2;
    k.event(QCA_EV_START);
    k.settle();
    CHECK(k.chip_ready());
    CHECK_EQ(k.life.runs, 3u);
    CHECK_EQ(k.life.state, QCA_LS_READY);

    std::puts("[sim: device that keeps failing gives up after 4, recovers on wake]");
    SimKext b;
    b.chip.fail_next_bulk = 100;
    b.event(QCA_EV_START);
    b.settle();
    CHECK_EQ(b.life.state, QCA_LS_FAILED);
    CHECK_EQ(b.life.runs, QCA_LIFE_MAX_ATTEMPTS);
    CHECK(!b.chip_ready());
    b.chip.fail_next_bulk = 0;
    b.event(QCA_EV_POWER_OFF);
    b.event(QCA_EV_POWER_ON);
    b.settle();
    CHECK(b.chip_ready());
    CHECK_EQ(b.life.state, QCA_LS_READY);
}

void test_sim_unplug_mid_download()
{
    std::puts("[sim: hot-unplug during a download: no retry, no timer, no I/O]");
    SimKext k;
    k.event(QCA_EV_START);
    // Unplug arrives while the run is in progress: willTerminate sets GONE
    // first (gated), the run's transfers fail with kIOReturnNoDevice.
    qca_life_action a = qca_life_on_event(&k.life, QCA_EV_TIMER);
    CHECK(a.run_setup);
    k.chip.present = false;
    qca_life_on_event(&k.life, QCA_EV_TERMINATE);
    qca_transport t{};
    t.ctx = &k.chip;
    t.control_in = Chip::in;
    t.control_out = Chip::out;
    t.bulk_out = Chip::bulk;
    t.sleep_ms = Chip::nap;
    qca_firmware f1{k.rp.data(), k.rp.size()}, f2{k.nv.data(), k.nv.size()};
    qca_result r;
    qca_err e = qca_rome_setup(&t, &f1, &f2, nullptr, &r);
    CHECK_EQ(e, QCA_ERR_GET_VERSION_IO);
    CHECK_EQ((uint32_t)r.transport_rc, 0xe00002c0u);
    a = qca_life_on_result(&k.life, e);
    CHECK(!a.arm_timer_ms && !a.run_setup);
    CHECK_EQ(k.life.state, QCA_LS_GONE);
    // Re-enumeration = a new device and a new instance with fresh policy.
    SimKext k2;
    k2.chip = k.chip;
    k2.chip.present = true;
    k2.event(QCA_EV_START);
    k2.settle();
    CHECK(k2.chip_ready());
}

} // namespace

int main()
{
    test_start_and_success();
    test_retry_backoff();
    test_permanent_errors();
    test_sleep_wake();
    test_run_straddles_sleep();
    test_terminate();
    test_sim_cold_boot_and_warm_wake();
    test_sim_wake_after_power_loss();
    test_sim_transient_bulk_errors();
    test_sim_unplug_mid_download();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
