#pragma once
#include "Ath10kHTC.hpp"
#include "Ath10kRXRate.hpp"

#ifndef ATH10K_RUNTIME_IRQ
#define ATH10K_RUNTIME_IRQ 1
#endif
#ifndef ATH10K_RX_BATCH
#define ATH10K_RX_BATCH 1
#endif
#ifndef ATH10K_RX_RESERVE
#define ATH10K_RX_RESERVE 1
#endif
namespace Ath10k {
static constexpr UInt32 kRXReserveCapacity=256,kRXReserveLowWater=128,kRXReserveRefillBudget=64;
static constexpr UInt32 kRXDeliveryBatchLimit=64,kOutputQueueCapacity=256;
struct Duration {
    UInt64 totalNS=0,maxNS=0;UInt32 count=0;
    void add(UInt64 ns){totalNS+=ns;if(ns>maxNS)maxNS=ns;++count;}
};
struct PerformanceState {
    // Reserve counters are protected by the reserve lock; all other counters
    // belong to the controller's shared work-loop gate.
    UInt32 reserveRefills=0,reserveFailures=0,reserveHits=0,reserveMisses=0;
    UInt32 rxFlushes=0,rxFlushMax=0,rxDirect=0,rxAssembly=0,txWakeups=0;
    Duration worker{},rx{},delivery{};
    RXRateState rate{};
};
UInt64 uptimeNS();
}
