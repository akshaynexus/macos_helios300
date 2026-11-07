#include "Ath10kHTC.hpp"
#include <cassert>
#include <vector>

static std::vector<UInt8> ready(bool extended = true)
{
    // Literal firmware-format fixture, with nonzero reserved bytes.
    std::vector<UInt8> b = {0, 0, 12, 0, 0, 0, 0, 0,
                           1, 0, 40, 0, 0, 1, 9, 0x77, 1, 8, 0x20, 0xf1};
    if (!extended) { b.resize(16); b[2] = 8; }
    return b;
}

int main()
{
    Ath10k::HTCReadyInfo info{};
    auto b = ready();
    assert(!Ath10k::parseHTCReady(b.data(), b.size(), info));
    assert(info.credits == 40 && info.creditSize == 256 && info.maxEndpoints == 9);
    assert(info.extended && info.version == 1 && info.maxBundleMessages == 8);
    assert(info.altDataCreditSize == 0x120 && info.payloadBytes == 12);
    b = ready(false);
    assert(!Ath10k::parseHTCReady(b.data(), b.size(), info) && !info.extended);
    // Deliberately unaligned CPU address.
    b.insert(b.begin(), 0xff);
    assert(!Ath10k::parseHTCReady(b.data() + 1, b.size() - 1, info));
    assert(Ath10k::parseHTCReady(nullptr, 20, info));
    b = ready();
    for (unsigned n = 0; n < b.size(); ++n)
        assert(Ath10k::parseHTCReady(b.data(), n, info));
    auto reject = [&](unsigned offset, UInt8 value) {
        auto packet = ready(); packet[offset] = value;
        assert(Ath10k::parseHTCReady(packet.data(), packet.size(), info));
        assert(info.credits == 0); // A rejected message cannot publish READY resources.
    };
    reject(0, 1); reject(1, 0x10); reject(1, 4); reject(2, 0xff);
    reject(8, 2); reject(10, 0); reject(13, 0); reject(14, 0); reject(16, 2);
    b = ready(); b[1] = 1;
    assert(!Ath10k::parseHTCReady(b.data(), b.size(), info));
    // Partial extended READY must not be read as a complete structure.
    for (unsigned length = 9; length < 12; ++length) {
        b = ready(); b.resize(8 + length); b[2] = length;
        assert(Ath10k::parseHTCReady(b.data(), b.size(), info));
    }
    // Valid credit trailer: four-byte record header and one four-byte entry.
    b = ready(); b[1] = 2; b[2] = 20; b[4] = 8;
    b.insert(b.end(), {1, 4, 0, 0, 0, 1, 0, 0});
    assert(!Ath10k::parseHTCReady(b.data(), b.size(), info));
    assert(info.trailerBytes == 8 && info.payloadBytes == 12);
    b[21] = 8; // Record claims bytes past the end of the DMA completion.
    assert(Ath10k::parseHTCReady(b.data(), b.size(), info));
    b[21] = 3;
    assert(Ath10k::parseHTCReady(b.data(), b.size(), info));
    b[4] = 21;
    assert(Ath10k::parseHTCReady(b.data(), b.size(), info));
    b[4] = 3;
    assert(Ath10k::parseHTCReady(b.data(), b.size(), info));
    b = ready(); b.push_back(0);
    assert(Ath10k::parseHTCReady(b.data(), b.size(), info));
    // Unknown, bounded trailer records can be ignored for this diagnostic.
    b = ready(); b[1] = 2; b[2] = 17; b[4] = 5;
    b.insert(b.end(), {99, 1, 0, 0, 0x42, 0, 0, 0}); // Three alignment bytes.
    assert(!Ath10k::parseHTCReady(b.data(), b.size(), info));
    b.resize(2049);
    assert(Ath10k::parseHTCReady(b.data(), b.size(), info));
}
