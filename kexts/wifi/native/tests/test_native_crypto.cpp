// Host test: the in-kernel crypto policy against published vectors, then the
// unchanged WPA2 supplicant driven through it with the existing handshake vectors.
#include <cassert>
#include <cstring>
#include <cstdio>
#include "../src/AirportAth10kCrypto.hpp"
#include "../../src/shared/Ath10kSupplicant.hpp"
#include "../../tests/security_vectors.inc"
static bool fixedRandom(uint8_t *p,size_t n){for(size_t i=0;i<n;++i)p[i]=uint8_t(i+51);return true;}
using Crypto=AirportCrypto::Policy<fixedRandom>;
static void hex(const char *s,uint8_t *out,size_t n){for(size_t i=0;i<n;++i){unsigned v;sscanf(s+2*i,"%2x",&v);out[i]=uint8_t(v);}}
int main(){
    uint8_t d[20],e[20];
    AirportCrypto::sha1(reinterpret_cast<const uint8_t *>("abc"),3,d);hex("a9993e364706816aba3e25717850c26c9cd0d89d",e,20);assert(!memcmp(d,e,20));
    const char *m2="abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    AirportCrypto::sha1(reinterpret_cast<const uint8_t *>(m2),strlen(m2),d);hex("84983e441c3bd26ebaae4aa1f95129e5e54670f1",e,20);assert(!memcmp(d,e,20));
    { // one million 'a' (FIPS 180-1 example), streamed
        AirportCrypto::SHA1 s;uint8_t a[1000];memset(a,'a',1000);for(int i=0;i<1000;++i)s.update(a,1000);s.final(d);
        hex("34aa973cd4c4daa4f61eeb2bdbad27316534016f",e,20);assert(!memcmp(d,e,20));
    }
    { // RFC 2202 HMAC-SHA1 test cases 1, 2, 4 and 6 (key longer than a block)
        uint8_t key[80];memset(key,0x0b,20);
        assert(Crypto::hmac(key,20,reinterpret_cast<const uint8_t *>("Hi There"),8,d));hex("b617318655057264e28bc0b6fb378c8ef146be00",e,20);assert(!memcmp(d,e,20));
        assert(Crypto::hmac(reinterpret_cast<const uint8_t *>("Jefe"),4,reinterpret_cast<const uint8_t *>("what do ya want for nothing?"),28,d));hex("effcdf6ae5eb2fa2d27416d5f184df9c259a7c79",e,20);assert(!memcmp(d,e,20));
        for(unsigned i=0;i<25;++i){key[i]=uint8_t(i+1);}
        uint8_t cd[50];memset(cd,0xcd,50);
        assert(Crypto::hmac(key,25,cd,50,d));hex("4c9007f4026250c6bc8414f9bf50c86c2d7235da",e,20);assert(!memcmp(d,e,20));
        memset(key,0xaa,80);
        assert(Crypto::hmac(key,80,reinterpret_cast<const uint8_t *>("Test Using Larger Than Block-Size Key - Hash Key First"),54,d));hex("aa4ae5e15272d00e95705637ce8a3b55ed402112",e,20);assert(!memcmp(d,e,20));
    }
    { // FIPS-197 C.1 and Appendix B
        uint8_t k[16],c[16],p[16],o[16];
        hex("000102030405060708090a0b0c0d0e0f",k,16);hex("69c4e0d86a7b0430d8cdb78070b4c55a",c,16);hex("00112233445566778899aabbccddeeff",p,16);
        assert(Crypto::decrypt(k,c,o) && !memcmp(o,p,16));
        hex("2b7e151628aed2a6abf7158809cf4f3c",k,16);hex("3925841d02dc09fbdc118597196a0b32",c,16);hex("3243f6a8885a308d313198a2e0370734",p,16);
        assert(Crypto::decrypt(k,c,o) && !memcmp(o,p,16));
        assert(!Crypto::decrypt(nullptr,c,o));
    }
    { // The unchanged supplicant over this policy with the shared vectors: M1 -> M2 -> M3 -> keys and M4.
        AthSecurity::Supplicant<Crypto> s;
        assert(s.begin(v_pmk,v_snonce,v_sta,v_ap,v_rsn,sizeof(v_rsn)));
        assert(s.receive(v_m1,sizeof(v_m1))==AthSecurity::Supplicant<Crypto>::Reply);
        assert(s.outputLength==sizeof(v_m2) && !memcmp(s.output,v_m2,sizeof(v_m2)));
        assert(s.receive(v_m3,sizeof(v_m3))==AthSecurity::Supplicant<Crypto>::Keys);
        assert(!memcmp(s.keys,v_ptk+32,16) && !memcmp(s.keys+16,v_gtk,16) && s.keys[48]==4);
        assert(s.outputLength==sizeof(v_m4) && !memcmp(s.output,v_m4,sizeof(v_m4)));
        uint8_t bad[sizeof(v_m3)];memcpy(bad,v_m3,sizeof(bad));bad[14+81]^=1;
        AthSecurity::Supplicant<Crypto> t;assert(t.begin(v_pmk,v_snonce,v_sta,v_ap,v_rsn,sizeof(v_rsn)));
        assert(t.receive(v_m1,sizeof(v_m1))==AthSecurity::Supplicant<Crypto>::Reply);
        assert(t.receive(bad,sizeof(bad))==AthSecurity::Supplicant<Crypto>::Fatal);
    }
    puts("Native crypto tests passed (SHA-1, HMAC-SHA1 RFC 2202, AES-128 FIPS-197, supplicant vectors over the kernel policy).");
}
