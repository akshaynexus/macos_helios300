// SPDX-License-Identifier: GPL-2.0-or-later
//
// Host tests for src/core/qca_hci_filter.c: the SCO command translation, the
// matching of answers, and the HCI command watchdog. Expected legacy commands
// are written out byte by byte from the Core specification's field order.
#include "qca_hci_filter.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
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

using Bytes = std::vector<uint8_t>;

void put16(Bytes &b, uint16_t v)
{
    b.push_back(uint8_t(v));
    b.push_back(uint8_t(v >> 8));
}

void put32(Bytes &b, uint32_t v)
{
    put16(b, uint16_t(v));
    put16(b, uint16_t(v >> 16));
}

void coding(Bytes &b, uint8_t id)
{
    b.push_back(id);
    put16(b, 0);
    put16(b, 0);
}

struct Enh {
    uint32_t tx_bw = 0x1f40, rx_bw = 0x1f40;
    uint8_t tx_air = QCA_CODEC_CVSD, rx_air = QCA_CODEC_CVSD;
    uint16_t tx_frame = 60, rx_frame = 60;
    uint32_t in_bw = 16000, out_bw = 16000;
    uint8_t in_coding = QCA_CODEC_LINEAR_PCM, out_coding = QCA_CODEC_LINEAR_PCM;
    uint16_t in_coded = 16, out_coded = 16;
    uint8_t in_fmt = 2, out_fmt = 2, in_msb = 0, out_msb = 0;
    uint8_t in_path = 0, out_path = 0, in_tu = 16, out_tu = 16;
    uint16_t latency = 0x000d, pkt = 0x0380;
    uint8_t retrans = 1;

    void body(Bytes &b) const
    {
        put32(b, tx_bw);
        put32(b, rx_bw);
        coding(b, tx_air);
        coding(b, rx_air);
        put16(b, tx_frame);
        put16(b, rx_frame);
        put32(b, in_bw);
        put32(b, out_bw);
        coding(b, in_coding);
        coding(b, out_coding);
        put16(b, in_coded);
        put16(b, out_coded);
        b.push_back(in_fmt);
        b.push_back(out_fmt);
        b.push_back(in_msb);
        b.push_back(out_msb);
        b.push_back(in_path);
        b.push_back(out_path);
        b.push_back(in_tu);
        b.push_back(out_tu);
        put16(b, latency);
        put16(b, pkt);
        b.push_back(retrans);
    }
    Bytes setup(uint16_t handle = 0x0042) const
    {
        Bytes b;
        put16(b, QCA_HCI_OP_ENH_SETUP_SYNC);
        b.push_back(QCA_HCI_ENH_SETUP_PLEN);
        put16(b, handle);
        body(b);
        return b;
    }
    Bytes accept() const
    {
        Bytes b;
        put16(b, QCA_HCI_OP_ENH_ACCEPT_SYNC);
        b.push_back(QCA_HCI_ENH_ACCEPT_PLEN);
        for (uint8_t i = 1; i <= 6; i++)
            b.push_back(i);
        body(b);
        return b;
    }
};

qca_sco_result translate(const Bytes &cmd, Bytes &out, uint16_t *voice, qca_sco_why *why)
{
    out.assign(QCA_SCO_LEGACY_MAX, 0xAA);
    uint32_t n = 0;
    qca_sco_result r = qca_sco_translate(cmd.data(), uint32_t(cmd.size()), out.data(),
                                         uint32_t(out.size()), &n, voice, why);
    if (r == QCA_SCO_TRANSLATED)
        out.resize(n);
    return r;
}

void test_lengths()
{
    std::puts("[translate: fixture lengths are the specification's]");
    CHECK_EQ(Enh().setup().size(), size_t(3 + 59));
    CHECK_EQ(Enh().accept().size(), size_t(3 + 63));
}

void test_setup_cvsd()
{
    std::puts("[translate: Enhanced Setup, CVSD over HCI -> Setup, byte for byte]");
    Bytes out;
    uint16_t v = 0;
    qca_sco_why why = QCA_SCO_WHY_LENGTH;
    CHECK_EQ(translate(Enh().setup(), out, &v, &why), QCA_SCO_TRANSLATED);
    CHECK_EQ(why, QCA_SCO_WHY_NONE);
    CHECK_EQ(v, 0x0060);
    const Bytes want = {0x28, 0x04, 0x11,             // Setup Synchronous Connection, 17 bytes
                        0x42, 0x00,                   // Connection_Handle
                        0x40, 0x1f, 0x00, 0x00,       // Transmit_Bandwidth
                        0x40, 0x1f, 0x00, 0x00,       // Receive_Bandwidth
                        0x0d, 0x00,                   // Max_Latency
                        0x60, 0x00,                   // Voice_Setting
                        0x01,                         // Retransmission_Effort
                        0x80, 0x03};                  // Packet_Type
    CHECK(out == want);
}

