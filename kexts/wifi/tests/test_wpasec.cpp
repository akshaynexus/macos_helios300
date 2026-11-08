// WPA3-SAE, PMF and 802.1X through the production core and front end (gate on).
//
//   AirportBridge::Session (front end)
//     -> AthControl ops through the production Ath10kPCIDevice::controlAction
//     -> production serviceConnection, receiveStationManagement, buildHTTPacket
//     -> simulated firmware and AP: the Dragonfly exchange from the AP side with
//        an independent AthSAE instance, anti-clogging, EAPOL 4-way with an
//        OpenSSL authenticator (test_wpa3_supplicant's), PMKSA caching, SA Query.
//
// The data-path pieces of 802.1X (EAP up to the stack, EAPOL out before the
// port opens) are in test_network_rx.cpp and test_native_tx.cpp (production
// releaseRXBatch/outputPacket); here the EAPOL frames enter at the core's queue.
#if !AIRPORT_ATH10K_WPASEC
#error "build with -DAIRPORT_ATH10K_WPASEC=1"
#endif
#include "Ath10kVersion.hpp"
#include <cstdio>
#include "../src/shared/Ath10kControl.hpp"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/params.h>
#include <openssl/core_names.h>
#define ATH10K_NATIVE_TX_MEMBERS \
    bool _nativeFrontEnd=true; \
    bool &_networkLinkUp=networkUp; \
    static IOReturn controlAction(OSObject *,void *,void *,void *,void *); \
    void fillControlInfo(AthControl::Response &); \
    bool requestDeviceRestart(){return false;}
#define ATH10K_CHANNEL_SWITCH_TEST
#include "test_connection.cpp"
constexpr IOReturn kIOReturnNotFound=-1000,kIOReturnExclusiveAccess=-1001,kIOReturnUnsupported=-1002,kIOReturnNotPermitted=-1003,kIOReturnBusy=-1004;
#include "control_info_method.inc"
#include "control_method.inc"
#include "../native/src/AirportAth10kCrypto.hpp"
#include "../native/src/AirportAth10kBridge.hpp"
namespace B=AirportBridge;namespace W=AirportWCL;
static uint8_t stationRandomSeed=7;
static bool testRandom(uint8_t *p,size_t n){for(size_t i=0;i<n;++i)p[i]=uint8_t(i*stationRandomSeed+1);return true;}
using Crypto=AirportCrypto::Policy<testRandom>;
using Session=B::Session<Crypto>;
static bool apRandom(uint8_t *p,size_t n){for(size_t i=0;i<n;++i)p[i]=uint8_t(0x5a^(i*13+3));return true;}
using ApSAE=AthSAE::Session<apRandom>;

