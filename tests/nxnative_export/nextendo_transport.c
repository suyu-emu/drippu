// SPDX-License-Identifier: GPL-3.0-or-later
// This fixture calls the SDK hooks used by the native runtime against localhost.
#include "hle_net.c"
#include <arpa/inet.h>
#include <assert.h>
#include <time.h>
__thread _Atomic int dm_host_work;
void dm_guest_sleep_ns(int64_t ns) {
    struct timespec t = {ns / 1000000000, ns % 1000000000};
    nanosleep(&t, NULL);
}
void dm_guest_cond_wait_ns(pthread_cond_t *c, pthread_mutex_t *m, int64_t ns) {
    struct timespec t = {ns / 1000000000, ns % 1000000000};
    pthread_cond_timedwait_relative_np(c, m, &t);
}
uint64_t guest_ns(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }
void drippu_session_activity(void) {} // no account in this transport fixture
int dm_detclock_enabled(void) { return 0; }

int main(int argc, char **argv) {
    assert(argc == 3);
    struct sockaddr_in address = {
        .sin_len = sizeof address, .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    int rx = dm_sk_socket(2, 2, 17), tx = dm_sk_socket(2, 2, 17);
    assert(rx >= 0 && tx >= 0 && rx != tx);
    assert(!dm_sk_bind(rx, &address, sizeof address));
    unsigned size = sizeof address;
    assert(!dm_sk_name(rx, &address, &size, 0) && address.sin_port);
    assert(dm_sk_sendto(tx, "probe", 5, 0, &address, sizeof address) == 5);
    sk_pollfd p = {rx, SK_POLLIN, 0};
    assert(dm_sk_poll(&p, 1, 1000) == 1 && (p.revents & SK_POLLIN));
    char buf[16] = {0};
    assert(dm_sk_recvfrom(rx, buf, sizeof buf, 0, NULL, NULL) == 5 && !memcmp(buf, "probe", 5));
    assert(!dm_sk_close(rx) && !dm_sk_close(tx));
    assert(drippu_dup_socket(rx) < 0);

    const char *host = argv[2];
    address.sin_port = htons((uint16_t)atoi(argv[1]));
    int fd = dm_sk_socket(2, 1, 6);
    assert(fd >= 0 && !dm_sk_connect(fd, &address, sizeof address));
    int key;
    assert(!drippu_tls_create(&key));
    assert(!drippu_tls_socket(&key, fd));
    assert(drippu_tls_handshake(&key)); // TLS requires the original service hostname.
    assert(!drippu_tls_host(&key, host, (unsigned)strlen(host)));
    uint32_t result = drippu_tls_handshake(&key);
    if (!strcmp(host, "fixture.nintendo.net")) {
        assert(!result);
        assert(drippu_tls_io(&key, "hello", 5, 1) == 5);
        int n = 0;
        assert(!drippu_tls_peek(&key, buf, &n, 5) && n == 5);
        assert(drippu_tls_io(&key, buf, 5, 0) == 5 && !memcmp(buf, "world", 5));
    } else {
        assert(result && drippu_tls_io(&key, "secret", 6, 1) < 0);
    }
    drippu_tls_destroy(&key);
    assert(!dm_sk_close(fd));
    return 0;
}
