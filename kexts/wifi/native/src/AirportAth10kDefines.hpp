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
#include <IOKit/IOService.h>
// Logging (AirportAth10kNub.cpp). Every line goes to IOLog and the last one is
// mirrored into the registry on the nub (AirportAth10kLastMessage,
// AirportAth10kLastStep), so the state survives a boot without the verbose
// console: ioreg -l -w0 | grep AirportAth10k. Neither is called from the poll
// timer or any wait loop (printing in a loop hung the PG106 boots).
void airportLog(const char *fmt,...) __attribute__((format(printf,1,2)));
// Bring-up breadcrumbs: one line per stage with a monotonic counter, so the
// last line before a hang names the step (log stream --predicate 'sender == "AirportAth10k"').
void airportStep(const char *name);
// Registry sink for the mirrored state: the nub while it exists (set in
// AirportAth10kNub::start, cleared in its stop), nullptr otherwise.
void airportSetStateSink(IOService *);
#define AIRPORT_LOG(fmt,...) airportLog(fmt,##__VA_ARGS__)
#define AIRPORT_STEP(name) airportStep(name)
