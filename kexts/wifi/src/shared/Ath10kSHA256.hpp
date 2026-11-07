// SPDX-License-Identifier: GPL-3.0-or-later
// Ath10kSHA256.hpp — SHA-256 (FIPS 180-4), HMAC-SHA256 (RFC 2104), the
// IEEE 802.11 KDF-SHA256 (12.7.1.6.2, "KDF-Hash-Length") and HKDF (RFC 5869)
// in self-contained C++ for the kernel (no libc, no allocation). Used by the
// WPA3 supplicant (SAE, AKM 00-0F-AC:5/6/8 key derivation) and by SAE's
// hash-to-element. Checked on the host against FIPS 180-4, RFC 4231,
// RFC 5869 and hostapd's sha256_prf_bits (tests/test_wpa3_crypto.cpp).
#pragma once
#include <stdint.h>
#include <stddef.h>
namespace AthSHA256 {
inline uint32_t ror(uint32_t v,unsigned n){return (v>>n)|(v<<(32-n));}
inline void wipe(void *p,size_t n){volatile uint8_t *b=(volatile uint8_t *)p;while(n--)*b++=0;}
struct SHA256 {
    static constexpr uint32_t K[64]={
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t h[8]{0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    uint8_t block[64]{};uint64_t length=0;size_t fill=0;
    void compress(const uint8_t *p){
        uint32_t w[64];
        for(unsigned i=0;i<16;++i)w[i]=uint32_t(p[4*i])<<24|uint32_t(p[4*i+1])<<16|uint32_t(p[4*i+2])<<8|p[4*i+3];
        for(unsigned i=16;i<64;++i){
            const uint32_t s0=ror(w[i-15],7)^ror(w[i-15],18)^(w[i-15]>>3),s1=ror(w[i-2],17)^ror(w[i-2],19)^(w[i-2]>>10);
            w[i]=w[i-16]+s0+w[i-7]+s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for(unsigned i=0;i<64;++i){
            const uint32_t S1=ror(e,6)^ror(e,11)^ror(e,25),ch=(e&f)^(~e&g),t1=hh+S1+ch+K[i]+w[i];
            const uint32_t S0=ror(a,2)^ror(a,13)^ror(a,22),maj=(a&b)^(a&c)^(b&c),t2=S0+maj;
            hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
        wipe(w,sizeof(w));
    }
    void update(const uint8_t *p,size_t n){
        length+=n;
        while(n){
            size_t take=64-fill;if(take>n)take=n;
            for(size_t i=0;i<take;++i)block[fill+i]=p[i];
            fill+=take;p+=take;n-=take;
            if(fill==64){compress(block);fill=0;}
        }
    }
    void final(uint8_t *out){
        const uint64_t bits=length*8;
        uint8_t pad=0x80;update(&pad,1);
        uint8_t zero=0;while(fill!=56)update(&zero,1);
        uint8_t len[8];for(unsigned i=0;i<8;++i)len[i]=uint8_t(bits>>(56-8*i));
        update(len,8);
        for(unsigned i=0;i<8;++i){out[4*i]=uint8_t(h[i]>>24);out[4*i+1]=uint8_t(h[i]>>16);out[4*i+2]=uint8_t(h[i]>>8);out[4*i+3]=uint8_t(h[i]);}
        wipe(block,64);
    }
};
inline void sha256(const uint8_t *p,size_t n,uint8_t *out){SHA256 s;s.update(p,n);s.final(out);}
// HMAC over up to four concatenated parts (hostapd's hmac_sha256_vector shape).
struct HMAC {
    SHA256 inner;uint8_t k[64]{};
    explicit HMAC(const uint8_t *key,size_t keyLength){
        if(keyLength>64)sha256(key,keyLength,k);else for(size_t i=0;i<keyLength;++i)k[i]=key[i];
        uint8_t pad[64];for(unsigned i=0;i<64;++i)pad[i]=uint8_t(k[i]^0x36);
        inner.update(pad,64);wipe(pad,64);
    }
    ~HMAC(){wipe(k,64);}
    void update(const uint8_t *p,size_t n){if(n)inner.update(p,n);}
    void final(uint8_t *out){
        uint8_t digest[32],pad[64];inner.final(digest);
        for(unsigned i=0;i<64;++i)pad[i]=uint8_t(k[i]^0x5c);
        SHA256 o;o.update(pad,64);o.update(digest,32);o.final(out);
        wipe(pad,64);wipe(digest,32);
    }
};
inline bool hmac(const uint8_t *key,size_t keyLength,const uint8_t *p,size_t n,uint8_t *out){
    if(!key || (!p && n) || !out)return false;
    HMAC h(key,keyLength);h.update(p,n);h.final(out);return true;
}
inline size_t cstrlen(const char *s){size_t n=0;while(s[n])++n;return n;}
// IEEE 802.11-2020 12.7.1.6.2 KDF-Hash-Length with SHA-256: Hash(K, i || label
// || context || Length), i as LE16 from 1, Length in bits as LE16. No NUL after
// the label (unlike the SHA-1 PRF of 12.7.1.2). outBits must be a multiple of 8.
inline bool kdf(const uint8_t *key,size_t keyLength,const char *label,const uint8_t *context,size_t contextLength,uint8_t *out,size_t outBits){
    if(!key || !label || (!context && contextLength) || !out || !outBits || (outBits&7) || outBits>65535)return false;
    const size_t outBytes=outBits/8;size_t at=0;
    for(uint16_t counter=1;at<outBytes;++counter){
        uint8_t c[2]={uint8_t(counter),uint8_t(counter>>8)},l[2]={uint8_t(outBits),uint8_t(outBits>>8)},digest[32];
        HMAC h(key,keyLength);h.update(c,2);h.update((const uint8_t *)label,cstrlen(label));h.update(context,contextLength);h.update(l,2);h.final(digest);
        const size_t take=outBytes-at<32?outBytes-at:32;
        for(size_t i=0;i<take;++i)out[at+i]=digest[i];
        at+=take;wipe(digest,32);
    }
    return true;
}
// RFC 5869 with SHA-256. Extract takes up to three IKM parts (SAE's
// password || identifier, k, or MAX(addr) || MIN(addr)).
inline bool hkdfExtract(const uint8_t *salt,size_t saltLength,const uint8_t *a,size_t an,const uint8_t *b,size_t bn,const uint8_t *c,size_t cn,uint8_t *prk){
    if(!prk || (!a && an) || (!b && bn) || (!c && cn))return false;
    static const uint8_t zero[32]{};
    HMAC h(salt?salt:zero,salt?saltLength:32);h.update(a,an);h.update(b,bn);h.update(c,cn);h.final(prk);return true;
}
inline bool hkdfExpand(const uint8_t *prk,size_t prkLength,const char *info,uint8_t *out,size_t outLength){
    if(!prk || !info || !out || !outLength || outLength>255*32)return false;
    uint8_t t[32];size_t tn=0,at=0;
    for(uint8_t counter=1;at<outLength;++counter){
        HMAC h(prk,prkLength);h.update(t,tn);h.update((const uint8_t *)info,cstrlen(info));h.update(&counter,1);h.final(t);tn=32;
        const size_t take=outLength-at<32?outLength-at:32;
        for(size_t i=0;i<take;++i)out[at+i]=t[i];
        at+=take;
    }
    wipe(t,32);return true;
}
}
