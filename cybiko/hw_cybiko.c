/* The Cybiko Xtreme side: CyOS starts main() like any application; the game
 * then takes the machine over until it quits. With interrupts off CyOS
 * stops (no task switches, no screen updates of its own) and all of the
 * 18 MHz H8S is the game's. The hardware is used directly:
 *
 *   screen    HD66421 at 100000 (index) / 100001 (data); display RAM row 0
 *             is the bottom line
 *   keys      matrix at e00000: a 16-bit read at e00000 + 2 * sel gives the
 *             rows whose bit in sel is 0, a pressed key reads 0
 *   clock     TPU channel 5, which CyOS runs free at clock / 256 (72 kHz)
 *   watchdog  CyOS enables it; with its interrupts off we must reset it
 *
 * The level pack is a file read with CyOS's own functions (cybiko/cyos.ld):
 * found on the MP3 cartridge's card (/MP3/) or in the Cybiko's storage.
 * While the game has the machine, a read hands it back to CyOS for the
 * time it takes (cyos_call below).
 *
 * See docs/cybiko-internals.md in the Dragonfruit emulator for the details. */
#include "../src/hw.h"
#include "../src/render.h"
#include "../src/level.h"

#define REG8(a) (*(volatile u8 *)(a))
#define REG16(a) (*(volatile u16 *)(a))

#define TCNT5 REG16(0xfffea6)
#define WDT_TCSR 0xffffbc

const u32 HW_HZ = 72000;
static u8 saved_ccr;
static u8 owned;                /* the game has the machine */
static u32 clock_hi;
static u16 clock_last;

void doom_main(void);
u8 irq_off(void);               /* rt.s: mask interrupts, return the old CCR */
void irq_restore(u8 ccr);
void run_on_stack(void (*fn)(void), void *top);
/* rt.s: fn(arg) on the stack run_on_stack was called on */
long on_cyos_stack(long (*fn)(void *), void *arg);

/* CyOS (cybiko/cyos.ld; the names are the Cybiko SDK's) */
void *cy_malloc(u32 n);
void cy_free(void *p);
void *cy_FileInput_ctor(void *fi, int magic);
u8 cy_FileInput_open(void *fi, const char *name);
long cy_Input_seek(void *in, long pos, int mode);
long cy_Input_read(void *in, void *buf, long n);
void cy_Input_dtor(void *in, int memory_flag);
#define LEAVE_MEMORY 2

/* The game runs with its screen buffer (fb, see link.ld) and its stack in
 * the on-chip RAM from IRAM_START to IRAM_END. CyOS's data there is kept in
 * iram_save meanwhile, and the data transfer controller, which works from
 * tables in that RAM, is switched off (DTCER) until the game is done. */
#define IRAM_START 0xffdc00UL
#define IRAM_END 0xfffc00UL
#define DTCER 0xffff30UL
static u8 iram_save[IRAM_END - IRAM_START];
static u8 iram_game[IRAM_END - IRAM_START];  /* the game's, during cyos_call */
static u8 dtcer_save[6];
extern u8 _iram_stack_top[], _iram_code[], _iram_code_end[];
#define IRAM_CODE 0xfff800UL

void hw_idle(void)
{
    REG16(WDT_TCSR) = 0x5a00;       /* watchdog counter back to 0 */
}

void hw_init(void)
{
    u8 i;
    saved_ccr = irq_off();
    for (i = 0; i < 6; i++) {
        dtcer_save[i] = REG8(DTCER + i);
        REG8(DTCER + i) = 0;
    }
    mcpy(iram_save, (void *)IRAM_START, sizeof iram_save);
    mcpy((void *)IRAM_CODE, _iram_code, (u32)(_iram_code_end - _iram_code));
    hw_idle();
    clock_last = TCNT5;
    owned = 1;
}

void hw_shutdown(void)
{
    u8 i;
    owned = 0;
    mcpy((void *)IRAM_START, iram_save, sizeof iram_save);
    for (i = 0; i < 6; i++) REG8(DTCER + i) = dtcer_save[i];
    hw_idle();
    irq_restore(saved_ccr);
}

/* Runs on CyOS's stack: put CyOS's on-chip RAM, DTC and interrupts back,
 * call fn, then take the machine again. */
static long (*cyos_fn)(void *);

static long cyos_side(void *arg)
{
    long r;
    u8 i;
    mcpy(iram_game, (void *)IRAM_START, sizeof iram_game);
    mcpy((void *)IRAM_START, iram_save, sizeof iram_save);
    for (i = 0; i < 6; i++) REG8(DTCER + i) = dtcer_save[i];
    hw_idle();
    irq_restore(saved_ccr);
    r = cyos_fn(arg);
    irq_off();
    for (i = 0; i < 6; i++) REG8(DTCER + i) = 0;
    mcpy(iram_save, (void *)IRAM_START, sizeof iram_save);
    mcpy((void *)IRAM_START, iram_game, sizeof iram_game);
    hw_idle();
    return r;
}

