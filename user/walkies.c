/*
 * walkies.c — Network configuration daemon for BlueyOS
 *
 * Reads /etc/interfaces and configures network interfaces, IP addresses,
 * and routes using the AF_BLUEY_NETCTL kernel control plane.
 *
 * Supports a daemon mode (-m) for ongoing event monitoring.
 *
 * "Let's go for a walkies!" — Bluey
 *
 * Usage:
 *   walkies [-c <config>] [-m]
 *
 *   -c <config>   Path to interfaces file (default: /etc/interfaces)
 *   -m            Monitor mode: subscribe to all netctl events and log them
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <netinet/in.h>

#include "walkies.h"

/* =========================================================================
 * /etc/interfaces configuration parser
 * ========================================================================= */

#define MAX_IFACES  16
#define IFNAME_LEN  16

typedef enum {
    METHOD_LOOPBACK,
    METHOD_STATIC,
    METHOD_DHCP,    /* Interface is UP; actual DHCP client handled separately */
    METHOD_MANUAL,
} iface_method_t;

typedef struct {
    char           name[IFNAME_LEN];
    iface_method_t method;
    uint32_t       address;     /* Host byte order, 0 = not set */
    uint32_t       netmask;     /* Host byte order, 0 = not set */
    uint32_t       gateway;     /* Host byte order, 0 = no gateway */
    uint32_t       mtu;         /* 0 = use kernel default */
    int            configured;  /* Set to 1 after successful apply */
} iface_config_t;

static iface_config_t g_ifaces[MAX_IFACES];
static int            g_iface_count = 0;

/* Convert dotted-decimal string to host byte order uint32_t. */
static int parse_ip(const char *str, uint32_t *out)
{
    struct in_addr a;
    if (inet_aton(str, &a) == 0)
        return -1;
    *out = ntohl(a.s_addr);
    return 0;
}

/* Convert a host-byte-order dotted-decimal netmask to a prefix length.
 * E.g. 0xFFFFFF00 → 24. */
static int netmask_to_prefix(uint32_t mask)
{
    int prefix = 0;
    uint32_t m = mask;
    while (m & 0x80000000U) {
        prefix++;
        m <<= 1;
    }
    return prefix;
}

/* Advance past leading whitespace. */
static char *ltrim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

/* Strip trailing whitespace and newlines in-place. */
static void rtrim(char *s)
{
    char *p = s + strlen(s);
    while (p > s && (*(p - 1) == '\n' || *(p - 1) == '\r' ||
                     *(p - 1) == ' '  || *(p - 1) == '\t'))
        *(--p) = '\0';
}

/*
 * Parse a Debian-style /etc/interfaces file.
 *
 * Supported stanza:
 *   iface <name> inet loopback
 *   iface <name> inet static
 *     address <dotted-decimal>
 *     netmask <dotted-decimal>
 *     gateway <dotted-decimal>
 *     mtu     <integer>
 *   iface <name> inet dhcp
 *
 * Lines beginning with '#' or blank lines are ignored.
 * 'auto' and 'allow-hotplug' lines are parsed but informational only.
 */
