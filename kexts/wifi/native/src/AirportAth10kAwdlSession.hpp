// SPDX-License-Identifier: GPL-3.0-or-later
// The AWDL engine (AirportAth10kAwdl.hpp, a port of OWL) bound to the radio
// path of the ath10k core and to the host stack. Pure logic: it drives the
// abstract RadioPort and HostPort below, so native/tests/test_native_awdl_radio.cpp
// runs the same code against a simulated core. Compiled into the kext only with
// AIRPORT_ATH10K_AWDL=1; the ports are implemented in AirportAth10kAwdlDriver.cpp.
//
// Degraded behaviour compared with a real Apple node (docs/WIFI-AWDL.md): a static
// channel sequence on one social channel, so this node is reachable only in the
// peer's slots on that channel; a monitor vdev with no ACKs (unicast from peers is
// retried up to 7 times); and no AWDL while the station vdev is joined or joining.
#pragma once
#include "AirportAth10kAwdl.hpp"

namespace AirportAwdl {
// Mirrors Ath10k::AwdlStep (Ath10kAwdl.hpp) without depending on the core headers.
enum class RadioStep : uint8_t { Off, Create, Start, Up, Running, Down, Restart, RestartUp, Stop, Delete, Failed };
struct RxFrame { const uint8_t *data; size_t length; int8_t rssi; uint8_t channel; bool data_; uint64_t timeUs; };

struct RadioPort {
    virtual bool radioStart(const uint8_t *mac, uint8_t channel) = 0;
    virtual void radioStop() = 0;
    virtual bool radioSetChannel(uint8_t channel) = 0;
    virtual bool radioTransmit(const uint8_t *frame, size_t length, bool action) = 0;
    // Fills the caller's buffer with the next received frame; false when none is waiting.
    virtual bool radioReceive(uint8_t *buffer, size_t capacity, RxFrame &out) = 0;
    virtual RadioStep radioStep() = 0;
    virtual ~RadioPort() {}
};
struct HostPort {
    virtual void hostEthernet(const uint8_t *eth, size_t length) = 0;
    virtual void hostPeer(const Peer &peer, bool added) = 0;
    virtual void hostSyncChanged() = 0;
    virtual ~HostPort() {}
};

class Session : public Sink {
public:
    struct Stats { uint64_t rxFrames, rxBad, txFrames, txRefused, sent, delivered, restarts; };
    enum class State : uint8_t { Disabled, Starting, Running, Waiting };

    Session(RadioPort &radio, HostPort &host) : radio_(radio), host_(host) {}

    // The MAC address and channel are fixed at configure time; the channel is a social channel.
    void configure(const char *hostname, const uint8_t *mac, uint8_t channel) {
        size_t n = strlen(hostname); if (n > kHostNameMax) n = kHostNameMax;
        memcpy(hostname_, hostname, n); hostname_[n] = 0;
        memcpy(mac_, mac, 6); channel_ = channel;
    }
    void enable(uint64_t nowUs) { wanted_ = true; if (state_ == State::Disabled) { state_ = State::Waiting; nextAction_ = nowUs; } }
    void disable() {
        wanted_ = false;
        if (engine_.running) stopEngine();
        radio_.radioStop();
        state_ = State::Disabled;
    }
    State state() const { return state_; }
    bool running() const { return state_ == State::Running && engine_.running; }
    const Stats &stats() const { return stats_; }
    const Engine &engine() const { return engine_; }
    uint8_t masterChannel() const { return running() ? chanNum(engine_.state.channel.master, kChanEncOpclass) : channel_; }
    uint8_t currentChannel() const { return running() ? engine_.currentChannel() : 0; }
    int peerCount() const { return running() ? engine_.state.peers.length() : 0; }
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
        if (step == RadioStep::Failed) {
            if (engine_.running) stopEngine();
            state_ = State::Waiting;
            return nowUs + kRadioRetryUs;
        }
        if (step != RadioStep::Running) {
            if (engine_.running) stopEngine();   // torn down (an infrastructure join, or a firmware reload)
            if (step == RadioStep::Off && nowUs >= nextAction_) {
                if (radio_.radioStart(mac_, channel_)) state_ = State::Starting;
                else state_ = State::Waiting;
                nextAction_ = nowUs + kRadioRetryUs;
                ++stats_.restarts;
            }
            return nowUs + kRadioPollUs;
        }
        if (!engine_.running) {
            state_ = State::Running;
            engine_.start(hostname_, Addr::of(mac_), Chan::opclass(channel_), nowUs, this);
        }
        RxFrame in{};
        for (int i = 0; i < kRxBurst && radio_.radioReceive(rxFrame_, sizeof rxFrame_, in); ++i) {
            ++stats_.rxFrames;
            if (engine_.receive(in.data, in.length, in.rssi, in.timeUs) < 0) ++stats_.rxBad;
        }
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
    void peerAdded(const Peer &p) override { host_.hostPeer(p, true); }
    void peerRemoved(const Peer &p) override { host_.hostPeer(p, false); }
    void syncChanged() override { host_.hostSyncChanged(); }
    void data(const uint8_t *eth, size_t len, const Addr &, const Addr &) override { ++stats_.delivered; host_.hostEthernet(eth, len); }

private:
    static constexpr uint64_t kRadioPollUs = 16 * 1024, kRadioRetryUs = 1000000, kDisabledPollUs = 1000000;
    static constexpr int kRxBurst = 32;
    void stopEngine() {
        for (auto &p : engine_.state.peers.peers)
            if (p.used && p.valid) host_.hostPeer(p, false);
        engine_.stop();
    }
    RadioPort &radio_;
    HostPort &host_;
    Engine engine_{};
    Stats stats_{};
    State state_ = State::Disabled;
    bool wanted_ = false;
    uint64_t nextAction_ = 0;
    char hostname_[kHostNameMax + 1] = "helios";
    uint8_t mac_[6]{};
    uint8_t channel_ = 6;
    uint8_t rxFrame_[kMaxFrame]; // not on the kernel stack (the poll timer runs in a 4 KB frame budget)
};
} // namespace AirportAwdl
