// SPDX-License-Identifier: GPL-3.0-or-later
// Offline fixture. Replace the Keychain calls and NSURLSession transport so
// this binary never reads a real account or contacts the Nextendo service.
#import <Security/Security.h>
#import <Foundation/Foundation.h>
#import <assert.h>
static NSData *stored;
static OSStatus readItem(CFDictionaryRef q, CFTypeRef *out) {
    if (!stored)
        return errSecItemNotFound;
    *out = CFBridgingRetain(stored);
    return errSecSuccess;
}
static OSStatus updateItem(CFDictionaryRef q, CFDictionaryRef attrs) {
    if (!stored)
        return errSecItemNotFound;
    stored = ((__bridge NSDictionary *)attrs)[(__bridge id)kSecValueData];
    return errSecSuccess;
}
static OSStatus addItem(CFDictionaryRef q, CFTypeRef *result) {
    stored = ((__bridge NSDictionary *)q)[(__bridge id)kSecValueData];
    return errSecSuccess;
}
static OSStatus deleteItem(CFDictionaryRef q) {
    stored = nil;
    return errSecSuccess;
}
#define SecItemCopyMatching readItem
#define SecItemUpdate updateItem
#define SecItemAdd addItem
#define SecItemDelete deleteItem
#define DRIPPU_NEXTENDO_CLIENT_ID "nxc_drippu_fixture"
#import "../../dist/nxnative/nextendo/session.m"

// Catalog signing is exercised against the public envelope separately. Keep
// this credential fixture deterministic as the published catalog rotates.
int drippu_verify_catalog(const uint8_t *data, size_t len, const uint8_t *sig, size_t n) { return 0; }
static unsigned rotations, gameRequests, revoked;
static BOOL limit;
@interface MatchmakingFixture : NSURLProtocol
@end
@implementation MatchmakingFixture
+ (BOOL)canInitWithRequest:(NSURLRequest *)r {
    return YES;
}
+ (NSURLRequest *)canonicalRequestForRequest:(NSURLRequest *)r {
    return r;
}
- (void)stopLoading {
}
- (void)startLoading {
    NSURLRequest *r = self.request;
    assert([r.URL.host isEqual:@"nextendo.network"]);
    assert([[r valueForHTTPHeaderField:@"X-Nextendo-Client-Id"] isEqual:client()]);
    NSInteger status = 200;
    NSDictionary *json = nil;
    if ([r.URL.path isEqual:@"/offline-fixture"]) {
        [self.client URLProtocol:self didFailWithError:[NSError errorWithDomain:NSURLErrorDomain
                                                                          code:NSURLErrorNotConnectedToInternet
                                                                      userInfo:nil]];
        return;
    }
    if ([r.URL.path isEqual:@"/api/oauth/token"]) {
        assert([r.HTTPMethod isEqual:@"POST"]);
        rotations++;
        json = @{
            @"access_token" : @"website",
            @"refresh_token" : [NSString stringWithFormat:@"rotate-%u", rotations],
            @"expires_in" : @3600
        };
    } else if ([r.URL.path isEqual:@"/api/nex-token"]) {
        assert([r.HTTPMethod isEqual:@"GET"]);
        assert([[r valueForHTTPHeaderField:@"Authorization"] isEqual:@"Bearer website"]);
        gameRequests++;
        status = limit ? 429 : 200;
        json = @{@"pid" : @1234, @"nex_token" : @"nx2.fixture", @"expires_in" : @86400};
    } else if ([r.URL.path isEqual:@"/api/nex-session"]) {
        assert([[r valueForHTTPHeaderField:@"Authorization"] isEqual:@"Bearer nx2.fixture"]);
        json = @{};
    } else {
        assert([r.URL.path isEqual:@"/api/oauth/revoke"]);
        assert([sessionLock tryLock]);
        assert(!stored && !gameToken && !pid);
        [sessionLock unlock];
        status = 429;
        revoked++;
        json = @{};
    }
    NSHTTPURLResponse *reply = [[NSHTTPURLResponse alloc] initWithURL:r.URL
                                                           statusCode:status
                                                          HTTPVersion:@"HTTP/1.1"
                                                         headerFields:@{@"Retry-After" : @"30"}];
    [self.client URLProtocol:self
          didReceiveResponse:reply
          cacheStoragePolicy:NSURLCacheStorageNotAllowed];
    [self.client URLProtocol:self
                 didLoadData:[NSJSONSerialization dataWithJSONObject:json options:0 error:NULL]];
    [self.client URLProtocolDidFinishLoading:self];
}
@end
int main(void) {
    @autoreleasepool {
        initialize();
        catalogUntil = now() + 3600;
        NSURLSessionConfiguration *config = NSURLSessionConfiguration.ephemeralSessionConfiguration;
        config.protocolClasses = @[ MatchmakingFixture.class ];
        http = [NSURLSession sessionWithConfiguration:config];
        stored = [@"first" dataUsingEncoding:NSUTF8StringEncoding];
        dispatch_group_t group = dispatch_group_create();
        dispatch_group_async(group, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
          NSInteger status;
          [sessionLock lock];
          assert(!api(@"/offline-fixture", @"GET", nil, nil, &status));
          assert(status == -1 && [failure containsString:@"network connection"]);
          [sessionLock unlock];
          assert(drippu_session_ensure(0) && drippu_session_pid() == 1234);
          assert([refreshToken() isEqual:@"rotate-1"] && rotations == 1);
          char token[4096];
          uint64_t length;
          assert(!drippu_session_id_token(token, 1, &length) && length > 1);
          assert(drippu_session_id_token(token, sizeof token, &length));
          NSString *encoded = [[[NSString alloc] initWithBytes:token
                                                        length:length
                                                      encoding:NSASCIIStringEncoding]
              componentsSeparatedByString:@"."][1];
          NSString *base = [[encoded stringByReplacingOccurrencesOfString:@"-" withString:@"+"]
              stringByReplacingOccurrencesOfString:@"_"
                                        withString:@"/"];
          while (base.length % 4)
              base = [base stringByAppendingString:@"="];
          NSDictionary *claims = [NSJSONSerialization
              JSONObjectWithData:[[NSData alloc] initWithBase64EncodedString:base options:0]
                         options:0
                           error:NULL];
          assert([claims[@"nnex"] isEqual:@"nx2.fixture"] && [claims[@"tv"] isEqual:@"4.0.1"]);
          assert(gameRequests == 1);
          accessUntil = gameUntil = 0;
          assert(drippu_session_ensure(0) && rotations == 2 && [refreshToken() isEqual:@"rotate-2"]);
          gameUntil = 0;
          limit = YES;
          assert(!drippu_session_ensure(0) && !drippu_session_ensure(0));
          assert(gameRequests == 3);
          NSTimeInterval previousRetry = retryAfter;
          NSString *previousError = failure;
          drippu_session_logout();
          assert(retryAfter == previousRetry && failure == previousError);
          assert(!stored && revoked == 1 && drippu_session_pid() == 0);
        });
        assert(dispatch_group_wait(group, dispatch_time(DISPATCH_TIME_NOW, 15LL * NSEC_PER_SEC)) == 0);
        puts("Nextendo credential rotation, token claims, rate limiting and revocation passed");
    }
}
