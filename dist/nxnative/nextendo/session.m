// SPDX-License-Identifier: GPL-3.0-or-later
#import <AuthenticationServices/AuthenticationServices.h>
#import <CommonCrypto/CommonDigest.h>
#import <Security/Security.h>
#import <TargetConditionals.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#else
#import <UIKit/UIKit.h>
#endif
#include <stdatomic.h>
#include "session.h"
#ifdef DRIPPU_DESKTOP_CRYPTO
#include <openssl/evp.h>
#endif
#ifndef DRIPPU_NEXTENDO_CLIENT_ID
#include "drippu_nextendo_config.h"
#endif

static NSString *const origin = @"https://nextendo.network";
static NSString *const callback = @"drippu-nextendo://oauth/callback";
static NSString *client(void) {
#ifdef DRIPPU_DESKTOP_CRYPTO
    NSString *override = NSProcessInfo.processInfo.environment[@"DRIPPU_NEXTENDO_CLIENT_ID"];
    if (override.length)
        return override;
#endif
    return @DRIPPU_NEXTENDO_CLIENT_ID;
}

@interface DrippuWebLogin
    : NSObject <ASWebAuthenticationPresentationContextProviding, NSURLSessionTaskDelegate>
@end
static ASPresentationAnchor presentationWindow(void) {
#if TARGET_OS_OSX
    return NSApp.keyWindow ?: NSApp.windows.firstObject;
#else
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if ([scene isKindOfClass:UIWindowScene.class] &&
            scene.activationState == UISceneActivationStateForegroundActive)
            return ((UIWindowScene *)scene).keyWindow;
    }
    return nil;
#endif
}
@implementation DrippuWebLogin
- (ASPresentationAnchor)presentationAnchorForWebAuthenticationSession:
    (ASWebAuthenticationSession *)session {
    return presentationWindow();
}
- (void)URLSession:(NSURLSession *)session
                          task:(NSURLSessionTask *)task
    willPerformHTTPRedirection:(NSHTTPURLResponse *)response
                    newRequest:(NSURLRequest *)request
             completionHandler:(void (^)(NSURLRequest *))completion {
    completion(nil);
}
@end

