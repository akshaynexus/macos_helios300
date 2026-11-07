#pragma once
#include <stdint.h>
// Fixed-width, pointer-free ABI; all callers are x86_64, version checked per call.
namespace AthControl {
constexpr uint32_t version=1, clientType=0xa7100057;
enum Operation:uint32_t { Status, BSS, Connect, ReceiveEAPOL, SendEAPOL, InstallKeys, Authorize, Abort, Disconnect, Scan, Cancel, RekeyGroup, RekeyPair, Info, Restart, SecurityHold, RadioOptions };
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
// BSS: same prefix, [40]=channel, [41]=supported WPA2, [42..43]=RSN length, [44..]=RSN.
// Extensions keep ABI 1 and structure sizes; Status flag 512 advertises them.
// Scan: index 0 starts a fresh passive scan, index 1 cancels the active scan.
// RekeyGroup/RekeyPair: same 56-byte layout as InstallKeys. Same keys are rejected.
// Info data: [0] capabilities u32, [4] scan generation u32, [8] last channel u32,
// [12] RSSI signed u32 (INT32_MIN unavailable), [16] width u32, [20] NSS u32,
// [24] security generation u32, [28] state bits (1 suspended,2 quarantined,4 wake restart required),
// [32] SSID length + [33..64] raw SSID, [68..83] driver version.
// BSS extensions after RSN: SNR u32, estimate RSSI = SNR - 95 dBm (Linux convention).
// After SNR, optional BSS flags u32: bit 1 (value 2) means hidden SSID.
// SecurityHold index 1 pauses ordinary TX so queued data can drain for EAPOL;
// RadioOptions index 0/1 requests STA power saving (no payload). Info bit 64 advertises it.
// Connect index 1 supplies the SSID of the selected hidden BSSID; Info bit 128 advertises it.
// Info bit 256 indicates infrastructure DFS association enabled in the kext policy.
// Info [84] PS requested/applied/busy bits (1/2/4), [88] beacon misses,
// [92] applied TX half-dBm, [96] AP Power Constraint dB, [100] radio transactions,
// [104] beacon timeout ms. The original prefix and structure sizes are unchanged.
// index 0 releases it after the serial EAPOL transaction.
// Cancel stops a connection at an ownership-safe boundary; Restart asynchronously
// terminates and rematches the PCI driver only after verified quiescence.
// InstallKeys: TK[16], GTK[32] (zero-padded when CCMP), group cipher[1] (2 TKIP, 4 CCMP),
// GTK index[1], GTK RSC[6]. Never persisted/logged.
// Disconnect: asynchronous, completed association only. Wait for flag 64 to clear.
// Flag 128 advertises reusable disconnect support; 64 means teardown in progress.
// Flags: 1 runtime, 2 scan done, 4 link, 8 TX busy, 16 secure, 32 EAPOL TX failed.
static_assert(sizeof(Request)==528 && sizeof(Response)==644,"control ABI");
}