struct TestCore:B::Core{
    ConnectionDriver &d;void *who;
    TestCore(ConnectionDriver &driver,void *client):d(driver),who(client){}
    int call(const AthControl::Request &r,AthControl::Response &o) override {
        const int rc=Ath10kPCIDevice::controlAction(&d,who,const_cast<AthControl::Request *>(&r),&o,nullptr);
        switch(rc){case 0:return B::kSuccess;case kIOReturnBadArgument:return B::kBadArgument;case kIOReturnNotReady:return B::kNotReady;case kIOReturnError:return B::kError;
        case kIOReturnNotFound:return B::kNotFound;case kIOReturnExclusiveAccess:return B::kExclusiveAccess;case kIOReturnUnsupported:return B::kUnsupported;case kIOReturnNotPermitted:return B::kNotPermitted;case kIOReturnBusy:return B::kBusy;}
        return B::kError;
    }
};
struct Message{uint32_t type;std::vector<uint8_t> data;};
struct FakeSink:B::Sink{
    std::vector<Message> messages;std::vector<bool> links;
    void post(uint32_t m,const void *d,size_t n) override {Message e{m,{}};if(n)e.data.assign((const uint8_t *)d,(const uint8_t *)d+n);messages.push_back(e);}
    void linkState(bool up) override {links.push_back(up);}
    size_t count(uint32_t t) const {size_t c=0;for(auto &m:messages)if(m.type==t)++c;return c;}
    const Message *last(uint32_t t) const {for(size_t i=messages.size();i>0;--i)if(messages[i-1].type==t)return &messages[i-1];return nullptr;}
};
static const char kSSID[]="HomeNet";
static const uint8_t kPassword[]="correct horse battery";
// --- A WMI management RX event (test_connection's managementResponse layout) --
static std::vector<UInt8> mgmtEvent(const Ath10k::ConnectionState &s,UInt8 fc0,UInt8 fc1,const std::vector<UInt8> &body,const UInt8 *ra=nullptr){
    const UInt32 n=24+UInt32(body.size()),aligned=(n+3u)&~3u;
    std::vector<UInt8> p(60+aligned);p[0]=2;
    Ath10k::htcWriteLE16(p.data()+2,UInt16(p.size()-8));Ath10k::htcWriteLE32(p.data()+8,0x7001);p[12]=40;p[14]=44;
    Ath10k::htcWriteLE32(p.data()+16,s.ap.rxChannel);Ath10k::htcWriteLE32(p.data()+32,n);
    Ath10k::htcWriteLE16(p.data()+56,UInt16(aligned));p[58]=17;
    UInt8 *f=p.data()+60;f[0]=fc0;f[1]=fc1;
    for(unsigned i=0;i<6;++i){f[4+i]=ra?ra[i]:s.local[i];f[10+i]=f[16+i]=s.bssid[i];}
    if(!body.empty())memcpy(f+24,body.data(),body.size());
    return p;
}
// --- EAPOL 4-way from the AP side (test_wpa3_supplicant's authenticator) ------
static void putBE16(uint8_t *p,uint16_t v){p[0]=uint8_t(v>>8);p[1]=uint8_t(v);}
static void putBE64(uint8_t *p,uint64_t v){for(int i=0;i<8;++i)p[i]=uint8_t(v>>(56-8*i));}
static void kdfSHA256(const uint8_t *key,size_t kl,const char *label,const uint8_t *ctx,size_t cl,uint8_t *out,size_t bits){
    size_t bytes=bits/8,at=0;
    for(uint16_t i=1;at<bytes;++i){
        std::vector<uint8_t> in;in.push_back(uint8_t(i));in.push_back(uint8_t(i>>8));in.insert(in.end(),label,label+strlen(label));in.insert(in.end(),ctx,ctx+cl);in.push_back(uint8_t(bits));in.push_back(uint8_t(bits>>8));
        uint8_t d[32];unsigned len=0;assert(HMAC(EVP_sha256(),key,int(kl),in.data(),in.size(),d,&len) && len==32);
        size_t take=bytes-at<32?bytes-at:32;memcpy(out+at,d,take);at+=take;
    }
}
static void prfSHA1(const uint8_t *key,const char *label,const uint8_t *ctx,uint8_t *out){
    size_t labelLength=strlen(label)+1;
    std::vector<uint8_t> in;in.insert(in.end(),label,label+labelLength);in.insert(in.end(),ctx,ctx+76);in.push_back(0);
    for(unsigned b=0;b<3;++b){in.back()=uint8_t(b);uint8_t d[20];unsigned len=0;assert(HMAC(EVP_sha1(),key,32,in.data(),in.size(),d,&len)&&len==20);memcpy(out+20*b,d,b==2?8:20);}
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
    uint8_t pmk[32]{},aa[6]{},spa[6]{},anonce[32]{},snonce[32]{},ptk[48]{},gtk[16],igtk[16],pmkid[16]{};
    uint8_t akm=0,version=2;bool sha256=false,havePMKID=false;uint64_t replay=1;
    std::vector<uint8_t> apRSN,apRSNXE;
    Authenticator(){for(unsigned i=0;i<32;++i)anonce[i]=uint8_t(0xa0+i);for(unsigned i=0;i<16;++i){gtk[i]=uint8_t(0x50+i);igtk[i]=uint8_t(0x70+i);}}
    void setKey(const uint8_t *pmk32,const uint8_t *ap,const uint8_t *sta,uint8_t akmType,const uint8_t *id){
        memcpy(pmk,pmk32,32);memcpy(aa,ap,6);memcpy(spa,sta,6);akm=akmType;sha256=AthSecurity::akmSHA256(akmType);version=sha256?3:2;
        havePMKID=id!=nullptr;if(id)memcpy(pmkid,id,16);replay=1;
    }
    void derive(){
        uint8_t ctx[76];auto less=[](const uint8_t *a,const uint8_t *b,size_t n){return memcmp(a,b,n)<0;};
        bool s=less(spa,aa,6);memcpy(ctx,s?spa:aa,6);memcpy(ctx+6,s?aa:spa,6);
        s=less(snonce,anonce,32);memcpy(ctx+12,s?snonce:anonce,32);memcpy(ctx+44,s?anonce:snonce,32);
        if(sha256)kdfSHA256(pmk,32,"Pairwise key expansion",ctx,76,ptk,384);else prfSHA1(pmk,"Pairwise key expansion",ctx,ptk);
    }
    void mic(uint8_t *p,size_t n,uint8_t *out){
        if(sha256)cmac(ptk,p,n,out);
        else {uint8_t d[20];unsigned len=0;assert(HMAC(EVP_sha1(),ptk,16,p,n,d,&len)&&len==20);memcpy(out,d,16);}
    }
    std::vector<uint8_t> frame(uint16_t info,const uint8_t *nonce,const std::vector<uint8_t> &keyData,const uint8_t *rsc=nullptr){
        std::vector<uint8_t> f(14+99+keyData.size(),0);memcpy(f.data(),spa,6);memcpy(f.data()+6,aa,6);f[12]=0x88;f[13]=0x8e;
        uint8_t *p=f.data()+14;p[0]=2;p[1]=3;putBE16(p+2,uint16_t(95+keyData.size()));p[4]=2;putBE16(p+5,uint16_t(info|version));putBE16(p+7,16);putBE64(p+9,replay);
        if(nonce)memcpy(p+17,nonce,32);
        if(rsc){memcpy(p+65,rsc,6);}
        putBE16(p+97,uint16_t(keyData.size()));if(!keyData.empty())memcpy(p+99,keyData.data(),keyData.size());
        if(info&0x100){uint8_t m[16];mic(p,99+keyData.size(),m);memcpy(p+81,m,16);}
        return f;
    }
    std::vector<uint8_t> m1(){std::vector<uint8_t> kd;if(havePMKID){kd={221,20,0,15,172,4};kd.insert(kd.end(),pmkid,pmkid+16);}return frame(0x0088,anonce,kd);}
    bool takeM2(const uint8_t *eapol,size_t n,size_t *rsnOffset=nullptr){
        memcpy(snonce,eapol+14+17,32);derive();
        uint8_t copy[512];memcpy(copy,eapol+14,n-14);memset(copy+81,0,16);uint8_t m[16];mic(copy,n-14,m);
        if(memcmp(m,eapol+14+81,16))return false;
        if(rsnOffset)*rsnOffset=14+99;
        return true;
    }
    std::vector<uint8_t> m3(bool withIGTK){
        std::vector<uint8_t> kd=apRSN;kd.insert(kd.end(),apRSNXE.begin(),apRSNXE.end());
        kd.insert(kd.end(),{221,22,0,15,172,1,1,0});kd.insert(kd.end(),gtk,gtk+16);
        if(withIGTK){const uint8_t ipn[6]={1,0,0,0,0,0};kd.insert(kd.end(),{221,28,0,15,172,4,4,0});kd.insert(kd.end(),ipn,ipn+6);kd.insert(kd.end(),igtk,igtk+16);}
        kd.push_back(221);while(kd.size()%8)kd.push_back(0);
        static const uint8_t rsc[6]={3,0,0,0,0,0};++replay;return frame(0x13c8,anonce,wrap(ptk+16,kd),rsc);
    }
    bool checkM4(const uint8_t *eapol,size_t n){uint8_t copy[512],m[16];memcpy(copy,eapol+14,n-14);memset(copy+81,0,16);mic(copy,n-14,m);return !memcmp(m,eapol+14+81,16);}
};
// --- Simulated AP and firmware over a production ConnectionDriver -------------
struct Harness {
    ConnectionDriver &d;Authenticator auth;
    FakeSink sink;int who=0;TestCore core;Session s;uint64_t t=1300;
    // AP configuration
    uint8_t akm=8;bool mfpr=true,mfpc=true,h2e=false,transition=false;
    bool tokenRequired=true,tokenConsumed=false,rejectGroup=false,badGroup=false,rejectAssocStatus53=false,wrongPassword=false;
    uint8_t token[8]={0xde,0xad,0xbe,0xef,1,2,3,4};
    unsigned commits=0,confirms=0,openAuths=0,assocs=0;
    bool assocHasPMKID=false,assocHasRSNXE=false,assocMFPR=false;std::vector<uint8_t> assocPMKID;
    ApSAE ap;bool apBegun=false,apConfirmed=false;uint8_t apPMK[32]{},apPMKID[16]{};bool saeDone=false;
    std::vector<std::vector<uint8_t>> eapolTx,saQueryTx;
    std::vector<UInt8> lastPeerAssoc;bool lastHttProtected=false;
    Harness(ConnectionDriver &driver):d(driver),core(driver,&who),s(core,sink){
        auto &st=d._connection;st.stage=LinkStage::Disabled;st.bssid[0]=0;
        d._wmiBSS.count=1;auto &b=d._wmiBSS.entries[0];b.ssidLength=7;memcpy(b.ssid,kSSID,7);
        b.privacy=true;b.rsnPresent=true;b.capabilities=0x431;
        wireFirmware();
    }
    void configureRSN(){
        auto &b=d._wmiBSS.entries[0];
        uint8_t ie[64];
        size_t n=AthSecurity::buildStationRSN(ie,sizeof(ie),AthSecurity::cipherCCMP,akm,mfpc,mfpr,nullptr);
        std::vector<uint8_t> rsn(ie,ie+n);
        if(transition){ // PSK + SAE AKMs, PMF capable
            rsn[14]=2;rsn[1]=uint8_t(n-2+4);
            const uint8_t psk[4]={0,15,172,2};rsn.insert(rsn.begin()+16,psk,psk+4);
        }
        b.rsnLength=UInt16(rsn.size());memcpy(b.rsn,rsn.data(),rsn.size());
        if(h2e){const uint8_t xe[3]={244,1,0x20};b.rsnxeLength=3;memcpy(b.rsnxe,xe,3);}else b.rsnxeLength=0;
        auth.apRSN=rsn;auth.apRSNXE.assign(h2e?std::initializer_list<uint8_t>{244,1,0x20}:std::initializer_list<uint8_t>{});
    }
    void authResponse(uint16_t algorithm,uint16_t sequence,uint16_t status,const std::vector<uint8_t> &payload){
        std::vector<UInt8> body(6);body[0]=uint8_t(algorithm);body[1]=uint8_t(algorithm>>8);body[2]=uint8_t(sequence);body[3]=uint8_t(sequence>>8);body[4]=uint8_t(status);body[5]=uint8_t(status>>8);
        body.insert(body.end(),payload.begin(),payload.end());
        d.deliver(2,mgmtEvent(d._connection,0xb0,0,body));
    }
    // The AP's SAE side: Commit answered, Confirm verified and answered.
    void onSAE(const UInt8 *body,UInt32 n){
        const uint16_t seq=uint16_t(body[2]|(body[3]<<8)),status=uint16_t(body[4]|(body[5]<<8));
        const UInt8 *payload=body+6;UInt32 m=n-6;
        if(seq==1){
            ++commits;assert(status==(h2e?AthSAE::kStatusH2E:0));
            if(rejectGroup){std::vector<uint8_t> g={19,0};authResponse(3,1,AthSAE::kStatusUnsupportedGroup,g);return;}
            if(tokenRequired && !tokenConsumed){
                if(!h2e){std::vector<uint8_t> t={19,0};t.insert(t.end(),token,token+8);authResponse(3,1,AthSAE::kStatusAntiCloggingTokenRequired,t);}
                else {std::vector<uint8_t> t={19,0,AthSAE::kExtensionElement,9,AthSAE::kExtAntiCloggingToken};t.insert(t.end(),token,token+8);authResponse(3,1,AthSAE::kStatusAntiCloggingTokenRequired,t);}
                tokenConsumed=true;return;
            }
            std::vector<UInt8> c(payload,payload+m);
            if(tokenRequired){ // strip the token: raw after the group for H&P, container at the end for H2E
                if(!h2e){assert(!memcmp(c.data()+2,token,8));c.erase(c.begin()+2,c.begin()+10);}
                else {assert(c.size()>=2+96+11 && c[2+96]==AthSAE::kExtensionElement && c[2+96+2]==AthSAE::kExtAntiCloggingToken && !memcmp(&c[2+96+3],token,8));c.resize(2+96);}
            }
            static const uint8_t other[]="not the password";
            assert(ap.begin(d._connection.bssid,d._connection.local,wrongPassword?other:kPassword,wrongPassword?sizeof(other)-1:sizeof(kPassword)-1,nullptr,0,(const uint8_t *)kSSID,7,h2e));
            uint8_t out[300];const size_t commitLen=ap.buildCommit(out,sizeof(out));assert(commitLen>=98);
            std::vector<uint8_t> reply(out,out+commitLen);
            if(badGroup){reply[0]=20;}
            assert(ap.receiveCommit(c.data(),c.size(),status));
            apBegun=true;authResponse(3,1,ap.authStatus(),reply);return;
        }
        if(seq==2){
            ++confirms;assert(apBegun && status==0 && m>=34);
            if(wrongPassword){assert(!ap.receiveConfirm(payload,m));return;} // AP silently drops: no Confirm reply
            assert(ap.receiveConfirm(payload,m));memcpy(apPMK,ap.pmk,32);memcpy(apPMKID,ap.pmkid,16);apConfirmed=true;saeDone=true;
            uint8_t out[64];const size_t len=ap.buildConfirm(out,sizeof(out));assert(len==34);
            authResponse(3,2,0,std::vector<uint8_t>(out,out+len));
        }
    }
    void onAssoc(const UInt8 *f,UInt32 n){
        ++assocs;
        // Station RSN in the association request.
        for(UInt32 p=24+4+2+d._connection.ssidLength;p+2<=n;p+=2+f[p+1]){
            if(p+2+f[p+1]>n)break;
            if(f[p]==48){
                AthSecurity::RSNInfo info;assert(AthSecurity::parseRSN(f+p,2u+f[p+1],info));
                assocMFPR=info.mfpr;assocHasPMKID=info.pmkidCount==1;
                if(assocHasPMKID)assocPMKID.assign(info.pmkids,info.pmkids+16);
            }
            if(f[p]==244)assocHasRSNXE=true;
        }
        const UInt16 status=rejectAssocStatus53?53:0;rejectAssocStatus53=false;
        d.deliver(2,managementResponse(d._connection,true,status));
    }
    void wireFirmware(){
        auto prev3=d._regs.tx3Consume;
        d._regs.tx3Consume=[this,prev3](){
            auto &st=d._connection;const UInt8 *f=static_cast<const UInt8 *>(d._connectionCommand.virtualAddress());
            if(Ath10k::wmiReadLE32(f+8)==0x6005)lastPeerAssoc.assign(f,f+200);
            if(st.radio.step){
                ++d.commands;
                if(st.radio.step==8)d.deliver(2,echoPacket(Ath10k::radioCookie(st)));
                else d.deliver(2,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});
                return;
            }
            if(st.stats.step || st.maintenance.step){prev3();return;}
            if(st.disconnect==Ath10k::DisconnectStage::ClearPair || st.disconnect==Ath10k::DisconnectStage::ClearGroup){
                std::vector<UInt8> p={1,0,28,0,0,59,0,0,11,128,71,0,0,0,0,0,0,0,0,0,136,54,67,0,4,0,0,1,0,0,192,0,1,0,4,0};
                p[9]=st.disconnect==Ath10k::DisconnectStage::ClearPair?0x80:0;Ath10k::htcWriteLE16(p.data()+10,st.peerId);
                d.deliver(2,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});d.deliver(1,p);return;
            }
            if(st.stage==LinkStage::Connected && st.securityPhase && st.disconnect==Ath10k::DisconnectStage::None){
                assert(st.wmiOwned);++d.commands;
                if(st.securityPhase==1 || st.securityPhase==2){
                    std::vector<UInt8> p={1,0,28,0,0,59,0,0,11,128,71,0,0,0,0,0,0,0,0,0,136,54,67,0,4,0,0,1,0,0,192,0,1,0,4,0};
                    p[9]=Ath10k::expectedSecurityIndication(st);Ath10k::htcWriteLE16(p.data()+10,st.peerId);
                    d.deliver(2,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});d.deliver(1,p);return;
                }
                if(st.securityPhase==4){d.deliver(2,echoPacket(0xa7100057));return;}
                d.deliver(2,{0,2,8,0,8,0,0,0,1,4,0,0,2,1,0,0});return;
            }
            prev3();
        };
        d._regs.tx4Consume=[this](){
            auto &st=d._connection;++d.packets;
            const UInt8 *cmd=static_cast<const UInt8 *>(d._packetCommand.virtualAddress());
            const UInt16 bytes=Ath10k::htcReadLE16(cmd+12);
            const UInt8 *f=static_cast<const UInt8 *>(d._packetBuffer.virtualAddress());
            if(st.txManagement){
                d.deliver(1,mgmtCompletion(st.txId));
                const UInt8 fc=f[0]&0xfc;
                if(fc==0xb0){
                    const uint16_t algorithm=uint16_t(f[24]|(f[25]<<8));
                    if(algorithm==3)onSAE(f+24,bytes-24);
                    else {assert(algorithm==0);++openAuths;d.deliver(2,managementResponse(st,false));}
                }else if(fc==0){onAssoc(f,bytes);}
                else if(fc==0xd0 && bytes>=32+4 && f[1]==0x40){saQueryTx.emplace_back(f,f+bytes);lastHttProtected=cmd[9]==0x61;}
            }else{
                d.deliver(1,completion(st.txId));
                if(bytes>=14 && f[12]==0x88 && f[13]==0x8e)eapolTx.emplace_back(f,f+bytes);
            }
        };
    }
    void pushEAPOL(const std::vector<uint8_t> &frame){
        auto &st=d._connection;assert(st.eapolCount<4 && frame.size()<=512);
        memcpy(st.eapolFrames[st.eapolWrite],frame.data(),frame.size());st.eapolLength[st.eapolWrite]=UInt16(frame.size());
        st.eapolWrite=(st.eapolWrite+1)%4;++st.eapolCount;
    }
    void step(){s.poll(t);d.tick(t);t+=10;}
    void steps(unsigned n){for(unsigned i=0;i<n;++i)step();}
    bool runUntil(std::function<bool()> done,unsigned maxSteps=800){for(unsigned i=0;i<maxSteps && !done();++i)step();return done();}
    // WCL join request for this network.
    W::JoinRequest request(uint32_t upperAuth,uint32_t cipher,const uint8_t *key,uint32_t keyLength) const {
        W::JoinRequest j{};j.lowerAuth=1;j.upperAuth=upperAuth;j.ssidLength=7;memcpy(j.ssid,kSSID,7);memcpy(j.bssid,d._wmiBSS.entries[0].bssid,6);
        j.keyCipher=cipher;j.keyLength=keyLength;if(keyLength)memcpy(j.key,key,keyLength);j.channelSpec=B::channelSpec(6);return j;
    }
    void ready(){d._connection.stage=LinkStage::Disabled;step();assert(s.state()==Session::State::Ready);}
    void fourWay(const uint8_t *pmk,const uint8_t *id,bool igtk){
        assert(runUntil([&]{return d._connection.stage==LinkStage::Connected;}));
        auth.setKey(pmk,d._connection.bssid,d._connection.local,akm,id);
        pushEAPOL(auth.m1());
        assert(runUntil([&]{return eapolTx.size()>=1;}));
        size_t rsnAt=0;assert(eapolTx[0].size()>=14+99 && auth.takeM2(eapolTx[0].data(),eapolTx[0].size(),&rsnAt));
        pushEAPOL(auth.m3(igtk));
        assert(runUntil([&]{return eapolTx.size()>=2;}));
        assert(auth.checkM4(eapolTx[1].data(),eapolTx[1].size()));
    }
};

