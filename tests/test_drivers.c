/* PrismOS — ドライバのホストテスト。
 * AHCI の FIS・PRDT・IDENTIFY 解析、virtio-blk のヘッダ・
 * キュー配置・記述子、UHCI の設定パケット・TD トークン・
 * 機器記述子解析を確かめる。ハードウェアには触らない。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ahci.h"
#include "ehci.h"
#include "xhci.h"
#include "uhci.h"
#include "virtio_blk.h"

static int checks = 0;
static int failures = 0;

static void check(int cond, const char *name) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", name);
    }
}

int main(void) {
    /* ---- AHCI: コマンド一覧の DW0 ---- */
    {
        check(ahci_cmd_header_flags(0, 1) == 0x00010005u,
              "ahci cmd flags read prdtl 1");
        check(ahci_cmd_header_flags(1, 2) == 0x00020045u,
              "ahci cmd flags write prdtl 2");
        check(ahci_cmd_header_flags(0, 0) == 0x00000005u,
              "ahci cmd flags prdtl 0");
    }
    /* ---- AHCI: Register FIS ---- */
    {
        uint8_t fis[20];
        ahci_build_reg_fis(fis, 0x25, 0x123456789ABULL, 1);
        check(fis[0] == 0x27, "ahci fis type");
        check(fis[1] == 0x80, "ahci fis command bit");
        check(fis[2] == 0x25, "ahci fis read dma ext");
        check(fis[3] == 0x00, "ahci fis features");
        check(fis[4] == 0xAB && fis[5] == 0x89 && fis[6] == 0x67,
              "ahci fis lba low");
        check(fis[7] == 0x40, "ahci fis device lba");
        check(fis[8] == 0x45 && fis[9] == 0x23 && fis[10] == 0x01,
              "ahci fis lba high");
        check(fis[12] == 1 && fis[13] == 0, "ahci fis count");
        check(fis[19] == 0, "ahci fis reserved zero");
        ahci_build_reg_fis(fis, 0xEC, 0, 1);
        check(fis[2] == 0xEC, "ahci fis identify");
    }
    /* ---- AHCI: PRDT 項目 ---- */
    {
        uint8_t e[16];
        ahci_prdt_entry(e, 0x1000, 512);
        check(e[0] == 0x00 && e[1] == 0x10 && e[2] == 0 && e[3] == 0,
              "ahci prdt addr low");
        check(e[4] == 0 && e[5] == 0 && e[6] == 0 && e[7] == 0,
              "ahci prdt addr high");
        check(e[12] == 0xFF && e[13] == 0x01 && e[14] == 0 && e[15] == 0,
              "ahci prdt byte count 511");
        ahci_prdt_entry(e, 0x1234567890ULL, 4096);
        check(e[0] == 0x90 && e[1] == 0x78 && e[2] == 0x56 &&
                  e[3] == 0x34 && e[4] == 0x12,
              "ahci prdt 64-bit addr");
        check(e[12] == 0xFF && e[13] == 0x0F, "ahci prdt 4095");
    }
    /* ---- AHCI: IDENTIFY 解析 ---- */
    {
        uint16_t id[256];
        memset(id, 0, sizeof(id));
        /* モデル名 "QEMU HARDDISK" (ワード 27-46、バイト順を
         * 入れ替えた形で置く)。 */
        const char *model = "QEMU HARDDISK";
        for (int i = 0; i < 20; i++) {
            uint8_t hi = (uint8_t)(i * 2 < 13 ? model[i * 2] : ' ');
            uint8_t lo = (uint8_t)(i * 2 + 1 < 13 ? model[i * 2 + 1] : ' ');
            id[27 + i] = (uint16_t)((hi << 8) | lo);
        }
        id[100] = 0x0000;
        id[101] = 0x0001; /* LBA48 = 65536 */
        id[102] = 0;
        id[103] = 0;
        id[60] = 0x8000; /* LBA28 の値 (使われないはず) */
        id[61] = 0x0000;
        char out[41];
        uint64_t sectors = 0;
        ahci_parse_identify(id, out, sizeof(out), &sectors);
        check(strcmp(out, "QEMU HARDDISK") == 0, "ahci identify model");
        check(sectors == 65536, "ahci identify lba48 sectors");
        /* LBA48 が 0 なら LBA28 を使う。 */
        id[100] = 0;
        id[101] = 0;
        id[60] = 0x8000;
        id[61] = 0x0000;
        ahci_parse_identify(id, out, sizeof(out), &sectors);
        check(sectors == 32768, "ahci identify lba28 fallback");
    }
    /* ---- virtio-blk: 要求ヘッダ ---- */
    {
        uint8_t hdr[16];
        virtio_blk_build_header(hdr, 0, 42);
        check(hdr[0] == 0 && hdr[1] == 0 && hdr[2] == 0 && hdr[3] == 0,
              "virtio header read type");
        check(hdr[8] == 42 && hdr[9] == 0, "virtio header sector 42");
        virtio_blk_build_header(hdr, 1, 0x100000001ULL);
        check(hdr[0] == 1, "virtio header write type");
        check(hdr[8] == 1 && hdr[12] == 1, "virtio header sector 64-bit");
    }
    /* ---- virtio-blk: キュー配置 ---- */
    {
        uint32_t desc, avail, used, total;
        virtio_queue_layout(128, &desc, &avail, &used, &total);
        check(desc == 2048, "virtio layout desc 128");
        check(avail == 2048, "virtio layout avail 128");
        check(used == 4096, "virtio layout used 128");
        check(total == 4096 + 6 + 128 * 8, "virtio layout total 128");
        virtio_queue_layout(256, &desc, &avail, &used, &total);
        check(desc == 4096, "virtio layout desc 256");
        check(used == 8192, "virtio layout used 256");
        check(total == 8192 + 6 + 256 * 8, "virtio layout total 256");
    }
    /* ---- virtio-blk: 記述子 ---- */
    {
        uint8_t d[16];
        virtio_desc(d, 0x2000, 512, 3, 2);
        check(d[0] == 0x00 && d[1] == 0x20, "virtio desc addr");
        check(d[8] == 0x00 && d[9] == 0x02, "virtio desc len 512");
        check(d[12] == 3, "virtio desc flags next+write");
        check(d[14] == 2, "virtio desc next");
        virtio_desc(d, 0x1234567890ULL, 1, 2, 0);
        check(d[0] == 0x90 && d[4] == 0x12, "virtio desc 64-bit addr");
        check(d[12] == 2, "virtio desc write only");
    }
    /* ---- UHCI: 設定パケット ---- */
    {
        uint8_t pkt[8];
        uhci_build_setup(pkt, 0x80, 0x06, 0x0100, 0, 18);
        check(pkt[0] == 0x80 && pkt[1] == 0x06, "uhci setup get desc");
        check(pkt[2] == 0x00 && pkt[3] == 0x01, "uhci setup value");
        check(pkt[6] == 18 && pkt[7] == 0, "uhci setup length");
        uhci_build_setup(pkt, 0x00, 0x05, 0x0001, 0, 0);
        check(pkt[0] == 0x00 && pkt[1] == 0x05 && pkt[2] == 0x01,
              "uhci setup set address");
    }
    /* ---- UHCI: TD トークン ---- */
    {
        uint32_t t = uhci_td_token(0x2D, 0, 0, 0, 8);
        check(t == 0x00E0002Du, "uhci token setup");
        t = uhci_td_token(0x69, 1, 0, 1, 8);
        check(t == 0x00E80169u, "uhci token in toggle");
        t = uhci_td_token(0xE1, 2, 1, 0, 64);
        check(t == 0x07E082E1u, "uhci token out ep1");
    }
    /* ---- UHCI: 機器記述子解析 ---- */
    {
        uint8_t d[18];
        memset(d, 0, sizeof(d));
        d[4] = 0; /* 機器種別 */
        d[7] = 8; /* 最大パケット */
        d[8] = 0x27;
        d[9] = 0x06; /* 売主 0x0627 */
        d[10] = 0x01;
        d[11] = 0x00; /* 製品 0x0001 */
        uhci_device info;
        memset(&info, 0, sizeof(info));
        uhci_parse_device_desc(d, &info);
        check(info.vendor == 0x0627, "uhci parse vendor");
        check(info.product == 0x0001, "uhci parse product");
        check(info.max_packet == 8, "uhci parse max packet");
        check(info.dev_class == 0, "uhci parse class");
    }
    /* ---- UHCI: TD 状態 ---- */
    {
        check(uhci_td_ok(0) == 1, "uhci td ok empty");
        check(uhci_td_ok(0x123) == 1, "uhci td ok actual len");
        check(uhci_td_ok(1u << 23) == 0, "uhci td active not ok");
        check(uhci_td_ok(1u << 22) == 0, "uhci td stalled not ok");
        check(uhci_td_ok(1u << 18) == 0, "uhci td crc error not ok");
        check(uhci_td_ok(1u << 19) == 0, "uhci td nak not ok");
    }
    /* ---- UHCI: 構成記述子の解析 (HID キーボード) ---- */
    {
        /* QEMU usb-kbd 相当: 構成(9) + 接続面 HID boot
         * keyboard(9) + HID(9) + 終端 interrupt IN(7)。 */
        uint8_t cfg[34];
        memset(cfg, 0, sizeof(cfg));
        cfg[0] = 9; cfg[1] = 2; cfg[2] = 34; cfg[5] = 1;
        cfg[9] = 9; cfg[10] = 4; cfg[11] = 0; cfg[12] = 0;
        cfg[13] = 1; cfg[14] = 3; cfg[15] = 1; cfg[16] = 1;
        cfg[18] = 9; cfg[19] = 0x21;
        cfg[27] = 7; cfg[28] = 5; cfg[29] = 0x81; cfg[30] = 3;
        cfg[31] = 8;
        uhci_device info;
        memset(&info, 0, sizeof(info));
        check(uhci_parse_config(cfg, sizeof(cfg), &info) == 0,
              "uhci cfg kbd parsed");
        check(info.config_value == 1, "uhci cfg value");
        check(info.kind == UHCI_KIND_HID_KEYBOARD, "uhci cfg kind kbd");
        check(info.hid_ep == 1 && info.hid_max_packet == 8,
              "uhci cfg hid ep");
    }
    /* ---- UHCI: 構成記述子の解析 (マスストレージ) ---- */
    {
        uint8_t cfg[32];
        memset(cfg, 0, sizeof(cfg));
        cfg[0] = 9; cfg[1] = 2; cfg[2] = 32; cfg[5] = 1;
        cfg[9] = 9; cfg[10] = 4; cfg[11] = 0; cfg[13] = 2;
        cfg[14] = 8; cfg[15] = 6; cfg[16] = 0x50;
        cfg[18] = 7; cfg[19] = 5; cfg[20] = 0x81; cfg[21] = 2;
        cfg[22] = 64;
        cfg[25] = 7; cfg[26] = 5; cfg[27] = 0x02; cfg[28] = 2;
        cfg[29] = 64;
        uhci_device info;
        memset(&info, 0, sizeof(info));
        check(uhci_parse_config(cfg, sizeof(cfg), &info) == 0,
              "uhci cfg storage parsed");
        check(info.kind == UHCI_KIND_STORAGE, "uhci cfg kind storage");
        check(info.bulk_in_ep == 1 && info.bulk_out_ep == 2,
              "uhci cfg bulk eps");
        check(info.bulk_max_packet == 64, "uhci cfg bulk mp");
        check(uhci_parse_config(cfg, 5, &info) == -1,
              "uhci cfg short rejected");
    }
    /* ---- UHCI: HID キー変換 ---- */
    {
        check(uhci_hid_key_code(0x04, 0, 0) == 'a', "hid key a");
        check(uhci_hid_key_code(0x04, 1, 0) == 'A', "hid key A");
        check(uhci_hid_key_code(0x1E, 0, 0) == '1', "hid key 1");
        check(uhci_hid_key_code(0x1E, 1, 0) == '!', "hid key !");
        check(uhci_hid_key_code(0x28, 0, 0) == '\n', "hid key enter");
        check(uhci_hid_key_code(0x2A, 0, 0) == '\b', "hid key bs");
        check(uhci_hid_key_code(0x12, 0, 1) == 0xA1, "hid key ctrl-o");
        check(uhci_hid_key_code(0x05, 0, 1) == 0xB0 + 1,
              "hid key ctrl-b base");
        check(uhci_hid_key_code(0x3A, 0, 0) == 0x81, "hid key F1");
        check(uhci_hid_key_code(0x45, 0, 0) == 0x8C, "hid key F12");
        check(uhci_hid_key_code(0x52, 0, 0) == 0x90, "hid key up");
        check(uhci_hid_key_code(0x00, 0, 0) == -1, "hid key none");
    }
    /* ---- UHCI: CBW 組み立て ---- */
    {
        uint8_t cbw[31];
        uint8_t cdb[10] = {0x28, 0, 0, 0, 0, 100, 0, 0, 1, 0};
        uhci_build_cbw(cbw, 0x11223344, 512, 1, cdb, 10);
        check(cbw[0] == 0x55 && cbw[1] == 0x53 && cbw[2] == 0x42 &&
                  cbw[3] == 0x43,
              "cbw signature");
        check(cbw[4] == 0x44 && cbw[7] == 0x11, "cbw tag LE");
        check(cbw[8] == 0x00 && cbw[9] == 0x02, "cbw len 512");
        check(cbw[12] == 0x80, "cbw direction in");
        check(cbw[14] == 10 && cbw[15] == 0x28 && cbw[20] == 100,
              "cbw cdb copied");
    }

    /* ---- EHCI: qTD トークン ---- */
    {
        uint32_t t = ehci_qtd_token(2, 8, 0, 0); /* SETUP 8 バイト */
        check((t & 0x80) != 0, "ehci qtd active set");
        check(((t >> 8) & 3) == 2, "ehci qtd pid setup");
        check(((t >> 16) & 0x7FFF) == 8, "ehci qtd len 8");
        check((t >> 31) == 0, "ehci qtd toggle 0");
        t = ehci_qtd_token(1, 512, 1, 1); /* IN 512 トグル1 IOC */
        check(((t >> 8) & 3) == 1, "ehci qtd pid in");
        check(((t >> 16) & 0x7FFF) == 512, "ehci qtd len 512");
        check((t >> 31) == 1, "ehci qtd toggle 1");
        check((t & (1u << 15)) != 0, "ehci qtd ioc");
        t = ehci_qtd_token(0, 0, 1, 0); /* OUT 状態段階 */
        check(((t >> 8) & 3) == 0 && ((t >> 16) & 0x7FFF) == 0,
              "ehci qtd status stage");
    }
    /* ---- EHCI: QH 接続点特性 ---- */
    {
        uint32_t v = ehci_qh_epchar(3, 0, 2, 64, 0, 0); /* HS 制御 */
        check((v & 0x7F) == 3, "ehci epchar addr");
        check(((v >> 12) & 3) == 2, "ehci epchar speed hs");
        check(((v >> 16) & 0x7FF) == 64, "ehci epchar mp 64");
        check((v & (1u << 14)) != 0, "ehci epchar dtc");
        check((v & (1u << 15)) == 0, "ehci epchar not head");
        check((v & (1u << 27)) == 0, "ehci epchar hs no cbit");
        v = ehci_qh_epchar(0, 0, 2, 64, 1, 0); /* 先頭 QH */
        check((v & (1u << 15)) != 0, "ehci epchar head bit");
        v = ehci_qh_epchar(1, 0, 0, 8, 0, 1); /* FS 制御 */
        check(((v >> 12) & 3) == 0, "ehci epchar speed fs");
        check((v & (1u << 27)) != 0, "ehci epchar fs cbit");
        v = ehci_qh_epchar(2, 1, 2, 512, 0, 0); /* HS バルク */
        check(((v >> 8) & 0xF) == 1, "ehci epchar ep 1");
        check(((v >> 16) & 0x7FF) == 512, "ehci epchar mp 512");
    }
    /* ---- EHCI: 構成記述子の解析 (高速ストレージ) ---- */
    {
        uint8_t cfg[32];
        memset(cfg, 0, sizeof(cfg));
        cfg[0] = 9; cfg[1] = 2; cfg[2] = 32; cfg[5] = 1;
        cfg[9] = 9; cfg[10] = 4; cfg[11] = 0; cfg[13] = 2;
        cfg[14] = 8; cfg[15] = 6; cfg[16] = 0x50;
        cfg[18] = 7; cfg[19] = 5; cfg[20] = 0x81; cfg[21] = 2;
        cfg[22] = 0x00; cfg[23] = 0x02; /* 最大パケット 512 */
        cfg[25] = 7; cfg[26] = 5; cfg[27] = 0x02; cfg[28] = 2;
        cfg[29] = 0x00; cfg[30] = 0x02;
        ehci_device info;
        memset(&info, 0, sizeof(info));
        check(ehci_parse_config(cfg, sizeof(cfg), &info) == 0,
              "ehci cfg storage parsed");
        check(info.bulk_in_ep == 1 && info.bulk_out_ep == 2,
              "ehci cfg bulk eps");
        check(info.bulk_max_packet == 512, "ehci cfg bulk mp 512");
        check(info.if_class == 8, "ehci cfg if class");
        memset(&info, 0, sizeof(info));
        check(ehci_parse_config(cfg, 5, &info) == -1,
              "ehci cfg short rejected");
        /* HID のみの構成ではストレージ終端が無い。 */
        uint8_t kbd[25];
        memset(kbd, 0, sizeof(kbd));
        kbd[0] = 9; kbd[1] = 2; kbd[2] = 25; kbd[5] = 1;
        kbd[9] = 9; kbd[10] = 4; kbd[13] = 1; kbd[14] = 3;
        kbd[18] = 7; kbd[19] = 5; kbd[20] = 0x81; kbd[21] = 3;
        kbd[22] = 8;
        memset(&info, 0, sizeof(info));
        check(ehci_parse_config(kbd, sizeof(kbd), &info) == -1,
              "ehci cfg hid no storage");
        check(info.if_class == 3, "ehci cfg hid if class");
    }
    /* ---- xHCI: 文脈ワードと TRB の組み立て ---- */
    {
        /* 制御終端: 種別 4、最大パケット 512、CErr 3。 */
        uint32_t v = xhci_ep_info2(4, 512, 0, 3);
        check(((v >> 1) & 3) == 3, "xhci ep cerr 3");
        check(((v >> 3) & 7) == 4, "xhci ep type control");
        check(((v >> 16) & 0xFFFF) == 512, "xhci ep mp 512");
        check(((v >> 8) & 0xFF) == 0, "xhci ep burst 0");
        v = xhci_ep_info2(6, 512, 0, 3); /* バルク IN */
        check(((v >> 3) & 7) == 6, "xhci ep type bulk in");
        v = xhci_ep_info2(2, 64, 0, 3); /* バルク OUT */
        check(((v >> 3) & 7) == 2, "xhci ep type bulk out");
        check(((v >> 16) & 0xFFFF) == 64, "xhci ep mp 64");
        /* スロット: 高速 (3)、文脈数 1。 */
        v = xhci_slot_info(3, 1);
        check(((v >> 20) & 0xF) == 3, "xhci slot speed hs");
        check(((v >> 27) & 0x1F) == 1, "xhci slot ctx 1");
        v = xhci_slot_info(4, 31); /* 超速、文脈数 31 */
        check(((v >> 20) & 0xF) == 4, "xhci slot speed ss");
        check(((v >> 27) & 0x1F) == 31, "xhci slot ctx 31");
        /* Setup の転送種別。 */
        check(xhci_setup_trt(0, 0) == 0, "xhci trt none");
        check(xhci_setup_trt(0, 1) == 2, "xhci trt out");
        check(xhci_setup_trt(1, 1) == 3, "xhci trt in");
        /* TRB 制御ワード: 種別とサイクルと IOC。 */
        v = xhci_trb_control(11, 1, (1u << 5));
        check((v & 1u) == 1u, "xhci trb cycle set");
        check(((v >> 10) & 0x3F) == 11, "xhci trb type addr");
        check((v & (1u << 5)) != 0, "xhci trb ioc kept");
        v = xhci_trb_control(1, 0, 0);
        check((v & 1u) == 0, "xhci trb cycle clear");
        check(((v >> 10) & 0x3F) == 1, "xhci trb type normal");
    }
    /* ---- xHCI: 構成記述子の解析 ---- */
    {
        uint8_t cfg[32];
        memset(cfg, 0, sizeof(cfg));
        cfg[0] = 9; cfg[1] = 2; cfg[2] = 32; cfg[5] = 1;
        cfg[9] = 9; cfg[10] = 4; cfg[11] = 0; cfg[13] = 2;
        cfg[14] = 8; cfg[15] = 6; cfg[16] = 0x50;
        cfg[18] = 7; cfg[19] = 5; cfg[20] = 0x81; cfg[21] = 2;
        cfg[22] = 0x00; cfg[23] = 0x02;
        cfg[25] = 7; cfg[26] = 5; cfg[27] = 0x02; cfg[28] = 2;
        cfg[29] = 0x00; cfg[30] = 0x02;
        xhci_device info;
        memset(&info, 0, sizeof(info));
        check(xhci_parse_config(cfg, sizeof(cfg), &info) == 0,
              "xhci cfg storage parsed");
        check(info.bulk_in_ep == 1 && info.bulk_out_ep == 2,
              "xhci cfg bulk eps");
        check(info.bulk_max_packet == 512, "xhci cfg bulk mp 512");
        check(info.if_class == 8, "xhci cfg if class");
    }

    printf("drivers tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
