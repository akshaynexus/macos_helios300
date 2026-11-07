// SPDX-License-Identifier: GPL-3.0-or-later
// Ath10kP256.hpp — NIST P-256 (IANA group 19) for SAE in the kernel: 256-bit
// integers as four 64-bit limbs, Montgomery multiplication (CIOS) modulo the
// field prime p and the group order n, Jacobian points with the a = -3
// formulas, a Montgomery ladder for scalar multiplication and constant-time
// selection throughout. No libc, no allocation, no data-dependent branches on
// secret values (the public exponents used for inversion, square roots and
// the Legendre symbol are fixed). Checked against the NIST test vectors and
// hostapd's SAE vectors in tests/test_wpa3_crypto.cpp.
#pragma once
#include <stdint.h>
#include <stddef.h>
namespace AthP256 {
typedef unsigned __int128 u128;
inline void wipe(void *p,size_t n){volatile uint8_t *b=(volatile uint8_t *)p;while(n--)*b++=0;}
struct U256 { uint64_t v[4]; };
inline U256 zero(){return U256{{0,0,0,0}};}
inline bool isZero(const U256 &a){return ((a.v[0]|a.v[1]|a.v[2]|a.v[3])==0);}
inline uint64_t mask64(bool b){return b?~uint64_t(0):0;}
inline void select(U256 &r,const U256 &a,const U256 &b,uint64_t m){for(unsigned i=0;i<4;++i)r.v[i]=(a.v[i]&~m)|(b.v[i]&m);} // m ? b : a
inline void cswap(U256 &a,U256 &b,uint64_t m){for(unsigned i=0;i<4;++i){const uint64_t t=(a.v[i]^b.v[i])&m;a.v[i]^=t;b.v[i]^=t;}}
inline bool equal(const U256 &a,const U256 &b){uint64_t d=0;for(unsigned i=0;i<4;++i)d|=a.v[i]^b.v[i];return d==0;}
// r = a + b, returns the carry.
inline uint64_t add(U256 &r,const U256 &a,const U256 &b){u128 c=0;for(unsigned i=0;i<4;++i){c+=u128(a.v[i])+b.v[i];r.v[i]=uint64_t(c);c>>=64;}return uint64_t(c);}
// r = a - b, returns the borrow.
inline uint64_t sub(U256 &r,const U256 &a,const U256 &b){uint64_t borrow=0;for(unsigned i=0;i<4;++i){const u128 d=u128(a.v[i])-b.v[i]-borrow;r.v[i]=uint64_t(d);borrow=uint64_t(d>>64)&1;}return borrow;}
inline bool less(const U256 &a,const U256 &b){U256 t;return sub(t,a,b)!=0;}
inline void fromBytes(U256 &r,const uint8_t *p){for(unsigned i=0;i<4;++i){uint64_t w=0;for(unsigned j=0;j<8;++j)w=(w<<8)|p[8*(3-i)+j];r.v[i]=w;}}
inline void toBytes(const U256 &a,uint8_t *p){for(unsigned i=0;i<4;++i)for(unsigned j=0;j<8;++j)p[8*(3-i)+j]=uint8_t(a.v[i]>>(56-8*j));}
// An odd modulus with its Montgomery constants (R = 2^256).
struct Modulus {
    U256 m,r2,one; // r2 = R^2 mod m, one = R mod m (Montgomery 1)
    uint64_t inv; // -m^-1 mod 2^64
    void init(const U256 &modulus){
        m=modulus;
        uint64_t x=1;for(unsigned i=0;i<7;++i)x*=2-m.v[0]*x; // Newton: x = m0^-1 mod 2^64
        inv=~x+1;
        sub(one,zero(),m); // R mod m = 2^256 - m (m > 2^255 for p and n)
        r2=one;for(unsigned i=0;i<256;++i)addMod(r2,r2,r2);
    }
    void addMod(U256 &r,const U256 &a,const U256 &b)const{
        U256 s,t;const uint64_t carry=add(s,a,b);const uint64_t borrow=sub(t,s,m);
        select(r,t,s,mask64(!carry && borrow)); // keep s when it did not reach m
    }
    void subMod(U256 &r,const U256 &a,const U256 &b)const{
        U256 d,t;const uint64_t borrow=sub(d,a,b);add(t,d,m);select(r,d,t,mask64(borrow));
    }
    // CIOS Montgomery multiplication: r = a*b*R^-1 mod m.
    void mul(U256 &r,const U256 &a,const U256 &b)const{
        uint64_t t[6]={0,0,0,0,0,0};
        for(unsigned i=0;i<4;++i){
            u128 c=0;
            for(unsigned j=0;j<4;++j){c+=u128(a.v[j])*b.v[i]+t[j];t[j]=uint64_t(c);c>>=64;}
            c+=t[4];t[4]=uint64_t(c);t[5]=uint64_t(c>>64);
            const uint64_t q=t[0]*inv;
            c=u128(q)*m.v[0]+t[0];c>>=64;
            for(unsigned j=1;j<4;++j){c+=u128(q)*m.v[j]+t[j];t[j-1]=uint64_t(c);c>>=64;}
            c+=t[4];t[3]=uint64_t(c);c>>=64;t[4]=t[5]+uint64_t(c);
        }
        U256 s{{t[0],t[1],t[2],t[3]}},d;const uint64_t borrow=sub(d,s,m);
        select(r,d,s,mask64(!t[4] && borrow));
        wipe(t,sizeof(t));
    }
    void sqr(U256 &r,const U256 &a)const{mul(r,a,a);}
    void toMont(U256 &r,const U256 &a)const{mul(r,a,r2);}
    void fromMont(U256 &r,const U256 &a)const{U256 o{{1,0,0,0}};mul(r,a,o);}
    // r = a^e (Montgomery form in, Montgomery form out); e is public.
    void pow(U256 &r,const U256 &a,const U256 &e)const{
        U256 acc=one,base=a;
        for(int i=255;i>=0;--i){sqr(acc,acc);U256 t;mul(t,acc,base);select(acc,acc,t,mask64((e.v[i/64]>>(i%64))&1));}
        r=acc;
    }
    // Reduce x (< 2^256) modulo m: one conditional subtraction suffices for p and n (m > 2^255).
    void reduce(U256 &r,const U256 &x)const{U256 d;const uint64_t borrow=sub(d,x,m);select(r,d,x,mask64(borrow));}
    // Reduce a 384-bit big-endian value (hi 16 bytes, lo 32 bytes) modulo m, normal form out.
    void reduceWide(U256 &r,const uint8_t *be48)const{
        U256 hi=zero(),lo,t;
        for(unsigned i=0;i<16;++i)hi.v[1-i/8]=(hi.v[1-i/8]<<8)|be48[i];
        fromBytes(lo,be48+16);
        mul(t,hi,r2); // hi*R mod m
        U256 s,d;const uint64_t carry=add(s,t,lo); // < 3m
        uint64_t borrow=sub(d,s,m);select(s,d,s,mask64(!carry && borrow));
        borrow=sub(d,s,m);select(r,d,s,mask64(borrow));
    }
};
static constexpr U256 kP{{0xFFFFFFFFFFFFFFFFull,0x00000000FFFFFFFFull,0x0000000000000000ull,0xFFFFFFFF00000001ull}};
static constexpr U256 kN{{0xF3B9CAC2FC632551ull,0xBCE6FAADA7179E84ull,0xFFFFFFFFFFFFFFFFull,0xFFFFFFFF00000000ull}};
static constexpr U256 kB{{0x3BCE3C3E27D2604Bull,0x651D06B0CC53B0F6ull,0xB3EBBD55769886BCull,0x5AC635D8AA3A93E7ull}};
static constexpr U256 kGx{{0xF4A13945D898C296ull,0x77037D812DEB33A0ull,0xF8BCE6E563A440F2ull,0x6B17D1F2E12C4247ull}};
static constexpr U256 kGy{{0xCBB6406837BF51F5ull,0x2BCE33576B315ECEull,0x8EE7EB4A7C0F9E16ull,0x4FE342E2FE1A7F9Bull}};
struct Point { U256 x,y,z; }; // Jacobian, Montgomery form; z == 0 is the point at infinity
class Curve {
public:
    Modulus fp,fn;U256 b,three,pMinus2,sqrtExp,legendreExp;
    Curve(){
        fp.init(kP);fn.init(kN);fp.toMont(b,kB);
        U256 t{{3,0,0,0}};fp.toMont(three,t);
        U256 two{{2,0,0,0}},one{{1,0,0,0}},four{{4,0,0,0}};
        sub(pMinus2,kP,two);
        add(sqrtExp,kP,one);for(unsigned i=0;i<2;++i)shr(sqrtExp); // (p+1)/4
        sub(legendreExp,kP,one);shr(legendreExp); // (p-1)/2
        (void)four;
    }
    static void shr(U256 &a){for(unsigned i=0;i<4;++i){a.v[i]>>=1;if(i<3)a.v[i]|=a.v[i+1]<<63;}}
    void inv(U256 &r,const U256 &a)const{fp.pow(r,a,pMinus2);}
    // Right-hand side of the curve equation in Montgomery form: x^3 - 3x + b.
    void rhs(U256 &r,const U256 &x)const{U256 x2,x3,t;fp.sqr(x2,x);fp.mul(x3,x2,x);fp.mul(t,three,x);fp.subMod(x3,x3,t);fp.addMod(r,x3,b);}
    bool isQR(const U256 &a)const{U256 l;fp.pow(l,a,legendreExp);return equal(l,fp.one);} // a != 0 assumed by callers
    // y = sqrt(a) when a is a quadratic residue (p = 3 mod 4); returns false otherwise.
    bool sqrt(U256 &y,const U256 &a)const{U256 s,c;fp.pow(s,a,sqrtExp);fp.sqr(c,s);y=s;return equal(c,a);}
    void infinity(Point &p)const{p.x=fp.one;p.y=fp.one;p.z=zero();}
    bool isInfinity(const Point &p)const{return isZero(p.z);}
    // Affine (normal-form bytes) to Jacobian; validates x,y < p and the curve equation.
    bool fromAffine(Point &p,const U256 &x,const U256 &y)const{
        if(!less(x,kP) || !less(y,kP))return false;
        fp.toMont(p.x,x);fp.toMont(p.y,y);p.z=fp.one;
        U256 l,r;fp.sqr(l,p.y);rhs(r,p.x);return equal(l,r);
    }
    void toAffine(const Point &p,U256 &x,U256 &y)const{
        U256 zi,zi2,zi3,t;inv(zi,p.z);fp.sqr(zi2,zi);fp.mul(zi3,zi2,zi);
        fp.mul(t,p.x,zi2);fp.fromMont(x,t);fp.mul(t,p.y,zi3);fp.fromMont(y,t);
    }
    void negate(Point &p)const{fp.subMod(p.y,zero(),p.y);}
    // dbl-2001-b (a = -3).
    void dbl(Point &r,const Point &p)const{
        U256 delta,gamma,beta,alpha,t1,t2,x3,y3,z3;
        fp.sqr(delta,p.z);fp.sqr(gamma,p.y);fp.mul(beta,p.x,gamma);
        fp.subMod(t1,p.x,delta);fp.addMod(t2,p.x,delta);fp.mul(alpha,t1,t2);fp.addMod(t1,alpha,alpha);fp.addMod(alpha,t1,alpha);
        fp.sqr(x3,alpha);fp.addMod(t1,beta,beta);fp.addMod(t1,t1,t1);fp.addMod(t2,t1,t1);fp.subMod(x3,x3,t2); // x3 = alpha^2 - 8 beta
        fp.addMod(z3,p.y,p.z);fp.sqr(z3,z3);fp.subMod(z3,z3,gamma);fp.subMod(z3,z3,delta);
        fp.subMod(t1,t1,x3); // 4 beta - x3
        fp.mul(y3,alpha,t1);fp.sqr(t2,gamma);fp.addMod(t2,t2,t2);fp.addMod(t2,t2,t2);fp.addMod(t2,t2,t2);fp.subMod(y3,y3,t2);
        r.x=x3;r.y=y3;r.z=z3;
    }
    // add-2007-bl with constant-time handling of the special cases.
    void addPoints(Point &r,const Point &p,const Point &q)const{
        U256 z1z1,z2z2,u1,u2,s1,s2,h,i,j,rr,v,x3,y3,z3,t;
        fp.sqr(z1z1,p.z);fp.sqr(z2z2,q.z);fp.mul(u1,p.x,z2z2);fp.mul(u2,q.x,z1z1);
        fp.mul(t,q.z,z2z2);fp.mul(s1,p.y,t);fp.mul(t,p.z,z1z1);fp.mul(s2,q.y,t);
        fp.subMod(h,u2,u1);fp.addMod(i,h,h);fp.sqr(i,i);fp.mul(j,h,i);fp.subMod(rr,s2,s1);fp.addMod(rr,rr,rr);fp.mul(v,u1,i);
        fp.sqr(x3,rr);fp.subMod(x3,x3,j);fp.subMod(x3,x3,v);fp.subMod(x3,x3,v);
        fp.subMod(t,v,x3);fp.mul(y3,rr,t);fp.mul(t,s1,j);fp.addMod(t,t,t);fp.subMod(y3,y3,t);
        fp.addMod(z3,p.z,q.z);fp.sqr(z3,z3);fp.subMod(z3,z3,z1z1);fp.subMod(z3,z3,z2z2);fp.mul(z3,z3,h);
        Point sum{x3,y3,z3},d;dbl(d,p);
        const uint64_t pInf=mask64(isZero(p.z)),qInf=mask64(isZero(q.z)),hZero=mask64(isZero(h)),rZero=mask64(isZero(rr));
        Point inf;infinity(inf);
        // h == 0 and r == 0: doubling; h == 0 and r != 0: P == -Q, infinity.
        Point out=sum;
        select(out.x,out.x,d.x,hZero&rZero);select(out.y,out.y,d.y,hZero&rZero);select(out.z,out.z,d.z,hZero&rZero);
        select(out.x,out.x,inf.x,hZero&~rZero);select(out.y,out.y,inf.y,hZero&~rZero);select(out.z,out.z,inf.z,hZero&~rZero);
        select(out.x,out.x,q.x,pInf);select(out.y,out.y,q.y,pInf);select(out.z,out.z,q.z,pInf);
        select(out.x,out.x,p.x,qInf);select(out.y,out.y,p.y,qInf);select(out.z,out.z,p.z,qInf);
        r=out;
    }
    // Montgomery ladder: r = k * p, constant time in k (k normal form).
    void mul(Point &r,const Point &p,const U256 &k)const{
        Point r0,r1=p;infinity(r0);
        for(int i=255;i>=0;--i){
            const uint64_t bit=mask64((k.v[i/64]>>(i%64))&1);
            cswap(r0.x,r1.x,bit);cswap(r0.y,r1.y,bit);cswap(r0.z,r1.z,bit);
            addPoints(r1,r0,r1);dbl(r0,r0);
            cswap(r0.x,r1.x,bit);cswap(r0.y,r1.y,bit);cswap(r0.z,r1.z,bit);
        }
        r=r0;wipe(&r1,sizeof(r1));
    }
    // Scalar helpers modulo n (normal form).
    void addModN(U256 &r,const U256 &a,const U256 &b)const{fn.addMod(r,a,b);}
    // Reduce a random 256-bit value into [2, n-1]: x mod (n-2) + 2 (one subtraction suffices).
    void scalarFromRandom(U256 &r,const U256 &x)const{
        U256 two{{2,0,0,0}},n2,d;sub(n2,kN,two);const uint64_t borrow=sub(d,x,n2);select(d,d,x,mask64(borrow));add(r,d,two);
    }
    // val mod (n-1) + 1 for SAE hash-to-element.
    void scalarOneToNMinus1(U256 &r,const U256 &x)const{
        U256 one{{1,0,0,0}},n1,d;sub(n1,kN,one);const uint64_t borrow=sub(d,x,n1);select(d,d,x,mask64(borrow));add(r,d,one);
    }
};
}
