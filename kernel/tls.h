/* PrismOS - TLS 1.2 client (see tls.c for the exact
 * scope and the certificate verification policy). */
#ifndef PRISM_TLS_H
#define PRISM_TLS_H

#include <stddef.h>
#include <stdint.h>

#include "netstack.h"

typedef struct {
    net_tcp_stream tcp;
    int secure; /* handshake completed, records encrypted */
    int send_secure; /* internal: CCS sent, encrypt output */
    int recv_secure; /* internal: CCS seen, decrypt input */
    uint8_t client_key[16];
    uint8_t server_key[16];
    uint8_t client_iv[4]; /* GCM fixed nonce parts */
    uint8_t server_iv[4];
    uint64_t send_seq;
    uint64_t recv_seq;
    uint8_t plain[4096]; /* decrypted bytes not yet consumed */
    size_t plain_len;
    size_t plain_pos;
    int peer_closed;
    char err[96]; /* last failure, for the serial log */
} tls_conn;

/* Open a TCP connection to host:port and run the TLS 1.2
 * handshake. pin_hex is the expected SHA-256 fingerprint
 * of the server's certificate as 64 hex characters
 * (certificate pinning; a connection without a pin, or
 * with a mismatching certificate, is refused). Returns 0
 * on success; on failure tls_last_error() says why. */
int tls_open(tls_conn *c, const char *host, uint16_t port,
             const char *pin_hex);

/* Like tls_open but without a pin: the handshake runs and
 * the server certificate's SHA-256 fingerprint (64 hex
 * chars + NUL) is written to fp_hex_out. The caller MUST
 * check the fingerprint against its own trust store
 * before sending application data (the browser uses
 * trust-on-first-use). Returns 0 on success. */
int tls_open_probe(tls_conn *c, const char *host, uint16_t port,
                   char *fp_hex_out);

/* Send application data (encrypted). Returns 0 on success. */
int tls_send(tls_conn *c, const char *data, size_t len);

/* Read raw application bytes (stream semantics like
 * net_tcp_recv): returns the number of bytes copied
 * (possibly less than cap), 0 on an orderly close,
 * -1 on error. */
int tls_recv(tls_conn *c, uint8_t *out, size_t cap);

/* Read one line from the decrypted stream, with the same
 * contract as net_tcp_recv_line. */
int tls_recv_line(tls_conn *c, char *out, size_t cap,
                  uint32_t timeout_sec);

/* Send close_notify and close the TCP connection. */
void tls_close(tls_conn *c);

const char *tls_last_error(const tls_conn *c);

/* ---- Pure helpers, exposed for the host unit tests ---- */

/* out = in^exp mod mod (all big-endian, mod_len bytes).
 * Returns 0 on success. Modulus up to 512 bytes (4096 bit). */
int tls_rsa_public(const uint8_t *mod, size_t mod_len,
                   const uint8_t *exp, size_t exp_len,
                   const uint8_t *in, uint8_t *out);

/* Find the RSA public key inside an X.509 certificate
 * (DER). On success mod/exp point into der. Returns 0
 * on success. */
int tls_x509_rsa_key(const uint8_t *der, size_t len,
                     const uint8_t **mod, size_t *mod_len,
                     const uint8_t **exp, size_t *exp_len);

/* TLS 1.2 PRF (P_SHA256). */
void tls_prf(const uint8_t *secret, size_t secret_len,
             const char *label, const uint8_t *seed,
             size_t seed_len, uint8_t *out, size_t out_len);

/* AES-128-GCM authenticated encryption. nonce is 12
 * bytes. The tag is 16 bytes. Decryption returns 0 only
 * when the tag matches. */
void tls_gcm_encrypt(const uint8_t key[16],
                     const uint8_t nonce[12], const uint8_t *pt,
                     size_t len, const uint8_t *aad,
                     size_t aad_len, uint8_t *ct,
                     uint8_t tag[16]);
int tls_gcm_decrypt(const uint8_t key[16],
                    const uint8_t nonce[12], const uint8_t *ct,
                    size_t len, const uint8_t *aad,
                    size_t aad_len, const uint8_t tag[16],
                    uint8_t *pt);

#endif /* PRISM_TLS_H */
