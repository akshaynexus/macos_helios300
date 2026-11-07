#define ATH10K_OTP_FLOW_TEST
#include "test_board_flow.cpp"
#include "release_method.inc"
int main(){
    for(unsigned i=0;i<5;++i)for(bool canDisable:{false,true}){
        Ath10kPCIDevice d;for(auto &r:d._wmiRadio)r={};
        d._wmiRadio[i].txOwned=true;d._pci->disableWorks=canDisable;
        assert(d.stopDMAForRelease()==canDisable);
        assert(d._dmaQuarantine && d._wmiRadio[i].txOwned);
        assert(bool(d._pci->command&4)==!canDisable);
        assert(d._wmiTxBuffer.allocated() && d._ceSrcRings[3].allocated());
    }
    Ath10kPCIDevice idle;assert(idle.stopDMAForRelease() && !idle._dmaQuarantine && (idle._pci->command&4));
    puts("DMA radio release passed: all five command owners block unverified release, PCI bus master verified, failed disable retains mappings.");
}
