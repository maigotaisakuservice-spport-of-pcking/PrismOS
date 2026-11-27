#include <stddef.h>
/* PrismOS - desktop (screen apps).
 *
 * Draws the screen: wallpaper, one app window at a time,
 * a Dock and a taskbar with a real clock. The sixteen apps are
 * real: Terminal mirrors the serial shell, Files browses and
 * edits the real file system, Settings edits the real settings
 * and saves them to "settings.txt" in the file system,
 * Browser fetches a real page over the network and shows
 * its text, Notes edits a real text file and saves it,
 * Calendar shows the real date from the clock, Photos shows
 * real BMP files from the file system, Music plays real WAV
 * files through the SB16 audio device, Cards plays a real
 * game of blackjack with a shuffled deck, Shooter is a
 * real 3D shooting game (raycasting, keyboard and mouse
 * aim, enemies that approach and attack), Sheet is a real
 * spreadsheet (numbers, text, formulas with cell references
 * and SUM/AVG/MIN/MAX, saved to "sheet.csv"), Video
 * plays the real compressed "vid-*.pv1" frames (320x240,
 * RGB565, inter-frame diff + DEFLATE) in the file system
 * in order at 4 frames per second (the old "vid-*.bmp"
 * sequence still plays), and Installer installs
 * PrismOS to the attached disk for real (partition screen,
 * standard or minimal profile, progress, verification),
 * Mail sends and receives real mail over SMTP/POP3
 * and stores it as files in the file system, Docs edits a
 * real document with headings, bullets and bold text
 * (saved to "doc.txt", exported to a real .docx file),
 * and Slides edits real slides (saved to "slides.txt",
 * exported to a real .pptx file) and shows them as a
 * slideshow on screen. The allowlist (security) hides
 * no apps from the Dock, but switch_mode() refuses to
 * open a denied app and shows a notice instead.
 *
 * Input: the shell loop hands every key to desktop_handle_key()
 * first. It returns 1 when the desktop consumed the key
 * (mode switch, app navigation), 0 when the key
 * belongs to the shell line being typed.
 * The pointer device is real too: the PS/2 mouse moves a cursor
 * on screen, and clicking a Dock button switches apps.
 *
 * What is NOT here (reported as incomplete, never faked):
 * mail has no encryption (no TLS in the stack) and no
 * folders or attachments yet.
 */
#ifndef PRISM_DESKTOP_H
#define PRISM_DESKTOP_H

/* Special keys the keyboard driver returns (see keyboard.h). */
#define DESK_KEY_F1 0x81
#define DESK_KEY_F2 0x82
#define DESK_KEY_F3 0x83
#define DESK_KEY_F4 0x84
#define DESK_KEY_F5 0x85
#define DESK_KEY_F6 0x86
#define DESK_KEY_F7 0x87
#define DESK_KEY_F8 0x88
#define DESK_KEY_F9 0x89
#define DESK_KEY_F10 0x8A
#define DESK_KEY_F11 0x8B
#define DESK_KEY_F12 0x8C
#define DESK_KEY_CTRL_O 0xA1
#define DESK_KEY_CTRL_V 0xA2
#define DESK_KEY_CTRL_I 0xA3
#define DESK_KEY_CTRL_M 0xA4
#define DESK_KEY_CTRL_D 0xA5
#define DESK_KEY_CTRL_P 0xA6
#define DESK_KEY_CTRL_N 0xA7
#define DESK_KEY_CTRL_E 0xA8
#define DESK_KEY_CTRL_R 0xA9
#define DESK_KEY_PGUP 0x94
#define DESK_KEY_PGDN 0x95
#define DESK_KEY_ZOOM_IN 0xD0
#define DESK_KEY_ZOOM_OUT 0xD1
#define DESK_KEY_UP 0x90
#define DESK_KEY_DOWN 0x91
#define DESK_KEY_RIGHT 0x92
#define DESK_KEY_LEFT 0x93

/* Initialise the desktop after the framebuffer and the file
 * system are ready. Draws the first screen. */
void desktop_init(void);
int desktop_ready(void);

/* Re-read "allowlist.txt" (after `security allow|deny`). */
void desktop_reload_allowlist(void);

/* Switch apps / navigate. Returns 1 if the key was consumed. */
int desktop_handle_key(int c);

/* On-screen keyboard: fetch the next key clicked on the
 * panel, or -1. The shell input loop polls this so OSK keys
 * take exactly the same path as physical keys. */
int desktop_osk_poll_key(void);

/* Screen console: the Terminal app's text grid. The shell
 * writes its echo and output here as well as to serial. */
void desktop_console_putc(int c);
void desktop_console_write(const char *s);
void desktop_console_clear(void);

/* Show a notification toast in the top-right corner for a
 * few seconds. Safe to call before the desktop is up. */
void desktop_notify(const char *msg);

/* appguard の自己テスト: 武装領域でわざとフォルトを
 * 起こし、封じ込めで戻ってくることをシリアルに出す。 */
void desktop_appguard_selftest(void);

/* Shared text clipboard (Notes line copy/cut/paste, Files
 * name copy). */
void desktop_clipboard_set(const char *s, size_t n);
const char *desktop_clipboard_get(size_t *n);

/* Redraw the whole screen (wallpaper, window, Dock, taskbar). */
void desktop_redraw(void);
/* Redraw everything except the wallpaper (fast path for typing). */
void desktop_refresh_console(void);
/* Called from the shell's input loop: refreshes the clock
 * when the second changes. */
void desktop_tick(void);

/* Called from the shell's input loop: drains mouse events,
 * moves the cursor, handles clicks, and redraws when the
 * pointer moved or was clicked. */
void desktop_mouse_poll(void);

#endif /* PRISM_DESKTOP_H */
