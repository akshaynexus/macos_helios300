// SPDX-License-Identifier: GPL-3.0-or-later
// IO80211VirtualInterface / IO80211AWDLProtocol as they exist in IO80211Family
// 1200.13.1 (macOS 26.7.2 25G332). Slot order and names were read from the
// symbol table and vtables of that release's kernel collection (tools/kc-audit.py
// --dump; symbol names and slot indices only, nothing else from Apple binaries is
// reproduced). Unlike the legacy (pre-WCL) header in ../IO80211VirtualInterface.h,
// the 1200.x class derives from IO80211SkywalkInterface.
//
// Facts this layout pins (each checked by kc-audit at every build):
//   IO80211VirtualInterface  505 vtable slots, classSize 0x130 (+8 bytes of ivars)
//   IO80211AWDLProtocol      568 vtable slots, classSize 0x130 (no ivars, 63 pure virtuals)
#ifndef IO80211AWDLProtocol_h
#define IO80211AWDLProtocol_h
#include "../IO80211SkywalkInterface.h"

class IO80211NetworkPacket;
class IO80211Controller;
struct userPrintCtx;
struct __ifnet;

struct apple80211_awdl_af_sec_payload_request;
struct apple80211_awdl_af_tx_mode;
struct apple80211_awdl_bssid;
struct apple80211_awdl_cap;
struct apple80211_awdl_cca;
struct apple80211_awdl_config_request;
struct apple80211_awdl_dev_stats;
struct apple80211_awdl_device_capabilities;
struct apple80211_awdl_dfsp_config;
struct apple80211_awdl_dfsp_ucsa_config;
struct apple80211_awdl_election_id;
struct apple80211_awdl_election_metric;
struct apple80211_awdl_election_params;
struct apple80211_awdl_election_rssi_thresholds;
struct apple80211_awdl_extension_state_machine_parameter;
struct apple80211_awdl_master_channel;
struct apple80211_awdl_min_rate;
struct apple80211_awdl_oob_request;
struct apple80211_awdl_peer_traffic_registration;
struct apple80211_awdl_presence_mode;
struct apple80211_awdl_psf_dwell_params;
struct apple80211_awdl_secondary_master_channel;
struct apple80211_awdl_sync_channel_sequence;
struct apple80211_awdl_sync_enabled;
struct apple80211_awdl_sync_frame_template;
struct apple80211_awdl_sync_params;
struct apple80211_awdl_sync_state;
struct apple80211_awdl_txcal_period;
struct apple80211_bssid_data;
struct apple80211_channel_data;
struct apple80211_companion_skywalk_if_create;
struct apple80211_debug_command;
struct apple80211_encrypt;
struct apple80211_key;
struct apple80211_llw_params;
struct apple80211_p2p_coex_protection;
struct apple80211_peer_cache_control;
struct apple80211_peer_cache_maximum_size;
struct apple80211_rtg_peer_stats_cfg;
struct apple80211_ssid_data;
struct apple80211_state_data;
struct apple80211_tx_rate;
struct awdl_peer_advert_add;
struct ether_addr;
struct packetCounters_t;
struct peerSlotDataStats;

