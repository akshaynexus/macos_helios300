// SPDX-License-Identifier: GPL-3.0-or-later
// AirportAth10kCrypto.hpp — the Crypto policy AthSecurity::Supplicant needs
// (HMAC-SHA1, AES-128 block decrypt for RFC 3394 unwrap, random nonces) in
// self-contained C++ so the WPA2 supplicant can run inside the kernel. The
// kernel exports SHA1 (libkern) but no single-block AES, and the AES exports
// live in com.apple.kpi.unsupported, so everything here is software and is
// checked on the host against the FIPS-180/RFC 2202/FIPS-197 vectors
// (tests/test_native_crypto.cpp). Only the random source differs per build.
#pragma once
#include <stdint.h>
#include <stddef.h>
namespace AirportCrypto {
inline uint32_t rol(uint32_t v,unsigned n){return (v<<n)|(v>>(32-n));}
struct SHA1 {
    uint32_t h[5]{0x67452301u,0xEFCDAB89u,0x98BADCFEu,0x10325476u,0xC3D2E1F0u};
    uint8_t block[64]{};uint64_t length=0;size_t fill=0;
    void compress(const uint8_t *p){
        uint32_t w[80];
        for(unsigned i=0;i<16;++i)w[i]=uint32_t(p[4*i])<<24|uint32_t(p[4*i+1])<<16|uint32_t(p[4*i+2])<<8|p[4*i+3];
        for(unsigned i=16;i<80;++i)w[i]=rol(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
        for(unsigned i=0;i<80;++i){
            uint32_t f,k;
            if(i<20){f=(b&c)|(~b&d);k=0x5A827999u;}
            else if(i<40){f=b^c^d;k=0x6ED9EBA1u;}
            else if(i<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDCu;}
            else {f=b^c^d;k=0xCA62C1D6u;}
            uint32_t t=rol(a,5)+f+e+k+w[i];e=d;d=c;c=rol(b,30);b=a;a=t;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;
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
        for(unsigned i=0;i<5;++i){out[4*i]=uint8_t(h[i]>>24);out[4*i+1]=uint8_t(h[i]>>16);out[4*i+2]=uint8_t(h[i]>>8);out[4*i+3]=uint8_t(h[i]);}
        volatile uint8_t *b=block;for(size_t i=0;i<64;++i)b[i]=0;
    }
};
inline void sha1(const uint8_t *p,size_t n,uint8_t *out){SHA1 s;s.update(p,n);s.final(out);}
inline bool hmacSHA1(const uint8_t *key,size_t keyLength,const uint8_t *p,size_t n,uint8_t *out){
    if(!key || (!p && n) || !out)return false;
    uint8_t k[64]{},inner[20];
    if(keyLength>64)sha1(key,keyLength,k);else for(size_t i=0;i<keyLength;++i)k[i]=key[i];
    uint8_t pad[64];
    for(unsigned i=0;i<64;++i)pad[i]=k[i]^0x36;
    SHA1 s;s.update(pad,64);s.update(p,n);s.final(inner);
    for(unsigned i=0;i<64;++i)pad[i]=k[i]^0x5c;
    SHA1 o;o.update(pad,64);o.update(inner,20);o.final(out);
    volatile uint8_t *w=k;for(unsigned i=0;i<64;++i)w[i]=0;
    w=pad;for(unsigned i=0;i<64;++i)w[i]=0;
    w=inner;for(unsigned i=0;i<20;++i)w[i]=0;
    return true;
}
// AES-128 single-block decrypt (FIPS-197 §5.3, straightforward byte-wise
// implementation; the key unwrap decrypts at most 49 blocks per handshake).
struct AES128 {
    uint8_t sbox[256],inv[256],roundKeys[176];
    static uint8_t xtime(uint8_t x){return uint8_t((x<<1)^((x&0x80)?0x1b:0));}
    static uint8_t mul(uint8_t a,uint8_t b){uint8_t r=0;while(b){if(b&1)r^=a;a=xtime(a);b>>=1;}return r;}
    void buildBoxes(){
        uint8_t p=1,q=1;
        do{ // multiplicative inverse via generator 3, then affine transform
            p=uint8_t(p^(p<<1)^((p&0x80)?0x1b:0));
            q^=uint8_t(q<<1);q^=uint8_t(q<<2);q^=uint8_t(q<<4);if(q&0x80)q^=0x09;
            uint8_t x=uint8_t(q^rol8(q,1)^rol8(q,2)^rol8(q,3)^rol8(q,4));
            sbox[p]=uint8_t(x^0x63);
        }while(p!=1);
        sbox[0]=0x63;
        for(unsigned i=0;i<256;++i)inv[sbox[i]]=uint8_t(i);
    }
    static uint8_t rol8(uint8_t v,unsigned n){return uint8_t((v<<n)|(v>>(8-n)));}
    explicit AES128(const uint8_t *key){
        buildBoxes();
        for(unsigned i=0;i<16;++i)roundKeys[i]=key[i];
        uint8_t rcon=1;
        for(unsigned i=16;i<176;i+=4){
            uint8_t t[4]={roundKeys[i-4],roundKeys[i-3],roundKeys[i-2],roundKeys[i-1]};
            if(i%16==0){
                uint8_t first=t[0];t[0]=uint8_t(sbox[t[1]]^rcon);t[1]=sbox[t[2]];t[2]=sbox[t[3]];t[3]=sbox[first];
                rcon=xtime(rcon);
            }
            for(unsigned j=0;j<4;++j)roundKeys[i+j]=uint8_t(roundKeys[i-16+j]^t[j]);
        }
    }
    ~AES128(){volatile uint8_t *w=roundKeys;for(unsigned i=0;i<176;++i)w[i]=0;}
    void addRoundKey(uint8_t *s,unsigned round)const{for(unsigned i=0;i<16;++i)s[i]^=roundKeys[16*round+i];}
    void decrypt(const uint8_t *in,uint8_t *out)const{
        uint8_t s[16];for(unsigned i=0;i<16;++i)s[i]=in[i];
        addRoundKey(s,10);
        for(unsigned round=9;;--round){
            // InvShiftRows: row r shifts right by r (state is column-major: s[c*4+r]).
            for(unsigned r=1;r<4;++r){
                uint8_t row[4];for(unsigned c=0;c<4;++c)row[c]=s[c*4+r];
                for(unsigned c=0;c<4;++c)s[((c+r)%4)*4+r]=row[c];
            }
            for(unsigned i=0;i<16;++i)s[i]=inv[s[i]];
            addRoundKey(s,round);
            if(!round)break;
            for(unsigned c=0;c<4;++c){ // InvMixColumns
                uint8_t *col=s+4*c,a0=col[0],a1=col[1],a2=col[2],a3=col[3];
                col[0]=uint8_t(mul(a0,14)^mul(a1,11)^mul(a2,13)^mul(a3,9));
                col[1]=uint8_t(mul(a0,9)^mul(a1,14)^mul(a2,11)^mul(a3,13));
                col[2]=uint8_t(mul(a0,13)^mul(a1,9)^mul(a2,14)^mul(a3,11));
                col[3]=uint8_t(mul(a0,11)^mul(a1,13)^mul(a2,9)^mul(a3,14));
            }
        }
        for(unsigned i=0;i<16;++i)out[i]=s[i];
        volatile uint8_t *w=s;for(unsigned i=0;i<16;++i)w[i]=0;
    }
};
inline bool aes128Decrypt(const uint8_t *key,const uint8_t *in,uint8_t *out){
    if(!key || !in || !out)return false;
    AES128 aes(key);aes.decrypt(in,out);return true;
}
// Crypto policy for AthSecurity::Supplicant<Crypto>. Random is supplied by
// the build: read_random (Mach KPI) in the kext, a test source on the host.
template<bool (*Random)(uint8_t *,size_t)> struct Policy {
    static bool random(uint8_t *p,size_t n){return Random(p,n);}
    static bool hmac(const uint8_t *k,size_t kl,const uint8_t *p,size_t n,uint8_t *out){return hmacSHA1(k,kl,p,n,out);}
    static bool decrypt(const uint8_t *key,const uint8_t *in,uint8_t *out){return aes128Decrypt(key,in,out);}
};
}
