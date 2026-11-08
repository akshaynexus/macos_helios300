// SPDX-License-Identifier: GPL-2.0-or-later
//
// QCARomePatchRAM: resident firmware uploader for Lite-On 04ca:3016
// (QCA6174A / Qualcomm Rome 3.2 USB Bluetooth) on macOS 12+ (target: 26).
//
// Originally duckducktw/qca-rome-bt-macos (one-shot uploader); reworked for
// kexts/bluetooth in macos_helios300 (see UPSTREAM.md).
//
// Lifecycle (model: acidanthera/BrcmPatchRAM3, IOUSBHostDevice API):
//   probe   VID/PID check, no device I/O.
//   start   stays attached in its own IOMatchCategory (other drivers and the
//           userspace Bluetooth stack can still use the device), joins the
//           power tree, and schedules a setup run on its own workloop.
//   setup   open the device, make sure it is configured, run the portable
//           qca_rome state machine (it downloads only what CHECK_STATUS says
//           is missing; the interface/bulk pipe are opened only when a
//           download is due, and before its DFU header), close everything. On success rename the device to
//           "Bluetooth USB Host Controller" (macOS 12+, as
//           IntelBluetoothFirmware does) and re-register it so bluetoothd
//           (patched by BlueToolFixup) picks it up.
//   power   setPowerState only records and posts the event to the workloop
//           (the PM thread never waits for a run).
//   wake    power on after an off: run the setup again 800 ms later
//           (four control reads if the chip kept its patch; a full download
//           if it lost power without re-enumerating).
//   retry   transient USB failures: up to 4 attempts, 250/500/1000 ms apart;
//           again on every wake. Permanent errors (wrong chip/firmware): no
//           retry, one attempt per wake (in case it was a garbage read).
//           If bluetoothd holds the device during a wake check, it is skipped.
//   unplug  willTerminate (terminate thread, never waits) aborts control
//           requests and the bulk pipe; the run ends fast and closes the
//           device; stop() then tears down. Re-enumeration = new instance.
// Decisions live in src/core/qca_lifecycle.c (host-tested).
//
// No Apple transport personality is injected: on macOS 12+ the USB HCI
// transport is bluetoothd in userspace (BlueToolFixup patches it); the
// kernel transport classes the old BrcmBluetoothInjector matched are
// macOS <= 11 only. See README.md.
//
// Boot args:  -qcarpoff     do not attach
//             -qcarpprobe   read-only: GET_VERSION + CHECK_STATUS, no writes
//             -qcarpnoname  do not rename the device (diagnostics)
#include "QCARomePatchRAM.hpp"

#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <libkern/version.h>
#include <pexpert/pexpert.h>

#include "qca_rome_firmware.h"

#define LOG(fmt, ...) IOLog("QCARomePatchRAM: " fmt "\n", ##__VA_ARGS__)

#define super IOService
OSDefineMetaClassAndStructors(QCARomePatchRAM, IOService)

