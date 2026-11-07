#pragma once

#include <IOKit/IOTypes.h>
#include "Ath10kCE.hpp"
#include "Ath10kDMA.hpp"

enum class Ath10kCERingDirection : UInt8 {
    Source = 0,
    Destination = 1,
};

class Ath10kCERing
{
public:
    bool allocate(UInt32 ceId, Ath10kCERingDirection direction, UInt32 entries,
                  IOMapper *mapper)
    {
        release();

        if (ceId >= Ath10k::kCECount || !Ath10k::isPowerOfTwo(entries)) {
            _allocationFailure = "invalid-ce-or-entry-count";
            _allocationFailureStatus = kIOReturnBadArgument;
            return false;
        }

        const UInt64 bytes = static_cast<UInt64>(entries) * Ath10k::kCEDescriptorSize;
        if (bytes == 0 || bytes > 0xffffffffULL) {
            _allocationFailure = "invalid-ring-size";
            _allocationFailureStatus = kIOReturnBadArgument;
            return false;
        }

        if (!_dma.allocate(bytes, Ath10k::kCEDescriptorRingAlign, mapper)) {
            _allocationFailure = _dma.allocationFailure();
            _allocationFailureStatus = _dma.allocationFailureStatus();
            _allocationFailureRequestedBytes = _dma.allocationFailureRequestedBytes();
            _allocationFailureDMAAddress = _dma.allocationFailureDMAAddress();
            _allocationFailureSegmentLength = _dma.allocationFailureSegmentLength();
            _allocationFailurePhysicalAddress = _dma.allocationFailurePhysicalAddress();
            _allocationFailureAlignment = _dma.allocationFailureAlignment();
            return false;
        }

        _ceId = ceId;
        _direction = direction;
        _entries = entries;
        _bytes = bytes;
        _allocationFailure = "none";
        _allocationFailureStatus = kIOReturnSuccess;
        _allocationFailureRequestedBytes = 0;
        _allocationFailureDMAAddress = 0;
        _allocationFailureSegmentLength = 0;
        _allocationFailurePhysicalAddress = 0;
        _allocationFailureAlignment = 0;
        return true;
    }

    void release()
    {
        _dma.release();
        _ceId = 0;
        _direction = Ath10kCERingDirection::Source;
        _entries = 0;
        _bytes = 0;
        _allocationFailure = "not-run";
        _allocationFailureStatus = kIOReturnSuccess;
        _allocationFailureRequestedBytes = 0;
        _allocationFailureDMAAddress = 0;
        _allocationFailureSegmentLength = 0;
        _allocationFailurePhysicalAddress = 0;
        _allocationFailureAlignment = 0;
    }

    bool allocated() const { return _dma.allocated() && _entries != 0; }
    UInt32 ceId() const { return _ceId; }
    Ath10kCERingDirection direction() const { return _direction; }
    UInt32 entries() const { return _entries; }
    UInt64 bytes() const { return _bytes; }
    UInt32 dmaAddress32() const { return static_cast<UInt32>(_dma.dmaAddress()); }
    UInt64 dmaAddress() const { return _dma.dmaAddress(); }
    IOPhysicalAddress physicalAddress() const { return _dma.physicalAddress(); }
    bool usesExplicitMapper() const { return _dma.usesExplicitMapper(); }
    const char *mapperModeName() const { return _dma.mapperModeName(); }
    void *virtualAddress() const { return _dma.virtualAddress(); }
    bool syncForDevice() { return _dma.syncForDevice(); }
    bool syncFromDevice() { return _dma.syncFromDevice(); }
    UInt32 syncOutCount() const { return _dma.syncOutCount(); }
    UInt32 syncInCount() const { return _dma.syncInCount(); }
    const char *allocationFailure() const { return _allocationFailure; }
    IOReturn allocationFailureStatus() const { return _allocationFailureStatus; }
    UInt64 allocationFailureRequestedBytes() const { return _allocationFailureRequestedBytes; }
    UInt64 allocationFailureDMAAddress() const { return _allocationFailureDMAAddress; }
    UInt64 allocationFailureSegmentLength() const { return _allocationFailureSegmentLength; }
    UInt64 allocationFailurePhysicalAddress() const { return _allocationFailurePhysicalAddress; }
    UInt64 allocationFailureAlignment() const { return _allocationFailureAlignment; }

    const char *directionName() const
    {
        return _direction == Ath10kCERingDirection::Source ? "src" : "dst";
    }

private:
    Ath10kDMABuffer _dma;
    UInt32 _ceId = 0;
    Ath10kCERingDirection _direction = Ath10kCERingDirection::Source;
    UInt32 _entries = 0;
    UInt64 _bytes = 0;
    const char *_allocationFailure = "not-run";
    IOReturn _allocationFailureStatus = kIOReturnSuccess;
    UInt64 _allocationFailureRequestedBytes = 0;
    UInt64 _allocationFailureDMAAddress = 0;
    UInt64 _allocationFailureSegmentLength = 0;
    UInt64 _allocationFailurePhysicalAddress = 0;
    UInt64 _allocationFailureAlignment = 0;
};
