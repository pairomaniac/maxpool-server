/*
 * Max Pool meta (master) server.
 *
 * UDP 6003  - answers the Dreamcast's DirG2GetDirectory query with the server
 *             list, built from self-registrations plus a config file.
 * TCP 15101 - accepts DirG2AddService registrations from game servers whose
 *             .scs points meta_server1 here.
 *
 * Wire formats are in docs/protocol.md. The reply header is 14 bytes and each
 * entity is 7 (length byte, big-endian port, IPv4). Treating the header as 15
 * and the entities as 6 works for one server and truncates the list for more.
 *
 * Derived from the Max Pool meta server by Shuouma <dreamcast-talk.com>,
 * copyright 2017, https://dreamcastlive.net/ (Server Software page).
 * License in LICENSE.upstream.
 *
 *   gcc -Wall -O2 maxpool_meta_server.c -o maxpool_meta_server
 *   ./maxpool_meta_server -f servers.conf -v
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT  6003
#define REG_PORT      15101
#define DEFAULT_CONF  "servers.conf"
#define MAX_SERVERS   64
#define HDR_LEN       14
#define ENTITY_LEN    7
#define REPLY_MAX     (HDR_LEN + MAX_SERVERS * ENTITY_LEN)
#define REG_TIMEOUT   5          /* seconds to wait on a registering client */

/* GF_DECOMPSERVICES | GF_DECOMPRECURSIVE | GF_SERVADDNETADDR */
#define REPLY_FLAGS   0x0400000AUL

/* The SDK value for MultiEntityReply is 3, but every deployed server sends 0
 * and clients accept it. Build with -DMSG_TYPE=3 to send the correct one. */
#ifndef MSG_TYPE
#define MSG_TYPE 0
#endif

struct server {
    struct in_addr ip;
    unsigned short port;
    time_t expires;              /* 0 for config entries, which never expire */
};

static const char *cfg_path = DEFAULT_CONF;
static int verbose = 0;
static volatile sig_atomic_t running = 1;

static struct server regs[MAX_SERVERS];
static int reg_count = 0;