// In the 1200.x kernel the driver-facing awdl0 class is a virtual interface that
// is itself a Skywalk interface (no separate IO80211P2PInterface as in the
// legacy family). IO80211VirtualInterface overrides many IO80211SkywalkInterface
// slots; only the methods this kext calls by qualified name are re-declared,
// inherited slots are patched to the real overrides by kxld/OpenCore at load.
class IO80211VirtualInterface : public IO80211SkywalkInterface {
    OSDeclareAbstractStructors(IO80211VirtualInterface)
public:
    // re-declared overrides of IO80211SkywalkInterface slots (called qualified)
    virtual void free() APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *) APPLE_KEXT_OVERRIDE;
    virtual SInt32 prepareBSDInterface(struct __ifnet *, unsigned int) APPLE_KEXT_OVERRIDE;
    virtual SInt32 setInterfaceEnable(bool) APPLE_KEXT_OVERRIDE;
    // new virtual slots 461..504 of IO80211VirtualInterface
    virtual bool init(IO80211Controller*, ether_addr*, unsigned int, char const*); // slot 461
    virtual void setLinkState(IO80211LinkState, unsigned int); // slot 462
    virtual IOReturn forwardPacket(IO80211NetworkPacket*); // slot 463
    virtual IOReturn handleIoctl(__ifnet*, unsigned long, void*); // slot 464
    virtual void addPeerToCache(unsigned char*); // slot 465
    virtual void deletePeerFromCache(unsigned char*); // slot 466
    virtual IOReturn handleDebugCmd(apple80211_debug_command*); // slot 467
    virtual IOReturn getPeerSlotDataStats(ether_addr*, bool, peerSlotDataStats*); // slot 468
    virtual IOReturn getPeerDataStats(ether_addr*, bool, packetCounters_t*); // slot 469
    virtual void clearPeerDataStats(ether_addr*, bool); // slot 470
    virtual void setAwdlCurrentChannelSequenceIndex(unsigned int); // slot 471
    virtual bool isOutputFlowControlled(); // slot 472
    virtual void setOutputFlowControlled(); // slot 473
    virtual void clearOutputFlowControlled(); // slot 474
    virtual void flushFlowQueues(ether_addr*); // slot 475
    virtual IOReturn configureAQMOutput(); // slot 476
    virtual bool attachToBpf(); // slot 477
    virtual bool configureIfnet(); // slot 478
    virtual void reset(); // slot 479
    virtual IOReturn dupAndTransmitMcastPacket(IO80211NetworkPacket*, unsigned char*, unsigned char*); // slot 480
    virtual void freeMulticastPacket(IO80211NetworkPacket*); // slot 481
    virtual IOReturn triggerAWDLMulticastTx(unsigned int, unsigned char); // slot 482
    virtual IOReturn p2pMulticastTx(IO80211NetworkPacket**, unsigned int); // slot 483
    virtual IOReturn p2pMulticastTx(unsigned int, bool); // slot 484
    virtual void signalMulticastPackets(unsigned int, unsigned int); // slot 485
    virtual IOReturn sendPendingPacketsToStack(IO80211NetworkPacket**, unsigned int); // slot 486
    virtual void freePendingPackets(IO80211NetworkPacket**, unsigned int); // slot 487
    virtual IOReturn setMacAddress(ether_addr&) = 0; // slot 488
    OSMetaClassDeclareReservedUnused(IO80211VirtualInterface, 0); // slot 489
    OSMetaClassDeclareReservedUnused(IO80211VirtualInterface, 1); // slot 490
    OSMetaClassDeclareReservedUnused(IO80211VirtualInterface, 2); // slot 491
    OSMetaClassDeclareReservedUnused(IO80211VirtualInterface, 3); // slot 492
    OSMetaClassDeclareReservedUnused(IO80211VirtualInterface, 4); // slot 493
    OSMetaClassDeclareReservedUnused(IO80211VirtualInterface, 5); // slot 494
    OSMetaClassDeclareReservedUnused(IO80211VirtualInterface, 6); // slot 495
    OSMetaClassDeclareReservedUnused(IO80211VirtualInterface, 7); // slot 496
    OSMetaClassDeclareReservedUnused(IO80211VirtualInterface, 8); // slot 497
    OSMetaClassDeclareReservedUnused(IO80211VirtualInterface, 9); // slot 498
    OSMetaClassDeclareReservedUnused(IO80211VirtualInterface,10); // slot 499
    OSMetaClassDeclareReservedUnused(IO80211VirtualInterface,11); // slot 500
    OSMetaClassDeclareReservedUnused(IO80211VirtualInterface,12); // slot 501
    OSMetaClassDeclareReservedUnused(IO80211VirtualInterface,13); // slot 502
    OSMetaClassDeclareReservedUnused(IO80211VirtualInterface,14); // slot 503
    OSMetaClassDeclareReservedUnused(IO80211VirtualInterface,15); // slot 504
    // non-virtual family functions used here
    SInt32 peerToPeerConfigureIfnet();
