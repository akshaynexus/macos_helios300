// SPDX-License-Identifier: GPL-3.0-or-later
#include "AirportAth10kEthernetInterface.hpp"
#include "AirportAth10k.hpp"
#include <sys/kpi_mbuf.h>
#include <net/ethernet.h>
#include <net/if.h>

#define super IOEthernetInterface
OSDefineMetaClassAndStructors(AirportAth10kEthernetInterface, IOEthernetInterface)

bool AirportAth10kEthernetInterface::initWithSkywalkInterfaceAndProvider(IONetworkController *controller,IO80211SkywalkInterface *skywalk)
{
    if(!super::init(controller))return false;
    interface=skywalk;attached=false;return true;
}

IOReturn AirportAth10kEthernetInterface::prepareBSDGated(OSObject *,void *arg0,void *,void *,void *)
{
    auto *self=static_cast<AirportAth10kEthernetInterface *>(arg0);
    auto *controller=OSDynamicCast(IO80211Controller,self->getController());
    auto *queue=controller?controller->getWorkQueue():nullptr;
    if(!queue || !queue->inGate() || queue->onThread())return kIOReturnNotPermitted;
    return self->interface->prepareBSDInterface(self->getIfnet(),0);
}

IOReturn AirportAth10kEthernetInterface::attachToDataLinkLayer(IOOptionBits options,void *parameter)
{
    char name[IFNAMSIZ];
    // AirportItlwm calls the private IONetworkInterface::setInterfaceSubType(3)
    // here; macOS 26.7.2's IONetworkingFamily does not export it (kc-audit), and
    // the Wi-Fi sub-family is applied by IO80211InfraInterface::prepareBSDInterface.
    IOReturn ret=super::attachToDataLinkLayer(options,parameter);
    if(ret!=kIOReturnSuccess || !interface)return ret;
    UInt8 builtIn=0;
    interface->setProperty("built-in",OSData::withBytes(&builtIn,sizeof(builtIn)));
    snprintf(name,sizeof(name),"%s%u",ifnet_name(getIfnet()),ifnet_unit(getIfnet()));
    interface->setProperty("IOInterfaceName",OSString::withCString(name));
    interface->setProperty(kIOInterfaceUnit,OSNumber::withNumber(ifnet_unit(getIfnet()),8));
    interface->setProperty(kIOInterfaceNamePrefix,OSString::withCString(ifnet_name(getIfnet())));
    IOEthernetAddress address;
    if(OSDynamicCast(IOEthernetController,getController())->getHardwareAddress(&address)==kIOReturnSuccess)
        setProperty(kIOMACAddress,&address,kIOEthernetAddressSize);
    auto *controller=OSDynamicCast(AirportAth10k,getController());
    IOCommandGate *gate=controller?controller->commandGate():nullptr;
    ret=gate?gate->runAction(prepareBSDGated,this):kIOReturnNotReady;
    if(ret!=kIOReturnSuccess){
        AIRPORT_LOG("prepareBSDInterface failed 0x%x",ret);
        super::detachFromDataLinkLayer(options,parameter);attached=false;return ret;
    }
    attached=true;
    interface->registerService();
    return kIOReturnSuccess;
}

void AirportAth10kEthernetInterface::detachFromDataLinkLayer(IOOptionBits options,void *parameter)
{
    super::detachFromDataLinkLayer(options,parameter);attached=false;
}

// IOSkywalkFamily casts the BSD interface's provider; present the Skywalk interface.
IOService *AirportAth10kEthernetInterface::getProvider() const
{
    return attached?interface:super::getProvider();
}

bool AirportAth10kEthernetInterface::setLinkState(IO80211LinkState state)
{
    if(!getIfnet())return false;
    if(state==kIO80211NetworkLinkUp)ifnet_set_flags(getIfnet(),ifnet_flags(getIfnet())|(IFF_UP|IFF_RUNNING),(IFF_UP|IFF_RUNNING));
    else ifnet_set_flags(getIfnet(),ifnet_flags(getIfnet())&~(IFF_UP|IFF_RUNNING),0);
    return true;
}

// The core hands over one linear mbuf per frame (deliverNetworkRX). Keep the
// family's expectations anyway: a packet header and a contiguous Ethernet header.
static mbuf_t normalizeInputPacket(mbuf_t packet,UInt32 length)
{
    const size_t packetLength=length?length:mbuf_pkthdr_len(packet);
    if(!(mbuf_flags(packet)&MBUF_PKTHDR) || packetLength<ETHER_HDR_LEN){mbuf_freem(packet);return nullptr;}
    if(!mbuf_next(packet) && mbuf_len(packet)>=packetLength)return packet;
    unsigned int maxChunks=1;mbuf_t linear=nullptr;
    if(mbuf_allocpacket(MBUF_DONTWAIT,packetLength+ETHER_ALIGN,&maxChunks,&linear)){mbuf_freem(packet);return nullptr;}
    UInt8 *data=static_cast<UInt8 *>(mbuf_data(linear))+ETHER_ALIGN;
    if(mbuf_copydata(packet,0,packetLength,data) || mbuf_copy_pkthdr(linear,packet) || mbuf_setdata(linear,data,packetLength)){
        mbuf_freem(linear);mbuf_freem(packet);return nullptr;
    }
    mbuf_pkthdr_setlen(linear,packetLength);mbuf_pkthdr_setheader(linear,nullptr);mbuf_freem(packet);
    return linear;
}

UInt32 AirportAth10kEthernetInterface::inputPacket(mbuf_t packet,UInt32 length,IOOptionBits options,void *param)
{
    if(!packet)return 0;
    if(!getIfnet()){mbuf_freem(packet);return 0;}
    const bool direct=param==packet;
    packet=normalizeInputPacket(packet,length);
    if(!packet){
        IONetworkData *data=getParameter(kIONetworkStatsKey);
        auto *stats=data?const_cast<IONetworkStats *>(static_cast<const IONetworkStats *>(data->getBuffer())):nullptr;
        if(stats)stats->inputErrors++;
        return 0;
    }
    if(direct)param=packet;
    return super::inputPacket(packet,length,options,param);
}
