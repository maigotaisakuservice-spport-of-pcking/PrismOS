/* PrismOS - packet helpers (pure, see net.h). */
#include "net.h"

uint16_t net_sum(const uint8_t *data, size_t len) {
    uint32_t sum = 0;
    size_t i = 0;
    while (i + 1 < len) {
        sum += (uint32_t)(((uint16_t)data[i] << 8) | data[i + 1]);
        i += 2;
    }
    if (i < len) {
        sum += (uint32_t)((uint16_t)data[i] << 8);
    }
    while ((sum >> 16) != 0) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)sum;
}

uint16_t net_checksum(const uint8_t *data, size_t len) {
    return (uint16_t)~net_sum(data, len);
}

uint16_t net_transport_checksum(const uint8_t *src_ip,
                                const uint8_t *dst_ip, uint8_t proto,
                                const uint8_t *data, size_t len) {
    uint32_t sum = 0;
    for (int i = 0; i < 4; i += 2) {
        sum += (uint32_t)(((uint16_t)src_ip[i] << 8) | src_ip[i + 1]);
        sum += (uint32_t)(((uint16_t)dst_ip[i] << 8) | dst_ip[i + 1]);
    }
    sum += (uint32_t)proto;
    sum += (uint32_t)len;
    sum += net_sum(data, len);
    while ((sum >> 16) != 0) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    uint16_t out = (uint16_t)~sum;
    return out == 0 ? (uint16_t)0xFFFF : out;
}

uint16_t net_read16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

uint32_t net_read32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

void net_write16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

void net_write32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}

int net_eth_parse(const uint8_t *frame, size_t len, uint8_t *dst,
                  uint8_t *src, uint16_t *ethertype,
                  const uint8_t **payload, size_t *payload_len) {
    if (frame == NULL || len < 14) {
        return -1;
    }
    for (int i = 0; i < 6; i++) {
        dst[i] = frame[i];
        src[i] = frame[6 + i];
    }
    *ethertype = net_read16(frame + 12);
    *payload = frame + 14;
    *payload_len = len - 14;
    return 0;
}

void net_eth_build(uint8_t *out, const uint8_t *dst, const uint8_t *src,
                   uint16_t ethertype) {
    for (int i = 0; i < 6; i++) {
        out[i] = dst[i];
        out[6 + i] = src[i];
    }
    net_write16(out + 12, ethertype);
}

void net_arp_build_request(uint8_t *out, const uint8_t *sender_hw,
                           const uint8_t *sender_ip,
                           const uint8_t *target_ip) {
    net_write16(out + 0, 1);      /* Ethernet */
    net_write16(out + 2, 0x0800); /* IPv4 */
    out[4] = 6;
    out[5] = 4;
    net_write16(out + 6, 1); /* request */
    for (int i = 0; i < 6; i++) {
        out[8 + i] = sender_hw[i];
    }
    for (int i = 0; i < 4; i++) {
        out[14 + i] = sender_ip[i];
    }
    for (int i = 0; i < 6; i++) {
        out[18 + i] = 0;
    }
    for (int i = 0; i < 4; i++) {
        out[24 + i] = target_ip[i];
    }
}

int net_arp_parse_reply(const uint8_t *payload, size_t len,
                        const uint8_t *target_ip, uint8_t *sender_hw) {
    if (payload == NULL || len < 28) {
        return 0;
    }
    if (net_read16(payload + 0) != 1 || net_read16(payload + 2) != 0x0800) {
        return 0;
    }
    if (net_read16(payload + 6) != 2) {
        return 0; /* not a reply */
    }
    for (int i = 0; i < 4; i++) {
        if (payload[24 + i] != target_ip[i]) {
            return 0;
        }
    }
    for (int i = 0; i < 6; i++) {
        sender_hw[i] = payload[8 + i];
    }
    return 1;
}

int net_ipv4_parse(const uint8_t *data, size_t len, uint8_t *proto,
                   const uint8_t **payload, size_t *payload_len,
                   uint8_t *src_ip, uint8_t *dst_ip) {
    if (data == NULL || len < 20) {
        return -1;
    }
    if ((data[0] >> 4) != 4) {
        return -1;
    }
    size_t hlen = (size_t)(data[0] & 0x0F) * 4;
    if (hlen < 20 || len < hlen) {
        return -1;
    }
    uint16_t total = net_read16(data + 2);
    if (total > len) {
        total = (uint16_t)len;
    }
    *proto = data[9];
    for (int i = 0; i < 4; i++) {
        src_ip[i] = data[12 + i];
        dst_ip[i] = data[16 + i];
    }
    *payload = data + hlen;
    *payload_len = (size_t)total - hlen;
    return 0;
}

