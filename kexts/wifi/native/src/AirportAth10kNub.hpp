// SPDX-License-Identifier: GPL-3.0-or-later
// AirportAth10kNub — the provider nub between the ath10k core and the
// IO80211Controller, after AirportItlwm's IOPCIEDeviceWrapper. It owns the
// shared IO80211WorkQueue and returns it from getWorkLoop(), which matters:
// IONetworkController::start compares getWorkLoop() with its provider's and,
// when they differ, calls ml_thread_policy(getWorkLoop()->getThread(), ...).
// IO80211WorkQueue::getThread() returns NULL on macOS 26 (1200.13.1), so a
// controller whose provider has another work loop (the core's) panics there
// (page fault at thread->mutex, 0x658, the first two hardware runs). With the
// nub as provider the loops are equal and that path is skipped, as in AirportItlwm.
#pragma once
#include "AirportAth10kDefines.hpp"
#include <Airport/Apple80211.h>
#include "Ath10kPCIDevice.hpp"

class AirportAth10kNub : public IOService {
    OSDeclareDefaultStructors(AirportAth10kNub)
public:
    IOService *probe(IOService *,SInt32 *) override;
    bool start(IOService *) override;
    void stop(IOService *) override;
    IOWorkLoop *getWorkLoop() const override { return workQueue; }
    IOReturn setPowerState(unsigned long,IOService *) override { return IOPMAckImplied; }
    Ath10kPCIDevice *core() const { return ath10k; }
    IO80211WorkQueue *queue() const { return workQueue; }
private:
    Ath10kPCIDevice *ath10k=nullptr;
    IO80211WorkQueue *workQueue=nullptr;
    bool pmStarted=false;
};
