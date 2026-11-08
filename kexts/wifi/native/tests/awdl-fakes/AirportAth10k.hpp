// Stand-in for the real AirportAth10k.hpp: a controller + core the production AWDL driver can use.
#pragma once
#define AIRPORT_ATH10K_AWDL 1
#include "AirportAth10kDefines.hpp"
#include "Ath10kControl.hpp"
#include "AirportAth10kAwdlDriver.hpp"
class IO80211Controller:public IOEthernetController{public:virtual IO80211WorkQueue *getWorkQueue() const=0;};
class Ath10kPCIDevice:public OSObject{public:int calls=0;uint32_t lastOp=0;uint8_t step=0;uint8_t channel=0;std::vector<std::vector<uint8_t>> rx;
    IOReturn controlRequest(OSObject *,const AthControl::Request *r,AthControl::Response *o);
    bool nativeAwdlTx(const uint8_t *,uint32_t,bool){return false;}
    bool nativeAwdlRx(uint8_t *,uint32_t,uint32_t *,uint8_t *,uint8_t *,SInt8 *,uint64_t *){return false;}
};
class AirportAth10k:public IO80211Controller{public:Ath10kPCIDevice *dev=nullptr;IO80211WorkQueue *q=nullptr;uint8_t hw[6]={0x00,1,2,3,4,5};
    IO80211WorkQueue *getWorkQueue() const override {return q;}
    IOWorkLoop *getWorkLoop() const override {return q;}
    IOReturn getHardwareAddress(IOEthernetAddress *a) override {memcpy(a->bytes,hw,6);return 0;}
    IOReturn setHardwareAddress(const void *,UInt32) override {return 0;}
    Ath10kPCIDevice *core() const {return dev;}
};
