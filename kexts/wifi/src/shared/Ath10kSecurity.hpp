#pragma once
#include <stdint.h>
#include <stddef.h>
// Gate of the paused security work; see Ath10kControl.hpp / docs/WIFI-SECURITY.md.
#ifndef AIRPORT_ATH10K_WPASEC
#define AIRPORT_ATH10K_WPASEC 0
#endif
namespace AthSecurity {
inline void wipe(void *p,size_t n){volatile uint8_t *b=(volatile uint8_t *)p;while(n--)*b++=0;}
inline bool equal(const uint8_t *a,const uint8_t *b,size_t n){uint8_t d=0;for(size_t i=0;i<n;++i)d|=a[i]^b[i];return d==0;}
inline uint16_t le16(const uint8_t *p){return static_cast<uint16_t>(p[0]|(uint16_t(p[1])<<8));}
inline uint16_t be16(const uint8_t *p){return static_cast<uint16_t>((uint16_t(p[0])<<8)|p[1]);}
inline void put16(uint8_t *p,uint16_t n){p[0]=static_cast<uint8_t>(n>>8);p[1]=static_cast<uint8_t>(n);}
inline uint64_t be64(const uint8_t *p){uint64_t n=0;for(unsigned i=0;i<8;++i)n=(n<<8)|p[i];return n;}
inline void copy(uint8_t *a,const uint8_t *b,size_t n){for(size_t i=0;i<n;++i)a[i]=b[i];}
// RSN IEs selected by this STA: CCMP pairwise and either CCMP or TKIP group,
// always with PSK AKM and without PMF.
static constexpr uint8_t stationRSN[]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
static constexpr uint8_t stationRSNTKIP[]={48,20,1,0,0,15,172,2,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
static constexpr uint8_t cipherTKIP=2,cipherCCMP=4,cipherBIP=6;
// AKM suite types (IEEE 802.11-2020 Table 9-151, OUI 00-0F-AC).
static constexpr uint8_t akm8021X=1,akmPSK=2,akm8021XSHA256=5,akmPSKSHA256=6,akmSAE=8;
inline bool akmSHA256(uint8_t akm){return akm==akm8021XSHA256 || akm==akmPSKSHA256 || akm==akmSAE;}
inline bool akmEnterprise(uint8_t akm){return akm==akm8021X || akm==akm8021XSHA256;}
inline bool suite(const uint8_t *p,uint8_t type){return !p[0]&&p[1]==15&&p[2]==172&&p[3]==type;}
inline uint8_t wpa2GroupCipher(const uint8_t *ie,size_t n){
 if(!ie||n<8||n>257||ie[0]!=48||size_t(ie[1])+2!=n||le16(ie+2)!=1)return 0;
 if(suite(ie+4,cipherTKIP))return cipherTKIP;
 if(suite(ie+4,cipherCCMP))return cipherCCMP;
 return 0;
}
inline const uint8_t *stationRSNForGroup(uint8_t group,size_t &n){
 if(group==cipherCCMP){n=sizeof(stationRSN);return stationRSN;}
 if(group==cipherTKIP){n=sizeof(stationRSNTKIP);return stationRSNTKIP;}
 n=0;return nullptr;
}
// Parsed RSN element (IEEE 802.11-2020 9.4.2.24). Strict bounds; every
// optional field must be complete. akmMask bit t is set for AKM 00-0F-AC:t
// (t < 16); pairwise CCMP/TKIP flags; group management cipher as a type.
struct RSNInfo {
 uint8_t group=0,groupMgmt=0,pmkidCount=0;const uint8_t *pmkids=nullptr;
 bool pairwiseCCMP=false,pairwiseTKIP=false,capsPresent=false,mfpc=false,mfpr=false;
 uint16_t akmMask=0,caps=0;
 bool akm(uint8_t t) const {return t<16 && (akmMask&(1u<<t));}
};
inline bool parseRSN(const uint8_t *ie,size_t n,RSNInfo &out){
 out=RSNInfo{};
 if(!ie||n<8||n>257||ie[0]!=48||size_t(ie[1])+2!=n||le16(ie+2)!=1)return false;
 if(suite(ie+4,cipherTKIP))out.group=cipherTKIP;else if(suite(ie+4,cipherCCMP))out.group=cipherCCMP;else return false;
 size_t p=8;
 for(unsigned section=0;section<2;++section){
  if(n-p<2)return section==0?false:true; // group-only IE is legal but useless; require the pairwise list
  unsigned count=le16(ie+p);p+=2;
  if(!count||count>(n-p)/4)return false;
  for(unsigned i=0;i<count;++i,p+=4){
   if(section==0){if(suite(ie+p,cipherCCMP))out.pairwiseCCMP=true;else if(suite(ie+p,cipherTKIP))out.pairwiseTKIP=true;}
   else if(!ie[p]&&ie[p+1]==15&&ie[p+2]==172&&ie[p+3]<16)out.akmMask|=uint16_t(1u<<ie[p+3]);
  }
 }
 if(n>p){if(n-p<2)return false;out.caps=le16(ie+p);out.capsPresent=true;p+=2;
  out.mfpr=(out.caps&(1u<<6))!=0;out.mfpc=(out.caps&(1u<<7))!=0;}
 if(n>p){if(n-p<2)return false;unsigned count=le16(ie+p);p+=2;if(count>(n-p)/16)return false;
  out.pmkidCount=uint8_t(count>255?255:count);out.pmkids=ie+p;p+=16*count;}
 if(n>p){if(n-p!=4)return false;if(!ie[p]&&ie[p+1]==15&&ie[p+2]==172)out.groupMgmt=ie[p+3];else return false;p+=4;}
 return p==n;
}
// Strict bounds on optional fields; require AES/CCMP pairwise and PSK.
// TKIP is allowed only for the group cipher; TKIP pairwise remains unsupported.
inline bool supportsWPA2(const uint8_t *ie,size_t n){
 if(n<20||n>257||!wpa2GroupCipher(ie,n))return false;
 size_t p=8;bool pair=false,psk=false;
 for(unsigned section=0;section<2;++section){
  if(n-p<2)return false;
  unsigned count=le16(ie+p);p+=2;
  if(!count||count>(n-p)/4)return false;
  for(unsigned i=0;i<count;++i,p+=4){if(suite(ie+p,section?2:4)){if(section)psk=true;else pair=true;}}
 }
 if(n>p){if(n-p<2)return false;uint16_t caps=le16(ie+p);p+=2;
  if(caps&((1u<<6)|(1u<<9)|(1u<<13)))return false; // MFPR, PeerKey, extended key ID
 }
 if(n>p){if(n-p<2)return false;
  unsigned count=le16(ie+p);p+=2;if(count>(n-p)/16)return false;p+=16*count;}
 if(n>p){if(n-p!=4)return false;p+=4;}
 return p==n&&pair&&psk;
}
// Can this station join the AP's RSN with the given AKM? CCMP pairwise is
// required; PMF must be possible when the AP requires it and vice versa;
// PeerKey/extended key ID are refused; BIP-CMAC-128 is the only group
// management cipher (an absent one means BIP-CMAC-128, 9.4.2.24.5).
inline bool rsnAcceptable(const RSNInfo &r,uint8_t akm,bool pmfCapable,bool pmfRequired){
 if(!r.pairwiseCCMP||!r.akm(akm)||(r.caps&((1u<<9)|(1u<<13))))return false;
 if(r.mfpr&&!pmfCapable)return false;
 if(pmfRequired&&!r.mfpc)return false;
 if(r.mfpc&&pmfCapable&&r.groupMgmt&&r.groupMgmt!=cipherBIP)return false;
 return true;
}
// The RSN element this station sends in its (Re)Association Request and in
// EAPOL message 2: CCMP pairwise, the AP's group cipher, one AKM, RSN
// capabilities (MFPC/MFPR), an optional PMKID and, with PMF, BIP-CMAC-128.
inline size_t buildStationRSN(uint8_t *out,size_t capacity,uint8_t group,uint8_t akm,bool mfpc,bool mfpr,const uint8_t *pmkid){
 const size_t n=22+(pmkid||mfpc?2:0)+(pmkid?16:0)+(mfpc?4:0);
 if(!out||capacity<n||(group!=cipherCCMP&&group!=cipherTKIP)||(mfpr&&!mfpc))return 0;
 size_t at=0;out[at++]=48;out[at++]=uint8_t(n-2);out[at++]=1;out[at++]=0;
 out[at++]=0;out[at++]=15;out[at++]=172;out[at++]=group;
 out[at++]=1;out[at++]=0;out[at++]=0;out[at++]=15;out[at++]=172;out[at++]=cipherCCMP;
 out[at++]=1;out[at++]=0;out[at++]=0;out[at++]=15;out[at++]=172;out[at++]=akm;
 const uint16_t caps=uint16_t((mfpc?1u<<7:0)|(mfpr?1u<<6:0));out[at++]=uint8_t(caps);out[at++]=uint8_t(caps>>8);
 if(pmkid||mfpc){out[at++]=pmkid?1:0;out[at++]=0;if(pmkid)for(unsigned i=0;i<16;++i)out[at++]=pmkid[i];}
 if(mfpc){out[at++]=0;out[at++]=15;out[at++]=172;out[at++]=cipherBIP;}
 return at;
}
// RSNXE (9.4.2.241): bit 5 of the first capability octet is SAE hash-to-element.
inline bool rsnxeH2E(const uint8_t *ie,size_t n){return ie&&n>=3&&ie[0]==244&&size_t(ie[1])+2==n&&(ie[2]&0x20);}
}
