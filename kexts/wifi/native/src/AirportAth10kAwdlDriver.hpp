// SPDX-License-Identifier: GPL-3.0-or-later
// awdl0 on macOS 26 (IO80211Family 1200.13.1), behind AIRPORT_ATH10K_AWDL and the boot-arg
// ath10k_awdl=1. Two objects, the same shape as the infrastructure interface:
//  - AirportAth10kAwdlInterface, the IO80211AWDLProtocol subclass the family talks to
//    (role 6, its ioctls and its peer manager);
//  - AirportAth10kAwdlNet, an IOEthernetController whose BSD interface is the awdl0
//    ifnet: the same hybrid model as en1 (IOEthernetInterface with the output pull
//    model), not a native Skywalk nexus with packet rings. See docs/WIFI-AWDL.md.
// The AWDL MAC layer runs in this driver (AirportAth10kAwdl.hpp); Apple's peer manager
// (the family's IO80211AWDLPeerManager, created for role 6) sees the received action
// frames through bpfTapInput and supplies the service records through OOB requests.
#pragma once
#include "AirportAth10kDefines.hpp"
#if AIRPORT_ATH10K_AWDL
#include <Airport/Apple80211.h>
#include <Airport/macOS26/IO80211AWDLProtocol.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include "AirportAth10kAwdlSession.hpp"
#include "AirportAth10kRegistration.hpp"

class AirportAth10k;
class AirportAth10kAwdlNet;
// What the macOS-side integration is allowed to do; the boot-arg ath10k_awdl_stack overrides
// the default so each piece can be turned off on a run that misbehaves (docs/WIFI-AWDL.md).
enum : uint32_t {
    kAwdlStackPeerManagerRx = 1<<0,  // feed action frames to the family's peer manager
    kAwdlStackNeighbors     = 1<<1,  // create IPv6 neighbor entries for valid peers (NET-2)
    kAwdlStackServiceTlvs   = 1<<2,  // take the service records from the OOB frames
    kAwdlStackLinkState     = 1<<3,  // report the link through IO80211VirtualInterface::setLinkState
    kAwdlStackDefault       = kAwdlStackPeerManagerRx|kAwdlStackNeighbors|kAwdlStackServiceTlvs,
};
struct apple80211_virt_if_create_data;