static long cyos_call(long (*fn)(void *), void *arg)
{
    if (!owned) return fn(arg);
    cyos_fn = fn;
    return on_cyos_stack(cyos_side, arg);
}

u32 hw_clock(void)
{
    u16 now = TCNT5;
    clock_hi += (u16)(now - clock_last);
    clock_last = now;
    hw_idle();
    return clock_hi;
}

static u16 row(u8 r)
{
    return (u16)~REG16(0xe00000UL + 2 * (0x3ff & ~(1u << r)));
}

/* key map: matrix row, bit, game key */
static const struct { u8 row, bit; u16 key; } keymap[] = {
    {6, 11, K_UP}, {3, 6, K_UP},                /* Up, W */
    {6, 13, K_DOWN}, {3, 5, K_DOWN},            /* Down, S */
    {6, 14, K_LEFT}, {6, 12, K_RIGHT},
    {4, 6, K_FIRE}, {0, 11, K_FIRE},            /* Space, K */
    {4, 3, K_USE}, {3, 7, K_USE},               /* Enter, E */
    {5, 10, K_ESC},
    {8, 15, K_RUN},                             /* Shift */
    {7, 15, K_STRAFE},                          /* Fn */
    {3, 2, K_STRAFEL}, {0, 10, K_STRAFEL},      /* A , */
    {2, 8, K_STRAFER}, {9, 1, K_STRAFER},       /* D . */
    {4, 5, K_MAP}, {0, 8, K_MAP},               /* Menu, M */
    {5, 7, K_NEXTW},                            /* Tab */
    {9, 0, K_FPS},                              /* Help */
    {4, 4, K_TOGGLERUN},                        /* Select */
};

u16 hw_keys(void)
{
    u16 rows[10], k = 0;
    u8 i;
    for (i = 0; i < 10; i++) rows[i] = row(i);
    for (i = 0; i < sizeof keymap / sizeof keymap[0]; i++)
        if (rows[keymap[i].row] & (1u << keymap[i].bit)) k |= keymap[i].key;
    return k;
}

u8 hw_weapon_key(void)
{
    u8 r;
    for (r = 0; r < 7; r++)
        if (row((u8)(6 - r)) & 1) return (u8)(r + 1);           /* F1 is in row 6 */
    return 0;
}

/* hw_present_rows is in rt.s */

void hw_present(void)
{
    hw_present_rows(SCREENH);
}

/* text on the serial console (SCI1), where the emulator prints it */
void hw_debug(const char *s)
{
    while (*s) {
        u16 t = 2000;
        while (!(REG8(0xffff84) & 0x80) && --t) ;
        if (!t) return;
        REG8(0xffff83) = (u8)*s++;
        REG8(0xffff84) &= 0x7f;
    }
}

/* the level pack */
static const char *const pack_paths[] = { "/MP3/doom.dat", "doom.dat" };
static const char *pack_path;
static u8 file_input[96];       /* struct FileInput: 84 bytes */
static struct { u32 off, n; void *buf; } rd;

static long pack_read(void *unused)
{
    long r = -1;
    (void)unused;
    cy_FileInput_ctor(file_input, 1);
    if (cy_FileInput_open(file_input, pack_path) && cy_Input_seek(file_input, (long)rd.off, 0) == (long)rd.off)
        r = cy_Input_read(file_input, rd.buf, (long)rd.n);
    cy_Input_dtor(file_input, LEAVE_MEMORY);
    return r;
}

int hw_pack_read(u32 off, void *buf, u32 n)
{
    long r;
    u32 t = hw_clock();
    rd.off = off;
    rd.n = n;
    rd.buf = buf;
    r = cyos_call(pack_read, 0);
    clock_hi = t;               /* the time CyOS had does not count */
    clock_last = TCNT5;
    return r == (long)n;
}

#define MAX_ALLOCS 4
static void *allocs[MAX_ALLOCS];

void *hw_alloc(u32 n)
{
    u8 i;
    for (i = 0; i < MAX_ALLOCS; i++)
        if (!allocs[i]) return allocs[i] = cy_malloc(n);
    return 0;
}

long main(int argc, char **argv, char start)
{
    u8 i;
    (void)argc;
    (void)argv;
    if (!start) return 0;
    for (i = 0; i < sizeof pack_paths / sizeof pack_paths[0]; i++) {
        pack_path = pack_paths[i];
        if (pack_open()) break;
    }
    hw_init();
    run_on_stack(doom_main, _iram_stack_top);
    hw_shutdown();
    for (i = 0; i < MAX_ALLOCS; i++)
        if (allocs[i]) cy_free(allocs[i]);
    return 0;
}
