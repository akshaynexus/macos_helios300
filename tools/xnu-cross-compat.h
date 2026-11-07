/* SPDX-License-Identifier: GPL-2.0-only
 *
 * xnu-cross-compat.h — force-included (-include) into every TU by
 * cross-build-kext.sh. Only for building kexts with upstream LLVM clang.
 *
 * The macOS 26 SDK's <i386/trap.h> emits the fortify trap
 * (XNU_HARD_TRAP_STRING_CHK) as asm("ud1l %0(%%eax), %%eax" : : "p"(code)).
 * Apple clang prints a "p" constant as a bare displacement ("0xbffe(%eax)").
 * Upstream LLVM prints "$0xbffe" and rejects the operand ("invalid operand
 * in inline asm"), in any TU where a fortified memcpy/bcopy/strlcpy check is
 * instantiated. "%c0" with an "i" operand produces the same instruction Apple
 * clang does, so the xnu fortify bounds checks stay enabled. <i386/trap.h> is
 * include-guarded, and ml_fatal_trap expands ml_recoverable_trap at its use
 * site, so this definition wins.
 *
 * Found and fixed by the wifi kext (kexts/wifi/src/kext/Ath10kXnuCompat.h).
 */
#pragma once
#if defined(KERNEL) && defined(__x86_64__) && !defined(__apple_build_version__)
#include <i386/trap.h>
#undef ml_recoverable_trap
#define ml_recoverable_trap(code) \
	__asm__ volatile ("ud1l %c0(%%eax), %%eax" : : "i"(code))
#endif