void test_accept()
{
    std::puts("[translate: Enhanced Accept -> Accept, BD_ADDR kept]");
    Enh e;
    e.tx_bw = 0x11223344;
    e.rx_bw = 0x55667788;
    e.latency = 0xABCD;
    e.pkt = 0x1234;
    e.retrans = 2;
    Bytes out;
    uint16_t v = 0;
    CHECK_EQ(translate(e.accept(), out, &v, nullptr), QCA_SCO_TRANSLATED);
    const Bytes want = {0x29, 0x04, 0x15, 1, 2, 3, 4, 5, 6, 0x44, 0x33, 0x22, 0x11,
                        0x88, 0x77, 0x66, 0x55, 0xCD, 0xAB, 0x60, 0x00, 0x02, 0x34, 0x12};
    CHECK(out == want);
}

void test_transparent()
{
    std::puts("[translate: host-coded streams go as transparent; a controller codec cannot]");
    uint16_t v = 0;
    Bytes out;
    qca_sco_why why;

    // Transparent air + transparent host (what Linux calls BT_CODEC_TRANSPARENT).
    Enh t;
    t.tx_air = t.rx_air = QCA_CODEC_TRANSPARENT;
    t.in_coding = t.out_coding = QCA_CODEC_TRANSPARENT;
    t.in_fmt = t.out_fmt = 0;
    CHECK_EQ(translate(t.setup(), out, &v, &why), QCA_SCO_TRANSLATED);
    CHECK_EQ(v, QCA_VOICE_TRANSPARENT);

    // mSBC on air, encoded by the host and carried transparently.
    Enh m = t;
    m.tx_air = m.rx_air = QCA_CODEC_MSBC;
    CHECK_EQ(translate(m.setup(), out, &v, &why), QCA_SCO_TRANSLATED);
    CHECK_EQ(v, QCA_VOICE_TRANSPARENT);

    // mSBC on air with PCM from the host needs a codec in the controller:
    // sending that PCM as transparent data would be noise on the air.
    Enh c;
    c.tx_air = c.rx_air = QCA_CODEC_MSBC;
    CHECK_EQ(translate(c.setup(), out, &v, &why), QCA_SCO_REFUSED);
    CHECK_EQ(why, QCA_SCO_WHY_HOST_CODING);

    // Transparent air with PCM from the host, and CVSD air with transparent
    // host data, are likewise not what the legacy command means.
    Enh d;
    d.tx_air = d.rx_air = QCA_CODEC_TRANSPARENT;
    CHECK_EQ(translate(d.setup(), out, &v, &why), QCA_SCO_REFUSED);
    CHECK_EQ(why, QCA_SCO_WHY_HOST_CODING);
    Enh f;
    f.in_coding = f.out_coding = QCA_CODEC_TRANSPARENT;
    CHECK_EQ(translate(f.setup(), out, &v, &why), QCA_SCO_REFUSED);
    CHECK_EQ(why, QCA_SCO_WHY_HOST_CODING);
}

void test_voice_settings()
{
    std::puts("[translate: Voice_Setting bits]");
    struct Case {
        uint8_t air, host, fmt, msb;
        uint16_t coded, voice;
    } cases[] = {
        {QCA_CODEC_CVSD, QCA_CODEC_LINEAR_PCM, 2, 0, 16, 0x0060},
        {QCA_CODEC_CVSD, QCA_CODEC_LINEAR_PCM, 2, 0, 8, 0x0040},
        {QCA_CODEC_CVSD, QCA_CODEC_LINEAR_PCM, 1, 0, 16, 0x0020},
        {QCA_CODEC_CVSD, QCA_CODEC_LINEAR_PCM, 3, 0, 16, 0x00A0},
        {QCA_CODEC_CVSD, QCA_CODEC_LINEAR_PCM, 4, 0, 16, 0x00E0},
        {QCA_CODEC_CVSD, QCA_CODEC_LINEAR_PCM, 2, 3, 16, 0x006C},
        {QCA_CODEC_ULAW, QCA_CODEC_LINEAR_PCM, 2, 0, 16, 0x0061},
        {QCA_CODEC_ALAW, QCA_CODEC_LINEAR_PCM, 2, 0, 16, 0x0062},
        {QCA_CODEC_CVSD, QCA_CODEC_ULAW, 0, 0, 8, 0x0100},
        {QCA_CODEC_CVSD, QCA_CODEC_ALAW, 0, 0, 8, 0x0200},
    };
    for (const Case &c : cases) {
        Enh e;
        e.tx_air = e.rx_air = c.air;
        e.in_coding = e.out_coding = c.host;
        e.in_fmt = e.out_fmt = c.fmt;
        e.in_msb = e.out_msb = c.msb;
        e.in_coded = e.out_coded = c.coded;
        Bytes out;
        uint16_t v = 0xFFFF;
        CHECK_EQ(translate(e.setup(), out, &v, nullptr), QCA_SCO_TRANSLATED);
        CHECK_EQ(v, c.voice);
    }
}