void net_ipv4_build(uint8_t *out, uint16_t total_len, uint8_t proto,
                    const uint8_t *src_ip, const uint8_t *dst_ip,
                    uint16_t id) {
    out[0] = 0x45;
    out[1] = 0;
    net_write16(out + 2, total_len);
    net_write16(out + 4, id);
    net_write16(out + 6, 0x4000); /* do not fragment */
    out[8] = 64;                  /* TTL */
    out[9] = proto;
    net_write16(out + 10, 0);
    for (int i = 0; i < 4; i++) {
        out[12 + i] = src_ip[i];
        out[16 + i] = dst_ip[i];
    }
    net_write16(out + 10, net_checksum(out, 20));
}

int net_udp_parse(const uint8_t *data, size_t len, uint16_t *src_port,
                  uint16_t *dst_port, const uint8_t **payload,
                  size_t *payload_len) {
    if (data == NULL || len < 8) {
        return -1;
    }
    *src_port = net_read16(data + 0);
    *dst_port = net_read16(data + 2);
    uint16_t ulen = net_read16(data + 4);
    if (ulen < 8 || (size_t)ulen > len) {
        ulen = (uint16_t)len;
    }
    *payload = data + 8;
    *payload_len = (size_t)ulen - 8;
    return 0;
}

void net_udp_build(uint8_t *out, uint16_t src_port, uint16_t dst_port,
                   uint16_t len, const uint8_t *src_ip,
                   const uint8_t *dst_ip, const uint8_t *payload,
                   size_t payload_len) {
    net_write16(out + 0, src_port);
    net_write16(out + 2, dst_port);
    net_write16(out + 4, len);
    net_write16(out + 6, 0);
    for (size_t i = 0; i < payload_len; i++) {
        out[8 + i] = payload[i];
    }
    uint16_t c = net_transport_checksum(src_ip, dst_ip, 17, out, len);
    net_write16(out + 6, c);
}

static void dhcp_fixed(uint8_t *out, const uint8_t *hwaddr, uint32_t xid,
                       uint8_t msg_type, const uint8_t *req_ip,
                       const uint8_t *server_ip) {
    for (int i = 0; i < 236; i++) {
        out[i] = 0;
    }
    out[0] = 1; /* BOOTREQUEST */
    out[1] = 1; /* Ethernet */
    out[2] = 6;
    out[3] = 0;
    net_write32(out + 4, xid);
    net_write16(out + 8, 0x8000); /* broadcast flag */
    for (int i = 0; i < 6; i++) {
        out[28 + i] = hwaddr[i];
    }
    /* Magic cookie. */
    out[236] = 99;
    out[237] = 130;
    out[238] = 83;
    out[239] = 99;
    size_t p = 240;
    out[p++] = 53; /* message type */
    out[p++] = 1;
    out[p++] = msg_type;
    if (req_ip != NULL) {
        out[p++] = 50; /* requested address */
        out[p++] = 4;
        for (int i = 0; i < 4; i++) {
            out[p++] = req_ip[i];
        }
    }
    if (server_ip != NULL) {
        out[p++] = 54; /* server identifier */
        out[p++] = 4;
        for (int i = 0; i < 4; i++) {
            out[p++] = server_ip[i];
        }
    }
    out[p++] = 55; /* parameter request list */
    out[p++] = 3;
    out[p++] = 1; /* subnet */
    out[p++] = 3; /* router */
    out[p++] = 6; /* DNS */
    out[p++] = 255; /* end */
    (void)p;
}

size_t net_dhcp_build_discover(uint8_t *out, const uint8_t *hwaddr,
                               uint32_t xid) {
    dhcp_fixed(out, hwaddr, xid, 1, NULL, NULL);
    return 240 + 3 + 5 + 1; /* cookie + type + params + end = 250 */
}

size_t net_dhcp_build_request(uint8_t *out, const uint8_t *hwaddr,
                              uint32_t xid, const uint8_t *offered_ip,
                              const uint8_t *server_ip) {
    dhcp_fixed(out, hwaddr, xid, 3, offered_ip, server_ip);
    return 240 + 3 + 6 + 6 + 5 + 1; /* 261 */
}