class AirportAth10kAwdlInterface : public IO80211AWDLProtocol {
    OSDeclareDefaultStructors(AirportAth10kAwdlInterface)
public:
    bool initWith(AirportAth10kAwdlNet *owner,AirportAth10k *controller,const uint8_t *mac);
    void free() override;
    void stop(IOService *) override;
    bool installRegistrationInfo(const IOSkywalkEthernetInterface::RegistrationInfo &);
    void releaseRegistrationInfo();
    const char *getBSDNamePrefix() override { return "awdl"; }
    IOReturn setMacAddress(ether_addr&) override { return kIOReturnUnsupported; }
    IOReturn handleDebugCmd(apple80211_debug_command*) override { return kIOReturnUnsupported; }
    AirportAwdl::Session *session() const { return awdlSession; }
#ifdef AIRPORT_ATH10K_AWDL_TEST
    IOTimerEventSource *awdlTestTimerForFakes() const { return timer; }
#endif
    void startSession();
    void stopSession();
    void kick();
    uint32_t stackFeatures() const { return stack; }
    // RadioPort and HostPort (AirportAth10kAwdlDriver.cpp).
    struct Port : AirportAwdl::RadioPort, AirportAwdl::HostPort {
        AirportAth10kAwdlInterface *owner=nullptr;
        bool radioStart(const uint8_t *,uint8_t,bool) override;
        void radioStop() override;
        bool radioSetChannel(uint8_t) override;
        bool radioTransmit(const uint8_t *,size_t,bool) override;
        bool radioReceive(uint8_t *,size_t,AirportAwdl::RxFrame &) override;
        AirportAwdl::RadioStep radioStep() override;
        void radioStation(AirportAwdl::StationInfo &) override;
        void hostActionFrame(const uint8_t *,size_t,uint8_t,int8_t) override;
        void hostEthernet(const uint8_t *,size_t) override;
        void hostPeer(const AirportAwdl::Peer &,bool) override;
        void hostSyncChanged() override;
        bool control(uint32_t op,const void *data,uint32_t length,AthControl::Response *out);
    } port;
private:
    static void pollTimer(OSObject *,IOTimerEventSource *);
    uint64_t nowUs() const;
    void mirror();
    IOReturn handleOob(apple80211_awdl_oob_request *,bool autoRequest);
    void feedPeerManager(const uint8_t *frame,size_t length,uint8_t channel,int8_t rssi);
    void neighbor(const AirportAwdl::Peer &,bool present);
    AirportAth10kAwdlNet *net=nullptr;
    AirportAth10k *controller=nullptr;
    AirportAwdl::Session *awdlSession=nullptr;
    IOTimerEventSource *timer=nullptr;
    AirportRegistration::Blocks registration{};
    uint8_t macAddress[6]{};
    uint32_t bssidValid=0;uint32_t syncEnabled=0,electionId=0;
    uint32_t stack=kAwdlStackDefault;
    uint32_t ioctlCalls[63]{};bool ioctlDirty=false;
    apple80211_awdl_sync_channel_sequence *lastChannelSequence=nullptr; // what the family last set
    uint32_t afFed=0,afFedDropped=0,oobCalls=0,oobTx=0;
    uint32_t mirroredPeers=0xffffffffu,mirroredState=0xffffffffu,mirroredPlacement=0xffffffffu,mirrorTicks=0;
    bool stopping=false;
public:
    // The 63 pure virtual slots of IO80211AWDLProtocol. Implemented where the layout
    // is known (apple80211_ioctl.h); the rest are refused with Unsupported.
    IOReturn getSSID(apple80211_ssid_data*) override;
    IOReturn getCHANNEL(apple80211_channel_data*) override;
    IOReturn getBSSID(apple80211_bssid_data*) override;
    IOReturn getSTATE(apple80211_state_data*) override;
    IOReturn getAWDL_SYNC_PARAMS(apple80211_awdl_sync_params*) override;
    IOReturn getAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled*) override;
    IOReturn getAWDL_ELECTION_ID(apple80211_awdl_election_id*) override;
    IOReturn getAWDL_ELECTION_METRIC(apple80211_awdl_election_metric*) override;
    IOReturn getPEER_CACHE_MAXIMUM_SIZE(apple80211_peer_cache_maximum_size*) override;
    IOReturn getAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel*) override;
    IOReturn getAWDL_SYNC_STATE(apple80211_awdl_sync_state*) override;
    IOReturn getAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode*) override;
    IOReturn setSSID(apple80211_ssid_data*) override;
    IOReturn setCHANNEL(apple80211_channel_data*) override;
    IOReturn setAWDL_SYNC_PARAMS(apple80211_awdl_sync_params*) override;
    IOReturn setAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled*) override;
    IOReturn setAWDL_ELECTION_ID(apple80211_awdl_election_id*) override;
    IOReturn setAWDL_BSSID(apple80211_awdl_bssid*) override;
    IOReturn setAWDL_ELECTION_METRIC(apple80211_awdl_election_metric*) override;
    IOReturn setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence*) override;
    IOReturn setAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel*) override;
    IOReturn setAWDL_SYNC_STATE(apple80211_awdl_sync_state*) override;
    IOReturn setAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode*) override;
    // Accepted and recorded; see AirportAth10kAwdlDriver.cpp for what each does and docs/WIFI-AWDL.md for why.
    IOReturn getAWDL_EXTENSION_STATE_MACHINE_PARAMETERS(apple80211_awdl_extension_state_machine_parameter*) override;
    IOReturn getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence*) override;
    IOReturn getAWDL_ELECTION_RSSI_THRESHOLDS(apple80211_awdl_election_rssi_thresholds*) override;
    IOReturn getAWDL_ELECTION_PARAMS(apple80211_awdl_election_params*) override;
    IOReturn getAWDL_DEVICE_CAPABILITIES(apple80211_awdl_device_capabilities*) override;
    IOReturn getAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel*) override;
    IOReturn getAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode*) override;
    IOReturn getAWDL_MIN_RATE(apple80211_awdl_min_rate*) override;
    IOReturn getAWDL_ENCRYPTION_TYPE(apple80211_encrypt*) override;
    IOReturn getAWDL_TXCAL_PERIOD(apple80211_awdl_txcal_period*) override;
    IOReturn getAWDL_DEV_STATS(apple80211_awdl_dev_stats*) override;
    IOReturn getAWDL_DFSP_CONFIG(apple80211_awdl_dfsp_config*) override;
    IOReturn getAWDL_CCA(apple80211_awdl_cca*) override;
    IOReturn getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES(apple80211_awdl_cap*) override;
    IOReturn getLLW_PARAMS(apple80211_llw_params*) override;
    IOReturn setAWDL_EXTENSION_STATE_MACHINE_PARAMETERS(apple80211_awdl_extension_state_machine_parameter*) override;
    IOReturn setAWDL_ELECTION_RSSI_THRESHOLDS(apple80211_awdl_election_rssi_thresholds*) override;
    IOReturn setPEER_CACHE_CONTROL(apple80211_peer_cache_control*) override;
    IOReturn setAWDL_OOB_REQUEST(apple80211_awdl_oob_request*) override;
    IOReturn setAWDL_SYNC_FRAME_TEMPLATE(apple80211_awdl_sync_frame_template*) override;
    IOReturn setAWDL_ELECTION_PARAMS(apple80211_awdl_election_params*) override;
    IOReturn setAWDL_PEER_TRAFFIC_REGISTRATION(apple80211_awdl_peer_traffic_registration*) override;
    IOReturn setAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel*) override;
    IOReturn setAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode*) override;
    IOReturn setAWDL_MIN_RATE(apple80211_awdl_min_rate*) override;
    IOReturn setAWDL_ENCRYPTION_KEYS(apple80211_key*) override;
    IOReturn setAWDL_ENCRYPTION_TYPE(apple80211_encrypt*) override;
    IOReturn setAWDL_OOB_AUTO_REQUEST(apple80211_awdl_oob_request*) override;
    IOReturn setAWDL_TXCAL_PERIOD(apple80211_awdl_txcal_period*) override;
    IOReturn setAWDL_AWDL_ADVERTISERS(awdl_peer_advert_add*) override;
    IOReturn setAWDL_DFSP_CONFIG(apple80211_awdl_dfsp_config*) override;
    IOReturn setAWDL_DFSP_UCSA_CONFIG(apple80211_awdl_dfsp_ucsa_config*) override;
    IOReturn setCOMPANION_SKYWALK_IF_CREATE(apple80211_companion_skywalk_if_create*) override;
    IOReturn setAWDL_OOB_SEC_PAYLOAD_REQUEST(apple80211_awdl_af_sec_payload_request*) override;
    IOReturn setLLW_PARAMS(apple80211_llw_params*) override;
    IOReturn setTX_RATE(apple80211_tx_rate*) override;
    IOReturn setP2P_COEX_PROTECTION(apple80211_p2p_coex_protection*) override;
    IOReturn setPSF_DWELL(apple80211_awdl_psf_dwell_params*) override;
    IOReturn setAWDL_CONFIG(apple80211_awdl_config_request*) override;
    IOReturn setAWDL_RTG_PEER_STATS_CONFIG(apple80211_rtg_peer_stats_cfg*) override;
};

