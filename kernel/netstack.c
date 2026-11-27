/* PrismOS - network stack (see netstack.h).
 *
 * Packet flow, all polling:
 *   Ethernet -> IPv4 -> UDP (DHCP port 68, DNS port 53)
 *   Ethernet -> ARP (gateway hardware address)
 *   Ethernet -> IPv4 -> TCP (any port, one connection at a
 *   time, simple retransmit on timeout)
 *
 * The virtual network in QEMU answers DHCP, DNS and
 * forwarding; there is no other host on the wire that the
 * stack needs to talk to.
 */
#include "netstack.h"
#include "tls.h"

#include "e1000.h"
#include "virtio_net.h"
#include "net.h"
#include "rtc.h"
#include "security.h"
#include "serial.h"
#include "string.h"
/* NIC の振り分け: まず e1000、無ければ virtio-net。
 * 以後のスタックはこの3つだけを見る。 */
static int nic_virtio;
static const uint8_t *nic_ethaddr(void) {
    return nic_virtio ? virtio_net_ethaddr() : e1000_ethaddr();
}
static int nic_send(const uint8_t *frame, size_t len) {
    return nic_virtio ? virtio_net_send(frame, len)
                      : e1000_send(frame, len);
}
static int nic_recv(uint8_t *buf, size_t max_len) {
    return nic_virtio ? virtio_net_recv(buf, max_len)
                      : e1000_recv(buf, max_len);
}
static int nic_link_up(void) {
    return nic_virtio ? virtio_net_link_up() : e1000_link_up();
}



#define NET_FRAME_MAX 1600
#define NET_BODY_MAX 32768

/* パケット用の作業バッファは静的に置く。理由は shell/diskfs.c と
 * 同じ: 1600 バイトのフレームをスタックに置くと、回復・更新の
 * ように呼び出しが深い場所でスタック (32KB) が溢れる。
 * 送信用の IP パケットと Ethernet フレームは別々のバッファに
 * する (send_ip_frame は IP パケットを読みながらフレームを
 * 作るため)。受信は frame_scratch を順番に使う。シングル
 * スレッドで割り込みから netstack は呼ばれないため安全。 */
static uint8_t frame_scratch[NET_FRAME_MAX];
static uint8_t ip_scratch[NET_FRAME_MAX];
static uint8_t tcp_seg_scratch[1500];

static int net_configured;
static uint64_t net_hhdm_offset;
static int net_hhdm_set;
static uint8_t net_ip[4];
static uint8_t net_subnet[4];
static uint8_t net_router[4];
static uint8_t net_dns[4];
static uint8_t net_gw_hw[6];
static int net_gw_known;
static uint32_t net_xid_counter = 0x12340000u;

static int arp_resolve(const uint8_t *target_ip, uint8_t *out_hw);

/* 固定 IP 設定 (設定アプリ/シェルが登録する)。 */
static int net_static_cfg;
static uint8_t cfg_ip[4];
static uint8_t cfg_mask[4];
static uint8_t cfg_gw[4];
static uint8_t cfg_dns[4];

int net_parse_ipv4(const char *s, uint8_t out[4]) {
    if (s == NULL) {
        return -1;
    }
    for (int part = 0; part < 4; part++) {
        int v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            digits++;
            s++;
            if (v > 255) {
                return -1;
            }
        }
        if (digits == 0) {
            return -1;
        }
        out[part] = (uint8_t)v;
        if (part < 3) {
            if (*s != '.') {
                return -1;
            }
            s++;
        } else if (*s != '\0') {
            return -1;
        }
    }
    return 0;
}

void net_set_static_config(const uint8_t ip[4],
                           const uint8_t mask[4],
                           const uint8_t gw[4],
                           const uint8_t dns[4]) {
    for (int i = 0; i < 4; i++) {
        cfg_ip[i] = ip[i];
        cfg_mask[i] = mask[i];
        cfg_gw[i] = gw[i];
        cfg_dns[i] = dns[i];
    }
    net_static_cfg = 1;
    if (net_configured) {
        for (int i = 0; i < 4; i++) {
            net_ip[i] = cfg_ip[i];
            net_subnet[i] = cfg_mask[i];
            net_router[i] = cfg_gw[i];
            net_dns[i] = cfg_dns[i];
        }
        net_gw_known = 0;
        if (arp_resolve(net_router, net_gw_hw) == 0) {
            net_gw_known = 1;
        }
        serial_write_line("[net] static config applied");
    }
}

void net_clear_static_config(void) {
    net_static_cfg = 0;
    if (net_configured) {
        serial_write_line(
            "[net] back to DHCP (applies at next init)");
    }
}

int net_static_active(void) {
    return net_static_cfg;
}

/* ---- clock ---- */

/* Seconds since boot, from the real time clock. The clock
 * only gives time of day, so wraps at midnight are handled
 * by carrying the day forward when the value jumps back. */
static uint32_t net_now_sec(void) {
    static uint32_t base_day = 0;
    static uint32_t last = 0;
    prism_rtc_time t;
    if (rtc_read(&t) != 0) {
        return last;
    }
    uint32_t tod = (uint32_t)t.hour * 3600u + (uint32_t)t.minute * 60u +
                   (uint32_t)t.second;
    if (tod + 3600u < last) {
        base_day += 86400u;
    }
    last = tod + base_day;
    /* First call: last is time of day, which is fine as a
     * relative base because callers only use differences. */
    return last;
}

/* ---- small helpers ---- */

static void ip_to_str(const uint8_t *ip, char *buf) {
    size_t p = 0;
    for (int i = 0; i < 4; i++) {
        if (i > 0) {
            buf[p++] = '.';
        }
        uint8_t v = ip[i];
        char rev[3];
        int n = 0;
        if (v == 0) {
            buf[p++] = '0';
        } else {
            while (v > 0) {
                rev[n++] = (char)('0' + (v % 10));
                v = (uint8_t)(v / 10);
            }
            while (n > 0) {
                buf[p++] = rev[--n];
            }
        }
    }
    buf[p] = '\0';
}

static void log_ip(const char *label, const uint8_t *ip) {
    char tmp[16];
    ip_to_str(ip, tmp);
    serial_write(label);
    serial_write_line(tmp);
}

/* ---- raw frame send/receive ---- */


/* ---- ファイアウォール (送信/受信の IP 遮断) ----
 * ルールは「この IP 宛・この IP からは落とす」の
 * 一覧だけ (最大 16 件)。送信は send_ip_frame の
 * 一箇所、受信は各待ちループの IP 解析直後で判定する。
 * ルールの読み込みと保存はシェル側 (fw.txt)。 */
#define NET_FW_MAX 16
static uint8_t fw_drop[NET_FW_MAX][4];
static int fw_count;

static int net_fw_blocked(const uint8_t *ip) {
    for (int i = 0; i < fw_count; i++) {
        int same = 1;
        for (int j = 0; j < 4; j++) {
            if (fw_drop[i][j] != ip[j]) {
                same = 0;
            }
        }
        if (same) {
            return 1;
        }
    }
    return 0;
}

void net_fw_clear(void) {
    fw_count = 0;
}

int net_fw_add_drop(const uint8_t *ip) {
    if (net_fw_blocked(ip)) {
        return 0;
    }
    if (fw_count >= NET_FW_MAX) {
        return -1;
    }
    for (int j = 0; j < 4; j++) {
        fw_drop[fw_count][j] = ip[j];
    }
    fw_count++;
    return 0;
}

int net_fw_count(void) {
    return fw_count;
}

int net_fw_get(int idx, uint8_t *ip_out) {
    if (idx < 0 || idx >= fw_count) {
        return -1;
    }
    for (int j = 0; j < 4; j++) {
        ip_out[j] = fw_drop[idx][j];
    }
    return 0;
}

static int send_ip_frame(const uint8_t *dst_hw, const uint8_t *ip_pkt,
                         size_t ip_len) {
    uint8_t *frame = frame_scratch;
    if (ip_len + 14 > NET_FRAME_MAX) {
        return -1;
    }
    if (ip_len >= 20 && net_fw_blocked(ip_pkt + 16)) {
        return -1; /* ファイアウォールで遮断 */
    }
    net_eth_build(frame, dst_hw, nic_ethaddr(), 0x0800);
    for (size_t i = 0; i < ip_len; i++) {
        frame[14 + i] = ip_pkt[i];
    }
    return nic_send(frame, 14 + ip_len);
}

static int send_arp_frame(const uint8_t *dst_hw, const uint8_t *arp_pkt) {
    uint8_t frame[64];
    net_eth_build(frame, dst_hw, nic_ethaddr(), 0x0806);
    for (int i = 0; i < 28; i++) {
        frame[14 + i] = arp_pkt[i];
    }
    return nic_send(frame, 42);
}

/* ---- ARP ---- */

static int arp_resolve(const uint8_t *target_ip, uint8_t *out_hw) {
    uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t zero_ip[4] = {0, 0, 0, 0};
    uint8_t arp[28];
    net_arp_build_request(arp, nic_ethaddr(), net_ip, target_ip);
    (void)zero_ip;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (send_arp_frame(broadcast, arp) != 0) {
            return -1;
        }
        uint32_t start = net_now_sec();
        while (net_now_sec() - start < 2) {
            uint8_t *frame = frame_scratch;
            int n = nic_recv(frame, NET_FRAME_MAX);
            if (n <= 0) {
                continue;
            }
            uint8_t dst[6], src[6];
            uint16_t et;
            const uint8_t *pl;
            size_t pln;
            if (net_eth_parse(frame, (size_t)n, dst, src, &et, &pl,
                              &pln) != 0) {
                continue;
            }
            if (et != 0x0806) {
                continue;
            }
            if (net_arp_parse_reply(pl, pln, net_ip, out_hw)) {
                return 0;
            }
        }
    }
    return -1;
}

/* ---- UDP ---- */

static int send_udp(const uint8_t *dst_hw, const uint8_t *dst_ip,
                    uint16_t src_port, uint16_t dst_port,
                    const uint8_t *payload, size_t payload_len,
                    uint16_t ip_id) {
    uint8_t *ip_pkt = ip_scratch;
    size_t udp_len = 8 + payload_len;
    size_t total = 20 + udp_len;
    if (total > NET_FRAME_MAX) {
        return -1;
    }
    net_ipv4_build(ip_pkt, (uint16_t)total, 17, net_ip, dst_ip, ip_id);
    net_udp_build(ip_pkt + 20, src_port, dst_port, (uint16_t)udp_len,
                  net_ip, dst_ip, payload, payload_len);
    return send_ip_frame(dst_hw, ip_pkt, total);
}

/* Wait for a UDP datagram to dst_port from src_ip, up to
 * timeout_sec seconds. Copies the payload into out and
 * returns its length, or -1 on timeout. Other frames are
 * dropped (ARP replies for us are answered on the way). */
static int wait_udp(uint8_t *out, size_t cap, uint16_t dst_port,
                    const uint8_t *src_ip, uint32_t timeout_sec) {
    uint32_t start = net_now_sec();
    while (net_now_sec() - start < timeout_sec) {
        uint8_t *frame = frame_scratch;
        int n = nic_recv(frame, NET_FRAME_MAX);
        if (n <= 0) {
            continue;
        }
        uint8_t dst[6], src[6];
        uint16_t et;
        const uint8_t *pl;
        size_t pln;
        if (net_eth_parse(frame, (size_t)n, dst, src, &et, &pl, &pln) != 0) {
            continue;
        }
        if (et == 0x0806) {
            /* Answer ARP requests for our address. */
            if (pln >= 28 && net_read16(pl + 6) == 1) {
                int for_us = 1;
                for (int i = 0; i < 4; i++) {
                    if (pl[24 + i] != net_ip[i]) {
                        for_us = 0;
                    }
                }
                if (for_us) {
                    uint8_t rep[28];
                    net_write16(rep + 0, 1);
                    net_write16(rep + 2, 0x0800);
                    rep[4] = 6;
                    rep[5] = 4;
                    net_write16(rep + 6, 2);
                    for (int i = 0; i < 6; i++) {
                        rep[8 + i] = nic_ethaddr()[i];
                    }
                    for (int i = 0; i < 4; i++) {
                        rep[14 + i] = net_ip[i];
                    }
                    for (int i = 0; i < 6; i++) {
                        rep[18 + i] = src[i];
                    }
                    for (int i = 0; i < 4; i++) {
                        rep[24 + i] = pl[14 + i];
                    }
                    (void)send_arp_frame(src, rep);
                }
            }
            continue;
        }
        if (et != 0x0800) {
            continue;
        }
        uint8_t proto;
        const uint8_t *ippl;
        size_t ipln;
        uint8_t sip[4], dip[4];
        if (net_ipv4_parse(pl, pln, &proto, &ippl, &ipln, sip, dip) != 0) {
            continue;
        }
        if (net_fw_blocked(sip)) {
            continue;
        }
        if (proto != 17) {
            continue;
        }
        if (src_ip != NULL) {
            int match = 1;
            for (int i = 0; i < 4; i++) {
                if (sip[i] != src_ip[i]) {
                    match = 0;
                }
            }
            if (!match) {
                continue;
            }
        }
        uint16_t sp, dp;
        const uint8_t *upl;
        size_t upln;
        if (net_udp_parse(ippl, ipln, &sp, &dp, &upl, &upln) != 0) {
            continue;
        }
        if (dp != dst_port) {
            continue;
        }
        size_t copy = upln < cap ? upln : cap;
        for (size_t i = 0; i < copy; i++) {
            out[i] = upl[i];
        }
        return (int)copy;
    }
    return -1;
}