void refused(const Enh &e, qca_sco_why expect)
{
    for (const Bytes &cmd : {e.setup(), e.accept()}) {
        Bytes out;
        qca_sco_why why = QCA_SCO_WHY_NONE;
        CHECK_EQ(translate(cmd, out, nullptr, &why), QCA_SCO_REFUSED);
        CHECK_EQ(why, expect);
        // Refused means the caller's buffer is left alone.
        bool untouched = true;
        for (uint8_t x : out)
            untouched = untouched && x == 0xAA;
        CHECK(untouched);
    }
}

void test_refusals()
{
    std::puts("[translate: what the legacy command cannot say is refused, not guessed]");
    Enh a;
    a.rx_air = QCA_CODEC_TRANSPARENT;
    refused(a, QCA_SCO_WHY_ASYMMETRIC);
    Enh b;
    b.out_coding = QCA_CODEC_ULAW;
    refused(b, QCA_SCO_WHY_ASYMMETRIC);
    Enh c;
    c.out_coded = 8;
    refused(c, QCA_SCO_WHY_ASYMMETRIC);
    Enh d;
    d.out_fmt = 1;
    refused(d, QCA_SCO_WHY_ASYMMETRIC);
    Enh e;
    e.out_msb = 2;
    refused(e, QCA_SCO_WHY_ASYMMETRIC);
    Enh f;
    f.tx_air = f.rx_air = 0xFF;
    refused(f, QCA_SCO_WHY_VENDOR_CODEC);
    Enh g;
    g.in_coding = g.out_coding = 0xFF;
    refused(g, QCA_SCO_WHY_VENDOR_CODEC);
    Enh h;
    h.in_path = h.out_path = 1;
    refused(h, QCA_SCO_WHY_DATA_PATH);
    Enh i;
    i.out_path = 1;
    refused(i, QCA_SCO_WHY_DATA_PATH);
    Enh j;
    j.in_coded = j.out_coded = 24;
    refused(j, QCA_SCO_WHY_HOST_CODING);
    Enh k;
    k.in_fmt = k.out_fmt = 0;
    refused(k, QCA_SCO_WHY_HOST_CODING);
    Enh l;
    l.in_fmt = l.out_fmt = 5;
    refused(l, QCA_SCO_WHY_HOST_CODING);
    Enh m;
    m.in_msb = m.out_msb = 8;
    refused(m, QCA_SCO_WHY_HOST_CODING);

    std::puts("[translate: malformed commands]");
    Bytes out;
    qca_sco_why why = QCA_SCO_WHY_NONE;
    Bytes shortcmd = Enh().setup();
    shortcmd.pop_back();
    CHECK_EQ(translate(shortcmd, out, nullptr, &why), QCA_SCO_REFUSED);
    CHECK_EQ(why, QCA_SCO_WHY_LENGTH);
    Bytes longcmd = Enh().setup();
    longcmd.push_back(0);
    CHECK_EQ(translate(longcmd, out, nullptr, &why), QCA_SCO_REFUSED);
    CHECK_EQ(why, QCA_SCO_WHY_LENGTH);
    Bytes badplen = Enh().setup();
    badplen[2] = 58;
    CHECK_EQ(translate(badplen, out, nullptr, &why), QCA_SCO_REFUSED);
    CHECK_EQ(why, QCA_SCO_WHY_LENGTH);

    std::puts("[translate: output buffer too small]");
    Bytes cmd = Enh().setup();
    uint8_t tiny[QCA_HCI_CMD_HDR + QCA_HCI_SETUP_PLEN - 1];
    std::memset(tiny, 0xAA, sizeof(tiny));
    uint32_t n = 77;
    CHECK_EQ(qca_sco_translate(cmd.data(), uint32_t(cmd.size()), tiny, sizeof(tiny), &n, nullptr, &why),
             QCA_SCO_REFUSED);
    CHECK_EQ(why, QCA_SCO_WHY_BUFFER);
    CHECK_EQ(n, 77u);
    CHECK_EQ(tiny[0], 0xAA);
    CHECK_EQ(qca_sco_translate(cmd.data(), uint32_t(cmd.size()), nullptr, 64, &n, nullptr, &why),
             QCA_SCO_REFUSED);
}

