// SPDX-License-Identifier: GPL-3.0-or-later
// Build-time selectors for the reconstructed Apple headers (native/include/Airport).
#pragma once
#ifndef __IO80211_TARGET
#define __IO80211_TARGET __MAC_26_0
#endif
#ifndef AIRPORT_WCL
#define AIRPORT_WCL 1
#endif
#ifndef IO80211FAMILY_V2
#define IO80211FAMILY_V2 1
#endif
#include <Availability.h>
#include <IOKit/IOLib.h>
#define AIRPORT_LOG(fmt,...) IOLog("AirportAth10k: " fmt "\n",##__VA_ARGS__)
// Bring-up breadcrumbs: one line per stage with a monotonic counter, so the
// last line before a hang names the step (log stream --predicate 'sender == "AirportAth10k"').
#include <libkern/OSAtomic.h>
extern volatile SInt32 gAirportAth10kStep;
#define AIRPORT_STEP(name) IOLog("AirportAth10k: step %d %s\n",OSIncrementAtomic(&gAirportAth10kStep)+1,name)
