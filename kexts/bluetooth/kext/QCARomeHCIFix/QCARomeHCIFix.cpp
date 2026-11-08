// SPDX-License-Identifier: GPL-2.0-or-later
//
// QCARomeHCIFix: Lilu plugin that watches the HCI traffic of the Helios 300's
// Rome Bluetooth controller (04ca:3016) on its way between macOS's Bluetooth
// stack and the USB bus. QCARomePatchRAM stays a plain firmware loader; this
// is separate so that a problem here can never stop the firmware load.
//
// What it does (the decisions are in src/core/qca_hci_filter.c, host-tested):
//   - Rewrites Enhanced Setup/Accept Synchronous Connection (0x043D/0x043E),
//     which Linux marks broken on this chip, to the legacy 0x0428/0x0429,
//     and rewrites the answer back. Inert if the stack never sends them.
//   - Counts HCI commands the firmware does not answer and, as Linux's
//     btusb_qca_reset() does, resets the USB device after five of them.
//     QCARomePatchRAM's new instance then patches the controller again.
//
// Hooks (the same three places IntelBTPatcher uses on Tahoe):
//   IOUSBHostDevice::deviceRequest(7 args)  every overload funnels here
//   IOUSBHostIOSource::io (three overloads) Pipe::io tail-jumps into these
//
// Everything is gated on the device being 04ca:3016, and every path that
// depends on seeing the controller's events waits until at least one event
// has been seen: with an unproven event hook a translated command's answer
// could not be rewritten, and every command would look unanswered.
//
// Boot args:  -qcahcioff   the plugin does not load (Lilu)
//             -qcahcinosco no command translation; the watchdog still runs
//             -qcahcinowd  no watchdog; translation still runs
//             -qcahcidbg   Lilu debug logging for this plugin
// Safe mode (-x): Lilu does not start the plugin.
#include <Headers/kern_api.hpp>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/plugin_start.hpp>

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostIOSource.h>
#include <IOKit/usb/IOUSBHostPipe.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <mach/kmod.h>

#include "qca_hci_filter.h"

#define LOG(fmt, ...) IOLog("QCARomeHCIFix: " fmt "\n", ##__VA_ARGS__)

#define QCA_VENDOR_ID  0x04ca
#define QCA_PRODUCT_ID 0x3016

