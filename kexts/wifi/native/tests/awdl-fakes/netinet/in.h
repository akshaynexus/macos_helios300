#pragma once
#include "../fake_kernel.hpp"
#define AF_INET6 30
struct in6_addr{unsigned char s6_addr[16];};
struct sockaddr_in6{unsigned char sin6_len,sin6_family;unsigned short sin6_port;unsigned sin6_flowinfo;struct in6_addr sin6_addr;unsigned sin6_scope_id;};
