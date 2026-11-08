// SPDX-License-Identifier: GPL-3.0-or-later
#include "AirportAth10kNub.hpp"

#define super IOService
OSDefineMetaClassAndStructors(AirportAth10kNub, IOService)

static IOPMPowerState nubPowerStates[2]={
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,kIOPMDeviceUsable,kIOPMPowerOn,kIOPMPowerOn,0,0,0,0,0,0,0,0}};

IOService *AirportAth10kNub::probe(IOService *provider,SInt32 *score)
{
    // Kill switch: -airportath10k_off leaves the core up (Ath10kNativeReady,
    // no interface) and never starts the front end.
    if(PE_parse_boot_argn("-airportath10k_off",nullptr,0)){AIRPORT_LOG("disabled by -airportath10k_off");return nullptr;}
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
    UInt8 builtIn=0;
    setProperty("built-in",OSData::withBytes(&builtIn,sizeof(builtIn)));
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
    if(pmStarted){PMstop();pmStarted=false;}
    super::stop(provider);
    if(workQueue){workQueue->release();workQueue=nullptr;}
    ath10k=nullptr;
}
