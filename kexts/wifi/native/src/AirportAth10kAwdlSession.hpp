// SPDX-License-Identifier: GPL-3.0-or-later
// The AWDL engine (AirportAth10kAwdl.hpp, a port of OWL) bound to the radio
// path of the ath10k core and to the host stack. Pure logic: it drives the
// abstract RadioPort and HostPort below, so native/tests/test_native_awdl_radio.cpp
// runs the same code against a simulated core. Compiled into the kext only with
// AIRPORT_ATH10K_AWDL=1; the ports are implemented in AirportAth10kAwdlDriver.cpp.
//
// Radio placement follows Owly's shared/exclusive design (https://github.com/t4t5/owly, MIT):
//  - while the station is associated on a social channel, AWDL shares that channel and stays
//    on it (shared mode: nothing else moves, the station's MAC is the AWDL address, so the
//    station's receive path ACKs the peer's unicast frames);
//  - while the station is not associated, the radio stays on one social channel (fixed mode)
//    with a monitor vdev only, so unicast from peers is not ACKed;
//  - while the station is associated elsewhere AWDL cannot run (no multi-channel support).
// See docs/WIFI-AWDL.md for what is proven and what is not.
#pragma once
#include "AirportAth10kAwdl.hpp"

namespace AirportAwdl {
// Mirrors Ath10k::AwdlStep (Ath10kAwdl.hpp) without depending on the core headers.
enum class RadioStep : uint8_t { Off, Create, Start, Up, Running, Down, Restart, RestartUp, Stop, Delete, Failed };
struct RxFrame { const uint8_t *data; size_t length; int8_t rssi; uint8_t channel; bool data_; uint64_t timeUs; };
// What the session needs to know about the station link (the core's connection state).
struct StationInfo { bool associated; uint8_t channel; };

struct RadioPort {
    virtual bool radioStart(const uint8_t *mac, uint8_t channel, bool shared) = 0;
    virtual void radioStop() = 0;
    virtual bool radioSetChannel(uint8_t channel) = 0;
    virtual bool radioTransmit(const uint8_t *frame, size_t length, bool action) = 0;
    // Fills the caller's buffer with the next received frame; false when none is waiting.
    virtual bool radioReceive(uint8_t *buffer, size_t capacity, RxFrame &out) = 0;
    virtual RadioStep radioStep() = 0;
    virtual void radioStation(StationInfo &out) { out = StationInfo{}; }   // default: not associated
    virtual ~RadioPort() {}
};
struct HostPort {
    virtual void hostEthernet(const uint8_t *eth, size_t length) = 0;
    virtual void hostPeer(const Peer &peer, bool added) = 0;
    virtual void hostSyncChanged() = 0;
    // Every received AWDL action frame, before the engine looks at it: the macOS stack's
    // peer manager is fed from here (802.11 frame, no FCS).
    virtual void hostActionFrame(const uint8_t *frame, size_t length, uint8_t channel, int8_t rssi) { (void)frame; (void)length; (void)channel; (void)rssi; }
    virtual ~HostPort() {}
};

inline bool socialChannel(uint8_t c) { return c == 6 || c == 44 || c == 149; }

class Session : public Sink {
public:
    struct Stats { uint64_t rxFrames, rxBad, txFrames, txRefused, sent, delivered, restarts, echoes, synsHeld, synsDelivered, synsExpired; };
    enum class State : uint8_t { Disabled, Starting, Running, Waiting };
    enum class Placement : uint8_t { None, Fixed, Shared };

    Session(RadioPort &radio, HostPort &host) : radio_(radio), host_(host) {}

    // The MAC address is the AWDL address and stays for the whole session (AWDL-2); `channel` is the
    // social channel used while the station is not associated.
    void configure(const char *hostname, const uint8_t *mac, uint8_t channel) {
        size_t n = strlen(hostname); if (n > kHostNameMax) n = kHostNameMax;
        memcpy(hostname_, hostname, n); hostname_[n] = 0;
        memcpy(mac_, mac, 6); channel_ = channel;
    }
    void setNeighbourEcho(bool on) { neighbourEcho_ = on; }  // test switch: OWLY-2 traffic stays out of frame-count assertions
    void enable(uint64_t nowUs) { wanted_ = true; if (state_ == State::Disabled) { state_ = State::Waiting; nextAction_ = nowUs; } }
    void disable() {
        wanted_ = false;
        if (engine_.running) stopEngine();
        radio_.radioStop();
        placement_ = Placement::None; state_ = State::Disabled;
    }
    State state() const { return state_; }
    bool running() const { return state_ == State::Running && engine_.running; }
    Placement placement() const { return running() ? placement_ : Placement::None; }
    const Stats &stats() const { return stats_; }
    const Engine &engine() const { return engine_; }
    Engine &engine() { return engine_; }
    uint8_t masterChannel() const { return running() ? chanNum(engine_.state.channel.master, kChanEncOpclass) : channel_; }
    uint8_t currentChannel() const { return running() ? engine_.currentChannel() : 0; }
    int peerCount() const { return running() ? engine_.state.peers.length() : 0; }

