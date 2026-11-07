#define ATH10K_WMI_RUNTIME_TEST
#include "test_wmi_init.cpp"
#include "wmi_runtime_methods.inc"
static std::vector<UInt8> memoryPacket(unsigned count=2){
    auto p=servicePacket();Ath10k::htcWriteLE32(p.data()+88,count);
    Ath10k::htcWriteLE16(p.data()+252,count*20);p.resize(256+20*count);
    for(unsigned i=0;i<count;++i){UInt8 *v=p.data()+256+20*i;
        Ath10k::htcWriteLE16(v,16);Ath10k::htcWriteLE16(v+2,34);
        Ath10k::htcWriteLE32(v+4,10+i);Ath10k::htcWriteLE32(v+8,i?3:8);
        Ath10k::htcWriteLE32(v+12,i?2:0);Ath10k::htcWriteLE32(v+16,i?0:128);}
    Ath10k::htcWriteLE16(p.data()+2,p.size()-8);return p;
}
static void requestParser(){
    auto p=memoryPacket();Ath10k::WMIBootstrapState state;
    assert(!Ath10k::parseWMIBootstrapEvent(p.data(),p.size(),2,state));
    assert(state.hostMemoryCount==2 && Ath10k::hostMemoryRequestBytes(state.hostMemory[0])==1024 && Ath10k::hostMemoryRequestBytes(state.hostMemory[1])==136);
    auto bad=[&](std::vector<UInt8> packet){const auto before=state;
        assert(Ath10k::parseWMIBootstrapEvent(packet.data(),packet.size(),2,state));
        assert(state.hostMemoryCount==before.hostMemoryCount && state.hostMemory[0].id==before.hostMemory[0].id && state.credits.outstanding==before.credits.outstanding);};
    for(unsigned n=0;n<p.size();++n)bad({p.begin(),p.begin()+n});
    auto q=p;Ath10k::htcWriteLE32(q.data()+280,10);bad(q); // duplicate ID
    q=p;Ath10k::htcWriteLE32(q.data()+268,8);bad(q); // unknown flags
    q=p;Ath10k::htcWriteLE32(q.data()+264,0xffffffffu);bad(q); // unit overflow
    q=p;Ath10k::htcWriteLE32(q.data()+88,3);bad(q); // count mismatch
    q=p;Ath10k::htcWriteLE16(q.data()+258,33);bad(q); // wrong nested tag
    q=memoryPacket(5);for(unsigned i=0;i<5;++i){Ath10k::htcWriteLE32(q.data()+264+20*i,Ath10k::kWMIHostMemoryPerRequest);Ath10k::htcWriteLE32(q.data()+268+20*i,0);Ath10k::htcWriteLE32(q.data()+272+20*i,1);}bad(q);
    Ath10k::WMIHostMemoryChunk chunks[2]={{10,0x300000,1024},{11,0x400000,136}};
    assert(!Ath10k::parseWMIBootstrapEvent(p.data(),p.size(),2,state));
    UInt8 frame[300]{};assert(!Ath10k::buildWMIInit(frame,sizeof(frame),state,2,3,chunks,2));
    assert(Ath10k::htcReadLE16(frame+2)==260 && Ath10k::wmiReadLE32(frame+40)==2 && Ath10k::htcReadLE16(frame+224)==40);
    chunks[1].address=chunks[0].address+4;assert(Ath10k::buildWMIInit(frame,sizeof(frame),state,2,3,chunks,2));
    chunks[1].address=0xfffffffc;assert(Ath10k::buildWMIInit(frame,sizeof(frame),state,2,3,chunks,2));
    chunks[1].address=0x400000;chunks[1].id=12;assert(Ath10k::buildWMIInit(frame,sizeof(frame),state,2,3,chunks,2));
}
int main(){
    requestParser();
    for(unsigned mode=0;mode<4;++mode){InitDriver d;
        d._regs.consume=[&](){++d.sent;d.deliver(2,memoryPacket());};
        assert(d.runWMIBootstrap());
        for(unsigned i=0;i<2;++i){d._wmiHostMemory[i].isAllocated=false;d._wmiHostMemory[i].iovm=0x300000+0x100000*i;}
        if(mode==1)d._wmiHostMemory[1].allocationFails=true;
        if(mode==2)d._wmiHostMemory[1].syncOK=false;
        if(mode==3){d._regs.tx3Fail=true;d.resetOK=false;d.pci.disableWorks=false;}
        const bool ok=d.runWMIInit();assert(ok==(mode==0));
        if(!mode){assert(d._wmiInit.hostMemoryOwned && d._wmiInit.hostMemoryCount==2 && d._wmiInit.frameBytes==268);
            const auto *f=static_cast<UInt8 *>(d._wmiTxBuffer.virtualAddress());
            for(unsigned i=0;i<2;++i){const UInt8 *v=f+232+20*i;
                assert(Ath10k::wmiReadLE32(v)==10+i && Ath10k::wmiReadLE32(v+4)==d._wmiHostMemory[i].dmaAddress());
                for(unsigned j=0;j<d._wmiHostMemory[i].length();++j)assert(!static_cast<UInt8 *>(d._wmiHostMemory[i].virtualAddress())[j]);}
            assert(d.quiesceTargetAfterBMI() && !d._wmiInit.hostMemoryOwned); // only verified reset returns ownership
        }else if(mode<3){assert(!d.initSends && !d._wmiInit.hostMemoryOwned);}
        else {assert(d._wmiInit.hostMemoryOwned && !d.quiesceTargetAfterBMI() && d._wmiInit.hostMemoryOwned);
            assert(!d.stopDMAForRelease() && d._dmaQuarantine && d._wmiHostMemory[0].allocated());}
    }
    puts("Firmware memory passed: bounded requests/chunks, count/flags/overflow/overlap rejection, zeroed DMA, INIT wire and reset/quarantine ownership.");
}