namespace {

enum { kPowerOff = 0, kPowerOn = 1, kPowerStates = 2 };
enum : UInt32 { kPostPowerOff = 1u << 0, kPostPowerOn = 1u << 1, kPostTerminate = 1u << 2 };

IOPMPowerState gPowerStates[kPowerStates] = {
    {kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {kIOPMPowerStateVersion1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0},
};

int trCtrlIn(void *ctx, uint8_t req, void *buf, uint16_t len, uint32_t *actual, uint32_t to)
{
    return static_cast<QCARomePatchRAM *>(ctx)->xferControlIn(req, buf, len, actual, to);
}

int trCtrlOut(void *ctx, uint8_t req, const void *buf, uint16_t len, uint32_t *actual, uint32_t to)
{
    return static_cast<QCARomePatchRAM *>(ctx)->xferControlOut(req, buf, len, actual, to);
}

int trBulkOut(void *ctx, uint8_t ep, const void *buf, uint32_t len, uint32_t *actual, uint32_t to)
{
    return static_cast<QCARomePatchRAM *>(ctx)->xferBulkOut(ep, buf, len, actual, to);
}

int trPrepare(void *ctx)
{
    return static_cast<QCARomePatchRAM *>(ctx)->prepareDownload();
}

void trSleep(void *, uint32_t ms)
{
    IOSleep(ms);
}

void trLog(void *, const char *msg, uint32_t a, uint32_t b, uint32_t c)
{
    char line[160];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
    snprintf(line, sizeof(line), msg, a, b, c);
#pragma clang diagnostic pop
    LOG("%s", line);
}

bool bootFlag(const char *name)
{
    char dummy[8];
    return PE_parse_boot_argn(name, dummy, sizeof(dummy));
}

IOReturn gatedEventAction(OSObject *owner, void *arg0, void *, void *, void *)
{
    static_cast<QCARomePatchRAM *>(owner)->gatedEvent(
        static_cast<qca_life_event>(reinterpret_cast<uintptr_t>(arg0)));
    return kIOReturnSuccess;
}

void timerAction(OSObject *owner, IOTimerEventSource *)
{
    static_cast<QCARomePatchRAM *>(owner)->timerFired();
}

void postedAction(OSObject *owner, IOInterruptEventSource *, int)
{
    static_cast<QCARomePatchRAM *>(owner)->postedEventsFired();
}

} // namespace

IOService *QCARomePatchRAM::probe(IOService *provider, SInt32 *score)
{
    IOUSBHostDevice *dev = OSDynamicCast(IOUSBHostDevice, provider);
    if (!dev)
        return nullptr;
    const StandardUSB::DeviceDescriptor *dd = dev->getDeviceDescriptor();
    if (!dd)
        return nullptr;
    uint16_t vid = USBToHost16(dd->idVendor);
    uint16_t pid = USBToHost16(dd->idProduct);
    if (vid != QCAROME_VENDOR_ID || pid != QCAROME_PRODUCT_ID) {
        LOG("probe: unexpected device %04x:%04x, declining", vid, pid);
        return nullptr;
    }
    if (bootFlag("-x")) {
        LOG("probe: safe mode (-x), not attaching");
        return nullptr;
    }
    if (bootFlag("-qcarpoff")) {
        LOG("probe: disabled by -qcarpoff");
        return nullptr;
    }
    LOG("probe: %04x:%04x on Darwin %d.%d", vid, pid, version_major, version_minor);
    return super::probe(provider, score);
}

bool QCARomePatchRAM::start(IOService *provider)
{
    IOUSBHostDevice *dev = OSDynamicCast(IOUSBHostDevice, provider);
    if (!dev || !super::start(provider))
        return false;

    fDevice = dev;
    fDevice->retain();
    qca_life_init(&fLife);

    fPipeLock = IOLockAlloc();
    fWorkLoop = IOWorkLoop::workLoop();
    fTimer = fWorkLoop ? IOTimerEventSource::timerEventSource(this, timerAction) : nullptr;
    fPostSrc = fWorkLoop ? IOInterruptEventSource::interruptEventSource(this, postedAction) : nullptr;
    bool timerAdded = fTimer && fWorkLoop->addEventSource(fTimer) == kIOReturnSuccess;
    bool postAdded = timerAdded && fPostSrc && fWorkLoop->addEventSource(fPostSrc) == kIOReturnSuccess;
    if (!fPipeLock || !postAdded) {
        LOG("start: cannot create workloop/event sources");
        if (timerAdded)
            fWorkLoop->removeEventSource(fTimer);
        OSSafeReleaseNULL(fTimer);
        OSSafeReleaseNULL(fPostSrc);
        OSSafeReleaseNULL(fWorkLoop);
        if (fPipeLock) {
            IOLockFree(fPipeLock);
            fPipeLock = nullptr;
        }
        OSSafeReleaseNULL(fDevice);
        super::stop(provider);
        return false;
    }

    // As BrcmPatchRAM3 and IntelBluetoothFirmware: join the power tree as a
    // child of the device and declare we want it on (makeUsable), so system
    // sleep/wake is delivered as setPowerState(off)/(on).
    PMinit();
    registerPowerDriver(this, gPowerStates, kPowerStates);
    provider->joinPMtree(this);
    makeUsable();
    fPMStarted = true;

    fWorkLoop->runAction(gatedEventAction, this, reinterpret_cast<void *>(QCA_EV_START));
    registerService();
    return true;
}

bool QCARomePatchRAM::willTerminate(IOService *provider, IOOptionBits options)
{
    // Unplug or unload, on the global terminate thread: do not wait here.
    // Fail every outstanding and future transfer fast; a run in progress then
    // finishes quickly and closes the device, which lets IOKit go on to stop().
    fTerminating = true;
    if (fDevice)
        fDevice->abortDeviceRequests(this, IOUSBHostIOSource::kAbortAsynchronous, kIOReturnAborted);
    if (fPipeLock) {
        IOLockLock(fPipeLock);
        if (fBulkOut)
            fBulkOut->abort(IOUSBHostIOSource::kAbortAsynchronous, kIOReturnAborted);
        IOLockUnlock(fPipeLock);
    }
    post(kPostTerminate);
    return super::willTerminate(provider, options);
}

void QCARomePatchRAM::stop(IOService *provider)
{
    fTerminating = true;
    // PMstop first (as both reference drivers do): it waits for a PM call in
    // flight, so setPowerState can no longer touch the event sources.
    if (fPMStarted) {
        PMstop();
        fPMStarted = false;
    }
    if (fWorkLoop) {
        fWorkLoop->runAction(gatedEventAction, this, reinterpret_cast<void *>(QCA_EV_TERMINATE));
        if (fTimer) {
            fTimer->cancelTimeout();
            fWorkLoop->removeEventSource(fTimer); // waits for a running action
        }
        if (fPostSrc)
            fWorkLoop->removeEventSource(fPostSrc);
    }
    OSSafeReleaseNULL(fTimer);
    OSSafeReleaseNULL(fPostSrc);
    OSSafeReleaseNULL(fWorkLoop);
    closeAll();
    if (fPipeLock) {
        IOLockFree(fPipeLock);
        fPipeLock = nullptr;
    }
    OSSafeReleaseNULL(fDevice);
    super::stop(provider);
}

void QCARomePatchRAM::post(UInt32 bit)
{
    OSBitOrAtomic(bit, &fPending);
    if (fPostSrc)
        fPostSrc->interruptOccurred(nullptr, this, 0);
}

IOReturn QCARomePatchRAM::setPowerState(unsigned long state, IOService *)
{
    // Never block the PM thread on a run: record and post to the workloop.
    if (!fTerminating) {
        fWantPowerOn = (state != kPowerOff);
        post(state == kPowerOff ? kPostPowerOff : kPostPowerOn);
    }
    return IOPMAckImplied;
}

void QCARomePatchRAM::postedEventsFired()
{
    UInt32 bits = OSBitAndAtomic(0u, &fPending);
    if (bits & kPostTerminate) {
        gatedEvent(QCA_EV_TERMINATE);
        return;
    }
    // Collapsed sequences: an "off" seen at all is delivered first; "on" is
    // delivered if it is the current wish (off->on = wake, on->off = net off).
    if (bits & kPostPowerOff)
        gatedEvent(QCA_EV_POWER_OFF);
    if ((bits & kPostPowerOn) && fWantPowerOn)
        gatedEvent(QCA_EV_POWER_ON);
}

void QCARomePatchRAM::gatedEvent(qca_life_event ev)
{
    if (ev == QCA_EV_POWER_OFF || ev == QCA_EV_POWER_ON)
        LOG("power %s (state %s)", ev == QCA_EV_POWER_ON ? "on" : "off",
            qca_life_state_name(fLife.state));
    apply(qca_life_on_event(&fLife, ev));
    publishState();
}

void QCARomePatchRAM::timerFired()
{
    apply(qca_life_on_event(&fLife, QCA_EV_TIMER));
    publishState();
}

void QCARomePatchRAM::apply(qca_life_action a)
{
    // Runs on fWorkLoop with the gate held. A run is synchronous here: the
    // workloop is ours alone, so blocking USB calls do not stall the USB
    // family or the power-management thread beyond one run (~0.5 s).
    while (true) {
        if (a.cancel_timer && fTimer)
            fTimer->cancelTimeout();
        if (a.arm_timer_ms && fTimer && !fTerminating)
            fTimer->setTimeoutMS(a.arm_timer_ms);
        if (!a.run_setup)
            return;
        qca_err e = runSetup();
        a = fLastRunSkipped ? qca_life_on_skipped(&fLife) : qca_life_on_result(&fLife, e);
        if (fLife.state == QCA_LS_FAILED || fLife.state == QCA_LS_DEAD)
            LOG("giving up (%s) after %u attempt(s): %s", qca_life_state_name(fLife.state),
                fLife.attempts, qca_strerror(e));
    }
}

bool QCARomePatchRAM::openDevice()
{
    if (fDeviceOpen)
        return true;
    if (fTerminating || isInactive() || !fDevice || fDevice->isInactive() ||
        !fDevice->open(this)) {
        LOG("cannot open IOUSBHostDevice (owned by another client or going away)");
        return false;
    }
    fDeviceOpen = true;
    return true;
}

bool QCARomePatchRAM::ensureConfigured()
{
    uint8_t current = 0;
    StandardUSB::DeviceRequest req;
    req.bmRequestType = makeDeviceRequestbmRequestType(kRequestDirectionIn, kRequestTypeStandard,
                                                       kRequestRecipientDevice);
    req.bRequest = kDeviceRequestGetConfiguration;
    req.wValue = 0;
    req.wIndex = 0;
    req.wLength = 1;
    uint32_t got = 0;
    IOReturn rc = fDevice->deviceRequest(this, req, &current, got,
                                         kUSBHostStandardRequestCompletionTimeout);
    if (rc != kIOReturnSuccess || got != 1) {
        LOG("GET_CONFIGURATION failed (0x%08x, %u bytes)", rc, got);
        return false;
    }
    if (current != 0)
        return true;
    const StandardUSB::ConfigurationDescriptor *cd = fDevice->getConfigurationDescriptor(0);
    if (!cd) {
        LOG("no configuration descriptor at index 0");
        return false;
    }
    // Interfaces are published (and matched) only once the device is
    // configured with interface matching on (as BrcmPatchRAM3 and
    // IntelBluetoothFirmware do). The device stays configured afterwards,
    // which is the state the Bluetooth stack expects.
    rc = fDevice->setConfiguration(cd->bConfigurationValue, true);
    if (rc != kIOReturnSuccess) {
        LOG("setConfiguration(%u) failed (0x%08x)", cd->bConfigurationValue, rc);
        return false;
    }
    LOG("configured device (bConfigurationValue %u)", cd->bConfigurationValue);
    return true;
}

IOUSBHostInterface *QCARomePatchRAM::findInterface0(int attempts)
{
    for (int i = 0; i < attempts && !fTerminating; i++) {
        OSIterator *it = fDevice->getChildIterator(gIOServicePlane);
        IOUSBHostInterface *found = nullptr;
        if (it) {
            while (OSObject *o = it->getNextObject()) {
                IOUSBHostInterface *intf = OSDynamicCast(IOUSBHostInterface, o);
                if (!intf || intf->isInactive())
                    continue;
                const StandardUSB::InterfaceDescriptor *id = intf->getInterfaceDescriptor();
                if (id && id->bInterfaceNumber == 0 && id->bAlternateSetting == 0) {
                    intf->retain();
                    found = intf;
                    break;
                }
            }
            it->release();
        }
        if (found)
            return found;
        IOSleep(10);
    }
    return nullptr;
}

bool QCARomePatchRAM::acquireBulkPipe()
{
    // Only needed when a download is due. Interface 0 carries the DFU bulk
    // endpoint 0x02 (Linux usb_sndbulkpipe(udev, 0x02)).
    if (fBulkOut)
        return true;
    if (!fInterface)
        fInterface = findInterface0(200);
    if (!fInterface) {
        // Configured by someone else with interface matching off: no nubs.
        // Re-apply the configuration once (we hold the device open, so no
        // other client is using it) so the interfaces get published.
        const StandardUSB::ConfigurationDescriptor *cd = fDevice->getConfigurationDescriptor(0);
        if (cd && !fTerminating) {
            LOG("no interface nubs; re-applying configuration %u", cd->bConfigurationValue);
            fDevice->setConfiguration(0, false);
            if (fDevice->setConfiguration(cd->bConfigurationValue, true) == kIOReturnSuccess)
                fInterface = findInterface0(200);
        }
    }
    if (!fInterface) {
        LOG("interface 0 not found");
        return false;
    }
    if (!fIfaceOpen) {
        if (!fInterface->open(this)) {
            LOG("cannot open interface 0 (owned by the Bluetooth stack?)");
            return false;
        }
        fIfaceOpen = true;
    }
    IOUSBHostPipe *pipe = fInterface->copyPipe(QCA_DFU_BULK_EP);
    if (!pipe) {
        LOG("copyPipe(0x%02x) failed", QCA_DFU_BULK_EP);
        return false;
    }
    const StandardUSB::EndpointDescriptor *ed = pipe->getEndpointDescriptor();
    if (!ed || StandardUSB::getEndpointType(ed) != kEndpointTypeBulk ||
        StandardUSB::getEndpointDirection(ed) != kEndpointDirectionOut) {
        LOG("endpoint 0x%02x is not bulk OUT", QCA_DFU_BULK_EP);
        pipe->release();
        return false;
    }
    fBulkBuffer = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionOut,
                                                              QCA_DFU_PACKET_LEN);
    if (!fBulkBuffer || fBulkBuffer->prepare(kIODirectionOut) != kIOReturnSuccess) {
        LOG("bulk buffer allocation failed");
        OSSafeReleaseNULL(fBulkBuffer);
        pipe->release();
        return false;
    }
    IOLockLock(fPipeLock);
    fBulkOut = pipe;
    IOLockUnlock(fPipeLock);
    return true;
}