    // The macOS stack's AirDrop/Bonjour records, from the frame body it hands the driver in an OOB
    // request (IO80211AWDLPeerManager::updatePrimaryPayloadMI on 26.x): Service Response TLVs (type 2)
    // and its host name (ARPA, 16). Returns how many TLVs were kept; negative if the body is not
    // a valid TLV run. Sticks until replaced; survives engine restarts.
    int setServiceTlvsFromFrame(const uint8_t *body, size_t length) {
        if (!body) return -1;
        const uint8_t *tlvs = body; size_t count = length;
        if (length >= kActionHeaderLen && body[0] == kVendorSpecific && !memcmp(body + 1, kAwdlOui, 3) && body[4] == kAwdlType) {
            tlvs += kActionHeaderLen; count -= kActionHeaderLen;
        }
        if (!count) return -1;
        // Validate before mutating (AWDL-4); a trailing zero padding of fewer than 3 bytes is allowed (AWDL-5).
        size_t off = 0;
        while (off + 3 <= count) {
            size_t n = size_t(tlvs[off + 1]) | (size_t(tlvs[off + 2]) << 8);
            if (n > count - off - 3) return -1;
            off += 3 + n;
        }
        for (; off < count; ++off) if (tlvs[off]) return -1;
        int kept = 0; size_t used = 0, arpa = 0;
        for (size_t at = 0; at + 3 <= count;) {
            uint8_t type = tlvs[at]; size_t n = size_t(tlvs[at + 1]) | (size_t(tlvs[at + 2]) << 8);
            if (type == kTlvServiceResponse && used + 3 + n <= sizeof serviceTlvs_) { memcpy(serviceTlvs_ + used, tlvs + at, 3 + n); used += 3 + n; ++kept; }
            else if (type == kTlvArpa && 3 + n <= sizeof arpaTlv_) { memcpy(arpaTlv_, tlvs + at, 3 + n); arpa = 3 + n; ++kept; }
            at += 3 + n;
        }
        serviceTlvsLen_ = uint16_t(used); arpaTlvLen_ = uint16_t(arpa);
        applyServiceTlvs();
        return kept;
    }
    // Copied into the engine whenever it starts or the records change.
    void applyServiceTlvs() {
        if (!engine_.running) return;
        memcpy(engine_.state.serviceTlvs, serviceTlvs_, serviceTlvsLen_); engine_.state.serviceTlvsLen = serviceTlvsLen_;
        memcpy(engine_.state.arpaTlv, arpaTlv_, arpaTlvLen_); engine_.state.arpaTlvLen = arpaTlvLen_;
    }