void test_pass()
{
    std::puts("[translate: everything else passes]");
    const uint16_t ops[] = {0x0000, QCA_HCI_OP_SETUP_SYNC, QCA_HCI_OP_ACCEPT_SYNC,
                            QCA_HCI_OP_RESET, 0xFC00, 0x043C, 0x043F};
    for (uint16_t op : ops) {
        Bytes cmd;
        put16(cmd, op);
        cmd.push_back(0);
        Bytes out;
        qca_sco_why why = QCA_SCO_WHY_LENGTH;
        CHECK_EQ(translate(cmd, out, nullptr, &why), QCA_SCO_PASS);
        CHECK_EQ(why, QCA_SCO_WHY_NONE);
    }
    uint8_t b[3] = {0x3D, 0x04, 0};
    uint8_t out[32];
    CHECK_EQ(qca_sco_translate(b, 2, out, sizeof(out), nullptr, nullptr, nullptr), QCA_SCO_PASS);
    CHECK_EQ(qca_sco_translate(nullptr, 0, out, sizeof(out), nullptr, nullptr, nullptr), QCA_SCO_PASS);
    for (uint32_t w = 0; w <= uint32_t(QCA_SCO_WHY_BUFFER); w++)
        CHECK(std::strcmp(qca_sco_why_name(qca_sco_why(w)), "unknown") != 0);
    CHECK(std::strcmp(qca_sco_why_name(qca_sco_why(99)), "unknown") == 0);
}

Bytes status(uint16_t op, uint8_t st = 0)
{
    return {QCA_HCI_EVT_CMD_STATUS, 4, st, 1, uint8_t(op), uint8_t(op >> 8)};
}

Bytes complete(uint16_t op)
{
    return {QCA_HCI_EVT_CMD_COMPLETE, 4, 1, uint8_t(op), uint8_t(op >> 8), 0};
}

uint16_t opcode_at(const Bytes &e, size_t at)
{
    return uint16_t(e[at] | (e[at + 1] << 8));
}

uint32_t filter(qca_sco_pending &p, Bytes &e, uint32_t *replies = nullptr, uint32_t *events = nullptr)
{
    return qca_sco_filter_events(&p, e.data(), uint32_t(e.size()), replies, events);
}

void test_answers()
{
    std::puts("[answers: the host sees the opcode it sent]");
    qca_sco_pending p;
    qca_sco_pending_init(&p);
    qca_sco_note_sent(&p, QCA_HCI_OP_SETUP_SYNC, 1);
    Bytes e = status(QCA_HCI_OP_SETUP_SYNC);
    uint32_t replies = 0, events = 0;
    CHECK_EQ(filter(p, e, &replies, &events), 1u);
    CHECK_EQ(opcode_at(e, 4), QCA_HCI_OP_ENH_SETUP_SYNC);
    CHECK_EQ(replies, 1u);
    CHECK_EQ(events, 1u);
    CHECK_EQ(p.count, 0u);

    // A second answer with nothing pending is left as it came.
    Bytes again = status(QCA_HCI_OP_SETUP_SYNC);
    CHECK_EQ(filter(p, again), 0u);
    CHECK_EQ(opcode_at(again, 4), QCA_HCI_OP_SETUP_SYNC);

    // Status is preserved, so an error reaches the host unchanged.
    qca_sco_note_sent(&p, QCA_HCI_OP_ACCEPT_SYNC, 1);
    Bytes bad = status(QCA_HCI_OP_ACCEPT_SYNC, 0x0C);
    CHECK_EQ(filter(p, bad), 1u);
    CHECK_EQ(opcode_at(bad, 4), QCA_HCI_OP_ENH_ACCEPT_SYNC);
    CHECK_EQ(bad[2], 0x0C);

    // Command Complete form, which a controller may use to reject a command.
    qca_sco_note_sent(&p, QCA_HCI_OP_SETUP_SYNC, 1);
    Bytes cc = complete(QCA_HCI_OP_SETUP_SYNC);
    CHECK_EQ(filter(p, cc), 1u);
    CHECK_EQ(opcode_at(cc, 3), QCA_HCI_OP_ENH_SETUP_SYNC);

    std::puts("[answers: a legacy command the host really sent stays legacy]");
    qca_sco_note_sent(&p, QCA_HCI_OP_SETUP_SYNC, 0);
    Bytes native = status(QCA_HCI_OP_SETUP_SYNC);
    CHECK_EQ(filter(p, native), 0u);
    CHECK_EQ(opcode_at(native, 4), QCA_HCI_OP_SETUP_SYNC);
    CHECK_EQ(p.count, 0u);

    std::puts("[answers: mixed commands match in order]");
    qca_sco_note_sent(&p, QCA_HCI_OP_SETUP_SYNC, 0);
    qca_sco_note_sent(&p, QCA_HCI_OP_SETUP_SYNC, 1);
    Bytes first = status(QCA_HCI_OP_SETUP_SYNC), second = status(QCA_HCI_OP_SETUP_SYNC);
    CHECK_EQ(filter(p, first), 0u);
    CHECK_EQ(filter(p, second), 1u);

    std::puts("[answers: other opcodes neither match nor consume]");
    qca_sco_note_sent(&p, QCA_HCI_OP_ACCEPT_SYNC, 1);
    Bytes other = status(QCA_HCI_OP_SETUP_SYNC);
    CHECK_EQ(filter(p, other), 0u);
    CHECK_EQ(p.count, 1u);
    Bytes accept = status(QCA_HCI_OP_ACCEPT_SYNC);
    CHECK_EQ(filter(p, accept), 1u);

    std::puts("[answers: note ignores other opcodes]");
    qca_sco_note_sent(&p, QCA_HCI_OP_RESET, 1);
    CHECK_EQ(p.count, 0u);

    std::puts("[answers: a failed transfer owes no answer]");
    qca_sco_note_sent(&p, QCA_HCI_OP_SETUP_SYNC, 1);
    qca_sco_note_sent(&p, QCA_HCI_OP_SETUP_SYNC, 0);
    qca_sco_cancel(&p, QCA_HCI_OP_SETUP_SYNC, 1);
    CHECK_EQ(p.count, 1u);
    Bytes left = status(QCA_HCI_OP_SETUP_SYNC);
    CHECK_EQ(filter(p, left), 0u); // the translated one was the cancelled one
    qca_sco_cancel(&p, QCA_HCI_OP_SETUP_SYNC, 1);
    CHECK_EQ(p.count, 0u);

    std::puts("[answers: full queue drops the oldest, order kept]");
    for (uint32_t i = 0; i < QCA_SCO_PENDING_MAX; i++)
        qca_sco_note_sent(&p, QCA_HCI_OP_SETUP_SYNC, 0);
    qca_sco_note_sent(&p, QCA_HCI_OP_SETUP_SYNC, 1);
    CHECK_EQ(p.count, uint8_t(QCA_SCO_PENDING_MAX));
    for (uint32_t i = 0; i + 1 < QCA_SCO_PENDING_MAX; i++) {
        Bytes s = status(QCA_HCI_OP_SETUP_SYNC);
        CHECK_EQ(filter(p, s), 0u);
    }
    Bytes last = status(QCA_HCI_OP_SETUP_SYNC);
    CHECK_EQ(filter(p, last), 1u);
    CHECK_EQ(p.count, 0u);
}