int net_dhcp_parse(const uint8_t *data, size_t len, uint32_t xid,
                   int want_ack, prism_dhcp_result *out) {
    if (data == NULL || len < 240 || out == NULL) {
        return 0;
    }
    if (data[0] != 2) { /* BOOTREPLY */
        return 0;
    }
    if (net_read32(data + 4) != xid) {
        return 0;
    }
    if (data[236] != 99 || data[237] != 130 || data[238] != 83 ||
        data[239] != 99) {
        return 0;
    }
    for (int i = 0; i < 4; i++) {
        out->yiaddr[i] = data[16 + i];
        out->server_ip[i] = data[20 + i];
        out->subnet[i] = 0;
        out->router[i] = 0;
        out->dns[i] = 0;
    }
    out->is_ack = 0;
    size_t p = 240;
    while (p < len) {
        uint8_t code = data[p];
        if (code == 255) {
            break;
        }
        if (code == 0) {
            p++;
            continue;
        }
        if (p + 1 >= len) {
            break;
        }
        uint8_t n = data[p + 1];
        if (p + 2 + n > len) {
            break;
        }
        const uint8_t *v = data + p + 2;
        if (code == 53 && n >= 1) {
            if (want_ack) {
                if (v[0] != 5) {
                    return 0;
                }
                out->is_ack = 1;
            } else {
                if (v[0] != 2) {
                    return 0;
                }
            }
        } else if (code == 1 && n >= 4) {
            for (int i = 0; i < 4; i++) {
                out->subnet[i] = v[i];
            }
        } else if (code == 3 && n >= 4) {
            for (int i = 0; i < 4; i++) {
                out->router[i] = v[i];
            }
        } else if (code == 6 && n >= 4) {
            for (int i = 0; i < 4; i++) {
                out->dns[i] = v[i];
            }
        } else if (code == 54 && n >= 4) {
            for (int i = 0; i < 4; i++) {
                out->server_ip[i] = v[i];
            }
        }
        p += (size_t)2 + n;
    }
    if (want_ack) {
        return out->is_ack;
    }
    return 1;
}

size_t net_dns_build_query(uint8_t *out, const char *name, uint16_t id) {
    net_write16(out + 0, id);
    net_write16(out + 2, 0x0100); /* recursion desired */
    net_write16(out + 4, 1);      /* questions */
    net_write16(out + 6, 0);
    net_write16(out + 8, 0);
    net_write16(out + 10, 0);
    size_t p = 12;
    const char *s = name;
    while (*s != '\0') {
        const char *dot = s;
        size_t n = 0;
        while (*dot != '\0' && *dot != '.') {
            dot++;
            n++;
        }
        if (n == 0 || n > 63 || p + 1 + n >= 512) {
            return 0;
        }
        out[p++] = (uint8_t)n;
        for (size_t i = 0; i < n; i++) {
            out[p++] = (uint8_t)s[i];
        }
        s = (*dot == '.') ? dot + 1 : dot;
    }
    out[p++] = 0;
    net_write16(out + p, 1); /* A */
    p += 2;
    net_write16(out + p, 1); /* IN */
    p += 2;
    return p;
}

static size_t dns_skip_name(const uint8_t *data, size_t len, size_t p) {
    while (p < len) {
        uint8_t n = data[p];
        if (n == 0) {
            return p + 1;
        }
        if ((n & 0xC0) == 0xC0) {
            return p + 2; /* compression pointer */
        }
        p += (size_t)1 + n;
    }
    return len;
}

int net_dns_parse(const uint8_t *data, size_t len, uint16_t id,
                  uint8_t *out_ip) {
    if (data == NULL || len < 12 || out_ip == NULL) {
        return 0;
    }
    if (net_read16(data + 0) != id) {
        return 0;
    }
    if ((net_read16(data + 2) & 0x8000) == 0) {
        return 0; /* not a response */
    }
    if ((net_read16(data + 2) & 0x000F) != 0) {
        return 0; /* error */
    }
    uint16_t qd = net_read16(data + 4);
    uint16_t an = net_read16(data + 6);
    size_t p = 12;
    for (int i = 0; i < qd; i++) {
        p = dns_skip_name(data, len, p);
        p += 4;
        if (p > len) {
            return 0;
        }
    }
    for (int i = 0; i < an; i++) {
        p = dns_skip_name(data, len, p);
        if (p + 10 > len) {
            return 0;
        }
        uint16_t type = net_read16(data + p);
        uint16_t rdlen = net_read16(data + p + 8);
        p += 10;
        if (p + rdlen > len) {
            return 0;
        }
        if (type == 1 && rdlen == 4) {
            for (int k = 0; k < 4; k++) {
                out_ip[k] = data[p + k];
            }
            return 1;
        }
        p += rdlen;
    }
    return 0;
}

