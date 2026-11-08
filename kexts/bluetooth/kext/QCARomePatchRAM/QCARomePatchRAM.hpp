/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QCAROMEPATCHRAM_HPP
#define QCAROMEPATCHRAM_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/IOUSBHostPipe.h>

#include "qca_lifecycle.h"
#include "qca_rome.h"

#define QCAROME_VENDOR_ID   0x04ca
#define QCAROME_PRODUCT_ID  0x3016

/* Name bluetoothd (macOS 12+) looks for to pick the USB HCI transport; Apple's
 * own USB modules report it as their product string. IntelBluetoothFirmware
 * sets the same name after loading firmware (commit eb7e3e1). */
#define QCAROME_BT_DEVICE_NAME "Bluetooth USB Host Controller"

class QCARomePatchRAM : public IOService {
    OSDeclareDefaultStructors(QCARomePatchRAM)

public:
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual bool willTerminate(IOService *provider, IOOptionBits options) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setPowerState(unsigned long state, IOService *whom) APPLE_KEXT_OVERRIDE;

    /* qca_transport callbacks (ctx == this); public for the C trampolines. */
    int xferControlIn(uint8_t req, void *buf, uint16_t len, uint32_t *actual, uint32_t timeoutMs);
    int xferControlOut(uint8_t req, const void *buf, uint16_t len, uint32_t *actual, uint32_t timeoutMs);
    int xferBulkOut(uint8_t ep, const void *buf, uint32_t len, uint32_t *actual, uint32_t timeoutMs);

    int prepareDownload();

    /* Workloop-gated entry points (static trampolines in the .cpp). */
    void gatedEvent(qca_life_event ev);
    void timerFired();
    void postedEventsFired();

private:
    void apply(qca_life_action a);
    void post(UInt32 bit);
    qca_err runSetup();
    bool openDevice();
    bool ensureConfigured();
    bool acquireBulkPipe();
    IOUSBHostInterface *findInterface0(int attempts);
    void closeAll();
    void publishResult(const qca_result &r, bool probeOnly);
    void publishState();

    IOUSBHostDevice          *fDevice     = nullptr;
    IOUSBHostInterface       *fInterface  = nullptr;
    IOUSBHostPipe            *fBulkOut    = nullptr;
    IOBufferMemoryDescriptor *fBulkBuffer = nullptr;
    IOWorkLoop               *fWorkLoop   = nullptr;
    IOTimerEventSource       *fTimer      = nullptr;
    IOInterruptEventSource   *fPostSrc    = nullptr; /* events from PM/terminate threads */
    IOLock                   *fPipeLock   = nullptr; /* fBulkOut vs willTerminate abort  */
    volatile UInt32           fPending    = 0;       /* kPost* bits                      */
    volatile bool             fWantPowerOn = true;
    bool                      fDeviceOpen = false;
    bool                      fIfaceOpen  = false;
    bool                      fPMStarted  = false;
    bool                      fRenamed    = false;
    bool                      fLastRunSkipped = false; /* runSetup: device in use */
    volatile bool             fTerminating = false;
    qca_life                  fLife;
};

#endif