/* One-shot UDP exchange for simple request/response
 * protocols (NTP): send one datagram, wait for one
 * reply from the same server. Off-subnet servers go
 * through the gateway, like DNS. Returns the reply
 * length or -1. */
int net_udp_exchange(const uint8_t *dst_ip, uint16_t dst_port,
                     uint16_t src_port, const uint8_t *payload,
                     size_t payload_len, uint8_t *out,
                     size_t cap, uint32_t timeout_sec) {
    if (net_ensure() != 0) {
        return -1;
    }
    const uint8_t *hw = net_gw_hw;
    uint8_t direct_hw[6];
    int same = 1;
    for (int i = 0; i < 4; i++) {
        if ((net_ip[i] & net_subnet[i]) !=
            (dst_ip[i] & net_subnet[i])) {
            same = 0;
        }
    }
    /* The gateway itself is reached via its own MAC (the
     * same address arp_resolve found at init); a direct
     * ARP for it is equivalent, but going through the
     * known gateway entry also covers user-net setups
     * where the host alias does not answer ARP the same
     * way. */
    {
        int is_router = 1;
        for (int i = 0; i < 4; i++) {
            if (dst_ip[i] != net_router[i]) {
                is_router = 0;
            }
        }
        if (is_router) {
            same = 0;
        }
    }
    if (same) {
        if (arp_resolve(dst_ip, direct_hw) != 0) {
            serial_write_line("[net] udp: ARP failed");
            return -1;
        }
        hw = direct_hw;
    }
    static uint16_t udp_id = 0x7500;
    udp_id++;
    if (send_udp(hw, dst_ip, src_port, dst_port, payload,
                 payload_len, udp_id) != 0) {
        serial_write_line("[net] udp: send failed");
        return -1;
    }
    int n = wait_udp(out, cap, src_port, dst_ip, timeout_sec);
    if (n < 0) {
        serial_write_line("[net] udp: no reply (timeout)");
    }
    return n;
}

/* ---- ICMP ping ---- */

static int send_icmp(const uint8_t *dst_hw, const uint8_t *dst_ip,
                     uint16_t id, uint16_t seq) {
    uint8_t *ip_pkt = ip_scratch;
    uint8_t icmp[16];
    icmp[0] = 8; /* echo request */
    icmp[1] = 0;
    icmp[2] = 0;
    icmp[3] = 0;
    net_write16(icmp + 4, id);
    net_write16(icmp + 6, seq);
    const char *pl = "prismos!";
    for (int i = 0; i < 8; i++) {
        icmp[8 + i] = (uint8_t)pl[i];
    }
    uint16_t csum = net_checksum(icmp, sizeof(icmp));
    net_write16(icmp + 2, csum);
    size_t total = 20 + sizeof(icmp);
    static uint16_t icmp_ip_id = 0x6900;
    icmp_ip_id++;
    net_ipv4_build(ip_pkt, (uint16_t)total, 1, net_ip, dst_ip,
                   icmp_ip_id);
    for (size_t i = 0; i < sizeof(icmp); i++) {
        ip_pkt[20 + i] = icmp[i];
    }
    return send_ip_frame(dst_hw, ip_pkt, total);
}

/* Echo reply を待つ。wait_udp と同じ作法 (ARP 応答も
 * さばく)。戻り値: 0 = 応答あり、-1 = タイムアウト。 */
static int wait_icmp(const uint8_t *src_ip, uint16_t id,
                     uint16_t seq, uint32_t timeout_sec) {
    uint32_t start = net_now_sec();
    while (net_now_sec() - start < timeout_sec) {
        uint8_t *frame = frame_scratch;
        int n = nic_recv(frame, NET_FRAME_MAX);
        if (n <= 0) {
            continue;
        }
        uint8_t dst[6], src[6];
        uint16_t et;
        const uint8_t *pl;
        size_t pln;
        if (net_eth_parse(frame, (size_t)n, dst, src, &et, &pl,
                          &pln) != 0) {
            continue;
        }
        if (et == 0x0806) {
            if (pln >= 28 && net_read16(pl + 6) == 1) {
                int for_us = 1;
                for (int i = 0; i < 4; i++) {
                    if (pl[24 + i] != net_ip[i]) {
                        for_us = 0;
                    }
                }
                if (for_us) {
                    uint8_t rep[28];
                    net_write16(rep + 0, 1);
                    net_write16(rep + 2, 0x0800);
                    rep[4] = 6;
                    rep[5] = 4;
                    net_write16(rep + 6, 2);
                    for (int i = 0; i < 6; i++) {
                        rep[8 + i] = nic_ethaddr()[i];
                    }
                    for (int i = 0; i < 4; i++) {
                        rep[14 + i] = net_ip[i];
                    }
                    for (int i = 0; i < 6; i++) {
                        rep[18 + i] = src[i];
                    }
                    for (int i = 0; i < 4; i++) {
                        rep[24 + i] = pl[14 + i];
                    }
                    (void)send_arp_frame(src, rep);
                }
            }
            continue;
        }
        if (et != 0x0800) {
            continue;
        }
        uint8_t proto;
        const uint8_t *ippl;
        size_t ipln;
        uint8_t sip[4], dip[4];
        if (net_ipv4_parse(pl, pln, &proto, &ippl, &ipln, sip,
                           dip) != 0) {
            continue;
        }
        if (net_fw_blocked(sip)) {
            continue;
        }
        if (proto != 1 || ipln < 8) {
            continue;
        }
        int from = 1;
        for (int i = 0; i < 4; i++) {
            if (sip[i] != src_ip[i]) {
                from = 0;
            }
        }
        if (!from) {
            continue;
        }
        if (ippl[0] != 0) { /* echo reply 以外は無視 */
            continue;
        }
        if (net_read16(ippl + 4) != id ||
            net_read16(ippl + 6) != seq) {
            continue;
        }
        return 0;
    }
    return -1;
}

int net_ping(const uint8_t *dst_ip, uint16_t seq,
             uint32_t timeout_sec) {
    if (net_ensure() != 0) {
        return -1;
    }
    if (net_fw_blocked(dst_ip)) {
        return -2;
    }
    const uint8_t *hw = net_gw_hw;
    uint8_t direct_hw[6];
    int same = 1;
    for (int i = 0; i < 4; i++) {
        if ((net_ip[i] & net_subnet[i]) !=
            (dst_ip[i] & net_subnet[i])) {
            same = 0;
        }
    }
    {
        int is_router = 1;
        for (int i = 0; i < 4; i++) {
            if (dst_ip[i] != net_router[i]) {
                is_router = 0;
            }
        }
        if (is_router) {
            same = 0;
        }
    }
    if (same) {
        if (arp_resolve(dst_ip, direct_hw) != 0) {
            return -1;
        }
        hw = direct_hw;
    }
    const uint16_t id = 0x5052;
    if (send_icmp(hw, dst_ip, id, seq) != 0) {
        return -1;
    }
    return wait_icmp(dst_ip, id, seq, timeout_sec);
}

/* ---- DHCP ---- */

