/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * See qca_hci_filter.h. Freestanding: no libc calls.
 */
#include "qca_hci_filter.h"

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void copy(uint8_t *to, const uint8_t *from, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        to[i] = from[i];
}

static int same(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

/*
 * Offsets inside the Enhanced Setup parameters. Enhanced Accept is the same
 * block after a 6-byte BD_ADDR instead of a 2-byte Connection_Handle, so
 * everything below shifts by 4.
 */
enum {
    P_ID_LEN = 2,     /* Connection_Handle                       */
    P_TX_BW = 2,
    P_RX_BW = 6,
    P_TX_CODING = 10, /* air, 5 bytes: id, company, vendor codec */
    P_RX_CODING = 15,
    P_IN_CODING = 32, /* host side                               */
    P_OUT_CODING = 37,
    P_IN_CODED = 42,
    P_OUT_CODED = 44,
    P_IN_FORMAT = 46,
    P_OUT_FORMAT = 47,
    P_IN_MSB = 48,
    P_OUT_MSB = 49,
    P_IN_PATH = 50,
    P_OUT_PATH = 51,
    P_LATENCY = 54,
    P_PACKET = 56,
    P_RETRANS = 58
};

#define CODING_LEN 5u
#define HCI_PATH   0u

/* Voice_Setting input coding, bits 9:8. */
static int input_coding_bits(uint8_t id, uint16_t *bits)
{
    switch (id) {
    case QCA_CODEC_LINEAR_PCM: *bits = 0u << 8; return 1;
    case QCA_CODEC_ULAW:       *bits = 1u << 8; return 1;
    case QCA_CODEC_ALAW:       *bits = 2u << 8; return 1;
    default:                   return 0;
    }
}

/* Voice_Setting air coding, bits 1:0. */
static int air_coding_bits(uint8_t id, uint16_t *bits)
{
    switch (id) {
    case QCA_CODEC_CVSD: *bits = 0u; return 1;
    case QCA_CODEC_ULAW: *bits = 1u; return 1;
    case QCA_CODEC_ALAW: *bits = 2u; return 1;
    default:             return 0;
    }
}

/* PCM_Data_Format (1 one's, 2 two's complement, 3 sign-magnitude, 4 unsigned)
 * to Voice_Setting input data format, bits 7:6. */
static int data_format_bits(uint8_t format, uint16_t *bits)
{
    if (format < 1u || format > 4u)
        return 0;
    *bits = (uint16_t)((uint16_t)(format - 1u) << 6);
    return 1;
}

static qca_sco_why voice_setting(const uint8_t *b, uint16_t *voice)
{
    uint8_t air = b[P_TX_CODING];
    uint8_t host = b[P_IN_CODING];
    uint16_t in, fmt = 0, size, msb, a;

    /* Host and air are the same bytes, so the controller does not code: the
     * legacy command's transparent mode is exactly this. mSBC with a
     * transparent host side is how a host that does its own mSBC asks. */
    if (host == QCA_CODEC_TRANSPARENT && (air == QCA_CODEC_TRANSPARENT || air == QCA_CODEC_MSBC)) {
        *voice = QCA_VOICE_TRANSPARENT;
        return QCA_SCO_WHY_NONE;
    }
    if (!air_coding_bits(air, &a) || !input_coding_bits(host, &in))
        return QCA_SCO_WHY_HOST_CODING;

    if (host == QCA_CODEC_LINEAR_PCM) {
        /* 16-bit two's complement is the usual case. Coded_Data_Size is in
         * bits; the legacy field only has room for 8 or 16. */
        uint16_t coded = rd16(b + P_IN_CODED);
        if (coded != 8u && coded != 16u)
            return QCA_SCO_WHY_HOST_CODING;
        if (!data_format_bits(b[P_IN_FORMAT], &fmt))
            return QCA_SCO_WHY_HOST_CODING;
        if (b[P_IN_MSB] > 7u)
            return QCA_SCO_WHY_HOST_CODING;
        size = coded == 16u ? (1u << 5) : 0u;
        msb = (uint16_t)((uint16_t)b[P_IN_MSB] << 2);
    } else {
        size = 0u; /* companded host data is 8-bit, with no linear position */
        msb = 0u;
    }
    *voice = (uint16_t)(in | fmt | size | msb | a);
    return QCA_SCO_WHY_NONE;
}

qca_sco_result qca_sco_translate(const uint8_t *cmd, uint32_t len, uint8_t *out, uint32_t cap,
                                 uint32_t *out_len, uint16_t *voice, qca_sco_why *why)
{
    qca_sco_why local;
    uint16_t op, v = 0;
    uint32_t id_len, plen, legacy_plen;
    uint16_t legacy_op;
    const uint8_t *p, *b;
    uint8_t *o;

    if (!why)
        why = &local;
    *why = QCA_SCO_WHY_NONE;
    if (!cmd || len < QCA_HCI_CMD_HDR)
        return QCA_SCO_PASS;

    op = rd16(cmd);
    if (op == QCA_HCI_OP_ENH_SETUP_SYNC) {
        id_len = P_ID_LEN;
        plen = QCA_HCI_ENH_SETUP_PLEN;
        legacy_op = QCA_HCI_OP_SETUP_SYNC;
        legacy_plen = QCA_HCI_SETUP_PLEN;
    } else if (op == QCA_HCI_OP_ENH_ACCEPT_SYNC) {
        id_len = 6u; /* BD_ADDR */
        plen = QCA_HCI_ENH_ACCEPT_PLEN;
        legacy_op = QCA_HCI_OP_ACCEPT_SYNC;
        legacy_plen = QCA_HCI_ACCEPT_PLEN;
    } else {
        return QCA_SCO_PASS;
    }

    if (cmd[2] != plen || len != QCA_HCI_CMD_HDR + plen) {
        *why = QCA_SCO_WHY_LENGTH;
        return QCA_SCO_REFUSED;
    }
    p = cmd + QCA_HCI_CMD_HDR;
    b = p + (id_len - P_ID_LEN); /* offsets above are relative to this */

    if (!same(b + P_TX_CODING, b + P_RX_CODING, CODING_LEN) ||
        !same(b + P_IN_CODING, b + P_OUT_CODING, CODING_LEN) ||
        rd16(b + P_IN_CODED) != rd16(b + P_OUT_CODED) ||
        b[P_IN_FORMAT] != b[P_OUT_FORMAT] || b[P_IN_MSB] != b[P_OUT_MSB]) {
        *why = QCA_SCO_WHY_ASYMMETRIC;
        return QCA_SCO_REFUSED;
    }
    if (b[P_TX_CODING] == 0xFFu || b[P_IN_CODING] == 0xFFu) {
        *why = QCA_SCO_WHY_VENDOR_CODEC;
        return QCA_SCO_REFUSED;
    }
    if (b[P_IN_PATH] != HCI_PATH || b[P_OUT_PATH] != HCI_PATH) {
        *why = QCA_SCO_WHY_DATA_PATH;
        return QCA_SCO_REFUSED;
    }
    *why = voice_setting(b, &v);
    if (*why != QCA_SCO_WHY_NONE)
        return QCA_SCO_REFUSED;
    if (!out || cap < QCA_HCI_CMD_HDR + legacy_plen) {
        *why = QCA_SCO_WHY_BUFFER;
        return QCA_SCO_REFUSED;
    }

    /* Legacy parameters: identifier, TX bandwidth, RX bandwidth, Max_Latency,
     * Voice_Setting, Retransmission_Effort, Packet_Type. */
    wr16(out, legacy_op);
    out[2] = (uint8_t)legacy_plen;
    o = out + QCA_HCI_CMD_HDR;
    copy(o, p, id_len);
    o += id_len;
    copy(o, b + P_TX_BW, 4u);
    copy(o + 4, b + P_RX_BW, 4u);
    copy(o + 8, b + P_LATENCY, 2u);
    wr16(o + 10, v);
    o[12] = b[P_RETRANS];
    copy(o + 13, b + P_PACKET, 2u);

    if (out_len)
        *out_len = QCA_HCI_CMD_HDR + legacy_plen;
    if (voice)
        *voice = v;
    return QCA_SCO_TRANSLATED;
}

const char *qca_sco_why_name(qca_sco_why why)
{
    switch (why) {
    case QCA_SCO_WHY_NONE:         return "none";
    case QCA_SCO_WHY_LENGTH:       return "parameter length";
    case QCA_SCO_WHY_ASYMMETRIC:   return "TX/RX or input/output differ";
    case QCA_SCO_WHY_VENDOR_CODEC: return "vendor codec";
    case QCA_SCO_WHY_DATA_PATH:    return "data path is not HCI";
    case QCA_SCO_WHY_HOST_CODING:  return "no voice setting for this coding";
    case QCA_SCO_WHY_BUFFER:       return "output buffer too small";
    }
    return "unknown";
}

/* ---- answer matching ---------------------------------------------------- */

void qca_sco_pending_init(qca_sco_pending *p)
{
    p->head = 0;
    p->count = 0;
}

static qca_sco_pending_entry *slot(qca_sco_pending *p, uint32_t i)
{
    return &p->e[(p->head + i) % QCA_SCO_PENDING_MAX];
}

/* Remove position i (0 = oldest), keeping the order of the others. */
static void remove_at(qca_sco_pending *p, uint32_t i)
{
    uint32_t j;
    for (j = i; j + 1u < p->count; j++)
        *slot(p, j) = *slot(p, j + 1u);
    p->count--;
}

void qca_sco_note_sent(qca_sco_pending *p, uint16_t legacy_op, int translated)
{
    qca_sco_pending_entry *e;

    if (legacy_op != QCA_HCI_OP_SETUP_SYNC && legacy_op != QCA_HCI_OP_ACCEPT_SYNC)
        return;
    if (p->count == QCA_SCO_PENDING_MAX)
        remove_at(p, 0);
    e = slot(p, p->count);
    e->op = legacy_op;
    e->translated = translated ? 1u : 0u;
    p->count++;
}

void qca_sco_cancel(qca_sco_pending *p, uint16_t legacy_op, int translated)
{
    uint32_t i = p->count;
    uint8_t t = translated ? 1u : 0u;

    while (i > 0u) {
        i--;
        if (slot(p, i)->op == legacy_op && slot(p, i)->translated == t) {
            remove_at(p, i);
            return;
        }
    }
}

/* Find and consume the oldest entry for this opcode. Returns its translated
 * flag, or 0 if there is none. */
static int consume(qca_sco_pending *p, uint16_t op)
{
    uint32_t i;
    for (i = 0; i < p->count; i++) {
        if (slot(p, i)->op == op) {
            int t = slot(p, i)->translated;
            remove_at(p, i);
            return t;
        }
    }
    return 0;
}

uint32_t qca_sco_filter_events(qca_sco_pending *p, uint8_t *buf, uint32_t len, uint32_t *replies,
                               uint32_t *events)
{
    uint32_t off = 0, rewritten = 0, nreplies = 0, nevents = 0;

    while (buf && off + 2u <= len) {
        uint8_t evt = buf[off];
        uint32_t plen = buf[off + 1u];
        uint32_t op_at = 0;

        nevents++; /* even a partial event shows the firmware is talking */
        if (off + 2u + plen > len)
            break; /* the rest of this event is in another transfer */
        if (evt == QCA_HCI_EVT_CMD_STATUS && plen == 4u)
            op_at = off + 4u; /* Status, Num_HCI_Command_Packets, Opcode */
        else if (evt == QCA_HCI_EVT_CMD_COMPLETE && plen >= 3u)
            op_at = off + 3u; /* Num_HCI_Command_Packets, Opcode, ...    */
        if (op_at) {
            uint16_t op = rd16(buf + op_at);
            nreplies++;
            if (consume(p, op)) {
                wr16(buf + op_at, op == QCA_HCI_OP_SETUP_SYNC ? (uint16_t)QCA_HCI_OP_ENH_SETUP_SYNC
                                                              : (uint16_t)QCA_HCI_OP_ENH_ACCEPT_SYNC);
                rewritten++;
            }
        }
        off += 2u + plen;
    }
    if (replies)
        *replies = nreplies;
    if (events)
        *events = nevents;
    return rewritten;
}

/* ---- command watchdog --------------------------------------------------- */

void qca_wd_init(qca_wd *w)
{
    uint32_t i;

    w->deadline_ms = 0;
    w->last_timeout_ms = 0;
    w->cooldown_until_ms = 0;
    for (i = 0; i < QCA_WD_MAX_RESETS; i++)
        w->reset_ms[i] = 0;
    w->reset_count = 0;
    w->outstanding = 0;
    w->timeouts = 0;
    w->events = 0;
    w->total_timeouts = 0;
    w->latched = 0;
}

void qca_wd_new_device(qca_wd *w)
{
    w->deadline_ms = 0;
    w->outstanding = 0;
    w->timeouts = 0;
    w->events = 0;
}

/* Host_Number_Of_Completed_Packets has no event; a vendor command is whatever
 * the firmware makes of it. Neither says anything about the controller. */
static int expects_answer(uint16_t opcode)
{
    if (opcode == QCA_HCI_OP_HOST_NUM_COMP)
        return 0;
    return (opcode >> 10) != QCA_HCI_OGF_VENDOR;
}

void qca_wd_on_command(qca_wd *w, uint16_t opcode, uint64_t now_ms)
{
    if (!expects_answer(opcode))
        return;
    if (w->outstanding == 0u)
        w->deadline_ms = now_ms + (opcode == QCA_HCI_OP_RESET ? QCA_WD_RESET_TIMEOUT_MS
                                                              : QCA_WD_CMD_TIMEOUT_MS);
    w->outstanding++;
}

void qca_wd_on_command_failed(qca_wd *w, uint16_t opcode)
{
    if (expects_answer(opcode) && w->outstanding > 0u)
        w->outstanding--;
}

void qca_wd_on_events(qca_wd *w, uint32_t events, uint32_t replies)
{
    if (events == 0u)
        return;
    w->events += events;
    w->timeouts = 0; /* the firmware is alive */
    if (replies >= w->outstanding)
        w->outstanding = 0;
    else
        w->outstanding -= replies;
}

qca_wd_action qca_wd_tick(qca_wd *w, uint64_t now_ms)
{
    uint32_t i;
    uint64_t oldest = 0;

    if (w->latched)
        return QCA_WD_NONE;
    if (now_ms < w->cooldown_until_ms) {
        /* The device is being replaced: what it did not answer means nothing. */
        w->outstanding = 0;
        return QCA_WD_NONE;
    }
    /* Without one event from this device the event hook is unproven, and every
     * command would look unanswered. Never reset on that. */
    if (w->events == 0u) {
        w->outstanding = 0;
        return QCA_WD_NONE;
    }
    if (w->outstanding > 0u && now_ms >= w->deadline_ms) {
        w->outstanding = 0;
        w->timeouts++;
        w->total_timeouts++;
        w->last_timeout_ms = now_ms;
    }
    if (w->timeouts < QCA_WD_TIMEOUTS_FOR_RESET &&
        !(w->timeouts >= QCA_WD_SILENT_TIMEOUTS &&
          now_ms - w->last_timeout_ms >= QCA_WD_SILENCE_MS))
        return QCA_WD_NONE;

    /* The budget: QCA_WD_MAX_RESETS inside the window, or stop for good. */
    if (w->reset_count >= QCA_WD_MAX_RESETS) {
        for (i = 0; i < QCA_WD_MAX_RESETS; i++)
            if (i == 0u || w->reset_ms[i] < oldest)
                oldest = w->reset_ms[i];
        if (now_ms - oldest < QCA_WD_BUDGET_WINDOW_MS) {
            w->latched = 1;
            return QCA_WD_NONE;
        }
    }
    w->reset_ms[w->reset_count % QCA_WD_MAX_RESETS] = now_ms;
    w->reset_count++;
    w->cooldown_until_ms = now_ms + QCA_WD_COOLDOWN_MS;
    w->timeouts = 0;
    w->outstanding = 0;
    return QCA_WD_USB_RESET;
}

uint32_t qca_wd_next_tick_ms(const qca_wd *w, uint64_t now_ms)
{
    if (w->latched)
        return 0u;
    if (w->outstanding > 0u) {
        if (w->deadline_ms <= now_ms)
            return 1u;
        return (uint32_t)(w->deadline_ms - now_ms);
    }
    if (w->timeouts > 0u)
        return QCA_WD_TICK_MS; /* waiting out the silence rule */
    return 0u;
}