int net_tcp_parse(const uint8_t *data, size_t len, prism_tcp_hdr *hdr,
                  const uint8_t **payload, size_t *payload_len) {
    if (data == NULL || len < 20 || hdr == NULL) {
        return -1;
    }
    hdr->src_port = net_read16(data + 0);
    hdr->dst_port = net_read16(data + 2);
    hdr->seq = net_read32(data + 4);
    hdr->ack = net_read32(data + 8);
    size_t hlen = (size_t)((data[12] >> 4) & 0x0F) * 4;
    if (hlen < 20 || len < hlen) {
        return -1;
    }
    hdr->flags = data[13];
    hdr->window = net_read16(data + 14);
    *payload = data + hlen;
    *payload_len = len - hlen;
    return 0;
}

void net_tcp_build(uint8_t *out, uint16_t src_port, uint16_t dst_port,
                   uint32_t seq, uint32_t ack, uint8_t flags,
                   uint16_t window, const uint8_t *src_ip,
                   const uint8_t *dst_ip, const uint8_t *payload,
                   size_t payload_len) {
    net_write16(out + 0, src_port);
    net_write16(out + 2, dst_port);
    net_write32(out + 4, seq);
    net_write32(out + 8, ack);
    out[12] = (uint8_t)(5 << 4);
    out[13] = flags;
    net_write16(out + 14, window);
    net_write16(out + 16, 0);
    net_write16(out + 18, 0);
    for (size_t i = 0; i < payload_len; i++) {
        out[20 + i] = payload[i];
    }
    uint16_t c = net_transport_checksum(src_ip, dst_ip, 6, out,
                                        20 + payload_len);
    net_write16(out + 16, c);
}

void net_tcp_build6(uint8_t *out, uint16_t src_port, uint16_t dst_port,
                    uint32_t seq, uint32_t ack, uint8_t flags,
                    uint16_t window, const uint8_t *src_ip6,
                    const uint8_t *dst_ip6, const uint8_t *payload,
                    size_t payload_len) {
    net_write16(out + 0, src_port);
    net_write16(out + 2, dst_port);
    net_write32(out + 4, seq);
    net_write32(out + 8, ack);
    out[12] = (uint8_t)(5 << 4);
    out[13] = flags;
    net_write16(out + 14, window);
    net_write16(out + 16, 0);
    net_write16(out + 18, 0);
    for (size_t i = 0; i < payload_len; i++) {
        out[20 + i] = payload[i];
    }
    uint16_t c = net_ipv6_transport_checksum(src_ip6, dst_ip6, 6, out,
                                             20 + payload_len);
    net_write16(out + 16, c);
}

const uint8_t *net_http_body(const uint8_t *data, size_t len,
                             size_t *body_len, int *status_code) {
    if (body_len != NULL) {
        *body_len = 0;
    }
    if (status_code != NULL) {
        *status_code = 0;
    }
    if (data == NULL || len < 4) {
        return NULL;
    }
    if (status_code != NULL && len >= 12 &&
        data[0] == 'H' && data[1] == 'T' && data[2] == 'T' &&
        data[3] == 'P') {
        int v = 0;
        size_t i = 9;
        while (i < len && data[i] >= '0' && data[i] <= '9') {
            v = v * 10 + (data[i] - '0');
            i++;
        }
        *status_code = v;
    }
    for (size_t i = 0; i + 3 < len; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' &&
            data[i + 3] == '\n') {
            if (body_len != NULL) {
                *body_len = len - (i + 4);
            }
            return data + i + 4;
        }
    }
    return NULL;
}

