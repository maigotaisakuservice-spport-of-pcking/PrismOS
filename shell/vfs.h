/* PrismOS — 仮想ファイルシステム (RAM 上のファイル)
 *
 * 設計 (本物の実装):
 *   * ファイルの中身は、呼び出し側が渡すアロケータで確保する。
 *     カーネル側では kmalloc/kfree (PMM の上に載るヒープ) を
 *     渡し、ファイルが本当にメモリ管理の上に載る。固定の
 *     内容プールは使わない。ファイル数の上限 (128 件) と
 *     1 ファイルの上限 (16384 バイト) は、inode 数の上限と
 *     同じ意味の設計上の上限で、仕様として明記する。
 *   * ファイル名・大きさ・使用中かどうかの表 (128 件分) だけは
 *     静的な表に置く。これは inode 表に相当する。
 *   * ディスク書き込み (永続化) は shell/diskfs.c と
 *     kernel/persist.c が行う。インストール済みなら
 *     ファイルは ATA ディスクに残り、再起動後も読める。
 *   * vfs_init() で組み込みの 36 ファイル (README.txt、VERSION、
 *     theme.toml、about.txt、prism.bmp、melody.wav、vid-01〜24.pv1、
 *     doc.txt、slides.txt、hello.elf・calc.elf・cat.elf・unsup.elf)
 *     を入れる。vfs_write() で新規作成・上書き、vfs_delete() で
 *     削除ができる。
 *   * vfs_read_ptr() は読み取り専用の参照を返す。返された
 *     ポインタは次の書き込み・削除まで有効。
 *   * アロケータを設定する前に vfs_write() を呼ぶと -4 を返す。
 *     vfs_init() より前に vfs_set_allocator() を呼ぶこと。
 */
#ifndef PRISM_VFS_H
#define PRISM_VFS_H

#include <stddef.h>

#define PRISM_VFS_MAX_FILES 128
#define PRISM_VFS_MAX_FILE_SIZE 16384
#define PRISM_VFS_MAX_NAME 31

typedef void *(*vfs_alloc_fn)(size_t size);
typedef void (*vfs_free_fn)(void *ptr);

/* ファイル内容の確保・解放に使うアロケータを登録する。
 * カーネル側では kmalloc/kfree、ホストテストでは malloc/free
 * を渡す。vfs_init() より前に呼ぶこと。 */
void vfs_set_allocator(vfs_alloc_fn alloc, vfs_free_fn free_fn);

/* 組み込みファイルで初期化する (既存のファイルは消える)。
 * アロケータ未設定の場合は何もせず、ファイル数は 0 のまま。 */
void vfs_init(void);

int vfs_file_count(void);
/* index 番目のファイル名。範囲外なら NULL。 */
const char *vfs_file_name(int index);

/* 更新時刻 (Unix 秒)。時刻の源が登録されていれば
 * vfs_write のたびに記録される。未記録は 0。 */
void vfs_set_time_source(uint32_t (*fn)(void));
uint32_t vfs_file_mtime(int index);
void vfs_set_mtime(const char *name, uint32_t t);
int vfs_exists(const char *name);

/* 読み取り専用の参照と大きさを返す。無い場合は NULL。 */
const char *vfs_read_ptr(const char *name, size_t *out_len);

/* 新規作成または上書きする。成功したら 0。
 * -1: 内容が大きすぎる、-2: ファイル数が上限、-3: 名前が不正、
 * -4: アロケータ未設定またはメモリ枯渇。 */
int vfs_write(const char *name, const char *data, size_t len);

/* 削除する。成功したら 0、無ければ -1。 */
int vfs_delete(const char *name);

/* 組み込みファイルだけを正しい中身に戻す (回復モードの修復用)。
 * 無いものは作り、中身が違うものは書き直す。ユーザーのファイルは
 * 一切触らない。直したファイルの数を返す。アロケータ未設定の
 * 場合は -1。 */
int vfs_repair_builtins(void);

/* 組み込みファイルかどうか。1 = 組み込み、0 = ユーザーのファイル。 */
int vfs_is_builtin(const char *name);

/* ファイルが変わった回数。書き込み・削除・初期化のたびに
 * 増える。ディスクへの書き戻しが、変わったときだけ動く
 * ための目安。 */
unsigned long vfs_change_seq(void);

#endif /* PRISM_VFS_H */