static NSLock *sessionLock;
static DrippuWebLogin *browserDelegate;
static NSURLSession *http;
static NSString *failure, *accessToken, *gameToken, *jwt;
static NSTimeInterval accessUntil, gameUntil, jwtUntil, retryAfter, heartbeatAfter;
static uint64_t pid;
static void initialize(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
      sessionLock = [NSLock new];
      browserDelegate = [DrippuWebLogin new];
      NSURLSessionConfiguration *config = NSURLSessionConfiguration.ephemeralSessionConfiguration;
      config.timeoutIntervalForRequest = 15;
      config.timeoutIntervalForResource = 30;
      http = [NSURLSession sessionWithConfiguration:config delegate:browserDelegate delegateQueue:nil];
    });
}
static NSString *base64url(NSData *data) {
    return [[[[data base64EncodedStringWithOptions:0] stringByReplacingOccurrencesOfString:@"+"
                                                                                withString:@"-"]
        stringByReplacingOccurrencesOfString:@"/"
                                  withString:@"_"] stringByReplacingOccurrencesOfString:@"="
                                                                             withString:@""];
}
static NSData *randomBytes(size_t length) {
    NSMutableData *data = [NSMutableData dataWithLength:length];
    return SecRandomCopyBytes(kSecRandomDefault, length, data.mutableBytes) == errSecSuccess ? data
                                                                                             : nil;
}
static NSData *sha256(NSData *value) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(value.bytes, (CC_LONG)value.length, digest);
    return [NSData dataWithBytes:digest length:sizeof digest];
}
static NSString *parameter(NSString *s) {
    NSCharacterSet *safe =
        [NSCharacterSet characterSetWithCharactersInString:
                            @"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~"];
    return [s stringByAddingPercentEncodingWithAllowedCharacters:safe];
}
static NSString *form(NSDictionary<NSString *, NSString *> *fields) {
    NSMutableArray *pairs = [NSMutableArray array];
    for (NSString *key in fields)
        [pairs addObject:[NSString stringWithFormat:@"%@=%@", parameter(key), parameter(fields[key])]];
    return [pairs componentsJoinedByString:@"&"];
}
static NSString *stringValue(id value) {
    return [value isKindOfClass:NSString.class] && [value length] ? value : nil;
}
static NSTimeInterval now(void) { return NSDate.date.timeIntervalSince1970; }
static NSDictionary *keyQuery(void) {
    NSString *service =
        [@"org.drippu.nextendo." stringByAppendingString:NSBundle.mainBundle.bundleIdentifier
                                                             ?: NSProcessInfo.processInfo.processName];
    return @{
        (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService : service,
        (__bridge id)kSecAttrAccount : client()
    };
}
static NSString *refreshToken(void) {
    NSMutableDictionary *query = [keyQuery() mutableCopy];
    query[(__bridge id)kSecReturnData] = @YES;
    CFTypeRef result = NULL;
    if (SecItemCopyMatching((__bridge CFDictionaryRef)query, &result) != errSecSuccess)
        return nil;
    return [[NSString alloc] initWithData:CFBridgingRelease(result) encoding:NSUTF8StringEncoding];
}
static BOOL storeRefresh(NSString *token) {
    NSDictionary *query = keyQuery();
    NSDictionary *attrs = @{
        (__bridge id)kSecValueData : [token dataUsingEncoding:NSUTF8StringEncoding],
        (__bridge id)kSecAttrAccessible : (__bridge id)kSecAttrAccessibleWhenUnlockedThisDeviceOnly
    };
    OSStatus result = SecItemUpdate((__bridge CFDictionaryRef)query, (__bridge CFDictionaryRef)attrs);
    if (result != errSecItemNotFound)
        return result == errSecSuccess;
    NSMutableDictionary *item = [query mutableCopy];
    [item addEntriesFromDictionary:attrs];
    return SecItemAdd((__bridge CFDictionaryRef)item, NULL) == errSecSuccess;
}
static NSDictionary *sendRequest(NSString *path, NSString *method, NSDictionary *fields,
                                 NSString *bearer, NSHTTPURLResponse **outResponse) {
    NSMutableURLRequest *req =
        [NSMutableURLRequest requestWithURL:[NSURL URLWithString:[origin stringByAppendingString:path]]];
    req.HTTPMethod = method;
    [req setValue:client() forHTTPHeaderField:@"X-Nextendo-Client-Id"];
    [req setValue:@"Drippu" forHTTPHeaderField:@"User-Agent"];
    if (bearer)
        [req setValue:[@"Bearer " stringByAppendingString:bearer] forHTTPHeaderField:@"Authorization"];
    if (fields) {
        req.HTTPBody = [form(fields) dataUsingEncoding:NSUTF8StringEncoding];
        [req setValue:@"application/x-www-form-urlencoded" forHTTPHeaderField:@"Content-Type"];
    }
    __block NSDictionary *json;
    __block NSHTTPURLResponse *response;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    NSURLSessionDataTask *task = [http dataTaskWithRequest:req
             completionHandler:^(NSData *data, NSURLResponse *reply, NSError *error) {
               if (!error && [reply isKindOfClass:NSHTTPURLResponse.class])
                   response = (NSHTTPURLResponse *)reply;
               id obj = data && !error
                            ? [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL]
                            : nil;
               if ([obj isKindOfClass:NSDictionary.class])
                   json = obj;
               dispatch_semaphore_signal(done);
             }];
    [task resume];
    // Allow the configured 30-second resource timeout to deliver its completion first.
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 35LL * NSEC_PER_SEC))) {
        [task cancel];
        if (outResponse)
            *outResponse = nil;
        return nil;
    }
    if (outResponse)
        *outResponse = response;
    return json;
}

// Called with sessionLock held. Only active-session requests update its error and retry state.
static NSDictionary *api(NSString *path, NSString *method, NSDictionary *fields, NSString *bearer,
                         NSInteger *status) {
    NSHTTPURLResponse *response = nil;
    NSDictionary *json = sendRequest(path, method, fields, bearer, &response);
    if (!response) {
        *status = -1;
        failure = @"Could not reach Nextendo. Check your network connection and try again.";
        return nil;
    }
    *status = response.statusCode;
    if (*status == 429)
        retryAfter = now() + MAX(1, [[response valueForHTTPHeaderField:@"Retry-After"] integerValue]);
    if (*status != 200) {
        failure =
            *status == 403
                ? @"Nextendo denied access. Check the game.matchmaking grant and account settings."
                : [NSString stringWithFormat:@"Nextendo request failed (HTTP %ld).", (long)*status];
    }
    return json;
}
#ifdef DRIPPU_DESKTOP_CRYPTO
static int drippu_verify_catalog(const uint8_t *data, size_t size, const uint8_t *sig, size_t sigSize) {
    NSData *keyData =
        [[NSData alloc] initWithBase64EncodedString:@"xI+atR6VesrCH89ZDZD4t3WQLcYOFEhk5F0HLMzgWk4="
                                            options:0];
    EVP_PKEY *key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, keyData.bytes, keyData.length);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    int valid = key && ctx && EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, key) == 1 &&
                EVP_DigestVerify(ctx, sig, sigSize, data, size) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return valid;
}
#else
extern int drippu_verify_catalog(const uint8_t *data, size_t size, const uint8_t *sig, size_t sigSize);
#endif

