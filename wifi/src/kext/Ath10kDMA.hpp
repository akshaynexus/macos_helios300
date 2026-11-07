#pragma once

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOMapper.h>
#include <IOKit/IOTypes.h>

#ifndef ATH10K_PAYLOAD_CACHE
#define ATH10K_PAYLOAD_CACHE 1
#endif

// Host-side DMA helper. The important distinction is between CPU physical
// memory and the device-visible 32-bit DMA address. IODMACommand may satisfy
// the 32-bit constraint through a mapper or through internal buffering. The
// caller therefore must explicitly synchronize CPU writes before device DMA
// and device writes before CPU reads.
class Ath10kDMABuffer
{
public:
    enum class Role { Ring, Payload };
    bool allocate(UInt64 size, UInt64 alignment, IOMapper *mapper = nullptr, Role role = Role::Ring)
    {
        release();
        auto fail = [this, size, alignment](const char *reason, IOReturn status) {
            const UInt64 dmaAddress = _dmaAddress;
            const UInt64 segmentLength = _segmentLength;
            const UInt64 physicalAddress = _physical;
            release();
            _allocationFailure = reason;
            _allocationFailureStatus = status;
            _allocationFailureRequestedBytes = size;
            _allocationFailureDMAAddress = dmaAddress;
            _allocationFailureSegmentLength = segmentLength;
            _allocationFailurePhysicalAddress = physicalAddress;
            _allocationFailureAlignment = alignment;
            return false;
        };

        if (size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0)
            return fail("invalid-size-or-alignment", kIOReturnBadArgument);

        // Physical memory may live above 4 GiB. IODMACommand constrains the
        // device-visible address to 32 bits, as required by the legacy
        // QCA6174/QCA9377 CE descriptor format.
        const UInt64 physicalMask = 0xffffffffffffffffULL & ~(alignment - 1ULL);
        _descriptor = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task,
            kIOMemoryPhysicallyContiguous | kIODirectionInOut |
                (role == Role::Payload && ATH10K_PAYLOAD_CACHE ? kIOMapCopybackCache : kIOMapInhibitCache),
            size,
            physicalMask);

        if (!_descriptor)
            return fail("contiguous-descriptor-allocation", kIOReturnNoMemory);

        const IOReturn prepareStatus = _descriptor->prepare();
        if (prepareStatus != kIOReturnSuccess)
            return fail("descriptor-prepare", prepareStatus);
        _prepared = true;

        // A nullptr mapper is valid for IODMACommand. On systems where
        // IOMapper::copyMapperForDevice() returns nullptr, kMapped may still
        // satisfy the 32-bit restriction using the default mapping path and/or
        // internal double-buffering. 0.0.17 continues to allow this path with explicit synchronization.
        _command = IODMACommand::withSpecification(
            kIODMACommandOutputHost64,
            32,
            0,
            IODMACommand::kMapped,
            0,
            static_cast<UInt32>(alignment),
            mapper,
            nullptr);
        if (!_command)
            return fail("dma-command-create", kIOReturnNoMemory);

        const IOReturn descriptorStatus = _command->setMemoryDescriptor(_descriptor);
        if (descriptorStatus != kIOReturnSuccess)
            return fail("dma-set-memory-descriptor", descriptorStatus);
        _commandHasDescriptor = true;

        _virtual = _descriptor->getBytesNoCopy();
        _physical = _descriptor->getPhysicalAddress();
        _length = size;
        _alignment = alignment;
        _explicitMapper = (mapper != nullptr);
        _cached = role == Role::Payload && ATH10K_PAYLOAD_CACHE;

        UInt64 offset = 0;
        UInt64 previousDMAAddress = 0;
        UInt64 previousSegmentLength = 0;
        UInt64 coveredBytes = 0;
        bool firstSegment = true;
        while (coveredBytes < size) {
            IODMACommand::Segment64 segment{};
            const UInt64 previousOffset = offset;
            UInt32 numSegments = 1;
            const IOReturn segmentStatus =
                _command->gen64IOVMSegments(&offset, &segment, &numSegments);
            if (segmentStatus != kIOReturnSuccess)
                return fail("dma-segment-generation", segmentStatus);
            if (numSegments != 1)
                return fail("dma-segment-count-invalid", kIOReturnBadArgument);
            if (segment.fLength == 0 || segment.fLength > size - coveredBytes)
                return fail("dma-segment-length-invalid", kIOReturnBadArgument);
            if (offset <= previousOffset || offset > size || offset - previousOffset != segment.fLength)
                return fail("dma-segment-offset-invalid", kIOReturnBadArgument);

            if (firstSegment) {
                _dmaAddress = segment.fIOVMAddr;
                firstSegment = false;
            } else {
                if (previousDMAAddress > ~static_cast<UInt64>(0) - previousSegmentLength)
                    return fail("dma-segment-address-overflow", kIOReturnBadArgument);
                if (segment.fIOVMAddr != previousDMAAddress + previousSegmentLength)
                    return fail("dma-segments-not-contiguous", kIOReturnBadArgument);
            }

            previousDMAAddress = segment.fIOVMAddr;
            previousSegmentLength = segment.fLength;
            coveredBytes += segment.fLength;
            _segmentLength = coveredBytes;
        }
        if (coveredBytes != size)
            return fail("dma-segment-coverage-incomplete", kIOReturnBadArgument);

