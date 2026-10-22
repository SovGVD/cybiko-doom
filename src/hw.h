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
#define K_FIRE 0x0010       /* Space */
#define K_USE 0x0020        /* Enter */
#define K_ESC 0x0040
#define K_RUN 0x0080        /* Shift */
#define K_STRAFE 0x0100     /* Fn */
#define K_STRAFEL 0x0200    /* A or "," */
#define K_STRAFER 0x0400    /* D or "." */
#define K_MAP 0x0800        /* Menu */
#define K_NEXTW 0x1000      /* Tab */
#define K_FPS 0x2000        /* Help */
#define K_TOGGLERUN 0x4000  /* Select */

void hw_init(void);
void hw_shutdown(void);
u16 hw_keys(void);
u8 hw_weapon_key(void);     /* 1-7 for F1-F7 held, else 0 */
u32 hw_clock(void);         /* ticks of HW_HZ */
void hw_present(void);      /* fb to the screen */
void hw_present_rows(s16 rows);     /* only its top rows */
void hw_idle(void);         /* keep the watchdog happy */
void hw_debug(const char *s);
extern const u32 HW_HZ;

#endif
