// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
uint32_t drippu_tls_create(void *key);
void drippu_tls_destroy(void *key);
uint32_t drippu_tls_host(void *key, const char *name, unsigned length);
uint32_t drippu_tls_socket(void *key, int guest);
uint32_t drippu_tls_handshake(void *key);
int drippu_tls_io(void *key, void *buffer, unsigned size, int writing);
uint32_t drippu_tls_last(void *key);
uint32_t drippu_tls_mode(void *key, int mode);
uint32_t drippu_tls_peek(void *key, void *buffer, int *length, unsigned size);
uint32_t drippu_tls_poll(void *key, unsigned *out, const unsigned *events, unsigned timeout);
