/* PrismOS — Prism Store のカーネル側共通処理 (store.h)。
 * 元は kernel/shell.c の store フローから切り出した。 */
#include "store.h"

#include "../shell/mldsa.h"
#include "../shell/storepkg.h"
#include "../shell/vfs.h"
#include "persist.h"
#include "security.h"
#include "serial.h"
#include "shell.h"
#include "string.h"

static void store_err_copy(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (cap == 0) {
        return;
    }
    while (src[i] != '\0' && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}



static uint8_t store_pk[MLDSA44_PK_LEN];
static uint8_t store_payload_buf[PRISM_VFS_MAX_FILE_SIZE];

static int store_load_pub(void) {
    size_t len = 0;
    const char *text = vfs_read_ptr("store.pub", &len);
    if (text == NULL || len != MLDSA44_PK_LEN) {
        return -1;
    }
    memcpy(store_pk, text, MLDSA44_PK_LEN);
    return 0;
}

/* パッケージ (メモリ上) を検証して導入する。
 * 成功したら 0、理由を err に書く。 */
int prism_store_install_mem(const uint8_t *data, size_t len,
                             char *err, size_t errcap) {
    storepkg_info info;
    const uint8_t *sig = NULL;
    const uint8_t *payload = NULL;
    if (storepkg_parse(data, len, &info, &sig, &payload) != 0) {
        store_err_copy(err, errcap, "bad package format");
        return -1;
    }
    if (info.payload_len > sizeof(store_payload_buf)) {
        store_err_copy(err, errcap, "package payload too large");
        return -1;
    }
    if (store_load_pub() != 0) {
        store_err_copy(err, errcap, "no store.pub trust root");
        return -1;
    }
    if (mldsa44_verify(store_pk, payload, info.payload_len, sig) !=
        0) {
        store_err_copy(err, errcap, "signature verification failed");
        return -1;
    }
    memcpy(store_payload_buf, payload, info.payload_len);
    if (vfs_write(info.file, (const char *)store_payload_buf,
                  info.payload_len) != 0) {
        store_err_copy(err, errcap, "could not write the app file");
        return -1;
    }
    /* store-installed.txt を更新する。 */
    char text[2048];
    size_t tlen = 0;
    const char *old = vfs_read_ptr("store-installed.txt", &tlen);
    if (old == NULL) {
        text[0] = '\0';
        tlen = 0;
    } else {
        if (tlen + 1 > sizeof(text)) {
            store_err_copy(err, errcap, "installed list too large");
            return -1;
        }
        memcpy(text, old, tlen);
        text[tlen] = '\0';
    }
    char updated[2048];
    if (store_installed_add(text, tlen, info.name, info.version,
                            info.file, updated,
                            sizeof(updated)) != 0) {
        store_err_copy(err, errcap, "could not update the installed list");
        return -1;
    }
    size_t ulen = 0;
    while (updated[ulen] != '\0') {
        ulen++;
    }
    (void)vfs_write("store-installed.txt", updated, ulen);
    persist_maybe_sync();
    serial_write("store: installed ");
    serial_write(info.name);
    serial_write(" ");
    serial_write_line(info.version);
    audit_log_event("store-install", info.name);
    return 0;
}

int prism_store_uninstall(const char *name) {
    char text[2048];
    size_t tlen = 0;
    const char *old = vfs_read_ptr("store-installed.txt", &tlen);
    if (old == NULL || tlen + 1 > sizeof(text)) {
        return -1;
    }
    memcpy(text, old, tlen);
    text[tlen] = '\0';
    char updated[2048];
    char file[STOREPKG_FILE_MAX + 1];
    if (store_installed_remove(text, tlen, name, updated,
                               sizeof(updated), file,
                               sizeof(file)) != 0) {
        return -1;
    }
    size_t ulen = 0;
    while (updated[ulen] != '\0') {
        ulen++;
    }
    (void)vfs_write("store-installed.txt", updated, ulen);
    (void)vfs_delete(file);
    persist_maybe_sync();
    audit_log_event("store-uninstall", name);
    return 0;
}

/* store.conf: "host port base" (例: 10.0.2.2 8377 /store)。 */
int prism_store_server(char *host, size_t hcap, int *port_out,
                        char *base, size_t bcap) {
    size_t len = 0;
    const char *text = vfs_read_ptr("store.conf", &len);
    if (text == NULL || len == 0) {
        return -1;
    }
    char buf[256];
    if (len + 1 > sizeof(buf)) {
        return -1;
    }
    memcpy(buf, text, len);
    buf[len] = '\0';
    /* 単語を 3 つに割る。 */
    char *words[3];
    int nw = 0;
    char *p = buf;
    while (*p != '\0' && nw < 3) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        words[nw++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\n' && *p != '\r' &&
               *p != '\t') {
            p++;
        }
        if (*p != '\0') {
            *p++ = '\0';
        }
    }
    if (nw < 2) {
        return -1;
    }
    size_t i = 0;
    while (words[0][i] != '\0' && i + 1 < hcap) {
        host[i] = words[0][i];
        i++;
    }
    host[i] = '\0';
    int port = 0;
    const char *q = words[1];
    while (*q >= '0' && *q <= '9') {
        port = port * 10 + (*q - '0');
        q++;
    }
    if (port <= 0 || port > 65535) {
        return -1;
    }
    *port_out = port;
    if (nw >= 3) {
        i = 0;
        while (words[2][i] != '\0' && i + 1 < bcap) {
            base[i] = words[2][i];
            i++;
        }
        base[i] = '\0';
    } else if (bcap > 1) {
        base[0] = '/';
        base[1] = '\0';
    }
    return 0;
}

