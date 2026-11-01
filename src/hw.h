/* What the game needs from the machine (hw_cybiko.c on the Cybiko,
 * pc/hw_pc.c for the PC test harness). */
#ifndef HW_H
#define HW_H
#include "doomtype.h"

/* keys, as bits of hw_keys() */
#define K_UP 0x0001
#define K_DOWN 0x0002
#define K_LEFT 0x0004
#define K_RIGHT 0x0008
#define K_FIRE 0x0010       /* Enter, Space */
#define K_USE 0x0020        /* Select */
#define K_ESC 0x0040
#define K_RUN 0x0080        /* Shift */
#define K_STRAFE 0x0100     /* Tab, Fn */
#define K_STRAFEL 0x0200    /* A or "," */
#define K_STRAFER 0x0400    /* D or "." */
#define K_MAP 0x0800        /* Menu */
#define K_NEXTW 0x1000      /* Ins */
#define K_FPS 0x2000        /* Help */
#define K_TOGGLERUN 0x4000  /* R */
#define K_PREVW 0x8000      /* Del */

void hw_init(void);
void hw_shutdown(void);
u16 hw_keys(void);
u8 hw_weapon_key(void);     /* 1-7 for F1-F7 held, else 0 */
u32 hw_clock(void);         /* ticks of HW_HZ */
void hw_present(void);      /* fb to the screen */
void hw_present_rows(s16 from, s16 to);     /* only rows from..to-1 */
void hw_idle(void);         /* keep the watchdog happy */
void hw_watchdog(void);     /* only that (long work such as unpacking) */
void hw_debug(const char *s);
/* a file of the level pack (doom.dat, doom_e1m1.dat, ...), looked for on
 * the MP3 card and then in storage: read n bytes at offset off, 0 on
 * failure */
int hw_file_read(const char *name, u32 off, void *buf, u32 n);
/* what the last reads did, newest first (i = 0, 1, ...; NULL past the
 * last): a line each, for the screen that says why the pack was not found */
const char *hw_file_log(int i);
void *hw_alloc(u32 n);      /* memory for the pack, NULL if none */
void hw_tone(u16 divisor);  /* a square wave of 1193181 Hz / divisor, 0 silence */
extern const u32 HW_HZ;

#endif
