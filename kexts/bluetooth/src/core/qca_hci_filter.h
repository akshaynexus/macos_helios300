/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Pure HCI logic for QCARomeHCIFix (the Lilu plugin that sits between macOS's
 * Bluetooth stack and the Rome controller). No I/O and no libc, so the kext
 * and the host tests run exactly the same decisions.
 *
 * Two jobs:
 *
 * 1. SCO command translation. Linux marks Rome
 *    HCI_QUIRK_BROKEN_ENHANCED_SETUP_SYNC_CONN: the controller advertises
 *    Enhanced Setup Synchronous Connection (0x043D) but it does not work, so
 *    Linux sends the legacy Setup Synchronous Connection (0x0428). Here a
 *    0x043D / 0x043E command is rewritten to 0x0428 / 0x0429 when the legacy
 *    command can express exactly the same stream. The Command Status that
 *    answers it is rewritten back, so the host sees the opcode it sent.
 *    Anything the legacy command cannot express (a codec in the controller,
 *    a vendor data path) is passed through untouched and counted.
 *
 * 2. HCI command watchdog. Mirrors btusb_qca_reset(): a controller whose
 *    firmware died stops answering HCI commands, and only a USB reset gets it
 *    back (QCARomePatchRAM then re-patches the new device). Bounded: see
 *    QCA_WD_MAX_RESETS.
 */
#ifndef QCA_HCI_FILTER_H
#define QCA_HCI_FILTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QCA_HCI_OP_SETUP_SYNC       0x0428u
#define QCA_HCI_OP_ACCEPT_SYNC      0x0429u
#define QCA_HCI_OP_ENH_SETUP_SYNC   0x043Du
#define QCA_HCI_OP_ENH_ACCEPT_SYNC  0x043Eu
#define QCA_HCI_OP_RESET            0x0C03u
#define QCA_HCI_OP_HOST_NUM_COMP    0x0C35u /* has no event, by specification */
#define QCA_HCI_OGF_VENDOR          0x3Fu

#define QCA_HCI_EVT_CMD_COMPLETE    0x0Eu
#define QCA_HCI_EVT_CMD_STATUS      0x0Fu

/* Parameter lengths from the Core specification, Vol 4 Part E 7.1.26/27/45/46. */
#define QCA_HCI_ENH_SETUP_PLEN      59u
#define QCA_HCI_ENH_ACCEPT_PLEN     63u
#define QCA_HCI_SETUP_PLEN          17u
#define QCA_HCI_ACCEPT_PLEN         21u
#define QCA_HCI_CMD_HDR             3u
/* Largest legacy command this code writes, header included. */
#define QCA_SCO_LEGACY_MAX          (QCA_HCI_CMD_HDR + QCA_HCI_ACCEPT_PLEN)
/* Largest enhanced command this code reads, header included. */
#define QCA_SCO_ENHANCED_MAX        (QCA_HCI_CMD_HDR + QCA_HCI_ENH_ACCEPT_PLEN)

/* Coding_Format IDs (Assigned Numbers). */
#define QCA_CODEC_ULAW        0x00u
#define QCA_CODEC_ALAW        0x01u
#define QCA_CODEC_CVSD        0x02u
#define QCA_CODEC_TRANSPARENT 0x03u
#define QCA_CODEC_LINEAR_PCM  0x04u
#define QCA_CODEC_MSBC        0x05u

/* What Linux and BlueZ send for mSBC / transparent over the legacy command
 * (BT_VOICE_TRANSPARENT). */
#define QCA_VOICE_TRANSPARENT 0x0003u

typedef enum qca_sco_result {
    QCA_SCO_PASS = 0,    /* not an enhanced synchronous command: leave it      */
    QCA_SCO_TRANSLATED,  /* out[] holds the legacy command                     */
    QCA_SCO_REFUSED      /* enhanced, but the legacy command cannot express it */
} qca_sco_result;

typedef enum qca_sco_why {
    QCA_SCO_WHY_NONE = 0,
    QCA_SCO_WHY_LENGTH,        /* parameter length is not the specified one   */
    QCA_SCO_WHY_ASYMMETRIC,    /* TX and RX (or input and output) differ      */
    QCA_SCO_WHY_VENDOR_CODEC,  /* vendor coding format                        */
    QCA_SCO_WHY_DATA_PATH,     /* data path is not HCI                        */
    QCA_SCO_WHY_HOST_CODING,   /* air and host coding have no voice setting   */
    QCA_SCO_WHY_BUFFER         /* caller's output buffer is too small         */
} qca_sco_why;