size_t net_html_strip_tags(char *text, size_t len) {
    size_t w = 0;
    int in_tag = 0;
    int in_script = 0;
    for (size_t r = 0; r < len; r++) {
        char c = text[r];
        if (in_script) {
            if (c == '<') {
                /* Look for </script> (case-insensitive first letter). */
                if (r + 8 < len && text[r + 1] == '/' &&
                    (text[r + 2] == 's' || text[r + 2] == 'S')) {
                    in_script = 0;
                }
            }
            continue;
        }
        if (c == '<') {
            /* <script ...> starts a skipped block. */
            if (r + 6 < len && (text[r + 1] == 's' || text[r + 1] == 'S') &&
                (text[r + 2] == 'c' || text[r + 2] == 'C')) {
                in_script = 1;
                in_tag = 1;
                continue;
            }
            in_tag = 1;
            continue;
        }
        if (c == '>') {
            in_tag = 0;
            /* Tags separate words: keep a space unless at start. */
            if (w > 0 && text[w - 1] != ' ' && text[w - 1] != '\n') {
                text[w++] = ' ';
            }
            continue;
        }
        if (in_tag) {
            continue;
        }
        text[w++] = c;
    }
    text[w] = '\0';
    return w;
}

/* ---- IPv6 ---- */

void net_ipv6_build(uint8_t *out, uint16_t payload_len,
                    uint8_t next_header, uint8_t hop_limit,
                    const uint8_t *src, const uint8_t *dst) {
    out[0] = 0x60;
    out[1] = 0;
    out[2] = 0;
    out[3] = 0;
    net_write16(out + 4, payload_len);
    out[6] = next_header;
    out[7] = hop_limit;
    for (int i = 0; i < 16; i++) {
        out[8 + i] = src[i];
        out[24 + i] = dst[i];
    }
}

int net_ipv6_parse(const uint8_t *data, size_t len,
                   uint8_t *next_header, const uint8_t **payload,
                   size_t *payload_len, uint8_t *src, uint8_t *dst) {
    if (len < 40 || (data[0] >> 4) != 6) {
        return -1;
    }
    uint16_t plen = net_read16(data + 4);
    if ((size_t)plen + 40 > len) {
        return -1;
    }
    *next_header = data[6];
    *payload = data + 40;
    *payload_len = plen;
    for (int i = 0; i < 16; i++) {
        src[i] = data[8 + i];
        dst[i] = data[24 + i];
    }
    return 0;
}

uint16_t net_ipv6_transport_checksum(const uint8_t *src,
                                     const uint8_t *dst,
                                     uint8_t next_header,
                                     const uint8_t *data, size_t len) {
    uint32_t sum = 0;
    for (int i = 0; i < 16; i += 2) {
        sum += (uint32_t)(((uint16_t)src[i] << 8) | src[i + 1]);
        sum += (uint32_t)(((uint16_t)dst[i] << 8) | dst[i + 1]);
    }
    sum += (uint32_t)((len >> 16) & 0xFFFF);
    sum += (uint32_t)(len & 0xFFFF);
    sum += (uint32_t)next_header;
    sum += net_sum(data, len);
    while ((sum >> 16) != 0) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    uint16_t out = (uint16_t)~sum;
    return out == 0 ? (uint16_t)0xFFFF : out;
}

size_t net_icmp6_build(uint8_t *out, const uint8_t *src,
                       const uint8_t *dst, uint8_t type, uint8_t code,
                       const uint8_t *body, size_t body_len) {
    out[0] = type;
    out[1] = code;
    out[2] = 0;
    out[3] = 0;
    for (size_t i = 0; i < body_len; i++) {
        out[4 + i] = body[i];
    }
    uint16_t c = net_ipv6_transport_checksum(src, dst, 58, out,
                                             4 + body_len);
    net_write16(out + 2, c);
    return 4 + body_len;
}

void net_ipv6_link_local(uint8_t *out, const uint8_t *mac) {
    for (int i = 0; i < 16; i++) {
        out[i] = 0;
    }
    out[0] = 0xFE;
    out[1] = 0x80;
    out[8] = (uint8_t)(mac[0] ^ 0x02);
    out[9] = mac[1];
    out[10] = mac[2];
    out[11] = 0xFF;
    out[12] = 0xFE;
    out[13] = mac[3];
    out[14] = mac[4];
    out[15] = mac[5];
}

void net_ipv6_solicited_node(uint8_t *out, const uint8_t *target) {
    for (int i = 0; i < 16; i++) {
        out[i] = 0;
    }
    out[0] = 0xFF;
    out[1] = 0x02;
    out[11] = 0x01;
    out[12] = 0xFF;
    out[13] = target[13];
    out[14] = target[14];
    out[15] = target[15];
}

