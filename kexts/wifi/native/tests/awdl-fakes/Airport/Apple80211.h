#pragma once
#include "IO80211SkywalkInterface.h"
#define APPLE80211_VERSION 1
#define APPLE80211_ADDR_LEN 6
#define APPLE80211_MAX_SSID_LEN 32
#define APPLE80211_S_INIT 0
#define APPLE80211_S_RUN 4
#define APPLE80211_C_FLAG_2GHZ 0x10
#define APPLE80211_C_FLAG_5GHZ 0x8
#define APPLE80211_C_FLAG_20MHZ 0x2
#define APPLE80211_C_FLAG_ACTIVE 0x80
enum IO80211LinkState{kIO80211NetworkLinkUndefined,kIO80211NetworkLinkDown,kIO80211NetworkLinkUp};
struct apple80211_channel{uint32_t version,channel,flags;};
struct apple80211_ssid_data{uint32_t version,ssid_len;uint8_t ssid_bytes[APPLE80211_MAX_SSID_LEN];};
struct apple80211_channel_data{uint32_t version;struct apple80211_channel channel;};
struct apple80211_bssid_data{uint32_t version;struct ether_addr bssid;};
struct apple80211_state_data{uint32_t version,state;};
struct apple80211_awdl_sync_params{uint32_t version,availability_window_length,availability_window_period,extension_length,synchronization_frame_period;};
struct apple80211_awdl_sync_enabled{uint32_t version,unk1,enabled;};
struct apple80211_awdl_sync_state{uint32_t version,state;};
struct apple80211_awdl_election_id{uint32_t version,election_id;};
struct apple80211_awdl_election_metric{uint32_t version,metric;};
struct apple80211_awdl_bssid{uint32_t version;uint8_t bssid[6];uint8_t unk_mac[6];};
struct apple80211_awdl_master_channel{uint32_t version,master_channel;};
struct apple80211_awdl_presence_mode{uint32_t version,mode;};
struct apple80211_peer_cache_maximum_size{uint32_t version,max_peers;};
struct apple80211_awdl_secondary_master_channel{uint32_t version,secondary_master_channel;};
struct apple80211_awdl_sync_channel_sequence{uint32_t version;uint8_t pad1;uint8_t length;uint8_t rest[394];};
struct apple80211_virt_if_create_data{uint32_t version;uint8_t mac[6];uint16_t unk1;uint32_t role;uint8_t bsd_name[16];};
class IO80211NetworkPacket;

struct apple80211_awdl_oob_request{uint32_t version,unk1,unk2,unk3,unk4;uint16_t pad1;uint32_t unk5;uint16_t unk6;uint32_t pad2;uint32_t unk7;uint32_t pad3;uint16_t data_len;uint32_t pad4;uint16_t unk9;uint8_t data[1782];} __attribute__((packed));
