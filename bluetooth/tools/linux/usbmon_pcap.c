// SPDX-License-Identifier: GPL-2.0-or-later
//
// usbmon-pcap: capture one USB bus from /dev/usbmonN (binary usbmon API,
// Documentation/usb/usbmon.rst) into a pcap file (LINKTYPE_USB_LINUX_MMAPPED,
// 220) with full payloads — the text interface truncates data to 32 bytes.
// No tcpdump/libpcap needed.
//
//   usbmon-pcap /dev/usbmon1 out.pcap SECONDS [DEVNUM]
//
// Runs for SECONDS, or until SIGINT/SIGTERM. With DEVNUM only that device's
// URBs are written. Needs root and `modprobe usbmon`.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

struct mon_bin_hdr { /* 64 bytes: the "mmapped" header pcap type 220 expects */
    uint64_t id;
    uint8_t type, xfer_type, epnum, devnum;
    uint16_t busnum;
    char flag_setup, flag_data;
    int64_t ts_sec;
    int32_t ts_usec;
    int32_t status;
    uint32_t len_urb, len_cap;
    uint8_t setup[8];
    int32_t interval, start_frame;
    uint32_t xfer_flags, ndesc;
};

struct mon_bin_get {
    struct mon_bin_hdr *hdr;
    void *data;
    size_t alloc;
};

#define MON_IOC_MAGIC 0x92
#define MON_IOCT_RING_SIZE _IO(MON_IOC_MAGIC, 4)
#define MON_IOCX_GETX _IOW(MON_IOC_MAGIC, 10, struct mon_bin_get)

static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s /dev/usbmonN out.pcap SECONDS [DEVNUM]\n", argv[0]);
        return 2;
    }
    _Static_assert(sizeof(struct mon_bin_hdr) == 64, "mon_bin_hdr layout");
    int secs = atoi(argv[3]);
    int devnum = argc > 4 ? atoi(argv[4]) : -1;
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        perror(argv[1]);
        return 1;
    }
    if (ioctl(fd, MON_IOCT_RING_SIZE, 1200 * 1024) < 0)
        perror("MON_IOCT_RING_SIZE (continuing with default ring)");
    FILE *out = fopen(argv[2], "wb");
    if (!out) {
        perror(argv[2]);
        return 1;
    }
    put32(out, 0xa1b2c3d4); put16(out, 2); put16(out, 4);
    put32(out, 0); put32(out, 0); put32(out, 262144); put32(out, 220);
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    fprintf(stderr, "usbmon-pcap: capturing %s for %d s (dev %d)\n", argv[1], secs, devnum);
    fflush(stderr);

    static uint8_t data[65536];
    struct mon_bin_hdr h;
    struct timespec t0, now;
    unsigned long n = 0, dropped_cap = 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!g_stop) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - t0.tv_sec >= secs)
            break;
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int pr = poll(&p, 1, 100);
        if (pr <= 0)
            continue;
        struct mon_bin_get g = { .hdr = &h, .data = data, .alloc = sizeof(data) };
        if (ioctl(fd, MON_IOCX_GETX, &g) < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            perror("MON_IOCX_GETX");
            break;
        }
        if (devnum >= 0 && h.devnum != devnum)
            continue;
        if (h.len_cap < h.len_urb && h.flag_data == 0)
            dropped_cap++;
        put32(out, (uint32_t)h.ts_sec); put32(out, (uint32_t)h.ts_usec);
        put32(out, 64 + h.len_cap); put32(out, 64 + h.len_urb);
        fwrite(&h, 64, 1, out);
        fwrite(data, 1, h.len_cap, out);
        n++;
    }
    fclose(out);
    fprintf(stderr, "usbmon-pcap: %lu records, %lu with truncated data\n", n, dropped_cap);
    return 0;
}
