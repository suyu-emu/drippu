// SPDX-License-Identifier: GPL-3.0-or-later
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#import <Security/SecureTransport.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <limits.h>
#include <sys/socket.h>
#include <unistd.h>
#include "network.h"
#include "tls.h"
#include "session.h"
#include "drippu_nextendo_ca.h"

enum { failed = 123 | (999 << 9), wouldBlock = 123 | (204 << 9) };
static OSStatus transfer(SSLConnectionRef socketRef, void *data, size_t *length, BOOL writing) {
    int fd = (int)(intptr_t)socketRef;
    size_t wanted = *length;
    ssize_t count;
    do {
        count = writing ? send(fd, data, wanted, 0) : recv(fd, data, wanted, 0);
    } while (count < 0 && errno == EINTR);
    *length = count > 0 ? (size_t)count : 0;
    if (count < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK ? errSSLWouldBlock : errSSLClosedAbort;
    if (!count && wanted)
        return errSSLClosedGraceful;
    return (size_t)count == wanted ? noErr : errSSLWouldBlock;
}
static OSStatus tlsRead(SSLConnectionRef fd, void *data, size_t *n) { return transfer(fd, data, n, NO); }
static OSStatus tlsWrite(SSLConnectionRef fd, const void *data, size_t *n) {
    return transfer(fd, (void *)data, n, YES);
}
@interface DrippuTLSConnection : NSObject {
  @public
    SSLContextRef ctx;
    int fd;
    NSString *host;
    BOOL verified, rejected;
    int mode;
    uint32_t lastError;
    NSMutableData *peeked;
}
@end
@implementation DrippuTLSConnection
- (instancetype)init {
    if ((self = [super init])) {
        fd = -1;
        ctx = SSLCreateContext(NULL, kSSLClientSide, kSSLStreamType);
        if (!ctx || SSLSetProtocolVersionMin(ctx, kTLSProtocol12) ||
            SSLSetIOFuncs(ctx, tlsRead, tlsWrite) ||
            SSLSetALPNProtocols(ctx, (__bridge CFArrayRef) @[ @"http/1.1" ]) ||
            SSLSetSessionOption(ctx, kSSLSessionOptionBreakOnServerAuth, true))
            return nil;
    }
    return self;
}
- (void)dealloc {
    if (ctx)
        CFRelease(ctx);
    if (fd >= 0)
        close(fd);
}
@end
static NSMutableDictionary *connections(void) {
    static NSMutableDictionary *table;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
      table = [NSMutableDictionary new];
    });
    return table;
}
static DrippuTLSConnection *connection(void *key) {
    NSMutableDictionary *table = connections();
    @synchronized(table) {
        return table[[NSValue valueWithPointer:key]];
    }
}
uint32_t drippu_tls_create(void *key) {
    DrippuTLSConnection *conn = [DrippuTLSConnection new];
    if (!key || !conn)
        return failed;
    NSMutableDictionary *table = connections();
    @synchronized(table) {
        table[[NSValue valueWithPointer:key]] = conn;
    }
    return 0;
}
void drippu_tls_destroy(void *key) {
    NSMutableDictionary *table = connections();
    @synchronized(table) {
        [table removeObjectForKey:[NSValue valueWithPointer:key]];
    }
}
uint32_t drippu_tls_host(void *key, const char *name, unsigned length) {
    DrippuTLSConnection *c = connection(key);
    if (!c || !name || !length || length > 256)
        return failed;
    size_t len = strnlen(name, length);
    NSString *value = [[NSString alloc] initWithBytes:name length:len encoding:NSASCIIStringEncoding];
    if (!value || !drippu_game_host(value.UTF8String))
        return failed;
    @synchronized(c) {
        if (c->verified || SSLSetPeerDomainName(c->ctx, name, len))
            return failed;
        c->host = value;
        return 0;
    }
}
uint32_t drippu_tls_socket(void *key, int guest) {
    DrippuTLSConnection *c = connection(key);
    if (!c)
        return failed;
    @synchronized(c) {
        if (c->fd >= 0)
            return failed;
        c->fd = drippu_dup_socket(guest);
        if (c->fd >= 0 && c->mode)
            fcntl(c->fd, F_SETFL, O_NONBLOCK);
        return c->fd >= 0 && !SSLSetConnection(c->ctx, (void *)(intptr_t)c->fd) ? 0 : failed;
    }
}
static BOOL verify(DrippuTLSConnection *c) {
    SecTrustRef trust = NULL;
    if (SSLCopyPeerTrust(c->ctx, &trust) || !trust)
        return NO;
    NSData *root = [NSData dataWithBytes:drippu_ca length:sizeof drippu_ca];
    SecCertificateRef cert = SecCertificateCreateWithData(NULL, (__bridge CFDataRef)root);
    SecPolicyRef policy = SecPolicyCreateSSL(true, (__bridge CFStringRef)c->host);
    BOOL ok = cert && policy && !SecTrustSetPolicies(trust, policy) &&
              !SecTrustSetAnchorCertificates(trust, (__bridge CFArrayRef) @[ (__bridge id)cert ]) &&
              !SecTrustSetAnchorCertificatesOnly(trust, true) && SecTrustEvaluateWithError(trust, NULL);
    if (cert)
        CFRelease(cert);
    if (policy)
        CFRelease(policy);
    CFRelease(trust);
    return ok;
}
uint32_t drippu_tls_handshake(void *key) {
    DrippuTLSConnection *c = connection(key);
    if (!c)
        return failed;
    @synchronized(c) {
        if (c->fd < 0 || c->rejected || !c->host)
            return failed;
        OSStatus status = SSLHandshake(c->ctx);
        if (status == errSSLServerAuthCompleted) {
            c->verified = verify(c);
            if (!c->verified) {
                c->rejected = YES;
                return c->lastError = failed;
            }
            status = SSLHandshake(c->ctx);
        }
        return c->lastError = status == errSSLWouldBlock       ? wouldBlock
                              : status == noErr && c->verified ? 0
                                                               : failed;
    }
}
int drippu_tls_io(void *key, void *buffer, unsigned size, int writing) {
    DrippuTLSConnection *c = connection(key);
    if (!c || (!buffer && size))
        return -1;
    @synchronized(c) {
        if (!c->verified || c->rejected)
            return -1;
        if (!writing && c->peeked.length) {
            NSUInteger n = MIN(size, c->peeked.length);
            memcpy(buffer, c->peeked.bytes, n);
            [c->peeked replaceBytesInRange:NSMakeRange(0, n) withBytes:NULL length:0];
            return (int)n;
        }
        size_t count = 0;
        OSStatus status =
            writing ? SSLWrite(c->ctx, buffer, size, &count) : SSLRead(c->ctx, buffer, size, &count);
        c->lastError = status == errSSLWouldBlock                          ? wouldBlock
                       : status == noErr || status == errSSLClosedGraceful ? 0
                                                                           : failed;
        if (count)
            drippu_session_activity();
        return count || !c->lastError ? (int)count : -1;
    }
}
uint32_t drippu_tls_last(void *key) {
    DrippuTLSConnection *c = connection(key);
    if (!c)
        return failed;
    @synchronized(c) {
        return c->lastError;
    }
}