static int parse_interfaces(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[walkies] Cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    char line[256];
    iface_config_t *cur = NULL;

    while (fgets(line, sizeof(line), f)) {
        char *p = ltrim(line);
        rtrim(p);

        /* Skip blank lines and comments */
        if (*p == '\0' || *p == '#')
            continue;

        /* iface <name> inet <method> — starts a new stanza */
        if (strncmp(p, "iface ", 6) == 0) {
            if (g_iface_count >= MAX_IFACES) {
                fprintf(stderr, "[walkies] Too many interfaces in %s (max %d)\n",
                        path, MAX_IFACES);
                break;
            }
            char name[IFNAME_LEN];
            char family[16];
            char method[16];

            if (sscanf(p + 6, "%15s %15s %15s", name, family, method) != 3) {
                fprintf(stderr, "[walkies] Malformed iface line: %s\n", p);
                continue;
            }

            cur = &g_ifaces[g_iface_count++];
            memset(cur, 0, sizeof(*cur));
            strncpy(cur->name, name, IFNAME_LEN - 1);

            if (strcmp(method, "loopback") == 0)
                cur->method = METHOD_LOOPBACK;
            else if (strcmp(method, "static") == 0)
                cur->method = METHOD_STATIC;
            else if (strcmp(method, "dhcp") == 0)
                cur->method = METHOD_DHCP;
            else
                cur->method = METHOD_MANUAL;

            continue;
        }

        /* 'auto' and 'allow-hotplug' are informational */
        if (strncmp(p, "auto ", 5) == 0 ||
            strncmp(p, "allow-hotplug ", 14) == 0)
            continue;

        /* Options within the current stanza */
        if (!cur)
            continue;

        char key[32], value[128];
        if (sscanf(p, "%31s %127s", key, value) != 2)
            continue;

        if (strcmp(key, "address") == 0) {
            if (parse_ip(value, &cur->address) < 0)
                fprintf(stderr, "[walkies] Invalid address: %s\n", value);
        } else if (strcmp(key, "netmask") == 0) {
            if (parse_ip(value, &cur->netmask) < 0)
                fprintf(stderr, "[walkies] Invalid netmask: %s\n", value);
        } else if (strcmp(key, "gateway") == 0) {
            if (parse_ip(value, &cur->gateway) < 0)
                fprintf(stderr, "[walkies] Invalid gateway: %s\n", value);
        } else if (strcmp(key, "mtu") == 0) {
            cur->mtu = (uint32_t)atoi(value);
        }
        /* Unrecognised option keys are silently ignored for forward compat */
    }

    fclose(f);
    return 0;
}

/* =========================================================================
 * netctl message helpers
 * ========================================================================= */

typedef struct {
    uint8_t  buf[NETCTL_MSG_MAX_SIZE];
    uint32_t len;
} netctl_msg_t;

static uint32_t g_seq = 1;  /* Monotonically increasing sequence counter */

/* Align a byte count up to the next 4-byte boundary. */
static uint32_t attr_align4(uint32_t n)
{
    return (n + 3U) & ~3U;
}

/* Initialise a message buffer with a header of the given type. */
static void msg_init(netctl_msg_t *msg, uint16_t type, uint16_t flags)
{
    memset(msg, 0, sizeof(*msg));
    netctl_msg_header_t *hdr = (netctl_msg_header_t *)msg->buf;
    hdr->msg_type    = type;
    hdr->msg_flags   = flags;
    hdr->msg_seq     = g_seq++;
    hdr->msg_pid     = (uint32_t)getpid();
    hdr->msg_version = NETCTL_VERSION;
    msg->len         = sizeof(netctl_msg_header_t);
    hdr->msg_len     = msg->len;
}

/* Append a TLV attribute to a message buffer.
 * Returns 0 on success, -1 if the buffer would overflow. */
static int msg_add_attr(netctl_msg_t *msg, uint16_t type,
                        const void *data, uint16_t data_len)
{
    uint32_t raw_total   = sizeof(netctl_attr_header_t) + data_len;
    uint32_t align_total = attr_align4(raw_total);

    if (msg->len + align_total > NETCTL_MSG_MAX_SIZE) {
        fprintf(stderr, "[walkies] Message buffer overflow adding attr %u\n", type);
        return -1;
    }

    netctl_attr_header_t *ah = (netctl_attr_header_t *)(msg->buf + msg->len);
    ah->attr_type = type;
    ah->attr_len  = (uint16_t)align_total;
    memcpy(msg->buf + msg->len + sizeof(netctl_attr_header_t), data, data_len);

    /* Zero any padding bytes */
    if (align_total > raw_total)
        memset(msg->buf + msg->len + raw_total, 0, align_total - raw_total);

    msg->len += align_total;
    ((netctl_msg_header_t *)msg->buf)->msg_len = msg->len;
    return 0;
}

