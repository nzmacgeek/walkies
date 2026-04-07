/*
 * walkies.h — netctl control plane constants and structures for BlueyOS
 *
 * Matches the AF_BLUEY_NETCTL socket family implemented in the BlueyOS kernel.
 * Userspace programs include this header to communicate with the kernel's
 * network configuration plane.
 *
 * "Let's go for a walkies!" — Bluey
 */

#ifndef WALKIES_H
#define WALKIES_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * Socket family and type
 * ------------------------------------------------------------------------- */

#define AF_BLUEY_NETCTL     2   /* Network control socket family */
#define SOCK_NETCTL         3   /* Message-oriented netctl socket type */

/* -------------------------------------------------------------------------
 * sockaddr for binding to multicast groups
 * ------------------------------------------------------------------------- */

struct sockaddr_netctl {
    uint16_t family;    /* AF_BLUEY_NETCTL */
    uint16_t pad;
    uint32_t groups;    /* Multicast group bitmask (NETCTL_GROUP_*) */
};

/* -------------------------------------------------------------------------
 * Message header — precedes every netctl message
 * ------------------------------------------------------------------------- */

typedef struct {
    uint32_t msg_len;       /* Total message length including this header */
    uint16_t msg_type;      /* Message type (NETCTL_MSG_*) */
    uint16_t msg_flags;     /* Message flags */
    uint32_t msg_seq;       /* Sequence number for request/response matching */
    uint32_t msg_pid;       /* Sender process ID */
    uint16_t msg_version;   /* Protocol version — currently 1 */
    uint16_t msg_reserved;
} netctl_msg_header_t;

/* -------------------------------------------------------------------------
 * Attribute header — TLV, follows the message header and may be chained
 * All attributes are 4-byte aligned.
 * ------------------------------------------------------------------------- */

typedef struct {
    uint16_t attr_len;      /* Total length including this header, 4-byte aligned */
    uint16_t attr_type;     /* Attribute type (NETCTL_ATTR_*) */
    /* Payload bytes follow immediately */
} netctl_attr_header_t;

/* -------------------------------------------------------------------------
 * Protocol version
 * ------------------------------------------------------------------------- */

#define NETCTL_VERSION      1

/* -------------------------------------------------------------------------
 * Message types
 * ------------------------------------------------------------------------- */

/* Device operations */
#define NETCTL_MSG_NETDEV_NEW       10  /* Event: link state changed */
#define NETCTL_MSG_NETDEV_GET       12  /* Request: get device info by ifindex */
#define NETCTL_MSG_NETDEV_SET       13  /* Request: set device attributes */
#define NETCTL_MSG_NETDEV_LIST      14  /* Request: list all network devices */

/* Address operations */
#define NETCTL_MSG_ADDR_NEW         20  /* Request/Event: add IP address */
#define NETCTL_MSG_ADDR_DEL         21  /* Request/Event: remove IP address */
#define NETCTL_MSG_ADDR_LIST        23  /* Request: list addresses */

/* Route operations */
#define NETCTL_MSG_ROUTE_NEW        30  /* Request/Event: add route */
#define NETCTL_MSG_ROUTE_DEL        31  /* Request/Event: remove route */
#define NETCTL_MSG_ROUTE_LIST       33  /* Request: list routes */

/* -------------------------------------------------------------------------
 * Attribute types
 * ------------------------------------------------------------------------- */

/* Device attributes */
#define NETCTL_ATTR_IFINDEX         10  /* Interface index (uint32_t) */
#define NETCTL_ATTR_IFNAME          11  /* Interface name (NUL-terminated string) */
#define NETCTL_ATTR_MTU             12  /* MTU value (uint32_t) */
#define NETCTL_ATTR_MAC             13  /* MAC address (6 bytes) */
#define NETCTL_ATTR_FLAGS           14  /* Device flags bitmask (uint32_t) */
#define NETCTL_ATTR_CARRIER         15  /* Carrier state (uint8_t: 0=down, 1=up) */

/* Address attributes */
#define NETCTL_ATTR_ADDR_FAMILY     20  /* Address family (uint16_t: 2=AF_INET) */
#define NETCTL_ATTR_ADDR_VALUE      21  /* Address value (4 bytes for IPv4, NBO) */
#define NETCTL_ATTR_ADDR_PREFIX     22  /* Prefix length (uint8_t, e.g. 24 for /24) */

/* Route attributes */
#define NETCTL_ATTR_ROUTE_DST       30  /* Destination prefix (4 bytes IPv4, NBO) */
#define NETCTL_ATTR_ROUTE_GW        31  /* Gateway address (4 bytes IPv4, NBO) */
#define NETCTL_ATTR_ROUTE_OIF       32  /* Output interface index (uint32_t) */
#define NETCTL_ATTR_ROUTE_METRIC    33  /* Route metric / priority (uint32_t) */

/* -------------------------------------------------------------------------
 * Device flags (bitmask, used in NETCTL_ATTR_FLAGS)
 * ------------------------------------------------------------------------- */

#define NETCTL_FLAG_UP              0x0001  /* Administratively up */
#define NETCTL_FLAG_RUNNING         0x0002  /* Resources allocated */
#define NETCTL_FLAG_CARRIER         0x0004  /* Physical link detected */
#define NETCTL_FLAG_LOOPBACK        0x0008  /* Loopback interface */
#define NETCTL_FLAG_BROADCAST       0x0010  /* Supports broadcast */

/* -------------------------------------------------------------------------
 * Multicast groups (bitmask, used in sockaddr_netctl.groups)
 * ------------------------------------------------------------------------- */

#define NETCTL_GROUP_NONE           0   /* No multicast groups */
#define NETCTL_GROUP_LINK           1   /* Link state change events */
#define NETCTL_GROUP_ADDR           2   /* Address add/remove events */
#define NETCTL_GROUP_ROUTE          4   /* Route add/remove events */
#define NETCTL_GROUP_ALL            7   /* All event groups */

/* -------------------------------------------------------------------------
 * Address families used in NETCTL_ATTR_ADDR_FAMILY payloads
 * (mirrors standard AF_* values for portability with the kernel)
 * ------------------------------------------------------------------------- */

#define NETCTL_ADDR_FAMILY_INET     2   /* IPv4 — AF_INET */

/* -------------------------------------------------------------------------
 * Well-known address constants (host byte order)
 * ------------------------------------------------------------------------- */

#define LOOPBACK_ADDR_HBO   ((uint32_t)0x7F000001U)  /* 127.0.0.1 */
#define LOOPBACK_PREFIX_LEN ((uint8_t)8)             /* /8 */
#define DEFAULT_ROUTE_DST   ((uint32_t)0U)           /* 0.0.0.0 — default route */

/* -------------------------------------------------------------------------
 * Buffer sizes
 * ------------------------------------------------------------------------- */

#define NETCTL_MSG_MAX_SIZE         4096    /* Maximum outgoing message size */
#define NETCTL_RECV_BUF_SIZE        8192    /* Receive buffer size */

#endif /* WALKIES_H */
