#pragma once
#include "Ath10kConnection.hpp"
namespace Ath10k {
// Linux v6.12 wmi_pdev_stats (180), wmi_tlv_vdev_stats (164),
// wmi_10x_peer_stats (20). Unknown layouts remain informational, never indexed.
inline bool decodeFirmwareStats(ConnectionState::FirmwareStats &out,const UInt8 *header,UInt32 headerBytes,
                               const UInt8 *data,UInt32 bytes,const UInt8 *bssid){
    if(!header || !data || !bssid || headerBytes!=40)return false;
    const UInt32 pdev=connLE32(header+4),vdev=connLE32(header+8),peers=connLE32(header+12);
    if(pdev>1 || vdev>4 || peers>33 || connLE32(header+16) || connLE32(header+20) || connLE32(header+24) ||
       connLE32(header+28) || connLE32(header+32) || connLE32(header+36) || (connLE32(header)&(1u<<10)))return false;
    const UInt32 expected=180*pdev+164*vdev+20*peers;
    if(bytes!=expected)return false;
    auto next=out;bool found=false;UInt32 offset=0;
    if(pdev){const int32_t noise=static_cast<int32_t>(connLE32(data));
        if(noise>=-130 && noise<=-20){next.noiseValid=true;next.noiseDbm=noise;++next.noiseSerial;found=true;}offset=180;}
    for(UInt32 i=0;i<vdev;++i,offset+=164){const UInt8 *p=data+offset;if(connLE32(p))continue;
        next.vdevValid=true;found=true;
        for(UInt32 ac=0;ac<4;++ac){next.tx[ac]=connLE32(p+12+4*ac);next.retries[ac]=connLE32(p+32+4*ac);next.failures[ac]=connLE32(p+48+4*ac);}
        next.notAcked=connLE32(p+80);}
    for(UInt32 i=0;i<peers;++i,offset+=20){const UInt8 *p=data+offset;if(!sameMAC(p,bssid))continue;
        const UInt32 tx=connLE32(p+12),rx=connLE32(p+16);
        if(tx && tx<=2000000 && rx<=2000000){next.txKbps=tx;next.rxKbps=rx;next.rateValid=true;++next.rateSerial;found=true;}}
    if(found){++next.serial;out=next;}
    return true;
}
}
