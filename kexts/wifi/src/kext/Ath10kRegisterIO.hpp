#pragma once

#include <IOKit/IOLib.h>
#include <IOKit/IOTypes.h>

// Audited BAR0/MMIO abstraction. All target-facing code must pass through
// this class so alignment and BAR bounds remain checked in one place.
class Ath10kRegisterIO
{
public:
    using AccessHook=bool(*)(void *,UInt32,UInt64 &);
    using AccessEnd=void(*)(void *,UInt32,UInt64);
    // Install/remove only while provider interrupts and the work loop are drained.
    void setAccessHook(AccessHook hook,AccessEnd end,void *context){_context=context;_end=end;_hook=hook;}
    void attach(volatile UInt8 *base, UInt64 length)
    {
        _base = base;
        _length = length;
    }

    void detach()
    {
        setAccessHook(nullptr,nullptr,nullptr);
        _base = nullptr;
        _length = 0;
    }

    bool attached() const { return _base != nullptr && _length >= sizeof(UInt32); }
    UInt64 length() const { return _length; }

    bool valid32(UInt32 offset) const
    {
        if (!attached())
            return false;
        if ((offset & 0x3u) != 0)
            return false;
        return static_cast<UInt64>(offset) + sizeof(UInt32) <= _length;
    }

    bool read32(UInt32 offset, UInt32 &value) const
    {
        UInt64 token=0;
        if(!valid32(offset) || (_hook && !_hook(_context,offset,token)))return false;
        const bool ok=read32Direct(offset,value);
        if(_hook && _end)_end(_context,offset,token);
        return ok;
    }
    // Wake/RTC access only: same bounds and ordering without recursive wake.
    bool read32Direct(UInt32 offset,UInt32 &value) const
    {
        if (!valid32(offset))
            return false;
        OSSynchronizeIO();
        value = *reinterpret_cast<volatile UInt32 *>(_base + offset);
        OSSynchronizeIO();
        return true;
    }

    bool write32(UInt32 offset, UInt32 value) const
    {
        UInt64 token=0;
        if(!valid32(offset) || (_hook && !_hook(_context,offset,token)))return false;
        const bool ok=write32Direct(offset,value);
        if(_hook && _end)_end(_context,offset,token);
        return ok;
    }
    bool write32Direct(UInt32 offset,UInt32 value) const
    {
        if (!valid32(offset))
            return false;
        *reinterpret_cast<volatile UInt32 *>(_base + offset) = value;
        OSSynchronizeIO();
        return true;
    }

private:
    AccessHook _hook=nullptr;AccessEnd _end=nullptr;void *_context=nullptr;
    volatile UInt8 *_base = nullptr;
    UInt64 _length = 0;
};
