#pragma once
#include "Ath10kHTC.hpp"
namespace Ath10k {
// Firmware RX supplies multicast; enforce the Ethernet subscription at delivery.
// Above the bounded table capacity, pass all multicast to the network stack.
struct MulticastFilter {
    static constexpr UInt32 capacity=64;
    UInt8 addresses[capacity][6]{};UInt32 count=0;
    bool enabled=true,all=true;UInt32 dropped=0;
    bool configure(const UInt8 *list,UInt32 n){
        if(n && !list)return false;
        for(UInt32 i=0;i<n;++i)if(!(list[6*i]&1))return false;
        all=n>capacity;count=all?0:n;
        for(UInt32 i=0;i<count;++i)for(UInt32 j=0;j<6;++j)addresses[i][j]=list[6*i+j];
        return true;
    }
    bool accepts(const UInt8 *address) const {
        if(!(address[0]&1))return true;
        bool broadcast=true;for(UInt32 j=0;j<6;++j)if(address[j]!=255)broadcast=false;
        if(broadcast)return true;
        if(!enabled)return false;
        if(all)return true;
        for(UInt32 i=0;i<count;++i){bool equal=true;for(UInt32 j=0;j<6;++j)if(address[j]!=addresses[i][j])equal=false;
            if(equal)return true;}
        return false;
    }
};
}
