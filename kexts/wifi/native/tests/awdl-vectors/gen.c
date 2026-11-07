/* Golden-vector generator: runs OWL's own C code with a fixed clock and prints frames + parsed state. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "state.h"
#include "tx.h"
#include "rx.h"
#include "schedule.h"
#include "wire.h"
static uint64_t fake_now = 0;
uint64_t clock_time_us(void) { return fake_now; }
static void hex(const char *name, const uint8_t *b, int n) { printf("%s %d ", name, n); for (int i = 0; i < n; i++) printf("%02x", b[i]); printf("\n"); }
int main(void) {
  struct ether_addr self = {{0x02,0x11,0x22,0x33,0x44,0x55}};
  struct awdl_state s; struct ieee80211_state is;
  fake_now = 5000000; /* 5 s */
  awdl_init_state(&s, "helios", &self, CHAN_OPCLASS_44, fake_now);
  ieee80211_init_state(&is);
  s.sync.aw_counter = 1000; s.sync.last_update = 4000000;
  s.election.self_metric = 60; s.election.self_counter = 0;
  uint8_t buf[2048]; int n;
  fake_now = 5123456;
  n = awdl_init_full_action_frame(buf, &s, &is, AWDL_ACTION_PSF); hex("PSF", buf, n);
  fake_now = 5234567;
  n = awdl_init_full_action_frame(buf, &s, &is, AWDL_ACTION_MIF); hex("MIF", buf, n);
  uint8_t payload[] = {0x60,0,0,0,0,8,0x3a,0xff, 1,2,3,4,5,6,7,8};
  struct ether_addr dst = {{0x33,0x33,0,0,0,1}};
  n = awdl_init_full_data_frame(buf, &self, &dst, payload, sizeof payload, &s, &is); hex("DATA", buf, n);
  /* sync/schedule numbers at a few times */
  for (uint64_t t = 5000000; t < 5300000; t += 37000) {
    printf("SYNC %llu next_aw_tu %u next_aw_us %llu cur_aw %u cur_eaw %u mcast %d can_send16 %.6f\n", (unsigned long long)t,
      awdl_sync_next_aw_tu(t, &s.sync), (unsigned long long)awdl_sync_next_aw_us(t, &s.sync), awdl_sync_current_aw(t, &s.sync),
      awdl_sync_current_eaw(t, &s.sync), awdl_is_multicast_eaw(&s, t), awdl_can_send_in(&s, t, 16));
  }
  /* receive side: a second node parses our MIF (via awdl_rx with radiotap) */
  struct ether_addr other = {{0x02,0xaa,0xbb,0xcc,0xdd,0xee}};
  struct awdl_state r; awdl_init_state(&r, "peer", &other, CHAN_OPCLASS_6, 6000000);
  r.filter_rssi = 0;
  fake_now = 6100000;
  /* PSF then MIF from s with radiotap header already included by tx (rate only) */
  n = awdl_init_full_action_frame(buf, &s, &is, AWDL_ACTION_PSF);
  { const struct buf *f = buf_new_const(buf, n); struct buf *arr[4]; struct buf **p = arr; int res = awdl_rx(f, &p, &r); printf("RX_PSF %d\n", res); buf_free(f); }
  hex("PSF2", buf, n);
  fake_now = 6200000;
  n = awdl_init_full_action_frame(buf, &s, &is, AWDL_ACTION_MIF);
  { const struct buf *f = buf_new_const(buf, n); struct buf *arr[4]; struct buf **p = arr; int res = awdl_rx(f, &p, &r); printf("RX_MIF %d\n", res); buf_free(f); }
  hex("MIF2", buf, n);
  struct awdl_peer *peer; awdl_peer_get(r.peers.peers, &self, &peer);
  printf("PEER valid %d name %s ver %u devclass %u v2 %u height %u master_metric %u self_metric %u master_counter %u self_counter %u last_update %llu\n",
    peer->is_valid, peer->name, peer->version, peer->devclass, peer->supports_v2, peer->election.height, peer->election.master_metric,
    peer->election.self_metric, peer->election.master_counter, peer->election.self_counter, (unsigned long long)peer->last_update);
  printf("PEER master "); for (int i=0;i<6;i++) printf("%02x", peer->election.master_addr.ether_addr_octet[i]); printf(" sync "); for (int i=0;i<6;i++) printf("%02x", peer->election.sync_addr.ether_addr_octet[i]); printf(" seq");
  for (int i=0;i<16;i++) printf(" %u", awdl_chan_num(peer->sequence[i], AWDL_CHAN_ENC_OPCLASS)); printf("\n");
  awdl_election_run(&r.election, &r.peers);
  printf("ELECT master "); for (int i=0;i<6;i++) printf("%02x", r.election.master_addr.ether_addr_octet[i]); printf(" height %u metric %u counter %u\n", r.election.height, r.election.master_metric, r.election.master_counter);
  /* now r syncs to s: feed the PSF again after election so sync params are accepted */
  fake_now = 6300000;
  n = awdl_init_full_action_frame(buf, &s, &is, AWDL_ACTION_PSF);
  { const struct buf *f = buf_new_const(buf, n); struct buf *arr[4]; struct buf **p = arr; int res = awdl_rx(f, &p, &r); printf("RX_PSF3 %d\n", res); buf_free(f); }
  hex("PSF3", buf, n);
  printf("RSYNC last_update %llu aw_counter %u\n", (unsigned long long)r.sync.last_update, r.sync.aw_counter);
  /* data frame from s to r */
  fake_now = 6400000;
  n = awdl_init_full_data_frame(buf, &self, &other, payload, sizeof payload, &s, &is); hex("DATA2", buf, n);
  { const struct buf *f = buf_new_const(buf, n); struct buf *arr[4]; struct buf **p = arr; int res = awdl_rx(f, &p, &r); printf("RX_DATA %d n %d\n", res, (int)(p-arr)); if (p>arr) hex("ETH", buf_data(arr[0]), buf_len(arr[0])); buf_free(f); }
  /* s becomes the better master: metric 100, counter 2; r must adopt it and then accept its sync params */
  s.election.self_metric = 100; s.election.master_metric = 100; s.election.self_counter = 2; s.election.master_counter = 2;
  fake_now = 6500000;
  n = awdl_init_full_action_frame(buf, &s, &is, AWDL_ACTION_MIF);
  { const struct buf *f = buf_new_const(buf, n); struct buf *arr[4]; struct buf **p = arr; int res = awdl_rx(f, &p, &r); printf("RX_MIF4 %d\n", res); buf_free(f); }
  hex("MIF4", buf, n);
  awdl_election_run(&r.election, &r.peers);
  printf("ELECT2 master "); for (int i=0;i<6;i++) printf("%02x", r.election.master_addr.ether_addr_octet[i]); printf(" sync "); for (int i=0;i<6;i++) printf("%02x", r.election.sync_addr.ether_addr_octet[i]); printf(" height %u metric %u counter %u\n", r.election.height, r.election.master_metric, r.election.master_counter);
  fake_now = 6600000;
  n = awdl_init_full_action_frame(buf, &s, &is, AWDL_ACTION_PSF);
  { const struct buf *f = buf_new_const(buf, n); struct buf *arr[4]; struct buf **p = arr; int res = awdl_rx(f, &p, &r); printf("RX_PSF5 %d\n", res); buf_free(f); }
  hex("PSF5", buf, n);
  printf("RSYNC2 last_update %llu aw_counter %u err %llu total %llu\n", (unsigned long long)r.sync.last_update, r.sync.aw_counter, (unsigned long long)r.sync.meas_err, (unsigned long long)r.sync.meas_total);
  for (uint64_t t = 6600000; t < 6800000; t += 41000) printf("RSYNCT %llu next_aw_tu %u cur_eaw %u same_chan %d\n", (unsigned long long)t, awdl_sync_next_aw_tu(t, &r.sync), awdl_sync_current_eaw(t, &r.sync), awdl_same_channel_as_peer(&r, t, peer));
  return 0;
}