class AirportAth10kAwdlBsd : public IOEthernetInterface {
    OSDeclareDefaultStructors(AirportAth10kAwdlBsd)
public:
    bool initWithSkywalkInterface(IONetworkController *,IO80211SkywalkInterface *);
    IOReturn attachToDataLinkLayer(IOOptionBits,void *) override;
    void detachFromDataLinkLayer(IOOptionBits,void *) override;
    IOService *getProvider() const override;
    const char *getNamePrefix() const override { return "awdl"; }
private:
    static IOReturn prepareGated(OSObject *,void *,void *,void *,void *);
    IO80211SkywalkInterface *interface=nullptr;
    bool attached=false;
};

class AirportAth10kAwdlNet : public IOEthernetController {
    OSDeclareDefaultStructors(AirportAth10kAwdlNet)
public:
    bool initWith(AirportAth10k *controller);
    bool start(IOService *) override;
    void stop(IOService *) override;
    void free() override;
    IOWorkLoop *getWorkLoop() const override;
    IONetworkInterface *createInterface() override;
    bool configureInterface(IONetworkInterface *) override;
    IOReturn getHardwareAddress(IOEthernetAddress *) override;
    IOReturn setHardwareAddress(const void *,UInt32) override;
    IOReturn getPacketFilters(const OSSymbol *,UInt32 *) const override;
    IOReturn setPromiscuousMode(bool) override { return kIOReturnSuccess; }
    IOReturn setMulticastMode(bool) override { return kIOReturnSuccess; }
    IOReturn setMulticastList(IOEthernetAddress *,UInt32) override { return kIOReturnSuccess; }
    const OSString *newVendorString() const override { return OSString::withCString("Qualcomm Atheros"); }
    const OSString *newModelString() const override { return OSString::withCString("QCA6174 AWDL (experimental)"); }
    IOReturn selectMedium(const IONetworkMedium *m) override { setSelectedMedium(m); return kIOReturnSuccess; }
    IOReturn enable(IONetworkInterface *) override;
    IOReturn disable(IONetworkInterface *) override;
    UInt32 outputPacket(mbuf_t,void *) override;
    // Output pull model: the SDK shows this slot as a pad (see AirportAth10k.hpp).
    void _RESERVEDIONetworkController2() override __asm__("__ZN20AirportAth10kAwdlNet11outputStartEP18IONetworkInterfacej");
    IOReturn outputStart(IONetworkInterface *,IOOptionBits);
    IOReturn registerWithPolicyMaker(IOService *policyMaker) override;
    IOReturn setPowerState(unsigned long,IOService *) override { return IOPMAckImplied; }
    bool setLinkUp(bool up);
    IOReturn fillCreateReply(apple80211_virt_if_create_data *) const;
    void inputEthernet(const uint8_t *eth,size_t length);
    IOCommandGate *commandGate() const { return gate; }
    AirportAth10kAwdlInterface *awdlInterface() const { return awdl; }
    AirportAth10kAwdlBsd *bsdInterface() const { return bsd; }
    const uint8_t *macAddress() const { return mac; }
    bool startedUp() const { return up; }
private:
    AirportAth10k *controller=nullptr;
    AirportAth10kAwdlInterface *awdl=nullptr;
    AirportAth10kAwdlBsd *bsd=nullptr;
    IOCommandGate *gate=nullptr;
    const IONetworkMedium *activeMedium=nullptr;
    uint8_t mac[6]{};
    bool up=false,attached=false,linkUp=false,threadRunning=false,interfaceStarted=false,netStarted=false;
};
#endif // AIRPORT_ATH10K_AWDL
