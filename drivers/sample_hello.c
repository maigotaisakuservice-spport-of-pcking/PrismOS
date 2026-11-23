/*
 * 後付けドライバのサンプル。カーネル API 表のシンボルだけを
 * 使う。導入されるとログを出し、動作の証拠として
 * driver-hello.txt を書く。
 *
 * ビルド: gcc -c -fpie -fno-plt -fno-stack-protector
 *         -mno-sse -mno-mmx -mno-80387
 * (ET_REL オブジェクトがパッケージ形式。署名は別ファイル。)
 */
#include <stddef.h>

void prism_drv_log(const char *msg);
int prism_drv_vfs_write(const char *name, const char *data,
                        size_t len);

static const char kText[] = "Hello from a loadable driver.\n";

int prism_driver_init(int api_version) {
    if (api_version != 1) {
        prism_drv_log("sample_hello: unsupported API version");
        return -1;
    }
    prism_drv_log("sample_hello: init");
    if (prism_drv_vfs_write("driver-hello.txt", kText,
                            sizeof(kText) - 1) != 0) {
        prism_drv_log("sample_hello: could not write the file");
        return -2;
    }
    return 0;
}