static void on_signal(int sig) { (void)sig; running = 0; }

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    char ts[32];
    time_t now = time(NULL);
    struct tm tm;

    localtime_r(&now, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    printf("[%s] ", ts);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

/* ---- server list ---------------------------------------------------------
 * Registrations expire; config entries don't. Both go in the same reply, with
 * registrations first and duplicate addresses skipped.
 */

static void reg_add(struct in_addr ip, unsigned short port, unsigned long lifespan)
{
    time_t expires;
    int i;

    if (lifespan == 0 || lifespan > 86400)
        lifespan = 3600;
    expires = time(NULL) + lifespan;

    for (i = 0; i < reg_count; i++)
        if (regs[i].ip.s_addr == ip.s_addr && regs[i].port == port) {
            regs[i].expires = expires;
            logmsg("refreshed %s:%u for %lus", inet_ntoa(ip), port, lifespan);
            return;
        }

    if (reg_count == MAX_SERVERS) {
        logmsg("registration table full, dropped %s:%u", inet_ntoa(ip), port);
        return;
    }

    regs[reg_count].ip = ip;
    regs[reg_count].port = port;
    regs[reg_count].expires = expires;
    reg_count++;
    logmsg("registered %s:%u for %lus", inet_ntoa(ip), port, lifespan);
}

static void reg_expire(void)
{
    time_t now = time(NULL);
    int i = 0;

    while (i < reg_count) {
        if (regs[i].expires <= now) {
            logmsg("expired %s:%u", inet_ntoa(regs[i].ip), regs[i].port);
            regs[i] = regs[--reg_count];
        } else {
            i++;
        }
    }
}

/* Append config entries to out[], skipping addresses already present. */
static int add_config_servers(struct server *out, int n, int max)
{
    FILE *f = fopen(cfg_path, "r");
    char line[512];

    if (!f) {
        logmsg("cannot open \"%s\": %s", cfg_path, strerror(errno));
        return n;
    }

    while (n < max && fgets(line, sizeof line, f)) {
        char host[256];
        int port, i, dup = 0;
        struct in_addr ip;
        char *hash = strchr(line, '#');

        if (hash)
            *hash = '\0';
        if (sscanf(line, "%255s %d", host, &port) != 2)
            continue;
        if (port <= 0 || port > 65535) {
            logmsg("bad port %d for \"%s\", skipped", port, host);
            continue;
        }

        if (inet_pton(AF_INET, host, &ip) != 1) {
            struct addrinfo hints, *res;
            memset(&hints, 0, sizeof hints);
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            if (getaddrinfo(host, NULL, &hints, &res) != 0) {
                logmsg("cannot resolve \"%s\", skipped", host);
                continue;
            }
            ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
            freeaddrinfo(res);
        }

        for (i = 0; i < n; i++)
            if (out[i].ip.s_addr == ip.s_addr && out[i].port == port)
                dup = 1;
        if (dup)
            continue;

        out[n].ip = ip;
        out[n].port = (unsigned short)port;
        out[n].expires = 0;
        n++;
    }

    fclose(f);
    return n;
}

/* ---- UDP 6003 ------------------------------------------------------------ */

static size_t build_reply(unsigned char *buf, const struct server *s, int n)
{
    size_t o = 0;
    int i;

    buf[o++] = 0x05;                      /* HeaderService2Message2      */
    buf[o++] = 0x02; buf[o++] = 0x00;     /* service SmallDirServerG2    */
    buf[o++] = MSG_TYPE & 0xFF;
    buf[o++] = (MSG_TYPE >> 8) & 0xFF;
    buf[o++] = 0x00; buf[o++] = 0x00;     /* status = success            */
    buf[o++] = 0x00;                      /* sequence                    */
    buf[o++] = REPLY_FLAGS & 0xFF;        /* flags, little-endian        */
    buf[o++] = (REPLY_FLAGS >> 8) & 0xFF;
    buf[o++] = (REPLY_FLAGS >> 16) & 0xFF;
    buf[o++] = (REPLY_FLAGS >> 24) & 0xFF;
    buf[o++] = n & 0xFF;                  /* entity count                */
    buf[o++] = (n >> 8) & 0xFF;

    for (i = 0; i < n; i++) {
        unsigned short be_port = htons(s[i].port);
        buf[o++] = 0x06;                  /* netaddress length           */
        memcpy(buf + o, &be_port, 2); o += 2;
        memcpy(buf + o, &s[i].ip.s_addr, 4); o += 4;
    }

    return o;
}

static void handle_query(int sock)
{
    struct server list[MAX_SERVERS];
    unsigned char req[1024], reply[REPLY_MAX];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof from;
    ssize_t got;
    size_t len;
    int n;

    got = recvfrom(sock, req, sizeof req, 0, (struct sockaddr *)&from, &fromlen);
    if (got < 0)
        return;

    if (got < 5 || req[0] != 0x05 || req[1] != 0x02 || req[2] != 0x00) {
        if (verbose)
            logmsg("ignored %zd bytes from %s (not a SmallMessage dir query)",
                   got, inet_ntoa(from.sin_addr));
        return;
    }

    reg_expire();
    memcpy(list, regs, sizeof(struct server) * reg_count);
    n = add_config_servers(list, reg_count, MAX_SERVERS);
    len = build_reply(reply, list, n);

    if (sendto(sock, reply, len, 0, (struct sockaddr *)&from, fromlen) < 0)
        logmsg("sendto %s failed: %s", inet_ntoa(from.sin_addr), strerror(errno));
    else
        logmsg("query from %s -> replied with %d server(s), %zu bytes",
               inet_ntoa(from.sin_addr), n, len);
}

/* ---- TCP 15101 -----------------------------------------------------------
 * The game server sends DirG2AddDirectory, waits for a StatusReply, then sends
 * DirG2AddService with its address, each on its own connection. Frames carry a
 * 4-byte little-endian length that counts itself. Strings are PW_STRING: a
 * 16-bit character count then UTF-16LE, so twice as many bytes.
 *
 * Connections are handled inline rather than in threads: the exchange is one
 * short frame and the socket carries a receive timeout, so a stalled client
 * costs at most REG_TIMEOUT seconds of query latency.
 */

static ssize_t skip_pw_string(const unsigned char *b, size_t len, size_t off)
{
    unsigned short chars;

    if (off + 2 > len)
        return -1;
    chars = b[off] | (b[off + 1] << 8);
    off += 2 + (size_t)chars * 2;
    return off > len ? -1 : (ssize_t)off;
}

/* AddService body: entity flags, path, name, netaddress, display name,
 * lifespan. The netaddress is part of the entity key, hence its position. */
static int parse_add_service(const unsigned char *b, size_t len,
                             struct in_addr *ip, unsigned short *port,
                             unsigned long *lifespan)
{
    ssize_t off = 6;                     /* header + entity flags */
    unsigned char alen;

    if (len < 8)
        return -1;
    if ((off = skip_pw_string(b, len, off)) < 0)   /* path */
        return -1;
    if ((off = skip_pw_string(b, len, off)) < 0)   /* name */
        return -1;

    if ((size_t)off >= len)
        return -1;
    alen = b[off++];
    if (alen < 6 || (size_t)off + alen > len)
        return -1;
    *port = (unsigned short)((b[off] << 8) | b[off + 1]);   /* big-endian */
    memcpy(&ip->s_addr, b + off + 2, 4);
    off += alen;

    if ((off = skip_pw_string(b, len, off)) < 0)   /* display name */
        return -1;
    if ((size_t)off + 4 > len)
        return -1;
    *lifespan = (unsigned long)b[off] | ((unsigned long)b[off + 1] << 8) |
                ((unsigned long)b[off + 2] << 16) | ((unsigned long)b[off + 3] << 24);
    return 0;
}

static void handle_registration(int listen_fd)
{
    unsigned char buf[4096];
    struct sockaddr_in peer;
    socklen_t peerlen = sizeof peer;
    struct timeval tv = { REG_TIMEOUT, 0 };
    size_t have = 0;
    int fd;

    fd = accept(listen_fd, (struct sockaddr *)&peer, &peerlen);
    if (fd < 0)
        return;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    for (;;) {
        unsigned long total;
        ssize_t got;

        if (have >= sizeof buf)
            break;                        /* frame bigger than the buffer */
        got = read(fd, buf + have, sizeof buf - have);
        if (got <= 0)
            break;
        have += (size_t)got;

        if (have < 4)
            continue;
        total = (unsigned long)buf[0] | ((unsigned long)buf[1] << 8) |
                ((unsigned long)buf[2] << 16) | ((unsigned long)buf[3] << 24);
        if (total < 9 || total > sizeof buf)
            break;                        /* not a frame we understand */
        if (have < total)
            continue;

        /* plain SmallMessage for SmallDirServerG2 */
        if (buf[4] == 0x05 && buf[5] == 0x02 && buf[6] == 0x00) {
            unsigned short msgtype = (unsigned short)(buf[7] | (buf[8] << 8));

            if (msgtype == 202) {         /* DirG2AddService */
                struct in_addr ip;
                unsigned short port;
                unsigned long lifespan;

                if (parse_add_service(buf + 4, total - 4, &ip, &port,
                                      &lifespan) == 0) {
                    /* a host behind NAT announces an address nobody can reach;
                     * the one it connected from works */
                    if (ip.s_addr != peer.sin_addr.s_addr) {
                        logmsg("announced %s, using peer address instead",
                               inet_ntoa(ip));
                        ip = peer.sin_addr;
                    }
                    reg_add(ip, port, lifespan);
                } else {
                    logmsg("malformed AddService from %s, ignored",
                           inet_ntoa(peer.sin_addr));
                }
            } else if (verbose) {
                logmsg("registration msg %u from %s (no action)",
                       msgtype, inet_ntoa(peer.sin_addr));
            }
        }

        /* DirG2StatusReply, success. The game server waits for this before
         * sending the next message. */
        {
            static const unsigned char ok[] = {
                0x0b, 0x00, 0x00, 0x00,   /* frame length, counts itself */
                0x05, 0x02, 0x00,         /* SmallMessage, service 2     */
                0x01, 0x00,               /* DirG2StatusReply            */
                0x00, 0x00                /* StatusCommon_Success        */
            };
            if (write(fd, ok, sizeof ok) != (ssize_t)sizeof ok)
                break;
        }

        memmove(buf, buf + total, have - total);
        have -= total;
    }

    close(fd);
}

static int listen_tcp(int port)
{
    struct sockaddr_in addr;
    int fd, one = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 ||
        listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv)
{
    int port = DEFAULT_PORT, reg_port = REG_PORT, opt;
    int udp = -1, tcp = -1;
    struct sockaddr_in addr;
    struct sigaction sa;
    struct pollfd fds[2];

    while ((opt = getopt(argc, argv, "p:f:r:vh")) != -1) {
        switch (opt) {
        case 'p': port = atoi(optarg); break;
        case 'f': cfg_path = optarg; break;
        case 'r': reg_port = atoi(optarg); break;
        case 'v': verbose = 1; break;
        default:
            fprintf(stderr,
                    "usage: %s [-p port] [-f servers.conf] [-r regport] [-v]\n"
                    "  -r 0 turns off self-registration on TCP %d\n",
                    argv[0], REG_PORT);
            return opt == 'h' ? 0 : 1;
        }
    }

    /* no SA_RESTART, so a signal breaks poll() and systemd gets a clean stop */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(udp, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        close(udp);
        return 1;
    }
    logmsg("listening on UDP %d, config \"%s\"", port, cfg_path);

    if (reg_port > 0) {
        tcp = listen_tcp(reg_port);
        if (tcp < 0)
            logmsg("registration listener on %d: %s", reg_port, strerror(errno));
        else
            logmsg("registration listener on TCP %d", reg_port);
    }

    fds[0].fd = udp;
    fds[0].events = POLLIN;
    fds[1].fd = tcp;
    fds[1].events = POLLIN;

    while (running) {
        if (poll(fds, tcp < 0 ? 1 : 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }
        if (fds[0].revents & POLLIN)
            handle_query(udp);
        if (tcp >= 0 && (fds[1].revents & POLLIN))
            handle_registration(tcp);
    }

    logmsg("shutting down");
    close(udp);
    if (tcp >= 0)
        close(tcp);
    return 0;
}
