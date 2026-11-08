// Host test: the Skywalk RegistrationInfo blocks stay the driver's (see
// AirportAth10kRegistration.hpp): both are 0x130 bytes, both family slots point
// at them after install, release clears exactly those slots and frees exactly
// what install allocated (same size), leaves a slot the family replaced alone,
// and is idempotent. The reboot panic of 2026-11-07 was the family typed-freeing
// a block it did not allocate.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include "../src/AirportAth10kRegistration.hpp"
static std::map<void *,size_t> live;static unsigned allocs=0,frees=0;static bool failNext=false;
static void *alloc(size_t n){if(failNext){failNext=false;return nullptr;}void *p=malloc(n);live[p]=n;++allocs;return p;}
static void release(void *p,size_t n){auto it=live.find(p);assert(it!=live.end() && "freed a block we did not allocate");assert(it->second==n && "freed with another size");live.erase(it);free(p);++frees;}
int main(){
    using namespace AirportRegistration;
    static_assert(kInfoBytes==0x130,"IOSkywalkEthernetInterface::RegistrationInfo on 25G332");
    unsigned char info[kInfoBytes];for(size_t i=0;i<sizeof(info);++i)info[i]=(unsigned char)(i*7);
    void *familyNet=nullptr,*familyEth=nullptr;
    Blocks b;
    assert(!b.install(&familyNet,&familyEth,info,kInfoBytes-1,alloc,release));   // wrong size refused
    assert(!b.installed() && allocs==0);
    assert(b.install(&familyNet,&familyEth,info,sizeof(info),alloc,release));
    assert(b.installed() && allocs==2 && familyNet==b.network && familyEth==b.ethernet && familyNet!=familyEth);
    assert(live.at(familyNet)==kInfoBytes && live.at(familyEth)==kInfoBytes);
    assert(!memcmp(familyNet,info,sizeof(info)) && !memcmp(familyEth,info,sizeof(info)));
    assert(!b.install(&familyNet,&familyEth,info,sizeof(info),alloc,release));   // no double install
    b.release(&familyNet,&familyEth,release);
    assert(!familyNet && !familyEth && !b.installed() && frees==2 && live.empty());
    b.release(&familyNet,&familyEth,release);                                      // idempotent
    assert(frees==2);
    // The family replaced the network slot with its own block: that one stays.
    assert(b.install(&familyNet,&familyEth,info,sizeof(info),alloc,release));
    unsigned char familyOwned[kInfoBytes]{};void *ours=familyNet;familyNet=familyOwned;
    b.release(&familyNet,&familyEth,release);
    assert(familyNet==familyOwned && !familyEth && live.empty() && frees==4);(void)ours;
    // Allocation failure leaves nothing behind and the slots untouched.
    familyNet=familyEth=nullptr;failNext=true;
    assert(!b.install(&familyNet,&familyEth,info,sizeof(info),alloc,release));
    assert(!familyNet && !familyEth && live.empty() && !b.installed());
    // Releasing with NULL slot pointers (stop after the family already dropped its expansion data) still frees ours.
    assert(b.install(&familyNet,&familyEth,info,sizeof(info),alloc,release));
    b.release(nullptr,nullptr,release);
    assert(live.empty() && !b.installed());
    puts("native registration-block ownership tests passed");
    return 0;
}