void test_event_walk()
{
    std::puts("[events: several in one transfer]");
    qca_sco_pending p;
    qca_sco_pending_init(&p);
    qca_sco_note_sent(&p, QCA_HCI_OP_SETUP_SYNC, 1);
    Bytes e = {0x13, 0x05, 1, 0x34, 0x12, 1, 0}; // Number Of Completed Packets
    Bytes st = status(QCA_HCI_OP_SETUP_SYNC);
    e.insert(e.end(), st.begin(), st.end());
    uint32_t replies = 0, events = 0;
    CHECK_EQ(filter(p, e, &replies, &events), 1u);
    CHECK_EQ(opcode_at(e, 7 + 4), QCA_HCI_OP_ENH_SETUP_SYNC);
    CHECK_EQ(replies, 1u);
    CHECK_EQ(events, 2u);

    std::puts("[events: a truncated event is not rewritten]");
    qca_sco_note_sent(&p, QCA_HCI_OP_SETUP_SYNC, 1);
    Bytes cut = status(QCA_HCI_OP_SETUP_SYNC);
    cut.pop_back();
    CHECK_EQ(filter(p, cut, &replies, &events), 0u);
    CHECK_EQ(replies, 0u);
    CHECK_EQ(events, 1u); // still shows the firmware is talking
    CHECK_EQ(p.count, 1u);

    std::puts("[events: no match on a malformed length or an empty buffer]");
    Bytes wrong = {QCA_HCI_EVT_CMD_STATUS, 3, 0, 1, 0x28};
    CHECK_EQ(filter(p, wrong, &replies), 0u);
    CHECK_EQ(replies, 0u);
    Bytes shortcc = {QCA_HCI_EVT_CMD_COMPLETE, 2, 1, 0x28};
    CHECK_EQ(filter(p, shortcc, &replies), 0u);
    CHECK_EQ(replies, 0u);
    CHECK_EQ(qca_sco_filter_events(&p, nullptr, 8, &replies, &events), 0u);
    CHECK_EQ(events, 0u);
    uint8_t one = QCA_HCI_EVT_CMD_STATUS;
    CHECK_EQ(qca_sco_filter_events(&p, &one, 1, &replies, &events), 0u);
    CHECK_EQ(events, 0u);

    // The Sync Connection Complete that follows is identical for both commands.
    Bytes done = {0x2C, 17, 0, 0x42, 0, 1, 2, 3, 4, 5, 6, 2, 7, 6, 60, 0, 60, 0, 2};
    CHECK_EQ(filter(p, done), 0u);
    CHECK_EQ(done[0], 0x2C);
}

