#pragma once
#include <IOKit/IOTypes.h>
namespace Ath10kPayload {
struct FirmwareSegment { UInt32 address; UInt32 offset; UInt32 length; };
// Views only: binary arrays are compiled once, in Ath10kPCIDevice.cpp.
struct Profile {
    UInt16 device=0, subvendor=0, subdevice=0;
    UInt8 chip=0;
    UInt32 targetVersion=0;
    bool available=false;
    const char *kBoardKey="unavailable", *kBoardSelection="unavailable", *kBoardSHA256="unavailable";
    const UInt8 *kBoard=nullptr;
    UInt32 kBoardSize=0;
    const char *kFirmwareVersion="unavailable";
    UInt32 kFirmwareSize=0;
    const char *kFirmwareSHA256="unavailable";
    const UInt8 *kFirmware=nullptr;
    UInt32 kOTPSize=0;
    const char *kOTPSHA256="unavailable";
    const UInt8 *kOTP=nullptr;
    bool kOTPIgnoreResult=false, kSupportsSkipClockInit=false;
    const FirmwareSegment *kFirmwareSegments=nullptr;
    UInt32 kFirmwareSegmentCount=0, kFirmwareSegmentDataBytes=0, kFirmwareEntryAddress=0;
    bool matchesTarget(UInt32 version, UInt32 type, UInt32 bytes) const {
        return available && version==targetVersion && type==8 && bytes==12;
    }
    bool boardAddressValid(UInt32 address) const {
        if (!available || (address & 3)) return false;
        // Keep the tested 6174 layout exact. The 9377 uses the ROM pointer,
        // restricted to reserved board RAM, excluding Host Interest and SGMT code.
        if (device==0x003e) return address==0x401fc0u;
        return address>=0x401000u && UInt64(address)+kBoardSize<=0x404000u;
    }
};
const Profile &selectProfile(UInt16 vendor, UInt16 device, UInt16 subvendor, UInt16 subdevice, UInt8 chip);
}