    // Host stack output (Ethernet frame). False is a drop, as in the core's data path.
    bool send(const uint8_t *eth, size_t length, uint64_t nowUs) {
        if (!running()) return false;
        const bool ok = engine_.send(eth, length, nowUs);
        if (ok) { ++stats_.sent; }
        return ok;
    }
    // Everything time-driven. Returns the next time poll() must run again (microseconds).
    uint64_t poll(uint64_t nowUs) {
        if (!wanted_) return nowUs + kDisabledPollUs;
        const RadioStep step = radio_.radioStep();
        // Where the radio should be: with the station on a social channel, shared on it; idle station,
        // fixed on the configured channel; station elsewhere, nowhere (no multi-channel support).
        StationInfo st{}; radio_.radioStation(st);
        Placement want = Placement::Fixed; uint8_t wantChannel = channel_;
        if (st.associated) {
            if (socialChannel(st.channel)) { want = Placement::Shared; wantChannel = st.channel; }
            else { want = Placement::None; wantChannel = 0; }
        }
        if (step == RadioStep::Failed) {
            if (engine_.running) stopEngine();
            state_ = State::Waiting;
            return nowUs + kRadioRetryUs;
        }
        if (want == Placement::None) {
            // AWDL cannot run on this station channel: nothing to transmit, and the radio is not there.
            if (engine_.running) stopEngine();
            if (step != RadioStep::Off) radio_.radioStop();
            placement_ = Placement::None; state_ = State::Waiting;
            return nowUs + kRadioPollUs;
        }
        if (step == RadioStep::Running && (placement_ != want || radioChannel_ != wantChannel)) {
            // The station joined, left, or roamed: tear down and come back in the new placement.
            if (engine_.running) stopEngine();
            radio_.radioStop();
            placement_ = Placement::None; state_ = State::Waiting; nextAction_ = nowUs;
            return nowUs + kRadioPollUs;
        }
        if (step != RadioStep::Running) {
            if (engine_.running) stopEngine();   // torn down (a join in progress, or a firmware reload)
            if (step == RadioStep::Off && nowUs >= nextAction_) {
                if (radio_.radioStart(mac_, wantChannel, want == Placement::Shared)) { state_ = State::Starting; placement_ = want; radioChannel_ = wantChannel; }
                else { state_ = State::Waiting; placement_ = Placement::None; }
                nextAction_ = nowUs + kRadioRetryUs;
                ++stats_.restarts;
            }
            return nowUs + kRadioPollUs;
        }
        if (!engine_.running) {
            state_ = State::Running;
            engine_.start(hostname_, Addr::of(mac_), Chan::opclass(radioChannel_), nowUs, this);
            engine_.setPresence(placement_ == Placement::Shared ? kPresenceShare : kPresenceFixed, Chan::opclass(radioChannel_));
            engine_.state.peers.timeout = kPeerTimeoutOwlyUs;   // AWDL-17
            engine_.state.psfHasHt = true;                      // SPEC: PSFs carry HT capabilities too
            applyServiceTlvs();
        }
        RxFrame in{};
        for (int i = 0; i < kRxBurst && radio_.radioReceive(rxFrame_, sizeof rxFrame_, in); ++i) {
            ++stats_.rxFrames;
            if (!in.data_) host_.hostActionFrame(in.data, in.length, in.channel, in.rssi);
            if (engine_.receive(in.data, in.length, in.rssi, in.timeUs) < 0) ++stats_.rxBad;
        }
        sendEchoes(nowUs);
        expireHeld(nowUs);
        uint64_t next = engine_.tick(nowUs);
        const uint64_t limit = nowUs + kRadioPollUs;
        return next < limit ? next : limit;
    }

    // Sink: the engine's side.
    void setChannel(uint8_t chan) override { radio_.radioSetChannel(chan); }
    bool transmit(const uint8_t *frame, size_t len, bool action) override {
        const bool ok = radio_.radioTransmit(frame, len, action);
        if (ok) ++stats_.txFrames; else ++stats_.txRefused;
        return ok;
    }
    void peerAdded(const Peer &p) override { host_.hostPeer(p, true); deliverHeld(p); }
    void peerRemoved(const Peer &p) override { host_.hostPeer(p, false); }
    void syncChanged() override { host_.hostSyncChanged(); }
    void data(const uint8_t *eth, size_t len, const Addr &src, const Addr &) override {
        if (isOurEchoReply(eth, len)) return;     // OWLY-2's replies need not reach the IP stack
        if (holdEarlySyn(eth, len, src)) return;  // AWDL-18
        ++stats_.delivered; host_.hostEthernet(eth, len);
    }

    // Exposed for the host tests.
    static size_t buildEcho(uint8_t *f, size_t cap, const Addr &src, const Addr &dst, uint16_t seq);
    static bool ipv6LinkLocal(const Addr &mac, uint8_t out[16]) {
        out[0] = 0xfe; out[1] = 0x80; memset(out + 2, 0, 6); out[8] = uint8_t(mac.b[0] ^ 2); out[9] = mac.b[1]; out[10] = mac.b[2]; out[11] = 0xff; out[12] = 0xfe;
        out[13] = mac.b[3]; out[14] = mac.b[4]; out[15] = mac.b[5]; return true;
    }

private:
    static constexpr uint64_t kRadioPollUs = 16 * 1024, kRadioRetryUs = 1000000, kDisabledPollUs = 1000000;
    static constexpr uint64_t kEchoIntervalUs = 3000000, kSynHoldUs = 2000000;
    static constexpr uint16_t kEchoId = 0x4f57;
    static constexpr int kRxBurst = 32, kHeldSlots = 4;
    struct Held { uint64_t at; uint16_t len; uint8_t data[kMaxEthernet]; Addr src; bool used; };