void test_round_trip()
{
    std::puts("[round trip: host sends 0x043D, wire carries 0x0428, host hears 0x043D]");
    qca_sco_pending p;
    qca_sco_pending_init(&p);
    Bytes cmd = Enh().setup(), wire;
    uint16_t v = 0;
    CHECK_EQ(translate(cmd, wire, &v, nullptr), QCA_SCO_TRANSLATED);
    CHECK_EQ(opcode_at(wire, 0), QCA_HCI_OP_SETUP_SYNC);
    qca_sco_note_sent(&p, opcode_at(wire, 0), 1);
    Bytes answer = status(opcode_at(wire, 0));
    CHECK_EQ(filter(p, answer), 1u);
    CHECK_EQ(opcode_at(answer, 4), opcode_at(cmd, 0));
}

void test_hostile_input()
{
    std::puts("[hostile input: random commands and events, under the sanitizers]");
    uint32_t x = 0x12345678;
    auto next = [&x]() {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return x;
    };
    qca_sco_pending p;
    qca_sco_pending_init(&p);
    for (int i = 0; i < 20000; i++) {
        Bytes b(next() % 80);
        for (uint8_t &c : b)
            c = uint8_t(next());
        if (b.size() >= 3 && (next() & 1)) {
            b[0] = (next() & 1) ? 0x3D : 0x3E;
            b[1] = 0x04;
            b[2] = (next() & 1) ? uint8_t(b.size() - 3) : uint8_t(next());
        }
        uint8_t out[QCA_SCO_LEGACY_MAX];
        uint32_t n = 0;
        uint16_t v = 0;
        qca_sco_why why;
        qca_sco_translate(b.data(), uint32_t(b.size()), out, sizeof(out), &n, &v, &why);
        qca_sco_note_sent(&p, (next() & 1) ? QCA_HCI_OP_SETUP_SYNC : QCA_HCI_OP_ACCEPT_SYNC, next() & 1);
        qca_sco_filter_events(&p, b.data(), uint32_t(b.size()), nullptr, nullptr);
    }
    CHECK(p.count <= QCA_SCO_PENDING_MAX);
}

// ---- watchdog -------------------------------------------------------------

void alive(qca_wd &w)
{
    qca_wd_on_events(&w, 1, 0);
}

// One command that nobody answers, ticked past its deadline.
qca_wd_action miss(qca_wd &w, uint64_t &t)
{
    qca_wd_on_command(&w, 0x0C24, t);
    t += QCA_WD_CMD_TIMEOUT_MS;
    return qca_wd_tick(&w, t);
}

void test_constants()
{
    std::puts("[policy constants are the documented ones, written out here on purpose]");
    // These come from Linux btusb/hci_core; a change must show up as a test edit.
    CHECK_EQ(QCA_WD_TIMEOUTS_FOR_RESET, 5u);       // btusb_qca_reset: ++cnt < 5
    CHECK_EQ(QCA_WD_CMD_TIMEOUT_MS, 2000u);        // HCI_CMD_TIMEOUT
    CHECK_EQ(QCA_WD_RESET_TIMEOUT_MS, 10000u);     // HCI_INIT_TIMEOUT
    CHECK_EQ(QCA_WD_SILENT_TIMEOUTS, 2u);
    CHECK_EQ(QCA_WD_SILENCE_MS, 30000u);
    CHECK_EQ(QCA_WD_COOLDOWN_MS, 30000u);
    CHECK_EQ(QCA_WD_MAX_RESETS, 3u);
    CHECK_EQ(QCA_WD_BUDGET_WINDOW_MS, 600000u);
    CHECK_EQ(QCA_HCI_OP_ENH_SETUP_SYNC, 0x043Du);
    CHECK_EQ(QCA_HCI_OP_ENH_ACCEPT_SYNC, 0x043Eu);
    CHECK_EQ(QCA_HCI_OP_SETUP_SYNC, 0x0428u);
    CHECK_EQ(QCA_HCI_OP_ACCEPT_SYNC, 0x0429u);
    CHECK_EQ(QCA_HCI_OP_HOST_NUM_COMP, 0x0C35u);
    CHECK_EQ(QCA_HCI_OP_RESET, 0x0C03u);
}

