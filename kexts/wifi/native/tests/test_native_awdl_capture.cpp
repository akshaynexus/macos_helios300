// Host test: replay Owly's captures of a real iPhone (https://github.com/t4t5/owly, MIT;
// tests/captures, the anonymised corpus) through the AWDL engine, at the capture's own timestamps.
// Mirrors Owly's crates/owly-radio/tests/replay.rs:
//   T-AWDL-1 (AWDL-3/4/5): every AWDL action frame parses whole;
//   T-AWDL-3 (AWDL-7/17): the capture's peer turns valid, the passive node follows a master and
//   keeps within 3 TU of that master's schedule; data frames parse.
// This proves the parsers and the schedule against what iPhones really send, nothing about radios.
// The discovery capture is checked in (awdl-captures/); with OWLY_CAPTURES=<owly>/tests/captures the
// whole corpus runs (two more captures, about 6 MB, not copied here).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cstdint>
#include "AirportAth10kAwdl.hpp"
using namespace AirportAwdl;

struct Rec { uint64_t us; std::vector<uint8_t> data; };
static std::vector<Rec> readPcap(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb"); std::vector<Rec> out; if (!f) return out;
    uint8_t h[24]; assert(fread(h, 1, 24, f) == 24);
    assert(h[0] == 0xd4 && h[1] == 0xc3 && h[2] == 0xb2 && h[3] == 0xa1);   // little-endian, microseconds
    assert(h[20] == 127);                                                   // LINKTYPE_IEEE802_11_RADIOTAP
    uint8_t r[16];
    while (fread(r, 1, 16, f) == 16) {
        uint32_t sec = get32le(r), usec = get32le(r + 4), n = get32le(r + 8);
        Rec rec; rec.us = uint64_t(sec) * 1000000 + usec; rec.data.resize(n);
        if (fread(rec.data.data(), 1, n, f) != n) break;
        out.push_back(rec);
    }
    fclose(f); return out;
}
// Our frames carry no radiotap header and no FCS; the corpus has channel + signal and no FCS.
static bool stripRadiotap(const std::vector<uint8_t> &in, const uint8_t **frame, size_t *len) {
    if (in.size() < 4) return false;
    const size_t rt = in[2] | (size_t(in[3]) << 8);
    if (rt >= in.size()) return false;
    *frame = in.data() + rt; *len = in.size() - rt; return true;
}
// The T-AWDL-1 check: walk the TLVs of an action frame; returns false on a malformed frame.
static bool parsesWhole(const uint8_t *frame, size_t len, uint16_t *txCounter, uint16_t *awCounter, bool *hasSync) {
    *hasSync = false;
    if (len < 24 + kActionHeaderLen) return true;     // not one of ours
    if (parseActionHeader(frame + 24, len - 24) < 0) return true;
    Reader f(frame + 24 + kActionHeaderLen, len - 24 - kActionHeaderLen);
    uint8_t type; uint16_t tlvLen; const uint8_t *value; size_t off = 0;
    while (off + 3 <= f.len) {
        if (!f.tlv(off, &type, &tlvLen, &value)) return false;
        if (type == kTlvSyncParams && tlvLen >= 31) { Reader v(value, tlvLen); *hasSync = v.le16(1, txCounter) && v.le16(29, awCounter); }
        off += size_t(tlvLen) + 3;
    }
    for (; off < f.len; ++off) if (f.data[off]) return false;     // AWDL-5: only zero padding after the last TLV
    return true;
}
struct Null : Sink {
    uint64_t data = 0; void setChannel(uint8_t) override {} bool transmit(const uint8_t *, size_t, bool) override { return true; }
    void data_(uint64_t n) { data += n; }
};
struct Counter : Null { uint64_t added = 0, frames = 0; void peerAdded(const Peer &) override { ++added; } void data(const uint8_t *, size_t, const Addr &, const Addr &) override { ++frames; } };