void QCARomePatchRAM::closeAll()
{
    if (fBulkBuffer) {
        fBulkBuffer->complete(kIODirectionOut);
        OSSafeReleaseNULL(fBulkBuffer);
    }
    IOUSBHostPipe *pipe = nullptr;
    if (fPipeLock)
        IOLockLock(fPipeLock);
    pipe = fBulkOut;
    fBulkOut = nullptr;
    if (fPipeLock)
        IOLockUnlock(fPipeLock);
    if (pipe) {
        pipe->abort();
        pipe->release();
    }
    if (fInterface) {
        if (fIfaceOpen)
            fInterface->close(this);
        fIfaceOpen = false;
        OSSafeReleaseNULL(fInterface);
    }
    if (fDeviceOpen && fDevice)
        fDevice->close(this);
    fDeviceOpen = false;
}

int QCARomePatchRAM::xferControlIn(uint8_t reqCode, void *buf, uint16_t len, uint32_t *actual,
                                   uint32_t timeoutMs)
{
    *actual = 0;
    if (fTerminating)
        return (int)kIOReturnAborted;
    StandardUSB::DeviceRequest req;
    req.bmRequestType = makeDeviceRequestbmRequestType(kRequestDirectionIn, kRequestTypeVendor,
                                                       kRequestRecipientDevice);
    req.bRequest = reqCode;
    req.wValue = 0;
    req.wIndex = 0;
    req.wLength = len;
    uint32_t got = 0;
    IOReturn rc = fDevice->deviceRequest(this, req, buf, got, timeoutMs);
    *actual = got;
    if (rc != kIOReturnSuccess)
        LOG("vendor IN 0x%02x failed (0x%08x)", reqCode, rc);
    return (int)rc;
}