/* Send a composed message over the netctl socket. */
static int netctl_send(int fd, netctl_msg_t *msg)
{
    struct iovec iov = { msg->buf, msg->len };
    struct msghdr mhdr;
    memset(&mhdr, 0, sizeof(mhdr));
    mhdr.msg_iov    = &iov;
    mhdr.msg_iovlen = 1;

    if (sendmsg(fd, &mhdr, 0) < 0) {
        fprintf(stderr, "[walkies] sendmsg: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* Receive one message into buf.  Returns bytes received, or -1 on error. */
static ssize_t netctl_recv(int fd, uint8_t *buf, size_t bufsz)
{
    struct iovec iov = { buf, bufsz };
    struct msghdr mhdr;
    memset(&mhdr, 0, sizeof(mhdr));
    mhdr.msg_iov    = &iov;
    mhdr.msg_iovlen = 1;

    ssize_t n = recvmsg(fd, &mhdr, 0);
    if (n < 0)
        fprintf(stderr, "[walkies] recvmsg: %s\n", strerror(errno));
    return n;
}

/* =========================================================================
 * Attribute parsing helpers
 * ========================================================================= */

/*
 * Walk the TLV chain in a received message and return a pointer to the
 * payload of the first attribute matching want_type.
 * If payload_len is non-NULL it receives the payload byte count.
 * Returns NULL if the attribute is not present.
 */
static const void *find_attr(const uint8_t *msg, ssize_t msglen,
                              uint16_t want_type, uint16_t *payload_len)
{
    size_t off = sizeof(netctl_msg_header_t);

    while ((ssize_t)(off + sizeof(netctl_attr_header_t)) <= msglen) {
        const netctl_attr_header_t *ah =
            (const netctl_attr_header_t *)(msg + off);

        if (ah->attr_len < sizeof(netctl_attr_header_t))
            break;  /* Malformed attribute, stop walking */

        if (ah->attr_type == want_type) {
            uint16_t plen =
                ah->attr_len - (uint16_t)sizeof(netctl_attr_header_t);
            if (payload_len)
                *payload_len = plen;
            return msg + off + sizeof(netctl_attr_header_t);
        }

        off += attr_align4(ah->attr_len);
    }

    return NULL;
}

/* =========================================================================
 * Device discovery — resolve an interface name to its kernel ifindex
 * ========================================================================= */

/*
 * Send NETCTL_MSG_NETDEV_LIST and scan the responses for an interface
 * whose NETCTL_ATTR_IFNAME matches name.  On success, *ifindex_out is
 * set and 0 is returned; -1 is returned if not found.
 */
static int find_ifindex(int fd, const char *name, uint32_t *ifindex_out)
{
    netctl_msg_t req;
    msg_init(&req, NETCTL_MSG_NETDEV_LIST, 0);
    if (netctl_send(fd, &req) < 0)
        return -1;

    uint8_t rbuf[NETCTL_RECV_BUF_SIZE];

    for (;;) {
        ssize_t n = netctl_recv(fd, rbuf, sizeof(rbuf));
        if (n <= 0)
            break;

        /* The kernel may return a multi-message response; each recv gives one. */
        const netctl_msg_header_t *hdr = (const netctl_msg_header_t *)rbuf;
        ssize_t msg_len = (ssize_t)hdr->msg_len;
        if (msg_len <= 0 || msg_len > n)
            msg_len = n;

        uint16_t plen = 0;
        const void *nm = find_attr(rbuf, msg_len, NETCTL_ATTR_IFNAME, &plen);
        if (nm && plen > 0 &&
            strncmp((const char *)nm, name, (size_t)plen) == 0) {
            const void *idx = find_attr(rbuf, msg_len, NETCTL_ATTR_IFINDEX,
                                        &plen);
            if (idx && plen >= sizeof(uint32_t)) {
                memcpy(ifindex_out, idx, sizeof(uint32_t));
                return 0;
            }
        }

        /* If this was the last record (no more to follow), stop. */
        if ((size_t)n < sizeof(netctl_msg_header_t) ||
            hdr->msg_type == 0)
            break;
    }

    fprintf(stderr, "[walkies] Interface not found: %s\n", name);
    return -1;
}

/* =========================================================================
 * Configuration operations
 * ========================================================================= */

/* Set NETCTL_FLAG_UP | NETCTL_FLAG_RUNNING on an interface. */
static int iface_set_up(int fd, uint32_t ifindex)
{
    netctl_msg_t msg;
    msg_init(&msg, NETCTL_MSG_NETDEV_SET, 0);

    if (msg_add_attr(&msg, NETCTL_ATTR_IFINDEX, &ifindex, sizeof(ifindex)) < 0)
        return -1;

    uint32_t flags = NETCTL_FLAG_UP | NETCTL_FLAG_RUNNING;
    if (msg_add_attr(&msg, NETCTL_ATTR_FLAGS, &flags, sizeof(flags)) < 0)
        return -1;

    return netctl_send(fd, &msg);
}

/* Set the MTU on an interface. */
static int iface_set_mtu(int fd, uint32_t ifindex, uint32_t mtu)
{
    netctl_msg_t msg;
    msg_init(&msg, NETCTL_MSG_NETDEV_SET, 0);

    if (msg_add_attr(&msg, NETCTL_ATTR_IFINDEX, &ifindex, sizeof(ifindex)) < 0)
        return -1;
    if (msg_add_attr(&msg, NETCTL_ATTR_MTU, &mtu, sizeof(mtu)) < 0)
        return -1;

    return netctl_send(fd, &msg);
}

/*
 * Add an IPv4 address to an interface.
 * addr_hbo is in host byte order; the kernel receives it in network byte order.
 */
static int iface_add_addr(int fd, uint32_t ifindex,
                          uint32_t addr_hbo, uint8_t prefix_len)
{
    netctl_msg_t msg;
    msg_init(&msg, NETCTL_MSG_ADDR_NEW, 0);

    if (msg_add_attr(&msg, NETCTL_ATTR_IFINDEX, &ifindex, sizeof(ifindex)) < 0)
        return -1;

    uint16_t family = NETCTL_ADDR_FAMILY_INET;
    if (msg_add_attr(&msg, NETCTL_ATTR_ADDR_FAMILY, &family, sizeof(family)) < 0)
        return -1;

    uint32_t addr_nbo = htonl(addr_hbo);
    if (msg_add_attr(&msg, NETCTL_ATTR_ADDR_VALUE,
                     &addr_nbo, sizeof(addr_nbo)) < 0)
        return -1;

    if (msg_add_attr(&msg, NETCTL_ATTR_ADDR_PREFIX,
                     &prefix_len, sizeof(prefix_len)) < 0)
        return -1;

    return netctl_send(fd, &msg);
}

/*
 * Add a default route (0.0.0.0/0) via a gateway on an output interface.
 * gw_hbo is in host byte order.
 */
static int route_add_default(int fd, uint32_t gw_hbo, uint32_t ifindex)
{
    netctl_msg_t msg;
    msg_init(&msg, NETCTL_MSG_ROUTE_NEW, 0);

    uint32_t dst = DEFAULT_ROUTE_DST;
    if (msg_add_attr(&msg, NETCTL_ATTR_ROUTE_DST, &dst, sizeof(dst)) < 0)
        return -1;

    uint32_t gw_nbo = htonl(gw_hbo);
    if (msg_add_attr(&msg, NETCTL_ATTR_ROUTE_GW, &gw_nbo, sizeof(gw_nbo)) < 0)
        return -1;

    if (msg_add_attr(&msg, NETCTL_ATTR_ROUTE_OIF, &ifindex, sizeof(ifindex)) < 0)
        return -1;

    return netctl_send(fd, &msg);
}

/* =========================================================================
 * Apply one interface stanza
 * ========================================================================= */

static const char *method_name(iface_method_t m)
{
    switch (m) {
        case METHOD_LOOPBACK: return "loopback";
        case METHOD_STATIC:   return "static";
        case METHOD_DHCP:     return "dhcp";
        default:              return "manual";
    }
}

static int apply_iface(int fd, iface_config_t *cfg)
{
    uint32_t ifindex = 0;

    fprintf(stdout, "[walkies] Configuring %s (method: %s)\n",
            cfg->name, method_name(cfg->method));

    if (find_ifindex(fd, cfg->name, &ifindex) < 0)
        return -1;

    fprintf(stdout, "[walkies]   ifindex=%u\n", ifindex);

    /* Bring the interface UP */
    if (iface_set_up(fd, ifindex) < 0) {
        fprintf(stderr, "[walkies]   Failed to bring up %s\n", cfg->name);
        return -1;
    }
    fprintf(stdout, "[walkies]   UP\n");

    /* Optional MTU override */
    if (cfg->mtu > 0) {
        if (iface_set_mtu(fd, ifindex, cfg->mtu) < 0)
            fprintf(stderr, "[walkies]   Warning: failed to set MTU=%u on %s\n",
                    cfg->mtu, cfg->name);
        else
            fprintf(stdout, "[walkies]   MTU=%u\n", cfg->mtu);
    }

    /* Static address assignment */
    if (cfg->method == METHOD_STATIC && cfg->address != 0) {
        uint8_t prefix = (uint8_t)netmask_to_prefix(cfg->netmask);
        if (iface_add_addr(fd, ifindex, cfg->address, prefix) < 0) {
            fprintf(stderr, "[walkies]   Failed to add address to %s\n",
                    cfg->name);
            return -1;
        }
        struct in_addr ia;
        ia.s_addr = htonl(cfg->address);
        fprintf(stdout, "[walkies]   Address=%s/%u\n", inet_ntoa(ia), prefix);
    }

    /* Loopback: default to 127.0.0.1/8 when no address was specified */
    if (cfg->method == METHOD_LOOPBACK && cfg->address == 0) {
        if (iface_add_addr(fd, ifindex, LOOPBACK_ADDR_HBO, LOOPBACK_PREFIX_LEN) < 0) {
            fprintf(stderr, "[walkies]   Failed to add loopback address\n");
            return -1;
        }
        fprintf(stdout, "[walkies]   Address=127.0.0.1/8\n");
    }

    /* Default route via gateway */
    if (cfg->gateway != 0) {
        if (route_add_default(fd, cfg->gateway, ifindex) < 0)
            fprintf(stderr, "[walkies]   Warning: failed to add default route "
                    "via %s\n", cfg->name);
        else {
            struct in_addr gw;
            gw.s_addr = htonl(cfg->gateway);
            fprintf(stdout, "[walkies]   Gateway=%s\n", inet_ntoa(gw));
        }
    }

    /* DHCP: interface is UP; the actual DHCP client is a separate component */
    if (cfg->method == METHOD_DHCP)
        fprintf(stdout, "[walkies]   DHCP ready — waiting for dhcp client\n");

    cfg->configured = 1;
    return 0;
}

/* =========================================================================
 * Event monitor (daemon mode — Phase 2)
 * ========================================================================= */

static void monitor_events(int fd)
{
    uint8_t rbuf[NETCTL_RECV_BUF_SIZE];

    fprintf(stdout, "[walkies] Monitoring network events...\n");
    fflush(stdout);

    for (;;) {
        ssize_t n = netctl_recv(fd, rbuf, sizeof(rbuf));
        if (n <= 0)
            break;

        const netctl_msg_header_t *hdr = (const netctl_msg_header_t *)rbuf;
        ssize_t msg_len = (ssize_t)hdr->msg_len;
        if (msg_len <= 0 || msg_len > n)
            msg_len = n;

        switch (hdr->msg_type) {

            case NETCTL_MSG_NETDEV_NEW: {
                uint16_t plen = 0;
                const void *nm = find_attr(rbuf, msg_len,
                                           NETCTL_ATTR_IFNAME, &plen);
                const void *fl = find_attr(rbuf, msg_len,
                                           NETCTL_ATTR_FLAGS, NULL);
                uint32_t flags = 0;
                if (fl)
                    memcpy(&flags, fl, sizeof(uint32_t));
                fprintf(stdout, "[walkies] event: link %-16.*s flags=0x%04x"
                        " (%s%s%s)\n",
                        nm ? (int)plen : 7,
                        nm ? (const char *)nm : "unknown",
                        flags,
                        (flags & NETCTL_FLAG_UP)      ? "UP "      : "",
                        (flags & NETCTL_FLAG_RUNNING)  ? "RUNNING " : "",
                        (flags & NETCTL_FLAG_CARRIER)  ? "CARRIER"  : "");
                break;
            }

            case NETCTL_MSG_ADDR_NEW:
                fprintf(stdout, "[walkies] event: address added\n");
                break;

            case NETCTL_MSG_ADDR_DEL:
                fprintf(stdout, "[walkies] event: address removed\n");
                break;

            case NETCTL_MSG_ROUTE_NEW:
                fprintf(stdout, "[walkies] event: route added\n");
                break;

            case NETCTL_MSG_ROUTE_DEL:
                fprintf(stdout, "[walkies] event: route removed\n");
                break;

            default:
                fprintf(stdout, "[walkies] event: unknown type %u\n",
                        hdr->msg_type);
                break;
        }
        fflush(stdout);
    }
}

/* =========================================================================
 * main
 * ========================================================================= */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-c <config>] [-m]\n"
            "\n"
            "  -c <config>   Path to interfaces config file\n"
            "                (default: /etc/interfaces)\n"
            "  -m            Monitor mode: subscribe to netctl events and\n"
            "                log them; stays running as a daemon\n"
            "\n"
            "walkies configures network interfaces using the BlueyOS\n"
            "AF_BLUEY_NETCTL control plane and reads /etc/interfaces.\n",
            prog);
}

