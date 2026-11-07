// Host test for the WPA3 crypto layer: SHA-256 (FIPS 180-4), HMAC-SHA256
// (RFC 4231), HKDF (RFC 5869), AES-128 (FIPS 197 C.1), AES-CMAC (RFC 4493),
// BIP-CMAC-128 framing, P-256 scalar multiplication (NIST/CAVP points), the
// IEEE 802.11-2020 Annex J.10 SAE vectors (hunting-and-pecking commit, KCK,
// PMK, PMKID; hash-to-element PT/PWE) and full SAE exchanges between two
// stations for both PWE methods.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include "../src/shared/Ath10kSHA256.hpp"
#include "../src/shared/Ath10kAES.hpp"
#include "../src/shared/Ath10kP256.hpp"
#include "../src/shared/Ath10kSAE.hpp"
static uint64_t lcg=0x9e3779b97f4a7c15ull;
static bool testRandom(uint8_t *p,size_t n){for(size_t i=0;i<n;++i){lcg=lcg*6364136223846793005ull+1442695040888963407ull;p[i]=uint8_t(lcg>>33);}return true;}
static void hex(const char *s,uint8_t *out,size_t n){for(size_t i=0;i<n;++i){unsigned v;sscanf(s+2*i,"%2x",&v);out[i]=uint8_t(v);}}
static bool eqhex(const uint8_t *a,const char *h,size_t n){uint8_t b[512];hex(h,b,n);return memcmp(a,b,n)==0;}
static void sha256Tests(){
    uint8_t d[32];AthSHA256::sha256((const uint8_t *)"abc",3,d);
    assert(eqhex(d,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",32));
    AthSHA256::sha256((const uint8_t *)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",56,d);
    assert(eqhex(d,"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",32));
    uint8_t m[1000];memset(m,'a',1000);AthSHA256::SHA256 s;for(unsigned i=0;i<1000;++i)s.update(m,1000);s.final(d);
    assert(eqhex(d,"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",32));
    // RFC 4231 cases 2 and 7 (key longer than the block).
    AthSHA256::hmac((const uint8_t *)"Jefe",4,(const uint8_t *)"what do ya want for nothing?",28,d);
    assert(eqhex(d,"5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",32));
    uint8_t key[131];memset(key,0xaa,131);
    const char *data="This is a test using a larger than block-size key and a larger than block-size data. The key needs to be hashed before being used by the HMAC algorithm.";
    AthSHA256::hmac(key,131,(const uint8_t *)data,strlen(data),d);
    assert(eqhex(d,"9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2",32));
    // RFC 5869 test case 1.
    uint8_t ikm[22],salt[13],info[10],prk[32],okm[42];memset(ikm,0x0b,22);for(unsigned i=0;i<13;++i)salt[i]=uint8_t(i);for(unsigned i=0;i<10;++i)info[i]=uint8_t(0xf0+i);
    assert(AthSHA256::hkdfExtract(salt,13,ikm,22,nullptr,0,nullptr,0,prk));
    assert(eqhex(prk,"077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5",32));
    char infoStr[11];memcpy(infoStr,info,10);infoStr[10]=0; // info bytes are non-zero
    assert(AthSHA256::hkdfExpand(prk,32,infoStr,okm,42));
    assert(eqhex(okm,"3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865",42));
    // KDF-SHA256 structure: the first block equals HMAC(K, 0x0001 || label || context || length).
    uint8_t out[40],ref[32],in[2+5+3+2]={1,0,'l','a','b','e','l',9,8,7,uint8_t(320),uint8_t(320>>8)};
    assert(AthSHA256::kdf(key,16,"label",in+7,3,out,320));
    AthSHA256::hmac(key,16,in,sizeof(in),ref);assert(!memcmp(out,ref,32));
    in[0]=2;AthSHA256::hmac(key,16,in,sizeof(in),ref);assert(!memcmp(out+32,ref,8));
    puts("SHA-256/HMAC/HKDF/KDF vectors OK");
}
static void aesTests(){
    uint8_t key[16],pt[16],ct[16],back[16];
    hex("000102030405060708090a0b0c0d0e0f",key,16);hex("00112233445566778899aabbccddeeff",pt,16);
    assert(AthAES::encrypt(key,pt,ct) && eqhex(ct,"69c4e0d86a7b0430d8cdb78070b4c55a",16));
    assert(AthAES::decrypt(key,ct,back) && !memcmp(back,pt,16));
    uint8_t k[16],m[64],mac[16];hex("2b7e151628aed2a6abf7158809cf4f3c",k,16);
    hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710",m,64);
    assert(AthAES::cmac(k,nullptr,0,nullptr,0,nullptr,0,mac) && eqhex(mac,"bb1d6929e95937287fa37d129b756746",16));
    assert(AthAES::cmac(k,m,16,nullptr,0,nullptr,0,mac) && eqhex(mac,"070a16b46b4d4144f79bdd9dd04a287c",16));
    assert(AthAES::cmac(k,m,10,m+10,20,m+30,10,mac) && eqhex(mac,"dfa66747de9ae63030ca32611497c827",16));
    assert(AthAES::cmac(k,m,64,nullptr,0,nullptr,0,mac) && eqhex(mac,"51f0bebf7e3b9d92fc49741779363cfe",16));
    assert(AthAES::cmac(k,m,32,m+32,32,nullptr,0,mac) && eqhex(mac,"51f0bebf7e3b9d92fc49741779363cfe",16));
    // BIP: broadcast deauth with MMIE; MIC = CMAC(IGTK, FC(masked)||A1||A2||A3||body with zero MIC).
    uint8_t frame[24+2+18]={0xc0,0x38,0,0, 0xff,0xff,0xff,0xff,0xff,0xff, 2,6,7,8,9,10, 2,6,7,8,9,10, 0x10,0, 7,0, 76,16, 4,0, 1,0,0,0,0,0};
    uint8_t mic[8],manual[16],buf[20+2+18];
    assert(AthAES::bip(k,frame,sizeof(frame),mic));
    buf[0]=0xc0;buf[1]=0;memcpy(buf+2,frame+4,18);memcpy(buf+20,frame+24,20);
    assert(AthAES::cmac(k,buf,sizeof(buf),nullptr,0,nullptr,0,manual) && !memcmp(mic,manual,8));
    frame[1]=0; // Retry/PwrMgt/MoreData do not change the MIC
    uint8_t mic2[8];assert(AthAES::bip(k,frame,sizeof(frame),mic2) && !memcmp(mic,mic2,8));
    frame[24]^=1;assert(AthAES::bip(k,frame,sizeof(frame),mic2) && memcmp(mic,mic2,8));
    puts("AES-128/CMAC/BIP vectors OK");
}
static void p256Tests(){
    AthP256::Curve c;AthP256::Point g,r;
    assert(c.fromAffine(g,AthP256::kGx,AthP256::kGy));
    auto check=[&](const char *k,const char *x,const char *y){
        uint8_t kb[32],xb[32],yb[32];hex(k,kb,32);AthP256::U256 kk,rx,ry;AthP256::fromBytes(kk,kb);
        c.mul(r,g,kk);assert(!c.isInfinity(r));c.toAffine(r,rx,ry);AthP256::toBytes(rx,xb);AthP256::toBytes(ry,yb);
        if(!(eqhex(xb,x,32) && eqhex(yb,y,32))){printf("k=%s\nx=",k);for(int i=0;i<32;++i)printf("%02x",xb[i]);printf("\ny=");for(int i=0;i<32;++i)printf("%02x",yb[i]);puts("");}
        assert(eqhex(xb,x,32) && eqhex(yb,y,32));
    };
    check("0000000000000000000000000000000000000000000000000000000000000001","6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296","4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5");
    check("0000000000000000000000000000000000000000000000000000000000000002","7cf27b188d034f7e8a52380304b51ac3c08969e277f21b35a60b48fc47669978","07775510db8ed040293d9ac69f7430dbba7dade63ce982299e04b79d227873d1");
    check("0000000000000000000000000000000000000000000000000000000000000003","5ecbe4d1a6330a44c8f7ef951d4bf165e6c6b721efada985fb41661bc6e7fd6c","8734640c4998ff7e374b06ce1a64a2ecd82ab036384fb83d9a79b127a27d5032");
    check("0000000000000000000000000000000000000000000000000000000000000004","e2534a3532d08fbba02dde659ee62bd0031fe2db785596ef509302446b030852","e0f1575a4c633cc719dfee5fda862d764efc96c3f30ee0055c42c23f184ed8c6");
    // k = 112233445566778899, cross-checked with an independent affine implementation (Python).
    check("0000000000000000000000000000000000000000000000112233445566778899","a418fae26a95714223ab4c076b77517b562f85ea77e5cf9d6152b309bf657a8d","d47069fb5b516512f0902c0d7a0260fc90347210a0635a751983a985e7360950");
    // k = n - 1 is -G; k = n is the identity.
    check("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632550","6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296","b01cbd1c01e58065711814b583f061e9d431cca994cea1313449bf97c840ae0a");
    c.mul(r,g,AthP256::kN);assert(c.isInfinity(r));
    // Addition consistency: 2G + 3G == 5G, G + (-G) == infinity, inf + G == G.
    AthP256::Point p2,p3,p5,s;AthP256::U256 two{{2,0,0,0}},three{{3,0,0,0}},five{{5,0,0,0}};
    c.mul(p2,g,two);c.mul(p3,g,three);c.mul(p5,g,five);c.addPoints(s,p2,p3);
    AthP256::U256 ax,ay,bx,by;c.toAffine(s,ax,ay);c.toAffine(p5,bx,by);assert(AthP256::equal(ax,bx) && AthP256::equal(ay,by));
    AthP256::Point ng=g;c.negate(ng);c.addPoints(s,g,ng);assert(c.isInfinity(s));
    AthP256::Point inf;c.infinity(inf);c.addPoints(s,inf,g);c.toAffine(s,ax,ay);assert(AthP256::equal(ax,AthP256::kGx));
    c.addPoints(s,g,g);c.toAffine(s,ax,ay);c.toAffine(p2,bx,by);assert(AthP256::equal(ax,bx) && AthP256::equal(ay,by));
    // Field: inverse, sqrt and Legendre on Gy.
    AthP256::U256 gy,gyInv,one,prod,sq,root;c.fp.toMont(gy,AthP256::kGy);c.inv(gyInv,gy);c.fp.mul(prod,gy,gyInv);assert(AthP256::equal(prod,c.fp.one));
    c.fp.sqr(sq,gy);assert(c.isQR(sq) && c.sqrt(root,sq));c.fp.sqr(prod,root);assert(AthP256::equal(prod,sq));
    (void)one;
    // Off-curve and out-of-range points are rejected.
    AthP256::U256 bad=AthP256::kGy;bad.v[0]^=1;assert(!c.fromAffine(r,AthP256::kGx,bad));
    assert(!c.fromAffine(r,AthP256::kP,AthP256::kGy));
    puts("P-256 vectors OK");
}
static void saeVectorTests(){
    using S=AthSAE::Session<testRandom>;
    const uint8_t addr1[6]={0x4d,0x3f,0x2f,0xff,0xe3,0x87},addr2[6]={0xa5,0xd8,0xaa,0x95,0x8e,0x3c};
    const char *pw="mekmitasdigoat";
    uint8_t rnd[32],msk[32],commit[98],peer[98];
    hex("992465fd3daa3c60aa6565b7f62a2a7f2e12dd12f198faf4fbed89d7ff1ace94",rnd,32);
    hex("9507a90f777a044d6a0830b91ea3d5dd70bece44e1acffb86983b5e1bf9fb322",msk,32);
    hex("13002e2c0f0db52440ad146d967114ce005ce1eab0aa2c2e5c2871b774f6c2575c65d5ad9e00829707aa36ba8b859738fc961d08243505f47c035376d7ac4bc8d7b95083bf43827d0fc31ed778dd3671fd21a46d1091d64b6f9a1e1272621325dbe1",commit,98);
    hex("1300591b96f3397fb945100848e7b550543b6720d88337ee93fc49fd6df7e08b5223e71b9bb048d3873f20556953a96c91536fd8ee6ca9b4a68a148b056a909be03e83ae208f60f8ef5537858074db0668703239986299 9b511e0a1552a5fea317c2",peer,0);
    // (the peer commit string above has a space to keep the line readable; rebuild it)
    hex("1300591b96f3397fb945100848e7b550543b6720d88337ee93fc49fd6df7e08b5223e71b9bb048d3873f20556953a96c91536fd8ee6ca9b4a68a148b056a909be03e83ae208f60f8ef5537858074db06687032399862999b511e0a1552a5fea317c2",peer,98);
    S *s=new S;
    assert(s->begin(addr1,addr2,(const uint8_t *)pw,strlen(pw),nullptr,0,nullptr,0,false));
    assert(s->setDeterministic(rnd,msk));
    uint8_t out[256];size_t n=s->buildCommit(out,sizeof(out));
    assert(n==98 && !memcmp(out,commit,98));
    assert(s->receiveCommit(peer,98,0) && s->state()==S::State::Confirmed);
    assert(eqhex(s->kck,"1e733f6d9bd532562873043388 31b09a39406d121017073a5c30db36f36cb81a",0) || true);
    assert(eqhex(s->kck,"1e733f6d9bd53256287304338831b09a39406d121017073a5c30db36f36cb81a",32));
    assert(eqhex(s->pmk,"4e4dfab1a2dd8ac1a91790f953faaa452ae5c6873ab75b63605ba663f8a7fe59",32));
    assert(eqhex(s->pmkid,"8747a600eea3f9f22475df58ca1e5498",16));
    // Confirm is HMAC(KCK, sc || own || peer): verify against a direct computation.
    n=s->buildConfirm(out,sizeof(out));assert(n==34 && out[0]==1 && out[1]==0);
    uint8_t ref[32];AthSHA256::HMAC h(s->kck,32);h.update(out,2);h.update(commit+2,96);h.update(peer+2,96);h.final(ref);assert(!memcmp(out+2,ref,32));
    // The peer's confirm (sc = 1) would be HMAC(KCK, sc || peer || own).
    uint8_t pc[34]={1,0};AthSHA256::HMAC h2(s->kck,32);h2.update(pc,2);h2.update(peer+2,96);h2.update(commit+2,96);h2.final(pc+2);
    assert(s->receiveConfirm(pc,34) && s->state()==S::State::Accepted);
    pc[5]^=1;S *t=new S;assert(t->begin(addr1,addr2,(const uint8_t *)pw,strlen(pw),nullptr,0,nullptr,0,false) && t->setDeterministic(rnd,msk));
    t->buildCommit(out,sizeof(out));assert(t->receiveCommit(peer,98,0) && !t->receiveConfirm(pc,34) && t->state()==S::State::Failed);
    // Reflection and bad element rejection.
    S *u=new S;assert(u->begin(addr1,addr2,(const uint8_t *)pw,strlen(pw),nullptr,0,nullptr,0,false) && u->setDeterministic(rnd,msk));
    u->buildCommit(out,sizeof(out));assert(!u->receiveCommit(commit,98,0) && !strcmp(u->error,"sae-reflection"));
    S *v=new S;assert(v->begin(addr1,addr2,(const uint8_t *)pw,strlen(pw),nullptr,0,nullptr,0,false) && v->setDeterministic(rnd,msk));
    v->buildCommit(out,sizeof(out));uint8_t badPeer[98];memcpy(badPeer,peer,98);badPeer[40]^=1;assert(!v->receiveCommit(badPeer,98,0) && !strcmp(v->error,"sae-peer-element"));
    // Anti-clogging token: status 76 stores it, the next Commit carries it after the group.
    S *w=new S;assert(w->begin(addr1,addr2,(const uint8_t *)pw,strlen(pw),nullptr,0,nullptr,0,false) && w->setDeterministic(rnd,msk));
    w->buildCommit(out,sizeof(out));uint8_t tokenMsg[2+16]={0x13,0};for(unsigned i=0;i<16;++i)tokenMsg[2+i]=uint8_t(0xa0+i);
    assert(w->receiveCommit(tokenMsg,18,76) && w->tokenPending());
    n=w->buildCommit(out,sizeof(out));assert(n==98+16 && !memcmp(out+2,tokenMsg+2,16) && !memcmp(out+18,commit+2,96));
    assert(w->receiveCommit(peer,98,0) && eqhex(w->pmk,"4e4dfab1a2dd8ac1a91790f953faaa452ae5c6873ab75b63605ba663f8a7fe59",32));
    delete s;delete t;delete u;delete v;delete w;
    // H2E PT/PWE vector: ssid "byteme", password identifier "psk4internet", group 19.
    const uint8_t addr1b[6]={0x00,0x09,0x5b,0x66,0xec,0x1e},addr2b[6]={0x00,0x0b,0x6b,0xd9,0x02,0x46};
    S *x=new S;assert(x->begin(addr1b,addr2b,(const uint8_t *)pw,strlen(pw),(const uint8_t *)"psk4internet",12,(const uint8_t *)"byteme",6,true));
    uint8_t xy[64];assert(x->pweBytes(xy));
    assert(eqhex(xy,"c93049b9e64000f848201649e999f2b5c22dea69b5632c9df4d633b8aa1f6c1e",32));
    assert(eqhex(xy+32,"73634e94b53d82e7383a8d258199d9dc1a5ee8269d060382ccbf33e614ff59a0",32));
    assert(x->authStatus()==126);
    n=x->buildCommit(out,sizeof(out));assert(n==98+3+12 && out[98]==255 && out[99]==13 && out[100]==33 && !memcmp(out+101,"psk4internet",12));
    delete x;
    puts("SAE Annex J.10 vectors OK");
}
static void saeExchange(bool h2e,const uint8_t *id,size_t idLen){
    using S=AthSAE::Session<testRandom>;
    const uint8_t a[6]={2,1,2,3,4,5},b[6]={2,6,7,8,9,10};const char *pw="correct horse battery";
    S *sa=new S,*sb=new S;
    assert(sa->begin(a,b,(const uint8_t *)pw,strlen(pw),id,idLen,(const uint8_t *)"TEST",4,h2e));
    assert(sb->begin(b,a,(const uint8_t *)pw,strlen(pw),id,idLen,(const uint8_t *)"TEST",4,h2e));
    uint8_t ca[256],cb[256],fa[34],fb[34];
    const size_t na=sa->buildCommit(ca,sizeof(ca)),nb=sb->buildCommit(cb,sizeof(cb));
    assert(na==nb && na==98+(idLen?3+idLen:0));
    assert(sa->receiveCommit(cb,nb,sa->authStatus()) && sb->receiveCommit(ca,na,sb->authStatus()));
    assert(!memcmp(sa->pmk,sb->pmk,32) && !memcmp(sa->pmkid,sb->pmkid,16) && !memcmp(sa->kck,sb->kck,32));
    assert(sa->buildConfirm(fa,34)==34 && sb->buildConfirm(fb,34)==34);
    assert(sa->receiveConfirm(fb,34) && sb->receiveConfirm(fa,34));
    assert(sa->state()==S::State::Accepted && sb->state()==S::State::Accepted);
    // A different password on one side: commits parse but confirms fail.
    S *sc=new S;assert(sc->begin(b,a,(const uint8_t *)"wrong password",14,id,idLen,(const uint8_t *)"TEST",4,h2e));
    S *sd=new S;assert(sd->begin(a,b,(const uint8_t *)pw,strlen(pw),id,idLen,(const uint8_t *)"TEST",4,h2e));
    uint8_t cc[256],cd[256];const size_t nc=sc->buildCommit(cc,sizeof(cc)),nd=sd->buildCommit(cd,sizeof(cd));
    assert(sc->receiveCommit(cd,nd,sc->authStatus()) && sd->receiveCommit(cc,nc,sd->authStatus()));
    assert(memcmp(sc->pmk,sd->pmk,32));
    uint8_t fc[34];assert(sc->buildConfirm(fc,34)==34 && !sd->receiveConfirm(fc,34));
    // PWE method mismatch: an H2E commit answered with status 0 is refused.
    S *se=new S;assert(se->begin(a,b,(const uint8_t *)pw,strlen(pw),nullptr,0,(const uint8_t *)"TEST",4,h2e));
    se->buildCommit(ca,sizeof(ca));assert(!se->receiveCommit(cb,nb,h2e?0:126));
    delete sa;delete sb;delete sc;delete sd;delete se;
}
int main(){setvbuf(stdout,nullptr,_IONBF,0);
    sha256Tests();aesTests();p256Tests();saeVectorTests();
    saeExchange(false,nullptr,0);saeExchange(true,nullptr,0);saeExchange(true,(const uint8_t *)"id",2);
    puts("SAE exchanges OK (H&P, H2E, H2E with identifier)");
    return 0;
}
