/* PrismOS - packet helpers (no hardware, no heap).
 *
 * Pure functions for Ethernet / ARP / IPv4 / UDP / TCP /
 * DHCP / DNS / HTTP. The kernel network stack and the host
 * unit tests share this file, so it touches no ports, no
 * clock and no allocator. All multi-byte fields are big
 * endian on the wire; helpers here convert explicitly.
 *
 * Address layout: an IPv4 address is 4 bytes in wire order.
 * A hardware address is 6 bytes. Callers keep them as
 * byte arrays, never as text, so no formatting is needed
 * here.
 */
#ifndef PRISM_NET_H
#define PRISM_NET_H

#include <stddef.h>
#include <stdint.h>

/* One's-complement sum over data (not yet complemented). */
uint16_t net_sum(const uint8_t *data, size_t len);
/* Internet checksum (complemented sum). */
uint16_t net_checksum(const uint8_t *data, size_t len);
/* TCP/UDP checksum over pseudo header + header + payload. */
uint16_t net_transport_checksum(const uint8_t *src_ip,
                                const uint8_t *dst_ip, uint8_t proto,
                                const uint8_t *data, size_t len);

uint16_t net_read16(const uint8_t *p);
uint32_t net_read32(const uint8_t *p);
void net_write16(uint8_t *p, uint16_t v);
void net_write32(uint8_t *p, uint32_t v);

/* Ethernet header (14 bytes). Returns 0 on success. */
int net_eth_parse(const uint8_t *frame, size_t len, uint8_t *dst,
                  uint8_t *src, uint16_t *ethertype,
                  const uint8_t **payload, size_t *payload_len);
/* Build an Ethernet header at out (14 bytes). */
void net_eth_build(uint8_t *out, const uint8_t *dst, const uint8_t *src,
                   uint16_t ethertype);

/* ARP: build a request (28-byte payload) for target_ip. */
void net_arp_build_request(uint8_t *out, const uint8_t *sender_hw,
                           const uint8_t *sender_ip,
                           const uint8_t *target_ip);
/* Parse an ARP reply: returns 1 and fills sender_hw when the
 * frame is an ARP reply for target_ip, else 0. */
int net_arp_parse_reply(const uint8_t *payload, size_t len,
                        const uint8_t *target_ip, uint8_t *sender_hw);

/* IPv4 header (20 bytes, no options). Returns 0 on success,
 * and fills the payload pointer/length and the protocol. */
int net_ipv4_parse(const uint8_t *data, size_t len, uint8_t *proto,
                   const uint8_t **payload, size_t *payload_len,
                   uint8_t *src_ip, uint8_t *dst_ip);
/* Build a 20-byte IPv4 header (checksum filled in). */
void net_ipv4_build(uint8_t *out, uint16_t total_len, uint8_t proto,
                    const uint8_t *src_ip, const uint8_t *dst_ip,
                    uint16_t id);

/* UDP header (8 bytes). */
int net_udp_parse(const uint8_t *data, size_t len, uint16_t *src_port,
                  uint16_t *dst_port, const uint8_t **payload,
                  size_t *payload_len);
void net_udp_build(uint8_t *out, uint16_t src_port, uint16_t dst_port,
                   uint16_t len, const uint8_t *src_ip,
                   const uint8_t *dst_ip, const uint8_t *payload,
                   size_t payload_len);

/* DHCP: build a discover (xid given) into out, return length.
 * The packet is a full DHCP message (236 bytes fixed part). */
size_t net_dhcp_build_discover(uint8_t *out, const uint8_t *hwaddr,
                               uint32_t xid);
/* Build a request for offered_ip with server_ip, return length. */
size_t net_dhcp_build_request(uint8_t *out, const uint8_t *hwaddr,
                              uint32_t xid, const uint8_t *offered_ip,
                              const uint8_t *server_ip);
/* DHCP offer/ack result. */
typedef struct {
    uint8_t yiaddr[4];
    uint8_t server_ip[4];
    uint8_t subnet[4];
    uint8_t router[4];
    uint8_t dns[4];
    int is_ack;
} prism_dhcp_result;
/* Parse a DHCP message: returns 1 when it is an offer (or an
 * ack when want_ack is set) for xid, else 0. */
int net_dhcp_parse(const uint8_t *data, size_t len, uint32_t xid,
                   int want_ack, prism_dhcp_result *out);

/* DNS: build a query for name (dotted, e.g. "example.com")
 * with id, into out; return length. */
size_t net_dns_build_query(uint8_t *out, const char *name, uint16_t id);
/* Parse a DNS response: returns 1 and fills out_ip with the
 * first A record answer, else 0. */
int net_dns_parse(const uint8_t *data, size_t len, uint16_t id,
                  uint8_t *out_ip);

/* TCP header (20 bytes, no options). */
typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t flags;
    uint16_t window;
} prism_tcp_hdr;
/* Parse a TCP segment header. Returns 0 on success. */
int net_tcp_parse(const uint8_t *data, size_t len, prism_tcp_hdr *hdr,
                  const uint8_t **payload, size_t *payload_len);
/* Build a TCP header (20 bytes, checksum filled in). */
void net_tcp_build(uint8_t *out, uint16_t src_port, uint16_t dst_port,
                   uint32_t seq, uint32_t ack, uint8_t flags,
                   uint16_t window, const uint8_t *src_ip,
                   const uint8_t *dst_ip, const uint8_t *payload,
                   size_t payload_len);
/* Build a TCP header for IPv6 (20 bytes, checksum filled
 * in over the IPv6 pseudo-header). */
void net_tcp_build6(uint8_t *out, uint16_t src_port, uint16_t dst_port,
                    uint32_t seq, uint32_t ack, uint8_t flags,
                    uint16_t window, const uint8_t *src_ip6,
                    const uint8_t *dst_ip6, const uint8_t *payload,
                    size_t payload_len);

