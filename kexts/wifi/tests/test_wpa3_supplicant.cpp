// Host test for the generalized supplicant with the SHA-256 AKMs: an
// authenticator modelled with OpenSSL (independent HMAC-SHA256 KDF, AES-CMAC,
// RFC 3394 wrap) runs the 4-way and group handshakes for AKM 00-0F-AC:8 (SAE,
// descriptor version 0), :6 (PSK-SHA256, version 3) and :5 (802.1X-SHA256)
// with PMF: PMKID KDE in message 1, RSNXE and IGTK KDE in message 3, IGTK
// rekey, and the failure cases (PMKID mismatch, RSNXE missing, wrong
// descriptor version, bad MIC).
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/params.h>
#include <openssl/core_names.h>
#include "../src/shared/Ath10kSupplicant.hpp"
struct OpenSSLCrypto {
 static bool random(uint8_t *p,size_t n){for(size_t i=0;i<n;++i)p[i]=uint8_t(i+51);return true;}
 static bool hmac(const uint8_t *k,size_t kl,const uint8_t *p,size_t n,uint8_t *o){unsigned len=0;return HMAC(EVP_sha1(),k,int(kl),p,n,o,&len)&&len==20;}
 static bool decrypt(const uint8_t *k,const uint8_t *p,uint8_t *o){
  EVP_CIPHER_CTX *c=EVP_CIPHER_CTX_new();assert(c);int n=0,last=0;
  bool ok=EVP_DecryptInit_ex(c,EVP_aes_128_ecb(),nullptr,k,nullptr)==1&&EVP_CIPHER_CTX_set_padding(c,0)==1&&EVP_DecryptUpdate(c,o,&n,p,16)==1&&EVP_DecryptFinal_ex(c,o+n,&last)==1&&n+last==16;
  EVP_CIPHER_CTX_free(c);return ok;
 }
};
using S=AthSecurity::Supplicant<OpenSSLCrypto>;
static void putBE16(uint8_t *p,uint16_t n){p[0]=uint8_t(n>>8);p[1]=uint8_t(n);}
static void putBE64(uint8_t *p,uint64_t n){for(unsigned i=0;i<8;++i)p[i]=uint8_t(n>>(56-8*i));}
// Independent references.
static void kdfSHA256(const uint8_t *key,size_t kl,const char *label,const uint8_t *ctx,size_t cl,uint8_t *out,size_t bits){
    size_t bytes=bits/8,at=0;
    for(uint16_t i=1;at<bytes;++i){
        std::vector<uint8_t> in;in.push_back(uint8_t(i));in.push_back(uint8_t(i>>8));in.insert(in.end(),label,label+strlen(label));in.insert(in.end(),ctx,ctx+cl);in.push_back(uint8_t(bits));in.push_back(uint8_t(bits>>8));
        uint8_t d[32];unsigned len=0;assert(HMAC(EVP_sha256(),key,int(kl),in.data(),in.size(),d,&len) && len==32);
        size_t take=bytes-at<32?bytes-at:32;memcpy(out+at,d,take);at+=take;
    }
}
static void cmac(const uint8_t *key,const uint8_t *p,size_t n,uint8_t *out16){
    EVP_MAC *mac=EVP_MAC_fetch(nullptr,"CMAC",nullptr);assert(mac);EVP_MAC_CTX *c=EVP_MAC_CTX_new(mac);assert(c);
    char cipher[]="AES-128-CBC";OSSL_PARAM params[]={OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_CIPHER,cipher,0),OSSL_PARAM_construct_end()};
    size_t len=0;assert(EVP_MAC_init(c,key,16,params)==1 && EVP_MAC_update(c,p,n)==1 && EVP_MAC_final(c,out16,&len,16)==1 && len==16);
    EVP_MAC_CTX_free(c);EVP_MAC_free(mac);
}
static std::vector<uint8_t> wrap(const uint8_t *kek,const std::vector<uint8_t> &plain){
    std::vector<uint8_t> out(plain.size()+8);EVP_CIPHER_CTX *c=EVP_CIPHER_CTX_new();assert(c);
    EVP_CIPHER_CTX_set_flags(c,EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);int n=0,last=0;
    assert(EVP_EncryptInit_ex(c,EVP_aes_128_wrap(),nullptr,kek,nullptr)==1 && EVP_EncryptUpdate(c,out.data(),&n,plain.data(),int(plain.size()))==1 && EVP_EncryptFinal_ex(c,out.data()+n,&last)==1);
    out.resize(size_t(n+last));EVP_CIPHER_CTX_free(c);return out;
}
struct Authenticator {
    uint8_t pmk[32],aa[6]={2,6,7,8,9,10},spa[6]={2,1,2,3,4,5},anonce[32],snonce[32],ptk[48],gtk[16],igtk[16],pmkid[16];
    uint8_t ver;uint64_t replay=1;
    std::vector<uint8_t> apRSN,apRSNXE,stationRSN,stationRSNXE;
    Authenticator(uint8_t akm,uint8_t version,bool rsnxe):ver(version){
        for(unsigned i=0;i<32;++i){pmk[i]=uint8_t(0x30+i);anonce[i]=uint8_t(0xa0+i);snonce[i]=uint8_t(i+51);}
        for(unsigned i=0;i<16;++i){gtk[i]=uint8_t(0x50+i);igtk[i]=uint8_t(0x70+i);pmkid[i]=uint8_t(0x90+i);}
        uint8_t ie[64];size_t n=AthSecurity::buildStationRSN(ie,sizeof(ie),AthSecurity::cipherCCMP,akm,true,akm==AthSecurity::akmSAE,nullptr);
        apRSN.assign(ie,ie+n);
        n=AthSecurity::buildStationRSN(ie,sizeof(ie),AthSecurity::cipherCCMP,akm,true,akm==AthSecurity::akmSAE,akm==AthSecurity::akmSAE?pmkid:nullptr);
        stationRSN.assign(ie,ie+n);
        if(rsnxe){apRSNXE={244,1,0x20};stationRSNXE={244,1,0x20};}
        uint8_t ctx[76];memcpy(ctx,spa,6);memcpy(ctx+6,aa,6);memcpy(ctx+12,snonce,32);memcpy(ctx+44,anonce,32); // spa<aa, snonce<anonce here
        kdfSHA256(pmk,32,"Pairwise key expansion",ctx,76,ptk,384);
    }
    std::vector<uint8_t> frame(uint16_t info,const uint8_t *nonce,const std::vector<uint8_t> &keyData,const uint8_t *rsc=nullptr){
        std::vector<uint8_t> f(14+99+keyData.size(),0);memcpy(f.data(),spa,6);memcpy(f.data()+6,aa,6);f[12]=0x88;f[13]=0x8e;
        uint8_t *p=f.data()+14;p[0]=2;p[1]=3;putBE16(p+2,uint16_t(95+keyData.size()));p[4]=2;putBE16(p+5,uint16_t(info|ver));putBE16(p+7,16);putBE64(p+9,replay);
        if(nonce){memcpy(p+17,nonce,32);}
        if(rsc){memcpy(p+65,rsc,6);}putBE16(p+97,uint16_t(keyData.size()));if(!keyData.empty()){memcpy(p+99,keyData.data(),keyData.size());}
        if(info&0x100){uint8_t m[16];cmac(ptk,p,95+4+keyData.size(),m);memcpy(p+81,m,16);}
        return f;
    }
    std::vector<uint8_t> m1(bool withPMKID=true){std::vector<uint8_t> kd;if(withPMKID){kd={221,20,0,15,172,4};kd.insert(kd.end(),pmkid,pmkid+16);}return frame(0x0088,anonce,kd);}
    std::vector<uint8_t> keyDataPlain(bool withRSN,bool withRSNXE,uint8_t gtkIndex,bool withIGTK,const uint8_t *ipn){
        std::vector<uint8_t> kd;if(withRSN)kd.insert(kd.end(),apRSN.begin(),apRSN.end());
        if(withRSNXE)kd.insert(kd.end(),apRSNXE.begin(),apRSNXE.end());
        kd.insert(kd.end(),{221,22,0,15,172,1,gtkIndex,0});kd.insert(kd.end(),gtk,gtk+16);
        if(withIGTK){kd.insert(kd.end(),{221,28,0,15,172,4,4,0});kd.insert(kd.end(),ipn,ipn+6);kd.insert(kd.end(),igtk,igtk+16);}
        kd.push_back(221);while(kd.size()%8)kd.push_back(0);return kd;
    }
    std::vector<uint8_t> m3(bool withRSNXE,bool withIGTK){static const uint8_t ipn[6]={1,0,0,0,0,0},rsc[6]={3,0,0,0,0,0};++replay;return frame(0x13c8,anonce,wrap(ptk+16,keyDataPlain(true,withRSNXE,1,withIGTK,ipn)),rsc);}
    std::vector<uint8_t> group(uint8_t index,const uint8_t *newGTK,const uint8_t *newIGTK,const uint8_t *ipn){memcpy(gtk,newGTK,16);if(newIGTK)memcpy(igtk,newIGTK,16);++replay;return frame(0x1380,nullptr,wrap(ptk+16,keyDataPlain(false,!apRSNXE.empty(),index,newIGTK!=nullptr,ipn)));}
    bool checkMIC(const uint8_t *f,size_t n){uint8_t copy[512],m[16];memcpy(copy,f+14,n-14);memset(copy+81,0,16);cmac(ptk,copy,n-14,m);return !memcmp(m,f+14+81,16);}
};
static void handshake(uint8_t akm,uint8_t version,bool rsnxe){
    Authenticator a(akm,version,rsnxe);S s;
    assert(s.begin(a.pmk,a.snonce,a.spa,a.aa,a.apRSN.data(),a.apRSN.size(),akm,a.stationRSN.data(),a.stationRSN.size(),true,
                   rsnxe?a.apRSNXE.data():nullptr,a.apRSNXE.size(),rsnxe?a.stationRSNXE.data():nullptr,a.stationRSNXE.size(),akm==AthSecurity::akmSAE?a.pmkid:nullptr));
    auto f=a.m1();assert(s.receive(f.data(),f.size())==S::Reply);
    // Message 2: version echoed, SNonce, key data = station RSN || RSNXE, CMAC MIC.
    const uint8_t *p=s.output+14;assert(AthSecurity::be16(p+5)==(0x0108|version) && !memcmp(p+17,a.snonce,32));
    const size_t kd=AthSecurity::be16(p+97);assert(kd==a.stationRSN.size()+a.stationRSNXE.size());
    assert(!memcmp(p+99,a.stationRSN.data(),a.stationRSN.size()) && (a.stationRSNXE.empty() || !memcmp(p+99+a.stationRSN.size(),a.stationRSNXE.data(),a.stationRSNXE.size())));
    assert(a.checkMIC(s.output,s.outputLength));
    // Message 3 with GTK + IGTK.
    f=a.m3(rsnxe,true);assert(s.receive(f.data(),f.size())==S::Keys);
    assert(!memcmp(s.keys,a.ptk+32,16) && !memcmp(s.keys+16,a.gtk,16) && s.keys[48]==4 && s.keys[49]==1 && s.keys[50]==3);
    assert(s.keys[56]==1 && s.keys[57]==4 && s.keys[58]==0 && s.keys[59]==1 && !memcmp(s.keys+65,a.igtk,16));
    assert(AthSecurity::be16(s.output+14+5)==(0x0308|version) && a.checkMIC(s.output,s.outputLength));
    // Retransmitted M3: same M4, no new keys.
    assert(s.receive(f.data(),f.size())==S::Reply);
    // Group rekey with a new GTK and IGTK (IPN advanced).
    uint8_t ngtk[16],nigtk[16],ipn[6]={9,0,0,0,0,0};for(unsigned i=0;i<16;++i){ngtk[i]=uint8_t(0xc0+i);nigtk[i]=uint8_t(0xd0+i);}
    f=a.group(2,ngtk,nigtk,ipn);assert(s.receive(f.data(),f.size())==S::GroupKeys);
    assert(!memcmp(s.keys+16,ngtk,16) && s.keys[49]==2 && s.keys[56]==1 && s.keys[59]==9 && !memcmp(s.keys+65,nigtk,16));
    assert(AthSecurity::be16(s.output+14+5)==(0x0300|version) && a.checkMIC(s.output,s.outputLength));
    // Group rekey without an IGTK KDE keeps the BIP key out of the payload.
    uint8_t ngtk2[16];for(unsigned i=0;i<16;++i)ngtk2[i]=uint8_t(0xe0+i);
    f=a.group(1,ngtk2,nullptr,ipn);assert(s.receive(f.data(),f.size())==S::GroupKeys && s.keys[56]==0);
    printf("AKM %u version %u%s: 4-way + group handshakes OK\n",akm,version,rsnxe?" with RSNXE":"");
}
static void failures(){
    // PMKID in message 1 must match the SAE PMKID.
    {Authenticator a(8,0,true);S s;uint8_t other[16];memset(other,7,16);
     assert(s.begin(a.pmk,a.snonce,a.spa,a.aa,a.apRSN.data(),a.apRSN.size(),8,a.stationRSN.data(),a.stationRSN.size(),true,a.apRSNXE.data(),3,a.stationRSNXE.data(),3,other));
     auto f=a.m1();assert(s.receive(f.data(),f.size())==S::Fatal);}
    // Message 1 without a PMKID is accepted; a wrong descriptor version is ignored.
    {Authenticator a(8,0,true);S s;
     assert(s.begin(a.pmk,a.snonce,a.spa,a.aa,a.apRSN.data(),a.apRSN.size(),8,a.stationRSN.data(),a.stationRSN.size(),true,a.apRSNXE.data(),3,a.stationRSNXE.data(),3,a.pmkid));
     Authenticator b(8,2,true);memcpy(b.pmk,a.pmk,32);auto f=b.m1(false);assert(s.receive(f.data(),f.size())==S::Ignore);
     f=a.m1(false);assert(s.receive(f.data(),f.size())==S::Reply);}
    // RSNXE advertised by the AP but missing from message 3 is fatal; a different one too.
    {Authenticator a(6,3,true);S s;
     assert(s.begin(a.pmk,a.snonce,a.spa,a.aa,a.apRSN.data(),a.apRSN.size(),6,a.stationRSN.data(),a.stationRSN.size(),true,a.apRSNXE.data(),3,a.stationRSNXE.data(),3,nullptr));
     auto f=a.m1();assert(s.receive(f.data(),f.size())==S::Reply);f=a.m3(false,true);assert(s.receive(f.data(),f.size())==S::Fatal);}
    // A PSK-SHA256 authenticator that signs with the wrong key: bad MIC.
    {Authenticator a(6,3,false);S s;
     assert(s.begin(a.pmk,a.snonce,a.spa,a.aa,a.apRSN.data(),a.apRSN.size(),6,a.stationRSN.data(),a.stationRSN.size(),false,nullptr,0,nullptr,0,nullptr));
     auto f=a.m1();assert(s.receive(f.data(),f.size())==S::Reply);a.ptk[0]^=1;f=a.m3(false,false);assert(s.receive(f.data(),f.size())==S::Fatal);}
    // AKM not offered by the AP, TKIP pairwise, MFPR without MFPC are refused by the RSN helpers.
    {AthSecurity::RSNInfo info;uint8_t ie[64];size_t n=AthSecurity::buildStationRSN(ie,64,4,8,true,true,nullptr);
     assert(AthSecurity::parseRSN(ie,n,info) && info.akm(8) && info.mfpc && info.mfpr && info.groupMgmt==6 && info.pairwiseCCMP && !info.pmkidCount);
     assert(AthSecurity::rsnAcceptable(info,8,true,true) && !AthSecurity::rsnAcceptable(info,2,true,false) && !AthSecurity::rsnAcceptable(info,8,false,false));
     assert(!AthSecurity::buildStationRSN(ie,64,4,8,false,true,nullptr));
     n=AthSecurity::buildStationRSN(ie,64,4,2,false,false,nullptr);assert(n==22 && !memcmp(ie,AthSecurity::stationRSN,22));
     const uint8_t transition[]={48,24,1,0,0,15,172,4,1,0,0,15,172,4,2,0,0,15,172,2,0,15,172,8,0x80,0};
     assert(AthSecurity::parseRSN(transition,sizeof(transition),info) && info.akm(2) && info.akm(8) && info.mfpc && !info.mfpr && !info.groupMgmt);
     assert(AthSecurity::rsnAcceptable(info,2,false,false) && AthSecurity::rsnAcceptable(info,8,true,false) && !AthSecurity::rsnAcceptable(info,5,true,false));
     static const uint8_t xe[]={244,1,0x20};assert(AthSecurity::rsnxeH2E(xe,3) && !AthSecurity::rsnxeH2E(xe,2));}
    puts("Failure cases OK");
}
int main(){
    setvbuf(stdout,nullptr,_IONBF,0);
    handshake(8,0,true);handshake(8,3,false);handshake(6,3,true);handshake(5,3,false);
    failures();return 0;
}
