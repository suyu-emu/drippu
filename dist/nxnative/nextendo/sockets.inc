// SPDX-License-Identifier: GPL-3.0-or-later
// Included inside the pinned BSD service. Descriptor numbers share its table
// with EventFd. Duplicate under its lock so a concurrent close/reopen cannot
// redirect an in-flight guest operation to a different host descriptor.
int drippu_dup_socket(int fd) {
    dm_host_mutex_lock(&g_efd_m);
    int host = sk_used(fd) && g_efd[fd].host > 0 ? dup(g_efd[fd].host - 1) : -1;
    pthread_mutex_unlock(&g_efd_m);
    return host;
}
static long finish_io(int host, long result) {
    int error = errno;
    close(host);
    return result < 0 ? sk_error(error) : result;
}
int dm_sk_socket(int family, int type, int protocol) {
    if (family != AF_INET || !((type == SOCK_STREAM && (!protocol || protocol == IPPROTO_TCP)) ||
                               (type == SOCK_DGRAM && (!protocol || protocol == IPPROTO_UDP))))
        return sk_error(EPROTONOSUPPORT);
    int host = socket(AF_INET, type, protocol);
    if (host < 0)
        return sk_error(errno);
    int on = 1;
    if (setsockopt(host, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on) || fcntl(host, F_SETFD, FD_CLOEXEC))
        return (int)finish_io(host, -1);
    dm_host_mutex_lock(&g_efd_m);
    int fd = sk_new_locked(SK_UDP);
    if (fd >= 0)
        g_efd[fd].host = host + 1;
    pthread_mutex_unlock(&g_efd_m);
    if (fd < 0)
        close(host);
    return fd;
}
static int address_in(const void *data, unsigned size, struct sockaddr_in *addr) {
    if (!data || size < sizeof *addr)
        return sk_error(EINVAL);
    memcpy(addr, data, sizeof *addr);
    if (addr->sin_family != AF_INET && addr->sin_family != AF_UNSPEC)
        return sk_error(EAFNOSUPPORT);
    addr->sin_len = sizeof *addr;
    return 0;
}
int dm_sk_bind(int fd, const void *a, unsigned size) {
    struct sockaddr_in addr;
    if (address_in(a, size, &addr))
        return -1;
    int host = drippu_dup_socket(fd);
    if (host < 0)
        return sk_error(EBADF);
    return (int)finish_io(host, bind(host, (struct sockaddr *)&addr, sizeof addr));
}
int dm_sk_connect(int fd, const void *a, unsigned size) {
    struct sockaddr_in addr;
    if (address_in(a, size, &addr))
        return -1;
    int host = drippu_dup_socket(fd);
    if (host < 0)
        return sk_error(EBADF);
    return (int)finish_io(host, connect(host, (struct sockaddr *)&addr, sizeof addr));
}
int dm_sk_shutdown(int fd, int how) {
    int host = drippu_dup_socket(fd);
    if (host < 0)
        return sk_error(EBADF);
    return (int)finish_io(host, shutdown(host, how));
}
int dm_sk_name(int fd, void *addr, unsigned *size, int peer) {
    if (!addr || !size)
        return sk_error(EINVAL);
    int host = drippu_dup_socket(fd);
    if (host < 0)
        return sk_error(EBADF);
    return (int)finish_io(host, peer ? getpeername(host, addr, size) : getsockname(host, addr, size));
}
int dm_sk_fcntl(int fd, int cmd, int v) {
    int host = drippu_dup_socket(fd);
    if (host < 0 || cmd == 1 || cmd == 2) {
        if (host >= 0)
            close(host);
        return local_fcntl(fd, cmd, v);
    }
    int result;
    if (cmd == 3) {
        result = fcntl(host, F_GETFL);
        if (result >= 0)
            result = result & O_NONBLOCK ? SK_NONBLOCK : 0;
    } else if (cmd == 4)
        result = fcntl(host, F_SETFL, v & SK_NONBLOCK ? O_NONBLOCK : 0);
    else {
        errno = EINVAL;
        result = -1;
    }
    return (int)finish_io(host, result);
}
static int socket_option(int level, int option) {
    if (level == IPPROTO_TCP && option == 1)
        return TCP_NODELAY;
    if (level != SOL_SOCKET)
        return -1;
    switch (option) {
    case 4:
        return SO_REUSEADDR;
    case 8:
        return SO_KEEPALIVE;
    case 0x20:
        return SO_BROADCAST;
    case 0x200:
        return SO_REUSEPORT;
    case 0x1001:
        return SO_SNDBUF;
    case 0x1002:
        return SO_RCVBUF;
    case 0x1005:
        return SO_SNDTIMEO;
    case 0x1006:
        return SO_RCVTIMEO;
    case 0x1007:
        return SO_ERROR;
    case 0x1008:
        return SO_TYPE;
    default:
        return -1;
    }
}
int dm_sk_setopt(int fd, int level, int option, const void *value, unsigned size) {
    int opt = socket_option(level, option);
    if (opt < 0 || !value)
        return sk_error(ENOPROTOOPT);
    int host = drippu_dup_socket(fd);
    if (host < 0)
        return sk_error(EBADF);
    if (opt == SO_RCVTIMEO || opt == SO_SNDTIMEO) {
        int64_t parts[2];
        if (size != sizeof parts) {
            close(host);
            return sk_error(EINVAL);
        }
        memcpy(parts, value, sizeof parts);
        if (parts[0] < 0 || parts[1] < 0 || parts[1] >= 1000000) {
            close(host);
            return sk_error(EINVAL);
        }
        struct timeval tv = {parts[0], (int)parts[1]};
        return (int)finish_io(host, setsockopt(host, level, opt, &tv, sizeof tv));
    }
    return (int)finish_io(host, setsockopt(host, level, opt, value, size));
}
int dm_sk_getopt(int fd, int level, int option, void *value, unsigned *size) {
    int opt = socket_option(level, option);
    if (opt < 0 || !value || !size)
        return sk_error(ENOPROTOOPT);
    int host = drippu_dup_socket(fd);
    if (host < 0)
        return sk_error(EBADF);
    if (opt == SO_RCVTIMEO || opt == SO_SNDTIMEO) {
        struct timeval tv = {0};
        socklen_t n = sizeof tv;
        if (*size < 16) {
            close(host);
            return sk_error(EINVAL);
        }
        int r = getsockopt(host, level, opt, &tv, &n);
        if (!r) {
            int64_t parts[] = {tv.tv_sec, tv.tv_usec};
            memcpy(value, parts, sizeof parts);
            *size = sizeof parts;
        }
        return (int)finish_io(host, r);
    }
    return (int)finish_io(host, getsockopt(host, level, opt, value, size));
}
static int message_flags(int f) {
    if (f & ~(2 | 0x80))
        return -1;
    return (f & 2 ? MSG_PEEK : 0) | (f & 0x80 ? MSG_DONTWAIT : 0);
}
long dm_sk_sendto(int fd, const void *data, unsigned long n, int flags, const void *a, unsigned size) {
    struct sockaddr_in addr;
    int f = message_flags(flags);
    if (f < 0 || (a && address_in(a, size, &addr)))
        return sk_error(EINVAL);
    int host = drippu_dup_socket(fd);
    if (host < 0)
        return sk_error(EBADF);
    drippu_session_activity();
    return finish_io(host,
                     sendto(host, data, n, f, a ? (struct sockaddr *)&addr : NULL, a ? sizeof addr : 0));
}
long dm_sk_recvfrom(int fd, void *data, unsigned long n, int flags, void *a, unsigned *size) {
    int f = message_flags(flags);
    if (f < 0 || (a && !size))
        return sk_error(EINVAL);
    int host = drippu_dup_socket(fd);
    if (host < 0)
        return sk_error(EBADF);
    return finish_io(host, recvfrom(host, data, n, f, a, size));
}
