#include "Ath10kBMI.hpp"
#include <cassert>
#include <cstring>
#include <vector>

int main()
{
    for (UInt32 size : {1u, 3u, 4u, 243u, 244u, 245u, 256u, 8124u}) {
        std::vector<UInt8> input(size);
        for (UInt32 i = 0; i < size; ++i) input[i] = i % 251;
        std::vector<UInt8> reconstructed;
        UInt32 offset = 0;
        while (offset < size) {
            Ath10k::BMIWriteMemoryRequest req;
            std::memset(&req, 0xaa, sizeof(req));
            UInt32 consumed = 0;
            auto wire = Ath10k::buildBMIWriteRequest(req, 0x401fc0 + offset,
                                                    input.data() + offset, size - offset, consumed);
            assert(wire >= 16 && wire <= 256 && wire % 4 == 0);
            assert(req.id == 3 && req.address == 0x401fc0 + offset);
            assert(req.length % 4 == 0 && req.length <= 244 && req.length + 12 == wire);
            assert(consumed > 0 && consumed <= size - offset);
            for (UInt32 i = consumed; i < req.length; ++i) assert(req.payload[i] == 0);
            reconstructed.insert(reconstructed.end(), req.payload, req.payload + consumed);
            offset += consumed;
        }
        assert(reconstructed == input);
    }
    Ath10k::BMIWriteMemoryRequest req{};
    UInt32 consumed = 123;
    UInt8 data[4]{};
    assert(!Ath10k::buildBMIWriteRequest(req, 0xffffffff, data, 4, consumed));
    assert(consumed == 0);
    assert(!Ath10k::buildBMIWriteRequest(req, 0xfffffffc, data, 5, consumed));
    assert(!Ath10k::buildBMIWriteRequest(req, 0x400001, data, 4, consumed));
    assert(!Ath10k::buildBMIWriteRequest(req, 0x400000, nullptr, 4, consumed));
    assert(!Ath10k::buildBMIWriteRequest(req, 0x400000, data, 0, consumed));
    assert(Ath10k::buildBMIWriteRequest(req, 0xfffffffc, data, 4, consumed) == 16);
    Ath10k::BMILZDataRequest lz{};
    std::vector<UInt8> lzInput(512, 0x7d);
    assert(!Ath10k::buildBMILZDataRequest(lz, nullptr, 4, consumed));
    assert(!Ath10k::buildBMILZDataRequest(lz, lzInput.data(), 0, consumed));
    assert(!Ath10k::buildBMILZDataRequest(lz, lzInput.data(), 3, consumed));
    assert(Ath10k::buildBMILZDataRequest(lz, lzInput.data(), 512, consumed) == 256);
    assert(consumed == 248 && lz.length == 248 && lz.id == 14);
    assert(Ath10k::buildBMILZDataRequest(lz, lzInput.data(), 4, consumed) == 12);
    assert(consumed == 4 && lz.length == 4 && lz.payload[4] == 0);
}
