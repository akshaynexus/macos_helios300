// SPDX-License-Identifier: GPL-3.0-or-later
// AirportAth10kInterface — the IO80211 infrastructure (Skywalk) interface for
// the ath10k core on macOS 26: WCL scan/join/leave commands and the get*
// queries, answered from AirportBridge::Session which polls the core.
//
// Structure and the WCL command/event choreography follow laobamac/itlwm
// AirportItlwm/AirportItlwmSkywalkInterface.* and WCL/WCL.cpp (commit 7204c69,
// Copyright © 2023 钟先耀, © 2026 laobamac, all rights reserved; used with the
// author's permission per the repository owner, 2026-11-08).
#pragma once
#include "AirportAth10kDefines.hpp"
#include <Airport/Apple80211.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOCommandGate.h>
#include "AirportAth10kCrypto.hpp"
#include "AirportAth10kBridge.hpp"
#include "AirportAth10kRegistration.hpp"

class AirportAth10k;
bool airportAth10kRandom(uint8_t *,size_t);
using AirportAth10kCrypto=AirportCrypto::Policy<airportAth10kRandom>;
using AirportAth10kSession=AirportBridge::Session<AirportAth10kCrypto>;

class AirportAth10kInterface : public IO80211InfraProtocol {
    OSDeclareDefaultStructors(AirportAth10kInterface)
public:
    bool init(AirportAth10k *);
    void stop(IOService *) override;
    void free() override;
    // The two Skywalk RegistrationInfo blocks (AirportAth10kRegistration.hpp):
    // installed by the controller before IO80211InfraInterface::start, taken
    // back in stop()/free() before the family's typed free can reach them.
    bool installRegistrationInfo(const IOSkywalkEthernetInterface::RegistrationInfo &);
    void releaseRegistrationInfo();
    // Polling (and with it every postMessage/setLQM) starts only after the
    // controller has attached and started this interface and registered it.
    void startPolling();
    bool polling() const {return started;}
    void stopSession();
    IOReturn setWCL_LINK_STATE_UPDATE(apple80211_wcl_update_link_state *) override;
    IOReturn setMacAddress(mloAddrArray &) override;
    // The family's pure virtuals: implemented ones first, the rest refuse.
    IOReturn getCHANNEL(apple80211_channel_data*) override;
    IOReturn getPOWERSAVE(apple80211_powersave_data*) override;
    IOReturn getTXPOWER(apple80211_txpower_data*) override;
    IOReturn getRATE(apple80211_rate_data*) override;
    IOReturn getOP_MODE(apple80211_opmode_data*) override;
    IOReturn getRSSI(apple80211_rssi_data*) override;
    IOReturn getSUPPORTED_CHANNELS(apple80211_sup_channel_data*) override;
    IOReturn getCHANNELS_INFO(apple80211_channels_info*) override;
    IOReturn getHW_SUPPORTED_CHANNELS(apple80211_sup_channel_data*) override;
    IOReturn getBEACON_INFO(apple80211_beacon_info_t*) override;
    IOReturn getNSS(apple80211_nss_data*) override;
    IOReturn getHW_ADDR(apple80211_hw_mac_address*) override;
    IOReturn getCOUNTRY_CHANNELS_INFO(apple80211_channels_info*) override;
    IOReturn getWCL_LOW_LATENCY_INFO(apple80211_low_latency_info*) override;
    IOReturn getWCL_BSS_INFO(apple80211_beacon_msg*) override;
    IOReturn getWCL_TRAFFIC_COUNTERS(apple80211_wcl_traffic_counters*) override;
    IOReturn getWCL_CHANNELS_INFO(apple80211ChannelInfo*) override;
    IOReturn getWCL_EXTENDED_BSS_INFO(apple80211_extended_bss_info*) override;
    IOReturn setCIPHER_KEY(apple80211_key*) override;
    IOReturn setPOWERSAVE(apple80211_powersave_data*) override;
    IOReturn setCLEAR_PMKSA_CACHE(void*) override;
    IOReturn setWCL_LEAVE_NETWORK(apple80211_leave_network*) override;
    IOReturn setWCL_SCAN_ABORT(void*) override;
    IOReturn setWCL_JOIN_ABORT(apple80211_wcl_abort_join*) override;
    IOReturn setWCL_SCAN_REQ(apple80211ScanRequest*) override;
    IOReturn setWCL_ASSOCIATE(apple80211AssocCandidates*) override;
    IOReturn setWCL_SET_SCAN_HOME_AWAY_TIME(scanHomeAndAwayTime*) override;
    IOReturn setWCL_UPDATE_FAST_LANE(apple80211_fastlane*) override;
    IOReturn getGUARD_INTERVAL(apple80211_guard_interval_data*) override { return kIOReturnUnsupported; }
    IOReturn getMCS(apple80211_mcs_data*) override;
    IOReturn getPOWER_DEBUG_INFO(apple80211_power_debug_info*) override { return kIOReturnUnsupported; }
    IOReturn getHT_CAPABILITY(apple80211_ht_capability*) override { return kIOReturnUnsupported; }
    IOReturn getMCS_VHT(apple80211_mcs_vht_data*) override { return kIOReturnUnsupported; }
    IOReturn getVHT_CAPABILITY(apple80211_vht_capability*) override { return kIOReturnUnsupported; }
    IOReturn getROAM_PROFILE(apple80211_roam_profile_all_bands*) override { return kIOReturnUnsupported; }
    IOReturn getCHIP_COUNTER_STATS(apple80211_chip_stats*) override { return kIOReturnUnsupported; }
    IOReturn getDBG_GUARD_TIME_PARAMS(apple80211_dbg_guard_time_params*) override { return kIOReturnUnsupported; }
    IOReturn getLEAKY_AP_STATS_MODE(apple80211_leaky_ap_setting*) override { return kIOReturnUnsupported; }
    IOReturn getCOUNTRY_CHANNELS(apple80211_country_channel_data*) override { return kIOReturnUnsupported; }
    IOReturn getPRIVATE_MAC(apple80211_private_mac_data*) override { return kIOReturnUnsupported; }
    IOReturn getRANGING_ENABLE(apple80211_ranging_enable_request_t*) override { return kIOReturnUnsupported; }
    IOReturn getRANGING_START(apple80211_ranging_start_request_t*) override { return kIOReturnUnsupported; }
    IOReturn getAWDL_RSDB_CAPS(apple80211_rsdb_capability*) override { return kIOReturnUnsupported; }
    IOReturn getTKO_PARAMS(apple80211_tko_params*) override { return kIOReturnUnsupported; }
    IOReturn getTKO_DUMP(apple80211_tko_dump*) override { return kIOReturnUnsupported; }
    IOReturn getBTCOEX_PROFILE(apple80211_btcoex_profile*) override { return kIOReturnUnsupported; }
    IOReturn getBTCOEX_PROFILE_ACTIVE(apple80211_btcoex_profile_active_data*) override { return kIOReturnUnsupported; }
    IOReturn getTRAP_INFO(apple80211_trap_info_data*) override { return kIOReturnUnsupported; }
    IOReturn getTHERMAL_INDEX(apple80211_thermal_index_t*) override { return kIOReturnUnsupported; }
    IOReturn getMAX_NSS_FOR_AP(apple80211_btcoex_max_nss_for_ap_data*) override { return kIOReturnUnsupported; }
    IOReturn getBTCOEX_2G_CHAIN_DISABLE(apple80211_btcoex_2g_chain_disable*) override { return kIOReturnUnsupported; }
    IOReturn getPOWER_BUDGET(apple80211_power_budget_t*) override { return kIOReturnUnsupported; }
    IOReturn getOFFLOAD_TCPKA_ENABLE(apple80211_offload_tcpka_enable_t*) override { return kIOReturnUnsupported; }
    IOReturn getRANGING_CAPS(apple80211_ranging_capabilities_t*) override { return kIOReturnUnsupported; }
    IOReturn getLQM_CONFIG(apple80211_lqm_config_t*) override { return kIOReturnUnsupported; }
    IOReturn getTRAP_CRASHTRACER_MINI_DUMP(apple80211_trap_mini_dump_data*) override { return kIOReturnUnsupported; }
    IOReturn getCHIP_POWER_RANGE(apple80211_chip_power_limit*) override { return kIOReturnUnsupported; }
    IOReturn getCHIP_DIAGS(appl80211_chip_diags_data*) override { return kIOReturnUnsupported; }
    IOReturn getHP2P_CTRL(apple80211_hp2p_ctrl*) override { return kIOReturnUnsupported; }
    IOReturn getBSS_BLACKLIST(bss_blacklist*) override { return kIOReturnUnsupported; }
    IOReturn getTXRX_CHAIN_INFO(apple80211_txrx_chain_info*) override { return kIOReturnUnsupported; }
    IOReturn getMIMO_STATUS(apple80211_mimo_status*) override { return kIOReturnUnsupported; }
    IOReturn getCUR_PMK(apple80211_pmk*) override { return kIOReturnUnsupported; }
    IOReturn getDYNSAR_DETAIL(apple80211_dynsar_detail*) override { return kIOReturnUnsupported; }
    IOReturn getLQM_SUMMARY(apple80211_lqm_summary*) override { return kIOReturnUnsupported; }
    IOReturn getSLOW_WIFI_FEATURE_ENABLED(apple80211_slow_wifi_feature_enabled*) override { return kIOReturnUnsupported; }
    IOReturn getTIMESYNC_INFO(apple80211_timesync_info*) override { return kIOReturnUnsupported; }
    IOReturn getSENSING_DATA(apple80211_sensing_data_t*) override { return kIOReturnUnsupported; }
    IOReturn getWCL_FW_HOT_CHANNELS(apple80211_fw_hot_channels*) override { return kIOReturnUnsupported; }
    IOReturn getWCL_GET_TX_BLANKING_STATUS(unsigned int*) override { return kIOReturnUnsupported; }
    IOReturn getHE_COUNTERS(apple80211_he_counters_ctl*) override { return kIOReturnUnsupported; }
    IOReturn getRSN_XE(apple80211_rsn_xe_data*) override { return kIOReturnUnsupported; }
    IOReturn getSIB_COEX_STATUS(apple80211_sib_coex_status*) override { return kIOReturnUnsupported; }
    IOReturn getWCL_LOW_LATENCY_INFO_STATS(apple80211_wcl_low_latency_stats*) override { return kIOReturnUnsupported; }
    IOReturn getWCL_BGSCAN_CACHE_RESULT(apple80211_bgscan_cached_network_data_list*) override { return kIOReturnUnsupported; }
    IOReturn getWCL_WNM_OFFLOAD(apple80211_wcl_wnm_offload_t*) override { return kIOReturnUnsupported; }
    IOReturn getWIFI_NOISE_PER_ANT(apple80211_noise_per_ant_t*) override { return kIOReturnUnsupported; }
    IOReturn getFW_CLOCK_INFO(apple80211_fw_clock_info*) override { return kIOReturnUnsupported; }
    IOReturn getTIMESYNC_STATS(apple80211_timesync_stats*) override { return kIOReturnUnsupported; }
    IOReturn getSYSTEM_SLEEP_CONFIG(apple80211_system_sleep_config*) override { return kIOReturnUnsupported; }
    IOReturn getSMARTCCA_OPMODE(apple80211_smartcca_opmode*) override { return kIOReturnUnsupported; }
    IOReturn getLQM_STATISTICS(apple80211_lqm_statistics*) override { return kIOReturnUnsupported; }
    IOReturn getHE_CAPABILITY(apple80211_he_capability*) override { return kIOReturnUnsupported; }
    IOReturn getDEVICE_ORIENTATION(apple80211_device_orientation*) override { return kIOReturnUnsupported; }
    IOReturn getACCESSORY_STATE(apple80211_device_accessory_info*) override { return kIOReturnUnsupported; }
    IOReturn getP2P_DEVICE_CAPABILITY(apple80211_p2p_device_capability*) override { return kIOReturnUnsupported; }
    IOReturn getPOWERTABLE_VERSION(apple80211_powertable_version_data*) override { return kIOReturnUnsupported; }
    IOReturn setCHANNEL(apple80211_channel_data*) override { return kIOReturnUnsupported; }
    IOReturn setTXPOWER(apple80211_txpower_data*) override { return kIOReturnUnsupported; }
    IOReturn setRATE(apple80211_rate_data*) override { return kIOReturnUnsupported; }
    IOReturn setIBSS_MODE(apple80211_network_data*) override { return kIOReturnUnsupported; }
    IOReturn setAP_MODE(apple80211_apmode_data*) override { return kIOReturnUnsupported; }
    IOReturn setIE(apple80211_ie_data*) override { return kIOReturnUnsupported; }
    IOReturn setWOW_TEST(apple80211_wow_test_data*) override { return kIOReturnUnsupported; }
    IOReturn setVIRTUAL_IF_CREATE(apple80211_virt_if_create_data*) override { return kIOReturnUnsupported; }
    IOReturn setHT_CAPABILITY(apple80211_ht_capability*) override { return kIOReturnUnsupported; }
    IOReturn setOFFLOAD_ARP(apple80211_offload_arp_data*) override { return kIOReturnUnsupported; }
    IOReturn setOFFLOAD_NDP(apple80211_offload_ndp_data*) override { return kIOReturnUnsupported; }
    IOReturn setGAS_REQ(apple80211_gas_query_t*) override { return kIOReturnUnsupported; }
    IOReturn setVHT_CAPABILITY(apple80211_vht_capability*) override { return kIOReturnUnsupported; }
    IOReturn setROAM_PROFILE(apple80211_roam_profile_all_bands*) override { return kIOReturnUnsupported; }
    IOReturn setDBG_GUARD_TIME_PARAMS(apple80211_dbg_guard_time_params*) override { return kIOReturnUnsupported; }
    IOReturn setLEAKY_AP_STATS_MODE(apple80211_leaky_ap_setting*) override { return kIOReturnUnsupported; }
    IOReturn setPRIVATE_MAC(apple80211_private_mac_data*) override { return kIOReturnUnsupported; }
    IOReturn setRESET_CHIP(apple80211_reset_command*) override { return kIOReturnUnsupported; }
    IOReturn setCRASH(apple80211_crash_command*) override { return kIOReturnUnsupported; }
    IOReturn setRANGING_ENABLE(apple80211_ranging_enable_request_t*) override { return kIOReturnUnsupported; }
    IOReturn setRANGING_START(apple80211_ranging_start_request_t*) override { return kIOReturnUnsupported; }
    IOReturn setRANGING_AUTHENTICATE(apple80211_ranging_authenticate_request_t*) override { return kIOReturnUnsupported; }
    IOReturn setTKO_PARAMS(apple80211_tko_params*) override { return kIOReturnUnsupported; }
    IOReturn setBTCOEX_PROFILE(apple80211_btcoex_profile*) override { return kIOReturnUnsupported; }
    IOReturn setBTCOEX_PROFILE_ACTIVE(apple80211_btcoex_profile_active_data*) override { return kIOReturnUnsupported; }
    IOReturn setTHERMAL_INDEX(apple80211_thermal_index_t*) override { return kIOReturnUnsupported; }
    IOReturn setBTCOEX_2G_CHAIN_DISABLE(apple80211_btcoex_2g_chain_disable*) override { return kIOReturnUnsupported; }
    IOReturn setPOWER_BUDGET(apple80211_power_budget_t*) override { return kIOReturnUnsupported; }
    IOReturn setOFFLOAD_TCPKA_ENABLE(apple80211_offload_tcpka_enable_t*) override { return kIOReturnUnsupported; }
    IOReturn setLQM_CONFIG(apple80211_lqm_config_t*) override { return kIOReturnUnsupported; }
    IOReturn setDYNAMIC_RSSI_WINDOW_CONFIG(apple80211_dynamic_rssi_window_config*) override { return kIOReturnUnsupported; }
    IOReturn setUSB_HOST_NOTIFICATION(apple80211_usb_host_notification_data*) override { return kIOReturnUnsupported; }
    IOReturn setHP2P_CTRL(apple80211_hp2p_ctrl*) override { return kIOReturnUnsupported; }
    IOReturn setBSS_BLACKLIST(bss_blacklist*) override { return kIOReturnUnsupported; }
    IOReturn setSET_PROPERTY(apple80211_set_property_unserialized_data*) override { return kIOReturnUnsupported; }
    IOReturn setROAM_CACHE_UPDATE(apple80211_roam_cache_data*) override { return kIOReturnUnsupported; }
    IOReturn setPM_MODE(apple80211_pm_mode*) override { return kIOReturnUnsupported; }
    IOReturn setSET_WIFI_ASSERTION_STATE(apple80211_wifi_assertion_data*) override { return kIOReturnUnsupported; }
    IOReturn setREALTIME_QOS_MSCS(apple80211_state_data*) override { return kIOReturnUnsupported; }
    IOReturn setSENSING_ENABLE(apple80211_sensing_enable_t*) override { return kIOReturnUnsupported; }
    IOReturn setSENSING_DISABLE(apple80211_sensing_disable_t*) override { return kIOReturnUnsupported; }
    IOReturn setWCL_REASSOC(apple80211_reassoc*) override { return kIOReturnUnsupported; }
    IOReturn setWCL_SET_ROAM_LOCK(apple80211_set_roam_lock*) override { return kIOReturnUnsupported; }
    IOReturn setWCL_LEGACY_ROAM_PROFILE_CONFIG(apple80211_legacy_roam_profile_config*) override { return kIOReturnUnsupported; }
    IOReturn setWCL_ROAM_PROFILE_CONFIG(apple80211_roam_profile_config*) override { return kIOReturnUnsupported; }
    IOReturn setWCL_ROAM_USER_CACHE(apple80211_user_roam_cache*) override { return kIOReturnUnsupported; }
    IOReturn setWCL_REAL_TIME_MODE(apple80211_wcl_real_time_mode*) override { return kIOReturnUnsupported; }
    IOReturn setWCL_ARP_MODE(apple80211_wcl_arp_mode*) override { return kIOReturnUnsupported; }
    IOReturn setWCL_TRIGGER_CC(triggerCC*) override { return kIOReturnUnsupported; }
    IOReturn setWCL_QOS_PARAMS(apple80211_wcl_qos_params*) override { return kIOReturnUnsupported; }
    IOReturn setWCL_LINK_UP_DONE(void*) override { return kIOReturnUnsupported; }
    IOReturn setVOICE_IND_STATE(apple80211_voice_ind_state*) override { return kIOReturnUnsupported; }
    IOReturn setRSN_XE(apple80211_rsn_xe_data*) override { return kIOReturnUnsupported; }
    IOReturn setMWS_ACCESSORY_POWER_LIMIT_WIFI_ENH(apple80211_mws_accessory_power_limit*) override { return kIOReturnUnsupported; }
    IOReturn setWCL_ULOFDMA_STATE(apple80211_wcl_ulofdma_state*) override { return kIOReturnUnsupported; }
    IOReturn setWCL_ACTION_FRAME(apple80211_wcl_action_frame*) override { return kIOReturnUnsupported; }
    IOReturn setGAS_ABORT(void*) override { return kIOReturnUnsupported; }
    IOReturn setOS_FEATURE_FLAGS(apple80211_feature_flags*) override { return kIOReturnUnsupported; }
    IOReturn setDHCP_RENEWAL_DATA(apple80211_dhcp_renewal_data*) override { return kIOReturnUnsupported; }
    IOReturn setBATTERY_POWERSAVE_CONFIG(apple80211_battery_ps_config*) override { return kIOReturnUnsupported; }
    IOReturn setMIMO_CONFIG(apple80211_mimo_config*) override { return kIOReturnUnsupported; }
    IOReturn setWCL_CONFIG_BG_MOTIONPROFILE(apple80211_bg_motion_profile*) override { return kIOReturnUnsupported; }
    IOReturn setWCL_CONFIG_BG_NETWORK(apple80211_bg_network*) override { return kIOReturnUnsupported; }
    IOReturn setWCL_CONFIG_BGSCAN(apple80211_bg_scan*) override { return kIOReturnUnsupported; }
    IOReturn setWCL_CONFIG_BG_PARAMS(apple80211_bg_params*) override { return kIOReturnUnsupported; }
    IOReturn setPOWER_PROFILE(apple80211_power_profile*) override { return kIOReturnUnsupported; }
    IOReturn setHEARTBEAT(void*) override { return kIOReturnUnsupported; }
    IOReturn setINTERFACE_SETTING(apple80211_interface_setting*) override { return kIOReturnUnsupported; }
    IOReturn setBYPASS_TX_POWER_CAP(apple80211_bypass_tx_power_cap*) override { return kIOReturnUnsupported; }
    IOReturn setFACETIME_WIFICALLING_PARAMS(apple80211_facetime_wificalling_params*) override { return kIOReturnUnsupported; }
    IOReturn setIPV4_PARAMS(apple80211_ipv4_params*) override { return kIOReturnUnsupported; }
    IOReturn setWCL_WNM_OPS(apple80211_wcl_wnm_config_t*) override { return kIOReturnUnsupported; }
    IOReturn setWCL_WNM_OFFLOAD(apple80211_wcl_wnm_offload_t*) override { return kIOReturnUnsupported; }
    IOReturn setWCL_LIMITED_AGGREGATION(apple80211_limited_aggregation_config*) override { return kIOReturnUnsupported; }
    IOReturn setWCL_BCN_MUTE_CONFIG(apple80211_bcn_mute_config*) override { return kIOReturnUnsupported; }
    IOReturn setEAP_FILTER_CONFIG(apple80211_eap_filter_config*) override { return kIOReturnUnsupported; }
    IOReturn setWOW_LOW_POWER_MODE(apple80211_wow_low_power_mode*) override { return kIOReturnUnsupported; }
    IOReturn setDUAL_POWER_MODE(apple80211_dual_power_mode_params*) override { return kIOReturnUnsupported; }
    IOReturn setWCL_ASSOCIATED_SLEEP(apple80211_associated_sleep_config*) override { return kIOReturnUnsupported; }
    IOReturn setCONGESTION_CTRL_IND(apple80211_congestion_control_indication*) override { return kIOReturnUnsupported; }
    IOReturn setSTAND_ALONE_MODE_STATE(apple80211_standalone_state*) override { return kIOReturnUnsupported; }
    IOReturn setIPV6_PARAMS(apple80211_ipv6_params*) override { return kIOReturnUnsupported; }
    IOReturn setINFRA_ENUMERATED(apple80211_infra_enumerated*) override { return kIOReturnUnsupported; }
    IOReturn setLMTPC_CONFIG(apple80211_lmtpc_config*) override { return kIOReturnUnsupported; }
    IOReturn setTRAFFIC_ENG_PARAMS(apple80211_traffic_eng_params*) override { return kIOReturnUnsupported; }
    IOReturn setLE_SCAN_PARAM(apple80211_le_scan_params*) override { return kIOReturnUnsupported; }
    IOReturn setTIMESYNC_GPIO(apple80211_timesync_gpio*) override { return kIOReturnUnsupported; }
    IOReturn setHOST_CLOCK_INFO(apple80211_host_clock_info*) override { return kIOReturnUnsupported; }
    IOReturn setFW_CLOCK_SOURCE(apple80211_fw_clock_source*) override { return kIOReturnUnsupported; }
    IOReturn setTIMESYNC_TX_POLICY(apple80211_timesync_tx_policy*) override { return kIOReturnUnsupported; }
    IOReturn setTIMESYNC_RX_POLICY(apple80211_timesync_rx_policy*) override { return kIOReturnUnsupported; }
    IOReturn setTIMESTAMPING_EN(apple80211_timestamping_en*) override { return kIOReturnUnsupported; }
    IOReturn setWCL_SOI_CONFIG(appl80211_sleep_on_inactivity_config*) override { return kIOReturnUnsupported; }
    IOReturn setMWS_TIME_SHARING_WIFI_ENH(apple80211_mws_time_sharing*) override { return kIOReturnUnsupported; }
    IOReturn setMWS_WIFI_TYPE_7_BITMAP_WIFI_ENH(apple80211_mws_wifi_channel_bitmap*) override { return kIOReturnUnsupported; }
    IOReturn setMWS_COEX_BITMAP_WIFI_ENH(apple80211_mws_wifi_channel_bitmap*) override { return kIOReturnUnsupported; }
    IOReturn setMWS_DISABLE_OCL_BITMAP_WIFI_ENH(apple80211_mws_wifi_channel_bitmap*) override { return kIOReturnUnsupported; }
    IOReturn setMWS_RFEM_CONFIG_WIFI_ENH(apple80211_mws_rfem_config*) override { return kIOReturnUnsupported; }
    IOReturn setMWS_ASSOC_PROTECTION_BITMAP_WIFI_ENH(apple80211_mws_wifi_channel_bitmap*) override { return kIOReturnUnsupported; }
    IOReturn setMWS_SCAN_FREQ_WIFI_ENH(apple80211_mws_scan_freq*) override { return kIOReturnUnsupported; }
    IOReturn setMWS_SCAN_FREQ_MODE_WIFI_ENH(apple80211_mws_scan_freq_mode*) override { return kIOReturnUnsupported; }
    IOReturn setMWS_CONDITION_ID_BITMAP_WIFI_ENH(apple80211_mws_condition_id_config*) override { return kIOReturnUnsupported; }
    IOReturn setMWS_ANTENNA_SELECTION_WIFI_ENH(apple80211_mws_antenna_selection*) override { return kIOReturnUnsupported; }
    IOReturn setNDD_REQ(apple80211_ndd_data*) override { return kIOReturnUnsupported; }
    IOReturn setDBRG_ENTROPY(apple80211_drbg_entropy*) override { return kIOReturnUnsupported; }
    IOReturn setSDB_ENABLE(apple80211_sdb_enable*) override { return kIOReturnUnsupported; }
    IOReturn setBTCOEX_EXT_PROFILE(apple80211_btcoex_ext_profile*) override { return kIOReturnUnsupported; }
    IOReturn setDEVICE_ORIENTATION(apple80211_device_orientation*) override { return kIOReturnUnsupported; }
    IOReturn setACCESSORY_STATE(apple80211_device_accessory_info*) override { return kIOReturnUnsupported; }
    IOReturn setOS_ELIGIBILITY(apple80211_os_eligibility*) override { return kIOReturnUnsupported; }
    IOReturn setTX_MODE_CONFIG(apple80211_tx_mode_config*) override { return kIOReturnUnsupported; }
    IOReturn setMITIGATE_INTERFERENCE(apple80211_mitigate_interference*) override { return kIOReturnUnsupported; }
    // Controller-level queries served from the session.
    IOReturn countryCode(apple80211_country_code_data *);
    void powerChanged(bool on);
    void stationMACChanged(const uint8_t *mac,bool permanent);
    uint32_t sessionState() const;
private:
    struct CoreAdapter : AirportBridge::Core { AirportAth10kInterface *owner=nullptr; int call(const AthControl::Request &,AthControl::Response &) override; };
    struct SinkAdapter : AirportBridge::Sink { AirportAth10kInterface *owner=nullptr; void post(uint32_t,const void *,size_t) override; void linkState(bool) override; };
    static IOReturn command(OSObject *,void *,void *,void *,void *);
    static void pollTimer(OSObject *,IOTimerEventSource *);
    IOReturn gated(uintptr_t op,void *arg=nullptr);
    uint64_t nowMs() const;
    void updateLinkQuality();
    void mirrorState();
    void kick();
    IOReturn fillRateSet(apple80211_rate_set_data *);
    IOReturn fillMCSSet(apple80211_mcs_index_set_data *);
    AirportAth10k *controller=nullptr;
    IOTimerEventSource *timer=nullptr;
    CoreAdapter coreAdapter{};SinkAdapter sinkAdapter{};
    AirportAth10kSession *session=nullptr;
    uint8_t permanentMAC[6]{};
    bool stopping=false,started=false;
    AirportRegistration::Blocks registration{};
    uint32_t mirroredState=~0u,mirroredReason=~0u,mirroredErrorSerial=~0u;
    uint32_t mirroredScanMs=~0u,mirroredJoinMs=~0u,mirroredScans=~0u,mirroredNetworks=~0u;
    int reportedQuality=-3;
    uint16_t scanHomeAwayMs=0;
};
