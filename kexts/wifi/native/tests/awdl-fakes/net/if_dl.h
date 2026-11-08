#pragma once
#include "../fake_kernel.hpp"
#define AF_LINK 18
struct sockaddr_dl{unsigned char sdl_len,sdl_family;unsigned short sdl_index;unsigned char sdl_type,sdl_nlen,sdl_alen,sdl_slen;char sdl_data[12];};
#define LLADDR(s) ((caddr_t)((s)->sdl_data+(s)->sdl_nlen))
