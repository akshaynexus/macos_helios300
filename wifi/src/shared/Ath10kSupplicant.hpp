#pragma once
#include "Ath10kSecurity.hpp"
namespace AthSecurity {
// Crypto supplies HMAC-SHA1 and AES-128-ECB decrypt (CommonCrypto on macOS).
// Limited RSN/PSK, CCMP pairwise, CCMP or TKIP group, descriptor version 2.
// No EAP, TKIP pairwise or PMF. Group and pairwise rekey retain replay protection.
// Retransmitted M3 returns cached M4; never asks the driver to reinstall a key.
template<class Crypto> class Supplicant {
 uint8_t pmk[32]{},ptk[48]{},snonce[32]{},anonce[32]{},local[6]{},ap[6]{};
 uint8_t apRSN[257]{},savedM3[512]{},m2[160]{},m4[128]{};
 size_t rsnLength=0,savedLength=0,m2Length=0,m4Length=0;
 uint64_t replay1=0,replay3=0;
 unsigned state=0;
 uint8_t establishedPTK[48]{},establishedANonce[32]{},establishedSNonce[32]{},currentGTK[32]{};
 uint8_t savedGroup[512]{},groupAck[128]{};size_t savedGroupLength=0,groupAckLength=0;
 uint64_t groupReplay=0;uint8_t currentGTKIndex=0,groupCipher=0;bool pairRekey=false;
 bool derive(){
  static constexpr char label[]="Pairwise key expansion";
  uint8_t input[sizeof(label)+76+1]{},digest[20]{};
  copy(input,(const uint8_t *)label,sizeof(label));size_t at=sizeof(label);
  auto less=[](const uint8_t *a,const uint8_t *b,size_t n){for(size_t i=0;i<n;++i){if(a[i]!=b[i])return a[i]<b[i];}return false;};
  bool swap=less(local,ap,6);copy(input+at,swap?local:ap,6);copy(input+at+6,swap?ap:local,6);at+=12;
  swap=less(snonce,anonce,32);copy(input+at,swap?snonce:anonce,32);copy(input+at+32,swap?anonce:snonce,32);
  for(unsigned block=0;block<3;++block){input[sizeof(input)-1]=uint8_t(block);
   if(!Crypto::hmac(pmk,32,input,sizeof(input),digest)){wipe(input,sizeof(input));wipe(digest,sizeof(digest));return false;}
   copy(ptk+20*block,digest,block==2?8:20);
  }
  wipe(input,sizeof(input));wipe(digest,sizeof(digest));return true;
 }
 bool mic(uint8_t *p,size_t n){uint8_t digest[20]{};bool ok=Crypto::hmac(ptk,16,p,n,digest);if(ok)copy(p+81,digest,16);wipe(digest,20);return ok;}
 size_t response(uint8_t *frame,const uint8_t *request,bool second,bool group=false){
  size_t rsnBytes=0;const uint8_t *rsn=stationRSNForGroup(groupCipher,rsnBytes);
  if(second&&!rsn)return 0;
  size_t n=99+(second?rsnBytes:0);wipe(frame,n+14);copy(frame,ap,6);copy(frame+6,local,6);frame[12]=0x88;frame[13]=0x8e;
  uint8_t *p=frame+14;p[0]=request[0];p[1]=3;put16(p+2,uint16_t(n-4));p[4]=2;
  put16(p+5,second?0x010a:(group?0x0302:0x030a));copy(p+9,request+9,8);
  if(second){copy(p+17,snonce,32);put16(p+97,uint16_t(rsnBytes));copy(p+99,rsn,rsnBytes);}
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
public:
 enum Result { Ignore, Reply, Keys, Fatal, GroupKeys, PairKeys };
 static constexpr size_t keyPayloadLength=56;
 // PTK[16], GTK[32], group cipher[1], GTK index[1], GTK RSC[6].
 uint8_t output[160]{},keys[keyPayloadLength]{};size_t outputLength=0;
 const char *error="none";
 bool pairwisePending() const {return pairRekey && state==2;}
 void cancelPairwiseAttempt(){
  if(!pairwisePending())return;
  copy(ptk,establishedPTK,48);copy(anonce,establishedANonce,32);copy(snonce,establishedSNonce,32);
  pairRekey=false;state=3;outputLength=0;
 }
 ~Supplicant(){reset();}
 void reset(){wipe(this,sizeof(*this));error="none";}
 bool begin(const uint8_t *master,const uint8_t *nonce,const uint8_t *station,const uint8_t *bssid,const uint8_t *rsn,size_t n){
  reset();if(!supportsWPA2(rsn,n))return false;
  groupCipher=wpa2GroupCipher(rsn,n);
  copy(pmk,master,32);copy(snonce,nonce,32);copy(local,station,6);copy(ap,bssid,6);copy(apRSN,rsn,n);rsnLength=n;state=1;return true;
 }
 Result fail(const char *reason){error=reason;return Fatal;}
 Result receive(const uint8_t *frame,size_t length){
  outputLength=0;
  if(!state||length<113||length>512||!equal(frame,local,6)||!equal(frame+6,ap,6)||frame[12]!=0x88||frame[13]!=0x8e)return Ignore;
  const uint8_t *p=frame+14;size_t n=size_t(be16(p+2))+4;
  if((p[0]!=1 && p[0]!=2)||p[1]!=3||p[4]!=2||n<99||n>length-14||size_t(be16(p+97))+99!=n)return Ignore;
  uint16_t info=be16(p+5);uint64_t replay=be64(p+9);
  if(info==0x008a){
   if(be16(p+7)!=16)return Ignore;
   uint8_t nz=0;for(unsigned i=0;i<32;++i)nz|=p[17+i];if(!nz)return Ignore;
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
   replay1=replay;m2Length=response(m2,p,true);if(!m2Length)return fail("Falha no MIC da mensagem 2.");
   copy(output,m2,m2Length);outputLength=m2Length;state=2;return Reply;
  }
  if(state>=3 && info==0x1382){
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
   size_t bytes=encrypted-8,at=0;bool haveGTK=false,valid=true;
   copy(candidate+50,p+65,6);candidate[48]=groupCipher;
   while(at<bytes){
    if(plain[at]==221 && (at+1==bytes || plain[at+1]==0)){
     for(size_t j=at+1;j<bytes;++j)if(plain[j])valid=false;
     break;
    }
    if(bytes-at<2 || size_t(plain[at+1])+2>bytes-at){valid=false;break;}
    size_t len=size_t(plain[at+1])+2;
    if(plain[at]==48 && (len!=rsnLength || !equal(plain+at,apRSN,len)))valid=false;
    if(plain[at]==221 && len>=6 && suite(plain+at+2,1)){
     const size_t gtkBytes=groupCipher==cipherTKIP?32:16;
     if(haveGTK || len!=8+gtkBytes || (plain[at+6]&0xf8) || plain[at+7])valid=false;
     else {candidate[49]=plain[at+6]&3;copy(candidate+16,plain+at+8,gtkBytes);}haveGTK=true;
    }
    at+=len;
   }
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
  if(info!=0x13ca){
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
  size_t bytes=encrypted-8,at=0;bool haveRSN=false,haveGTK=false,valid=true;
  uint8_t candidate[keyPayloadLength]{};copy(candidate,ptk+32,16);copy(candidate+50,p+65,6);candidate[48]=groupCipher;
  while(at<bytes){
   if(plain[at]==221 && (at+1==bytes || plain[at+1]==0)){
    for(size_t j=at+1;j<bytes;++j)if(plain[j])valid=false;
    break;
   }
   if(bytes-at<2||size_t(plain[at+1])+2>bytes-at){valid=false;break;}
   size_t len=plain[at+1]+2;
   if(plain[at]==48){if(haveRSN||len!=rsnLength||!equal(plain+at,apRSN,len))valid=false;haveRSN=true;}
   if(plain[at]==221 && len>=6 && suite(plain+at+2,1)){
    const size_t gtkBytes=groupCipher==cipherTKIP?32:16;
    if(haveGTK||len!=8+gtkBytes||(plain[at+6]&0xf8)||plain[at+7])valid=false;
    else {candidate[49]=plain[at+6]&3;copy(candidate+16,plain+at+8,gtkBytes);}haveGTK=true;
   }
   at+=len;
  }
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