static void assertPeerAssocPMF(const Harness &h,bool pmf){
    assert(h.lastPeerAssoc.size()>=40 && ((Ath10k::connLE32(h.lastPeerAssoc.data()+36)&0x08000000u)!=0)==pmf);
}
static std::vector<UInt8> protectedAction(UInt8 action,UInt16 tid,uint64_t pn=1){
    std::vector<UInt8> body(8+4+8,0);body[0]=UInt8(pn);body[1]=UInt8(pn>>8);body[3]=0x20;body[4]=UInt8(pn>>16);
    body[8]=8;body[9]=action;body[10]=UInt8(tid);body[11]=UInt8(tid>>8);return body;
}
static void saeHuntingAndPecking(){
    ConnectionDriver d;Harness h(d);h.configureRSN();h.ready();
    assert(h.s.startJoin(h.request(W::kAuthWPA3SAE,W::kCipherPWD,kPassword,sizeof(kPassword)-1),h.t)==B::kSuccess);
    assert(h.runUntil([&]{return h.saeDone;}));
    assert(h.commits==2 && h.tokenConsumed && h.confirms==1 && h.openAuths==0); // commit, token retry, confirm
    h.fourWay(h.apPMK,nullptr,true);
    assert(h.runUntil([&]{return h.s.linkUp();}));
    auto &st=d._connection;
    assert(st.pmf && st.pmfRequired && st.externalAuth && st.igtkValid && !memcmp(st.igtk,h.auth.igtk,16) && st.portAuthorized && st.akm==8);
    assertPeerAssocPMF(h,true);
    assert(h.assocMFPR && !h.assocHasPMKID && !h.assocHasRSNXE);
    assert(h.s.pmksaCount()==1 && h.sink.count(W::ConnectComplete)==1);
    auto *c=reinterpret_cast<const W::ConnectionEvent *>(h.sink.last(W::ConnectComplete)->data.data());assert(c->status==0);
    AthControl::Request q{};AthControl::Response o{};q.operation=AthControl::Info;assert(h.core.call(q,o)==0 && o.length==128 && (o.data[116]&2));
    puts("SAE (H&P, anti-clogging, group 19) through core + front end: OK");
}
static void saeHashToElement(){
    ConnectionDriver d;Harness h(d);h.h2e=true;h.mfpr=false;h.configureRSN();h.ready();
    assert(h.s.startJoin(h.request(W::kAuthWPA3SAE,W::kCipherPWD,kPassword,sizeof(kPassword)-1),h.t)==B::kSuccess);
    assert(h.runUntil([&]{return h.saeDone;}));
    assert(h.commits==2 && h.tokenConsumed);
    h.fourWay(h.apPMK,nullptr,false);
    assert(h.runUntil([&]{return h.s.linkUp();}));
    assert(h.assocHasRSNXE && !h.assocMFPR && d._connection.pmf && !d._connection.igtkValid);
    puts("SAE hash-to-element with token container and RSNXE: OK");
}
static void saeFailures(){
    { // AP rejects group 19.
        ConnectionDriver d;Harness h(d);h.rejectGroup=true;h.configureRSN();h.ready();
        assert(h.s.startJoin(h.request(W::kAuthWPA3SAE,W::kCipherPWD,kPassword,sizeof(kPassword)-1),h.t)==B::kSuccess);
        assert(h.runUntil([&]{return !h.s.joining();}));
        auto *c=reinterpret_cast<const W::ConnectionEvent *>(h.sink.last(W::ConnectComplete)->data.data());assert(c->status==1 && c->reason==23);
    }
    { // AP answers with another group: the Commit is refused.
        ConnectionDriver d;Harness h(d);h.badGroup=true;h.tokenRequired=false;h.configureRSN();h.ready();
        assert(h.s.startJoin(h.request(W::kAuthWPA3SAE,W::kCipherPWD,kPassword,sizeof(kPassword)-1),h.t)==B::kSuccess);
        assert(h.runUntil([&]{return !h.s.joining();}));
        assert(!strcmp(h.s.lastError,"sae-group-mismatch"));
    }
    { // Wrong password: the AP never confirms, the station retransmits and gives up.
        ConnectionDriver d;Harness h(d);h.wrongPassword=true;h.tokenRequired=false;h.configureRSN();h.ready();
        assert(h.s.startJoin(h.request(W::kAuthWPA3SAE,W::kCipherPWD,kPassword,sizeof(kPassword)-1),h.t)==B::kSuccess);
        assert(h.runUntil([&]{return !h.s.joining();},900));
        assert(!strcmp(h.s.lastError,"sae-no-response") && h.confirms>=2);
    }
    puts("SAE failures (group rejected, group mismatch, wrong password): OK");
}
static void pmksaCaching(){
    ConnectionDriver d;Harness h(d);h.tokenRequired=false;h.configureRSN();h.ready();
    auto join=[&]{assert(h.s.startJoin(h.request(W::kAuthWPA3SAE,W::kCipherPWD,kPassword,sizeof(kPassword)-1),h.t)==B::kSuccess);};
    join();assert(h.runUntil([&]{return h.saeDone;}));h.fourWay(h.apPMK,nullptr,true);assert(h.runUntil([&]{return h.s.linkUp();}));
    assert(h.commits==1 && h.s.pmksaCount()==1);
    uint8_t pmkid[16];memcpy(pmkid,h.apPMKID,16);
    auto leave=[&]{h.s.leaveNetwork(h.t);assert(h.runUntil([&]{return h.s.state()==Session::State::Ready;}));};
    leave();h.eapolTx.clear();h.assocHasPMKID=false;
    join();
    assert(h.runUntil([&]{return h.s.joining() && h.assocs==2;}));
    assert(h.commits==1 && h.openAuths==1 && h.assocHasPMKID && !memcmp(h.assocPMKID.data(),pmkid,16)); // Open System + PMKID, no SAE frames
    h.fourWay(h.apPMK,pmkid,true);assert(h.runUntil([&]{return h.s.linkUp();}));
    leave();h.eapolTx.clear();h.assocHasPMKID=false;h.rejectAssocStatus53=true; // AP lost the PMKSA: status 53, full SAE follows
    join();assert(h.runUntil([&]{return h.commits==2 && h.saeDone;}));h.saeDone=false;
    assert(h.assocs==3 && h.assocHasPMKID); // the rejected association named the PMKID
    h.saeDone=true;
    uint8_t fresh[32];memcpy(fresh,h.apPMK,32);
    assert(h.runUntil([&]{return h.assocs==4;}));
    assert(!h.assocHasPMKID || h.assocs==4);
    h.fourWay(fresh,nullptr,true);assert(h.runUntil([&]{return h.s.linkUp();}));
    assert(h.s.pmksaCount()==1);
    h.s.clearPMKSA();assert(h.s.pmksaCount()==0);
    leave();
    puts("PMKSA caching (Open System + PMKID, status 53 fallback, clear): OK");
}
static void transitionPrefersSAE(){
    { // Password from airportd: SAE wins on a PSK+SAE network.
        ConnectionDriver d;Harness h(d);h.transition=true;h.mfpr=false;h.tokenRequired=false;h.configureRSN();h.ready();
        assert(h.s.startJoin(h.request(W::kAuthWPA3SAE|(1u<<3),W::kCipherPWD,kPassword,sizeof(kPassword)-1),h.t)==B::kSuccess);
        assert(h.runUntil([&]{return h.saeDone;}));assert(h.commits==1 && h.openAuths==0);
        h.fourWay(h.apPMK,nullptr,false);assert(h.runUntil([&]{return h.s.linkUp();}));
        assert(d._connection.akm==8);
    }
    { // Only a PMK: the PSK side, still with PMF on the PMF-capable AP.
        ConnectionDriver d;Harness h(d);h.transition=true;h.mfpr=false;h.tokenRequired=false;h.configureRSN();h.ready();
        uint8_t pmk[32];for(unsigned i=0;i<32;++i)pmk[i]=uint8_t(i+1);
        assert(h.s.startJoin(h.request((1u<<3)|W::kAuthWPA3SAE,6,pmk,32),h.t)==B::kSuccess);
        h.akm=2;h.fourWay(pmk,nullptr,false);assert(h.runUntil([&]{return h.s.linkUp();}));
        assert(h.commits==0 && h.openAuths==1 && d._connection.akm==2 && d._connection.pmf);
        assertPeerAssocPMF(h,true);
    }
    puts("Transition networks: SAE preferred with a password, PSK with a PMK: OK");
}
static void plainWPA2(){
    ConnectionDriver d;Harness h(d);h.akm=2;h.mfpc=false;h.mfpr=false;h.tokenRequired=false;h.configureRSN();h.ready();
    uint8_t pmk[32];for(unsigned i=0;i<32;++i)pmk[i]=uint8_t(0x40+i);
    assert(h.s.startJoin(h.request(1u<<3,6,pmk,32),h.t)==B::kSuccess);
    h.fourWay(pmk,nullptr,false);assert(h.runUntil([&]{return h.s.linkUp();}));
    auto &st=d._connection;
    assert(!st.stationRSNLength && st.akm==2 && !st.pmf && !st.externalAuth && h.openAuths==1 && h.commits==0);
    assertPeerAssocPMF(h,false);
    puts("WPA2-PSK without PMF stays on the original path: OK");
}
static void pmf(){
    ConnectionDriver d;Harness h(d);h.tokenRequired=false;h.configureRSN();h.ready();
    assert(h.s.startJoin(h.request(W::kAuthWPA3SAE,W::kCipherPWD,kPassword,sizeof(kPassword)-1),h.t)==B::kSuccess);
    assert(h.runUntil([&]{return h.saeDone;}));h.fourWay(h.apPMK,nullptr,true);assert(h.runUntil([&]{return h.s.linkUp();}));
    auto &st=d._connection;const uint64_t ipnBase=st.igtkIPN;
    assert(h.runUntil([&]{return st.maintenance.valid && !st.maintenance.blocked;},300)); // the AP parameters applied after the port opened
    // Unprotected deauth reason 6/7: SA Query instead of tearing down.
    d.deliver(2,mgmtEvent(st,0xc0,0,{7,0}));d.drainWMIRuntime();
    assert(st.saQuery.active && st.portAuthorized && st.stage==LinkStage::Connected && st.pmfDropped==1);
    assert(h.runUntil([&]{return !h.saQueryTx.empty();},200)); // leaves power save first
    assert(h.saQueryTx.size()==1 && h.lastHttProtected);
    auto &req=h.saQueryTx[0];assert(req.size()==44 && req[0]==0xd0 && req[1]==0x40 && req[27]==0x20 && req[24]==1 && req[32]==8 && req[33]==0);
    const UInt16 tid=UInt16(req[34]|(req[35]<<8));
    d.deliver(2,mgmtEvent(st,0xd0,0x40,protectedAction(1,tid)));d.drainWMIRuntime();
    assert(!st.saQuery.active && st.saQueryCompleted==1);
    h.steps(3); // the request's TX completion
    // AP asks: a protected response with the next PN.
    d.deliver(2,mgmtEvent(st,0xd0,0x40,protectedAction(0,0x1234,2)));d.drainWMIRuntime();
    assert(h.runUntil([&]{return h.saQueryTx.size()==2;},100));
    assert(h.saQueryTx.size()==2 && h.saQueryTx[1].size()==44 && h.saQueryTx[1][33]==1 && h.saQueryTx[1][34]==0x34 && h.saQueryTx[1][35]==0x12 && h.saQueryTx[1][24]==2);
    // Unprotected robust action: dropped.
    const UInt32 dropped=st.pmfDropped;d.deliver(2,mgmtEvent(st,0xd0,0,{8,0,1,2}));d.drainWMIRuntime();assert(st.pmfDropped==dropped+1);
    // BIP-CMAC: broadcast deauth with a valid MMIE is honoured, a bad or replayed one is not.
    const UInt8 bcast[6]={255,255,255,255,255,255};
    auto bip=[&](bool valid,uint64_t ipn){
        std::vector<UInt8> body={3,0,76,16,4,0};for(unsigned i=0;i<6;++i)body.push_back(UInt8(ipn>>(8*i)));body.resize(body.size()+8,0);
        auto p=mgmtEvent(st,0xc0,0,body,bcast);
        uint8_t *f=p.data()+60;uint8_t mic[8];const UInt32 n=24+UInt32(body.size());
        assert(AthAES::bip(st.igtk,f,n,mic));if(!valid)mic[0]^=1;memcpy(f+n-8,mic,8);
        return p;
    };
    d.deliver(2,bip(false,ipnBase+5));d.drainWMIRuntime();assert(st.stage==LinkStage::Connected && st.bipFailures==1);
    d.deliver(2,bip(true,ipnBase));d.drainWMIRuntime();assert(st.stage==LinkStage::Connected && st.bipFailures==2); // replay
    d.deliver(2,bip(true,ipnBase+9));d.drainWMIRuntime();assert(st.diagnostics.mlmeCount==1 && !strcmp(st.error,"ap-disconnected"));
    // SA Query without an answer: the link goes down after 1000 TU.
    puts("PMF: SA Query both ways, protected TX, BIP-CMAC, robust drop: OK");
}
static void saQueryTimeout(){
    ConnectionDriver d;Harness h(d);h.tokenRequired=false;h.configureRSN();h.ready();
    assert(h.s.startJoin(h.request(W::kAuthWPA3SAE,W::kCipherPWD,kPassword,sizeof(kPassword)-1),h.t)==B::kSuccess);
    assert(h.runUntil([&]{return h.saeDone;}));h.fourWay(h.apPMK,nullptr,false);assert(h.runUntil([&]{return h.s.linkUp();}));
    auto &st=d._connection;
    assert(h.runUntil([&]{return st.maintenance.valid && !st.maintenance.blocked;},300));
    d.deliver(2,mgmtEvent(st,0xc0,0,{6,0}));d.drainWMIRuntime();assert(st.saQuery.active);
    assert(h.runUntil([&]{return !h.s.linkUp();},600));
    assert(st.saQueryTimeouts==1 && h.saQueryTx.size()>=3 && h.saQueryTx.size()<=5 && !strcmp(st.error,"sa-query-timeout"));
    assert(h.sink.count(W::Deauth)==1);
    puts("SA Query timeout tears the link down: OK");
}
static void enterprise(){
    ConnectionDriver d;Harness h(d);h.akm=1;h.mfpc=false;h.mfpr=false;h.tokenRequired=false;h.configureRSN();h.ready();
    // EAP from eapolclient: the MSK's first 32 bytes are the PMK.
    uint8_t msk[64];for(unsigned i=0;i<64;++i)msk[i]=uint8_t(0x11*(i+1));
    assert(h.s.startJoin(h.request(1u<<2,0,nullptr,0),h.t)==B::kSuccess);
    const uint8_t zero[6]{};
    assert(h.s.setCipherKey(9,msk,64,zero,h.t)==B::kNotReady); // before association
    assert(h.runUntil([&]{return d._connection.stage==LinkStage::Connected && h.s.associated();}));
    assert(h.openAuths==1 && d._connection.enterprise && !d._connection.portAuthorized);
    // The data link is up for EAP but the join is not complete.
    assert(!h.sink.links.empty() && h.sink.links.back() && !h.s.linkUp() && h.s.joining());
    assert(h.sink.count(W::ConnectComplete)==0);
    h.auth.setKey(msk,d._connection.bssid,d._connection.local,1,nullptr);
    h.pushEAPOL(h.auth.m1());h.steps(3);
    assert(d._connection.eapolCount==1 && h.eapolTx.empty()); // held until the PMK exists
    assert(h.s.setCipherKey(9,msk,64,zero,h.t)==B::kSuccess);
    assert(h.runUntil([&]{return h.eapolTx.size()>=1;}));
    assert(h.auth.takeM2(h.eapolTx[0].data(),h.eapolTx[0].size()));
    h.pushEAPOL(h.auth.m3(false));
    assert(h.runUntil([&]{return h.eapolTx.size()>=2;}));
    assert(h.auth.checkM4(h.eapolTx[1].data(),h.eapolTx[1].size()));
    assert(h.runUntil([&]{return h.s.linkUp();}));
    assert(h.sink.count(W::ConnectComplete)==1 && h.s.pmksaCount()==1 && d._connection.portAuthorized);
    // Rejoin: the PMKSA is offered by PMKID, EAP skipped, handshake from the cached PMK.
    uint8_t pmkid[16],data[20];memcpy(data,"PMK Name",8);memcpy(data+8,d._connection.bssid,6);memcpy(data+14,d._connection.local,6);
    uint8_t digest[20];unsigned len=0;assert(HMAC(EVP_sha1(),msk,32,data,20,digest,&len));memcpy(pmkid,digest,16);
    h.s.leaveNetwork(h.t);assert(h.runUntil([&]{return h.s.state()==Session::State::Ready;}));
    h.eapolTx.clear();h.assocHasPMKID=false;
    assert(h.s.startJoin(h.request(1u<<2,0,nullptr,0),h.t)==B::kSuccess);
    h.fourWay(msk,pmkid,false);
    assert(h.assocHasPMKID && !memcmp(h.assocPMKID.data(),pmkid,16));
    assert(h.runUntil([&]{return h.s.linkUp();}));
    puts("802.1X: data link for EAP, PMK/MSK handover, 4-way, PMKSA rejoin: OK");
}
int main(){
    setvbuf(stdout,nullptr,_IONBF,0);
    saeHuntingAndPecking();saeHashToElement();saeFailures();pmksaCaching();transitionPrefersSAE();plainWPA2();pmf();saQueryTimeout();enterprise();
    puts("WPA3/PMF/802.1X end-to-end tests passed.");
    return 0;
}