static int dhcp_configure(void) {
    uint8_t broadcast_ip[4] = {255, 255, 255, 255};
    uint8_t broadcast_hw[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t zero_ip[4] = {0, 0, 0, 0};
    uint8_t dhcp[300];
    uint8_t reply[1024];
    for (int i = 0; i < 4; i++) {
        net_ip[i] = 0;
    }
    uint32_t xid = net_xid_counter + 0x1234u;
    net_xid_counter += 0x100u;
    size_t dlen = net_dhcp_build_discover(dhcp, nic_ethaddr(), xid);
    /* DHCP discover is a UDP broadcast from 0.0.0.0:68 to
     * 255.255.255.255:67. Build it by hand because our
     * address is still 0. */
    uint8_t *ip_pkt = ip_scratch;
    net_ipv4_build(ip_pkt, (uint16_t)(20 + 8 + dlen), 17, zero_ip,
                   broadcast_ip, 0x1234);
    net_udp_build(ip_pkt + 20, 68, 67, (uint16_t)(8 + dlen), zero_ip,
                  broadcast_ip, dhcp, dlen);
    for (int attempt = 0; attempt < 3; attempt++) {
        if (send_ip_frame(broadcast_hw, ip_pkt, 20 + 8 + dlen) != 0) {
            serial_write_line("[net] discover send failed");
            return -1;
        }
        serial_write_line("[net] discover sent, waiting for offer");
        int n = wait_udp(reply, sizeof(reply), 68, NULL, 4);
        if (n < 0) {
            serial_write_line("[net] no offer in 4s");
            continue;
        }
        serial_write_line("[net] offer received");
        prism_dhcp_result res;
        /* wait_udp returns the UDP payload, which is the
         * DHCP message itself. */
        if (!net_dhcp_parse(reply, (size_t)n, xid, 0, &res)) {
            continue;
        }
        for (int i = 0; i < 4; i++) {
            net_ip[i] = res.yiaddr[i];
            net_subnet[i] = res.subnet[i];
            net_router[i] = res.router[i];
            net_dns[i] = res.dns[i];
        }
        if (net_dns[0] == 0 && net_dns[1] == 0 && net_dns[2] == 0 &&
            net_dns[3] == 0) {
            for (int i = 0; i < 4; i++) {
                net_dns[i] = net_router[i];
            }
        }
        /* Request the offered address. */
        uint32_t xid2 = xid + 1;
        size_t rlen = net_dhcp_build_request(dhcp, nic_ethaddr(), xid2,
                                             res.yiaddr, res.server_ip);
        net_ipv4_build(ip_pkt, (uint16_t)(20 + 8 + rlen), 17, zero_ip,
                       broadcast_ip, 0x1235);
        net_udp_build(ip_pkt + 20, 68, 67, (uint16_t)(8 + rlen), zero_ip,
                      broadcast_ip, dhcp, rlen);
        for (int a2 = 0; a2 < 3; a2++) {
            if (send_ip_frame(broadcast_hw, ip_pkt, 20 + 8 + rlen) != 0) {
                return -1;
            }
            n = wait_udp(reply, sizeof(reply), 68, NULL, 4);
            if (n < 0) {
                continue;
            }
            prism_dhcp_result ack;
            if (net_dhcp_parse(reply, (size_t)n, xid2, 1, &ack)) {
                for (int i = 0; i < 4; i++) {
                    net_ip[i] = ack.yiaddr[i];
                    net_subnet[i] = ack.subnet[i];
                    net_router[i] = ack.router[i];
                    net_dns[i] = ack.dns[i];
                }
                if (net_dns[0] == 0 && net_dns[1] == 0 &&
                    net_dns[2] == 0 && net_dns[3] == 0) {
                    for (int i = 0; i < 4; i++) {
                        net_dns[i] = net_router[i];
                    }
                }
                return 0;
            }
        }
        return -1;
    }
    return -1;
}

void net_set_hhdm(uint64_t hhdm_offset) {
    net_hhdm_offset = hhdm_offset;
    net_hhdm_set = 1;
}

int net_ensure(void) {
    if (net_configured) {
        return 0;
    }
    if (!net_hhdm_set) {
        return -1;
    }
    return net_init(net_hhdm_offset);
}

/* NIC の検出とリンク確認だけを行う (IPv4 のアドレス設定は
 * しない)。IPv6 リンクローカルは MAC アドレスさえあれば
 * 使えるので、DHCP の無い v6 だけのネットワークでも
 * 動くように、アドレス設定とは分けてある。 */
static int net_nic_ready_flag;

static int net_nic_init(uint64_t hhdm_offset) {
    if (net_nic_ready_flag) {
        return 0;
    }
    if (e1000_init(hhdm_offset) == 0) {
        nic_virtio = 0;
    } else if (virtio_net_init(hhdm_offset) == 1) {
        nic_virtio = 1;
    } else {
        serial_write_line("[net] no card found");
        return -1;
    }
    serial_write_line("[net] card found, waiting for link");
    uint32_t start = net_now_sec();
    while (!nic_link_up()) {
        if (net_now_sec() - start >= 5) {
            break;
        }
    }
    serial_write("[net] link: ");
    serial_write_line(nic_link_up() ? "up" : "down (continuing)");
    net_nic_ready_flag = 1;
    return 0;
}

int net_ensure_nic(void) {
    if (net_nic_ready_flag) {
        return 0;
    }
    if (!net_hhdm_set) {
        return -1;
    }
    return net_nic_init(net_hhdm_offset);
}

int net_init(uint64_t hhdm_offset) {
    if (net_configured) {
        return 0;
    }
    if (net_nic_init(hhdm_offset) != 0) {
        return -1;
    }
    if (net_static_cfg) {
        for (int i = 0; i < 4; i++) {
            net_ip[i] = cfg_ip[i];
            net_subnet[i] = cfg_mask[i];
            net_router[i] = cfg_gw[i];
            net_dns[i] = cfg_dns[i];
        }
        serial_write_line("[net] using static config");
    } else if (dhcp_configure() != 0) {
        serial_write_line("[net] DHCP failed");
        return -1;
    }
    log_ip("[net] ip: ", net_ip);
    log_ip("[net] router: ", net_router);
    log_ip("[net] dns: ", net_dns);
    if (arp_resolve(net_router, net_gw_hw) != 0) {
        serial_write_line("[net] gateway ARP failed");
        return -1;
    }
    net_gw_known = 1;
    net_configured = 1;
    serial_write_line("[net] ready");
    return 0;
}

int net_ready(void) {
    return net_configured;
}

/* ---- IPv6 ----
 * 基礎実装: リンクローカル (EUI-64) の自動構成、ルータ
 * 要請/広告による SLAAC (プレフィックス /64 前提)、近隣
 * 探索 (NS/NA)、ICMPv6 エコー (ping6 と応答)、UDP 交換。
 * 拡張ヘッダと断片化は扱わない。重複アドレス検出 (DAD)
 * は行わない (単一ホストのテスト網前提)。
 * 受信はポーリングで、待ちループが裏側の仕事 (NA の記録、
 * NS への応答、RA の取り込み、エコー応答) をさばく。 */

static uint8_t net6_ll[16];
static int net6_ll_ready;
static uint8_t net6_global[16];
static int net6_have_global;
static uint8_t net6_prefix[16];
static int net6_prefix_len;
static uint8_t net6_router[16];
static uint8_t net6_router_hw[6];
static int net6_have_router;

#define NET6_NEIGH_MAX 8
static uint8_t net6_neigh_ip[NET6_NEIGH_MAX][16];
static uint8_t net6_neigh_hw[NET6_NEIGH_MAX][6];
static int net6_neigh_n;

static uint8_t ip6_scratch[NET_FRAME_MAX];
static uint8_t frame6_scratch[NET_FRAME_MAX];

/* エコー応答待ちの照合条件と結果。 */
static int net6_er_armed;
static int net6_er_got;
static uint8_t net6_er_from[16];
static uint16_t net6_er_id;
static uint16_t net6_er_seq;

/* UDP 待ちの条件と結果。 */
static uint16_t net6_udp_port;
static uint8_t *net6_udp_out;
static size_t net6_udp_cap;
static int net6_udp_got;

static int addr6_eq(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 16; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static int addr6_is_linklocal(const uint8_t *a) {
    return a[0] == 0xFE && (a[1] & 0xC0) == 0x80;
}

static int addr6_is_multicast(const uint8_t *a) { return a[0] == 0xFF; }

static int addr6_is_ours(const uint8_t *a) {
    if (net6_ll_ready && addr6_eq(a, net6_ll)) {
        return 1;
    }
    if (net6_have_global && addr6_eq(a, net6_global)) {
        return 1;
    }
    return 0;
}

static void net6_neigh_add(const uint8_t *ip, const uint8_t *hw) {
    for (int i = 0; i < net6_neigh_n; i++) {
        if (addr6_eq(net6_neigh_ip[i], ip)) {
            for (int k = 0; k < 6; k++) {
                net6_neigh_hw[i][k] = hw[k];
            }
            return;
        }
    }
    int i = net6_neigh_n < NET6_NEIGH_MAX ? net6_neigh_n++
                                          : NET6_NEIGH_MAX - 1;
    for (int k = 0; k < 16; k++) {
        net6_neigh_ip[i][k] = ip[k];
    }
    for (int k = 0; k < 6; k++) {
        net6_neigh_hw[i][k] = hw[k];
    }
}

static int net6_neigh_find(const uint8_t *ip, uint8_t *hw_out) {
    for (int i = 0; i < net6_neigh_n; i++) {
        if (addr6_eq(net6_neigh_ip[i], ip)) {
            for (int k = 0; k < 6; k++) {
                hw_out[k] = net6_neigh_hw[i][k];
            }
            return 0;
        }
    }
    return -1;
}

/* IPv6 パケットを 1 フレーム送る。 */
static int send6_packet(const uint8_t *dst_hw, const uint8_t *src,
                        const uint8_t *dst, uint8_t next,
                        uint8_t hop_limit, const uint8_t *payload,
                        size_t plen) {
    if (40 + plen + 14 > NET_FRAME_MAX) {
        return -1;
    }
    net_ipv6_build(ip6_scratch, (uint16_t)plen, next, hop_limit, src,
                   dst);
    for (size_t i = 0; i < plen; i++) {
        ip6_scratch[40 + i] = payload[i];
    }
    net_eth_build(frame6_scratch, dst_hw, nic_ethaddr(), 0x86DD);
    for (size_t i = 0; i < 40 + plen; i++) {
        frame6_scratch[14 + i] = ip6_scratch[i];
    }
    return nic_send(frame6_scratch, 14 + 40 + plen);
}

/* RA を取り込む: ルータとプレフィックスを記録し、
 * 自律フラグがあれば SLAAC でグローバルを作る。 */
static void net6_handle_ra(const uint8_t *src_ip,
                           const uint8_t *eth_src,
                           const uint8_t *icmp, size_t icmp_len) {
    uint8_t hw[6];
    int has_hw = 0;
    uint8_t prefix[16];
    int plen = -1;
    int a_flag = 0;
    if (net_nd_parse_ra(icmp, icmp_len, hw, &has_hw, prefix, &plen,
                        &a_flag) != 0) {
        return;
    }
    for (int i = 0; i < 16; i++) {
        net6_router[i] = src_ip[i];
    }
    const uint8_t *rhw = has_hw ? hw : eth_src;
    for (int i = 0; i < 6; i++) {
        net6_router_hw[i] = rhw[i];
    }
    net6_have_router = 1;
    net6_neigh_add(src_ip, rhw);
    if (a_flag && plen > 0 && plen <= 64) {
        for (int i = 0; i < 16; i++) {
            net6_prefix[i] = prefix[i];
            net6_global[i] = 0;
        }
        net6_prefix_len = plen;
        /* プレフィックス部分 (バイト単位 + 端数ビット)。 */
        int full = plen / 8;
        int rem = plen % 8;
        for (int i = 0; i < full; i++) {
            net6_global[i] = prefix[i];
        }
        if (rem) {
            uint8_t mask = (uint8_t)(0xFF << (8 - rem));
            net6_global[full] = prefix[full] & mask;
        }
        /* インタフェース識別子はリンクローカルの下位を使う。 */
        for (int i = 8; i < 16; i++) {
            net6_global[i] = net6_ll[i];
        }
        net6_have_global = 1;
    }
}

/* NS に NA で応える。 */
static void net6_answer_ns(const uint8_t *ns_src, const uint8_t *target,
                           const uint8_t *eth_src) {
    uint8_t body[28];
    size_t blen = net_nd_build_na_body(body, target, nic_ethaddr());
    uint8_t icmp[64];
    size_t ilen = net_icmp6_build(icmp, target, ns_src, 136, 0, body,
                                  blen);
    const uint8_t *dst = ns_src;
    uint8_t mcast[16];
    uint8_t mhw[6];
    const uint8_t *hw = eth_src;
    int unspecified = 1;
    for (int i = 0; i < 16; i++) {
        if (ns_src[i] != 0) {
            unspecified = 0;
        }
    }
    if (unspecified) { /* DAD: 全ノードマルチキャストへ */
        for (int i = 0; i < 16; i++) {
            mcast[i] = 0;
        }
        mcast[0] = 0xFF;
        mcast[1] = 0x02;
        mcast[15] = 1; /* ff02::1 */
        dst = mcast;
        net_ipv6_multicast_mac(mhw, mcast);
        hw = mhw;
    }
    (void)send6_packet(hw, target, dst, 58, 255, icmp, ilen);
}

/* 受信フレームを 1 個さばく (IPv6 の裏側の仕事)。
 * 待ち側の条件 (net6_er_* / net6_udp_*) もここで見る。
 * TCP の待ちループからも、TCP でないフレームの裏側処理
 * (RA/NS/NA/エコー応答) をするために呼ばれる。 */
static void net6_handle_frame(const uint8_t *frame, int n) {
    uint8_t dst[6], src[6];
    uint16_t et;
    const uint8_t *pl;
    size_t pln;
    if (net_eth_parse(frame, (size_t)n, dst, src, &et, &pl, &pln) !=
        0) {
        return;
    }
    if (et == 0x0806) { /* ARP 要求には v4 と同じく応える */
        if (pln >= 28 && net_read16(pl + 6) == 1 && net_configured) {
            int for_us = 1;
            for (int i = 0; i < 4; i++) {
                if (pl[24 + i] != net_ip[i]) {
                    for_us = 0;
                }
            }
            if (for_us) {
                uint8_t rep[28];
                net_write16(rep + 0, 1);
                net_write16(rep + 2, 0x0800);
                rep[4] = 6;
                rep[5] = 4;
                net_write16(rep + 6, 2);
                for (int i = 0; i < 6; i++) {
                    rep[8 + i] = nic_ethaddr()[i];
                }
                for (int i = 0; i < 4; i++) {
                    rep[14 + i] = net_ip[i];
                }
                for (int i = 0; i < 6; i++) {
                    rep[18 + i] = src[i];
                }
                for (int i = 0; i < 4; i++) {
                    rep[24 + i] = pl[14 + i];
                }
                (void)send_arp_frame(src, rep);
            }
        }
        return;
    }
    if (et != 0x86DD) {
        return;
    }
    uint8_t next;
    const uint8_t *ipl;
    size_t ipln;
    uint8_t sip[16], dip[16];
    if (net_ipv6_parse(pl, pln, &next, &ipl, &ipln, sip, dip) != 0) {
        return;
    }
    if (next == 17 && net6_udp_port != 0) { /* UDP */
        if (ipln >= 8 && net_read16(ipl + 2) == net6_udp_port &&
            addr6_is_ours(dip)) {
            size_t body = ipln - 8;
            if (body > net6_udp_cap) {
                body = net6_udp_cap;
            }
            for (size_t i = 0; i < body; i++) {
                net6_udp_out[i] = ipl[8 + i];
            }
            net6_udp_got = (int)body;
        }
        return;
    }
    if (next != 58 || ipln < 4) {
        return;
    }
    uint8_t type = ipl[0];
    if (type == 134) { /* ルータ広告 */
        net6_handle_ra(sip, src, ipl, ipln);
    } else if (type == 135) { /* 近隣要請 */
        if (ipln >= 24) {
            const uint8_t *target = ipl + 8;
            if (addr6_is_ours(target)) {
                net6_answer_ns(sip, target, src);
            }
        }
    } else if (type == 136) { /* 近隣広告 */
        if (ipln >= 24) {
            const uint8_t *target = ipl + 8;
            uint8_t hw[6];
            int have = 0;
            if (ipln >= 32 && ipl[24] == 2 && ipl[25] == 1) {
                for (int i = 0; i < 6; i++) {
                    hw[i] = ipl[26 + i];
                }
                have = 1;
            }
            if (!have) {
                for (int i = 0; i < 6; i++) {
                    hw[i] = src[i];
                }
            }
            net6_neigh_add(target, hw);
        }
    } else if (type == 128) { /* エコー要求には応える */
        if (addr6_is_ours(dip) && ipln >= 8) {
            uint8_t icmp[64];
            size_t blen = ipln - 4;
            if (blen > 60) {
                blen = 60;
            }
            size_t ilen = net_icmp6_build(icmp, dip, sip, 129, 0,
                                          ipl + 4, blen);
            (void)send6_packet(src, dip, sip, 58, 64, icmp, ilen);
        }
    } else if (type == 129) { /* エコー応答 */
        if (net6_er_armed && ipln >= 8 &&
            addr6_eq(sip, net6_er_from) &&
            net_read16(ipl + 4) == net6_er_id &&
            net_read16(ipl + 6) == net6_er_seq) {
            net6_er_got = 1;
        }
    }
}

static void net6_pump_once(void) {
    uint8_t *frame = frame_scratch;
    int n = nic_recv(frame, NET_FRAME_MAX);
    if (n <= 0) {
        return;
    }
    net6_handle_frame(frame, n);
}

/* リンクローカルを用意する (NIC 初期化が前提)。
 * IPv4 の設定 (DHCP) は要らない: リンクローカルは
 * MAC から作るだけなので、v6 だけのネットワークでも
 * 使える。 */
static int net6_ensure_ll(void) {
    if (net6_ll_ready) {
        return 0;
    }
    if (net_ensure_nic() != 0) {
        return -1;
    }
    net_ipv6_link_local(net6_ll, nic_ethaddr());
    net6_ll_ready = 1;
    return 0;
}

int net6_configure(void) {
    if (net6_ensure_ll() != 0) {
        return -1;
    }
    if (net6_have_router) {
        return 0;
    }
    /* ルータ要請を全ルータマルチキャストへ。 */
    uint8_t body[12];
    for (int i = 0; i < 4; i++) {
        body[i] = 0;
    }
    body[4] = 1; /* 送信元リンク層アドレス選択肢 */
    body[5] = 1;
    for (int i = 0; i < 6; i++) {
        body[6 + i] = nic_ethaddr()[i];
    }
    uint8_t icmp[32];
    uint8_t all_routers[16] = {0xFF, 0x02, 0, 0, 0, 0, 0, 0,
                               0,    0,    0, 0, 0, 0, 0, 2};
    size_t ilen = net_icmp6_build(icmp, net6_ll, all_routers, 133, 0,
                                  body, sizeof(body));
    uint8_t hw[6];
    net_ipv6_multicast_mac(hw, all_routers);
    for (int attempt = 0; attempt < 3 && !net6_have_router;
         attempt++) {
        (void)send6_packet(hw, net6_ll, all_routers, 58, 255, icmp,
                           ilen);
        uint32_t start = net_now_sec();
        while (net_now_sec() - start < 2 && !net6_have_router) {
            net6_pump_once();
        }
    }
    return net6_have_router ? 0 : -1;
}

int net6_ready(void) { return net6_ll_ready; }

int net6_have_global_addr(void) { return net6_have_global; }

void net6_my_ll(uint8_t *out) {
    for (int i = 0; i < 16; i++) {
        out[i] = net6_ll[i];
    }
}

void net6_my_global(uint8_t *out) {
    for (int i = 0; i < 16; i++) {
        out[i] = net6_global[i];
    }
}

/* 次ホップを決めて近隣解決する。成功したら hw へ書く。 */
static int net6_next_hop(const uint8_t *dst, const uint8_t **src_out,
                         uint8_t *hw_out) {
    const uint8_t *target = dst;
    const uint8_t *src = net6_ll;
    int direct = 0;
    if (addr6_is_multicast(dst)) {
        net_ipv6_multicast_mac(hw_out, dst);
        *src_out = net6_have_global ? net6_global : net6_ll;
        return 0;
    }
    if (addr6_is_linklocal(dst)) {
        direct = 1;
    } else if (net6_have_global) {
        src = net6_global;
        /* プレフィックス内なら直接届く。 */
        int same = 1;
        int full = net6_prefix_len / 8;
        for (int i = 0; i < full; i++) {
            if (dst[i] != net6_prefix[i]) {
                same = 0;
            }
        }
        if (same) {
            direct = 1;
        }
    }
    if (!direct) {
        if (!net6_have_router) {
            return -1;
        }
        target = net6_router;
        src = net6_have_global ? net6_global : net6_ll;
    }
    *src_out = src;
    if (net6_neigh_find(target, hw_out) == 0) {
        return 0;
    }
    /* 近隣要請を出す。 */
    uint8_t sn[16];
    net_ipv6_solicited_node(sn, target);
    uint8_t mhw[6];
    net_ipv6_multicast_mac(mhw, sn);
    uint8_t body[28];
    size_t blen = net_nd_build_ns_body(body, target, nic_ethaddr());
    uint8_t icmp[64];
    size_t ilen = net_icmp6_build(icmp, src, sn, 135, 0, body, blen);
    for (int attempt = 0; attempt < 3; attempt++) {
        (void)send6_packet(mhw, src, sn, 58, 255, icmp, ilen);
        uint32_t start = net_now_sec();
        while (net_now_sec() - start < 2) {
            net6_pump_once();
            if (net6_neigh_find(target, hw_out) == 0) {
                return 0;
            }
        }
    }
    return -1;
}

int net6_ping6(const uint8_t *dst, uint16_t seq,
               uint32_t timeout_sec) {
    if (net6_configure() != 0 && !net6_ll_ready) {
        return -1;
    }
    const uint8_t *src = NULL;
    uint8_t hw[6];
    if (net6_next_hop(dst, &src, hw) != 0) {
        return -1;
    }
    uint8_t body[12];
    const uint16_t id = 0x5052;
    net_write16(body + 0, id);
    net_write16(body + 2, seq);
    const char *pl = "prismos!";
    for (int i = 0; i < 8; i++) {
        body[4 + i] = (uint8_t)pl[i];
    }
    uint8_t icmp[32];
    size_t ilen = net_icmp6_build(icmp, src, dst, 128, 0, body,
                                  sizeof(body));
    net6_er_armed = 1;
    net6_er_got = 0;
    for (int i = 0; i < 16; i++) {
        net6_er_from[i] = dst[i];
    }
    net6_er_id = id;
    net6_er_seq = seq;
    if (send6_packet(hw, src, dst, 58, 64, icmp, ilen) != 0) {
        net6_er_armed = 0;
        return -1;
    }
    uint32_t start = net_now_sec();
    while (net_now_sec() - start < timeout_sec) {
        net6_pump_once();
        if (net6_er_got) {
            net6_er_armed = 0;
            return 0;
        }
    }
    net6_er_armed = 0;
    return -1;
}

int net6_udp_exchange(const uint8_t *dst, uint16_t dst_port,
                      uint16_t src_port, const uint8_t *payload,
                      size_t payload_len, uint8_t *out, size_t cap,
                      uint32_t timeout_sec) {
    if (net6_configure() != 0 && !net6_ll_ready) {
        return -1;
    }
    const uint8_t *src = NULL;
    uint8_t hw[6];
    if (net6_next_hop(dst, &src, hw) != 0) {
        return -1;
    }
    size_t ulen = 8 + payload_len;
    if (40 + ulen > NET_FRAME_MAX) {
        return -1;
    }
    uint8_t *udp = ip6_scratch + 40;
    net_write16(udp + 0, src_port);
    net_write16(udp + 2, dst_port);
    net_write16(udp + 4, (uint16_t)ulen);
    net_write16(udp + 6, 0);
    for (size_t i = 0; i < payload_len; i++) {
        udp[8 + i] = payload[i];
    }
    uint16_t c = net_ipv6_transport_checksum(src, dst, 17, udp, ulen);
    net_write16(udp + 6, c);
    net6_udp_port = src_port;
    net6_udp_out = out;
    net6_udp_cap = cap;
    net6_udp_got = 0;
    net_ipv6_build(ip6_scratch, (uint16_t)ulen, 17, 64, src, dst);
    net_eth_build(frame6_scratch, hw, nic_ethaddr(), 0x86DD);
    for (size_t i = 0; i < 40 + ulen; i++) {
        frame6_scratch[14 + i] = ip6_scratch[i];
    }
    if (nic_send(frame6_scratch, 14 + 40 + ulen) != 0) {
        net6_udp_port = 0;
        return -1;
    }
    uint32_t start = net_now_sec();
    int got = -1;
    while (net_now_sec() - start < timeout_sec) {
        net6_pump_once();
        if (net6_udp_got > 0) {
            got = net6_udp_got;
            break;
        }
    }
    net6_udp_port = 0;
    return got;
}

void net6_status(char *buf, size_t cap) {
    char tmp[48];
    size_t w = 0;
    const char *p;
    /* link-local */
    p = "ipv6 link-local ";
    while (*p && w + 1 < cap) {
        buf[w++] = *p++;
    }
    net_ipv6_format_addr(net6_ll, tmp);
    p = tmp;
    while (*p && w + 1 < cap) {
        buf[w++] = *p++;
    }
    if (w + 1 < cap) {
        buf[w++] = '\n';
    }
    /* global */
    p = "ipv6 global ";
    while (*p && w + 1 < cap) {
        buf[w++] = *p++;
    }
    if (net6_have_global) {
        net_ipv6_format_addr(net6_global, tmp);
    } else {
        const char *none = "(none)";
        for (int i = 0; i < 6; i++) {
            tmp[i] = none[i];
        }
        tmp[6] = '\0';
    }
    p = tmp;
    while (*p && w + 1 < cap) {
        buf[w++] = *p++;
    }
    if (w + 1 < cap) {
        buf[w++] = '\n';
    }
    /* router */
    p = "ipv6 router ";
    while (*p && w + 1 < cap) {
        buf[w++] = *p++;
    }
    if (net6_have_router) {
        net_ipv6_format_addr(net6_router, tmp);
    } else {
        const char *none = "(none)";
        for (int i = 0; i < 6; i++) {
            tmp[i] = none[i];
        }
        tmp[6] = '\0';
    }
    p = tmp;
    while (*p && w + 1 < cap) {
        buf[w++] = *p++;
    }
    if (w < cap) {
        buf[w] = '\0';
    } else if (cap > 0) {
        buf[cap - 1] = '\0';
    }
}

void net_my_ip(uint8_t *out) {
    for (int i = 0; i < 4; i++) {
        out[i] = net_ip[i];
    }
}

void net_my_router(uint8_t *out) {
    for (int i = 0; i < 4; i++) {
        out[i] = net_router[i];
    }
}

void net_my_dns(uint8_t *out) {
    for (int i = 0; i < 4; i++) {
        out[i] = net_dns[i];
    }
}

/* Random values from the entropy pool: connection
 * identifiers must not be predictable (the TCP initial
 * sequence number and the DNS transaction id / source
 * port used to be a time formula and fixed constants,
 * which made off-path spoofing trivial). */
static uint32_t net_rand32(void) {
    uint8_t b[4];
    entropy_bytes(b, 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* ---- DNS ---- */

static int dns_lookup(const char *host, uint8_t *out_ip) {
    uint16_t dns_id = (uint16_t)net_rand32();
    uint16_t src_port = (uint16_t)(49152 + (net_rand32() % 16384));
    uint8_t query[512];
    size_t qlen = net_dns_build_query(query, host, dns_id);
    if (qlen == 0) {
        return -1;
    }
    for (int attempt = 0; attempt < 3; attempt++) {
        if (send_udp(net_gw_hw, net_dns, src_port, 53, query, qlen,
                     (uint16_t)(0x2000 + attempt)) != 0) {
            return -1;
        }
        uint8_t reply[1024];
        int n = wait_udp(reply, sizeof(reply), src_port, net_dns, 3);
        if (n < 0) {
            continue;
        }
        if (net_dns_parse(reply, (size_t)n, dns_id, out_ip)) {
            return 0;
        }
    }
    return -1;
}

int net_dns_test(const char *server) {
    uint8_t srv[4];
    if (server == NULL) {
        return -1;
    }
    /* Parse dotted decimal. */
    int part = 0;
    int val = 0;
    int digits = 0;
    for (int i = 0; server[i] != '\0'; i++) {
        char ch = server[i];
        if (ch >= '0' && ch <= '9') {
            val = val * 10 + (ch - '0');
            digits++;
            if (val > 255 || digits > 3) {
                return -1;
            }
        } else if (ch == '.' && part < 3 && digits > 0) {
            srv[part++] = (uint8_t)val;
            val = 0;
            digits = 0;
        } else {
            return -1;
        }
    }
    if (part != 3 || digits == 0) {
        return -1;
    }
    srv[3] = (uint8_t)val;
    if (!net_configured) {
        if (net_ensure() != 0) {
            return -1;
        }
    }
    uint8_t query[512];
    uint16_t dns_id = (uint16_t)net_rand32();
    uint16_t src_port = (uint16_t)(49152 + (net_rand32() % 16384));
    size_t qlen = net_dns_build_query(query, "example.com", dns_id);
    if (qlen == 0) {
        return -1;
    }
    for (int attempt = 0; attempt < 3; attempt++) {
        if (send_udp(net_gw_hw, srv, src_port, 53, query, qlen,
                     (uint16_t)(0x2000 + attempt)) != 0) {
            return -1;
        }
        uint8_t reply[1024];
        int n = wait_udp(reply, sizeof(reply), src_port, srv, 3);
        if (n < 0) {
            continue;
        }
        uint8_t ip[4];
        if (net_dns_parse(reply, (size_t)n, dns_id, ip)) {
            log_ip("[net] dnstest resolved: ", ip);
            return 0;
        }
        serial_write_line("[net] dnstest: reply was not a valid answer");
    }
    serial_write_line("[net] dnstest: no answer");
    return -1;
}

/* ---- TCP ---- */

/* The TCP layer is port-generic: the peer port lives in
 * the stream (net_tcp_stream in netstack.h), so the same
 * code serves HTTP on 80, SMTP on 25 and POP3 on 110. */

static int tcp_send_segment(net_tcp_stream *s, uint8_t flags,
                            const uint8_t *payload, size_t payload_len,
                            uint32_t ack) {
    if (s->v6) {
        /* IPv6: 擬似ヘッダチェックサム付きで組み立て、
         * 接続時に決めた次ホップへ直接送る。 */
        static uint8_t seg6[1500];
        if (20 + payload_len > sizeof(seg6)) {
            return -1;
        }
        net_tcp_build6(seg6, s->my_port, s->peer_port, s->my_seq,
                       ack, flags, 8192, s->src_ip6, s->peer_ip6,
                       payload, payload_len);
        return send6_packet(s->peer_hw6, s->src_ip6, s->peer_ip6,
                            6, 64, seg6, 20 + payload_len);
    }
    uint8_t *ip_pkt = ip_scratch;
    size_t total = 20 + 20 + payload_len;
    if (total > NET_FRAME_MAX) {
        return -1;
    }
    net_ipv4_build(ip_pkt, (uint16_t)total, 6, net_ip, s->peer_ip,
                   (uint16_t)(0x3000 + (s->my_seq & 0xFF)));
    net_tcp_build(ip_pkt + 20, s->my_port, s->peer_port, s->my_seq,
                  ack, flags, 8192, net_ip, s->peer_ip, payload,
                  payload_len);
    return send_ip_frame(net_gw_hw, ip_pkt, total);
}

/* ---- 同時接続の振り分け台帳 ----
 * 開いている接続を登録しておき、ある接続の待ち時間に
 * 届いた他接続のセグメントを、その接続の受信バッファへ
 * 振り分ける。以前は待ち中の接続宛て以外のセグメントを
 * 読んで捨てていたため、同時接続は再送頼みで相互に
 * 引っ張り合っていた。 */
static net_tcp_stream *tcp_streams[NET_TCP_MAX_STREAMS];

static void tcp_register(net_tcp_stream *s) {
    for (int i = 0; i < NET_TCP_MAX_STREAMS; i++) {
        if (tcp_streams[i] == s) {
            return;
        }
    }
    for (int i = 0; i < NET_TCP_MAX_STREAMS; i++) {
        if (tcp_streams[i] == NULL) {
            tcp_streams[i] = s;
            return;
        }
    }
}

static void tcp_unregister(net_tcp_stream *s) {
    for (int i = 0; i < NET_TCP_MAX_STREAMS; i++) {
        if (tcp_streams[i] == s) {
            tcp_streams[i] = NULL;
        }
    }
}

static net_tcp_stream *tcp_find_stream(const uint8_t *src_ip,
                                       uint16_t src_port,
                                       uint16_t dst_port,
                                       net_tcp_stream *exclude) {
    for (int i = 0; i < NET_TCP_MAX_STREAMS; i++) {
        net_tcp_stream *t = tcp_streams[i];
        if (t == NULL || t == exclude || !t->connected) {
            continue;
        }
        if (t->my_port != dst_port || t->peer_port != src_port) {
            continue;
        }
        int same = 1;
        for (int j = 0; j < 4; j++) {
            if (t->peer_ip[j] != src_ip[j]) {
                same = 0;
            }
        }
        if (same) {
            return t;
        }
    }
    return NULL;
}

static net_tcp_stream *tcp_find_stream6(const uint8_t *src_ip6,
                                        uint16_t src_port,
                                        uint16_t dst_port,
                                        net_tcp_stream *exclude) {
    for (int i = 0; i < NET_TCP_MAX_STREAMS; i++) {
        net_tcp_stream *t = tcp_streams[i];
        if (t == NULL || t == exclude || !t->connected || !t->v6) {
            continue;
        }
        if (t->my_port != dst_port || t->peer_port != src_port) {
            continue;
        }
        if (addr6_eq(t->peer_ip6, src_ip6)) {
            return t;
        }
    }
    return NULL;
}

/* Fold one received segment into a stream (no waiting):
 * an in-order data segment is appended to the receive
 * buffer and ACKed; a pure ACK only updates last_ack.
 * Returns 1 when data was appended, 0 when nothing new
 * arrived, -1 on RST. */
static int tcp_fold_segment(net_tcp_stream *s, const prism_tcp_hdr *h,
                            const uint8_t *seg, int n) {
    if ((h->flags & PRISM_TCP_RST) != 0) {
        /* RST はシーケンス番号が次の受信予定と一致する
         * ときだけ受理する (監査 F5: 送信元を詐称した RST で
         * 待機中でない接続まで切れてしまう問題への対策)。
         * 合わない RST は黙って捨てる。 */
        if (h->seq != s->peer_seq) {
            return 0;
        }
        s->rst_seen = 1;
        return -1;
    }
    s->last_ack = h->ack;
    if (h->ack > s->my_seq) {
        s->my_seq = h->ack;
    }
    if (n == 0) {
        if ((h->flags & PRISM_TCP_FIN) != 0) {
            s->peer_closed = 1;
        }
        return 0;
    }
    if (h->seq != s->peer_seq) {
        /* Out of order: re-ACK what we have and wait. */
        (void)tcp_send_segment(s, PRISM_TCP_ACK, NULL, 0,
                               s->peer_seq);
        return 0;
    }
    /* Accept only the bytes that fit the receive buffer
     * and advance peer_seq (the ACK number) by exactly
     * that many. The rest is NOT acknowledged, so the
     * peer retransmits it later. */
    size_t space = sizeof(s->rxbuf) - s->rx_len;
    size_t copy = (size_t)n < space ? (size_t)n : space;
    for (size_t i = 0; i < copy; i++) {
        s->rxbuf[s->rx_len + i] = seg[i];
    }
    s->rx_len += copy;
    s->peer_seq = (uint32_t)(s->peer_seq + copy);
    (void)tcp_send_segment(s, PRISM_TCP_ACK, NULL, 0, s->peer_seq);
    if ((h->flags & PRISM_TCP_FIN) != 0 && copy == (size_t)n) {
        /* The FIN sits after the last data byte; only
         * honour it when every byte was accepted. */
        s->peer_closed = 1;
    }
    return copy > 0 ? 1 : 0;
}

/* Wait for a TCP segment for this connection, up to
 * timeout_sec seconds. Returns payload length (>=0), or
 * -1 on timeout. Fills hdr and copies payload into out.
 * Segments for other registered connections are folded
 * into their streams, not dropped. */
static int tcp_wait_segment(net_tcp_stream *s, prism_tcp_hdr *hdr,
                            uint8_t *out, size_t cap,
                            uint32_t timeout_sec) {
    uint32_t start = net_now_sec();
    while (net_now_sec() - start < timeout_sec) {
        uint8_t *frame = frame_scratch;
        int n = nic_recv(frame, NET_FRAME_MAX);
        if (n <= 0) {
            continue;
        }
        uint8_t dst[6], src[6];
        uint16_t et;
        const uint8_t *pl;
        size_t pln;
        if (net_eth_parse(frame, (size_t)n, dst, src, &et, &pl,
                          &pln) != 0) {
            continue;
        }
        if (et == 0x86DD) {
            /* IPv6: TCP なら振り分け、それ以外 (ICMPv6 等) は
             * 裏側処理へ渡す (ND や RA が待ち時間にも進む)。 */
            uint8_t next6;
            const uint8_t *ippl6;
            size_t ipln6;
            uint8_t sip6[16], dip6[16];
            if (net_ipv6_parse(pl, pln, &next6, &ippl6, &ipln6,
                               sip6, dip6) != 0) {
                continue;
            }
            if (next6 != 6) {
                net6_handle_frame(frame, n);
                continue;
            }
            const uint8_t *tpl6;
            size_t tpln6;
            if (net_tcp_parse(ippl6, ipln6, hdr, &tpl6, &tpln6) != 0) {
                continue;
            }
            int match6 = s->v6 && addr6_eq(sip6, s->peer_ip6) &&
                         hdr->dst_port == s->my_port &&
                         hdr->src_port == s->peer_port;
            if (!match6) {
                net_tcp_stream *t = tcp_find_stream6(
                    sip6, hdr->src_port, hdr->dst_port, s);
                if (t != NULL) {
                    (void)tcp_fold_segment(t, hdr, tpl6, (int)tpln6);
                }
                continue;
            }
            size_t copy6 = tpln6 < cap ? tpln6 : cap;
            for (size_t i = 0; i < copy6; i++) {
                out[i] = tpl6[i];
            }
            return (int)copy6;
        }
        if (et != 0x0800) {
            continue;
        }
        uint8_t proto;
        const uint8_t *ippl;
        size_t ipln;
        uint8_t sip[4], dip[4];
        if (net_ipv4_parse(pl, pln, &proto, &ippl, &ipln, sip, dip) != 0) {
            continue;
        }
        if (net_fw_blocked(sip)) {
            continue;
        }
        if (proto != 6) {
            continue;
        }
        const uint8_t *tpl;
        size_t tpln;
        if (net_tcp_parse(ippl, ipln, hdr, &tpl, &tpln) != 0) {
            continue;
        }
        int from_peer = !s->v6;
        for (int i = 0; i < 4; i++) {
            if (sip[i] != s->peer_ip[i]) {
                from_peer = 0;
            }
        }
        if (!from_peer || hdr->dst_port != s->my_port ||
            hdr->src_port != s->peer_port) {
            /* 他の開いている接続宛てなら、その接続へ
             * 振り分ける (捨てない)。 */
            net_tcp_stream *t = tcp_find_stream(
                sip, hdr->src_port, hdr->dst_port, s);
            if (t != NULL) {
                (void)tcp_fold_segment(t, hdr, tpl, (int)tpln);
            }
            continue;
        }
        size_t copy = tpln < cap ? tpln : cap;
        for (size_t i = 0; i < copy; i++) {
            out[i] = tpl[i];
        }
        return (int)copy;
    }
    return -1;
}

/* Wait for one segment and fold it into the stream.
 * Returns 1 when data was appended, 0 when nothing new
 * arrived, -1 on RST or timeout. */
static int tcp_pump(net_tcp_stream *s, uint32_t timeout_sec) {
    if (s->rst_seen) {
        return -1;
    }
    prism_tcp_hdr h;
    uint8_t *seg = tcp_seg_scratch;
    int n = tcp_wait_segment(s, &h, seg, sizeof(tcp_seg_scratch),
                             timeout_sec);
    if (n < 0) {
        return -1;
    }
    return tcp_fold_segment(s, &h, seg, n);
}

/* One TCP segment can carry at most this many payload
 * bytes: the frame scratch is NET_FRAME_MAX (1600) bytes
 * including the Ethernet/IP/TCP headers, and the peer's
 * MTU is 1500. Longer sends are split into chunks of
 * this size. */
#define TCP_MAX_PAYLOAD 1400

/* Send data and wait until the peer ACKs all of it,
 * retransmitting on timeout (up to 3 attempts per chunk).
 * A data segment that arrives while waiting is kept in
 * the receive buffer by tcp_pump, not dropped. Returns 0
 * on success. (Before this split existed, any send over
 * one segment silently failed, which capped plain mail
 * bodies at about 1.5KB.) */
static int tcp_send_reliable(net_tcp_stream *s, const uint8_t *data,
                             size_t len) {
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > TCP_MAX_PAYLOAD) {
            chunk = TCP_MAX_PAYLOAD;
        }
        uint32_t seq0 = s->my_seq;
        int ok = 0;
        for (int attempt = 0; attempt < 3 && !ok; attempt++) {
            s->my_seq = seq0;
            if (tcp_send_segment(s, PRISM_TCP_PSH | PRISM_TCP_ACK,
                                 data + off, chunk, s->peer_seq) != 0) {
                return -1;
            }
            uint32_t start = net_now_sec();
            while (net_now_sec() - start < 5) {
                int r = tcp_pump(s, 1);
                if (r < 0) {
                    break;
                }
                if (s->last_ack >= seq0 + (uint32_t)chunk) {
                    s->my_seq = seq0 + (uint32_t)chunk;
                    ok = 1;
                    break;
                }
            }
        }
        if (!ok) {
            return -1;
        }
        off += chunk;
    }
    return 0;
}

/* 送信元ポートは接続ごとに変える。同じポートと秒単位の
 * 初期シーケンス番号を再利用すると、連続する 2 回の接続が
 * 相手側で同じ接続とみなされ、SYN が拒否される。 */
static uint16_t next_tcp_port = 54322;

/* ハンドシェイク本体 (アドレス体系に依存しない部分)。
 * 呼び出し前に s の相手アドレス (v4: peer_ip、v6: peer_ip6 /
 * src_ip6 / peer_hw6 と v6=1) を設定しておくこと。 */
static int tcp_handshake(net_tcp_stream *s, uint16_t peer_port) {
    s->peer_port = peer_port;
    s->my_port = next_tcp_port;
    next_tcp_port++;
    if (next_tcp_port >= 60000) {
        next_tcp_port = 54322;
    }
    s->my_seq = net_rand32();
    s->peer_seq = 0;
    s->last_ack = 0;
    s->connected = 0;
    s->peer_closed = 0;
    s->rst_seen = 0;
    s->rx_len = 0;
    s->rx_pos = 0;
    /* ハンドシェイクの待ち時間も、他の接続のセグメントを
     * 振り分けられるよう先に登録する。 */
    tcp_register(s);
    for (int attempt = 0; attempt < 3; attempt++) {
        if (tcp_send_segment(s, PRISM_TCP_SYN, NULL, 0, 0) != 0) {
            tcp_unregister(s);
            return -1;
        }
        prism_tcp_hdr h;
        uint8_t tmp[64];
        int n = tcp_wait_segment(s, &h, tmp, sizeof(tmp), 3);
        if (n < 0) {
            continue;
        }
        if ((h.flags & PRISM_TCP_RST) != 0) {
            tcp_unregister(s);
            return -1;
        }
        if ((h.flags & (PRISM_TCP_SYN | PRISM_TCP_ACK)) !=
            (PRISM_TCP_SYN | PRISM_TCP_ACK)) {
            continue;
        }
        if (h.ack != s->my_seq + 1) {
            continue;
        }
        s->peer_seq = h.seq + 1;
        s->my_seq += 1;
        if (tcp_send_segment(s, PRISM_TCP_ACK, NULL, 0, s->peer_seq) != 0) {
            tcp_unregister(s);
            return -1;
        }
        s->connected = 1;
        return 0;
    }
    tcp_unregister(s);
    return -1;
}

static int tcp_connect(net_tcp_stream *s, const uint8_t *peer_ip,
                       uint16_t peer_port) {
    for (int i = 0; i < 4; i++) {
        s->peer_ip[i] = peer_ip[i];
    }
    s->v6 = 0;
    return tcp_handshake(s, peer_port);
}

int net6_tcp_open(const uint8_t dst[16], uint16_t port,
                  net_tcp_stream *s) {
    if (dst == NULL || s == NULL || port == 0) {
        return -1;
    }
    for (size_t i = 0; i < sizeof(*s); i++) {
        ((uint8_t *)s)[i] = 0;
    }
    if (net6_configure() != 0 && !net6_ll_ready) {
        return -1;
    }
    s->v6 = 1;
    for (int i = 0; i < 16; i++) {
        s->peer_ip6[i] = dst[i];
    }
    /* 次ホップと送信元アドレスを先に決める (ND の待ちは
     * ハンドシェイクの前に済ませる)。 */
    const uint8_t *src = NULL;
    uint8_t hw[6];
    if (net6_next_hop(dst, &src, hw) != 0) {
        return -1;
    }
    for (int i = 0; i < 16; i++) {
        s->src_ip6[i] = src[i];
    }
    for (int i = 0; i < 6; i++) {
        s->peer_hw6[i] = hw[i];
    }
    if (tcp_handshake(s, port) != 0) {
        serial_write_line("[net] TCP6 connect failed");
        return -1;
    }
    return 0;
}

/* サーバ側: port への SYN を待って接続を受け付ける。
 * 受け付けたら s がその接続になる (net_tcp_send /
 * net_tcp_recv / net_tcp_close がそのまま使える)。
 * 受け付けた接続は台帳に登録され、他の接続の待ち時間に
 * 届いたセグメントも捨てずに振り分けられるため、
 * NET_TCP_MAX_STREAMS 本まで同時に開いておける。
 * 連続した接続は繰り返し listen することで受けられる。 */
int net_tcp_listen(uint16_t port, uint32_t timeout_sec,
                   net_tcp_stream *s) {
    s->v6 = 0;
    s->my_port = port;
    s->peer_port = 0;
    s->peer_ip[0] = s->peer_ip[1] = s->peer_ip[2] = s->peer_ip[3] = 0;
    s->my_seq = net_rand32();
    s->peer_seq = 0;
    s->last_ack = 0;
    s->connected = 0;
    s->peer_closed = 0;
    s->rst_seen = 0;
    s->rx_len = 0;
    s->rx_pos = 0;
    uint32_t start = net_now_sec();
    while (net_now_sec() - start < timeout_sec) {
        uint8_t *frame = frame_scratch;
        int n = nic_recv(frame, NET_FRAME_MAX);
        if (n <= 0) {
            continue;
        }
        uint8_t dst[6], src[6];
        uint16_t et;
        const uint8_t *pl;
        size_t pln;
        if (net_eth_parse(frame, (size_t)n, dst, src, &et, &pl,
                          &pln) != 0 || et != 0x0800) {
            continue;
        }
        uint8_t proto;
        const uint8_t *ippl;
        size_t ipln;
        uint8_t sip[4], dip[4];
        if (net_ipv4_parse(pl, pln, &proto, &ippl, &ipln, sip,
                           dip) != 0 || proto != 6) {
            continue;
        }
        if (net_fw_blocked(sip)) {
            continue;
        }
        prism_tcp_hdr h;
        const uint8_t *tpl;
        size_t tpln;
        if (net_tcp_parse(ippl, ipln, &h, &tpl, &tpln) != 0) {
            continue;
        }
        if (h.dst_port != port ||
            (h.flags & PRISM_TCP_SYN) == 0 ||
            (h.flags & PRISM_TCP_ACK) != 0) {
            /* 待ち受け中でも、開いている接続宛ての
             * セグメントはその接続へ振り分ける。 */
            net_tcp_stream *t = tcp_find_stream(
                sip, h.src_port, h.dst_port, s);
            if (t != NULL) {
                (void)tcp_fold_segment(t, &h, tpl, (int)tpln);
            }
            continue;
        }
        /* SYN を受けた: 相手を固定して SYN-ACK を返す。 */
        for (int i = 0; i < 4; i++) {
            s->peer_ip[i] = sip[i];
        }
        s->peer_port = h.src_port;
        s->peer_seq = h.seq + 1;
        if (tcp_send_segment(s, PRISM_TCP_SYN | PRISM_TCP_ACK,
                             NULL, 0, s->peer_seq) != 0) {
            return -1;
        }
        s->my_seq += 1;
        /* 最後の ACK を待つ (データ付きでもよい)。 */
        uint8_t tmp[256];
        prism_tcp_hdr h2;
        int m = tcp_wait_segment(s, &h2, tmp, sizeof(tmp), 5);
        if (m < 0 || (h2.flags & PRISM_TCP_ACK) == 0 ||
            h2.ack != s->my_seq) {
            return -1;
        }
        if (m > 0) {
            /* ハンドシェイクと一緒に来たデータを
             * 受信バッファへ入れておく。 */
            size_t copy = (size_t)m < sizeof(s->rxbuf)
                              ? (size_t)m
                              : sizeof(s->rxbuf);
            for (size_t i = 0; i < copy; i++) {
                s->rxbuf[i] = tmp[i];
            }
            s->rx_len = copy;
            s->peer_seq = h2.seq + (uint32_t)copy;
        } else {
            s->peer_seq = h2.seq;
            if (s->peer_seq == 0) {
                s->peer_seq = h.seq + 1;
            }
        }
        s->connected = 1;
        tcp_register(s);
        return 0;
    }
    return -1;
}

/* サーバ側 (IPv6): 自分の IPv6 アドレス宛の SYN を待って
 * 接続を受け付ける。作法は net_tcp_listen と同じ。 */
int net6_tcp_listen(uint16_t port, uint32_t timeout_sec,
                    net_tcp_stream *s) {
    if (net6_configure() != 0 && !net6_ll_ready) {
        return -1;
    }
    s->v6 = 1;
    s->my_port = port;
    s->peer_port = 0;
    for (int i = 0; i < 16; i++) {
        s->peer_ip6[i] = 0;
        s->src_ip6[i] = 0;
    }
    for (int i = 0; i < 4; i++) {
        s->peer_ip[i] = 0;
    }
    s->my_seq = net_rand32();
    s->peer_seq = 0;
    s->last_ack = 0;
    s->connected = 0;
    s->peer_closed = 0;
    s->rst_seen = 0;
    s->rx_len = 0;
    s->rx_pos = 0;
    uint32_t start = net_now_sec();
    while (net_now_sec() - start < timeout_sec) {
        uint8_t *frame = frame_scratch;
        int n = nic_recv(frame, NET_FRAME_MAX);
        if (n <= 0) {
            continue;
        }
        uint8_t dst[6], src[6];
        uint16_t et;
        const uint8_t *pl;
        size_t pln;
        if (net_eth_parse(frame, (size_t)n, dst, src, &et, &pl,
                          &pln) != 0) {
            continue;
        }
        if (et != 0x86DD) {
            net6_handle_frame(frame, n);
            continue;
        }
        uint8_t next;
        const uint8_t *ippl;
        size_t ipln;
        uint8_t sip[16], dip[16];
        if (net_ipv6_parse(pl, pln, &next, &ippl, &ipln, sip,
                           dip) != 0) {
            continue;
        }
        if (next != 6) {
            net6_handle_frame(frame, n);
            continue;
        }
        prism_tcp_hdr h;
        const uint8_t *tpl;
        size_t tpln;
        if (net_tcp_parse(ippl, ipln, &h, &tpl, &tpln) != 0) {
            continue;
        }
        if (h.dst_port != port ||
            (h.flags & PRISM_TCP_SYN) == 0 ||
            (h.flags & PRISM_TCP_ACK) != 0) {
            /* 待ち受け中でも、開いている接続宛ての
             * セグメントはその接続へ振り分ける。 */
            net_tcp_stream *t = tcp_find_stream6(
                sip, h.src_port, h.dst_port, s);
            if (t != NULL) {
                (void)tcp_fold_segment(t, &h, tpl, (int)tpln);
            }
            continue;
        }
        if (!addr6_is_ours(dip)) {
            continue;
        }
        /* SYN を受けた: 相手と宛先を固定して SYN-ACK を返す。
         * 返信先 MAC は届いたフレームの送信元 (相手か
         * ルータ) で正しい。 */
        for (int i = 0; i < 16; i++) {
            s->peer_ip6[i] = sip[i];
            s->src_ip6[i] = dip[i];
        }
        for (int i = 0; i < 6; i++) {
            s->peer_hw6[i] = src[i];
        }
        s->peer_port = h.src_port;
        s->peer_seq = h.seq + 1;
        if (tcp_send_segment(s, PRISM_TCP_SYN | PRISM_TCP_ACK,
                             NULL, 0, s->peer_seq) != 0) {
            return -1;
        }
        s->my_seq += 1;
        /* 最後の ACK を待つ (データ付きでもよい)。 */
        uint8_t tmp[256];
        prism_tcp_hdr h2;
        int m = tcp_wait_segment(s, &h2, tmp, sizeof(tmp), 5);
        if (m < 0 || (h2.flags & PRISM_TCP_ACK) == 0 ||
            h2.ack != s->my_seq) {
            return -1;
        }
        if (m > 0) {
            size_t copy = (size_t)m < sizeof(s->rxbuf)
                              ? (size_t)m
                              : sizeof(s->rxbuf);
            for (size_t i = 0; i < copy; i++) {
                s->rxbuf[i] = tmp[i];
            }
            s->rx_len = copy;
            s->peer_seq = h2.seq + (uint32_t)copy;
        } else {
            s->peer_seq = h2.seq;
            if (s->peer_seq == 0) {
                s->peer_seq = h.seq + 1;
            }
        }
        s->connected = 1;
        tcp_register(s);
        return 0;
    }
    return -1;
}

static void tcp_close(net_tcp_stream *s) {
    tcp_unregister(s);
    if (!s->connected) {
        return;
    }
    (void)tcp_send_segment(s, PRISM_TCP_FIN | PRISM_TCP_ACK, NULL, 0,
                           s->peer_seq);
    s->connected = 0;
}

/* Resolve a host name: a dotted decimal address is used
 * directly, anything else goes through DNS. Returns 0 on
 * success. */
static int resolve_host(const char *host, uint8_t *peer_ip);

int net_resolve(const char *host, uint8_t *ip_out) {
    return resolve_host(host, ip_out);
}

static int resolve_host(const char *host, uint8_t *peer_ip) {
    int is_ip = 1;
    int part = 0;
    int val = 0;
    int digits = 0;
    for (int i = 0; host[i] != '\0'; i++) {
        char ch = host[i];
        if (ch >= '0' && ch <= '9') {
            val = val * 10 + (ch - '0');
            digits++;
            if (val > 255 || digits > 3) {
                is_ip = 0;
                break;
            }
        } else if (ch == '.' && part < 3 && digits > 0) {
            peer_ip[part++] = (uint8_t)val;
            val = 0;
            digits = 0;
        } else {
            is_ip = 0;
            break;
        }
    }
    if (is_ip) {
        if (part != 3 || digits == 0) {
            is_ip = 0;
        } else {
            peer_ip[3] = (uint8_t)val;
        }
    }
    if (!is_ip) {
        if (dns_lookup(host, peer_ip) != 0) {
            return -1;
        }
    }
    return 0;
}

int net_tcp_open(const char *host, uint16_t port, net_tcp_stream *s) {
    if (host == NULL || s == NULL || port == 0) {
        return -1;
    }
    for (size_t i = 0; i < sizeof(*s); i++) {
        ((uint8_t *)s)[i] = 0;
    }
    if (!net_configured) {
        if (net_ensure() != 0) {
            return -1;
        }
    }
    uint8_t peer_ip[4];
    if (resolve_host(host, peer_ip) != 0) {
        serial_write_line("[net] DNS failed");
        return -1;
    }
    log_ip("[net] resolved: ", peer_ip);
    if (tcp_connect(s, peer_ip, port) != 0) {
        serial_write_line("[net] TCP connect failed");
        return -1;
    }
    return 0;
}

int net_tcp_send(net_tcp_stream *s, const char *data, size_t len) {
    if (s == NULL || !s->connected || data == NULL) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    return tcp_send_reliable(s, (const uint8_t *)data, len);
}

int net_tcp_recv_line(net_tcp_stream *s, char *out, size_t cap,
                      uint32_t timeout_sec) {
    if (s == NULL || out == NULL || cap < 2 || !s->connected) {
        return -1;
    }
    uint32_t deadline = net_now_sec() + timeout_sec;
    size_t n = 0;
    for (;;) {
        /* Serve what is already buffered. */
        while (s->rx_pos < s->rx_len) {
            uint8_t ch = s->rxbuf[s->rx_pos++];
            if (ch == '\n') {
                if (n > 0 && out[n - 1] == '\r') {
                    n--;
                }
                out[n] = '\0';
                return (int)n;
            }
            if (n + 1 < cap) {
                out[n++] = (char)ch;
            }
            /* A line longer than the buffer is consumed and
             * truncated, not left half-read. */
        }
        s->rx_pos = 0;
        s->rx_len = 0;
        if (s->peer_closed || s->rst_seen) {
            return -1;
        }
        uint32_t now = net_now_sec();
        if (now >= deadline) {
            return -1;
        }
        uint32_t remain = deadline - now;
        if (remain == 0) {
            remain = 1;
        }
        int r = tcp_pump(s, remain);
        if (r < 0) {
            if (s->peer_closed) {
                return -1;
            }
            if (net_now_sec() >= deadline) {
                return -1;
            }
            /* A quiet second inside the deadline: try again. */
        }
    }
}

int net_tcp_recv(net_tcp_stream *s, uint8_t *out, size_t cap,
                 uint32_t timeout_sec) {
    if (s == NULL || out == NULL || cap == 0 || !s->connected) {
        return -1;
    }
    uint32_t deadline = net_now_sec() + timeout_sec;
    for (;;) {
        if (s->rx_pos < s->rx_len) {
            size_t avail = s->rx_len - s->rx_pos;
            size_t n = avail < cap ? avail : cap;
            for (size_t i = 0; i < n; i++) {
                out[i] = s->rxbuf[s->rx_pos + i];
            }
            s->rx_pos += n;
            if (s->rx_pos >= s->rx_len) {
                s->rx_pos = 0;
                s->rx_len = 0;
            }
            return (int)n;
        }
        s->rx_pos = 0;
        s->rx_len = 0;
        if (s->peer_closed) {
            return 0;
        }
        if (s->rst_seen) {
            return -1;
        }
        uint32_t now = net_now_sec();
        if (now >= deadline) {
            return -1;
        }
        uint32_t remain = deadline - now;
        if (remain == 0) {
            remain = 1;
        }
        int r = tcp_pump(s, remain);
        if (r < 0 && s->peer_closed) {
            return 0;
        }
        /* On a quiet wait inside the deadline, loop and
         * check the deadline again. */
    }
}

void net_tcp_close(net_tcp_stream *s) {
    if (s == NULL) {
        return;
    }
    tcp_close(s);
}

/* GET リクエスト文を作る。net_fetch_http() と
 * net_fetch_http_stream() で共用する。 */
/* プロキシ経由フェッチの間だけ設定される上書き:
 * リクエストターゲット (絶対 URI) と Host ヘッダ。 */
static const char *fetch_uri_override;
static const char *fetch_host_override;
/* 次のフェッチで送る Cookie ヘッダの値 (jar が設定)。 */
static const char *fetch_cookie_hdr;

void net_set_fetch_cookie(const char *cookie) {
    fetch_cookie_hdr =
        (cookie != NULL && cookie[0] != '\0') ? cookie : NULL;
}

static size_t build_get_request(const char *host, const char *path,
                                char *req, size_t cap) {
    size_t p = 0;
    const char *parts[5];
    parts[0] = "GET ";
    parts[1] = fetch_uri_override != NULL ? fetch_uri_override
                                          : path;
    parts[2] = " HTTP/1.0\r\nHost: ";
    parts[3] = fetch_host_override != NULL ? fetch_host_override
                                           : host;
    parts[4] = "\r\nUser-Agent: PrismOS\r\nAccept: text/html\r\n\r\n";
    for (int i = 0; i < 4; i++) {
        const char *s = parts[i];
        while (*s != '\0' && p + 1 < cap) {
            req[p++] = *s++;
        }
    }
    if (fetch_cookie_hdr != NULL) {
        const char *pre = "\r\nCookie: ";
        while (*pre != '\0' && p + 1 < cap) {
            req[p++] = *pre++;
        }
        const char *s = fetch_cookie_hdr;
        while (*s != '\0' && p + 1 < cap) {
            req[p++] = *s++;
        }
    }
    {
        const char *s = parts[4];
        while (*s != '\0' && p + 1 < cap) {
            req[p++] = *s++;
        }
    }
    req[p] = '\0';
    return p;
}

/* Defined below with the streaming fetch; the plain
 * fetch validates its headers with it too. */
static const uint8_t *header_find_ci(const uint8_t *hdr, size_t len,
                                     const char *needle);

/* chunked 転送符号化をその場で解読する純粋関数。
 * buf[0..len) の chunked 本体を解読し、前に詰めて
 * *out_len に解読後の長さを入れる。形式が壊れている・
 * 途中で終わっている場合は -1。 */
int net_http_decode_chunked(uint8_t *buf, size_t len,
                            size_t *out_len) {
    size_t r = 0;
    size_t w = 0;
    for (;;) {
        uint64_t n = 0;
        int digits = 0;
        while (r < len) {
            uint8_t ch = buf[r];
            int v = -1;
            if (ch >= '0' && ch <= '9') {
                v = ch - '0';
            } else if (ch >= 'a' && ch <= 'f') {
                v = ch - 'a' + 10;
            } else if (ch >= 'A' && ch <= 'F') {
                v = ch - 'A' + 10;
            }
            if (v < 0) {
                break;
            }
            n = n * 16 + (uint64_t)v;
            if (n > 16777216) {
                return -1;
            }
            r++;
            digits++;
        }
        if (digits == 0) {
            return -1;
        }
        /* 拡張 (;...) を読み飛ばして行末へ。 */
        while (r < len && buf[r] != '\r') {
            if (buf[r] == '\n') {
                return -1;
            }
            r++;
        }
        if (r + 1 >= len || buf[r] != '\r' || buf[r + 1] != '\n') {
            return -1;
        }
        r += 2;
        if (n == 0) {
            /* 終端チャンク。トレーラは読み飛ばす。 */
            *out_len = w;
            return 0;
        }
        if (r + n + 2 > len) {
            return -1; /* 本体が途中で切れている */
        }
        for (size_t i = 0; i < n; i++) {
            buf[w + i] = buf[r + i];
        }
        w += (size_t)n;
        r += (size_t)n;
        if (buf[r] != '\r' || buf[r + 1] != '\n') {
            return -1;
        }
        r += 2;
    }
}

/* 行頭が prefix で始まるか (大文字小文字を区別しない)。
 * prefix は小文字で渡すこと。 */
static int hdr_ci_prefix(const char *line, const char *prefix) {
    size_t i = 0;
    while (prefix[i] != '\0') {
        char c = line[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c + 32);
        }
        if (c != prefix[i]) {
            return 0;
        }
        i++;
    }
    return 1;
}

/* 生の応答 (out[0..have)) を処理する: ヘッダと本体を分け、
 * chunked を解読し、完全性を確かめ、本体を out の先頭へ
 * 寄せる。info があれば状態コードと Location を入れる。
 * 完全な応答なら 0 (状態コードは問わない)、駄目なら -1。 */
static int process_response(char *out, size_t have, int closed_ok,
                            size_t cap, size_t *out_len,
                            net_fetch_info *info) {
    if (info != NULL) {
        info->status = 0;
        info->location[0] = '\0';
        info->set_cookie[0] = '\0';
    }
    size_t body_len = 0;
    int code = 0;
    const uint8_t *body = net_http_body((const uint8_t *)out, have,
                                        &body_len, &code);
    if (info != NULL) {
        info->status = code;
    }
    if (body == NULL) {
        return -1;
    }
    size_t off = (size_t)(body - (const uint8_t *)out);
    if (info != NULL) {
        const uint8_t *v =
            header_find_ci((const uint8_t *)out, off, "location:");
        if (v != NULL) {
            const uint8_t *end = (const uint8_t *)out + off;
            while (v < end && (*v == ' ' || *v == '\t')) {
                v++;
            }
            size_t n = 0;
            while (v + n < end && v[n] != '\r' && v[n] != '\n' &&
                   n + 1 < sizeof(info->location)) {
                info->location[n] = (char)v[n];
                n++;
            }
            info->location[n] = '\0';
        }
        /* Set-Cookie ヘッダを全部集める (行単位で走査し、
         * 値を '\n' 区切りで set_cookie に入れる)。 */
        {
            size_t used = 0;
            size_t p = 0;
            while (p < off) {
                size_t eol = p;
                while (eol < off && out[eol] != '\n') {
                    eol++;
                }
                size_t line_len = eol - p;
                if (line_len > 11 &&
                    hdr_ci_prefix(out + p, "set-cookie:")) {
                    const char *v = out + p + 11;
                    size_t vl = line_len - 11;
                    while (vl > 0 && (*v == ' ' || *v == '\t')) {
                        v++;
                        vl--;
                    }
                    while (vl > 0 &&
                           (v[vl - 1] == '\r' || v[vl - 1] == ' ' ||
                            v[vl - 1] == '\t')) {
                        vl--;
                    }
                    if (used > 0 &&
                        used + 1 < sizeof(info->set_cookie)) {
                        info->set_cookie[used++] = '\n';
                    }
                    for (size_t i = 0;
                         i < vl &&
                         used + 1 < sizeof(info->set_cookie);
                         i++) {
                        info->set_cookie[used++] = v[i];
                    }
                    info->set_cookie[used] = '\0';
                }
                p = eol + 1;
            }
        }
    }
    int chunked = header_find_ci((const uint8_t *)out, off,
                                 "chunked") != NULL;
    if (chunked) {
        size_t decoded = 0;
        if (net_http_decode_chunked((uint8_t *)out + off, body_len,
                                    &decoded) != 0) {
            return -1;
        }
        body_len = decoded;
    } else {
        const uint8_t *v =
            header_find_ci((const uint8_t *)out, off,
                           "content-length:");
        if (v != NULL) {
            const uint8_t *end = (const uint8_t *)out + off;
            while (v < end && (*v == ' ' || *v == '\t')) {
                v++;
            }
            uint64_t n = 0;
            int digits = 0;
            while (v < end && *v >= '0' && *v <= '9') {
                n = n * 10 + (uint64_t)(*v - '0');
                v++;
                digits++;
            }
            if (digits > 0 && n != (uint64_t)body_len) {
                return -1;
            }
        } else if (!closed_ok) {
            return -1;
        }
    }
    (void)cap;
    for (size_t i = 0; i < body_len; i++) {
        out[i] = out[off + i];
    }
    out[body_len] = '\0';
    if (out_len != NULL) {
        *out_len = body_len;
    }
    return 0;
}

/* 平文 HTTP で生の応答を out へ読み、process_response する。
 * 完全な応答なら 0。 */
static int fetch_http_raw(const char *host, uint16_t port,
                          const char *path, char *out, size_t cap,
                          size_t *out_len, net_fetch_info *info) {
    char req[512];
    size_t p = build_get_request(host, path, req, sizeof(req));
    static net_tcp_stream s;
    size_t have = 0;
    int closed_ok = 0;
    for (int attempt = 0; attempt < 3 && have == 0; attempt++) {
        if (net_tcp_open(host, port, &s) != 0) {
            return -1;
        }
        if (net_tcp_send(&s, req, p) != 0) {
            net_tcp_close(&s);
            return -1;
        }
        uint32_t start = net_now_sec();
        uint32_t last_data = start;
        while (net_now_sec() - start < 15) {
            while (s.rx_pos < s.rx_len && have + 1 < cap) {
                out[have++] = (char)s.rxbuf[s.rx_pos++];
            }
            if (s.rx_pos >= s.rx_len) {
                s.rx_pos = 0;
                s.rx_len = 0;
            }
            if (s.peer_closed && s.rx_len == 0) {
                closed_ok = 1;
                break;
            }
            if (have >= cap - 1) {
                break;
            }
            if (have > 0 && net_now_sec() - last_data >= 3) {
                break;
            }
            int r = tcp_pump(&s, 1);
            if (r == 1) {
                last_data = net_now_sec();
            }
            if (r < 0 && s.peer_closed) {
                break;
            }
        }
        net_tcp_close(&s);
    }
    if (have == 0) {
        return -1;
    }
    out[have] = '\0';
    return process_response(out, have, closed_ok, cap, out_len,
                            info);
}

int net_fetch_http_px(const char *host, uint16_t port,
                      const char *path, char *out, size_t cap,
                      size_t *out_len, net_fetch_info *info) {
    if (out_len != NULL) {
        *out_len = 0;
    }
    if (info != NULL) {
        info->status = 0;
        info->location[0] = '\0';
        info->set_cookie[0] = '\0';
    }
    if (out == NULL || cap < 2 || host == NULL || path == NULL) {
        return -1;
    }
    out[0] = '\0';
    return fetch_http_raw(host, port, path, out, cap, out_len,
                          info);
}

/* HTTP プロキシ経由で取得する。プロキシへ接続し、
 * 絶対 URI で要求する (平文 HTTP のみ。HTTPS は
 * CONNECT トンネルが必要なため対象外)。 */
int net_fetch_http_via(const char *proxy_host, uint16_t proxy_port,
                       const char *host, const char *path,
                       char *out, size_t cap, size_t *out_len,
                       net_fetch_info *info) {
    if (out_len != NULL) {
        *out_len = 0;
    }
    if (info != NULL) {
        info->status = 0;
        info->location[0] = '\0';
        info->set_cookie[0] = '\0';
    }
    if (out == NULL || cap < 2 || proxy_host == NULL ||
        host == NULL || path == NULL) {
        return -1;
    }
    out[0] = '\0';
    char uri[300];
    size_t p = 0;
    const char *pre = "http://";
    while (*pre != '\0' && p + 1 < sizeof(uri)) {
        uri[p++] = *pre++;
    }
    const char *h = host;
    while (*h != '\0' && p + 1 < sizeof(uri)) {
        uri[p++] = *h++;
    }
    const char *pa = path;
    while (*pa != '\0' && p + 1 < sizeof(uri)) {
        uri[p++] = *pa++;
    }
    uri[p] = '\0';
    fetch_uri_override = uri;
    fetch_host_override = host;
    int rc = fetch_http_raw(proxy_host, proxy_port, path, out, cap,
                            out_len, info);
    fetch_uri_override = NULL;
    fetch_host_override = NULL;
    return rc;
}

int net_fetch_http_ex(const char *host, const char *path, char *out,
                      size_t cap, size_t *out_len,
                      net_fetch_info *info) {
    /* A host longer than the scratch buffer keeps the old
     * behaviour (port 80); every bounded host goes through
     * the host:port split, defaulting to port 80. */
    char hostbuf[128];
    uint16_t port = 80;
    size_t hlen = 0;
    if (host == NULL) {
        return -1;
    }
    while (host[hlen] != '\0') {
        hlen++;
    }
    if (hlen >= sizeof(hostbuf)) {
        return net_fetch_http_px(host, 80, path, out, cap, out_len,
                                 info);
    }
    if (net_split_host_port(host, hostbuf, sizeof(hostbuf), &port) != 0) {
        return -1;
    }
    return net_fetch_http_px(hostbuf, port, path, out, cap, out_len,
                             info);
}

int net_fetch_http(const char *host, const char *path, char *out,
                   size_t cap, size_t *out_len, int *status) {
    net_fetch_info info;
    int rc = net_fetch_http_ex(host, path, out, cap, out_len,
                               &info);
    if (status != NULL) {
        *status = info.status;
    }
    if (rc != 0) {
        return -1;
    }
    return info.status == 200 ? 0 : -1;
}

int net_fetch_https(const char *host, uint16_t port,
                    const char *path, const char *pin_hex,
                    char *fp_out, char *out, size_t cap,
                    size_t *out_len, net_fetch_info *info) {
    if (out_len != NULL) {
        *out_len = 0;
    }
    if (info != NULL) {
        info->status = 0;
        info->location[0] = '\0';
        info->set_cookie[0] = '\0';
    }
    if (out == NULL || cap < 2 || host == NULL || path == NULL) {
        return -1;
    }
    out[0] = '\0';
    static tls_conn tc;
    int rc;
    if (pin_hex != NULL) {
        rc = tls_open(&tc, host, port, pin_hex);
        if (fp_out != NULL) {
            /* ピン留め接続でも指紋は返す (表示用)。 */
            fp_out[0] = '\0';
        }
    } else {
        if (fp_out == NULL) {
            return -1;
        }
        rc = tls_open_probe(&tc, host, port, fp_out);
    }
    if (rc != 0) {
        return -1;
    }
    char req[512];
    size_t p = build_get_request(host, path, req, sizeof(req));
    if (tls_send(&tc, req, p) != 0) {
        tls_close(&tc);
        return -1;
    }
    size_t have = 0;
    int closed_ok = 0;
    uint32_t start = net_now_sec();
    while (net_now_sec() - start < 20 && have + 1 < cap) {
        int n = tls_recv(&tc, (uint8_t *)out + have, cap - 1 - have);
        if (n > 0) {
            have += (size_t)n;
            continue;
        }
        if (n == 0) {
            closed_ok = 1;
            break;
        }
        /* n < 0: エラー。接続が閉じられていれば、受信済みの
         * 分は完全性チェックに回す。 */
        if (tc.peer_closed) {
            closed_ok = 1;
        }
        break;
    }
    tls_close(&tc);
    if (have == 0) {
        return -1;
    }
    out[have] = '\0';
    return process_response(out, have, closed_ok, cap, out_len,
                            info);
}

/* ヘッダの中を小文字で比べながら探す。word が見つかれば
 * その直後の位置、無ければ NULL。 */
static const uint8_t *header_find_ci(const uint8_t *hdr, size_t len,
                                     const char *word) {
    size_t wlen = 0;
    while (word[wlen] != '\0') {
        wlen++;
    }
    if (len < wlen) {
        return NULL;
    }
    for (size_t i = 0; i + wlen <= len; i++) {
        size_t j = 0;
        while (j < wlen) {
            uint8_t a = hdr[i + j];
            if (a >= 'A' && a <= 'Z') {
                a = (uint8_t)(a - 'A' + 'a');
            }
            if (a != (uint8_t)word[j]) {
                break;
            }
            j++;
        }
        if (j == wlen) {
            return hdr + i + wlen;
        }
    }
    return NULL;
}

int net_fetch_http_stream(const char *host, const char *path,
                          int (*cb)(void *ctx, const uint8_t *data,
                                    size_t len),
                          void *ctx, size_t *total_out, int *status) {
    if (total_out != NULL) {
        *total_out = 0;
    }
    if (status != NULL) {
        *status = 0;
    }
    if (host == NULL || path == NULL || cb == NULL) {
        return -1;
    }
    char req[512];
    size_t req_len = build_get_request(host, path, req, sizeof(req));
    /* One static stream: same reason as net_fetch_http. */
    static net_tcp_stream s;
    if (net_tcp_open(host, 80, &s) != 0) {
        return -1;
    }
    if (net_tcp_send(&s, req, req_len) != 0) {
        net_tcp_close(&s);
        return -1;
    }
    uint32_t start = net_now_sec();
    uint32_t deadline = start + 180; /* 全体で 180 秒まで */
    /* Phase 1: ヘッダの終わり (\r\n\r\n) まで 1 バイトずつ読む。 */
    static uint8_t hdr[1024];
    size_t hlen = 0;
    int header_done = 0;
    while (net_now_sec() < deadline && !header_done) {
        while (s.rx_pos < s.rx_len && !header_done) {
            uint8_t ch = s.rxbuf[s.rx_pos++];
            if (hlen + 1 >= sizeof(hdr)) {
                net_tcp_close(&s);
                return -1; /* ヘッダが大きすぎる */
            }
            hdr[hlen++] = ch;
            if (hlen >= 4 && hdr[hlen - 4] == '\r' &&
                hdr[hlen - 3] == '\n' && hdr[hlen - 2] == '\r' &&
                hdr[hlen - 1] == '\n') {
                header_done = 1;
            }
        }
        if (header_done) {
            break;
        }
        if (s.rx_pos >= s.rx_len) {
            s.rx_pos = 0;
            s.rx_len = 0;
        }
        if (s.peer_closed) {
            net_tcp_close(&s);
            return -1; /* ヘッダの前に閉じられた */
        }
        int r = tcp_pump(&s, 1);
        if (r < 0 && s.peer_closed) {
            net_tcp_close(&s);
            return -1;
        }
    }
    if (!header_done) {
        net_tcp_close(&s);
        return -1;
    }
    /* 状態コードと Content-Length を読む。 */
    int code = 0;
    if (hlen >= 12 && hdr[0] == 'H' && hdr[1] == 'T' && hdr[2] == 'T' &&
        hdr[3] == 'P') {
        size_t i = 9;
        while (i < hlen && hdr[i] >= '0' && hdr[i] <= '9') {
            code = code * 10 + (hdr[i] - '0');
            i++;
        }
    }
    if (status != NULL) {
        *status = code;
    }
    if (code != 200) {
        net_tcp_close(&s);
        return -1;
    }
    if (header_find_ci(hdr, hlen, "chunked") != NULL) {
        net_tcp_close(&s);
        return -1; /* チャンク転送には未対応 */
    }
    uint64_t content_len = 0;
    int have_content_len = 0;
    {
        const uint8_t *v =
            header_find_ci(hdr, hlen, "content-length:");
        if (v != NULL) {
            const uint8_t *end = hdr + hlen;
            while (v < end && (*v == ' ' || *v == '\t')) {
                v++;
            }
            uint64_t n = 0;
            int digits = 0;
            while (v < end && *v >= '0' && *v <= '9') {
                n = n * 10 + (uint64_t)(*v - '0');
                v++;
                digits++;
            }
            if (digits > 0) {
                content_len = n;
                have_content_len = 1;
            }
        }
    }
    size_t body_total = 0;
    int rc = -1;
    /* ヘッダと一緒に届いた本体の先頭が受信バッファに残っている。 */
    if (s.rx_pos < s.rx_len) {
        size_t n = s.rx_len - s.rx_pos;
        if (have_content_len && body_total + n > content_len) {
            n = (size_t)(content_len - body_total);
        }
        if (n > 0) {
            if (cb(ctx, s.rxbuf + s.rx_pos, n) != 0) {
                net_tcp_close(&s);
                return -1;
            }
            body_total += n;
            s.rx_pos += n;
        }
    }
    s.rx_pos = 0;
    s.rx_len = 0;
    /* Phase 2: 本体を届ける。長さが分かればその分だけ、
     * 分からなければ相手が閉じるまで。 */
    while (net_now_sec() < deadline) {
        if (have_content_len && body_total >= content_len) {
            rc = 0;
            break;
        }
        if (s.rx_pos < s.rx_len) {
            size_t n = s.rx_len - s.rx_pos;
            if (have_content_len && body_total + n > content_len) {
                n = (size_t)(content_len - body_total);
            }
            if (cb(ctx, s.rxbuf + s.rx_pos, n) != 0) {
                net_tcp_close(&s);
                return -1;
            }
            body_total += n;
            s.rx_pos += n;
            continue;
        }
        s.rx_pos = 0;
        s.rx_len = 0;
        if (s.peer_closed) {
            rc = have_content_len ? -1 : 0;
            break;
        }
        int r = tcp_pump(&s, 1);
        if (r < 0 && s.peer_closed) {
            rc = have_content_len ? -1 : 0;
            break;
        }
    }
    net_tcp_close(&s);
    if (total_out != NULL) {
        *total_out = body_total;
    }
    return rc;
}

void net_status(char *buf, size_t cap) {
    if (buf == NULL || cap == 0) {
        return;
    }
    char ip[16], rt[16], dn[16];
    ip_to_str(net_ip, ip);
    ip_to_str(net_router, rt);
    ip_to_str(net_dns, dn);
    size_t p = 0;
    const char *parts[6];
    parts[0] = "ip=";
    parts[1] = ip;
    parts[2] = " router=";
    parts[3] = rt;
    parts[4] = " dns=";
    parts[5] = dn;
    for (int i = 0; i < 6; i++) {
        const char *s = parts[i];
        while (*s != '\0' && p + 1 < cap) {
            buf[p++] = *s++;
        }
    }
    buf[p] = '\0';
}
