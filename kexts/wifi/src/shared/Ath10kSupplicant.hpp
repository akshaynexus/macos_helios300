#pragma once
#include "Ath10kSecurity.hpp"
#include "Ath10kSHA256.hpp"
#include "Ath10kAES.hpp"
namespace AthSecurity {
// Crypto supplies HMAC-SHA1 and AES-128-ECB decrypt (software in the kernel).
// RSN 4-way and group handshakes (IEEE 802.11-2020 12.7) for AKMs
// 00-0F-AC:1 (802.1X), :2 (PSK), :5 (802.1X-SHA256), :6 (PSK-SHA256) and
// :8 (SAE): PRF-SHA1/HMAC-SHA1-128 with descriptor version 2 for 1 and 2,
// KDF-SHA256/AES-128-CMAC with version 3 (or 0, AKM-defined, as SAE
// authenticators send) for the others. CCMP pairwise, CCMP or TKIP group,
// IGTK KDE (BIP-CMAC-128) for PMF, RSNXE check, PMKID check in message 1.
// No EAP (the PMK/MSK comes from outside), no TKIP pairwise. Group and
// pairwise rekey retain replay protection. A retransmitted M3 returns the
// cached M4; keys are never asked to be reinstalled.
template<class Crypto> class Supplicant {
 uint8_t pmk[32]{},ptk[48]{},snonce[32]{},anonce[32]{},local[6]{},ap[6]{};
 uint8_t apRSN[257]{},savedM3[512]{},m2[192]{},m4[128]{};
 uint8_t stationRSN[64]{},apRSNXE[18]{},stationRSNXE[18]{},expectedPMKID[16]{};
 size_t rsnLength=0,savedLength=0,m2Length=0,m4Length=0,stationRSNLength=0,apRSNXELength=0,stationRSNXELength=0;
 uint64_t replay1=0,replay3=0;
 unsigned state=0;uint8_t akm=akmPSK,version=2;bool pmf=false,havePMKID=false;
 uint8_t establishedPTK[48]{},establishedANonce[32]{},establishedSNonce[32]{},currentGTK[32]{};
 uint8_t savedGroup[512]{},groupAck[128]{};size_t savedGroupLength=0,groupAckLength=0;
 uint64_t groupReplay=0;uint8_t currentGTKIndex=0,groupCipher=0;bool pairRekey=false;
 bool sha256() const {return akmSHA256(akm);}
 bool derive(){
  uint8_t context[76],digest[32];
  auto less=[](const uint8_t *a,const uint8_t *b,size_t n){for(size_t i=0;i<n;++i){if(a[i]!=b[i])return a[i]<b[i];}return false;};
  bool swap=less(local,ap,6);copy(context,swap?local:ap,6);copy(context+6,swap?ap:local,6);
  swap=less(snonce,anonce,32);copy(context+12,swap?snonce:anonce,32);copy(context+44,swap?anonce:snonce,32);
  bool ok;
  if(sha256())ok=AthSHA256::kdf(pmk,32,"Pairwise key expansion",context,76,ptk,384);
  else {
   static constexpr char label[]="Pairwise key expansion";
   uint8_t input[sizeof(label)+76+1]{};
   copy(input,(const uint8_t *)label,sizeof(label));copy(input+sizeof(label),context,76);
   ok=true;
   for(unsigned block=0;block<3 && ok;++block){input[sizeof(input)-1]=uint8_t(block);
    if(!Crypto::hmac(pmk,32,input,sizeof(input),digest))ok=false;
    else copy(ptk+20*block,digest,block==2?8:20);
   }
   wipe(input,sizeof(input));
  }
  wipe(context,sizeof(context));wipe(digest,sizeof(digest));return ok;
 }
 bool mic(uint8_t *p,size_t n){
  uint8_t digest[32]{};bool ok;
  if(sha256())ok=AthAES::cmac(ptk,p,n,nullptr,0,nullptr,0,digest);
  else ok=Crypto::hmac(ptk,16,p,n,digest);
  if(ok){copy(p+81,digest,16);}
  wipe(digest,32);return ok;
 }
 size_t response(uint8_t *frame,const uint8_t *request,bool second,bool group=false){
  const size_t ies=second?stationRSNLength+stationRSNXELength:0;
  if(second&&!stationRSNLength)return 0;
  size_t n=99+ies;wipe(frame,n+14);copy(frame,ap,6);copy(frame+6,local,6);frame[12]=0x88;frame[13]=0x8e;
  uint8_t *p=frame+14;p[0]=request[0];p[1]=3;put16(p+2,uint16_t(n-4));p[4]=2;
  put16(p+5,uint16_t(version|(second?0x0108:(group?0x0300:0x0308))));copy(p+9,request+9,8);
  if(second){copy(p+17,snonce,32);put16(p+97,uint16_t(ies));copy(p+99,stationRSN,stationRSNLength);copy(p+99+stationRSNLength,stationRSNXE,stationRSNXELength);}
  return mic(p,n)?n+14:0;
 }
 bool unwrap(const uint8_t *in,size_t n,uint8_t *out){
  if(n<24||n>392||n%8)return false;
  uint8_t a[8],b[16],dec[16];copy(a,in,8);copy(out,in+8,n-8);size_t blocks=n/8-1;
  bool ok=true;
  for(int j=5;j>=0 && ok;--j)for(size_t i=blocks;i>0;--i){
   copy(b,a,8);uint64_t t=blocks*unsigned(j)+i;for(unsigned k=0;k<8;++k)b[7-k]^=uint8_t(t>>(8*k));
   copy(b+8,out+8*(i-1),8);if(!Crypto::decrypt(ptk+16,b,dec)){ok=false;break;}
   copy(a,dec,8);copy(out+8*(i-1),dec+8,8);
  }
  for(uint8_t c:a)if(c!=0xa6)ok=false;
  wipe(a,8);wipe(b,16);wipe(dec,16);if(!ok)wipe(out,n-8);return ok;
 }
 // Key data KDEs: RSN (must equal the AP's), RSNXE (must equal the AP's when
 // either side has one), GTK, IGTK. Returns false on any malformed/mismatched
 // element. candidate is the InstallKeys payload being assembled.
 bool parseKeyData(const uint8_t *plain,size_t bytes,uint8_t *candidate,bool &haveRSN,bool &haveGTK,bool requireRSN){
  size_t at=0;bool valid=true,haveRSNXE=false;haveRSN=haveGTK=false;candidate[56]=0;
  while(at<bytes){
   if(plain[at]==221 && (at+1==bytes || plain[at+1]==0)){
    for(size_t j=at+1;j<bytes;++j)if(plain[j])valid=false;
    break;
   }
   if(bytes-at<2 || size_t(plain[at+1])+2>bytes-at){valid=false;break;}
   size_t len=size_t(plain[at+1])+2;
   if(plain[at]==48){if(haveRSN||len!=rsnLength||!equal(plain+at,apRSN,len))valid=false;haveRSN=true;}
   if(plain[at]==244){if(haveRSNXE||len!=apRSNXELength||!equal(plain+at,apRSNXE,len))valid=false;haveRSNXE=true;}
   if(plain[at]==221 && len>=6 && suite(plain+at+2,1)){
    const size_t gtkBytes=groupCipher==cipherTKIP?32:16;
    if(haveGTK || len!=8+gtkBytes || (plain[at+6]&0xf8) || plain[at+7])valid=false;
    else {candidate[49]=plain[at+6]&3;copy(candidate+16,plain+at+8,gtkBytes);}haveGTK=true;
   }
   if(plain[at]==221 && len>=6 && suite(plain+at+2,4)){ // IGTK KDE: KeyID(2) IPN(6) IGTK(16)
    if(candidate[56] || len!=30 || le16(plain+at+6)>5)valid=false;
    else {candidate[56]=1;copy(candidate+57,plain+at+6,2+6+16);}
   }
   at+=len;
  }
  if(apRSNXELength && !haveRSNXE)valid=false;
  if(requireRSN && !haveRSN)valid=false;
  return valid;
 }
public:
 enum Result { Ignore, Reply, Keys, Fatal, GroupKeys, PairKeys };
 // PTK[16], GTK[32], group cipher[1], GTK index[1], GTK RSC[6],
 // IGTK present[1], IGTK key id LE16[2], IPN[6], IGTK[16].
 static constexpr size_t keyPayloadLength=81;
 uint8_t output[192]{},keys[keyPayloadLength]{};size_t outputLength=0;
 const char *error="none";
 bool pairwisePending() const {return pairRekey && state==2;}
 bool pmfEnabled() const {return pmf;}
 uint8_t akmType() const {return akm;}
 void cancelPairwiseAttempt(){
  if(!pairwisePending())return;
  copy(ptk,establishedPTK,48);copy(anonce,establishedANonce,32);copy(snonce,establishedSNonce,32);
  pairRekey=false;state=3;outputLength=0;
 }
 ~Supplicant(){reset();}
 void reset(){wipe(this,sizeof(*this));error="none";akm=akmPSK;version=2;}
 // WPA2-PSK with the fixed station RSN of Ath10kSecurity.hpp (no PMF).
 bool begin(const uint8_t *master,const uint8_t *nonce,const uint8_t *station,const uint8_t *bssid,const uint8_t *rsn,size_t n){
  if(!supportsWPA2(rsn,n))return false;
  size_t sl=0;const uint8_t *s=stationRSNForGroup(wpa2GroupCipher(rsn,n),sl);
  return s && begin(master,nonce,station,bssid,rsn,n,akmPSK,s,sl,false,nullptr,0,nullptr,0,nullptr);
 }
 // General form: the AKM, the RSN element this station associated with (it
 // is echoed in message 2 and must match the AP's expectations), PMF, the
 // AP's and the station's RSNXE and the expected PMKID (SAE/802.1X) or null.
 bool begin(const uint8_t *master,const uint8_t *nonce,const uint8_t *station,const uint8_t *bssid,const uint8_t *rsn,size_t n,
            uint8_t akmType,const uint8_t *ownRSN,size_t ownRSNLength,bool managementProtection,
            const uint8_t *apXE,size_t apXELength,const uint8_t *ownXE,size_t ownXELength,const uint8_t *pmkid){
  reset();
  RSNInfo info;
  if(!parseRSN(rsn,n,info) || !info.pairwiseCCMP || !info.akm(akmType))return false;
  if(akmType!=akm8021X && akmType!=akmPSK && akmType!=akm8021XSHA256 && akmType!=akmPSKSHA256 && akmType!=akmSAE)return false;
  if(!ownRSN || ownRSNLength<20 || ownRSNLength>sizeof(stationRSN) || apXELength>sizeof(apRSNXE) || ownXELength>sizeof(stationRSNXE))return false;
  if((apXELength && (!apXE || apXE[0]!=244 || size_t(apXE[1])+2!=apXELength)) || (ownXELength && (!ownXE || ownXE[0]!=244 || size_t(ownXE[1])+2!=ownXELength)))return false;
  akm=akmType;version=sha256()?3:2;pmf=managementProtection;groupCipher=info.group;
  copy(pmk,master,32);copy(snonce,nonce,32);copy(local,station,6);copy(ap,bssid,6);copy(apRSN,rsn,n);rsnLength=n;
  copy(stationRSN,ownRSN,ownRSNLength);stationRSNLength=ownRSNLength;
  if(apXELength){copy(apRSNXE,apXE,apXELength);apRSNXELength=apXELength;}
  if(ownXELength){copy(stationRSNXE,ownXE,ownXELength);stationRSNXELength=ownXELength;}
  if(pmkid){copy(expectedPMKID,pmkid,16);havePMKID=true;}
  state=1;return true;
 }
 Result fail(const char *reason){error=reason;return Fatal;}
 Result receive(const uint8_t *frame,size_t length){
  outputLength=0;
  if(!state||length<113||length>512||!equal(frame,local,6)||!equal(frame+6,ap,6)||frame[12]!=0x88||frame[13]!=0x8e)return Ignore;
  const uint8_t *p=frame+14;size_t n=size_t(be16(p+2))+4;
  if((p[0]!=1 && p[0]!=2)||p[1]!=3||p[4]!=2||n<99||n>length-14||size_t(be16(p+97))+99!=n)return Ignore;
  const uint16_t info=be16(p+5);uint64_t replay=be64(p+9);
  const uint8_t ver=uint8_t(info&7);const uint16_t kind=uint16_t(info&~7u);
  // Descriptor version: 2 (HMAC-SHA1-128/AES key wrap) for AKM 1/2, 3
  // (AES-128-CMAC) or 0 (AKM-defined, same MIC) for the SHA-256 AKMs.
  if(sha256()?(ver!=3 && ver!=0):ver!=2)return Ignore;
  if(kind==0x0088){
   if(be16(p+7)!=16)return Ignore;
   uint8_t nz=0;for(unsigned i=0;i<32;++i)nz|=p[17+i];if(!nz)return Ignore;
   // Message 1 key data: optional PMKID KDE; with SAE/802.1X it must name our PMK.
   const size_t kd=be16(p+97);
   if(kd){
    if(kd!=22 || p[99]!=221 || p[100]!=20 || !suite(p+101,4))return Ignore;
    if(havePMKID && !equal(p+105,expectedPMKID,16))return fail("PMKID da mensagem 1 não corresponde à PMK.");
   }
   if(state>=3){
    if(replay<=replay3 || replay<=groupReplay || equal(p+17,anonce,32))return Ignore;
    // M1 is unauthenticated. Retain the established keys until a valid M3;
    // a timed-out candidate can be discarded without tearing down the link.
    if(!Crypto::random(snonce,32))return fail("Falha ao gerar nonce de renovação.");
    copy(anonce,p+17,32);if(!derive())return fail("Falha na derivação da nova PTK.");
    pairRekey=true;state=2;
   }
   if(state==2 && (replay<replay1 || !equal(p+17,anonce,32)))return Ignore;
   if(state==1){copy(anonce,p+17,32);if(!derive())return fail("Falha na derivação da PTK.");}
   version=ver;
   replay1=replay;m2Length=response(m2,p,true);if(!m2Length)return fail("Falha no MIC da mensagem 2.");
   copy(output,m2,m2Length);outputLength=m2Length;state=2;return Reply;
  }
  if(state>=3 && kind==0x1380){
   if(replay<groupReplay || replay<=replay3)return Ignore;
   if(replay==groupReplay){
    if(n!=savedGroupLength || !equal(p,savedGroup,n))return Ignore;
    copy(output,groupAck,groupAckLength);outputLength=groupAckLength;return Reply;
   }
   uint8_t check[512]{},plain[384]{},candidate[keyPayloadLength]{};copy(check,p,n);wipe(check+81,16);
   bool verified=mic(check,n) && equal(check+81,p+81,16);wipe(check,sizeof(check));
   if(!verified)return Ignore;
   size_t encrypted=be16(p+97);
   if(!unwrap(p+99,encrypted,plain)){wipe(plain,sizeof(plain));return fail("GTK de renovação inválida.");}
   bool haveRSN=false,haveGTK=false;
   copy(candidate+50,p+65,6);candidate[48]=groupCipher;
   const bool valid=parseKeyData(plain,encrypted-8,candidate,haveRSN,haveGTK,false);
   if(!valid || !haveGTK){wipe(candidate,sizeof(candidate));wipe(plain,sizeof(plain));return fail("KDE de GTK inválida.");}
   const size_t gtkBytes=groupCipher==cipherTKIP?32:16;
   const bool sameKey=equal(currentGTK,candidate+16,gtkBytes);
   if(sameKey && currentGTKIndex!=candidate[49]){wipe(candidate,sizeof(candidate));wipe(plain,sizeof(plain));return fail("GTK reutilizada em outro índice.");}
   groupAckLength=response(groupAck,p,false,true);
   if(!groupAckLength){wipe(candidate,sizeof(candidate));wipe(plain,sizeof(plain));return fail("Falha no MIC de renovação GTK.");}
   copy(savedGroup,p,n);savedGroupLength=n;groupReplay=replay;
   copy(output,groupAck,groupAckLength);outputLength=groupAckLength;
   if(!sameKey){copy(keys,candidate,keyPayloadLength);copy(currentGTK,candidate+16,gtkBytes);currentGTKIndex=candidate[49];}
   wipe(candidate,sizeof(candidate));wipe(plain,sizeof(plain));return sameKey?Reply:GroupKeys;
  }
  if(kind!=0x13c8){
   // Unsupported descriptors cannot trigger key replacement.
   return Ignore;
  }
  if(state<2 || be16(p+7)!=16 || !equal(p+17,anonce,32))return Ignore;
  if(state>=3){
   if(replay==replay3 && n==savedLength && equal(p,savedM3,n)){
    copy(output,m4,m4Length);outputLength=m4Length;return Reply;
   }
   // Authenticators may increment replay on a retry. Verify its MIC and
   // identical encrypted key data/RSC; acknowledge without reinstalling keys.
   if(replay>replay3 && n==savedLength && equal(p+65,savedM3+65,8) && equal(p+97,savedM3+97,n-97)){
    uint8_t check[512]{};copy(check,p,n);wipe(check+81,16);
    bool verified=mic(check,n) && equal(check+81,p+81,16);wipe(check,sizeof(check));
    if(verified){m4Length=response(m4,p,false);if(!m4Length)return fail("Falha no MIC da resposta repetida.");
     replay3=replay;copy(savedM3,p,n);copy(output,m4,m4Length);outputLength=m4Length;return Reply;}
   }
   return Ignore;
  }
  if(replay<=replay1)return Ignore;
  uint8_t check[512]{},plain[384]{};copy(check,p,n);wipe(check+81,16);
  if(!mic(check,n)){wipe(check,sizeof(check));return fail("Falha criptográfica.");}
  bool verified=equal(check+81,p+81,16);wipe(check,sizeof(check));
  if(!verified)return pairRekey?Ignore:fail("MIC inválido: confira a senha da rede.");
  size_t encrypted=be16(p+97);
  if(!unwrap(p+99,encrypted,plain)){wipe(plain,sizeof(plain));return fail("Dados de chave inválidos.");}
  bool haveRSN=false,haveGTK=false;
  uint8_t candidate[keyPayloadLength]{};copy(candidate,ptk+32,16);copy(candidate+50,p+65,6);candidate[48]=groupCipher;
  const bool valid=parseKeyData(plain,encrypted-8,candidate,haveRSN,haveGTK,true);
  if(!valid||!haveRSN||!haveGTK){wipe(candidate,sizeof(candidate));wipe(plain,sizeof(plain));return fail("RSN ou GTK inválida na mensagem 3.");}
  copy(keys,candidate,sizeof(keys));wipe(candidate,sizeof(candidate));wipe(plain,sizeof(plain));
  m4Length=response(m4,p,false);if(!m4Length)return fail("Falha no MIC da mensagem 4.");
  copy(savedM3,p,n);savedLength=n;replay3=replay;state=3;
  const bool wasRekey=pairRekey;
  copy(establishedPTK,ptk,48);copy(establishedANonce,anonce,32);copy(establishedSNonce,snonce,32);
  copy(currentGTK,keys+16,groupCipher==cipherTKIP?32:16);currentGTKIndex=keys[49];pairRekey=false;
  copy(output,m4,m4Length);outputLength=m4Length;return wasRekey?PairKeys:Keys;
 }
 // PMK is retained only for the live session to permit pairwise rekey.
 // reset()/destruction wipe every key, nonce and cached handshake frame.
};
}