private:
    void *mVirtualInterfacePrivate;
};

// The AWDL role (6). The 63 pure virtual slots below are what a driver implements.
class IO80211AWDLProtocol : public IO80211VirtualInterface {
    OSDeclareAbstractStructors(IO80211AWDLProtocol)
public:
    virtual IOReturn getSSID(apple80211_ssid_data*) = 0; // slot 505
    virtual IOReturn getCHANNEL(apple80211_channel_data*) = 0; // slot 506
    virtual IOReturn getBSSID(apple80211_bssid_data*) = 0; // slot 507
    virtual IOReturn getSTATE(apple80211_state_data*) = 0; // slot 508
    virtual IOReturn getAWDL_SYNC_PARAMS(apple80211_awdl_sync_params*) = 0; // slot 509
    virtual IOReturn getAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled*) = 0; // slot 510
    virtual IOReturn getAWDL_EXTENSION_STATE_MACHINE_PARAMETERS(apple80211_awdl_extension_state_machine_parameter*) = 0; // slot 511
    virtual IOReturn getAWDL_ELECTION_ID(apple80211_awdl_election_id*) = 0; // slot 512
    virtual IOReturn getAWDL_ELECTION_METRIC(apple80211_awdl_election_metric*) = 0; // slot 513
    virtual IOReturn getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence*) = 0; // slot 514
    virtual IOReturn getPEER_CACHE_MAXIMUM_SIZE(apple80211_peer_cache_maximum_size*) = 0; // slot 515
    virtual IOReturn getAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel*) = 0; // slot 516
    virtual IOReturn getAWDL_SYNC_STATE(apple80211_awdl_sync_state*) = 0; // slot 517
    virtual IOReturn getAWDL_ELECTION_RSSI_THRESHOLDS(apple80211_awdl_election_rssi_thresholds*) = 0; // slot 518
    virtual IOReturn getAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode*) = 0; // slot 519
    virtual IOReturn getAWDL_ELECTION_PARAMS(apple80211_awdl_election_params*) = 0; // slot 520
    virtual IOReturn getAWDL_DEVICE_CAPABILITIES(apple80211_awdl_device_capabilities*) = 0; // slot 521
    virtual IOReturn getAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel*) = 0; // slot 522
    virtual IOReturn getAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode*) = 0; // slot 523
    virtual IOReturn getAWDL_MIN_RATE(apple80211_awdl_min_rate*) = 0; // slot 524
    virtual IOReturn getAWDL_ENCRYPTION_TYPE(apple80211_encrypt*) = 0; // slot 525
    virtual IOReturn getAWDL_TXCAL_PERIOD(apple80211_awdl_txcal_period*) = 0; // slot 526
    virtual IOReturn getAWDL_DEV_STATS(apple80211_awdl_dev_stats*) = 0; // slot 527
    virtual IOReturn getAWDL_DFSP_CONFIG(apple80211_awdl_dfsp_config*) = 0; // slot 528
    virtual IOReturn getAWDL_CCA(apple80211_awdl_cca*) = 0; // slot 529
    virtual IOReturn getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES(apple80211_awdl_cap*) = 0; // slot 530
    virtual IOReturn getLLW_PARAMS(apple80211_llw_params*) = 0; // slot 531
    virtual IOReturn setSSID(apple80211_ssid_data*) = 0; // slot 532
    virtual IOReturn setCHANNEL(apple80211_channel_data*) = 0; // slot 533
    virtual IOReturn setAWDL_SYNC_PARAMS(apple80211_awdl_sync_params*) = 0; // slot 534
    virtual IOReturn setAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled*) = 0; // slot 535
    virtual IOReturn setAWDL_EXTENSION_STATE_MACHINE_PARAMETERS(apple80211_awdl_extension_state_machine_parameter*) = 0; // slot 536
    virtual IOReturn setAWDL_ELECTION_ID(apple80211_awdl_election_id*) = 0; // slot 537
    virtual IOReturn setAWDL_BSSID(apple80211_awdl_bssid*) = 0; // slot 538
    virtual IOReturn setAWDL_ELECTION_METRIC(apple80211_awdl_election_metric*) = 0; // slot 539
    virtual IOReturn setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence*) = 0; // slot 540
    virtual IOReturn setAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel*) = 0; // slot 541
    virtual IOReturn setAWDL_SYNC_STATE(apple80211_awdl_sync_state*) = 0; // slot 542
    virtual IOReturn setAWDL_ELECTION_RSSI_THRESHOLDS(apple80211_awdl_election_rssi_thresholds*) = 0; // slot 543
    virtual IOReturn setAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode*) = 0; // slot 544
    virtual IOReturn setPEER_CACHE_CONTROL(apple80211_peer_cache_control*) = 0; // slot 545
    virtual IOReturn setAWDL_OOB_REQUEST(apple80211_awdl_oob_request*) = 0; // slot 546
    virtual IOReturn setAWDL_SYNC_FRAME_TEMPLATE(apple80211_awdl_sync_frame_template*) = 0; // slot 547
    virtual IOReturn setAWDL_ELECTION_PARAMS(apple80211_awdl_election_params*) = 0; // slot 548
    virtual IOReturn setAWDL_PEER_TRAFFIC_REGISTRATION(apple80211_awdl_peer_traffic_registration*) = 0; // slot 549
    virtual IOReturn setAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel*) = 0; // slot 550
    virtual IOReturn setAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode*) = 0; // slot 551
    virtual IOReturn setAWDL_MIN_RATE(apple80211_awdl_min_rate*) = 0; // slot 552
    virtual IOReturn setAWDL_ENCRYPTION_KEYS(apple80211_key*) = 0; // slot 553
    virtual IOReturn setAWDL_ENCRYPTION_TYPE(apple80211_encrypt*) = 0; // slot 554
    virtual IOReturn setAWDL_OOB_AUTO_REQUEST(apple80211_awdl_oob_request*) = 0; // slot 555
    virtual IOReturn setAWDL_TXCAL_PERIOD(apple80211_awdl_txcal_period*) = 0; // slot 556
    virtual IOReturn setAWDL_AWDL_ADVERTISERS(awdl_peer_advert_add*) = 0; // slot 557
    virtual IOReturn setAWDL_DFSP_CONFIG(apple80211_awdl_dfsp_config*) = 0; // slot 558
    virtual IOReturn setAWDL_DFSP_UCSA_CONFIG(apple80211_awdl_dfsp_ucsa_config*) = 0; // slot 559
    virtual IOReturn setCOMPANION_SKYWALK_IF_CREATE(apple80211_companion_skywalk_if_create*) = 0; // slot 560
    virtual IOReturn setAWDL_OOB_SEC_PAYLOAD_REQUEST(apple80211_awdl_af_sec_payload_request*) = 0; // slot 561
    virtual IOReturn setLLW_PARAMS(apple80211_llw_params*) = 0; // slot 562
    virtual IOReturn setTX_RATE(apple80211_tx_rate*) = 0; // slot 563
    virtual IOReturn setP2P_COEX_PROTECTION(apple80211_p2p_coex_protection*) = 0; // slot 564
    virtual IOReturn setPSF_DWELL(apple80211_awdl_psf_dwell_params*) = 0; // slot 565
    virtual IOReturn setAWDL_CONFIG(apple80211_awdl_config_request*) = 0; // slot 566
    virtual IOReturn setAWDL_RTG_PEER_STATS_CONFIG(apple80211_rtg_peer_stats_cfg*) = 0; // slot 567
};

#endif /* IO80211AWDLProtocol_h */