void test_watchdog_basic()
{
    std::puts("[watchdog: five unanswered commands, as Linux]");
    qca_wd w;
    qca_wd_init(&w);
    uint64_t t = 1000;
    alive(w);
    for (uint32_t i = 1; i < QCA_WD_TIMEOUTS_FOR_RESET; i++) {
        CHECK_EQ(miss(w, t), QCA_WD_NONE);
        CHECK_EQ(w.timeouts, i);
    }
    CHECK_EQ(miss(w, t), QCA_WD_USB_RESET);
    CHECK_EQ(w.timeouts, 0u);
    CHECK_EQ(w.total_timeouts, QCA_WD_TIMEOUTS_FOR_RESET);
    CHECK_EQ(w.reset_count, 1u);

    std::puts("[watchdog: an answer in time is not a timeout]");
    qca_wd_init(&w);
    t = 1000;
    alive(w);
    qca_wd_on_command(&w, 0x0C24, t);
    CHECK_EQ(qca_wd_tick(&w, t + QCA_WD_CMD_TIMEOUT_MS - 1), QCA_WD_NONE);
    CHECK_EQ(w.timeouts, 0u);
    qca_wd_on_events(&w, 1, 1);
    CHECK_EQ(w.outstanding, 0u);
    CHECK_EQ(qca_wd_tick(&w, t + QCA_WD_CMD_TIMEOUT_MS * 5), QCA_WD_NONE);
    CHECK_EQ(w.timeouts, 0u);

    std::puts("[watchdog: any event from the firmware clears the count]");
    qca_wd_init(&w);
    t = 1000;
    alive(w);
    for (uint32_t i = 1; i < QCA_WD_TIMEOUTS_FOR_RESET; i++)
        miss(w, t);
    alive(w);
    CHECK_EQ(w.timeouts, 0u);
    CHECK_EQ(miss(w, t), QCA_WD_NONE);
    qca_wd_on_events(&w, 0, 0); // an empty transfer is not an event
    CHECK_EQ(w.timeouts, 1u);

    std::puts("[watchdog: HCI_Reset gets Linux's longer timeout]");
    qca_wd_init(&w);
    alive(w);
    qca_wd_on_command(&w, QCA_HCI_OP_RESET, 0);
    qca_wd_tick(&w, QCA_WD_RESET_TIMEOUT_MS - 1);
    CHECK_EQ(w.timeouts, 0u);
    qca_wd_tick(&w, QCA_WD_RESET_TIMEOUT_MS);
    CHECK_EQ(w.timeouts, 1u);
}

void test_watchdog_false_positives()
{
    std::puts("[watchdog: never without proof the event hook works]");
    qca_wd w;
    qca_wd_init(&w);
    uint64_t t = 1000;
    for (int i = 0; i < 100; i++)
        CHECK_EQ(miss(w, t), QCA_WD_NONE);
    CHECK_EQ(w.total_timeouts, 0u);
    CHECK_EQ(w.outstanding, 0u);

    std::puts("[watchdog: commands that get no answer by design]");
    qca_wd_init(&w);
    alive(w);
    qca_wd_on_command(&w, QCA_HCI_OP_HOST_NUM_COMP, 0);
    CHECK_EQ(w.outstanding, 0u);
    qca_wd_on_command(&w, 0xFC12, 0); // OGF 0x3F
    CHECK_EQ(w.outstanding, 0u);
    qca_wd_on_command_failed(&w, QCA_HCI_OP_HOST_NUM_COMP);
    qca_wd_on_command_failed(&w, 0xFC12);
    CHECK_EQ(w.outstanding, 0u);

    std::puts("[watchdog: a failed transfer owes no answer]");
    qca_wd_init(&w);
    alive(w);
    qca_wd_on_command(&w, 0x0C24, 0);
    qca_wd_on_command_failed(&w, 0x0C24);
    CHECK_EQ(w.outstanding, 0u);
    CHECK_EQ(qca_wd_tick(&w, 10000), QCA_WD_NONE);
    CHECK_EQ(w.timeouts, 0u);
    qca_wd_on_command_failed(&w, 0x0C24); // no underflow
    CHECK_EQ(w.outstanding, 0u);

    std::puts("[watchdog: one lost answer and silence (e.g. across sleep) is no reset]");
    qca_wd_init(&w);
    alive(w);
    t = 1000;
    CHECK_EQ(miss(w, t), QCA_WD_NONE);
    for (uint64_t s = 0; s < 3600; s++)
        CHECK_EQ(qca_wd_tick(&w, t + s * 1000), QCA_WD_NONE);
    CHECK_EQ(w.reset_count, 0u);
}

void test_watchdog_silence_rule()
{
    std::puts("[watchdog: two unanswered, then the host goes quiet]");
    qca_wd w;
    qca_wd_init(&w);
    alive(w);
    uint64_t t = 1000;
    miss(w, t);
    CHECK_EQ(miss(w, t), QCA_WD_NONE);
    uint64_t last = t;
    CHECK_EQ(qca_wd_tick(&w, last + QCA_WD_SILENCE_MS - 1), QCA_WD_NONE);
    CHECK_EQ(qca_wd_tick(&w, last + QCA_WD_SILENCE_MS), QCA_WD_USB_RESET);

    // An event during the wait calls it off.
    qca_wd_init(&w);
    alive(w);
    t = 1000;
    miss(w, t);
    miss(w, t);
    alive(w);
    CHECK_EQ(qca_wd_tick(&w, t + QCA_WD_SILENCE_MS), QCA_WD_NONE);
}

