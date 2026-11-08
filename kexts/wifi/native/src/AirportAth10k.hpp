// SPDX-License-Identifier: GPL-3.0-or-later
// AirportAth10k — IO80211Controller front end over the ath10k core
// (Ath10kPCIDevice, kexts/wifi/src/kext). It matches the core once the core
// publishes Ath10kNativeReady, creates the IO80211/Skywalk interface pair and
// the corecapture pipes WCL expects, and routes data between the BSD
// interface and the core. Structure after laobamac/itlwm AirportItlwmV2.*
// (commit 7204c69, Copyright © 2023 钟先耀, © 2026 laobamac, all rights
// reserved; used with permission per the repository owner, 2026-11-08).
#pragma once
#include "AirportAth10kDefines.hpp"
#include <Airport/Apple80211.h>
#include <Airport/macOS26/FaultReporter.h>
#include <IOKit/network/IOGatedOutputQueue.h>
#include <IOKit/IOCommandGate.h>
#include <kern/thread_call.h>
#include "AirportAth10kEthernetInterface.hpp"
#include "AirportAth10kInterface.hpp"
#include "AirportAth10kNub.hpp"
#include "AirportAth10kTx.hpp"
#include "Ath10kPCIDevice.hpp"

class AirportAth10k : public IO80211Controller {
    OSDeclareDefaultStructors(AirportAth10k)
public:
    bool init(OSDictionary *) override;
    void free() override;
    IOService *probe(IOService *,SInt32 *) override;
    bool start(IOService *) override;
    void stop(IOService *) override;
    IOReturn getHardwareAddress(IOEthernetAddress *) override;
    IOReturn setHardwareAddress(const void *,UInt32) override;
    IOReturn getPacketFilters(const OSSymbol *,UInt32 *) const override;
    IOReturn setPromiscuousMode(bool) override;
    IOReturn setMulticastMode(bool) override;
    IOReturn setMulticastList(IOEthernetAddress *,UInt32) override;
    UInt32 getFeatures() const override;
    const OSString *newVendorString() const override;
    const OSString *newModelString() const override;
    IOReturn selectMedium(const IONetworkMedium *) override;
    bool createWorkQueue() override;
    IOWorkLoop *getWorkLoop() const override;
    IO80211WorkQueue *getWorkQueue() const override;
    IONetworkInterface *createInterface() override;
    bool configureInterface(IONetworkInterface *) override;
    IOOutputQueue *createOutputQueue() override;
    UInt32 outputPacket(mbuf_t,void *) override;
    // Output pull model (IFEF_TXSTART). IONetworkController::outputStart is
    // private SPI: the macOS 26.5 SDK shows its vtable slot as the pad
    // _RESERVEDIONetworkController2 (real IONetworkingFamily 3.4 slot 325 is
    // outputStart(IONetworkInterface*, unsigned int), see tools/kc-audit.py
    // --dump IONetworkController). The pad is overridden and its symbol aliased
    // to outputStart below, so the family calls outputStart with its real ABI.
    // Without the pull model the ifnet has no TXSTART, dlil attaches no Skywalk
    // flowswitch and airportd does not treat the interface as Wi-Fi (run 3).
    void _RESERVEDIONetworkController2() override __asm__("__ZN13AirportAth10k11outputStartEP18IONetworkInterfacej");
    IOReturn outputStart(IONetworkInterface *,IOOptionBits);
    bool setLinkStatus(UInt32,const IONetworkMedium *activeMedium=0,UInt64 speed=0,OSData *data=0) override;
    IOReturn enable(IONetworkInterface *) override;
    IOReturn disable(IONetworkInterface *) override;
    IOReturn registerWithPolicyMaker(IOService *) override;
    IOReturn setPowerState(unsigned long,IOService *) override;
    // IO80211Controller pure virtuals
    bool isCommandProhibited(int) override { return false; }
    SInt32 handleCardSpecific(IO80211SkywalkInterface *,unsigned long,void *,bool) override { return 0; }
    IOReturn getDRIVER_VERSION(IO80211SkywalkInterface *,apple80211_version_data *) override;
    IOReturn getHARDWARE_VERSION(IO80211SkywalkInterface *,apple80211_version_data *) override;
    IOReturn getCARD_CAPABILITIES(IO80211SkywalkInterface *,apple80211_capability_data *) override;
    IOReturn getPOWER(IO80211SkywalkInterface *,apple80211_power_data *) override;
    IOReturn setPOWER(IO80211SkywalkInterface *,apple80211_power_data *) override;
    IOReturn getCOUNTRY_CODE(IO80211SkywalkInterface *,apple80211_country_code_data *) override;
    IOReturn setCOUNTRY_CODE(IO80211SkywalkInterface *,apple80211_country_code_data *) override;
    IOReturn setGET_DEBUG_INFO(IO80211SkywalkInterface *,apple80211_debug_command *) override { return kIOReturnSuccess; }
    CCLogStream *getLogger() const override { return driverLogger; }
    CommonFaultReporter *getFaultReporterFromDriver() override { return driverFaultReporter; }
    bool getLogPipes(CCPipe **,CCPipe **,CCPipe **) override;
    SInt32 enableFeature(IO80211FeatureCode,void *) override;
    bool requiresExplicitMBufRelease() override { return false; }
    bool flowIdSupported() override { return false; }
    SInt32 monitorModeSetEnabled(bool,unsigned int) override { return kIOReturnSuccess; }
    IOReturn requestQueueSizeAndTimeout(unsigned short *,unsigned short *) override { return kIOReturnSuccess; }
    // For the interface
    Ath10kPCIDevice *core() const { return ath10k; }
    // The core's IONetworkController entry points are private in its class; call them through the base.
    IOEthernetController *coreController() const { return ath10k; }
    IOCommandGate *commandGate() const { return gate; }
    AirportAth10kEthernetInterface *bsdInterface() const { return bsd; }
    void postInterfaceMessage(unsigned int,void *,unsigned long);
    void nativeLink(bool up);
    // Output path counters (outputStart runs under the IO80211 work-queue gate,
    // as the poll that mirrors them): AirportAth10kTx* on the controller.
    const AirportTx::PullStats &txStats() const {return pullStats;}
    const AirportTx::OutputThread &txThread() const {return outputThread;}
private:
    bool bringUp(IOService *);
    static void deferredStart(thread_call_param_t,thread_call_param_t);
    thread_call_t deferredCall=nullptr;
    IOService *deferredProvider=nullptr;
    bool broughtUp=false;
    bool initCCLogs();
    void releaseAll();
    AirportAth10kNub *nub=nullptr;
    Ath10kPCIDevice *ath10k=nullptr;
    IO80211WorkQueue *workQueue=nullptr;
    IOCommandGate *gate=nullptr;
    AirportAth10kInterface *netif=nullptr;
    AirportAth10kEthernetInterface *bsd=nullptr;
    const IONetworkMedium *activeMedium=nullptr;
    CCPipe *driverLogPipe=nullptr,*driverDataPathPipe=nullptr,*driverSnapshotsPipe=nullptr;
    bool logPipeStarted=false,dataPathPipeStarted=false,snapshotsPipeStarted=false;
    CCLogStream *driverLogger=nullptr;
    CCStream *driverFaultStream=nullptr;
    CCFaultReporter *driverCoreFaultReporter=nullptr;
    IO80211FaultReporter *driverFaultReporter=nullptr;
    IOService *pmPolicyMaker=nullptr;
    unsigned long pmState=1;
    uint32_t powerState=1;
    bool coreAttached=false,interfaceAttached=false;
    // The BSD interface's output thread (stopped until the link is up, see
    // AirportAth10kTx.hpp) and what outputStart moved.
    AirportTx::OutputThread outputThread;
    AirportTx::PullStats pullStats;
    void setOutputThread(bool up);
    char geoCountry[3]{};
};