static NSTimeInterval catalogUntil, nextCatalogAttempt;
static BOOL checkCatalog(void) {
    if (catalogUntil > now())
        return YES;
    if (nextCatalogAttempt > now())
        return NO;
    nextCatalogAttempt = now() + 60;
    NSInteger status;
    NSDictionary *envelope = api(@"/service-catalog/v1/game-catalog", @"GET", nil, nil, &status);
    if (status != 200)
        return NO;
    NSData *data = [[NSData alloc] initWithBase64EncodedString:stringValue(envelope[@"payload"]) ?: @""
                                                       options:0];
    NSData *signature =
        [[NSData alloc] initWithBase64EncodedString:stringValue(envelope[@"signature"]) ?: @""
                                            options:0];
    if (![envelope[@"key_id"] isEqual:@"catalog-key-1"] || !data.length || data.length > 1048576 ||
        signature.length != 64 ||
        !drippu_verify_catalog(data.bytes, data.length, signature.bytes, signature.length)) {
        failure = @"Could not verify the Nextendo game catalog signature.";
        return NO;
    }
    NSDictionary *catalog = [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];
    if (![catalog isKindOfClass:NSDictionary.class] ||
        ![catalog[@"document_type"] isEqual:@"game_catalog"] ||
        ![catalog[@"revision"] isKindOfClass:NSNumber.class] ||
        [catalog[@"revision"] integerValue] < 1 || ![catalog[@"games"] isKindOfClass:NSArray.class]) {
        failure = @"Unexpected Nextendo game catalog.";
        return NO;
    }
    NSISO8601DateFormatter *format = [NSISO8601DateFormatter new];
    NSString *timestamp = stringValue(catalog[@"expires_at"]) ?: @"";
    format.formatOptions |= NSISO8601DateFormatWithFractionalSeconds;
    NSDate *expiry = [format dateFromString:timestamp];
    if (!expiry) {
        format.formatOptions = NSISO8601DateFormatWithInternetDateTime;
        expiry = [format dateFromString:timestamp];
    }
    if (expiry.timeIntervalSinceNow <= 0) {
        failure = @"The Nextendo game catalog expired.";
        return NO;
    }
    for (NSDictionary *game in catalog[@"games"]) {
        if (![game isKindOfClass:NSDictionary.class] ||
            ![game[@"title_ids"] isKindOfClass:NSArray.class] ||
            ![game[@"title_ids"] containsObject:@"0100152000022000"])
            continue;
        NSDictionary *versions = game[@"versions"];
        NSArray *capabilities = game[@"required_capabilities"];
        if ([game[@"enabled"] isEqual:@YES] && [versions isKindOfClass:NSDictionary.class] &&
            [versions[@"policy"] isEqual:@"exact"] &&
            [versions[@"accepted"] isKindOfClass:NSArray.class] &&
            [versions[@"accepted"] containsObject:@"4.0.1"] &&
            [capabilities isKindOfClass:NSArray.class] &&
            [[NSSet setWithArray:capabilities]
                isSubsetOfSet:[NSSet setWithObject:@"versions.exact.v1"]]) {
            catalogUntil = MIN(expiry.timeIntervalSince1970, now() + 86400);
            return YES;
        }
    }
    failure = @"Nextendo does not currently accept this version of Mario Kart 8 Deluxe.";
    return NO;
}