void test_watchdog_budget()
{
    std::puts("[watchdog: cooldown after a reset; re-enumeration keeps the budget]");
    qca_wd w;
    qca_wd_init(&w);
    alive(w);
    uint64_t t = 1000;
    for (uint32_t i = 1; i < QCA_WD_TIMEOUTS_FOR_RESET; i++)
        miss(w, t);
    CHECK_EQ(miss(w, t), QCA_WD_USB_RESET);
    uint64_t reset_at = t;
    qca_wd_new_device(&w);
    CHECK_EQ(w.events, 0u);
    CHECK_EQ(w.reset_count, 1u);
    alive(w);
    // During the cooldown nothing counts, not even a dead-looking device.
    qca_wd_on_command(&w, 0x0C24, reset_at);
    CHECK_EQ(qca_wd_tick(&w, reset_at + QCA_WD_COOLDOWN_MS - 1), QCA_WD_NONE);
    CHECK_EQ(w.timeouts, 0u);
    CHECK_EQ(w.outstanding, 0u);

    std::puts("[watchdog: a controller that stays dead gets three resets, then none]");
    qca_wd_init(&w);
    t = 1000;
    uint32_t resets = 0;
    for (int i = 0; i < 2000; i++) { // over an hour of retries
        alive(w); // each new instance answers once, then dies
        qca_wd_new_device(&w);
        alive(w);
        t += QCA_WD_COOLDOWN_MS;
        for (uint32_t m = 0; m < QCA_WD_TIMEOUTS_FOR_RESET; m++)
            if (miss(w, t) == QCA_WD_USB_RESET)
                resets++;
        if (w.latched)
            break;
    }
    CHECK(w.latched != 0);
    CHECK_EQ(resets, QCA_WD_MAX_RESETS);
    alive(w);
    CHECK_EQ(miss(w, t), QCA_WD_NONE);
    CHECK_EQ(qca_wd_next_tick_ms(&w, t), 0u);
    CHECK_EQ(w.reset_count, QCA_WD_MAX_RESETS);

    std::puts("[watchdog: the measured crash rate (one per ~15 min) never spends the budget]");
    qca_wd_init(&w);
    t = 1000;
    for (int crash = 0; crash < 100; crash++) {
        alive(w);
        t += 909000; // the gap between the two crashes in evidence/linux-dmesg.txt
        qca_wd_action last = QCA_WD_NONE;
        for (uint32_t m = 0; m < QCA_WD_TIMEOUTS_FOR_RESET; m++)
            last = miss(w, t);
        CHECK_EQ(last, QCA_WD_USB_RESET);
        qca_wd_new_device(&w);
    }
    CHECK(w.latched == 0);
    CHECK_EQ(w.reset_count, 100u);
}

void test_watchdog_ticks()
{
    std::puts("[watchdog: the timer only runs when there is something to wait for]");
    qca_wd w;
    qca_wd_init(&w);
    alive(w);
    CHECK_EQ(qca_wd_next_tick_ms(&w, 0), 0u);
    qca_wd_on_command(&w, 0x0C24, 1000);
    CHECK_EQ(qca_wd_next_tick_ms(&w, 1000), QCA_WD_CMD_TIMEOUT_MS);
    CHECK_EQ(qca_wd_next_tick_ms(&w, 2500), QCA_WD_CMD_TIMEOUT_MS - 1500);
    CHECK_EQ(qca_wd_next_tick_ms(&w, 1000 + QCA_WD_CMD_TIMEOUT_MS), 1u);
    qca_wd_tick(&w, 1000 + QCA_WD_CMD_TIMEOUT_MS);
    CHECK_EQ(w.timeouts, 1u);
    CHECK_EQ(qca_wd_next_tick_ms(&w, 5000), QCA_WD_TICK_MS);
    alive(w);
    CHECK_EQ(qca_wd_next_tick_ms(&w, 5000), 0u);

    // Several in flight: an answer leaves the others pending.
    qca_wd_init(&w);
    alive(w);
    qca_wd_on_command(&w, 0x0C24, 0);
    qca_wd_on_command(&w, 0x0C25, 0);
    CHECK_EQ(w.outstanding, 2u);
    qca_wd_on_events(&w, 1, 1);
    CHECK_EQ(w.outstanding, 1u);
    qca_wd_on_events(&w, 3, 5); // more answers than commands
    CHECK_EQ(w.outstanding, 0u);
}

} // namespace

int main()
{
    test_lengths();
    test_setup_cvsd();
    test_accept();
    test_transparent();
    test_voice_settings();
    test_refusals();
    test_pass();
    test_answers();
    test_event_walk();
    test_round_trip();
    test_hostile_input();
    test_constants();
    test_watchdog_basic();
    test_watchdog_false_positives();
    test_watchdog_silence_rule();
    test_watchdog_budget();
    test_watchdog_ticks();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
