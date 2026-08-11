/*
 * Max Pool meta (master) server.
 *
 * Based on the Max Pool meta server by Shuouma <dreamcast-talk.com>,
 * Copyright 2017. He wrote the original and reverse-engineered the protocol;
 * this version reads the server list from a file at runtime instead of using
 * a hardcoded hostname, and adds -p / -f / -v flags and logging. Wire format
 * is unchanged.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define BUFSIZE      1024
#define HDRSIZE      15
#define ENTRYSIZE    6
#define MAX_SERVERS  ((BUFSIZE - HDRSIZE) / ENTRYSIZE)

static const char *cfg_path = "servers.conf";
static int verbose = 0;

struct entry {
    char host[256];
    uint16_t port;
};

static void logts(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#include <stdarg.h>
static void logts(const char *fmt, ...)
{
    char ts[32];
    time_t t = time(NULL);
    struct tm tm;
    va_list ap;

    localtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(stderr, "[%s] ", ts);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/*
 * Config: one server per line, "<host-or-ip> <port>".
 * Blank lines and lines starting with # are ignored.
 */
static int load_config(struct entry *out, int max)
{
    FILE *f;
    char line[512];
    int n = 0;

    f = fopen(cfg_path, "r");
    if (!f) {
        logts("cannot open config \"%s\" - no servers will be advertised", cfg_path);
        return 0;
    }

    while (n < max && fgets(line, sizeof line, f)) {
        char host[256];
        unsigned int port;
        char *p = line;

        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#' || *p == '\n' || *p == '\r')
            continue;

        if (sscanf(p, "%255s %u", host, &port) != 2 || port == 0 || port > 65535) {
            logts("skipping malformed config line: %s", p);
            continue;
        }

        strncpy(out[n].host, host, sizeof out[n].host - 1);
        out[n].host[sizeof out[n].host - 1] = '\0';
        out[n].port = (uint16_t)port;
        n++;
    }

    fclose(f);
    return n;
}

static int resolve_ipv4(const char *host, uint32_t *net_ip)
{
    struct addrinfo hints, *res;
    struct sockaddr_in *sin;
    int rc;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0) {
        logts("cannot resolve \"%s\": %s", host, gai_strerror(rc));
        return -1;
    }

    sin = (struct sockaddr_in *)res->ai_addr;
    *net_ip = sin->sin_addr.s_addr;   /* already network byte order */
    freeaddrinfo(res);
    return 0;
}

static void write_header(char *msg, uint16_t nr_of_servers)
{
    memset(msg, 0, HDRSIZE);
    msg[0]  = 0x05;
    msg[1]  = 0x02;
    msg[8]  = 0x0A;
    msg[9]  = 0x00;
    msg[10] = 0x00;
    msg[11] = 0x04;
    msg[12] = (char)(nr_of_servers & 0xFF);         /* LE, matches original */
    msg[13] = (char)((nr_of_servers >> 8) & 0xFF);
    msg[14] = 0x00;
}

static void write_entry(char *msg, uint16_t port, uint32_t net_ip)
{
    msg[0] = (char)(port >> 8);                     /* BE16 port */
    msg[1] = (char)(port & 0xFF);
    memcpy(&msg[2], &net_ip, 4);                    /* BE32 ipv4 */
}

static int verify_request(const char *msg, ssize_t len)
{
    static const unsigned char magic[9] =
        { 0x05, 0x02, 0x00, 0x66, 0x00, 0x0A, 0x00, 0x00, 0x04 };

    if (len < 9)
        return -1;
    if (memcmp(msg, magic, 9) != 0)
        return -1;
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [-p listen_port] [-f servers.conf] [-v]\n"
        "  -p  UDP port to bind (default 6003)\n"
        "  -f  server list file (default ./servers.conf)\n"
        "  -v  log every request, including ones that fail the magic check\n",
        argv0);
}

int main(int argc, char **argv)
{
    int sockfd, opt, optval = 1;
    int portno = 6003;
    struct sockaddr_in serveraddr, clientaddr;
    socklen_t clientlen;
    char buf[BUFSIZE];
    struct entry servers[MAX_SERVERS];

    while ((opt = getopt(argc, argv, "p:f:vh")) != -1) {
        switch (opt) {
        case 'p': portno = atoi(optarg); break;
        case 'f': cfg_path = optarg; break;
        case 'v': verbose = 1; break;
        default:  usage(argv[0]); return opt == 'h' ? 0 : 1;
        }
    }

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }

    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof optval);

    memset(&serveraddr, 0, sizeof serveraddr);
    serveraddr.sin_family      = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port        = htons((unsigned short)portno);

    if (bind(sockfd, (struct sockaddr *)&serveraddr, sizeof serveraddr) < 0) {
        perror("bind");
        return 1;
    }

    logts("listening on UDP %d, server list \"%s\"", portno, cfg_path);

    for (;;) {
        ssize_t n;
        int i, count, written = 0;
        uint16_t advertised = 0;
        int pkt_size;

        clientlen = sizeof clientaddr;
        memset(buf, 0, sizeof buf);

        n = recvfrom(sockfd, buf, sizeof buf, 0,
                     (struct sockaddr *)&clientaddr, &clientlen);
        if (n < 0) { perror("recvfrom"); continue; }

        if (verify_request(buf, n) != 0) {
            if (verbose)
                logts("ignored %zd bytes from %s (bad magic)",
                      n, inet_ntoa(clientaddr.sin_addr));
            continue;
        }

        count = load_config(servers, MAX_SERVERS);
        memset(buf, 0, sizeof buf);

        for (i = 0; i < count; i++) {
            uint32_t ip;
            if (resolve_ipv4(servers[i].host, &ip) != 0)
                continue;
            write_entry(&buf[HDRSIZE + written * ENTRYSIZE], servers[i].port, ip);
            written++;
        }

        advertised = (uint16_t)written;
        write_header(buf, advertised);
        pkt_size = HDRSIZE + written * ENTRYSIZE;

        if (sendto(sockfd, buf, (size_t)pkt_size, 0,
                   (struct sockaddr *)&clientaddr, clientlen) < 0) {
            perror("sendto");
            continue;
        }

        logts("query from %s -> replied with %d server(s), %d bytes",
              inet_ntoa(clientaddr.sin_addr), written, pkt_size);
    }
}