int QCARomePatchRAM::xferControlOut(uint8_t reqCode, const void *buf, uint16_t len,
                                    uint32_t *actual, uint32_t timeoutMs)
{
    *actual = 0;
    if (fTerminating)
        return (int)kIOReturnAborted;
    uint8_t local[64];
    if (len > sizeof(local))
        return (int)kIOReturnBadArgument;
    memcpy(local, buf, len);

    StandardUSB::DeviceRequest req;
    req.bmRequestType = makeDeviceRequestbmRequestType(kRequestDirectionOut, kRequestTypeVendor,
                                                       kRequestRecipientDevice);
    req.bRequest = reqCode;
    req.wValue = 0;
    req.wIndex = 0;
    req.wLength = len;
    uint32_t got = 0;
    IOReturn rc = fDevice->deviceRequest(this, req, local, got, timeoutMs);
    *actual = got;
    if (rc != kIOReturnSuccess)
        LOG("vendor OUT 0x%02x (%u bytes) failed (0x%08x)", reqCode, len, rc);
    return (int)rc;
}

int QCARomePatchRAM::xferBulkOut(uint8_t ep, const void *buf, uint32_t len, uint32_t *actual,
                                 uint32_t timeoutMs)
{
    *actual = 0;
    if (fTerminating)
        return (int)kIOReturnAborted;
    if (ep != QCA_DFU_BULK_EP || len > QCA_DFU_PACKET_LEN)
        return (int)kIOReturnBadArgument;
    if (!fBulkOut || !fBulkBuffer)
        return (int)kIOReturnNotOpen; // prepareDownload() was not called or failed
    memcpy(fBulkBuffer->getBytesNoCopy(), buf, len);
    uint32_t sent = 0;
    IOReturn rc = fBulkOut->io(fBulkBuffer, len, sent, timeoutMs);
    *actual = sent;
    if (rc != kIOReturnSuccess) {
        LOG("bulk OUT %u bytes failed (0x%08x)", len, rc);
        if (rc != kIOReturnAborted && rc != kIOReturnNoDevice && !fTerminating)
            fBulkOut->clearStall(true);
    }
    return (int)rc;
}

