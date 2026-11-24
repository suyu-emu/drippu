// SPDX-License-Identifier: GPL-3.0-or-later
#include "network.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
static int ends(const char *name, const char *suffix) {
    size_t n = strlen(name), s = strlen(suffix);
    return n > s && !strcasecmp(name + n - s, suffix);
}
int drippu_game_host(const char *name) {
    return name && (ends(name, ".nintendo.net") || ends(name, ".nintendo.com") ||
                    ends(name, ".nintendowifi.net") || ends(name, ".nintendo.co.jp"));
}
static const char *destination(const char *name) {
    if (!name)
        return NULL;
    struct in_addr literal;
    if (inet_pton(AF_INET, name, &literal) == 1)
        return name;
    if (!drippu_game_host(name))
        return NULL;
    if ((!strncasecmp(name, "nncs2-", 6) && ends(name, ".n.n.srv.nintendo.net")) ||
        !strcasecmp(name, "g2b309e01-lp1.s.n.srv.nintendo.net"))
        return "nat.nextendo.network";
    return "nex.nextendo.network";
}
// Horizon/FreeBSD's addrinfo has canonname before addr. Build the guest
// layout explicitly so no operating-system-owned address list escapes.
struct guest_ai {
    int flags, family, type, protocol;
    unsigned length;
    char *name;
    struct sockaddr *address;
    struct guest_ai *next;
};
void drippu_freeaddrinfo(void *result) {
    struct guest_ai *p = result;
    while (p) {
        struct guest_ai *next = p->next;
        free(p->name);
        free(p);
        p = next;
    }
}
int drippu_addrinfo(const char *name, const char *service, const void *hint, void **result) {
    if (!result)
        return EAI_FAIL;
    *result = NULL;
    const char *target = destination(name);
    if (!target && name)
        return EAI_NONAME;
    const struct guest_ai *g = hint;
    if (g && g->family && g->family != AF_INET)
        return EAI_FAMILY;
    struct addrinfo hints = {.ai_family = AF_INET,
                             .ai_socktype = g ? g->type : 0,
                             .ai_protocol = g ? g->protocol : 0,
                             .ai_flags =
                                 g ? g->flags & (AI_PASSIVE | AI_NUMERICHOST | AI_CANONNAME) : 0};
    struct addrinfo *list = NULL;
    int error = getaddrinfo(target, service, &hints, &list);
    if (error)
        return error;
    struct guest_ai *head = NULL, **tail = &head;
    for (struct addrinfo *ai = list; ai; ai = ai->ai_next) {
        struct guest_ai *p = calloc(1, sizeof(*p) + ai->ai_addrlen);
        if (!p) {
            error = EAI_MEMORY;
            break;
        }
        *tail = p;
        tail = &p->next;
        p->family = ai->ai_family;
        p->type = ai->ai_socktype;
        p->protocol = ai->ai_protocol;
        p->length = ai->ai_addrlen;
        p->address = (struct sockaddr *)(p + 1);
        memcpy(p->address, ai->ai_addr, p->length);
        if (name && (hints.ai_flags & AI_CANONNAME))
            p->name = strdup(name);
    }
    freeaddrinfo(list);
    if (error)
        drippu_freeaddrinfo(head);
    else
        *result = head;
    return error;
}
struct hostent *drippu_host(const char *name) {
    static _Thread_local struct {
        struct hostent entry;
        struct in_addr ip;
        char name[256];
        char *addresses[2], *aliases[1];
    } h;
    struct guest_ai *addresses = NULL;
    if (!name || strlen(name) >= sizeof h.name ||
        drippu_addrinfo(name, NULL, NULL, (void **)&addresses) || !addresses)
        return NULL;
    h.ip = ((struct sockaddr_in *)addresses->address)->sin_addr;
    drippu_freeaddrinfo(addresses);
    strcpy(h.name, name);
    h.addresses[0] = (char *)&h.ip;
    h.entry = (struct hostent){h.name, h.aliases, AF_INET, sizeof h.ip, h.addresses};
    return &h.entry;
}
int drippu_ip(uint32_t *address, uint32_t *mask, uint32_t *broadcast) {
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list))
        return 0;
    int found = 0;
    for (struct ifaddrs *i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) ||
            (i->ifa_flags & IFF_LOOPBACK))
            continue;
        uint32_t a = ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr;
        uint32_t m = i->ifa_netmask ? ((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr : 0;
        if (address)
            *address = a;
        if (mask)
            *mask = m;
        if (broadcast)
            *broadcast = a | ~m;
        found = 1;
        break;
    }
    freeifaddrs(list);
    return found;
}
