/* PrismOS — 公式ロゴの埋め込み (明るい版)
 *
 * kernel/logo.bmp は assets/logo-light.png (公式ロゴの明るい版、
 * Canva デザイン DAHbf4AwTkU) の中央部分を 640px 幅の 24 ビット
 * BMP にしたもの。カーネルに埋め込み、起動時に main.c が白い画面
 * の中央に描く。暗い版は不採用のため使わない。
 */
    .section .rodata.logo, "a"
    .globl prism_logo_data
    .globl prism_logo_end
prism_logo_data:
    .incbin "kernel/logo.bmp"
prism_logo_end:
