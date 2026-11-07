// SPDX-License-Identifier: GPL-2.0-or-later
//
// qca-rome-usb: run the portable QCA Rome core (src/core/qca_rome.c) against
// the real chip from Linux userspace, through usbdevfs (no libusb needed).
// This is the same state machine the kext runs; only the transport differs.
//
//   qca-rome-usb [-d /dev/bus/usb/BBB/DDD] probe
//   qca-rome-usb [-d ...] setup RAMPATCH.bin NVM.bin
//   qca-rome-usb [-d ...] reset            (USBDEVFS_RESET = USB port reset)
//
// Without -d the first 04ca:3016 is used. btusb must not own interface 0:
//   echo 1-7:1.0 | sudo tee /sys/bus/usb/drivers/btusb/unbind
// Every transfer is printed as a trace line in the format
// tools/usbmon_reduce.py produces, so a run can be diffed against a usbmon
// capture of Linux btusb doing the same job.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "qca_rome.h"
#include "sha256.h"

static int g_fd = -1;

static void hex(const uint8_t *p, uint32_t n, char *out)
{
    for (uint32_t i = 0; i < n; i++)
        sprintf(out + 2 * i, "%02x", p[i]);
    out[2 * n] = 0;
}

static void trace_payload(const char *tag, const char *what, const uint8_t *p,
                          uint32_t n, int inline_data)
{
    if (!n) {
        printf("%s %s len=0\n", tag, what);
        return;
    }
    if (inline_data) {
        char h[2 * 64 + 1];
        hex(p, n > 64 ? 64 : n, h);
        printf("%s %s len=%u data=%s\n", tag, what, n, h);
    } else {
        uint8_t d[32];
        char h[65];
        sha256(p, n, d);
        hex(d, 32, h);
        printf("%s %s len=%u sha256=%s\n", tag, what, n, h);
    }
}

static int ctrl(uint8_t rt, uint8_t req, void *buf, uint16_t len, uint32_t *actual,
                uint32_t to)
{
    struct usbdevfs_ctrltransfer c = {
        .bRequestType = rt, .bRequest = req, .wValue = 0, .wIndex = 0,
        .wLength = len, .timeout = to, .data = buf,
    };
    char what[64];
    snprintf(what, sizeof(what), "ctrl %02x %02x 0000 0000 %04x", rt, req, len);
    if (!(rt & 0x80))
        trace_payload("S", what, buf, len, 0);
    else
        printf("S %s\n", what);
    int r = ioctl(g_fd, USBDEVFS_CONTROL, &c);
    if (r < 0) {
        printf("C %s status=%d\n", what, -errno);
        *actual = 0;
        return -errno;
    }
    *actual = (uint32_t)r;
    if (rt & 0x80)
        trace_payload("C", what, buf, (uint32_t)r, 1);
    else
        printf("C %s status=0 len=%d\n", what, r);
    return 0;
}

static int t_ctrl_in(void *ctx, uint8_t req, void *buf, uint16_t len, uint32_t *actual,
                     uint32_t to)
{
    (void)ctx;
    return ctrl(0xc0, req, buf, len, actual, to);
}

static int t_ctrl_out(void *ctx, uint8_t req, const void *buf, uint16_t len,
                      uint32_t *actual, uint32_t to)
{
    (void)ctx;
    uint8_t tmp[64];
    if (len > sizeof(tmp))
        return -EINVAL;
    memcpy(tmp, buf, len);
    return ctrl(0x40, req, tmp, len, actual, to);
}

static int t_bulk_out(void *ctx, uint8_t ep, const void *buf, uint32_t len,
                      uint32_t *actual, uint32_t to)
{
    (void)ctx;
    char what[32];
    snprintf(what, sizeof(what), "bulk %02x", ep);
    trace_payload("S", what, buf, len, 0);
    struct usbdevfs_bulktransfer b = {
        .ep = ep, .len = len, .timeout = to, .data = (void *)buf,
    };
    int r = ioctl(g_fd, USBDEVFS_BULK, &b);
    if (r < 0) {
        printf("C %s status=%d\n", what, -errno);
        *actual = 0;
        return -errno;
    }
    *actual = (uint32_t)r;
    printf("C %s status=0 len=%d\n", what, r);
    return 0;
}