int main(int argc, char *argv[])
{
    const char *config_path = "/etc/interfaces";
    int monitor_mode = 0;
    int opt;

    while ((opt = getopt(argc, argv, "c:mh")) != -1) {
        switch (opt) {
            case 'c':
                config_path = optarg;
                break;
            case 'm':
                monitor_mode = 1;
                break;
            case 'h':
                usage(argv[0]);
                return 0;
            default:
                usage(argv[0]);
                return 1;
        }
    }

    fprintf(stdout, "[walkies] Starting network configuration for BlueyOS\n");

    /* Parse /etc/interfaces — non-fatal if absent (monitor-only is valid) */
    if (parse_interfaces(config_path) < 0)
        fprintf(stderr, "[walkies] Warning: could not parse %s; "
                "skipping static configuration\n", config_path);

    /* Open netctl socket */
    int fd = socket(AF_BLUEY_NETCTL, SOCK_NETCTL, 0);
    if (fd < 0) {
        fprintf(stderr, "[walkies] socket(AF_BLUEY_NETCTL, SOCK_NETCTL): %s\n",
                strerror(errno));
        return 1;
    }

    /* Apply configuration for every parsed interface stanza */
    int errors = 0;
    for (int i = 0; i < g_iface_count; i++) {
        if (apply_iface(fd, &g_ifaces[i]) < 0) {
            fprintf(stderr, "[walkies] Failed to fully configure %s\n",
                    g_ifaces[i].name);
            errors++;
        }
    }

    if (g_iface_count == 0)
        fprintf(stdout, "[walkies] No interfaces found in %s\n", config_path);

    if (monitor_mode) {
        /*
         * Bind to all multicast groups *after* one-shot configuration so
         * that the recvmsg calls inside find_ifindex only see unicast
         * responses, not events.
         */
        struct sockaddr_netctl sa;
        memset(&sa, 0, sizeof(sa));
        sa.family = AF_BLUEY_NETCTL;
        sa.groups = NETCTL_GROUP_ALL;

        if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
            fprintf(stderr, "[walkies] bind(multicast): %s — "
                    "events may not be received\n", strerror(errno));

        monitor_events(fd);
    }

    close(fd);

    fprintf(stdout, "[walkies] Done: %d interface(s) configured, %d error(s)\n",
            g_iface_count, errors);

    return errors > 0 ? 1 : 0;
}