namespace {

const char *bootargOff[] = {"-qcahcioff"};
const char *bootargDebug[] = {"-qcahcidbg"};
const char *bootargBeta[] = {"-qcahcibeta"};

const char *pathIOUSBHostFamily[] = {
    "/System/Library/Extensions/IOUSBHostFamily.kext/Contents/MacOS/IOUSBHostFamily"};

KernelPatcher::KextInfo kextIOUSBHostFamily{
    "com.apple.iokit.IOUSBHostFamily", pathIOUSBHostFamily, 1, {true, true}, {},
    KernelPatcher::KextInfo::Unloaded};

mach_vm_address_t orgDeviceRequest, orgIoCompletion, orgIoSync, orgIoBundled;

IOSimpleLock *sLock;
thread_call_t sTick;
bool sTickArmed;            // sLock
bool sScoEnabled = true;    // set before the hooks exist
bool sWdEnabled = true;     // set before the hooks exist
IOUSBHostDevice *volatile sDevice; // retained; replaced under sLock
qca_sco_pending sPending;   // sLock
qca_wd sWd;                 // sLock

struct Stats {
    uint32_t translatedSetup, translatedAccept, refused, unproven, rewritten, resets;
    uint16_t lastVoice;
    const char *lastRefused;
    bool latchLogged;
} sStats; // sLock

struct Guard {
    IOInterruptState st;
    Guard() { st = IOSimpleLockLockDisableInterrupt(sLock); }
    ~Guard() { IOSimpleLockUnlockEnableInterrupt(sLock, st); }
};

uint64_t nowMs()
{
    uint64_t t = 0, ns = 0;
    clock_get_uptime(&t);
    absolutetime_to_nanoseconds(t, &ns);
    return ns / 1000000ULL;
}

void tickFired(thread_call_param_t, thread_call_param_t);

// Caller does not hold sLock.
void armTick(uint32_t ms)
{
    if (!sTick || ms == 0)
        return;
    {
        Guard g;
        if (sTickArmed)
            return;
        sTickArmed = true;
    }
    uint64_t deadline = 0;
    clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
    thread_call_enter_delayed(sTick, deadline);
}

void publish(const Stats &s, const qca_wd &w)
{
    auto *self = ADDPR(selfInstance);
    if (!self)
        return;
    self->setProperty("TranslatedSetup", (unsigned long long)s.translatedSetup, 32);
    self->setProperty("TranslatedAccept", (unsigned long long)s.translatedAccept, 32);
    self->setProperty("Refused", (unsigned long long)s.refused, 32);
    self->setProperty("RefusedBeforeEvents", (unsigned long long)s.unproven, 32);
    self->setProperty("AnswersRewritten", (unsigned long long)s.rewritten, 32);
    self->setProperty("LastVoiceSetting", (unsigned long long)s.lastVoice, 16);
    self->setProperty("LastRefusedReason", s.lastRefused ? s.lastRefused : "none");
    self->setProperty("EventsSeen", (unsigned long long)w.events, 32);
    self->setProperty("CommandTimeouts", (unsigned long long)w.total_timeouts, 32);
    self->setProperty("USBResets", (unsigned long long)s.resets, 32);
    self->setProperty("RecoveryExhausted", w.latched != 0);
}

void tickFired(thread_call_param_t, thread_call_param_t)
{
    qca_wd_action act = QCA_WD_NONE;
    uint32_t next = 0;
    Stats stats;
    qca_wd wd;
    IOUSBHostDevice *dev = nullptr;
    bool logLatch = false;
    {
        Guard g;
        sTickArmed = false;
        if (sWdEnabled) {
            act = qca_wd_tick(&sWd, nowMs());
            next = qca_wd_next_tick_ms(&sWd, nowMs());
            if (sWd.latched && !sStats.latchLogged) {
                sStats.latchLogged = true;
                logLatch = true;
            }
        }
        if (act == QCA_WD_USB_RESET) {
            dev = sDevice;
            if (dev) {
                dev->retain();
                sStats.resets++;
            }
        }
        stats = sStats;
        wd = sWd;
    }
    if (logLatch)
        LOG("USB reset budget spent (%u in %u s): no more resets until reboot", QCA_WD_MAX_RESETS,
            QCA_WD_BUDGET_WINDOW_MS / 1000u);
    if (dev) {
        if (dev->isInactive()) {
            LOG("controller is going away: no USB reset needed");
        } else {
            LOG("controller stopped answering HCI commands: USB reset (%u of %u allowed in %u s)",
                stats.resets, QCA_WD_MAX_RESETS, QCA_WD_BUDGET_WINDOW_MS / 1000u);
            IOReturn rc = dev->reset();
            if (rc != kIOReturnSuccess)
                LOG("USB reset failed (0x%08x)", rc);
        }
        dev->release();
    }
    publish(stats, wd);
    armTick(next);
}

bool looksLikeHciCommand(const StandardUSB::DeviceRequest &r)
{
    // Class, host to device, device or interface recipient, bRequest 0 or
    // 0xE0 (the two forms in use for HCI commands over USB). Up to 255
    // parameter bytes.
    return (r.bmRequestType & 0xE0) == 0x20 && (r.bmRequestType & 0x1F) <= 1 &&
           (r.bRequest == 0 || r.bRequest == 0xE0) && r.wLength >= QCA_HCI_CMD_HDR &&
           r.wLength <= QCA_HCI_CMD_HDR + 255u;
}

// Is dev the controller? A new instance replaces the old one.
bool isController(IOUSBHostDevice *dev)
{
    if (dev == sDevice)
        return true;
    const StandardUSB::DeviceDescriptor *dd = dev->getDeviceDescriptor();
    if (!dd || USBToHost16(dd->idVendor) != QCA_VENDOR_ID ||
        USBToHost16(dd->idProduct) != QCA_PRODUCT_ID)
        return false;
    IOUSBHostDevice *old = nullptr;
    {
        Guard g;
        if (sDevice != dev) {
            old = sDevice;
            dev->retain();
            sDevice = dev;
            qca_wd_new_device(&sWd);
            qca_sco_pending_init(&sPending);
        }
    }
    if (old)
        old->release();
    return true;
}

uint32_t readCommand(const StandardUSB::DeviceRequest &r, void *data, IOMemoryDescriptor *desc,
                     uint8_t *buf, uint32_t cap)
{
    uint32_t want = r.wLength < cap ? r.wLength : cap;
    if (data) {
        memcpy(buf, data, want);
        return want;
    }
    if (!desc || desc->getLength() < want || desc->prepare(kIODirectionOut) != kIOReturnSuccess)
        return 0;
    uint32_t got = (uint32_t)desc->readBytes(0, buf, want);
    desc->complete(kIODirectionOut);
    return got;
}

struct ScoCmdCtx {
    IOUSBHostCompletionAction action;
    void *owner;
    uint32_t hostLength;
    uint16_t legacyOp;
    uint16_t hostOp;
    uint8_t cmd[QCA_SCO_LEGACY_MAX];
};

void forgetCommand(uint16_t legacyOp, uint16_t hostOp, bool translated)
{
    Guard g;
    qca_sco_cancel(&sPending, legacyOp, translated);
    qca_wd_on_command_failed(&sWd, hostOp);
}

void scoCmdDone(void *owner, void *parameter, IOReturn status, uint32_t bytes)
{
    auto *ctx = static_cast<ScoCmdCtx *>(owner);
    if (status == kIOReturnSuccess)
        bytes = ctx->hostLength; // what the host asked to send
    else
        forgetCommand(ctx->legacyOp, ctx->hostOp, true);
    if (ctx->action)
        ctx->action(ctx->owner, parameter, status, bytes);
    IOFree(ctx, sizeof(*ctx));
}

IOReturn hDeviceRequest(void *that, IOService *client, StandardUSB::DeviceRequest &req, void *data,
                        IOMemoryDescriptor *desc, unsigned int &length,
                        IOUSBHostCompletion *completion, unsigned int timeout)
{
    auto org = FunctionCast(hDeviceRequest, orgDeviceRequest);
    if (!looksLikeHciCommand(req) || !isController(static_cast<IOUSBHostDevice *>(that)))
        return org(that, client, req, data, desc, length, completion, timeout);

    uint8_t cmd[QCA_SCO_ENHANCED_MAX];
    uint32_t got = readCommand(req, data, desc, cmd, sizeof(cmd));
    if (got < QCA_HCI_CMD_HDR)
        return org(that, client, req, data, desc, length, completion, timeout);
    const uint16_t op = (uint16_t)(cmd[0] | (cmd[1] << 8));

    uint8_t legacy[QCA_SCO_LEGACY_MAX];
    uint32_t legacyLen = 0;
    uint16_t voice = 0;
    qca_sco_why why = QCA_SCO_WHY_NONE;
    qca_sco_result res = QCA_SCO_PASS;
    if (sScoEnabled && (op == QCA_HCI_OP_ENH_SETUP_SYNC || op == QCA_HCI_OP_ENH_ACCEPT_SYNC)) {
        if (req.wLength == got)
            res = qca_sco_translate(cmd, got, legacy, sizeof(legacy), &legacyLen, &voice, &why);
        else {
            res = QCA_SCO_REFUSED;
            why = QCA_SCO_WHY_LENGTH;
        }
    }

    const uint64_t now = nowMs();
    bool proven;
    uint32_t nextTick;
    const uint16_t legacyOp = res == QCA_SCO_TRANSLATED ? (uint16_t)(legacy[0] | (legacy[1] << 8)) : op;
    {
        Guard g;
        proven = sWd.events > 0;
        if (res == QCA_SCO_TRANSLATED && !proven) {
            res = QCA_SCO_REFUSED;
            sStats.unproven++;
        } else if (res == QCA_SCO_REFUSED) {
            sStats.refused++;
            sStats.lastRefused = qca_sco_why_name(why);
        }
        if (op == QCA_HCI_OP_RESET)
            qca_sco_pending_init(&sPending);
        if (res == QCA_SCO_TRANSLATED) {
            if (op == QCA_HCI_OP_ENH_SETUP_SYNC)
                sStats.translatedSetup++;
            else
                sStats.translatedAccept++;
            sStats.lastVoice = voice;
        }
        qca_sco_note_sent(&sPending, res == QCA_SCO_TRANSLATED ? legacyOp : op,
                          res == QCA_SCO_TRANSLATED);
        qca_wd_on_command(&sWd, op, now);
        nextTick = qca_wd_next_tick_ms(&sWd, now);
    }
    // Translation results are published from the tick, so one is due soon.
    if (res != QCA_SCO_PASS)
        armTick(1u);
    else if (sWdEnabled)
        armTick(nextTick);
    if (res == QCA_SCO_REFUSED)
        LOG("left 0x%04x as it is: %s", op, proven ? qca_sco_why_name(why) : "no HCI event seen yet");
    if (res != QCA_SCO_TRANSLATED) {
        IOReturn rc = org(that, client, req, data, desc, length, completion, timeout);
        if (rc != kIOReturnSuccess)
            forgetCommand(op, op, false);
        return rc;
    }

    LOG("HCI 0x%04x -> 0x%04x (voice setting 0x%04x)", op, legacyOp, voice);
    StandardUSB::DeviceRequest lreq = req;
    lreq.wLength = (uint16_t)legacyLen;
    if (!completion) {
        unsigned int n = legacyLen;
        IOReturn rc = org(that, client, lreq, legacy, nullptr, n, nullptr, timeout);
        if (rc == kIOReturnSuccess)
            length = req.wLength;
        else
            forgetCommand(legacyOp, op, true);
        return rc;
    }

    auto *ctx = static_cast<ScoCmdCtx *>(IOMalloc(sizeof(ScoCmdCtx)));
    if (!ctx) {
        forgetCommand(legacyOp, op, true);
        return kIOReturnNoMemory;
    }
    ctx->action = completion->action;
    ctx->owner = completion->owner;
    ctx->hostLength = req.wLength;
    ctx->legacyOp = legacyOp;
    ctx->hostOp = op;
    memcpy(ctx->cmd, legacy, legacyLen);
    IOUSBHostCompletion wrapped = *completion;
    wrapped.owner = ctx;
    wrapped.action = scoCmdDone;
    unsigned int n = legacyLen;
    IOReturn rc = org(that, client, lreq, ctx->cmd, nullptr, n, &wrapped, timeout);
    if (rc != kIOReturnSuccess) {
        forgetCommand(legacyOp, op, true);
        IOFree(ctx, sizeof(*ctx));
    }
    return rc;
}

bool isEventPipe(void *that)
{
    IOUSBHostDevice *dev = sDevice;
    if (!dev)
        return false;
    auto *src = static_cast<IOUSBHostIOSource *>(that);
    if (src->getDevice() != dev)
        return false;
    auto *pipe = OSDynamicCast(IOUSBHostPipe, src);
    if (!pipe)
        return false;
    const StandardUSB::EndpointDescriptor *ed = pipe->getEndpointDescriptor();
    return ed && StandardUSB::getEndpointType(ed) == kEndpointTypeInterrupt &&
           StandardUSB::getEndpointDirection(ed) == kEndpointDirectionIn;
}

void processEvents(IOMemoryDescriptor *buf, uint32_t bytes)
{
    uint8_t pkt[64];
    uint32_t n = bytes < sizeof(pkt) ? bytes : (uint32_t)sizeof(pkt);
    if (!buf || n < 2 || buf->prepare(kIODirectionInOut) != kIOReturnSuccess)
        return;
    if (buf->readBytes(0, pkt, n) == n) {
        uint32_t replies = 0, events = 0, rewritten;
        {
            Guard g;
            rewritten = qca_sco_filter_events(&sPending, pkt, n, &replies, &events);
            qca_wd_on_events(&sWd, events, replies);
            sStats.rewritten += rewritten;
        }
        if (rewritten && buf->writeBytes(0, pkt, n) != n)
            LOG("could not rewrite an answer");
    }
    buf->complete(kIODirectionInOut);
}

struct IoCtx {
    IOUSBHostCompletionAction action;
    IOUSBHostBundledCompletionAction bundled;
    void *owner;
    IOMemoryDescriptor *buffer;
};

IoCtx *newIoCtx(IOMemoryDescriptor *buffer, void *owner)
{
    auto *ctx = static_cast<IoCtx *>(IOMalloc(sizeof(IoCtx)));
    if (ctx) {
        ctx->action = nullptr;
        ctx->bundled = nullptr;
        ctx->owner = owner;
        ctx->buffer = buffer;
        buffer->retain();
    }
    return ctx;
}

void freeIoCtx(IoCtx *ctx)
{
    ctx->buffer->release();
    IOFree(ctx, sizeof(*ctx));
}

void ioDone(void *owner, void *parameter, IOReturn status, uint32_t bytes)
{
    auto *ctx = static_cast<IoCtx *>(owner);
    if (status == kIOReturnSuccess)
        processEvents(ctx->buffer, bytes);
    if (ctx->action)
        ctx->action(ctx->owner, parameter, status, bytes);
    freeIoCtx(ctx);
}

void ioBundledDone(void *owner, uint32_t count, IOMemoryDescriptor **buffers, void **parameter,
                   IOReturn *status, uint32_t *bytes)
{
    auto *ctx = static_cast<IoCtx *>(owner);
    if (buffers && status && bytes)
        for (uint32_t i = 0; i < count; i++)
            if (status[i] == kIOReturnSuccess)
                processEvents(buffers[i], bytes[i]);
    if (ctx->bundled)
        ctx->bundled(ctx->owner, count, buffers, parameter, status, bytes);
    freeIoCtx(ctx);
}

IOReturn hIoCompletion(void *that, IOMemoryDescriptor *buf, uint32_t len,
                       IOUSBHostCompletion *completion, uint32_t timeout)
{
    auto org = FunctionCast(hIoCompletion, orgIoCompletion);
    if (!buf || !completion || !completion->action || !isEventPipe(that))
        return org(that, buf, len, completion, timeout);
    IoCtx *ctx = newIoCtx(buf, completion->owner);
    if (!ctx)
        return org(that, buf, len, completion, timeout);
    ctx->action = completion->action;
    IOUSBHostCompletion wrapped = *completion;
    wrapped.owner = ctx;
    wrapped.action = ioDone;
    IOReturn rc = org(that, buf, len, &wrapped, timeout);
    if (rc != kIOReturnSuccess)
        freeIoCtx(ctx);
    return rc;
}

IOReturn hIoSync(void *that, IOMemoryDescriptor *buf, uint32_t len, uint32_t &bytes, uint32_t timeout)
{
    auto org = FunctionCast(hIoSync, orgIoSync);
    IOReturn rc = org(that, buf, len, bytes, timeout);
    if (rc == kIOReturnSuccess && buf && isEventPipe(that))
        processEvents(buf, bytes);
    return rc;
}

IOReturn hIoBundled(void *that, IOMemoryDescriptor *buf, uint32_t len,
                    IOUSBHostBundledCompletion *completion, uint32_t timeout)
{
    auto org = FunctionCast(hIoBundled, orgIoBundled);
    if (!buf || !completion || !completion->action || !isEventPipe(that))
        return org(that, buf, len, completion, timeout);
    IoCtx *ctx = newIoCtx(buf, completion->owner);
    if (!ctx)
        return org(that, buf, len, completion, timeout);
    ctx->bundled = completion->action;
    IOUSBHostBundledCompletion wrapped = *completion;
    wrapped.owner = ctx;
    wrapped.action = ioBundledDone;
    IOReturn rc = org(that, buf, len, &wrapped, timeout);
    if (rc != kIOReturnSuccess)
        freeIoCtx(ctx);
    return rc;
}

void route(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size,
           const char *symbol, mach_vm_address_t hook, mach_vm_address_t &org)
{
    KernelPatcher::RouteRequest req(symbol, hook, org);
    if (patcher.routeMultiple(index, &req, 1, address, size)) {
        LOG("hooked %s", symbol);
    } else {
        LOG("cannot hook %s (error %d): this path stays unfiltered", symbol, patcher.getError());
        patcher.clearError();
    }
}

void processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size)
{
    if (kextIOUSBHostFamily.loadIndex != index)
        return;
    route(patcher, index, address, size,
          "__ZN17IOUSBHostIOSource2ioEP18IOMemoryDescriptorjP19IOUSBHostCompletionj",
          reinterpret_cast<mach_vm_address_t>(hIoCompletion), orgIoCompletion);
    route(patcher, index, address, size, "__ZN17IOUSBHostIOSource2ioEP18IOMemoryDescriptorjRjj",
          reinterpret_cast<mach_vm_address_t>(hIoSync), orgIoSync);
    route(patcher, index, address, size,
          "__ZN17IOUSBHostIOSource2ioEP18IOMemoryDescriptorjP26IOUSBHostBundledCompletionj",
          reinterpret_cast<mach_vm_address_t>(hIoBundled), orgIoBundled);
    // Last: commands are only touched once the event side is in place.
    route(patcher, index, address, size,
          "__ZN15IOUSBHostDevice13deviceRequestEP9IOServiceRN11StandardUSB13DeviceRequestEPvP18IOMemoryDescriptorRjP19IOUSBHostCompletionj",
          reinterpret_cast<mach_vm_address_t>(hDeviceRequest), orgDeviceRequest);
}