static void t_sleep(void *ctx, uint32_t ms)
{
    (void)ctx;
    printf("# sleep %u ms\n", ms);
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void t_log(void *ctx, const char *msg, uint32_t a, uint32_t b, uint32_t c)
{
    (void)ctx;
    printf("# ");
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
    printf(msg, a, b, c);
#pragma GCC diagnostic pop
    printf("\n");
}

static int find_device(char *path, size_t n)
{
    DIR *d = opendir("/sys/bus/usb/devices");
    struct dirent *e;
    int found = 0;
    if (!d)
        return -1;
    while (!found && (e = readdir(d))) {
        char p[512], v[16] = "", pr[16] = "", bus[16] = "", dev[16] = "";
        FILE *f;
#define RD(name, buf)                                                        \
        snprintf(p, sizeof(p), "/sys/bus/usb/devices/%s/" name, e->d_name);  \
        if ((f = fopen(p, "r"))) { if (!fgets(buf, sizeof(buf), f)) buf[0] = 0; fclose(f); }
        RD("idVendor", v);
        RD("idProduct", pr);
        if (strncmp(v, "04ca", 4) || strncmp(pr, "3016", 4))
            continue;
        RD("busnum", bus);
        RD("devnum", dev);
#undef RD
        snprintf(path, n, "/dev/bus/usb/%03d/%03d", atoi(bus), atoi(dev));
        found = 1;
    }
    closedir(d);
    return found ? 0 : -1;
}

static int load(const char *path, qca_firmware *fw)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc((size_t)n);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(p);
        return -1;
    }
    fclose(f);
    fw->data = p;
    fw->size = (size_t)n;
    uint8_t d[32];
    char h[65];
    sha256(p, (size_t)n, d);
    hex(d, 32, h);
    printf("# firmware %s %ld bytes sha256=%s\n", path, n, h);
    return 0;
}

int main(int argc, char **argv)
{
    char path[64] = "";
    int ai = 1;
    if (argc > 2 && !strcmp(argv[1], "-d")) {
        snprintf(path, sizeof(path), "%s", argv[2]);
        ai = 3;
    }
    if (ai >= argc) {
        fprintf(stderr, "usage: %s [-d DEV] probe | setup RAMPATCH NVM | reset\n", argv[0]);
        return 2;
    }
    if (!path[0] && find_device(path, sizeof(path))) {
        fprintf(stderr, "no 04ca:3016 found\n");
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    g_fd = open(path, O_RDWR);
    if (g_fd < 0) {
        perror(path);
        return 1;
    }
    printf("# device %s\n", path);

    if (!strcmp(argv[ai], "reset")) {
        int r = ioctl(g_fd, USBDEVFS_RESET, 0);
        printf("# USBDEVFS_RESET -> %d (%s)\n", r, r < 0 ? strerror(errno) : "ok");
        return r < 0;
    }

    unsigned int ifc = 0;
    if (ioctl(g_fd, USBDEVFS_CLAIMINTERFACE, &ifc) < 0) {
        fprintf(stderr, "claim interface 0: %s (unbind btusb first)\n", strerror(errno));
        return 1;
    }

    qca_transport t = {
        .ctx = NULL, .control_in = t_ctrl_in, .control_out = t_ctrl_out,
        .bulk_out = t_bulk_out, .sleep_ms = t_sleep, .log = t_log,
    };
    qca_firmware rp = { 0 }, nv = { 0 };
    qca_options opt = { .probe_only = 1 };
    if (!strcmp(argv[ai], "setup")) {
        if (ai + 2 >= argc || load(argv[ai + 1], &rp) || load(argv[ai + 2], &nv)) {
            fprintf(stderr, "setup needs readable RAMPATCH and NVM files\n");
            return 2;
        }
        opt.probe_only = 0;
    } else if (strcmp(argv[ai], "probe")) {
        fprintf(stderr, "unknown command %s\n", argv[ai]);
        return 2;
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    qca_result r;
    qca_err e = qca_rome_setup(&t, &rp, &nv, &opt, &r);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    printf("# result %s stage=%s err=\"%s\" transport=%d\n", e ? "FAIL" : "OK",
           qca_stage_name(r.stage), qca_strerror(e), r.transport_rc);
    printf("# rom=0x%08x patch_before=0x%x patch_after=0x%x ram=0x%08x chip=0x%02x "
           "platform=0x%02x flag=0x%04x\n",
           r.version_initial.rom_version, r.version_initial.patch_version,
           r.version_after.patch_version, r.version_initial.ram_version,
           r.version_initial.chip_id, r.version_initial.platform_id,
           r.version_initial.flag);
    printf("# status 0x%02x -> 0x%02x rampatch=%d nvm=%d bulk=%u in %ld ms\n",
           r.status_initial, r.status_final, r.rampatch_downloaded, r.nvm_downloaded,
           r.bulk_packets, ms);
    ioctl(g_fd, USBDEVFS_RELEASEINTERFACE, &ifc);
    close(g_fd);
    return e ? 1 : 0;
}