/* TCP flag bits. */
#define PRISM_TCP_FIN 0x01
#define PRISM_TCP_SYN 0x02
#define PRISM_TCP_RST 0x04
#define PRISM_TCP_PSH 0x08
#define PRISM_TCP_ACK 0x10

/* HTTP: find the body in a response (after the blank line).
 * Returns the body pointer, or NULL when the headers are
 * incomplete. Sets *body_len and *status_code (0 if unknown). */
const uint8_t *net_http_body(const uint8_t *data, size_t len,
                             size_t *body_len, int *status_code);

/* Strip <...> tags from text in place, keeping the visible
 * text. Returns the new length. */
size_t net_html_strip_tags(char *text, size_t len);

/* ---- HTTP cookies (pure string logic; the jar itself is
 * a text file the caller owns) ----
 * Jar text format: one cookie per line,
 * "host<TAB>name<TAB>value\n". Expiry/path attributes are
 * not tracked (session-style storage with domain matching
 * only) — a deliberate, documented subset. */

/* Extract the first name=value pair from a Set-Cookie
 * header value (attributes after ';' are dropped).
 * Returns 0 on success, -1 when there is no pair. */
int net_cookie_parse_pair(const char *set_cookie, char *name,
                          size_t name_cap, char *value,
                          size_t value_cap);

/* Build the "name=value; name2=value2" header value for
 * host from jar text. A jar entry matches when its host
 * equals the request host or is a parent domain of it.
 * Returns the length written (0 = no cookies). */
size_t net_cookie_build_header(const char *jar, const char *host,
                               char *out, size_t cap);

/* Insert or replace one cookie in jar text (in place,
 * NUL-terminated, cap bytes including the NUL).
 * Returns 0 on success, -1 when it does not fit. */
int net_cookie_jar_update(char *jar, size_t cap, const char *host,
                          const char *name, const char *value);

/* ---- IPv6 (addresses are 16 bytes in wire order) ---- */

/* Build a 40-byte IPv6 header (version 6, no flow label). */
void net_ipv6_build(uint8_t *out, uint16_t payload_len,
                    uint8_t next_header, uint8_t hop_limit,
                    const uint8_t *src, const uint8_t *dst);
/* Parse an IPv6 header. Returns 0 on success. Extension
 * headers are not walked: next_header is reported as-is. */
int net_ipv6_parse(const uint8_t *data, size_t len,
                   uint8_t *next_header, const uint8_t **payload,
                   size_t *payload_len, uint8_t *src, uint8_t *dst);

/* Checksum over the IPv6 pseudo header + data, for ICMPv6
 * (next_header 58) and UDP/TCP over IPv6. The data's own
 * checksum field must be zero when computing. */
uint16_t net_ipv6_transport_checksum(const uint8_t *src,
                                     const uint8_t *dst,
                                     uint8_t next_header,
                                     const uint8_t *data, size_t len);

/* Build an ICMPv6 message (type/code/checksum + body) into
 * out; returns the total length. */
size_t net_icmp6_build(uint8_t *out, const uint8_t *src,
                       const uint8_t *dst, uint8_t type, uint8_t code,
                       const uint8_t *body, size_t body_len);

/* fe80::/64 + modified EUI-64 from a MAC address. */
void net_ipv6_link_local(uint8_t *out, const uint8_t *mac);
/* ff02::1:ffXX:XXXX solicited-node multicast for target. */
void net_ipv6_solicited_node(uint8_t *out, const uint8_t *target);
/* Ethernet multicast MAC for an IPv6 multicast address. */
void net_ipv6_multicast_mac(uint8_t *out, const uint8_t *addr);

/* Parse a textual IPv6 address ("::" compression allowed).
 * Returns 0 on success. */
int net_ipv6_parse_addr(const char *text, uint8_t *out);
/* Format an IPv6 address (longest zero run becomes "::").
 * out needs at least 40 bytes. */
void net_ipv6_format_addr(const uint8_t *addr, char *out);

/* Parse a Router Advertisement payload (after the IPv6
 * header). On success returns 0 and fills: router MAC from
 * the source link-layer option (when present), and the
 * first advertised on-link/autonomous prefix (when a Prefix
 * Information option is present): prefix_out (16 bytes),
 * *prefix_len, and *a_flag (autonomous, usable for SLAAC). */
int net_nd_parse_ra(const uint8_t *icmp, size_t len,
                    uint8_t *router_hw, int *has_hw,
                    uint8_t *prefix_out, int *prefix_len, int *a_flag);

/* Build a Neighbor Advertisement body (after the ICMPv6
 * header fields): flags byte R=0,S=1,O=1, target, and a
 * target link-layer option with mac. Writes 4+16+8 bytes
 * into body and returns 28. */
size_t net_nd_build_na_body(uint8_t *body, const uint8_t *target,
                            const uint8_t *mac);
/* Build a Neighbor Solicitation body: reserved + target +
 * source link-layer option. Returns 28. */
size_t net_nd_build_ns_body(uint8_t *body, const uint8_t *target,
                            const uint8_t *mac);

/* Split "host" or "host:port" into a bare host and a port
 * number. Without ':' the port is 80. A host with two or
 * more ':' (an IPv6 literal) is passed through with port
 * 80. Returns 0 on success, -1 on malformed input (empty
 * host, empty or non-numeric port, port out of range 1..65535,
 * host too long for out). The split lets fetch reach test
 * servers on unprivileged ports, which matters where
 * binding port 80 needs privileges (CI runners run as an
 * unprivileged user). */
int net_split_host_port(const char *host, char *out_host, size_t cap,
                        uint16_t *out_port);

#endif /* PRISM_NET_H */