        if (!_virtual)
            return fail("dma-null-virtual-address", kIOReturnBadArgument);
        if (_dmaAddress == 0)
            return fail("dma-zero-address", kIOReturnBadArgument);
        if (_dmaAddress > 0xffffffffULL || size - 1 > 0xffffffffULL - _dmaAddress)
            return fail("dma-address-span-above-32bit", kIOReturnBadArgument);
        if ((_dmaAddress & (alignment - 1ULL)) != 0)
            return fail("dma-address-misaligned", kIOReturnBadArgument);

        // setMemoryDescriptor() auto-prepares the DMA command. If the command
        // had to create an internal bounce/double buffer, that prepare may have
        // copied the pre-zero contents. Zero the CPU-visible memory and then
        // explicitly synchronize it to the device-visible DMA buffer.
        bzero(_virtual, static_cast<size_t>(_length));
        OSSynchronizeIO();
        if (!syncForDevice())
            return fail("dma-sync-for-device", _lastSyncOut);
        _allocationFailure = "none";
        _allocationFailureStatus = kIOReturnSuccess;
        return true;
    }

    bool syncForDevice()
    {
        if (!_command || !_commandHasDescriptor)
            return false;
        const IOReturn ret = _command->synchronize(kIODirectionOut);
        _lastSyncOut = ret;
        if (ret == kIOReturnSuccess)
            ++_syncOutCount;
        return ret == kIOReturnSuccess;
    }

    bool syncFromDevice()
    {
        if (!_command || !_commandHasDescriptor)
            return false;
        const IOReturn ret = _command->synchronize(kIODirectionIn);
        _lastSyncIn = ret;
        if (ret == kIOReturnSuccess)
            ++_syncInCount;
        return ret == kIOReturnSuccess;
    }

    void release()
    {
        if (_command) {
            if (_commandHasDescriptor)
                _command->clearMemoryDescriptor();
            _command->release();
        }
        _command = nullptr;
        _commandHasDescriptor = false;

        if (_descriptor) {
            if (_prepared)
                _descriptor->complete();
            _descriptor->release();
        }
        _descriptor = nullptr;
        _prepared = false;

        _virtual = nullptr;
        _physical = 0;
        _dmaAddress = 0;
        _segmentLength = 0;
        _length = 0;
        _alignment = 0;
        _explicitMapper = false;
        _cached = false;
        _syncOutCount = 0;
        _syncInCount = 0;
        _lastSyncOut = kIOReturnNotReady;
        _lastSyncIn = kIOReturnNotReady;
        _allocationFailure = "not-run";
        _allocationFailureStatus = kIOReturnSuccess;
        _allocationFailureRequestedBytes = 0;
        _allocationFailureDMAAddress = 0;
        _allocationFailureSegmentLength = 0;
        _allocationFailurePhysicalAddress = 0;
        _allocationFailureAlignment = 0;
    }

    bool allocated() const
    {
        return _descriptor != nullptr && _command != nullptr && _virtual != nullptr;
    }

    void *virtualAddress() const { return _virtual; }
    IOPhysicalAddress physicalAddress() const { return _physical; }
    UInt64 dmaAddress() const { return _dmaAddress; }
    UInt64 segmentLength() const { return _segmentLength; }
    UInt64 length() const { return _length; }
    UInt64 alignment() const { return _alignment; }
    bool usesExplicitMapper() const { return _explicitMapper; }
    bool usesCache() const { return _cached; }
    const char *mapperModeName() const { return _explicitMapper ? "device-bound" : "default/null"; }
    UInt32 syncOutCount() const { return _syncOutCount; }
    UInt32 syncInCount() const { return _syncInCount; }
    IOReturn lastSyncOut() const { return _lastSyncOut; }
    IOReturn lastSyncIn() const { return _lastSyncIn; }
    const char *allocationFailure() const { return _allocationFailure; }
    IOReturn allocationFailureStatus() const { return _allocationFailureStatus; }
    UInt64 allocationFailureRequestedBytes() const { return _allocationFailureRequestedBytes; }
    UInt64 allocationFailureDMAAddress() const { return _allocationFailureDMAAddress; }
    UInt64 allocationFailureSegmentLength() const { return _allocationFailureSegmentLength; }
    UInt64 allocationFailurePhysicalAddress() const { return _allocationFailurePhysicalAddress; }
    UInt64 allocationFailureAlignment() const { return _allocationFailureAlignment; }

private:
    IOBufferMemoryDescriptor *_descriptor = nullptr;
    IODMACommand *_command = nullptr;
    bool _prepared = false;
    bool _commandHasDescriptor = false;

    void *_virtual = nullptr;
    IOPhysicalAddress _physical = 0;
    UInt64 _dmaAddress = 0;
    UInt64 _segmentLength = 0;
    UInt64 _length = 0;
    UInt64 _alignment = 0;
    bool _explicitMapper = false;
    bool _cached = false;
    UInt32 _syncOutCount = 0;
    UInt32 _syncInCount = 0;
    IOReturn _lastSyncOut = kIOReturnNotReady;
    IOReturn _lastSyncIn = kIOReturnNotReady;
    const char *_allocationFailure = "not-run";
    IOReturn _allocationFailureStatus = kIOReturnSuccess;
    UInt64 _allocationFailureRequestedBytes = 0;
    UInt64 _allocationFailureDMAAddress = 0;
    UInt64 _allocationFailureSegmentLength = 0;
    UInt64 _allocationFailurePhysicalAddress = 0;
    UInt64 _allocationFailureAlignment = 0;
};
