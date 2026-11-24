// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <netdb.h>
#include <netinet/in.h>
int drippu_addrinfo(const char *host, const char *service, const void *hints, void **result);
void drippu_freeaddrinfo(void *result);
struct hostent *drippu_host(const char *name);
int drippu_ip(uint32_t *address, uint32_t *mask, uint32_t *broadcast);
int drippu_dup_socket(int guest_fd);
int drippu_game_host(const char *name);
