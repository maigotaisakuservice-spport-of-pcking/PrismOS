/* PrismOS - network stack (DHCP / DNS / TCP / HTTP).
 *
 * Runs on top of the e1000 driver, polling only. There is
 * no background task: each call drives the card until it
 * gets its answer or times out, then returns.
 *
 *   net_init()      - find the card, wait for link, DHCP.
 *   net_ready()     - 1 when an address was configured.
 *   net_fetch_http() - fetch one plain HTTP page (port 80,
 *     no encryption) and copy its body into out.
 *   net_tcp_open() / net_tcp_send() / net_tcp_recv_line() /
 *     net_tcp_close() - one line-oriented TCP connection to
 *     any port (used by the mail client for SMTP/POP3).
 *     The TCP layer is port-generic: the peer port is part
 *     of the connection, not hard-coded to 80.
 *   net_status()    - write "ip=... router=... dns=..." into
 *     buf for the shell to print.
 *
 * Timeouts are in whole seconds, measured with the real
 * time clock, so a lost packet cannot hang the shell.
 */
#ifndef PRISM_NETSTACK_H
#define PRISM_NETSTACK_H

#include <stddef.h>
#include <stdint.h>

/* Remember the bootloader HHDM offset (set at boot, before
 * any network use). */
void net_set_hhdm(uint64_t hhdm_offset);

/* Configure the network on first use, using the stored
 * offset. Returns 0 on success. */
int net_ensure(void);

/* Bring up just the NIC (probe + link), without any IPv4
 * address configuration. IPv6 link-local needs only the
 * MAC address, so it uses this and works on networks with
 * no DHCP server. Returns 0 on success. */
int net_ensure_nic(void);

/* Find the card and configure an address with DHCP.
 * Returns 0 on success. Safe to call again: a second call
 * does nothing when already configured. */
int net_init(uint64_t hhdm_offset);
int net_ready(void);

/* Resolve a host name (or dotted IPv4) to an address.
 * Returns 0 on success. */
int net_resolve(const char *host, uint8_t *ip_out);

/* ICMP echo。0 = 応答あり、-1 = タイムアウト/失敗、
 * -2 = ファイアウォールで遮断。 */
int net_ping(const uint8_t *dst_ip, uint16_t seq,
             uint32_t timeout_sec);

/* ファイアウォール (遮断 IP の一覧)。 */
void net_fw_clear(void);
int net_fw_add_drop(const uint8_t *ip);
int net_fw_count(void);
int net_fw_get(int idx, uint8_t *ip_out);

/* One-shot UDP request/response (NTP-style): send one
 * datagram to dst_ip:dst_port from src_port and wait
 * for one reply. Returns the reply length or -1. */
int net_udp_exchange(const uint8_t *dst_ip,
                     uint16_t dst_port, uint16_t src_port,
                     const uint8_t *payload, size_t payload_len,
                     uint8_t *out, size_t cap,
                     uint32_t timeout_sec);

/* Our configured addresses (wire order). Valid after
 * net_ready() is true. */
void net_my_ip(uint8_t *out);
void net_my_router(uint8_t *out);
void net_my_dns(uint8_t *out);

/* 固定 IP 設定。"192.168.0.2" 形式を解析する
 * (成功 0)。net_set_static_config は DHCP の代わりに
 * 使う設定を登録し、初期化済みなら即時適用する。
 * net_clear_static_config は DHCP に戻す (適用は
 * 次回の初期化時)。 */
int net_parse_ipv4(const char *s, uint8_t out[4]);
void net_set_static_config(const uint8_t ip[4],
                           const uint8_t mask[4],
                           const uint8_t gw[4],
                           const uint8_t dns[4]);
void net_clear_static_config(void);
int net_static_active(void);

/* One line-oriented TCP connection. The receive buffer
 * holds bytes that arrived but were not consumed yet, so a
 * line split across segments still comes out whole.
 * Keep instances static (the buffer is 4KB): the kernel
 * stack is only 32KB and deep call chains already use
 * much of it. */
#define NET_TCP_RXBUF 4096

/* How many TCP connections can be open (registered for
 * receive dispatch) at the same time. */
#define NET_TCP_MAX_STREAMS 4