/* Translate one complete HCI command (header + parameters). On
 * QCA_SCO_TRANSLATED, out holds *out_len bytes and *voice the voice setting
 * chosen; on the other results out is not touched. why may be NULL. */
qca_sco_result qca_sco_translate(const uint8_t *cmd, uint32_t len, uint8_t *out, uint32_t cap,
                                 uint32_t *out_len, uint16_t *voice, qca_sco_why *why);

const char *qca_sco_why_name(qca_sco_why why);

/* ---- answer matching ---------------------------------------------------- */

#define QCA_SCO_PENDING_MAX 8u

typedef struct qca_sco_pending_entry {
    uint16_t op;         /* legacy opcode on the wire */
    uint8_t  translated; /* the host sent the enhanced one */
} qca_sco_pending_entry;

/* Commands sent with a legacy opcode and not yet answered, oldest first, so a
 * legacy command the host really sent is not mistaken for a translated one. */
typedef struct qca_sco_pending {
    qca_sco_pending_entry e[QCA_SCO_PENDING_MAX];
    uint8_t  head;
    uint8_t  count;
} qca_sco_pending;

void qca_sco_pending_init(qca_sco_pending *p);
/* Record a 0x0428/0x0429 before it goes out (its answer can beat the return).
 * Other opcodes are ignored. If full, the oldest entry is dropped. */
void qca_sco_note_sent(qca_sco_pending *p, uint16_t legacy_op, int translated);
/* The transfer failed, so no answer will come: forget the newest match. */
void qca_sco_cancel(qca_sco_pending *p, uint16_t legacy_op, int translated);

/* Walk the complete events in one interrupt transfer. A Command Status or
 * Command Complete that answers a translated command gets the enhanced opcode
 * back. Returns the number of events rewritten; *replies (may be NULL) is the
 * number of Command Status/Complete events seen, *events (may be NULL) the
 * number of complete events. */
uint32_t qca_sco_filter_events(qca_sco_pending *p, uint8_t *buf, uint32_t len, uint32_t *replies,
                               uint32_t *events);

/* ---- command watchdog --------------------------------------------------- */

#define QCA_WD_CMD_TIMEOUT_MS      2000u  /* Linux HCI_CMD_TIMEOUT              */
#define QCA_WD_RESET_TIMEOUT_MS    10000u /* Linux HCI_INIT_TIMEOUT             */
#define QCA_WD_TIMEOUTS_FOR_RESET  5u     /* Linux btusb_qca_reset: cnt < 5     */
/* If the host stops sending after a couple of timeouts, five never comes: two
 * unanswered commands and then this long with nothing back also counts. */
#define QCA_WD_SILENT_TIMEOUTS     2u
#define QCA_WD_SILENCE_MS          30000u
#define QCA_WD_COOLDOWN_MS         30000u /* after a reset: re-enumerate, re-patch */
#define QCA_WD_MAX_RESETS          3u     /* per QCA_WD_BUDGET_WINDOW_MS, then off */
#define QCA_WD_BUDGET_WINDOW_MS    600000u
#define QCA_WD_TICK_MS             1000u

typedef enum qca_wd_action {
    QCA_WD_NONE = 0,
    QCA_WD_USB_RESET
} qca_wd_action;

typedef struct qca_wd {
    uint64_t deadline_ms;     /* oldest unanswered command                       */
    uint64_t last_timeout_ms;
    uint64_t cooldown_until_ms;
    uint64_t reset_ms[QCA_WD_MAX_RESETS]; /* ring of the last resets             */
    uint32_t reset_count;     /* total, also the ring's write index modulo size  */
    uint32_t outstanding;
    uint32_t timeouts;        /* consecutive, cleared by any event               */
    uint32_t events;          /* events seen on this device: proves the hook     */
    uint32_t total_timeouts;
    int      latched;         /* budget spent: never again until reboot          */
} qca_wd;

void qca_wd_init(qca_wd *w);
/* A new controller instance: forget the transfer state, keep the budget. */
void qca_wd_new_device(qca_wd *w);
void qca_wd_on_command(qca_wd *w, uint16_t opcode, uint64_t now_ms);
/* The command's transfer failed, so no answer is owed. */
void qca_wd_on_command_failed(qca_wd *w, uint16_t opcode);
void qca_wd_on_events(qca_wd *w, uint32_t events, uint32_t replies);
/* Call periodically while qca_wd_next_tick_ms() is non-zero. */
qca_wd_action qca_wd_tick(qca_wd *w, uint64_t now_ms);
/* Milliseconds until the next useful tick, 0 if none is needed. */
uint32_t qca_wd_next_tick_ms(const qca_wd *w, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* QCA_HCI_FILTER_H */