static void replay(const std::string &path, const char *peerText) {
    auto records = readPcap(path); if (records.empty()) { printf("  (skipped %s: not found)\n", path.c_str()); return; }
    Addr peer; unsigned v[6]; assert(sscanf(peerText, "%x:%x:%x:%x:%x:%x", v, v + 1, v + 2, v + 3, v + 4, v + 5) == 6);
    for (int i = 0; i < 6; ++i) peer.b[i] = uint8_t(v[i]);
    Engine *engine = new Engine(); Counter sink;
    const Addr self = Addr::of((const uint8_t *)"\x02\x00\x00\x00\x00\x00");
    const uint64_t base = records.front().us;
    engine->start("replay", self, Chan::opclass44(), 1, &sink);
    engine->setPresence(kPresenceFixed, Chan::opclass44());
    engine->state.filterRssi = false;
    engine->state.peers.timeout = kPeerTimeoutOwlyUs;
    uint64_t actions = 0, dataFrames = 0, malformed = 0, unknown = 0, syncedFrames = 0, offSchedule = 0; bool synced = false; Addr syncedTo{};
    for (const auto &r : records) {
        const uint64_t now = r.us - base + 1;
        engine->tick(now);
        const uint8_t *frame; size_t len;
        if (!stripRadiotap(r.data, &frame, &len) || len < 24) continue;
        uint16_t fc = get16le(frame); Addr src = Addr::of(frame + 10);
        uint16_t txc = 0, awc = 0; bool hasSync = false;
        if ((fc & 0x00fc) == kFcMgmtAction) {
            if (!parsesWhole(frame, len, &txc, &awc, &hasSync)) ++malformed;
            ++actions;
            if (hasSync && src == engine->state.election.syncAddr && !(src == self)) {
                if (synced && syncedTo == src) { ++syncedFrames; const int64_t e = syncErrorTu(now, txc, awc, engine->state.sync); if (e > 3 || e < -3) ++offSchedule; }
                synced = true; syncedTo = src;
            }
        } else if ((fc & 0x000c) == 0x0008) ++dataFrames;
        const int rc = engine->receive(frame, len, -40, now);
        if (rc == kRxUnexpectedFormat || rc == kRxTooShort || rc == kRxUnexpectedValue) ++malformed;
        else if (rc == kRxUnexpectedType) ++unknown;
    }
    assert(actions > 0);
    assert(malformed == 0);                                           // T-AWDL-1 and the AWDL data frames
    const Peer *p = engine->state.peers.find(peer);
    assert(p && p->valid && p->sentMif && p->version && p->devclass); // AWDL-17: the phone turned valid
    assert(syncedFrames > 0);
    assert(offSchedule * 10 <= syncedFrames);                         // within 3 TU of the master's schedule
    printf("  %-48s %5zu records %5zu action %5zu data; peer valid; %zu synced frames, %zu off-schedule; %llu ethernet frames, %zu unknown\n",
           path.c_str() + path.rfind('/') + 1, records.size(), size_t(actions), size_t(dataFrames), size_t(syncedFrames), size_t(offSchedule), (unsigned long long)sink.frames, size_t(unknown));
    delete engine;
}

int main() {
    replay("native/tests/awdl-captures/discovery-ios26.x-mt7925e-ch44-1.pcap", "02:00:00:00:00:02");
    if (const char *dir = getenv("OWLY_CAPTURES")) {
        replay(std::string(dir) + "/receive-ios26.x-mt7925e-ch44-1.pcap", "02:00:00:00:00:01");
        replay(std::string(dir) + "/send-ios26.x-mt7925e-ch44-1.pcap", "02:00:00:00:00:03");
    } else printf("  (the other two corpus captures: set OWLY_CAPTURES=<owly>/tests/captures)\n");
    printf("AWDL capture replay: every action frame parses, the iPhone turns valid, the schedule is followed.\n");
    return 0;
}