typedef struct {
    uint32_t my_seq;
    uint32_t peer_seq;
    uint32_t last_ack; /* last ACK number seen from the peer */
    uint16_t my_port;
    uint16_t peer_port;
    uint8_t peer_ip[4];
    /* IPv6 接続のとき 1。その場合 peer_ip ではなくこちらを
     * 使う: 相手アドレス、送信元アドレス (接続/受付時に
     * 決めたもの)、次ホップの MAC アドレス。 */
    int v6;
    uint8_t peer_ip6[16];
    uint8_t src_ip6[16];
    uint8_t peer_hw6[6];
    int connected;
    int peer_closed; /* the peer sent FIN */
    int rst_seen; /* an RST arrived (possibly while another
                     connection was being waited on) */
    uint8_t rxbuf[NET_TCP_RXBUF];
    size_t rx_len; /* bytes stored in rxbuf */
    size_t rx_pos; /* bytes already consumed */
} net_tcp_stream;

/* Open a TCP connection to host (dotted decimal or name via
 * DNS) on the given port. Returns 0 on success. */
int net_tcp_open(const char *host, uint16_t port, net_tcp_stream *s);

/* Send len bytes, retransmitting on timeout. Returns 0
 * when the peer ACKed all of them. */
int net_tcp_send(net_tcp_stream *s, const char *data, size_t len);

/* Read one line (up to and including '\n', CRLF stripped)
 * into out (NUL terminated, at most cap-1 chars). Returns
 * the line length (>= 0), or -1 on timeout, close, or
 * error. A line longer than the buffer is consumed and
 * truncated, not left half-read. */
int net_tcp_recv_line(net_tcp_stream *s, char *out, size_t cap,
                      uint32_t timeout_sec);

/* Read raw bytes (the TLS layer sits on this). Copies up
 * to cap buffered bytes into out, waiting up to
 * timeout_sec for at least one byte when the buffer is
 * empty. Returns the byte count (> 0), 0 when the peer
 * closed and no data remains, or -1 on timeout/error. */
int net_tcp_recv(net_tcp_stream *s, uint8_t *out, size_t cap,
                 uint32_t timeout_sec);

/* Send FIN and forget the connection. */
void net_tcp_close(net_tcp_stream *s);

/* Server side: wait up to timeout_sec for a connection
 * to the given port and accept it into s (which the
 * caller provides). Returns 0 on success. Up to
 * NET_TCP_MAX_STREAMS connections can be open at once:
 * while any one connection is being waited on, segments
 * for the others are folded into their receive buffers
 * instead of being dropped. Call again for the next
 * connection. */
int net_tcp_listen(uint16_t port, uint32_t timeout_sec,
                   net_tcp_stream *s);

/* Fetch http://host/path over plain HTTP. The body is
 * copied into out (at most cap-1 bytes) and NUL terminated.
 * The host may carry an explicit port ("host:port",
 * default 80). Returns 0 on HTTP 200, else -1. *out_len
 * gets the body length, *status gets the HTTP status code
 * (0 if unknown). */
int net_fetch_http(const char *host, const char *path, char *out,
                   size_t cap, size_t *out_len, int *status);

/* Extra fetch results: the HTTP status code and the
 * Location header (for redirects), if any. set_cookie
 * holds every Set-Cookie header value, '\n'-separated
 * (attributes included; the caller parses pairs). */
typedef struct {
    int status;
    char location[256];
    char set_cookie[512];
} net_fetch_info;

/* Set the Cookie header value ("name=value; ...") sent
 * with subsequent fetches, or NULL/"" to send none.
 * The browser's cookie jar drives this. */
void net_set_fetch_cookie(const char *cookie);

/* Like net_fetch_http but returns 0 for any complete
 * response (redirects included) and fills *info.
 * Chunked transfer encoding is decoded. */
int net_fetch_http_ex(const char *host, const char *path, char *out,
                      size_t cap, size_t *out_len,
                      net_fetch_info *info);

/* net_fetch_http_ex with an explicit port. */
/* Fetch via an HTTP proxy (plain HTTP only): the
 * request goes to the proxy with an absolute URI. */
int net_fetch_http_via(const char *proxy_host, uint16_t proxy_port,
                       const char *host, const char *path,
                       char *out, size_t cap, size_t *out_len,
                       net_fetch_info *info);