void net_ipv6_multicast_mac(uint8_t *out, const uint8_t *addr) {
    out[0] = 0x33;
    out[1] = 0x33;
    out[2] = addr[12];
    out[3] = addr[13];
    out[4] = addr[14];
    out[5] = addr[15];
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int net_ipv6_parse_addr(const char *text, uint8_t *out) {
    uint16_t groups[8];
    int n = 0;          /* groups parsed before "::" */
    int dbl = -1;       /* group index where "::" appeared */
    const char *p = text;
    if (p[0] == ':' && p[1] == ':') {
        dbl = 0;
        p += 2;
        if (*p == '\0') {
            for (int i = 0; i < 16; i++) {
                out[i] = 0;
            }
            return 0;
        }
    }
    while (*p != '\0') {
        if (n >= 8) {
            return -1;
        }
        uint32_t v = 0;
        int digits = 0;
        while (hexval(*p) >= 0) {
            v = v * 16 + (uint32_t)hexval(*p);
            p++;
            digits++;
            if (digits > 4) {
                return -1;
            }
        }
        if (digits == 0) {
            return -1;
        }
        groups[n++] = (uint16_t)v;
        if (*p == ':') {
            if (p[1] == ':') {
                if (dbl >= 0) {
                    return -1;
                }
                dbl = n;
                p += 2;
                if (*p == '\0') {
                    break;
                }
                continue;
            }
            p++;
            if (*p == '\0') {
                return -1; /* trailing single ':' */
            }
            continue;
        }
        if (*p != '\0') {
            return -1;
        }
    }
    if (dbl < 0) {
        if (n != 8) {
            return -1;
        }
        for (int i = 0; i < 8; i++) {
            out[2 * i] = (uint8_t)(groups[i] >> 8);
            out[2 * i + 1] = (uint8_t)(groups[i] & 0xFF);
        }
        return 0;
    }
    int zeros = 8 - n;
    if (zeros < 1) {
        return -1;
    }
    int idx = 0;
    for (int i = 0; i < dbl; i++) {
        out[2 * idx] = (uint8_t)(groups[i] >> 8);
        out[2 * idx + 1] = (uint8_t)(groups[i] & 0xFF);
        idx++;
    }
    for (int i = 0; i < zeros; i++) {
        out[2 * idx] = 0;
        out[2 * idx + 1] = 0;
        idx++;
    }
    for (int i = dbl; i < n; i++) {
        out[2 * idx] = (uint8_t)(groups[i] >> 8);
        out[2 * idx + 1] = (uint8_t)(groups[i] & 0xFF);
        idx++;
    }
    return 0;
}

void net_ipv6_format_addr(const uint8_t *addr, char *out) {
    uint16_t g[8];
    for (int i = 0; i < 8; i++) {
        g[i] = (uint16_t)((addr[2 * i] << 8) | addr[2 * i + 1]);
    }
    /* Longest run of zero groups (length >= 2). */
    int best_start = -1, best_len = 0;
    int cur_start = -1, cur_len = 0;
    for (int i = 0; i < 8; i++) {
        if (g[i] == 0) {
            if (cur_start < 0) {
                cur_start = i;
                cur_len = 1;
            } else {
                cur_len++;
            }
            if (cur_len > best_len) {
                best_len = cur_len;
                best_start = cur_start;
            }
        } else {
            cur_start = -1;
            cur_len = 0;
        }
    }
    if (best_len < 2) {
        best_start = -1;
    }
    char *w = out;
    int i = 0;
    int need_colon = 0;
    while (i < 8) {
        if (i == best_start) {
            /* The compressed run is exactly "::" wherever it
             * sits; the following group needs no separator. */
            *w++ = ':';
            *w++ = ':';
            i += best_len;
            need_colon = 0;
            continue;
        }
        if (need_colon) {
            *w++ = ':';
        }
        /* One group in lowercase hex, no leading zeros. */
        uint16_t v = g[i];
        char tmp[4];
        int tn = 0;
        if (v == 0) {
            tmp[tn++] = '0';
        } else {
            char rev[4];
            int rn = 0;
            while (v > 0) {
                rev[rn++] = "0123456789abcdef"[v & 0xF];
                v >>= 4;
            }
            while (rn > 0) {
                tmp[tn++] = rev[--rn];
            }
        }
        for (int k = 0; k < tn; k++) {
            *w++ = tmp[k];
        }
        need_colon = 1;
        i++;
    }
    if (w == out) { /* "::" (all zeros) needs both colons */
        *w++ = ':';
        *w++ = ':';
    }
    *w = '\0';
}

int net_nd_parse_ra(const uint8_t *icmp, size_t len,
                    uint8_t *router_hw, int *has_hw,
                    uint8_t *prefix_out, int *prefix_len, int *a_flag) {
    *has_hw = 0;
    *prefix_len = -1;
    *a_flag = 0;
    if (len < 16 || icmp[0] != 134) {
        return -1;
    }
    size_t off = 16; /* options start after the fixed RA body */
    while (off + 2 <= len) {
        uint8_t type = icmp[off];
        uint8_t olen = icmp[off + 1];
        if (olen == 0) {
            break;
        }
        size_t bytes = (size_t)olen * 8;
        if (off + bytes > len) {
            break;
        }
        if (type == 1 && bytes >= 8) { /* source link-layer */
            for (int i = 0; i < 6; i++) {
                router_hw[i] = icmp[off + 2 + i];
            }
            *has_hw = 1;
        } else if (type == 3 && bytes >= 32) { /* prefix info */
            int plen = icmp[off + 2];
            int flags = icmp[off + 3];
            if (plen <= 128 && *prefix_len < 0) {
                for (int i = 0; i < 16; i++) {
                    prefix_out[i] = icmp[off + 16 + i];
                }
                *prefix_len = plen;
                *a_flag = (flags & 0x40) != 0;
            }
        }
        off += bytes;
    }
    return 0;
}

size_t net_nd_build_ns_body(uint8_t *body, const uint8_t *target,
                            const uint8_t *mac) {
    for (int i = 0; i < 4; i++) {
        body[i] = 0;
    }
    for (int i = 0; i < 16; i++) {
        body[4 + i] = target[i];
    }
    body[20] = 1; /* option: source link-layer, 1 unit */
    body[21] = 1;
    for (int i = 0; i < 6; i++) {
        body[22 + i] = mac[i];
    }
    return 28;
}

size_t net_nd_build_na_body(uint8_t *body, const uint8_t *target,
                            const uint8_t *mac) {
    body[0] = 0x60; /* Solicited + Override */
    body[1] = 0;
    body[2] = 0;
    body[3] = 0;
    for (int i = 0; i < 16; i++) {
        body[4 + i] = target[i];
    }
    body[20] = 2; /* option: target link-layer, 1 unit */
    body[21] = 1;
    for (int i = 0; i < 6; i++) {
        body[22 + i] = mac[i];
    }
    return 28;
}

/* ---- HTTP cookies ---- */

static int cookie_ci_equal(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca + 32);
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb + 32);
        }
        if (ca != cb) {
            return 0;
        }
    }
    return 1;
}

