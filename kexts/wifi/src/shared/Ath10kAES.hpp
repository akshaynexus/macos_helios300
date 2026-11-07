// SPDX-License-Identifier: GPL-3.0-or-later
// Ath10kAES.hpp — AES-128 block encrypt/decrypt (FIPS 197), AES-128-CMAC
// (RFC 4493 / NIST SP 800-38B) and the BIP-CMAC-128 MIC of IEEE 802.11-2020
// 12.5.4 for the kernel: no libc, no allocation, byte-wise tables built at
// construction. The kernel exports no single-block AES outside
// com.apple.kpi.unsupported, so the supplicant's RFC 3394 unwrap, the EAPOL
// AES-128-CMAC MIC of the SHA-256 AKMs and the core's BIP check of broadcast
// robust management frames all use this. Vectors: FIPS 197 C.1, RFC 4493,
// IEEE 802.11-2020 Annex J.9 (BIP); tests/test_wpa3_crypto.cpp.
#pragma once
#include <stdint.h>
#include <stddef.h>
namespace AthAES {
inline void wipe(void *p,size_t n){volatile uint8_t *b=(volatile uint8_t *)p;while(n--)*b++=0;}
struct AES128 {
    uint8_t sbox[256],inv[256],roundKeys[176];
    static uint8_t xtime(uint8_t x){return uint8_t((x<<1)^((x&0x80)?0x1b:0));}
    static uint8_t mul(uint8_t a,uint8_t b){uint8_t r=0;while(b){if(b&1)r^=a;a=xtime(a);b>>=1;}return r;}
    static uint8_t rol8(uint8_t v,unsigned n){return uint8_t((v<<n)|(v>>(8-n)));}
    void buildBoxes(){
        uint8_t p=1,q=1;
        do{ // multiplicative inverse via generator 3, then the affine transform
            p=uint8_t(p^(p<<1)^((p&0x80)?0x1b:0));
            q^=uint8_t(q<<1);q^=uint8_t(q<<2);q^=uint8_t(q<<4);if(q&0x80)q^=0x09;
            sbox[p]=uint8_t(q^rol8(q,1)^rol8(q,2)^rol8(q,3)^rol8(q,4)^0x63);
        }while(p!=1);
        sbox[0]=0x63;
        for(unsigned i=0;i<256;++i)inv[sbox[i]]=uint8_t(i);
    }
    explicit AES128(const uint8_t *key){
        buildBoxes();
        for(unsigned i=0;i<16;++i)roundKeys[i]=key[i];
        uint8_t rcon=1;
        for(unsigned i=16;i<176;i+=4){
            uint8_t t[4]={roundKeys[i-4],roundKeys[i-3],roundKeys[i-2],roundKeys[i-1]};
            if(i%16==0){
                const uint8_t first=t[0];t[0]=uint8_t(sbox[t[1]]^rcon);t[1]=sbox[t[2]];t[2]=sbox[t[3]];t[3]=sbox[first];
                rcon=xtime(rcon);
            }
            for(unsigned j=0;j<4;++j)roundKeys[i+j]=uint8_t(roundKeys[i-16+j]^t[j]);
        }
    }
    ~AES128(){wipe(roundKeys,176);}
    void addRoundKey(uint8_t *s,unsigned round)const{for(unsigned i=0;i<16;++i)s[i]^=roundKeys[16*round+i];}
    // State is column-major (s[c*4+r]); ShiftRows shifts row r left by r.
    void encrypt(const uint8_t *in,uint8_t *out)const{
        uint8_t s[16];for(unsigned i=0;i<16;++i)s[i]=in[i];
        addRoundKey(s,0);
        for(unsigned round=1;round<=10;++round){
            for(unsigned i=0;i<16;++i)s[i]=sbox[s[i]];
            for(unsigned r=1;r<4;++r){
                uint8_t row[4];for(unsigned c=0;c<4;++c)row[c]=s[c*4+r];
                for(unsigned c=0;c<4;++c)s[c*4+r]=row[(c+r)%4];
            }
            if(round<10)for(unsigned c=0;c<4;++c){
                uint8_t *col=s+4*c;const uint8_t a0=col[0],a1=col[1],a2=col[2],a3=col[3];
                col[0]=uint8_t(xtime(a0)^xtime(a1)^a1^a2^a3);
                col[1]=uint8_t(a0^xtime(a1)^xtime(a2)^a2^a3);
                col[2]=uint8_t(a0^a1^xtime(a2)^xtime(a3)^a3);
                col[3]=uint8_t(xtime(a0)^a0^a1^a2^xtime(a3));
            }
            addRoundKey(s,round);
        }
        for(unsigned i=0;i<16;++i)out[i]=s[i];
        wipe(s,16);
    }
    void decrypt(const uint8_t *in,uint8_t *out)const{
        uint8_t s[16];for(unsigned i=0;i<16;++i)s[i]=in[i];
        addRoundKey(s,10);
        for(unsigned round=9;;--round){
            for(unsigned r=1;r<4;++r){
                uint8_t row[4];for(unsigned c=0;c<4;++c)row[c]=s[c*4+r];
                for(unsigned c=0;c<4;++c)s[((c+r)%4)*4+r]=row[c];
            }
            for(unsigned i=0;i<16;++i)s[i]=inv[s[i]];
            addRoundKey(s,round);
            if(!round)break;
            for(unsigned c=0;c<4;++c){
                uint8_t *col=s+4*c;const uint8_t a0=col[0],a1=col[1],a2=col[2],a3=col[3];
                col[0]=uint8_t(mul(a0,14)^mul(a1,11)^mul(a2,13)^mul(a3,9));
                col[1]=uint8_t(mul(a0,9)^mul(a1,14)^mul(a2,11)^mul(a3,13));
                col[2]=uint8_t(mul(a0,13)^mul(a1,9)^mul(a2,14)^mul(a3,11));
                col[3]=uint8_t(mul(a0,11)^mul(a1,13)^mul(a2,9)^mul(a3,14));
            }
        }
        for(unsigned i=0;i<16;++i)out[i]=s[i];
        wipe(s,16);
    }
};
inline bool encrypt(const uint8_t *key,const uint8_t *in,uint8_t *out){if(!key || !in || !out)return false;AES128 a(key);a.encrypt(in,out);return true;}
inline bool decrypt(const uint8_t *key,const uint8_t *in,uint8_t *out){if(!key || !in || !out)return false;AES128 a(key);a.decrypt(in,out);return true;}
// AES-128-CMAC over up to three concatenated parts (RFC 4493 §2.4).
inline bool cmac(const uint8_t *key,const uint8_t *a,size_t an,const uint8_t *b,size_t bn,const uint8_t *c,size_t cn,uint8_t *out){
    if(!key || !out || (!a && an) || (!b && bn) || (!c && cn))return false;
    AES128 aes(key);
    uint8_t L[16]{},k1[16],k2[16];aes.encrypt(L,L);
    auto shift=[](const uint8_t *in,uint8_t *o){uint8_t carry=0;for(int i=15;i>=0;--i){const uint8_t nc=uint8_t(in[i]>>7);o[i]=uint8_t((in[i]<<1)|carry);carry=nc;}if(carry)o[15]^=0x87;};
    shift(L,k1);shift(k1,k2);
    const size_t total=an+bn+cn;
    const uint8_t *parts[3]={a,b,c};const size_t lens[3]={an,bn,cn};
    uint8_t x[16]{},blk[16];size_t fill=0,consumed=0;
    for(unsigned p=0;p<3;++p)for(size_t i=0;i<lens[p];++i){
        blk[fill++]=parts[p][i];++consumed;
        if(fill==16 && consumed<total){for(unsigned j=0;j<16;++j)x[j]^=blk[j];aes.encrypt(x,x);fill=0;}
    }
    if(total && fill==16){for(unsigned j=0;j<16;++j)x[j]^=uint8_t(blk[j]^k1[j]);}
    else {blk[fill++]=0x80;while(fill<16)blk[fill++]=0;for(unsigned j=0;j<16;++j)x[j]^=uint8_t(blk[j]^k2[j]);}
    aes.encrypt(x,out);
    wipe(L,16);wipe(k1,16);wipe(k2,16);wipe(x,16);wipe(blk,16);return true;
}
// BIP-CMAC-128 (IEEE 802.11-2020 12.5.4.5): MIC = AES-128-CMAC(IGTK, AAD ||
// body) over AAD = FC (with Retry, Power Management and More Data cleared)
// || A1 || A2 || A3 and the frame body including the MMIE with a zeroed MIC.
// frame points to the 802.11 header; length covers the body up to and
// including the MMIE (id 76, 16 bytes: 2 key id, 6 IPN, 8 MIC).
inline bool bip(const uint8_t *igtk,const uint8_t *frame,size_t length,uint8_t *mic8){
    if(!igtk || !frame || !mic8 || length<24+18)return false;
    uint8_t aad[20]={uint8_t(frame[0]),uint8_t(frame[1]&0xc7)}; // clear Retry, Power Management, More Data
    for(unsigned i=0;i<18;++i)aad[2+i]=frame[4+i];
    uint8_t out[16];
    // Body with the last 8 bytes (the MMIE MIC) replaced by zeros.
    static const uint8_t zeros[8]{};
    if(!cmac(igtk,aad,20,frame+24,length-24-8,zeros,8,out))return false;
    for(unsigned i=0;i<8;++i)mic8[i]=out[i];
    wipe(out,16);return true;
}
}
