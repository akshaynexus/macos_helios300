// SPDX-License-Identifier: GPL-3.0-or-later
// Ownership of the two Skywalk RegistrationInfo blocks of an IO80211 interface.
//
// IOSkywalkFamily 1.0 (macOS 26.7.2 25G332) keeps a RegistrationInfo pointer in
// IOSkywalkNetworkInterface::mExpansionData and another in
// IOSkywalkEthernetInterface::mExpansionData2. The family fills them itself only
// in registerNetworkInterface/registerEthernetInterface (IOMallocType, 0x108 and
// 0x130 bytes), which the IO80211Controller::attachInterface path never calls;
// yet prepareBSDInterface, initBSDInterfaceParameters, the media/capability
// ioctls and the wake-flag queries dereference both without a NULL check (the
// MTU sits at +0x4c). A driver on that path has to provide the blocks, as
// AirportItlwm does. The family then typed-frees them: the network block in
// IOSkywalkNetworkInterface::deregisterNetworkInterface (from ::stop and ::free,
// kalloc.type zone of 288 bytes) and the Ethernet block in
// IOSkywalkEthernetInterface::free. A block from IOMalloc(0x130) lives in
// kalloc.type.var 384, so that free panics ("not in the expected zone
// early.kalloc.288", teardown at reboot/shutdown and at the wake restart).
//
// Rule: the blocks are the driver's. They are handed to the family for the
// interface's active life and taken back (pointer cleared, memory freed by the
// allocator that made it) before the family's stop/free can reach them.
// deregisterNetworkInterface and IOSkywalkEthernetInterface::free both skip a
// NULL pointer. Header-only and allocator-agnostic so the host suite can test it.
#pragma once
#include <stddef.h>
#include <string.h>

namespace AirportRegistration {
// sizeof(IOSkywalkEthernetInterface::RegistrationInfo) on 1.0 (25G332):
// copyRegistrationInfo refuses any other value in the size field.
constexpr size_t kInfoBytes=0x130;

struct Blocks {
    void *network=nullptr,*ethernet=nullptr;
    size_t bytes=0;
    bool installed() const {return network!=nullptr && ethernet!=nullptr;}
    // Copies `info` into two fresh blocks and points the family's slots at them.
    template<typename Alloc,typename Free>
    bool install(void **familyNetwork,void **familyEthernet,const void *info,size_t n,Alloc alloc,Free release){
        if(!familyNetwork || !familyEthernet || !info || n!=kInfoBytes || network || ethernet)return false;
        network=alloc(n);ethernet=alloc(n);
        if(!network || !ethernet){
            if(network)release(network,n);
            if(ethernet)release(ethernet,n);
            network=ethernet=nullptr;return false;
        }
        memcpy(network,info,n);memcpy(ethernet,info,n);bytes=n;
        *familyNetwork=network;*familyEthernet=ethernet;
        return true;
    }
    // Takes the blocks back. A slot that no longer points at our block (the
    // family replaced it with its own typed allocation) is left alone: that
    // block is the family's to free. Safe to call more than once.
    template<typename Free>
    void release(void **familyNetwork,void **familyEthernet,Free release){
        if(network){
            if(familyNetwork && *familyNetwork==network)*familyNetwork=nullptr;
            release(network,bytes);network=nullptr;
        }
        if(ethernet){
            if(familyEthernet && *familyEthernet==ethernet)*familyEthernet=nullptr;
            release(ethernet,bytes);ethernet=nullptr;
        }
        if(!network && !ethernet)bytes=0;
    }
};
}