int QCARomePatchRAM::prepareDownload()
{
    // Called by the core before each DFU header: interface and bulk pipe must
    // be ready first, so a failure never leaves EP2 switched to DFU with no
    // body following (Linux sends the body 20 ms after the header).
    if (fTerminating)
        return (int)kIOReturnAborted;
    return acquireBulkPipe() ? 0 : (int)kIOReturnNotOpen;
}

qca_err QCARomePatchRAM::runSetup()
{
    uint64_t t0 = 0, t1 = 0, ns = 0;
    clock_get_uptime(&t0);

    const bool probeOnly = bootFlag("-qcarpprobe");
    qca_result r;
    bzero(&r, sizeof(r));
    qca_err e;
    bool skipped = false;
    fLastRunSkipped = false;
    if (!openDevice()) {
        if (fLife.successes > 0 && !fTerminating) {
            // Wake check while another client (bluetoothd) holds the device:
            // it is in use, so do not fight over it or count a failure. A
            // chip that lost power re-enumerates and gets a new instance.
            skipped = true;
            r.err = e = QCA_OK;
            r.stage = QCA_STAGE_DONE;
        } else {
            // First run, busy or going away: transient, retried by the policy.
            r.err = e = QCA_ERR_GET_VERSION_IO;
            r.stage = QCA_STAGE_GET_VERSION;
            r.transport_rc = (int)kIOReturnExclusiveAccess;
        }
    } else if (!ensureConfigured()) {
        r.err = e = QCA_ERR_GET_VERSION_IO;
        r.stage = QCA_STAGE_GET_VERSION;
        r.transport_rc = (int)kIOReturnNotReady;
    } else {
        qca_transport t;
        t.ctx = this;
        t.control_in = trCtrlIn;
        t.control_out = trCtrlOut;
        t.bulk_out = trBulkOut;
        t.sleep_ms = trSleep;
        t.log = trLog;
        t.prepare_download = trPrepare;
        const qca_firmware rampatch = {qca_rampatch_usb_00000302, qca_rampatch_usb_00000302_len};
        const qca_firmware nvm = {qca_nvm_usb_00000302, qca_nvm_usb_00000302_len};
        qca_options opt;
        opt.probe_only = probeOnly ? 1 : 0;
        e = qca_rome_setup(&t, &rampatch, &nvm, &opt, &r);
    }
    closeAll();

    clock_get_uptime(&t1);
    absolutetime_to_nanoseconds(t1 - t0, &ns);
    if (skipped) {
        fLastRunSkipped = true;
        LOG("wake check skipped: device in use by another client (run %u)", fLife.runs);
        if (!fTerminating && !fDevice->isInactive())
            fDevice->setProperty("QCARomePatchRAM-LastCheck", "skipped (device in use)");
        return e;
    }
    if (e == QCA_OK) {
        LOG("%s in %llu ms: rom 0x%08x patch 0x%x->0x%x status 0x%02x->0x%02x "
            "(rampatch=%d nvm=%d, %u bulk packets, run %u)",
            probeOnly ? "probe-only done" : "firmware ready", ns / 1000000ULL,
            r.version_initial.rom_version, r.version_initial.patch_version,
            r.version_after.patch_version, r.status_initial, r.status_final,
            r.rampatch_downloaded, r.nvm_downloaded, r.bulk_packets, fLife.runs);
    } else {
        LOG("run %u FAILED at %s: %s (transport 0x%08x, offset %u, expected %u, actual %u)",
            fLife.runs, qca_stage_name(r.stage), qca_strerror(e), (uint32_t)r.transport_rc,
            r.fail_offset, r.expected_len, r.actual_len);
    }
    if (!fTerminating && !isInactive() && !fDevice->isInactive()) {
        publishResult(r, probeOnly);
        fDevice->setProperty("QCARomePatchRAM-LastCheck", e == QCA_OK ? "ok" : "failed");
        // Rename and re-publish once, after the first success, so bluetoothd
        // adopts the controller. Later (wake) runs never re-publish a device
        // bluetoothd already uses.
        if (e == QCA_OK && !probeOnly && !fRenamed && version_major >= 21 &&
            !bootFlag("-qcarpnoname")) {
            fDevice->setName(QCAROME_BT_DEVICE_NAME);
            fRenamed = true;
            LOG("device named \"%s\" for bluetoothd", QCAROME_BT_DEVICE_NAME);
            fDevice->registerService(kIOServiceAsynchronous);
        }
    }
    return e;
}

