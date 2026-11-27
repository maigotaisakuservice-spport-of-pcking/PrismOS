/* PrismOS — 自前の GDT と TSS
 *
 * 起動直後はブートローダ (Limine) の GDT を使っているが、
 * そこにはユーザーモード (リング 3) 用のセグメントも TSS も
 * 無い。互換レイヤーのゲストをリング 3 で隔離して動かす
 * (サンドボックス) ため、カーネルコード/データは Limine と
 * 同じセレクタ配置のまま、自前の GDT に載せ替える:
 *
 *   0x00 ヌル
 *   0x08 16 ビットコード (Limine と同じ位置)
 *   0x10 16 ビットデータ
 *   0x18 32 ビットコード
 *   0x20 32 ビットデータ
 *   0x28 64 ビットカーネルコード (IDT のセレクタと一致)
 *   0x30 カーネルデータ
 *   0x38 ユーザーデータ   (DPL 3)
 *   0x40 64 ビットユーザーコード (DPL 3)
 *   0x48 TSS
 *
 * TSS の RSP0 は、リング 3 から割り込み/例外で入るときに
 * CPU が切り替える先のカーネルスタック。ゲスト実行中は
 * int $0x80 も例外も全部ここに載る。
 */
#ifndef PRISM_GDT_H
#define PRISM_GDT_H

#define GDT_KERNEL_CODE 0x28
#define GDT_KERNEL_DATA 0x30
#define GDT_USER_DATA 0x38
#define GDT_USER_CODE 0x40
#define GDT_TSS 0x48

/* GDT を自前に載せ替え、TSS を読み込む。kmain の早い段階で
 * 1 回だけ呼ぶ。 */
void gdt_init(void);
void gdt_load_existing(void);

#endif /* PRISM_GDT_H */
