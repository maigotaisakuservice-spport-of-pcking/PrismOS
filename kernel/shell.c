/* PrismOS — シリアルシェル (カーネル側ラッパー)
 *
 * コマンドの解析と実行そのものは shell/core.c (ホストでも
 * テストできる純粋なロジック) にあり、ここは入出力と
 * reboot / clear の副作用だけを担当する。
 */
#include "shell.h"
#include "io.h"
#include "serial.h"
#include "string.h"
#include "../shell/core.h"

#define LINE_CAP 256
#define OUT_CAP 2048

void shell_run(void) {
    prism_shell_state state;
    char line[LINE_CAP];
    char out[OUT_CAP];

    prism_shell_init(&state);

    for (;;) {
        serial_write("prismos> ");
        serial_read_line(line, sizeof(line));
        if (line[0] == '\0') {
            continue;
        }

        prism_shell_execute(&state, line, out, sizeof(out));

        if (state.should_clear) {
            state.should_clear = 0;
            serial_write("\x1b[2J\x1b[H");
        }
        if (out[0] != '\0') {
            serial_write(out);
            if (out[strlen(out) - 1] != '\n') {
                serial_write_char('\n');
            }
        }
        if (state.should_reboot) {
            serial_write_line("Rebooting PrismOS...");
            /* キーボードコントローラ経由でリセットを要求する。
             * 効かなければ hlt ループに落ち、QEMU の -no-reboot が
             * あれば終了する。 */
            prism_outb(0x64, 0xFE);
            prism_halt();
        }
    }
}
