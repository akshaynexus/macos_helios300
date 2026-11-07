// SPDX-License-Identifier: GPL-3.0-or-later
// Ath10kSAE.hpp — Simultaneous Authentication of Equals (IEEE 802.11-2020
// 12.4, "Dragonfly") for the station side, group 19 (NIST P-256), with both
// PWE derivations: hunting-and-pecking (12.4.4.2.2) and hash-to-element
// (12.4.4.3.2, SSWU of RFC 9380 as 802.11 specifies it), anti-clogging
// tokens (12.4.6, raw for H&P and the Anti-Clogging Token Container element
// for H2E), password identifiers, the Commit/Confirm messages (12.4.7),
// KCK/PMK/PMKID derivation (12.4.5.4) and the reflection check.
//
// The structure and the constant-time choices follow hostapd/wpa_supplicant
// src/common/sae.c and dragonfly.c (Jouni Malinen and contributors, BSD
// licence; see kexts/wifi/UPSTREAM.md "hostap"). This is a fresh
// implementation on Ath10kP256/Ath10kSHA256 for the kernel: no libc, no
// allocation, fixed-size state wiped on reset. Vectors: IEEE 802.11-2020
// Annex J.10 as carried in hostap's common_module_tests.c
// (tests/test_wpa3_crypto.cpp).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "Ath10kSHA256.hpp"
#include "Ath10kP256.hpp"
namespace AthSAE {
constexpr uint16_t kGroup=19;
constexpr uint16_t kStatusSuccess=0,kStatusAntiCloggingTokenRequired=76,kStatusUnsupportedGroup=77,kStatusH2E=126;
constexpr uint8_t kExtensionElement=255,kExtPasswordIdentifier=33,kExtRejectedGroups=92,kExtAntiCloggingToken=93;
constexpr size_t kMaxPassword=64,kMaxIdentifier=32,kMaxToken=256,kPrimeLen=32;
inline void wipe(void *p,size_t n){volatile uint8_t *b=(volatile uint8_t *)p;while(n--)*b++=0;}
inline bool ctEqual(const uint8_t *a,const uint8_t *b,size_t n){uint8_t d=0;for(size_t i=0;i<n;++i)d|=a[i]^b[i];return d==0;}
inline uint8_t ctMask(bool b){return b?0xff:0;}
inline void ctSelect(uint8_t *out,const uint8_t *a,const uint8_t *b,size_t n,uint8_t m){for(size_t i=0;i<n;++i)out[i]=uint8_t((a[i]&~m)|(b[i]&m));} // m ? b : a
inline uint16_t rd16(const uint8_t *p){return uint16_t(p[0]|(uint16_t(p[1])<<8));}
inline void wr16(uint8_t *p,uint16_t v){p[0]=uint8_t(v);p[1]=uint8_t(v>>8);}

template<bool (*Random)(uint8_t *,size_t)> class Session {
public:
    enum class State : uint8_t { Idle, Committed, Confirmed, Accepted, Failed };
    const char *error="none";
    uint8_t pmk[32]{},pmkid[16]{},kck[32]{};
    State state() const {return state_;}
    bool h2e() const {return useH2E;}
    uint16_t authStatus() const {return useH2E?kStatusH2E:kStatusSuccess;} // status code of our Commit
    bool tokenPending() const {return tokenLength!=0;}
    ~Session(){reset();}
    void reset(){AthSAE::wipe(this,sizeof(*this));error="none";curve=AthP256::Curve();}
    // Password identifier and SSID are optional (SSID is needed for H2E only).
    bool begin(const uint8_t *localMAC,const uint8_t *peerMAC,const uint8_t *password,size_t passwordLength,
               const uint8_t *identifier,size_t identifierLength,const uint8_t *ssid,size_t ssidLength,bool hashToElement){
        reset();
        if(!localMAC || !peerMAC || !password || !passwordLength || passwordLength>kMaxPassword || identifierLength>kMaxIdentifier || ssidLength>32 ||
           (identifierLength && !identifier) || (hashToElement && (!ssid || !ssidLength)) || (identifierLength && !hashToElement && false))return false;
        for(unsigned i=0;i<6;++i){local[i]=localMAC[i];peer[i]=peerMAC[i];}
        for(size_t i=0;i<passwordLength;++i){pw[i]=password[i];}pwLength=passwordLength;
        for(size_t i=0;i<identifierLength;++i){pwid[i]=identifier[i];}pwidLength=identifierLength;
        for(size_t i=0;i<ssidLength;++i){this->ssid[i]=ssid[i];}this->ssidLength=ssidLength;
        useH2E=hashToElement;
        if(!(useH2E?derivePWEH2E():derivePWEHuntingPecking()))return fail("sae-pwe-derivation");
        if(!chooseScalars())return fail("sae-random");
        state_=State::Idle;return true;
    }
    // Tests: fixed rand/mask (Annex J.10) instead of the random source.
    bool setDeterministic(const uint8_t *rand32,const uint8_t *mask32){
        AthP256::fromBytes(rand,rand32);AthP256::fromBytes(mask,mask32);return computeCommit();
    }
    // Commit message body (after Authentication algorithm/sequence/status).
    size_t buildCommit(uint8_t *out,size_t capacity){
        if(state_==State::Failed)return 0;
        size_t n=2+kPrimeLen*3+(pwidLength?3+pwidLength:0)+(tokenLength?(useH2E?3+tokenLength:tokenLength):0);
        if(!out || capacity<n)return 0;
        size_t at=0;wr16(out,kGroup);at=2;
        if(tokenLength && !useH2E){for(size_t i=0;i<tokenLength;++i)out[at+i]=token[i];at+=tokenLength;}
        for(unsigned i=0;i<32;++i){out[at+i]=ownScalarBytes[i];}at+=32;
        for(unsigned i=0;i<64;++i){out[at+i]=ownElementBytes[i];}at+=64;
        if(pwidLength){out[at++]=kExtensionElement;out[at++]=uint8_t(1+pwidLength);out[at++]=kExtPasswordIdentifier;for(size_t i=0;i<pwidLength;++i)out[at+i]=pwid[i];at+=pwidLength;}
        if(tokenLength && useH2E){out[at++]=kExtensionElement;out[at++]=uint8_t(1+tokenLength);out[at++]=kExtAntiCloggingToken;for(size_t i=0;i<tokenLength;++i)out[at+i]=token[i];at+=tokenLength;}
        state_=State::Committed;sendConfirm=0;return at;
    }
    // The AP's Authentication frame with sequence 1. Returns true when the
    // commit was accepted (keys derived) or a token was stored (tokenPending()).
    bool receiveCommit(const uint8_t *body,size_t length,uint16_t status){
        if(state_!=State::Committed || !body || length<2)return fail("sae-commit-unexpected");
        if(rd16(body)!=kGroup)return fail(status==kStatusUnsupportedGroup?"sae-group-rejected":"sae-group-mismatch");
        if(status==kStatusAntiCloggingTokenRequired){
            const uint8_t *t=body+2;size_t n=length-2;
            if(useH2E){ // Anti-Clogging Token Container element
                if(n<3 || t[0]!=kExtensionElement || t[2]!=kExtAntiCloggingToken || size_t(t[1])+2!=n)return fail("sae-token-container");
                t+=3;n-=3;
            }
            if(!n || n>kMaxToken)return fail("sae-token-length");
            for(size_t i=0;i<n;++i){token[i]=t[i];}tokenLength=n;
            state_=State::Idle;return true; // caller sends a new Commit with the token
        }
        if(status!=authStatus())return fail(status?"sae-commit-rejected":"sae-pwe-method-mismatch");
        if(length<2+kPrimeLen*3)return fail("sae-commit-short");
        size_t at=2;
        AthP256::U256 ps;AthP256::fromBytes(ps,body+at);
        // 1 < peer-scalar < n
        AthP256::U256 one{{1,0,0,0}};
        if(AthP256::isZero(ps) || AthP256::equal(ps,one) || !AthP256::less(ps,AthP256::kN))return fail("sae-peer-scalar");
        at+=32;
        AthP256::U256 ex,ey;AthP256::fromBytes(ex,body+at);AthP256::fromBytes(ey,body+at+32);
        if(!curve.fromAffine(peerElement,ex,ey))return fail("sae-peer-element");
        for(unsigned i=0;i<64;++i)peerElementBytes[i]=body[at+i];
        at+=64;
        // Trailing elements: password identifier (must match ours), rejected groups, token container.
        while(at<length){
            if(length-at<2 || size_t(body[at+1])+2>length-at)return fail("sae-commit-elements");
            const uint8_t id=body[at],len=body[at+1];const uint8_t *v=body+at+2;
            if(id==kExtensionElement && len>=1 && v[0]==kExtPasswordIdentifier){
                if(size_t(len)-1!=pwidLength || !ctEqual(v+1,pwid,pwidLength))return fail("sae-password-identifier");
            }
            at+=2+len;
        }
        AthP256::toBytes(ps,peerScalarBytes);peerScalar=ps;
        // Reflection: the peer echoing our own commit would let it derive our keys.
        if(ctEqual(peerScalarBytes,ownScalarBytes,32) && ctEqual(peerElementBytes,ownElementBytes,64))return fail("sae-reflection");
        if(!deriveKeys())return fail("sae-k-infinity");
        state_=State::Confirmed;return true;
    }
    // Confirm message body: Send-Confirm (LE16) || confirm (32 bytes).
    size_t buildConfirm(uint8_t *out,size_t capacity){
        if(state_!=State::Confirmed && state_!=State::Accepted)return 0;
        if(!out || capacity<34)return 0;
        if(sendConfirm<0xffff)++sendConfirm;
        wr16(out,sendConfirm);
        AthSHA256::HMAC h(kck,32);h.update(out,2);h.update(ownScalarBytes,32);h.update(ownElementBytes,64);h.update(peerScalarBytes,32);h.update(peerElementBytes,64);h.final(out+2);
        return 34;
    }
    bool receiveConfirm(const uint8_t *body,size_t length){
        if(state_!=State::Confirmed || !body || length<34)return fail("sae-confirm-unexpected");
        uint8_t verifier[32];
        AthSHA256::HMAC h(kck,32);h.update(body,2);h.update(peerScalarBytes,32);h.update(peerElementBytes,64);h.update(ownScalarBytes,32);h.update(ownElementBytes,64);h.final(verifier);
        const bool ok=ctEqual(verifier,body+2,32);AthSAE::wipe(verifier,32);
        if(!ok)return fail("sae-confirm-mismatch");
        state_=State::Accepted;return true;
    }
    // Exposed for the tests.
    const uint8_t *ownCommitScalar() const {return ownScalarBytes;}
    bool pweBytes(uint8_t *xy64) const {AthP256::U256 x,y;if(curve.isInfinity(pwe))return false;curve.toAffine(pwe,x,y);AthP256::toBytes(x,xy64);AthP256::toBytes(y,xy64+32);return true;}
private:
    AthP256::Curve curve;
    State state_=State::Idle;bool useH2E=false;
    uint8_t local[6]{},peer[6]{},pw[kMaxPassword]{},pwid[kMaxIdentifier]{},ssid[32]{},token[kMaxToken]{};
    size_t pwLength=0,pwidLength=0,ssidLength=0,tokenLength=0;
    AthP256::Point pwe{},peerElement{};
    AthP256::U256 rand{},mask{},ownScalar{},peerScalar{};
    uint8_t ownScalarBytes[32]{},ownElementBytes[64]{},peerScalarBytes[32]{},peerElementBytes[64]{};
    uint16_t sendConfirm=0;
    bool fail(const char *why){error=why;state_=State::Failed;return false;}
    void maxMin(uint8_t *out12) const {
        bool localHigher=false;for(unsigned i=0;i<6;++i){if(local[i]!=peer[i]){localHigher=local[i]>peer[i];break;}}
        for(unsigned i=0;i<6;++i){out12[i]=localHigher?local[i]:peer[i];out12[6+i]=localHigher?peer[i]:local[i];}
    }
    // 12.4.4.2.2: pwd-seed = H(MAX||MIN, password || counter); pwd-value =
    // KDF-256(pwd-seed, "SAE Hunting and Pecking", p); first counter whose
    // pwd-value < p and x^3 - 3x + b is a quadratic residue wins. At least 40
    // iterations run regardless, with a stub password once found.
    bool derivePWEHuntingPecking(){
        uint8_t addrs[12];maxMin(addrs);
        uint8_t prime[32];AthP256::toBytes(AthP256::kP,prime);
        uint8_t stub[kMaxPassword],base[kMaxPassword],seed[32],value[32],xFound[32]{};
        if(!Random(stub,pwLength))return false;
        uint8_t found=0,seedOdd=0;
        for(unsigned counter=1;counter<=40 || !found;++counter){
            if(counter>200)return false;
            ctSelect(base,pw,stub,pwLength,found);
            const uint8_t c=uint8_t(counter);
            AthSHA256::HMAC h(addrs,12);h.update(base,pwLength);h.update(&c,1);h.final(seed);
            if(!AthSHA256::kdf(seed,32,"SAE Hunting and Pecking",prime,32,value,256))return false;
            AthP256::U256 x,xm,r;AthP256::fromBytes(x,value);
            const bool inRange=AthP256::less(x,AthP256::kP);
            curve.fp.toMont(xm,x);curve.rhs(r,xm);
            const bool qr=!AthP256::isZero(r) && curve.isQR(r);
            const uint8_t hit=uint8_t(ctMask(inRange && qr)&~found);
            ctSelect(xFound,xFound,value,32,hit);
            seedOdd=uint8_t((seedOdd&~hit)|((seed[31]&1)&hit));
            found|=hit;
        }
        AthSAE::wipe(stub,sizeof(stub));AthSAE::wipe(base,sizeof(base));AthSAE::wipe(seed,32);AthSAE::wipe(value,32);
        AthP256::U256 x,xm,r,y,yNeg;AthP256::fromBytes(x,xFound);AthSAE::wipe(xFound,32);
        curve.fp.toMont(xm,x);curve.rhs(r,xm);
        if(!curve.sqrt(y,r))return false;
        curve.fp.subMod(yNeg,AthP256::zero(),y);
        uint8_t yb[32],ynb[32],sel[32];AthP256::U256 yn,ynn;curve.fp.fromMont(yn,y);curve.fp.fromMont(ynn,yNeg);
        AthP256::toBytes(yn,yb);AthP256::toBytes(ynn,ynb);
        ctSelect(sel,ynb,yb,32,ctMask((yb[31]&1)==seedOdd)); // LSB(pwd-seed) == LSB(y) ? y : p - y
        AthP256::U256 ys;AthP256::fromBytes(ys,sel);
        const bool ok=curve.fromAffine(pwe,x,ys);
        AthSAE::wipe(yb,32);AthSAE::wipe(ynb,32);AthSAE::wipe(sel,32);return ok;
    }
    // 12.4.4.3.2: PT = SSWU(u1) + SSWU(u2), u = HKDF-Expand(HKDF-Extract(ssid,
    // password [|| identifier]), "SAE Hash to Element uN PN", 48) mod p;
    // PWE = val * PT with val = H(0^32, MAX||MIN) mod (n - 1) + 1.
    bool derivePWEH2E(){
        uint8_t prk[32],value[48];
        if(!AthSHA256::hkdfExtract(ssid,ssidLength,pw,pwLength,pwid,pwidLength,nullptr,0,prk))return false;
        AthP256::Point p1,p2,pt;AthP256::U256 u;
        if(!AthSHA256::hkdfExpand(prk,32,"SAE Hash to Element u1 P1",value,48))return false;
        curve.fp.reduceWide(u,value);if(!sswu(p1,u))return false;
        if(!AthSHA256::hkdfExpand(prk,32,"SAE Hash to Element u2 P2",value,48))return false;
        curve.fp.reduceWide(u,value);if(!sswu(p2,u))return false;
        AthSAE::wipe(prk,32);AthSAE::wipe(value,48);
        curve.addPoints(pt,p1,p2);
        if(curve.isInfinity(pt))return false;
        uint8_t addrs[12],hash[32];maxMin(addrs);
        static const uint8_t zeros[32]{};
        if(!AthSHA256::hkdfExtract(zeros,32,addrs,12,nullptr,0,nullptr,0,hash))return false;
        AthP256::U256 val,v;AthP256::fromBytes(v,hash);curve.scalarOneToNMinus1(val,v);
        curve.mul(pwe,pt,val);
        return !curve.isInfinity(pwe);
    }
    // Simplified SWU as hostapd's sswu() computes it for z = -10 (group 19).
    bool sswu(AthP256::Point &out,const AthP256::U256 &uNormal){
        const auto &f=curve.fp;AthP256::U256 u,u2,t1,t2,z,t,x1a,x1b,x1,x2,gx1,gx2,v,x,y,a,one,ten;
        f.toMont(u,uNormal);
        AthP256::U256 tenN{{10,0,0,0}},oneN{{1,0,0,0}},threeN{{3,0,0,0}};
        f.toMont(ten,tenN);f.subMod(z,AthP256::zero(),ten); // z = -10
        f.toMont(one,oneN);AthP256::U256 three;f.toMont(three,threeN);f.subMod(a,AthP256::zero(),three); // a = -3
        f.sqr(u2,u);f.mul(t1,z,u2);f.sqr(t2,t1);f.addMod(t1,t1,t2); // m = z u^2 + z^2 u^4
        const uint64_t mZero=AthP256::mask64(AthP256::isZero(t1));
        curve.inv(t,t1); // t = m^(p-2): inverse, or 0 when m == 0
        // x1a = b / (z a); x1b = (-b / a) (1 + t)
        f.mul(t1,z,a);curve.inv(t1,t1);f.mul(x1a,curve.b,t1);
        f.subMod(t1,AthP256::zero(),curve.b);curve.inv(t2,a);f.mul(t1,t1,t2);f.addMod(t2,one,t);f.mul(x1b,t1,t2);
        AthP256::select(x1,x1b,x1a,mZero);
        curve.rhs(gx1,x1);
        f.mul(t1,z,u2);f.mul(x2,t1,x1);
        curve.rhs(gx2,x2);
        const uint64_t qr=AthP256::mask64(AthP256::isZero(gx1) || curve.isQR(gx1));
        AthP256::select(v,gx2,gx1,qr);AthP256::select(x,x2,x1,qr);
        if(!curve.sqrt(y,v))return false;
        AthP256::U256 yn,yNeg,ynn;f.fromMont(yn,y);f.subMod(yNeg,AthP256::zero(),y);f.fromMont(ynn,yNeg);
        const uint64_t same=AthP256::mask64((uNormal.v[0]&1)==(yn.v[0]&1));
        AthP256::U256 ySel;AthP256::select(ySel,ynn,yn,same);
        AthP256::U256 xn;f.fromMont(xn,x);
        return curve.fromAffine(out,xn,ySel);
    }
    bool chooseScalars(){
        for(unsigned attempt=0;attempt<16;++attempt){
            uint8_t r[32],m[32];
            if(!Random(r,32) || !Random(m,32))return false;
            AthP256::U256 rr,mm;AthP256::fromBytes(rr,r);AthP256::fromBytes(mm,m);AthSAE::wipe(r,32);AthSAE::wipe(m,32);
            curve.scalarFromRandom(rand,rr);curve.scalarFromRandom(mask,mm);
            if(computeCommit())return true;
        }
        return false;
    }
    // commit-scalar = (rand + mask) mod n (must exceed 1), COMMIT-ELEMENT = -(mask * PWE).
    bool computeCommit(){
        curve.addModN(ownScalar,rand,mask);
        AthP256::U256 one{{1,0,0,0}};
        if(AthP256::isZero(ownScalar) || AthP256::equal(ownScalar,one))return false;
        AthP256::Point e;curve.mul(e,pwe,mask);
        if(curve.isInfinity(e))return false;
        curve.negate(e);
        AthP256::U256 x,y;curve.toAffine(e,x,y);
        AthP256::toBytes(ownScalar,ownScalarBytes);AthP256::toBytes(x,ownElementBytes);AthP256::toBytes(y,ownElementBytes+32);
        return true;
    }
    // K = rand * (peer-scalar * PWE + PEER-ELEMENT); keyseed = H(0^32, x(K));
    // KCK || PMK = KDF-512(keyseed, "SAE KCK and PMK", (scalar + peer-scalar) mod n).
    bool deriveKeys(){
        AthP256::Point k;curve.mul(k,pwe,peerScalar);curve.addPoints(k,k,peerElement);curve.mul(k,k,rand);
        if(curve.isInfinity(k))return false;
        AthP256::U256 kx,ky;curve.toAffine(k,kx,ky);
        uint8_t kb[32],keyseed[32],val[32],keys[64];AthP256::toBytes(kx,kb);
        static const uint8_t zeros[32]{};
        AthSHA256::hkdfExtract(zeros,32,kb,32,nullptr,0,nullptr,0,keyseed);
        AthP256::U256 sum;curve.addModN(sum,ownScalar,peerScalar);AthP256::toBytes(sum,val);
        const bool ok=AthSHA256::kdf(keyseed,32,"SAE KCK and PMK",val,32,keys,512);
        for(unsigned i=0;i<32;++i){kck[i]=keys[i];pmk[i]=keys[32+i];}
        for(unsigned i=0;i<16;++i)pmkid[i]=val[i];
        AthSAE::wipe(kb,32);AthSAE::wipe(keyseed,32);AthSAE::wipe(keys,64);AthSAE::wipe(&k,sizeof(k));
        return ok;
    }
};
}
