#include <cassert>
#include <vector>
#include <cstdio>
#include "Ath10kWMI.hpp"
static std::vector<UInt8> notification(UInt32 id,UInt16 tag,UInt16 length){
    std::vector<UInt8> f(16+length);f[0]=2;Ath10k::htcWriteLE16(f.data()+2,f.size()-8);
    Ath10k::htcWriteLE32(f.data()+8,id);Ath10k::htcWriteLE16(f.data()+12,length);Ath10k::htcWriteLE16(f.data()+14,tag);return f;
}
int main(){
    Ath10k::HTCCreditState credits;credits.initialize(2,256,2);credits.reserve(32);
    Ath10k::ConnectionState s;s.stage=Ath10k::LinkStage::Connected;s.portAuthorized=true;s.bssid[0]=2;
    Ath10k::WMIInitState events;
    auto parse=[&](const std::vector<UInt8> &f){return Ath10k::parseWMIInitResponse(f.data(),f.size(),2,credits,events,nullptr,nullptr,nullptr,false,&s);};
    auto f=notification(0x12345,999,4);assert(!parse(f));assert(events.unknownCount==1 && s.portAuthorized && !s.securityAck && credits.outstanding==1);
    f=notification(0x12345,999,4);f[1]=2;f[4]=8;f.insert(f.end(),{1,4,0,0,2,2,0,0});Ath10k::htcWriteLE16(f.data()+2,f.size()-8);
    assert(parse(f) && events.unknownCount==1 && credits.outstanding==1); // invalid credit: no commit
    f=notification(0x4008,343,8);Ath10k::htcWriteLE32(f.data()+16,85);assert(!parse(f));assert(events.temperature==85 && !s.cancelRequested);
    for(unsigned n=0;n<f.size();++n){std::vector<UInt8> shortFrame(f.begin(),f.begin()+n);assert(parse(shortFrame) && events.temperatureCount==1);}
    f=notification(0x4008,343,8);Ath10k::htcWriteLE32(f.data()+16,300);assert(parse(f) && events.temperature==85);
    f=notification(0x6001,43,8);f[16]=4;assert(!parse(f) && !s.cancelRequested);f[16]=2;
    assert(!parse(f) && s.cancelRequested && s.radio.powerBlocked && !s.securityAck);
    s.cancelRequested=false;s.radio.powerBlocked=false;
    f=notification(0x1e004,231,12);Ath10k::htcWriteLE32(f.data()+24,1);
    assert(!parse(f) && events.rfKill && s.cancelRequested);Ath10k::htcWriteLE32(f.data()+24,2);
    assert(!parse(f) && !events.rfKill && s.cancelRequested); // no unsolicited resurrection
    Ath10k::htcWriteLE32(f.data()+24,0);assert(parse(f) && events.rfKillCount==2);
    f=notification(0x5003,45,12);assert(!parse(f) && events.keyCompleteCount==1 && !s.securityAck);
    f=notification(0x5006,42,4);assert(parse(f) && !s.deleteReply);
    f=notification(1,32,4);assert(parse(f));
    f=notification(0x12345,999,8);Ath10k::htcWriteLE16(f.data()+12,12);assert(parse(f) && events.unknownCount==1);
    puts("WMI notifications passed: unknown bounded events, atomic credits, temperature, peer kickout, RF-kill and no false command/key acknowledgements.");
}