uint32_t drippu_tls_mode(void *key, int mode) {
    DrippuTLSConnection *c = connection(key);
    if (!c || (mode != 1 && mode != 2))
        return failed;
    @synchronized(c) {
        c->mode = mode == 2;
        return c->fd < 0 || !fcntl(c->fd, F_SETFL, c->mode ? O_NONBLOCK : 0) ? 0 : failed;
    }
}

uint32_t drippu_tls_peek(void *key, void *buffer, int *length, unsigned size) {
    DrippuTLSConnection *c = connection(key);
    if (!c || !length || (!buffer && size))
        return failed;
    *length = 0;
    @synchronized(c) {
        if (!c->verified || c->rejected)
            return failed;
        if (!c->peeked.length && size) {
            NSMutableData *temp = [NSMutableData dataWithLength:MIN(size, 16384u)];
            int n = drippu_tls_io(key, temp.mutableBytes, (unsigned)temp.length, 0);
            if (n < 0)
                return c->lastError;
            temp.length = (NSUInteger)n;
            c->peeked = temp;
        }
        *length = (int)MIN(size, c->peeked.length);
        if (*length)
            memcpy(buffer, c->peeked.bytes, (size_t)*length);
        return 0;
    }
}
uint32_t drippu_tls_poll(void *key, unsigned *out, const unsigned *events, unsigned timeout) {
    DrippuTLSConnection *c = connection(key);
    if (!c || !out || !events)
        return failed;
    @synchronized(c) {
        if (c->fd < 0)
            return failed;
        unsigned mask = *events;
        *out = 0;
        size_t buffered = 0;
        SSLGetBufferedReadSize(c->ctx, &buffered);
        if ((mask & 1) && (buffered || c->peeked.length)) {
            *out = 1;
            timeout = 0;
        }
        struct pollfd p = {c->fd, ((mask & 1) ? POLLIN : 0) | ((mask & 2) ? POLLOUT : 0), 0};
        if (poll(&p, 1, (int)MIN(timeout, (unsigned)INT_MAX)) < 0)
            return failed;
        if (p.revents & (POLLIN | POLLHUP))
            *out |= mask & 1;
        if (p.revents & POLLOUT)
            *out |= mask & 2;
        if (p.revents & (POLLERR | POLLNVAL))
            *out |= mask & 4;
        return 0;
    }
}