    void stopEngine() {
        for (auto &p : engine_.state.peers.peers)
            if (p.used && p.valid) host_.hostPeer(p, false);
        engine_.stop();
    }
    // OWLY-2: an ICMPv6 echo request to each valid peer every 3 s teaches Apple peers our neighbor entry.
    void sendEchoes(uint64_t now) {
        if (!neighbourEcho_) return;
        for (auto &p : engine_.state.peers.peers) {
            if (!p.used || !p.valid || (p.lastEcho && now - p.lastEcho < kEchoIntervalUs)) continue;
            p.lastEcho = now ? now : 1;
            uint8_t f[80]; size_t n = buildEcho(f, sizeof f, Addr::of(mac_), p.addr, echoSeq_++);
            if (n && engine_.send(f, n, now)) ++stats_.echoes;
        }
    }
    bool isIpv6(const uint8_t *e, size_t n, uint8_t nextHeader) const {
        return n >= 14 + 40 && e[12] == 0x86 && e[13] == 0xdd && (e[14] >> 4) == 6 && e[20] == nextHeader;
    }
    bool isOurEchoReply(const uint8_t *e, size_t n) const {
        return isIpv6(e, n, 58) && n >= 14 + 40 + 8 && e[54] == 129 && get16be(e + 58) == kEchoId;
    }
    bool isTcpSyn(const uint8_t *e, size_t n) const {
        if (!isIpv6(e, n, 6) || n < 14 + 40 + 20) return false;
        const uint8_t flags = e[14 + 40 + 13];
        return (flags & 0x12) == 0x02;   // SYN without ACK
    }
    // AWDL-18: a peer may open a TCP connection before it is valid; delivered now, its SYN-ACK has no neighbor
    // entry to go to. Hold the SYN up to 2 s and deliver it once the peer is valid.
    bool holdEarlySyn(const uint8_t *e, size_t n, const Addr &src) {
        if (!isTcpSyn(e, n) || n > kMaxEthernet) return false;
        const Peer *p = engine_.state.peers.find(src);
        if (!p || p->valid) return false;
        for (auto &h : held_) if (!h.used) {
            h.used = true; h.at = heldNow_; h.len = uint16_t(n); h.src = src; memcpy(h.data, e, n); ++stats_.synsHeld; return true;
        }
        return true;   // table full: the sender retries
    }
    void deliverHeld(const Peer &p) {
        for (auto &h : held_) if (h.used && h.src == p.addr) { h.used = false; ++stats_.delivered; ++stats_.synsDelivered; host_.hostEthernet(h.data, h.len); }
    }
    void expireHeld(uint64_t now) {
        heldNow_ = now ? now : 1;
        for (auto &h : held_) if (h.used && now - h.at > kSynHoldUs) { h.used = false; ++stats_.synsExpired; }
    }
    RadioPort &radio_;
    HostPort &host_;
    Engine engine_{};
    Stats stats_{};
    State state_ = State::Disabled;
    Placement placement_ = Placement::None;
    bool wanted_ = false, neighbourEcho_ = true;
    uint64_t nextAction_ = 0, heldNow_ = 1;
    char hostname_[kHostNameMax + 1] = "helios";
    uint8_t mac_[6]{};
    uint8_t channel_ = 6, radioChannel_ = 0;
    uint16_t echoSeq_ = 1;
    uint8_t serviceTlvs_[kServiceTlvsMax]; uint16_t serviceTlvsLen_ = 0;
    uint8_t arpaTlv_[3 + 2 + kHostNameMax + 2]; uint16_t arpaTlvLen_ = 0;
    Held held_[kHeldSlots]{};
    uint8_t rxFrame_[kMaxFrame]; // not on the kernel stack (the poll timer runs in a 4 KB frame budget)
};

// An Ethernet frame holding an ICMPv6 echo request, link-local to link-local (OWLY-2). Returns its length or 0.
inline size_t Session::buildEcho(uint8_t *f, size_t cap, const Addr &src, const Addr &dst, uint16_t seq) {
    const size_t icmp = 8 + 8;  // a short identifying payload
    const size_t len = 14 + 40 + icmp;
    if (cap < len) return 0;
    memset(f, 0, len);
    memcpy(f, dst.b, 6); memcpy(f + 6, src.b, 6); f[12] = 0x86; f[13] = 0xdd;
    uint8_t *ip = f + 14;
    ip[0] = 0x60; put16be(ip + 4, uint16_t(icmp)); ip[6] = 58; ip[7] = 255;
    ipv6LinkLocal(src, ip + 8); ipv6LinkLocal(dst, ip + 24);
    uint8_t *m = ip + 40;
    m[0] = 128; m[1] = 0; put16be(m + 4, kEchoId); put16be(m + 6, seq);
    memcpy(m + 8, "AWDLping", 8);
    // ICMPv6 checksum over the pseudo-header.
    uint32_t sum = 0;
    for (size_t i = 8; i + 1 < 40; i += 2) sum += get16be(ip + i);   // source and destination addresses
    sum += uint32_t(icmp); sum += 58;
    for (size_t i = 0; i + 1 < icmp; i += 2) sum += get16be(m + i);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    put16be(m + 2, uint16_t(~sum));
    return len;
}
} // namespace AirportAwdl
