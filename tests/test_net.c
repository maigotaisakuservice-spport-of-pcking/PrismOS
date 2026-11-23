/* PrismOS - packet helpers host test.
 *
 * Compiles kernel/net.c on the host and checks checksums,
 * Ethernet/ARP/IPv4/UDP, DHCP, DNS, TCP and HTTP parsing
 * with hand-built packets. No hardware, no QEMU.
 */
#include <stdio.h>
#include <string.h>

#include "net.h"

static int failures = 0;
static int checks = 0;

static int mem_eq(const uint8_t *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != (uint8_t)b[i]) return 0;
    }
    return 1;
}

static int contains(const char *hay, const char *needle) {
    size_t hn = strlen(hay);
    size_t nn = strlen(needle);
    if (nn == 0) return 1;
    if (nn > hn) return 0;
    for (size_t i = 0; i + nn <= hn; i++) {
        if (strncmp(hay + i, needle, nn) == 0) return 1;
    }
    return 0;
}

#define CHECK(cond)                                                     \
    do {                                                                \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            failures++;                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                               \
    } while (0)

int main(void) {
    /* ---- byte order ---- */
    uint8_t b[4];
    net_write16(b, 0x1234);
    CHECK(b[0] == 0x12 && b[1] == 0x34);
    CHECK(net_read16(b) == 0x1234);
    net_write32(b, 0x01020304);
    CHECK(b[0] == 1 && b[3] == 4);
    CHECK(net_read32(b) == 0x01020304u);

    /* ---- checksum: known IPv4 header checksum ---- */
    uint8_t ip[20] = {0x45, 0, 0, 0x3c, 0x1c, 0x46, 0x40, 0, 0x40, 6,
                      0, 0, 10, 0, 0, 1, 10, 0, 0, 2};
    CHECK(net_checksum(ip, 20) == 0x0A74);
    uint8_t empty[1] = {0};
    CHECK(net_checksum(empty, 0) == 0xFFFF);

    /* ---- Ethernet ---- */
    uint8_t frame[64];
    uint8_t dst[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t src[6] = {0x52, 0x54, 0, 0x12, 0x34, 0x56};
    net_eth_build(frame, dst, src, 0x0806);
    uint8_t pd[6], ps[6];
    uint16_t et;
    const uint8_t *pl;
    size_t pln;
    CHECK(net_eth_parse(frame, 20, pd, ps, &et, &pl, &pln) == 0);
    CHECK(et == 0x0806 && pln == 6);
    CHECK(ps[0] == 0x52 && ps[5] == 0x56);
    CHECK(net_eth_parse(frame, 10, pd, ps, &et, &pl, &pln) != 0);

    /* ---- ARP ---- */
    uint8_t arp[28];
    uint8_t my_ip[4] = {10, 0, 2, 15};
    uint8_t gw_ip[4] = {10, 0, 2, 2};
    net_arp_build_request(arp, src, my_ip, gw_ip);
    CHECK(net_read16(arp + 6) == 1);
    CHECK(arp[24] == 10 && arp[27] == 2);
    /* Build a reply by hand: opcode 2, sender = gateway. */
    uint8_t reply[28];
    memcpy(reply, arp, 28);
    net_write16(reply + 6, 2);
    uint8_t gw_hw[6] = {0x52, 0x55, 0x0A, 0, 2, 2};
    memcpy(reply + 8, gw_hw, 6);
    memcpy(reply + 14, gw_ip, 4);
    memcpy(reply + 18, src, 6);
    memcpy(reply + 24, my_ip, 4);
    uint8_t out_hw[6];
    CHECK(net_arp_parse_reply(reply, 28, my_ip, out_hw) == 1);
    CHECK(out_hw[1] == 0x55);
    CHECK(net_arp_parse_reply(reply, 28, gw_ip, out_hw) == 0);

    /* ---- IPv4 + UDP ---- */
    uint8_t pkt[64];
    net_ipv4_build(pkt, 28, 17, my_ip, gw_ip, 0x1234);
    CHECK(pkt[0] == 0x45 && pkt[9] == 17);
    CHECK(net_checksum(pkt, 20) == 0);
    uint8_t proto;
    const uint8_t *ippl;
    size_t ipln;
    uint8_t sip[4], dip[4];
    CHECK(net_ipv4_parse(pkt, 28, &proto, &ippl, &ipln, sip, dip) == 0);
    CHECK(proto == 17 && ipln == 8);
    CHECK(sip[3] == 15 && dip[3] == 2);
    uint8_t udp[16];
    const char *hello = "hello";
    net_udp_build(udp, 68, 67, 13, my_ip, gw_ip, (const uint8_t *)hello, 5);
    CHECK(net_read16(udp + 0) == 68 && net_read16(udp + 2) == 67);
    uint16_t sp, dp;
    CHECK(net_udp_parse(udp, 13, &sp, &dp, &pl, &pln) == 0);
    CHECK(sp == 68 && pln == 5 && mem_eq(pl, "hello", 5));

    /* ---- DHCP ---- */
    uint8_t dhcp[300];
    size_t dlen = net_dhcp_build_discover(dhcp, src, 0x11223344);
    CHECK(dlen == 249);
    CHECK(dhcp[0] == 1 && dhcp[236] == 99);
    CHECK(net_read32(dhcp + 4) == 0x11223344u);
    /* Fake an offer: copy the discover, turn it into a reply. */
    uint8_t offer[300];
    memcpy(offer, dhcp, dlen);
    offer[0] = 2;
    offer[16] = 10;
    offer[17] = 0;
    offer[18] = 2;
    offer[19] = 15;
    /* Options: type=offer, subnet, router, DNS, server id, end. */
    size_t p = 240;
    offer[p++] = 53;
    offer[p++] = 1;
    offer[p++] = 2;
    offer[p++] = 1;
    offer[p++] = 4;
    offer[p++] = 255;
    offer[p++] = 255;
    offer[p++] = 255;
    offer[p++] = 0;
    offer[p++] = 3;
    offer[p++] = 4;
    offer[p++] = 10;
    offer[p++] = 0;
    offer[p++] = 2;
    offer[p++] = 2;
    offer[p++] = 6;
    offer[p++] = 4;
    offer[p++] = 10;
    offer[p++] = 0;
    offer[p++] = 2;
    offer[p++] = 3;
    offer[p++] = 54;
    offer[p++] = 4;
    offer[p++] = 10;
    offer[p++] = 0;
    offer[p++] = 2;
    offer[p++] = 2;
    offer[p++] = 255;
    prism_dhcp_result res;
    CHECK(net_dhcp_parse(offer, p, 0x11223344, 0, &res) == 1);
    CHECK(res.yiaddr[3] == 15 && res.subnet[0] == 255);
    CHECK(res.router[3] == 2 && res.dns[3] == 3);
    CHECK(res.server_ip[3] == 2);
    CHECK(net_dhcp_parse(offer, p, 0xDEADBEEFu, 0, &res) == 0);
    size_t rlen = net_dhcp_build_request(dhcp, src, 0x11223344, res.yiaddr,
                                         res.server_ip);
    CHECK(rlen == 261);

    /* ---- DNS ---- */
    uint8_t dns[512];
    size_t qlen = net_dns_build_query(dns, "example.com", 0x1234);
    CHECK(qlen > 12);
    CHECK(net_read16(dns) == 0x1234);
    CHECK(dns[12] == 7); /* "example" length */
    /* Fake a response: header + question echo + one A answer. */
    uint8_t resp[512];
    memcpy(resp, dns, qlen);
    net_write16(resp + 2, 0x8180);
    net_write16(resp + 6, 1);
    size_t rp = qlen;
    resp[rp++] = 0xC0;
    resp[rp++] = 0x0C;
    net_write16(resp + rp, 1);
    rp += 2;
    net_write16(resp + rp, 1);
    rp += 2;
    net_write32(resp + rp, 60);
    rp += 4;
    net_write16(resp + rp, 4);
    rp += 2;
    resp[rp++] = 93;
    resp[rp++] = 184;
    resp[rp++] = 216;
    resp[rp++] = 34;
    uint8_t ans_ip[4];
    CHECK(net_dns_parse(resp, rp, 0x1234, ans_ip) == 1);
    CHECK(ans_ip[0] == 93 && ans_ip[3] == 34);
    CHECK(net_dns_parse(resp, rp, 0x9999, ans_ip) == 0);

    /* ---- TCP ---- */
    uint8_t tcp[64];
    uint8_t a_ip[4] = {10, 0, 2, 15};
    uint8_t b_ip[4] = {93, 184, 216, 34};
    net_tcp_build(tcp, 12345, 80, 1000, 0, PRISM_TCP_SYN, 8192, a_ip,
                  b_ip, NULL, 0);
    CHECK(tcp[13] == PRISM_TCP_SYN);
    prism_tcp_hdr h;
    CHECK(net_tcp_parse(tcp, 20, &h, &pl, &pln) == 0);
    CHECK(h.src_port == 12345 && h.seq == 1000 && pln == 0);
    /* Checksum of a built segment verifies to zero when the
     * checksum field is included in the pseudo-header sum. */
    uint16_t csum = net_transport_checksum(a_ip, b_ip, 6, tcp, 20);
    CHECK(csum == 0xFFFF); /* valid segment sums to all ones */

    /* ---- HTTP ---- */
    const char *http_resp = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    size_t blen = 0;
    int status = 0;
    const uint8_t *body = net_http_body((const uint8_t *)http_resp,
                                        strlen(http_resp), &blen, &status);
    CHECK(body != NULL && blen == 5 && status == 200);
    CHECK(mem_eq(body, "hello", 5));
    CHECK(net_http_body((const uint8_t *)"HTTP/1.1 404", 12, &blen,
                        &status) == NULL);

    /* ---- IPv6 header build/parse ---- */
    {
        uint8_t src[16], dst[16], buf[64];
        CHECK(net_ipv6_parse_addr("2001:db8::1", src) == 0);
        CHECK(net_ipv6_parse_addr("fe80::5054:ff:fe12:3456", dst) == 0);
        net_ipv6_build(buf, 8, 58, 64, src, dst);
        CHECK(buf[0] == 0x60 && buf[6] == 58 && buf[7] == 64);
        uint8_t nh;
        const uint8_t *pl;
        size_t pln;
        uint8_t s2[16], d2[16];
        CHECK(net_ipv6_parse(buf, 48, &nh, &pl, &pln, s2, d2) == 0);
        CHECK(nh == 58 && pln == 8);
        CHECK(memcmp(s2, src, 16) == 0 && memcmp(d2, dst, 16) == 0);
        CHECK(net_ipv6_parse(buf, 20, &nh, &pl, &pln, s2, d2) != 0);
    }
    /* ---- IPv6 address text ---- */
    {
        uint8_t a[16];
        char text[48];
        CHECK(net_ipv6_parse_addr("::", a) == 0);
        net_ipv6_format_addr(a, text);
        CHECK(strcmp(text, "::") == 0);
        CHECK(net_ipv6_parse_addr("::1", a) == 0 && a[15] == 1);
        net_ipv6_format_addr(a, text);
        CHECK(strcmp(text, "::1") == 0);
        CHECK(net_ipv6_parse_addr("fe80::5054:ff:fe12:3456", a) == 0);
        net_ipv6_format_addr(a, text);
        CHECK(strcmp(text, "fe80::5054:ff:fe12:3456") == 0);
        CHECK(net_ipv6_parse_addr("2001:db8:0:0:1:0:0:1", a) == 0);
        net_ipv6_format_addr(a, text);
        CHECK(strcmp(text, "2001:db8::1:0:0:1") == 0);
        CHECK(net_ipv6_parse_addr("1:2:3:4:5:6:7:8", a) == 0);
        net_ipv6_format_addr(a, text);
        CHECK(strcmp(text, "1:2:3:4:5:6:7:8") == 0);
        CHECK(net_ipv6_parse_addr("1::2::3", a) != 0);
        CHECK(net_ipv6_parse_addr("12345::", a) != 0);
        CHECK(net_ipv6_parse_addr("fe80:", a) != 0);
        CHECK(net_ipv6_parse_addr("", a) != 0);
        CHECK(net_ipv6_parse_addr("fec0::2", a) == 0 && a[0] == 0xFE &&
              a[1] == 0xC0 && a[15] == 2);
    }
    /* ---- EUI-64 link-local / solicited-node / multicast MAC ---- */
    {
        uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
        uint8_t ll[16], sn[16], mm[6];
        char text[48];
        net_ipv6_link_local(ll, mac);
        net_ipv6_format_addr(ll, text);
        CHECK(strcmp(text, "fe80::5054:ff:fe12:3456") == 0);
        net_ipv6_solicited_node(sn, ll);
        net_ipv6_format_addr(sn, text);
        CHECK(strcmp(text, "ff02::1:ff12:3456") == 0);
        net_ipv6_multicast_mac(mm, sn);
        CHECK(mm[0] == 0x33 && mm[1] == 0x33 && mm[2] == 0xFF &&
              mm[3] == 0x12 && mm[4] == 0x34 && mm[5] == 0x56);
    }
    /* ---- ICMPv6 / UDP6 checksums (independent vectors) ---- */
    {
        uint8_t src[16], dst[16];
        CHECK(net_ipv6_parse_addr("fe80::1", src) == 0);
        CHECK(net_ipv6_parse_addr("ff02::2", dst) == 0);
        uint8_t body[12] = {0, 0, 0, 0, 1, 1,
                            0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
        uint8_t icmp[32];
        size_t n = net_icmp6_build(icmp, src, dst, 133, 0, body, 12);
        CHECK(n == 16);
        CHECK(icmp[0] == 133 && icmp[2] == 0xF5 && icmp[3] == 0x70);
        uint8_t s2[16], d2[16];
        CHECK(net_ipv6_parse_addr("2001:db8::1", s2) == 0);
        CHECK(net_ipv6_parse_addr("2001:db8::2", d2) == 0);
        uint8_t udp[10] = {0x30, 0x39, 0x00, 0x35, 0x00, 0x0A,
                           0x00, 0x00, 'h', 'i'};
        uint16_t c = net_ipv6_transport_checksum(s2, d2, 17, udp, 10);
        CHECK(c == 0x0B8E);
    }
    /* ---- ND: RA parse / NS / NA build ---- */
    {
        /* RA: fixed 16 bytes + SLLA option + PIO (fec0::/64, A=1) */
        uint8_t ra[64];
        memset(ra, 0, sizeof(ra));
        ra[0] = 134;
        ra[5] = 64; /* router lifetime hi/lo (bytes 4-5) */
        ra[16] = 1; /* SLLA */
        ra[17] = 1;
        ra[18] = 0x52; ra[19] = 0x54; ra[20] = 0x00;
        ra[21] = 0x12; ra[22] = 0x34; ra[23] = 0x56;
        ra[24] = 3; /* PIO */
        ra[25] = 4;
        ra[26] = 64; /* prefix len */
        ra[27] = 0xC0; /* L + A flags */
        ra[40] = 0xFE; ra[41] = 0xC0; /* fec0:: prefix */
        uint8_t hw[6], prefix[16];
        int has_hw = 0, plen = -1, aflag = 0;
        CHECK(net_nd_parse_ra(ra, 56, hw, &has_hw, prefix, &plen,
                              &aflag) == 0);
        CHECK(has_hw && hw[0] == 0x52 && hw[5] == 0x56);
        CHECK(plen == 64 && aflag == 1);
        CHECK(prefix[0] == 0xFE && prefix[1] == 0xC0);
        CHECK(net_nd_parse_ra(ra, 8, hw, &has_hw, prefix, &plen,
                              &aflag) != 0);
        uint8_t body[28], target[16], mac[6] = {0x52, 0x54, 0, 0x12, 0x34, 0x56};
        CHECK(net_ipv6_parse_addr("fe80::5054:ff:fe12:3456", target) == 0);
        CHECK(net_nd_build_ns_body(body, target, mac) == 28);
        CHECK(body[20] == 1 && body[21] == 1 && body[22] == 0x52);
        CHECK(memcmp(body + 4, target, 16) == 0);
        CHECK(net_nd_build_na_body(body, target, mac) == 28);
        CHECK(body[0] == 0x60 && body[20] == 2 && body[21] == 1);
    }

    /* ---- HTTP cookies ---- */
    {
        char nm[64], vl[128];
        CHECK(net_cookie_parse_pair("session=abc123; Path=/; "
                                    "HttpOnly",
                                    nm, sizeof(nm), vl,
                                    sizeof(vl)) == 0);
        CHECK(strcmp(nm, "session") == 0);
        CHECK(strcmp(vl, "abc123") == 0);
        CHECK(net_cookie_parse_pair("theme = dark", nm,
                                    sizeof(nm), vl,
                                    sizeof(vl)) == 0);
        CHECK(strcmp(nm, "theme") == 0);
        CHECK(strcmp(vl, "dark") == 0);
        CHECK(net_cookie_parse_pair("noequals", nm, sizeof(nm),
                                    vl, sizeof(vl)) == -1);
        CHECK(net_cookie_parse_pair("=orphan", nm, sizeof(nm),
                                    vl, sizeof(vl)) == -1);

        char jar[1024];
        jar[0] = '\0';
        char hdr[512];
        CHECK(net_cookie_build_header(jar, "example.com", hdr,
                                      sizeof(hdr)) == 0);
        CHECK(net_cookie_jar_update(jar, sizeof(jar),
                                    "example.com", "session",
                                    "abc123") == 0);
        CHECK(net_cookie_jar_update(jar, sizeof(jar),
                                    "example.com", "theme",
                                    "dark") == 0);
        CHECK(net_cookie_jar_update(jar, sizeof(jar),
                                    "other.org", "x", "1") == 0);
        size_t hn = net_cookie_build_header(jar, "example.com",
                                            hdr, sizeof(hdr));
        CHECK(hn > 0);
        CHECK(contains(hdr, "session=abc123"));
        CHECK(contains(hdr, "theme=dark"));
        CHECK(!contains(hdr, "x=1"));
        /* 親ドメインの Cookie はサブドメインにも送る。 */
        hn = net_cookie_build_header(jar, "www.example.com",
                                     hdr, sizeof(hdr));
        CHECK(hn > 0 && contains(hdr, "session=abc123"));
        /* 無関係なホストには送らない。 */
        hn = net_cookie_build_header(jar, "badexample.com",
                                     hdr, sizeof(hdr));
        CHECK(hn == 0);
        /* 同じ名前は置き換わる (重複しない)。 */
        CHECK(net_cookie_jar_update(jar, sizeof(jar),
                                    "example.com", "session",
                                    "newval") == 0);
        hn = net_cookie_build_header(jar, "example.com", hdr,
                                     sizeof(hdr));
        CHECK(contains(hdr, "session=newval"));
        CHECK(!contains(hdr, "abc123"));
    }

    /* ---- HTML tag stripping ---- */
    char html[64] = "<html><body><h1>Hi</h1><p>there you</p></body></html>";
    size_t hl = net_html_strip_tags(html, strlen(html));
    CHECK(hl > 0);
    CHECK(contains(html, "Hi"));
    CHECK(contains(html, "there"));
    CHECK(!contains(html, "<"));

    if (failures == 0) {
        printf("net tests: %d checks, 0 failures\n", checks);
        printf("PASS\n");
    } else {
        printf("net tests: %d checks, %d failures\n", checks, failures);
        printf("FAIL\n");
    }
    return failures == 0 ? 0 : 1;
}