void pluginStart()
{
    sLock = IOSimpleLockAlloc();
    sTick = thread_call_allocate(tickFired, nullptr);
    if (!sLock || !sTick) {
        LOG("no lock or thread call: not hooking anything");
        return;
    }
    qca_sco_pending_init(&sPending);
    qca_wd_init(&sWd);
    sScoEnabled = !checkKernelArgument("-qcahcinosco");
    sWdEnabled = !checkKernelArgument("-qcahcinowd");
    LOG("start: SCO translation %s, watchdog %s", sScoEnabled ? "on" : "off (-qcahcinosco)",
        sWdEnabled ? "on" : "off (-qcahcinowd)");
    lilu.onKextLoadForce(
        &kextIOUSBHostFamily, 1,
        [](void *, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
            processKext(patcher, index, address, size);
        },
        nullptr);
}

} // namespace

PluginConfiguration ADDPR(config){
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal,
    bootargOff,
    arrsize(bootargOff),
    bootargDebug,
    arrsize(bootargDebug),
    bootargBeta,
    arrsize(bootargBeta),
    KernelVersion::Tahoe,
    KernelVersion::Tahoe,
    pluginStart,
};

// The bundle id and version must match Info.plist (cross-build-hcifix.sh checks).
extern "C" kern_return_t QCARomeHCIFix_kern_start(kmod_info_t *, void *);
extern "C" kern_return_t QCARomeHCIFix_kern_stop(kmod_info_t *, void *);
extern "C" {
KMOD_EXPLICIT_DECL(io.github.akshaynexus.helios300.QCARomeHCIFix, "0.1.0", QCARomeHCIFix_kern_start,
                   QCARomeHCIFix_kern_stop)
}
