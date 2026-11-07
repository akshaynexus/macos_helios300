#include <cassert>
#include <cstring>
#include "Ath10kRX.hpp"
using namespace Ath10k;
static ConnectionState connected(){
    ConnectionState s;s.stage=LinkStage::Connected;s.secure=s.portAuthorized=true;
    s.securityPhase=5;s.peerId=7;s.peerCount=1;s.peerIDs[0]=1u<<7;
    s.local[0]=s.bssid[0]=2;s.local[5]=1;s.bssid[5]=2;return s;
}
static void descriptor(UInt8 *d,const ConnectionState &s){
    memset(d,0,2048);htcWriteLE32(d+4,0x80000000u);htcWriteLE32(d+12,(6u<<28)|(1u<<13)|7);
    htcWriteLE32(d+16,5);htcWriteLE32(d+24,18);htcWriteLE32(d+32,2<<8);htcWriteLE32(d+56,0xc000);
    htcWriteLE16(d+236,0x4288);memcpy(d+240,s.local,6);memcpy(d+246,s.bssid,6);
    d[263]=d[267]=d[271]=0x20; // all possible IV/key-control offsets
    memcpy(d+300,s.local,6);memcpy(d+306,s.bssid,6);d[312]=8;d[314]=0x45;
}
static void tkipGroupReplay(){
    auto s=connected();s.groupCipher=AthSecurity::cipherTKIP;s.groupIndex=2;
    UInt8 d[2048];descriptor(d,s);htcWriteLE16(d+236,0x4208);
    const UInt8 multicast[]={1,0,0x5e,0,0,1};memcpy(d+240,multicast,6);memcpy(d+300,multicast,6);
    htcWriteLE32(d+12,(4u<<28)|(1u<<13)|7);
    UInt8 *iv=d+260;iv[0]=2;iv[1]=0x22;iv[2]=1;iv[3]=0xa0;iv[4]=3;iv[5]=4;iv[6]=5;iv[7]=6;
    const UInt64 tsc=1u|(UInt64(2)<<8)|(UInt64(3)<<16)|(UInt64(4)<<24)|(UInt64(5)<<32)|(UInt64(6)<<40);
    assert(acceptSecureRX(s,d,d+300,18,7));assert(s.groupPN[16]==tsc);
    assert(!acceptSecureRX(s,d,d+300,18,7)&&s.rxLastReason==ConnectionState::Replay);
    s.groupPN[16]=0;htcWriteLE32(d+12,(6u<<28)|(1u<<13)|7);
    assert(!acceptSecureRX(s,d,d+300,18,7)&&s.rxLastReason==ConnectionState::Key&&!s.groupPN[16]);
}
int main(){
    tkipGroupReplay();
    auto s=connected();UInt8 d[2048];descriptor(d,s);
    // Exhaustive FC equivalence: instrumentation must not widen acceptance.
    for(UInt32 fc=0;fc<=0xffff;++fc){
        htcWriteLE16(d+236,static_cast<UInt16>(fc));s.pairPN[0]=s.pairPN[16]=0;
        const bool oldPredicate=(fc&0x470f)==0x4208;
        assert(acceptSecureRX(s,d,d+300,18,7)==oldPredicate);
        if(!oldPredicate)assert(!s.pairPN[0] && !s.pairPN[16]);
    }
    descriptor(d,s);s.rxSecurity={};
    const UInt16 frames[]={0,0x4188,0x0288,0x4688};
    const RXSecurityFault faults[]={RXSecurityFault::HeaderType,RXSecurityFault::Direction,
        RXSecurityFault::Protected,RXSecurityFault::MoreFragments};
    for(UInt32 i=0;i<4;++i){htcWriteLE16(d+236,frames[i]);
        assert(!acceptSecureRX(s,d,d+300,18,79));
        assert(s.rxSecurity.last==faults[i] && s.rxSecurity.peer==79 && s.rxSecurity.fc==frames[i]);
        assert(s.rxSecurity.counts[static_cast<UInt32>(faults[i])]==1);
    }
    descriptor(d,s);htcWriteLE16(d+258,1);assert(!acceptSecureRX(s,d,d+300,18));
    assert(s.rxSecurity.last==RXSecurityFault::FragmentNumber && s.rxSecurity.sequenceControl==1);
    descriptor(d,s);d[246]^=4;assert(!acceptSecureRX(s,d,d+300,18));
    assert(s.rxSecurity.last==RXSecurityFault::Transmitter && s.rxSecurity.originalHeader[10]==d[246]);
    // Non-QoS bytes 24/25 are crypto metadata: do not publish them as a header.
    descriptor(d,s);htcWriteLE16(d+236,0x4208);d[246]^=4;d[260]=0xab;d[261]=0xcd;
    assert(!acceptSecureRX(s,d,d+300,18));assert(!s.rxSecurity.originalHeader[24] && !s.rxSecurity.originalHeader[25]);
    // MPDU metadata is valid only on FIRST. Undefined fields on a LAST
    // descriptor must neither emit MixedCipher nor advance/compare the PN.
    descriptor(d,s);s.pairPN[0]=0;s.rxSecurity={};RXBatch b;
    htcWriteLE32(d+56,0x4000);
    assert(appendRXBatch(s,b,d,2048,1,7));const UInt32 first=connLE32(d+12);
    htcWriteLE32(d+56,0x8000);htcWriteLE32(d+12,0);htcWriteLE32(d+16,99);
    assert(appendRXBatch(s,b,d,2048,1,7) && b.count==2 && !b.failed);
    assert(connLE32(b.descriptor+12)==first && !s.pairPN[0] && !s.rxSecurity.valid);
    assert(!s.rxSecurity.counts[static_cast<UInt32>(RXSecurityFault::MixedCipher)]);
    // A new FIRST cannot inherit authorization from an existing batch.
    htcWriteLE32(d+56,0xc000);
    assert(!appendRXBatch(s,b,d,2048,1,7) && b.failed && !s.pairPN[0]);
    // Preserve actual first-header diagnostics across disconnect.
    descriptor(d,s);d[246]^=4;
    assert(!acceptSecureRX(s,d,d+300,18));
    const auto diagnostic=s.rxSecurity;const UInt32 drops=s.rxDropped;
    resetDisconnected(s);assert(s.rxDropped==drops && s.rxSecurity.last==diagnostic.last &&
        s.rxSecurity.counts[static_cast<UInt32>(RXSecurityFault::Transmitter)]==1);
}
