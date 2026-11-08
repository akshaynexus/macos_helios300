// Host stand-in for the reconstructed family base class (tests only).
#pragma once
#include "../fake_kernel.hpp"
struct RegistrationBlock{uint8_t b[0x130];};
struct SkyExp{void *fRegistrationInfo;};
class IO80211Controller;
class IOSkywalkEthernetInterface:public IOService{public:struct RegistrationInfo{uint8_t b[0x130];};
    SkyExp *mExpansionData2;
    bool initRegistrationInfo(RegistrationInfo *r,unsigned,unsigned long){memset(r,7,sizeof(*r));rec("initRegistrationInfo");return true;}
};
class IOSkywalkNetworkInterface:public IOSkywalkEthernetInterface{public:SkyExp *mExpansionData;
    void deferBSDAttach(bool v){rec(v?"deferBSDAttach(true)":"deferBSDAttach(false)");}
    IOSkywalkNetworkInterface(){mExpansionData=new SkyExp{nullptr};mExpansionData2=new SkyExp{nullptr};}
    ~IOSkywalkNetworkInterface(){delete mExpansionData;delete mExpansionData2;}
};
class IO80211SkywalkInterface:public IOSkywalkNetworkInterface{public:
    OSDeclareAbstractStructors(IO80211SkywalkInterface)
    virtual void free() override {}
    virtual bool start(IOService *) override {extern bool gFailSkywalkStart;rec("skywalk.start");return !gFailSkywalkStart;}
    virtual void stop(IOService *) override {rec("skywalk.stop");}
    virtual const char *cls() const override {return "skywalk";}
    virtual SInt32 prepareBSDInterface(struct __ifnet *ifp,unsigned int){rec("prepareBSDInterface");lastIfp=ifp;return 0;}
    virtual SInt32 setInterfaceEnable(bool){return 0;}
    virtual const char *getBSDNamePrefix(){return "en";}
    bool startResult=true;struct __ifnet *lastIfp=nullptr;unsigned role=0,ifaceId=0;
    bool init(IOService *,ether_addr *){return true;}
    void setInterfaceRole(unsigned r){role=r;rec("setInterfaceRole."+std::to_string(r));}
    void setInterfaceId(unsigned r){ifaceId=r;}
    SInt32 peerToPeerCalled(){return 0;}
};