static NSDictionary *authorize(void) {
    NSString *verifier = base64url(randomBytes(32)), *state = base64url(randomBytes(24));
    if (!verifier || !state)
        return nil;
    NSString *challenge = base64url(sha256([verifier dataUsingEncoding:NSASCIIStringEncoding]));
    NSString *query = form(@{
        @"client_id" : client(),
        @"redirect_uri" : callback,
        @"response_type" : @"code",
        @"scope" : @"game.matchmaking",
        @"state" : state,
        @"code_challenge" : challenge,
        @"code_challenge_method" : @"S256"
    });
    NSURL *url =
        [NSURL URLWithString:[NSString stringWithFormat:@"%@/api/oauth/authorize?%@", origin, query]];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block ASWebAuthenticationSession *session;
    __block NSString *code;
    dispatch_async(dispatch_get_main_queue(), ^{
      session = [[ASWebAuthenticationSession alloc]
                initWithURL:url
          callbackURLScheme:@"drippu-nextendo"
          completionHandler:^(NSURL *reply, NSError *error) {
            NSURLComponents *c = [NSURLComponents componentsWithURL:reply ?: url
                                            resolvingAgainstBaseURL:NO];
            NSMutableDictionary *values = [NSMutableDictionary dictionary];
            BOOL unique = YES;
            for (NSURLQueryItem *item in c.queryItems) {
                if (values[item.name])
                    unique = NO;
                values[item.name] = item.value ?: @"";
            }
            if (!error && unique && [c.scheme isEqual:@"drippu-nextendo"] && [c.host isEqual:@"oauth"] &&
                [c.path isEqual:@"/callback"] && [values[@"state"] isEqual:state] && !values[@"error"])
                code = stringValue(values[@"code"]);
            session = nil;
            dispatch_semaphore_signal(done);
          }];
      session.presentationContextProvider = browserDelegate;
      if (![session start]) {
          session = nil;
          dispatch_semaphore_signal(done);
      }
    });
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 300LL * NSEC_PER_SEC))) {
        dispatch_async(dispatch_get_main_queue(), ^{
          [session cancel];
        });
        return nil;
    }
    return code ? @{
        @"grant_type" : @"authorization_code",
        @"client_id" : client(),
        @"redirect_uri" : callback,
        @"code_verifier" : verifier,
        @"code" : code
    }
                : nil;
}
static BOOL obtainAccess(BOOL interactive) {
    if (accessToken && now() + 60 < accessUntil)
        return YES;
    NSString *refresh = refreshToken();
    NSDictionary *grant =
        refresh
            ? @{@"grant_type" : @"refresh_token", @"refresh_token" : refresh, @"client_id" : client()}
            : nil;
    for (int pass = 0; pass < 2; pass++) {
        if (!grant && interactive)
            grant = authorize();
        if (!grant) {
            failure = @"Sign in to Nextendo to play online.";
            return NO;
        }
        NSInteger status;
        NSDictionary *res = api(@"/api/oauth/token", @"POST", grant, nil, &status);
        if (refresh && [res[@"error"] isEqual:@"invalid_grant"] && status == 400) {
            NSString *latest = refreshToken();
            refresh = [latest isEqual:refresh] ? nil : latest;
            grant = refresh ? @{
                @"grant_type" : @"refresh_token",
                @"refresh_token" : refresh,
                @"client_id" : client()
            }
                            : nil;
            continue;
        }
        if (status != 200)
            return NO;
        NSString *rotated = stringValue(res[@"refresh_token"]),
                 *token = stringValue(res[@"access_token"]);
        if (!token || !rotated || ![res[@"expires_in"] isKindOfClass:NSNumber.class] ||
            [res[@"expires_in"] integerValue] <= 0) {
            failure = @"Incomplete OAuth session from Nextendo.";
            return NO;
        }
        if (!storeRefresh(rotated)) {
            failure = @"Could not save the Nextendo session in Keychain.";
            return NO;
        }
        accessToken = token;
        accessUntil = now() + [res[@"expires_in"] doubleValue];
        return YES;
    }
    return NO;
}
static BOOL ensure(BOOL interactive) {
    if (!client().length) {
        failure = @"Configure the registered Drippu public client ID.";
        return NO;
    }
    if (now() < retryAfter) {
        failure = @"Nextendo requested a retry delay.";
        return NO;
    }
    if (gameToken && now() + 120 < gameUntil)
        return checkCatalog();
    if (!obtainAccess(interactive) || !checkCatalog())
        return NO;
    NSInteger status;
    NSDictionary *res = api(@"/api/nex-token", @"GET", nil, accessToken, &status);
    if (status == 401) {
        accessToken = nil;
        if (!obtainAccess(interactive))
            return NO;
        res = api(@"/api/nex-token", @"GET", nil, accessToken, &status);
    }
    if (status != 200)
        return NO;
    NSString *token = stringValue(res[@"nex_token"]);
    NSNumber *identity = res[@"pid"], *ttl = res[@"expires_in"];
    if (!token || ![identity isKindOfClass:NSNumber.class] || identity.longLongValue <= 0 ||
        ![ttl isKindOfClass:NSNumber.class] || ttl.doubleValue <= 0) {
        failure = @"Incomplete Nextendo game session.";
        return NO;
    }
    pid = identity.unsignedLongLongValue;
    gameToken = token;
    gameUntil = now() + ttl.doubleValue;
    jwt = nil;
    failure = nil;
    return YES;
}
int drippu_session_ensure(int interactive) {
    if (NSThread.isMainThread)
        return 0;
    initialize();
    [sessionLock lock];
    BOOL ok = ensure(interactive);
    [sessionLock unlock];
    return ok;
}
uint64_t drippu_session_pid(void) {
    initialize();
    [sessionLock lock];
    uint64_t value = gameUntil > now() && catalogUntil > now() ? pid : 0;
    [sessionLock unlock];
    return value;
}
// sessionLock serializes key creation, gameToken access, and jwtUntil updates.
// A transient key-generation failure can be retried on the next token request.
static NSString *signedIdentityLocked(void) {
    static SecKeyRef key;
    if (!key)
        key = SecKeyCreateRandomKey((__bridge CFDictionaryRef) @{
            (__bridge id)kSecAttrKeyType : (__bridge id)kSecAttrKeyTypeRSA,
            (__bridge id)kSecAttrKeySizeInBits : @2048
        },
                                    NULL);
    if (!key) {
        failure = @"Could not create the signing key for the Nextendo game identity.";
        return nil;
    }
    NSTimeInterval issued = now();
    jwtUntil = MIN(issued + 10800, gameUntil);
    NSString *device = [NSUUID.UUID.UUIDString.lowercaseString stringByReplacingOccurrencesOfString:@"-"
                                                                                         withString:@""];
    NSDictionary *claims = @{
        @"iss" : @"https://e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com",
        @"aud" : @"ed9e2f05d286f7b8",
        @"typ" : @"id_token",
        @"sub" : device,
        @"di" : device,
        @"bs:did" : device,
        @"iat" : @((int64_t)issued),
        @"exp" : @((int64_t)jwtUntil),
        @"jti" : NSUUID.UUID.UUIDString,
        @"sn" : @"XAW10000000000",
        @"hm" : @YES,
        @"tv" : @"4.0.1",
        @"nnex" : gameToken
    };
    NSString *body =
        [NSString stringWithFormat:@"%@.%@",
                                   base64url([NSJSONSerialization
                                       dataWithJSONObject:@{@"alg" : @"RS256", @"typ" : @"id_token"}
                                                  options:0
                                                    error:NULL]),
                                   base64url([NSJSONSerialization dataWithJSONObject:claims
                                                                             options:0
                                                                               error:NULL])];
    NSData *signature = CFBridgingRelease(
        SecKeyCreateSignature(key, kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256,
                              (__bridge CFDataRef)[body dataUsingEncoding:NSASCIIStringEncoding], NULL));
    return signature ? [body stringByAppendingFormat:@".%@", base64url(signature)] : nil;
}
// Renew an active game while it exchanges packets, even when its SDK keeps
// the BAAS identity cached for hours. The packet thread never waits on HTTPS.
static BOOL heartbeat(void) {
    if (now() < MAX(heartbeatAfter, retryAfter))
        return YES;
    heartbeatAfter = now() + 60;
    NSInteger status;
    api(@"/api/nex-session", @"POST", nil, gameToken, &status);
    if (status != 401 && status != 403)
        return YES;
    gameUntil = jwtUntil = 0;
    gameToken = jwt = nil;
    return NO;
}
void drippu_session_activity(void) {
    static _Atomic int64_t nextCheck;
    int64_t current = (int64_t)time(NULL);
    int64_t next = atomic_load(&nextCheck);
    if (current < next || !atomic_compare_exchange_strong(&nextCheck, &next, current + 60))
        return;
    initialize();
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      [sessionLock lock];
      if (gameToken && ensure(NO))
          heartbeat();
      [sessionLock unlock];
    });
}
int drippu_session_id_token(char *buffer, size_t capacity, uint64_t *length) {
    if (!length || NSThread.isMainThread)
        return 0;
    *length = 0;
    initialize();
    [sessionLock lock];
    BOOL ok = ensure(NO);
    if (ok) {
        if (!jwt || jwtUntil <= now() + 60)
            jwt = signedIdentityLocked();
        size_t size = [jwt lengthOfBytesUsingEncoding:NSASCIIStringEncoding];
        *length = size;
        ok = size && (!buffer || capacity >= size);
        if (ok && buffer)
            memcpy(buffer, jwt.UTF8String, size);
        if (ok && !heartbeat()) {
            ok = NO;
            *length = 0;
        }
    }
    [sessionLock unlock];
    return ok;
}
const char *drippu_session_error(void) {
    static _Thread_local char message[256];
    initialize();
    [sessionLock lock];
    snprintf(message, sizeof message, "%s", failure.UTF8String ?: "Nextendo sign-in unavailable.");
    [sessionLock unlock];
    return message;
}
void drippu_session_logout(void) {
    initialize();
    [sessionLock lock];
    NSString *token = refreshToken();
    SecItemDelete((__bridge CFDictionaryRef)keyQuery());
    accessToken = gameToken = jwt = nil;
    accessUntil = gameUntil = jwtUntil = heartbeatAfter = 0;
    pid = 0;
    [sessionLock unlock];
    if (token)
        sendRequest(@"/api/oauth/revoke", @"POST", @{@"token" : token, @"client_id" : client()}, nil, NULL);
}
// The SDK owns the AsyncContext layout and its vtable. Track only contexts
// completed by the token service, including move assignment and destruction.
int drippu_async_mark(void *context, int mark) {
    static NSMutableSet *done;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
      done = [NSMutableSet new];
    });
    NSValue *key = [NSValue valueWithPointer:context];
    @synchronized(done) {
        BOOL had = [done containsObject:key];
        if (mark == 1)
            [done addObject:key];
        if (!mark)
            [done removeObject:key];
        return had;
    }
}
static void message(NSString *text) {
#if TARGET_OS_OSX
    NSAlert *alert = [NSAlert new];
    alert.messageText = @"Nextendo";
    alert.informativeText = text;
    [alert addButtonWithTitle:@"OK"];
    [alert runModal];
#else
    UIWindow *window = presentationWindow();
    UIViewController *view = window.rootViewController;
    while (view.presentedViewController)
        view = view.presentedViewController;
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Nextendo"
                                                                   message:text
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [view presentViewController:alert animated:YES completion:nil];
#endif
}
static void manage(BOOL logout) {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      NSString *text;
      if (logout) {
          drippu_session_logout();
          text = @"Signed out on this device.";
      } else if (drippu_session_ensure(1))
          text = @"Signed in for Mario Kart 8 Deluxe.";
      else
          text = [NSString stringWithUTF8String:drippu_session_error()];
      dispatch_async(dispatch_get_main_queue(), ^{
        message(text);
      });
    });
}
void drippu_manage_session(void) {
    initialize();
    dispatch_async(dispatch_get_main_queue(), ^{
#if TARGET_OS_OSX
      NSAlert *alert = [NSAlert new];
      alert.messageText = @"Nextendo account";
      [alert addButtonWithTitle:@"Sign in"];
      [alert addButtonWithTitle:@"Sign out"];
      [alert addButtonWithTitle:@"Cancel"];
      NSModalResponse choice = [alert runModal];
      if (choice == NSAlertFirstButtonReturn || choice == NSAlertSecondButtonReturn)
          manage(choice == NSAlertSecondButtonReturn);
#else
        UIWindow *window = presentationWindow();
        UIViewController *view = window.rootViewController;
        while (view.presentedViewController) view = view.presentedViewController;
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Nextendo account" message:nil preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"Sign in" style:UIAlertActionStyleDefault handler:^(UIAlertAction *a){ manage(NO); }]];
        [alert addAction:[UIAlertAction actionWithTitle:@"Sign out" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *a){ manage(YES); }]];
        [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
        [view presentViewController:alert animated:YES completion:nil];
#endif
    });
}
#if TARGET_OS_OSX
@implementation DrippuWebLogin (Menu)
- (void)manageAccount:(id)sender {
    drippu_manage_session();
}
@end
void drippu_install_account_menu(void *menu) {
    initialize();
    [(__bridge NSMenu *)menu addItemWithTitle:@"Nextendo Account…"
                                       action:@selector(manageAccount:)
                                keyEquivalent:@""]
        .target = browserDelegate;
}
#endif
