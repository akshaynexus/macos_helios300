/* SPDX-License-Identifier: GPL-2.0-or-later
 * Minimal SHA-256 (FIPS 180-4) for host-side tools and tests only. */
#ifndef QCA_TEST_SHA256_H
#define QCA_TEST_SHA256_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void sha256(const void *data, size_t len, uint8_t out[32]);
#ifdef __cplusplus
}
#endif
#endif
