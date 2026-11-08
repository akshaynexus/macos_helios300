// SPDX-License-Identifier: GPL-3.0-or-later
// Output path of the native front end, as pure logic (host-tested in
// native/tests/test_native_tx.cpp against a model of IONetworkingFamily 3.4's
// output pull model and of IOBasicOutputQueue).
//
// IONetworkingFamily 3.4 (macOS 26.7.2, 25G332) output pull model, read from
// the collection (IONetworkInterface::init, if_start_precheck, startOutputThread,
// haltOutputThread, signalOutputThread, drainOutputQueue):
//  - IONetworkInterface::init sets the TX thread state to 0x801: bit 0 "never
//    run" and bit 11 "halted". if_start_precheck clears bit 0 on the first
//    start; while bit 11 is set every if_start returns without calling the
//    driver. Only startOutputThread clears bit 11 (and the stop request 0x100).
//    A driver that never calls it never sees outputStart: the ifnet send queue
//    fills, nothing is dequeued and, since the pull model counts output in
//    dequeueOutputPackets (ifnet_stat_increment_out), Opkts stays 0.
//    That was hardware run 7: join, RX and IPv6 RA fine, `netstat` Opkts 0.
//  - drainOutputQueue loops while the send queue is not empty: it clears the
//    signal word, calls outputStart (IONetworkController vtable slot 325);
//    kIOReturnSuccess -> check the queue again; kIOReturnNoResources -> it
//    marks "waiting" and returns unless signalOutputThread ran meanwhile;
//    signalOutputThread restarts it (ifnet_start) only from "waiting".
//  - stopOutputThread = haltOutputThread(0x100). Called inside the gate of the
//    interface's TX work loop (the controller's, kIONetworkWorkLoopSynchronous)
//    it halts at once; elsewhere it waits for the start thread. The front end
//    calls it from the IO80211 work-queue gate (session link-down) and from
//    stop() (no gate held), never from the core's loop.
// AirportItlwm (macOS 26) does the same: startOutputThread on link-up,
// stopOutputThread + flushOutputQueue on link-down (AirportItlwmV2::setLinkStatus).
#pragma once
#include <stdint.h>

namespace AirportTx {
static constexpr int kSuccess=0;
static constexpr int kNoResources=int(0xe00002be); // kIOReturnNoResources

// outputStart: move packets from the interface (dequeueOutputPackets, one at a
// time) into the gated output queue on the core's work loop while it has room.
// Success when the interface queue is empty; NoResources when the gated queue
// is full (the core signals the output thread when it has drained it,
// Ath10kPCIDevice::serviceTxQueue). Ops: ready(), size(), capacity(),
// dequeue() -> packet or null, enqueue(packet).
struct PullStats { uint32_t calls=0,packets=0,full=0; };
template<class Ops> int pull(Ops &o,PullStats &stats){
    ++stats.calls;
    if(!o.ready()){++stats.full;return kNoResources;}
    const uint32_t capacity=o.capacity();
    while(o.size()+1<capacity){
        auto packet=o.dequeue();
        if(!packet)return kSuccess;
        o.enqueue(packet);++stats.packets;
    }
    ++stats.full;return kNoResources;
}

// The interface's output thread follows the data link (the session's BSD link,
// which follows the core's authorized port). Ops: start(), stop(), flush().
class OutputThread {
public:
    template<class Ops> void link(Ops &o,bool up){
        if(up==running)return;
        running=up;
        if(up){o.start();++starts;}
        else {o.stop();o.flush();++stops;}
    }
    bool started() const {return running;}
    uint32_t starts=0,stops=0;
private:
    bool running=false;
};
} // namespace AirportTx
