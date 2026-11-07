// SPDX-License-Identifier: GPL-2.0-or-later
//
// Golden replay: run the core against transfer traces recorded from the real
// Helios 300 BT chip (tests/golden/*.trace: usbmon captures of Linux btusb,
// reduced by tools/usbmon_reduce.py, and runs of our core on the chip via
// tools/linux/qca_rome_usb.c). The replay device answers each request with
// the recorded completion, and every request the core makes must equal the
// recorded one: setup fields, length and payload SHA-256 (for downloads this
// proves byte-identical firmware packets to Linux's).
//
// Directives in the trace header:
//   # replay: probe-only                 run with probe_only = 1
//   # replay: allow-extra-final-status[=XX]  the core may append one
//                                        read-only CHECK_STATUS after the
//                                        recording (Linux does not re-check
//                                        status); it is answered with XX, or
//                                        with the last recorded status
//
// Usage: test_golden FILE.trace...   (exit 0 = all match)
#include "qca_rome.h"
#include "sha256.h"

#if defined(QCA_HAVE_FIRMWARE)
#include "qca_rome_firmware.h"
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string hex(const uint8_t *p, size_t n)
{
    static const char *d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

std::string sha(const void *p, size_t n)
{
    uint8_t d[32];
    sha256(p, n, d);
    return hex(d, 32);
}

std::vector<uint8_t> unhex(const std::string &s)
{
    std::vector<uint8_t> v;
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        v.push_back(uint8_t(std::stoul(s.substr(i, 2), nullptr, 16)));
    return v;
}

std::string field(const std::string &line, const std::string &key)
{
    size_t p = line.find(" " + key + "=");
    if (p == std::string::npos)
        return "";
    p += key.size() + 2;
    size_t e = line.find(' ', p);
    return line.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

struct Replay {
    std::vector<std::string> lines; // S/C lines only
    size_t pos = 0;
    bool allow_extra_status = false;
    int extra_status = -1; // -1: answer with the last recorded status
    uint8_t last_status = 0;
    int extra_used = 0;
    std::vector<std::string> errors;
    std::vector<std::string> ours; // what the core did, same format

    // Expect the next recorded submit to equal `s`; return the matching
    // completion line ("" = beyond recording).
    std::string submit(const std::string &s, bool is_status_read)
    {
        ours.push_back(s);
        if (pos >= lines.size()) {
            if (allow_extra_status && is_status_read && extra_used == 0) {
                extra_used = 1;
                char c[96];
                std::snprintf(c, sizeof(c), "C ctrl c0 05 0000 0000 0001 len=1 data=%02x",
                              extra_status >= 0 ? extra_status : last_status);
                return c;
            }
            errors.push_back("extra request beyond recording: " + s);
            return "";
        }
        const std::string &want = lines[pos];
        if (want != s) {
            errors.push_back("request " + std::to_string(pos) + ": got  '" + s + "'\n" +
                             "                 want '" + want + "'");
            pos = lines.size();
            return "";
        }
        pos++;
        if (pos >= lines.size() || lines[pos][0] != 'C') {
            errors.push_back("recording has no completion for: " + s);
            return "";
        }
        return lines[pos++];
    }
};

Replay *g;

int complete_status(const std::string &c)
{
    std::string st = field(c, "status");
    return st.empty() ? 0 : std::stoi(st);
}

int r_ctrl_in(void *, uint8_t req, void *buf, uint16_t len, uint32_t *actual, uint32_t)
{
    char s[64];
    std::snprintf(s, sizeof(s), "S ctrl c0 %02x 0000 0000 %04x", req, len);
    std::string c = g->submit(s, req == QCA_REQ_CHECK_STATUS);
    *actual = 0;
    if (c.empty())
        return -1000;
    if (int st = complete_status(c))
        return st;
    std::vector<uint8_t> d = unhex(field(c, "data"));
    size_t n = std::min<size_t>(d.size(), len);
    std::memcpy(buf, d.data(), n);
    *actual = uint32_t(std::stoul(field(c, "len")));
    if (*actual > len)
        *actual = len;
    if (req == QCA_REQ_CHECK_STATUS && n == 1)
        g->last_status = d[0];
    char o[160];
    std::snprintf(o, sizeof(o), "C ctrl c0 %02x 0000 0000 %04x len=%u data=%s", req, len,
                  *actual, hex(d.data(), n).c_str());
    g->ours.push_back(o);
    return 0;
}

int r_ctrl_out(void *, uint8_t req, const void *buf, uint16_t len, uint32_t *actual, uint32_t)
{
    char s[160];
    std::snprintf(s, sizeof(s), "S ctrl 40 %02x 0000 0000 %04x len=%u sha256=%s", req, len, len,
                  sha(buf, len).c_str());
    std::string c = g->submit(s, false);
    *actual = 0;
    if (c.empty())
        return -1000;
    if (int st = complete_status(c))
        return st;
    *actual = uint32_t(std::stoul(field(c, "len")));
    return 0;
}

int r_bulk(void *, uint8_t ep, const void *buf, uint32_t len, uint32_t *actual, uint32_t)
{
    char s[160];
    std::snprintf(s, sizeof(s), "S bulk %02x len=%u sha256=%s", ep, len, sha(buf, len).c_str());
    std::string c = g->submit(s, false);
    *actual = 0;
    if (c.empty())
        return -1000;
    if (int st = complete_status(c))
        return st;
    *actual = uint32_t(std::stoul(field(c, "len")));
    return 0;
}

void r_sleep(void *, uint32_t) {}

int run(const char *path)
{
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    Replay rp;
    bool probe_only = false;
    std::string line;
    bool needs_fw = false;
    while (std::getline(f, line)) {
        if (line.rfind("# replay:", 0) == 0) {
            if (line.find("probe-only") != std::string::npos)
                probe_only = true;
            size_t k = line.find("allow-extra-final-status");
            if (k != std::string::npos) {
                rp.allow_extra_status = true;
                // Optional "=XX": the status the chip returns at that point
                // (Linux never reads it, so the recording cannot tell).
                k += std::strlen("allow-extra-final-status");
                if (k < line.size() && line[k] == '=')
                    rp.extra_status = uint8_t(std::stoul(line.substr(k + 1, 2), nullptr, 16));
            }
            continue;
        }
        if (line.empty() || line[0] == '#')
            continue;
        if (line[0] == 'S' || line[0] == 'C') {
            if (line.rfind("S bulk", 0) == 0)
                needs_fw = true;
            rp.lines.push_back(line);
        }
    }
    std::printf("[golden %s: %zu recorded lines%s%s]\n", path, rp.lines.size(),
                probe_only ? ", probe-only" : "", needs_fw ? ", download" : "");
#if !defined(QCA_HAVE_FIRMWARE)
    if (needs_fw) {
        std::puts("  SKIPPED: download trace needs the embedded firmware build");
        return 0;
    }
    qca_firmware rpf{nullptr, 0}, nvf{nullptr, 0};
#else
    qca_firmware rpf{qca_rampatch_usb_00000302, qca_rampatch_usb_00000302_len};
    qca_firmware nvf{qca_nvm_usb_00000302, qca_nvm_usb_00000302_len};
#endif
    g = &rp;
    qca_transport t{};
    t.control_in = r_ctrl_in;
    t.control_out = r_ctrl_out;
    t.bulk_out = r_bulk;
    t.sleep_ms = r_sleep;
    qca_options o{probe_only ? 1 : 0};
    qca_result r;
    qca_err e = qca_rome_setup(&t, &rpf, &nvf, &o, &r);
    if (rp.pos != rp.lines.size())
        rp.errors.push_back("core stopped after " + std::to_string(rp.pos) + " of " +
                            std::to_string(rp.lines.size()) + " recorded lines (" +
                            qca_strerror(e) + " at " + qca_stage_name(r.stage) + ")");
    if (e != QCA_OK)
        rp.errors.push_back(std::string("setup returned: ") + qca_strerror(e));
    for (auto &er : rp.errors)
        std::fprintf(stderr, "  FAIL %s\n", er.c_str());
    if (rp.errors.empty())
        std::printf("  OK: %zu requests identical; rom 0x%x patch 0x%x status 0x%02x->0x%02x, "
                    "rampatch=%d nvm=%d bulk=%u%s\n",
                    rp.lines.size() / 2, r.version_initial.rom_version,
                    r.version_after.patch_version, r.status_initial, r.status_final,
                    r.rampatch_downloaded, r.nvm_downloaded, r.bulk_packets,
                    rp.extra_used ? " (+1 final CHECK_STATUS)" : "");
    g = nullptr;
    return rp.errors.empty() ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
    int fails = 0;
    for (int i = 1; i < argc; i++)
        fails += run(argv[i]);
    std::printf("%d golden trace(s), %d failing\n", argc - 1, fails);
    return fails ? 1 : 0;
}
