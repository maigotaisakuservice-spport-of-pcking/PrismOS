/* PrismOS — 壁紙画像の埋め込み (Canva で作った壁紙を BMP 化したもの)
 *
 * kernel/wallpaper.bmp は assets/wallpaper.png (1920x1080) を
 * 1280x800 の 24 ビット BMP に変換したもの。カーネルに埋め込み、
 * wallpaper.c が BMP として解析して画面に描く。
 */
    .section .rodata.wallpaper, "a"
    .globl prism_wallpaper_data
    .globl prism_wallpaper_end
prism_wallpaper_data:
    .incbin "kernel/wallpaper.bmp"
prism_wallpaper_end:
