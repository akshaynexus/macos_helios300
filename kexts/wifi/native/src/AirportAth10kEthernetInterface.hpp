// SPDX-License-Identifier: GPL-3.0-or-later
// The BSD-facing IOEthernetInterface that fronts the IO80211 Skywalk
// interface (after laobamac/itlwm AirportItlwmEthernetInterface, commit
// 7204c69, Copyright © 2023 钟先耀, all rights reserved; used with permission
// per the repository owner, 2026-11-08). The ath10k core delivers received
// Ethernet frames to it (inputPacket) and takes its output through the
// gated queue AirportAth10k::createOutputQueue creates on the core's loop.
#pragma once
#include "AirportAth10kDefines.hpp"
#include <Airport/Apple80211.h>
#include <IOKit/network/IOEthernetInterface.h>

class AirportAth10kEthernetInterface : public IOEthernetInterface {
    OSDeclareDefaultStructors(AirportAth10kEthernetInterface)
public:
    bool initWithSkywalkInterfaceAndProvider(IONetworkController *,IO80211SkywalkInterface *);
    IOReturn attachToDataLinkLayer(IOOptionBits,void *) override;
    void detachFromDataLinkLayer(IOOptionBits,void *) override;
    UInt32 inputPacket(mbuf_t,UInt32 length=0,IOOptionBits options=0,void *param=0) override;
    IOService *getProvider() const override;
    bool setLinkState(IO80211LinkState);
private:
    static IOReturn prepareBSDGated(OSObject *,void *,void *,void *,void *);
    IO80211SkywalkInterface *interface=nullptr;
    bool attached=false;
};