void QCARomePatchRAM::publishResult(const qca_result &r, bool probeOnly)
{
    // Diagnostics on the provider for `ioreg -l -w0 | grep QCARomePatchRAM`.
    fDevice->setProperty("QCARomePatchRAM-Result", qca_strerror(r.err));
    fDevice->setProperty("QCARomePatchRAM-Stage", qca_stage_name(r.stage));
    fDevice->setProperty("QCARomePatchRAM-ProbeOnly", probeOnly);
    fDevice->setProperty("QCARomePatchRAM-RomVersion", (unsigned long long)r.version_initial.rom_version, 32);
    fDevice->setProperty("QCARomePatchRAM-PatchVersionBefore", (unsigned long long)r.version_initial.patch_version, 32);
    fDevice->setProperty("QCARomePatchRAM-PatchVersionAfter", (unsigned long long)r.version_after.patch_version, 32);
    fDevice->setProperty("QCARomePatchRAM-StatusInitial", (unsigned long long)r.status_initial, 8);
    fDevice->setProperty("QCARomePatchRAM-StatusFinal", (unsigned long long)r.status_final, 8);
    fDevice->setProperty("QCARomePatchRAM-RampatchDownloaded", r.rampatch_downloaded != 0);
    fDevice->setProperty("QCARomePatchRAM-NVMDownloaded", r.nvm_downloaded != 0);
    fDevice->setProperty("QCARomePatchRAM-TransportReturn", (unsigned long long)(uint32_t)r.transport_rc, 32);
}

void QCARomePatchRAM::publishState()
{
    setProperty("State", qca_life_state_name(fLife.state));
    setProperty("Runs", (unsigned long long)fLife.runs, 32);
    setProperty("Successes", (unsigned long long)fLife.successes, 32);
    setProperty("Wakes", (unsigned long long)fLife.wakes, 32);
    setProperty("LastError", qca_strerror(fLife.last_err));
}
