// SPDX-License-Identifier: GPL-3.0-or-later
#include "AirportAth10kNub.hpp"

#include <libkern/OSAtomic.h>
#include <stdarg.h>

#define super IOService
OSDefineMetaClassAndStructors(AirportAth10kNub, IOService)

static volatile SInt32 gAirportAth10kStep=0;
static IOService *volatile gAirportAth10kStateSink=nullptr;
void airportSetStateSink(IOService *sink){gAirportAth10kStateSink=sink;}
static void airportMirror(const char *key,const char *value)
{
    IOService *sink=gAirportAth10kStateSink;
    if(sink && !sink->isInactive())sink->setProperty(key,value);
}
void airportLog(const char *fmt,...)
{
    char line[192];
    va_list ap;va_start(ap,fmt);vsnprintf(line,sizeof(line),fmt,ap);va_end(ap);
    IOLog("AirportAth10k: %s\n",line);
    airportMirror("AirportAth10kLastMessage",line);
}
void airportStep(const char *name)
{
    char line[160];
    snprintf(line,sizeof(line),"%d %s",OSIncrementAtomic(&gAirportAth10kStep)+1,name);
    IOLog("AirportAth10k: step %s\n",line);
    airportMirror("AirportAth10kLastStep",line);
}

static IOPMPowerState nubPowerStates[2]={
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,kIOPMDeviceUsable,kIOPMPowerOn,kIOPMPowerOn,0,0,0,0,0,0,0,0}};

IOService *AirportAth10kNub::probe(IOService *provider,SInt32 *score)
{
    // Safe mode (-x, OpenCore Shift+Enter / macOS safe boot): stand down after
    // one line, so a bad build is always bypassed from the boot menu. The core
    // declines in the same way, so this is reached only when the core was
    // forced in; it still refuses.
    if(PE_parse_boot_argn("-x",nullptr,0)){IOLog("AirportAth10k: safe mode (-x): not attaching\n");return nullptr;}
    // Kill switches: -airportath10k_off disables the whole stack (the core
    // declines too, so this is only reached if it was forced in);
    // -airportath10k_frontoff leaves the core up (Ath10kNativeReady, no
    // interface) and never starts the front end.
    if(PE_parse_boot_argn("-airportath10k_off",nullptr,0)){AIRPORT_LOG("disabled by -airportath10k_off");return nullptr;}
    if(PE_parse_boot_argn("-airportath10k_frontoff",nullptr,0)){AIRPORT_LOG("disabled by -airportath10k_frontoff");return nullptr;}
    AIRPORT_STEP("nub probe");
    auto *device=OSDynamicCast(Ath10kPCIDevice,provider);
    if(!device || !device->nativeFrontEnd()){AIRPORT_LOG("provider is not an Ath10kPCIDevice in native mode");return nullptr;}
    IOEthernetAddress address;
    if(static_cast<IOEthernetController *>(device)->getHardwareAddress(&address)!=kIOReturnSuccess){AIRPORT_LOG("core has no MAC address yet");return nullptr;}
    return super::probe(provider,score);
}

bool AirportAth10kNub::start(IOService *provider)
{
    AIRPORT_STEP("nub start");
    ath10k=OSDynamicCast(Ath10kPCIDevice,provider);
    if(!ath10k)return false;
    workQueue=IO80211WorkQueue::workQueue();
    if(!workQueue){AIRPORT_LOG("no IO80211WorkQueue");return false;}
    if(!super::start(provider)){workQueue->release();workQueue=nullptr;return false;}
    airportSetStateSink(this);
    UInt8 builtIn=0;
    if(OSData *d=OSData::withBytes(&builtIn,sizeof(builtIn))){setProperty("built-in",d);d->release();} // setProperty retains
    PMinit();
    registerPowerDriver(this,nubPowerStates,2);
    provider->joinPMtree(this);
    pmStarted=true;
    AIRPORT_STEP("nub registered (work queue owner)");
    registerService();
    return true;
}

void AirportAth10kNub::stop(IOService *provider)
{
    AIRPORT_STEP("nub stop");
    airportSetStateSink(nullptr);
    if(pmStarted){PMstop();pmStarted=false;}
    super::stop(provider);
    if(workQueue){workQueue->release();workQueue=nullptr;}
    ath10k=nullptr;
}
