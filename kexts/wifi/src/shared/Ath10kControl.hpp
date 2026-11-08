#pragma once
#include <stdint.h>
// Fixed-width, pointer-free ABI; all callers are x86_64, version checked per call.
// AIRPORT_ATH10K_WPASEC gates the paused security work (Private Wi-Fi Address,
// WPA3-SAE/PMF, 802.1X): 0 (default) keeps the WPA2-PSK join path of the first
// release unchanged in both kexts; 1 compiles the new control operations and
// the core's PMF/external-authentication paths (docs/WIFI-SECURITY.md).
#ifndef AIRPORT_ATH10K_WPASEC
#define AIRPORT_ATH10K_WPASEC 0
#endif
namespace AthControl {
constexpr uint32_t version=1, clientType=0xa7100057;
enum Operation:uint32_t { Status, BSS, Connect, ReceiveEAPOL, SendEAPOL, InstallKeys, Authorize, Abort, Disconnect, Scan, Cancel, RekeyGroup, RekeyPair, Info, Restart, SecurityHold, RadioOptions,
 SendAuth, ReceiveAuth, AuthResult, OperationCount };
// Operations 32..47 are reserved for the AWDL front end (AwdlRadioStart/Stop/SetChannel/Tx/RxPoll).
constexpr uint32_t awdlOperationBase=32;
struct Request {
 uint32_t version=AthControl::version,operation=0,index=0,length=0;
 uint8_t data[512]{};
};
struct Response {
 uint32_t version=AthControl::version,stage=0,phase=0,flags=0,count=0,length=0;
 uint8_t local[6]{},bssid[6]{};
 char error[96]{};
 uint8_t data[512]{};
};
// Connect: data[0]=SSID length, [1]=WPA2(1)/open(0), [2..7]=BSSID, [8..]=SSID.
// Connect index is a bit mask: 1 = hidden SSID (the SSID names the selected
// BSSID), 2 = extended block after the SSID (Info bit 1024 advertises it):
//   x[0] flags: 1 external authentication (the front end sends/receives
//        Authentication frames through SendAuth/ReceiveAuth, e.g. SAE),
//        2 PMF capable, 4 PMF required, 8 enterprise (EAPOL frames other than
//        EAPOL-Key pass to the network interface before the port opens),
//        16 station MAC present (the vdev is recreated with it); a block with
//        flags 16 alone (r.length = 8 + SSID + 10, AKM/RSN/RSNXE zero) is the
//        Private Wi-Fi Address request and is accepted in every build;
//   x[1] AKM suite type (00-0F-AC:n), x[2..7] station MAC, x[8] RSN length,
//   x[9..] the station's RSN element (sent in the association request and
//   expected in EAPOL message 2), then RSNXE length u8 + RSNXE.
//   r.length = 8 + SSID + 9 + RSN + 1 + RSNXE.
// SendAuth: data = Authentication frame body (algorithm, sequence, status
// code, payload); the core adds the 802.11 header, transmits it and resets
// the stage timer. ReceiveAuth: next queued Authentication frame body from
// the AP (o.length), kIOReturnNotReady when none. AuthResult index 1 ends
// external authentication successfully (the association request follows),
// index 0 fails the connection. All three need the external-auth stage.
// BSS: same prefix, [40]=channel, [41]=supported WPA2, [42..43]=RSN length, [44..]=RSN.
// Extensions keep ABI 1 and structure sizes; Status flag 512 advertises them.
// Scan: index 0 starts a fresh scan, index 1 cancels the active scan. Channels
// the regulatory rules let this host transmit on (no DFS, no NO-IR, or a
// beacon heard there before) are probed actively (40 ms), the rest passively
// (110 ms). Optional 42-byte block (Info capability 2048): channel mask u64
// over the kWMIScanPlan order (0 = all), flags u8 (1 = passive only), SSID
// length u8, SSID[32] probed by name besides the wildcard (hidden networks).
// RekeyGroup/RekeyPair: same 56-byte layout as InstallKeys. Same keys are rejected.
// Info data: [0] capabilities u32, [4] scan generation u32, [8] last channel u32,
// [12] RSSI signed u32 (INT32_MIN unavailable), [16] width u32, [20] NSS u32,
// [24] security generation u32, [28] state bits (1 suspended,2 quarantined,4 wake restart required),
// [32] SSID length + [33..64] raw SSID, [68..83] driver version.
// BSS extensions after RSN: SNR u32, estimate RSSI = SNR - 95 dBm (Linux convention).
// After SNR, optional BSS flags u32: bit 1 (value 2) means hidden SSID.
// After the flags, 105 bytes for the native front end: capabilities u16, beacon
// interval u16, rate count u8 + 32 rates, element bits u8 (1 HT caps, 2 HT op,
// 4 VHT caps, 8 VHT op), HT caps[26], HT op[22], VHT caps[12], VHT op[5],
// power constraint present u8 + dB u8.
// SecurityHold index 1 pauses ordinary TX so queued data can drain for EAPOL;
// RadioOptions index 0/1 requests STA power saving (no payload). Info bit 64 advertises it.
// Connect index 1 supplies the SSID of the selected hidden BSSID; Info bit 128 advertises it.
// Info bit 256 indicates infrastructure DFS association enabled in the kext policy.
// Info [84] PS requested/applied/busy bits (1/2/4), [88] beacon misses,
// [92] applied TX half-dBm, [96] AP Power Constraint dB, [100] radio transactions,
// [104] beacon timeout ms, [108] scan bits (1 completed,2 failed,4 cancel,8 started),
// [112..113] regulatory country alpha2 ("00" world), [115] bit 0 = native front end
// (AirportAth10k owns the interface). The original prefix and structure sizes are unchanged.
// Info [116] security u32: 1 external auth active, 2 PMF negotiated, 4 enterprise,
// 8 station MAC differs from the hardware MAC, 16 SA Query in progress; [120..125]
// the station MAC in use. Capabilities (Info [0]) bit 1024: this extension exists.
// Status local = the station MAC of the connection (the vdev MAC when idle).
// BSS extension after the 105 native bytes: RSNXE length u8 + up to 18 bytes.
// index 0 releases it after the serial EAPOL transaction.
// Cancel stops a connection at an ownership-safe boundary; Restart asynchronously
// terminates and rematches the PCI driver only after verified quiescence.
// InstallKeys: TK[16], GTK[32] (zero-padded when CCMP), group cipher[1] (2 TKIP, 4 CCMP),
// GTK index[1], GTK RSC[6]. Never persisted/logged. Length 81 appends the IGTK
// block: present[1], key id LE16, IPN[6], IGTK[16] (BIP-CMAC-128, verified in
// software by the core like Linux ath10k/mac80211). RekeyGroup/RekeyPair alike.
// Connect: kIOReturnUnsupported carries the reason in error (e.g.
// "configured-bss-not-found": the BSSID is not in the last scan's cache; a
// cancelled scan's partial cache is accepted).
// Disconnect: asynchronous, completed association only. Wait for flag 64 to clear.
// Flag 128 advertises reusable disconnect support; 64 means teardown in progress.
// Flags: 1 runtime, 2 scan done, 4 link, 8 TX busy, 16 secure, 32 EAPOL TX failed.
static_assert(sizeof(Request)==528 && sizeof(Response)==644,"control ABI");
}
