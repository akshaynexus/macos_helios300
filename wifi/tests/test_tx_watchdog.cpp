#define ATH10K_AP_MAINTENANCE_TEST
#include "test_connection.cpp"
#include <cstdio>

class WatchdogDriver : public ConnectionDriver {
public:
    UInt8 frame[64]{};
    WatchdogDriver(){
        connect();assert(networkUp && _wmiLive.active);
        _regs.tx4Consume=[](){};frame[12]=8;
        for(unsigned i=0;i<Ath10k::kDataTxSlots;++i){
            auto &b=_dataBuffers[i];b.payload.isAllocated=b.command.isAllocated=b.fragments.isAllocated=false;
            b.payload.iovm=0x2000000+i*0x10000;b.command.iovm=b.payload.iovm+0x1000;b.fragments.iovm=b.payload.iovm+0x2000;
        }
    }
    UInt16 submit(UInt64 now){assert(!submitDataPacket(frame,sizeof(frame),now));return _connection.txId;}
    void complete(UInt16 id){auto p=completion(id);assert(!Ath10k::parseHTTSetup(p.data(),p.size(),1,_wmi.credits,_rxSetup,&_connection));}
    void fill(){for(unsigned i=0;i<Ath10k::kDataTxSlots;++i)submit(1800);assert(!serviceDataTX(1810));}
};

int main(){
    // Firmware retains one payload while newer traffic completes normally.
    // Age alone must not reset the driver, free DMA, or reuse this ID/buffer.
    {
        WatchdogDriver d;const auto old=d.submit(1800);assert(!d.serviceDataTX(1810));
        const auto retainedAddress=d._dataBuffers[0].payload.dmaAddress();
        assert(!d.serviceDataTX(7000));assert(d._connection.dataPending==1);
        assert(d._connection.dataTX[0].owned && d._connection.dataTX[0].ceConsumed && !d._connection.dataTX[0].complete);
        assert(d._connection.dataDelayed==1 && !d._connection.dataTimeouts);
        const auto fresh=d.submit(7010);assert(fresh!=old);d.complete(fresh);
        assert(!d.serviceDataTX(7020) && d._connection.dataPending==1 && d._connection.txPackets>0);
        assert(d._dataBuffers[0].payload.dmaAddress()==retainedAddress && d._connection.dataTX[0].owned);
        assert(!d.serviceDataTX(60000) && d._connection.dataDelayed==1);
        d.complete(old);assert(!d.serviceDataTX(60010));assert(!d._connection.dataPending);
        assert(!d._connection.dataStallArmed && !d._connection.dataPauseArmed && !d._connection.dataTimeouts);
        d.releaseWMIRuntime();
    }
    // A CE transport stall still fails at five seconds, including during scan.
    for(bool scanning:{false,true}){
        WatchdogDriver d;d._regs.tx4Timeout=true;d.submit(1800);
        d._wmiScan.completed=!scanning;
        assert(!d.serviceDataTX(6799));
        assert(!strcmp(d.serviceDataTX(6800),"data-tx-completion-timeout"));
        assert(d._connection.dataPending==1 && d._connection.dataTX[0].owned && !d._connection.dataTX[0].ceConsumed);
        assert(d._connection.dataTimeouts==1);d.releaseWMIRuntime();
    }
    // HTT alone never releases a buffer whose command is still owned by CE.
    {
        WatchdogDriver d;d._regs.tx4Timeout=true;const auto id=d.submit(1800);d.complete(id);
        assert(!d.serviceDataTX(1810) && d._connection.dataPending==1);
        assert(d.serviceDataTX(6800) && d._connection.dataTX[0].owned);
        d.releaseWMIRuntime();
    }
    // Full queue with no progress keeps the five-second recovery bound.
    {
        WatchdogDriver d;d.fill();assert(d._connection.dataStallArmed);
        assert(!d.serviceDataTX(6809));
        assert(!strcmp(d.serviceDataTX(6810),"data-tx-completion-timeout"));
        assert(d._connection.dataPending==Ath10k::kDataTxSlots && d._connection.dataTimeouts==1);
        for(const auto &slot:d._connection.dataTX)assert(slot.owned && !slot.complete);
        d.releaseWMIRuntime();
    }
    // Genuine completions drain/wake the queue even if its first ID is old.
    {
        WatchdogDriver d;d.fill();const auto old=d._connection.dataTX[0].id;
        d.queueEnabled=true;d._dataTXStalled=true;
        for(unsigned i=1;i<Ath10k::kDataTxSlots;++i)d.complete(d._connection.dataTX[i].id);
        assert(!d.serviceDataTX(6810));assert(d._connection.dataPending==1 && d.queue.wakes==1);
        d._connection.txId=static_cast<UInt16>((old+1055)%1056);
        assert(d.submit(6820)!=old && d._connection.dataTX[0].owned);
        assert(!d.serviceDataTX(6830) && !d._connection.dataStallArmed);
        d.releaseWMIRuntime();
    }
    // Legitimate firmware/scan pauses defer the firmware queue watchdog only.
    // A stuck pause is bounded; resume receives a fresh stall window.
    for(bool scanning:{false,true}){
        WatchdogDriver d;d.fill();
        if(scanning)d._wmiScan.completed=false;else d._connection.txPauseMask=2;
        assert(!d.serviceDataTX(2000) && d._connection.dataPauseArmed && !d._connection.dataStallArmed);
        assert(!d.serviceDataTX(20000));
        d._wmiScan.completed=true;d._connection.txPauseMask=0;
        assert(!d.serviceDataTX(20010) && !d._connection.dataPauseArmed);
        assert(!d.serviceDataTX(25009));assert(d.serviceDataTX(25010));d.releaseWMIRuntime();
        WatchdogDriver hung;hung.fill();
        if(scanning)hung._wmiScan.completed=false;else hung._connection.txPauseMask=2;
        const UInt64 deadline=2000+(scanning?Ath10k::kWMIScanHostMs:30000);
        assert(!hung.serviceDataTX(2000));assert(!hung.serviceDataTX(deadline-1));
        assert(!strcmp(hung.serviceDataTX(deadline),"data-tx-pause-timeout"));
        assert(hung._connection.dataPending==Ath10k::kDataTxSlots);hung.releaseWMIRuntime();
    }
    // Teardown cannot pretend that a retained firmware payload has completed.
    {
        WatchdogDriver d;d.submit(1800);assert(!d.serviceDataTX(1810));
        assert(Ath10k::requestDisconnect(d._connection,2000));
        assert(d.serviceDataTX(6800) && d._connection.dataTX[0].owned && d._connection.dataPending==1);
        d.releaseWMIRuntime();
    }
    std::puts("TX watchdog passed: late HTT ownership, progressing traffic, CE stalls, full-queue stalls, bounded pauses, ID retention, wakeup and teardown.");
}
