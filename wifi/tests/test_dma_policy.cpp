#include <cassert>
#include <cstring>
#include <strings.h>
#include <vector>
#include "Ath10kHTC.hpp"
#ifndef ATH10K_PAYLOAD_CACHE
#define ATH10K_PAYLOAD_CACHE 1
#endif
using IOPhysicalAddress=UInt64;using IOReturn=int;
constexpr int kernel_task=0,kIOReturnSuccess=0,kIOReturnNotReady=1;
constexpr UInt32 kIOMemoryPhysicallyContiguous=1,kIODirectionInOut=2,kIOMapInhibitCache=4,kIOMapCopybackCache=8;
constexpr UInt32 kIODirectionOut=16,kIODirectionIn=32,kIODMACommandOutputHost64=1;
static void OSSynchronizeIO(){}
struct IOMapper {};
struct IOBufferMemoryDescriptor {
    inline static UInt32 options=0;inline static int live=0;
    std::vector<UInt8> data;bool prepared=false;
    explicit IOBufferMemoryDescriptor(UInt64 bytes):data(bytes,0xa5){++live;}
    static IOBufferMemoryDescriptor *inTaskWithPhysicalMask(int,UInt32 opts,UInt64 bytes,UInt64){
        options=opts;return new IOBufferMemoryDescriptor(bytes);
    }
    int prepare(){prepared=true;return 0;}void complete(){assert(prepared);prepared=false;}
    void release(){assert(!prepared);--live;delete this;}
    void *getBytesNoCopy(){return data.data();}UInt64 getPhysicalAddress(){return 0x100000000ULL;}
};
struct IODMACommand {
    enum {kMapped=1};struct Segment64 {UInt64 fIOVMAddr,fLength;};
    inline static IODMACommand *last=nullptr;inline static bool failSync=false;
    IOBufferMemoryDescriptor *descriptor=nullptr;std::vector<UInt8> device;
    static IODMACommand *withSpecification(UInt32,UInt32 bits,UInt32,UInt32 mode,UInt32,UInt32,IOMapper *,void *){
        assert(bits==32 && mode==kMapped);last=new IODMACommand;return last;
    }
    int setMemoryDescriptor(IOBufferMemoryDescriptor *d){assert(d->prepared);descriptor=d;device=d->data;return 0;}
    int gen64IOVMSegments(UInt64 *,Segment64 *s,UInt32 *n){assert(*n==1);*s={0x200000,descriptor->data.size()};return 0;}
    int synchronize(UInt32 direction){
        if(failSync)return 1;
        if(direction==kIODirectionOut)device=descriptor->data;
        else {assert(direction==kIODirectionIn);descriptor->data=device;}
        return 0;
    }
    void clearMemoryDescriptor(){descriptor=nullptr;}void release(){assert(!descriptor);delete this;last=nullptr;}
};
#include "dma_methods.inc"
int main(){
    for(auto role:{Ath10kDMABuffer::Role::Ring,Ath10kDMABuffer::Role::Payload}){
        Ath10kDMABuffer b;assert(b.allocate(2048,128,nullptr,role));
        const bool cached=role==Ath10kDMABuffer::Role::Payload && ATH10K_PAYLOAD_CACHE;
        assert(b.usesCache()==cached && !b.usesExplicitMapper());
        assert((IOBufferMemoryDescriptor::options&(kIOMapInhibitCache|kIOMapCopybackCache))==(cached?kIOMapCopybackCache:kIOMapInhibitCache));
        assert(b.physicalAddress()>0xffffffffULL && b.dmaAddress()==0x200000);
        auto *p=static_cast<UInt8 *>(b.virtualAddress());assert(p[0]==0 && IODMACommand::last->device[0]==0);
        p[5]=3;assert(b.syncForDevice() && IODMACommand::last->device[5]==3);
        IODMACommand::last->device[5]=9;assert(p[5]==3);assert(b.syncFromDevice() && p[5]==9);
        b.release();assert(!IOBufferMemoryDescriptor::live && !b.allocated());
    }
    Ath10kDMABuffer invalid;assert(!invalid.allocate(0,128) && !invalid.allocate(100,3));
    IODMACommand::failSync=true;assert(!invalid.allocate(2048,128));
    assert(!IOBufferMemoryDescriptor::live && !IODMACommand::last);
}