int net_fetch_http_px(const char *host, uint16_t port,
                      const char *path, char *out, size_t cap,
                      size_t *out_len, net_fetch_info *info);

/* Fetch https://host:port/path with the TLS client.
 * pin_hex is the expected certificate fingerprint (64
 * hex): the handshake then refuses any other certificate
 * before the request is sent. pin_hex == NULL is the
 * trust-on-first-use probe for a host with no stored
 * pin: the handshake runs, the actual fingerprint is
 * written to fp_out (65 bytes), and the caller stores
 * it as the pin for future visits (and passes it as
 * pin_hex from then on). Returns 0 for any complete
 * response; *info is filled like net_fetch_http_ex. */
int net_fetch_https(const char *host, uint16_t port,
                    const char *path, const char *pin_hex,
                    char *fp_out, char *out, size_t cap,
                    size_t *out_len, net_fetch_info *info);

/* Decode a chunked transfer-encoded body in place.
 * Returns 0 and *out_len on success, -1 if malformed
 * or truncated. */
int net_http_decode_chunked(uint8_t *buf, size_t len,
                            size_t *out_len);

/* Fetch http://host/path, delivering the body to cb() in
 * arrival order as it comes off the wire (a few KB per call,
 * pointing into the stream's receive buffer: cb must consume
 * the bytes synchronously). Returns 0 on HTTP 200 with the
 * complete body (Content-Length bytes, or until the peer
 * closes when the length is unknown), else -1. *total_out
 * gets the body byte count, *status the HTTP status (0 if
 * unknown). cb returning non-zero aborts the fetch.
 * Chunked transfer encoding is not supported (returns -1).
 * A whole-file fetch does not fit a multi-MB kernel image,
 * so the kernel update uses this to stream the image
 * straight onto the boot partition. */
int net_fetch_http_stream(const char *host, const char *path,
                          int (*cb)(void *ctx, const uint8_t *data,
                                    size_t len),
                          void *ctx, size_t *total_out, int *status);

/* Send one DNS query for example.com to the given server
 * (dotted decimal) and report the answer. Returns 0 when an
 * address was resolved. Diagnostic only. */
int net_dns_test(const char *server);

/* ---- IPv6 ----
 * Link-local (EUI-64) is derived from the NIC. Global
 * configuration uses Router Solicitation / Advertisement
 * (SLAAC). Neighbor Discovery (NS/NA) resolves link-layer
 * addresses; incoming solicitations for our addresses are
 * answered inside the receive pump. */

/* Derive the link-local address and, on first call, ask
 * for a Router Advertisement. Returns 0 when a router
 * answered (a global address may then exist). */
int net6_configure(void);
int net6_ready(void);
int net6_have_global_addr(void);
void net6_my_ll(uint8_t *out);
void net6_my_global(uint8_t *out);

/* ICMPv6 echo. 0 = reply received, -1 = timeout/failure. */
int net6_ping6(const uint8_t *dst, uint16_t seq,
               uint32_t timeout_sec);

/* One-shot UDP request/response over IPv6. Returns the
 * reply length or -1. */
int net6_udp_exchange(const uint8_t *dst, uint16_t dst_port,
                      uint16_t src_port, const uint8_t *payload,
                      size_t payload_len, uint8_t *out, size_t cap,
                      uint32_t timeout_sec);

/* TCP over IPv6: open a connection to dst:port. The stream
 * is marked v6; net_tcp_send / net_tcp_recv /
 * net_tcp_recv_line / net_tcp_close then work exactly as
 * for IPv4. Returns 0 on success. */
int net6_tcp_open(const uint8_t dst[16], uint16_t port,
                  net_tcp_stream *s);

/* TCP over IPv6, server side: wait up to timeout_sec for a
 * connection to the given port (on any of our IPv6
 * addresses) and accept it into s. Returns 0 on success. */
int net6_tcp_listen(uint16_t port, uint32_t timeout_sec,
                    net_tcp_stream *s);

/* Multi-line status text ("ipv6 link-local ...") into buf. */
void net6_status(char *buf, size_t cap);

/* One-line status text into buf (NUL terminated). */
void net_status(char *buf, size_t cap);

#endif /* PRISM_NETSTACK_H */
