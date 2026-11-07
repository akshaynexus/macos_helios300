#!/usr/bin/env python3
"""Independent WPA2 test AP: Python hashlib + cryptography RFC3394 wrapping.
The fixture has fixed public credentials/nonces and is never used at runtime.
"""
import hashlib,hmac,struct,sys
from cryptography.hazmat.primitives.keywrap import aes_key_wrap
pmk=hashlib.pbkdf2_hmac('sha1',b'password',b'IEEE',4096,32)
assert pmk.hex()=='f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e'
sta=bytes.fromhex('020102030405');ap=bytes.fromhex('02060708090a')
snonce=bytes(range(32));anonce=bytes(range(32,64))
context=min(sta,ap)+max(sta,ap)+min(snonce,anonce)+max(snonce,anonce)
ptk=b''.join(hmac.digest(pmk,b'Pairwise key expansion\0'+context+bytes([i]),'sha1') for i in range(3))[:48]
rsn=bytes.fromhex('30140100000fac040100000fac040100000fac020000')
gtk=bytes(range(80,96))
kde=bytes.fromhex('dd16000fac010100')+gtk
plain=rsn+kde+b'\xdd\x00'
assert len(plain)%8==0

def packet(info,replay,nonce,data=b'',keylen=16,sign=False):
 p=bytearray(99+len(data));p[0]=2;p[1]=3;p[2:4]=struct.pack('>H',len(p)-4);p[4]=2
 p[5:7]=struct.pack('>H',info);p[7:9]=struct.pack('>H',keylen);p[9:17]=struct.pack('>Q',replay);p[17:49]=nonce
 p[97:99]=struct.pack('>H',len(data));p[99:]=data
 if sign:p[81:97]=hmac.digest(ptk[:16],p,'sha1')[:16]
 return sta+ap+b'\x88\x8e'+p
m1=packet(0x8a,1,anonce)
m3=packet(0x13ca,2,anonce,aes_key_wrap(ptk[16:32],plain),sign=True)
m2=packet(0x10a,1,snonce,rsn,keylen=0,sign=True)
m2=ap+sta+m2[12:]
m4=packet(0x30a,2,bytes(32),keylen=0,sign=True);m4=ap+sta+m4[12:]
wrongrsn=bytearray(plain);wrongrsn[21]=1
bad_rsn=packet(0x13ca,2,anonce,aes_key_wrap(ptk[16:32],bytes(wrongrsn)),sign=True)
# Authenticated group-rekey request: preview must disconnect, not reinstall.
group=packet(0x1382,4,bytes(32),aes_key_wrap(ptk[16:32],plain),sign=True)
retry=packet(0x13ca,3,anonce,aes_key_wrap(ptk[16:32],plain),sign=True)
retry4=packet(0x30a,3,bytes(32),keylen=0,sign=True);retry4=ap+sta+retry4[12:]
with open(sys.argv[1],'w') as out:
 for name,data in dict(pmk=pmk,sta=sta,ap=ap,snonce=snonce,rsn=rsn,gtk=gtk,ptk=ptk,m1=m1,m2=m2,m3=m3,m4=m4,bad_rsn=bad_rsn,group=group,retry=retry,retry4=retry4).items():
  out.write('static const uint8_t v_'+name+'[]={'+','.join(str(x) for x in data)+'};\n')
