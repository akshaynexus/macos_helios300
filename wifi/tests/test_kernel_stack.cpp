// Compile the real gated handler and use heap/global fixtures so the host test
// does not itself create the 28 KB cache or 8 KB connection on its stack.
#define ATH10K_STACK_PROBE
#include "test_control.cpp"
#include <memory>
#include <pthread.h>
#include <cstdio>
static Ath10kPCIDevice device;
static IOUserClient client;
static AthControl::Request request;
static AthControl::Response response;

__attribute__((noinline)) static void resetCache(Ath10k::WMIBSSCache &state){Ath10k::resetInPlace(state);}
__attribute__((noinline)) static void resetScan(Ath10k::WMIScanState &state){Ath10k::resetInPlace(state);}
__attribute__((noinline)) static void resetLink(Ath10k::ConnectionState &state){Ath10k::resetDisconnected(state);}

static void *smallStack(void *){
    using namespace AthControl;
    auto call=[](Operation op){request.operation=op;
        return Ath10kPCIDevice::controlAction(&device,&client,&request,&response,nullptr);};
    // Opening/polling/reading the cache must not mutate or drop an existing link.
    device._networkLinkUp=true;device._connection.stage=Ath10k::LinkStage::Connected;
    device._connection.portAuthorized=true;device._wmiBSS.count=1;
    device._wmiBSS.entries[0].ssidLength=1;device._wmiBSS.entries[0].ssid[0]='T';
    for(unsigned i=0;i<100;++i){
        assert(call(Status)==0 && (response.flags&4));assert(call(Info)==0);
        request.index=0;assert(call(BSS)==0 && response.data[8]=='T');
        assert(device._networkLinkUp && device._wmiBSS.count==1 && device._connection.portAuthorized);
    }
    // Resetting a scan retains its documented defaults and advances generation.
    for(unsigned i=0;i<40;++i){
        device._wmiScan.completed=true;request.index=0;
        assert(call(Scan)==0 && device._wmiScan.generation==i+1);
        assert(device._wmiScan.id==(0xa000u|((0x36u+i+1u)&0xfffu)));
        assert(device._wmiBSS.count==0 && device._wmiBSS.lastReject==Ath10k::BSSRejectCount);
        assert(device._networkLinkUp && device._connection.portAuthorized);
        for(auto &b:device._wmiBSS.entries)assert(b.hidden && b.ratesValid && !b.rsnLength && !b.htPresent && !b.seen);
    }
    // In-place default construction preserves every byte/default of the value
    // initialized cache and scan, without depending on an optimizer removing copies.
    auto cache=std::make_unique<Ath10k::WMIBSSCache>();
    auto scan=std::make_unique<Ath10k::WMIScanState>();
    memset(static_cast<void *>(&device._wmiBSS),0xa5,sizeof(device._wmiBSS));resetCache(device._wmiBSS);
    memset(static_cast<void *>(&device._wmiScan),0xa5,sizeof(device._wmiScan));resetScan(device._wmiScan);
    assert(!memcmp(&device._wmiBSS,cache.get(),sizeof(*cache)));
    assert(!memcmp(&device._wmiScan,scan.get(),sizeof(*scan)));
    auto &s=device._connection;s.disconnectCount=2;s.wmiSequence=15;s.httSequence=17;
    s.local[0]=2;s.local[5]=9;s.txPackets=50;s.rxBytes=1234;s.rxDropped=4;s.rxReasons[0]=4;
    memset(s.temporalKey,0x55,sizeof(s.temporalKey));memset(s.installedPair,0xaa,sizeof(s.installedPair));
    resetLink(s);
    assert(s.stage==Ath10k::LinkStage::Disabled && s.disconnectCount==3);
    assert(s.wmiSequence==15 && s.httSequence==17 && s.local[5]==9);
    assert(s.txPackets==50 && s.rxBytes==1234 && s.rxDropped==4 && s.rxReasons[0]==4);
    assert(s.width==20 && s.nss==1 && s.peerId==0xffff && s.vhtMap==0xffff);
    for(auto b:s.temporalKey)assert(b==0);
    for(auto b:s.installedPair)assert(b==0);
    return nullptr;
}
int main(){
    pthread_attr_t attr;assert(pthread_attr_init(&attr)==0);
    // Darwin's minimum may exceed Linux's 16 KB. Use the supported minimum.
    size_t bytes=PTHREAD_STACK_MIN>16384?PTHREAD_STACK_MIN:16384;
    assert(pthread_attr_setstacksize(&attr,bytes)==0);
    pthread_t thread;assert(pthread_create(&thread,&attr,smallStack,nullptr)==0);
    assert(pthread_join(thread,nullptr)==0);pthread_attr_destroy(&attr);
    printf("Production control/reset paths passed on %zu-byte host thread stack.\n",bytes);
}
