// WMI command sizes the kext builds for the owner's AP equal what Linux ath10k sent
// to the same QCA6174 firmware on the Helios 300 (evidence/linux-ath10k-load-trace.txt).
// The trace records WMI length (4-byte WMI header + TLVs); kext frames also carry
// the 8-byte HTC header, so kext bytes == Linux len + 8.
//
// AP (evidence/linux-scan-security-summary.txt): 5 GHz channel 44, VHT80 (center 42),
// 2x2, RSN CCMP/CCMP PSK, 8 OFDM basic+extended rates.
#include <cassert>
#include <cstring>
#include "Ath10kRX.hpp"
#include "Ath10kWMI.hpp"
using namespace Ath10k;

static ConnectionState heliosAP() {
    ConnectionState s;
    const UInt8 local[] = {2, 0, 0, 0, 0, 1}, ap[] = {2, 0, 0, 0, 0, 2};
    memcpy(s.local, local, 6); memcpy(s.bssid, ap, 6);
    s.ssidLength = 8; memcpy(s.ssid, "helios-t", 8);
    s.secure = true; s.aid = 5;
    s.regulatory.country = regulatoryCountry("US");
    s.regulatory.ceilingHalfDbm = 20;
    s.operatingChannel = 44; s.centerChannel = 42; s.width = 80; s.nss = 2;
    s.txPowerHalfDbm = 20;
    s.ap.beaconInterval = 100; s.ap.timPresent = true; s.ap.dtimPeriod = 1;
    const UInt8 rates[] = {0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c};
    s.ap.rateCount = 8; memcpy(s.ap.rates, rates, 8); s.ap.ratesValid = true;
    s.ap.htPresent = s.ap.htOperation = true; s.ap.ht[3] = 255; s.ap.ht[4] = 255;
    s.ap.vhtPresent = s.ap.vhtOperation = true;
    htcWriteLE16(s.ap.vht + 4, 0xfffa); htcWriteLE16(s.ap.vht + 8, 0xfffa);
    s.ap.vhtOp[0] = 1; s.ap.vhtOp[1] = 42; htcWriteLE16(s.ap.vhtOp + 3, 0xfffc);
    s.htEnabled = s.vhtEnabled = true;
    s.firmwareHT = s.firmwareVHT = true;
    s.rfChains = 2;
    // Negotiated from the association response: HT MCS 0-15 (2x2).
    s.htRateCount = 16; for (UInt8 i = 0; i < 16; ++i) s.htRates[i] = i;
    return s;
}

static UInt32 wmiLength(UInt32 kextFrameBytes) { assert(kextFrameBytes >= 8); return kextFrameBytes - 8; }

int main() {
    UInt8 f[256];
    ConnectionState s = heliosAP();

    // WMI_TLV_VDEV_START_REQUEST_CMDID len 112
    UInt32 n = buildConnectionWMI(f, sizeof(f), 2, s, LinkStage::Start);
    assert(n && connLE32(f + 8) == 0x5003 && wmiLength(n) == 112);
    assert(connLE32(f + 92) == 5220 && connLE32(f + 96) == 5210); // freq / center freq MHz

    // WMI_TLV_PEER_CREATE_CMDID len 24
    n = buildConnectionWMI(f, sizeof(f), 2, s, LinkStage::Peer);
    assert(n && connLE32(f + 8) == 0x6001 && wmiLength(n) == 24);

    // WMI_TLV_PEER_ASSOC_CMDID len 136: 8 legacy rates + 16 HT MCS (2x2) arrays.
    s.wmmConfigured = true;
    n = buildConnectionWMI(f, sizeof(f), 2, s, LinkStage::PeerAssoc, 2, 80);
    assert(n && connLE32(f + 8) == 0x6005 && wmiLength(n) == 136);

    // WMI_TLV_VDEV_UP_CMDID len 24
    n = buildConnectionWMI(f, sizeof(f), 2, s, LinkStage::Up);
    assert(n && connLE32(f + 8) == 0x5005 && wmiLength(n) == 24);

    // Init-time commands (Ath10kWMI.hpp) against the same trace:
    // INIT 220 (no host-memory chunks), ECHO 12, VDEV_CREATE 28, SET_REGDOMAIN 32.
    assert(wmiLength(kWMIInitFrameBytes) == 220);
    assert(wmiLength(kWMIEchoFrameBytes) == 12);
    assert(wmiLength(kWMIVdevCreateFrameBytes) == 28);
    assert(wmiLength(kWMIRegdomainFrameBytes) == 32);
    // SCAN_CHAN_LIST: 16 + 28 per channel; Linux sent 1052 for its 37-channel US list.
    assert(16 + 28 * 37 == 1052);
    return 0;
}