static size_t cookie_len(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

int net_cookie_parse_pair(const char *set_cookie, char *name,
                          size_t name_cap, char *value,
                          size_t value_cap) {
    if (set_cookie == NULL || name == NULL || value == NULL ||
        name_cap < 2 || value_cap < 1) {
        return -1;
    }
    const char *p = set_cookie;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    const char *eq = p;
    while (*eq != '\0' && *eq != '=' && *eq != ';' &&
           *eq != '\r' && *eq != '\n') {
        eq++;
    }
    if (*eq != '=' || eq == p) {
        return -1;
    }
    size_t nl = (size_t)(eq - p);
    while (nl > 0 && (p[nl - 1] == ' ' || p[nl - 1] == '\t')) {
        nl--;
    }
    if (nl == 0 || nl + 1 > name_cap) {
        return -1;
    }
    for (size_t i = 0; i < nl; i++) {
        name[i] = p[i];
    }
    name[nl] = '\0';
    const char *v = eq + 1;
    while (*v == ' ' || *v == '\t') {
        v++;
    }
    size_t vl = 0;
    while (v[vl] != '\0' && v[vl] != ';' && v[vl] != '\r' &&
           v[vl] != '\n') {
        vl++;
    }
    while (vl > 0 && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) {
        vl--;
    }
    if (vl + 1 > value_cap) {
        return -1;
    }
    for (size_t i = 0; i < vl; i++) {
        value[i] = v[i];
    }
    value[vl] = '\0';
    return 0;
}

/* ホスト照合: 完全一致、または jar 側が親ドメイン。 */
static int cookie_host_match(const char *entry, size_t entry_len,
                             const char *host) {
    size_t hl = cookie_len(host);
    if (entry_len == hl &&
        cookie_ci_equal(entry, host, hl)) {
        return 1;
    }
    if (hl > entry_len + 1 && host[hl - entry_len - 1] == '.' &&
        cookie_ci_equal(host + (hl - entry_len), entry,
                        entry_len)) {
        return 1;
    }
    return 0;
}

size_t net_cookie_build_header(const char *jar, const char *host,
                               char *out, size_t cap) {
    if (jar == NULL || host == NULL || out == NULL || cap == 0) {
        return 0;
    }
    size_t pos = 0;
    out[0] = '\0';
    const char *line = jar;
    while (*line != '\0') {
        const char *eol = line;
        while (*eol != '\0' && *eol != '\n') {
            eol++;
        }
        /* line = host TAB name TAB value */
        const char *t1 = line;
        while (t1 < eol && *t1 != '\t') {
            t1++;
        }
        const char *t2 = t1 < eol ? t1 + 1 : eol;
        while (t2 < eol && *t2 != '\t') {
            t2++;
        }
        if (t1 < eol && t2 < eol) {
            size_t hlen = (size_t)(t1 - line);
            if (cookie_host_match(line, hlen, host)) {
                const char *nm = t1 + 1;
                size_t nl = (size_t)(t2 - nm);
                const char *vv = t2 + 1;
                size_t vl = (size_t)(eol - vv);
                size_t need = nl + 1 + vl + (pos > 0 ? 2 : 0);
                if (pos + need + 1 <= cap) {
                    if (pos > 0) {
                        out[pos++] = ';';
                        out[pos++] = ' ';
                    }
                    for (size_t i = 0; i < nl; i++) {
                        out[pos++] = nm[i];
                    }
                    out[pos++] = '=';
                    for (size_t i = 0; i < vl; i++) {
                        out[pos++] = vv[i];
                    }
                    out[pos] = '\0';
                }
            }
        }
        line = *eol == '\n' ? eol + 1 : eol;
    }
    return pos;
}

int net_cookie_jar_update(char *jar, size_t cap, const char *host,
                          const char *name, const char *value) {
    if (jar == NULL || host == NULL || name == NULL ||
        value == NULL || cap == 0) {
        return -1;
    }
    size_t hl = cookie_len(host);
    size_t nl = cookie_len(name);
    size_t vl = cookie_len(value);
    size_t total = cookie_len(jar);
    size_t need = hl + 1 + nl + 1 + vl + 1; /* TAB TAB 改行 */
    /* 既存の同じ host+name 行を探す。 */
    size_t pos = 0;
    size_t found_pos = (size_t)-1;
    size_t found_len = 0;
    while (pos < total) {
        size_t eol = pos;
        while (eol < total && jar[eol] != '\n') {
            eol++;
        }
        size_t line_end = eol < total ? eol + 1 : eol;
        size_t t1 = pos;
        while (t1 < eol && jar[t1] != '\t') {
            t1++;
        }
        size_t t2 = t1 < eol ? t1 + 1 : eol;
        while (t2 < eol && jar[t2] != '\t') {
            t2++;
        }
        if (t1 < eol && t2 < eol &&
            (size_t)(t1 - pos) == hl &&
            cookie_ci_equal(jar + pos, host, hl) &&
            (size_t)(t2 - (t1 + 1)) == nl &&
            cookie_ci_equal(jar + t1 + 1, name, nl)) {
            found_pos = pos;
            found_len = line_end - pos;
            break;
        }
        pos = line_end;
    }
    if (total - found_len + need + 1 > cap) {
        return -1; /* 入らない (既存行は壊さない) */
    }
    if (found_pos != (size_t)-1) {
        /* 既存行を取り除いて末尾に入れ直す。 */
        size_t rest = total - (found_pos + found_len);
        for (size_t i = 0; i < rest; i++) {
            jar[found_pos + i] = jar[found_pos + found_len + i];
        }
        total -= found_len;
        jar[total] = '\0';
    }
    char *w = jar + total;
    for (size_t i = 0; i < hl; i++) {
        *w++ = host[i];
    }
    *w++ = '\t';
    for (size_t i = 0; i < nl; i++) {
        *w++ = name[i];
    }
    *w++ = '\t';
    for (size_t i = 0; i < vl; i++) {
        *w++ = value[i];
    }
    *w++ = '\n';
    *w = '\0';
    return 0;
}
