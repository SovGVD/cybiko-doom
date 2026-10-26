/* The Cybiko Xtreme side: CyOS starts main() like any application; the game
 * then takes the machine over until it quits. With interrupts off CyOS
 * stops (no task switches, no screen updates of its own) and all of the
 * 18 MHz H8S is the game's. The hardware is used directly:
 *
 *   screen    HD66421 at 100000 (index) / 100001 (data); display RAM row 0
 *             is the bottom line
 *   keys      matrix at e00000: a 16-bit read at e00000 + 2 * sel gives the
 *             rows whose bit in sel is 0, a pressed key reads 0; read with
 *             the DMA controller as CyOS does (key_read_dma)
 *   clock     TPU channel 5, which CyOS runs free at clock / 256 (72 kHz)
 *   watchdog  CyOS enables it; with its interrupts off we must reset it
 *
 * The level pack is a file read with CyOS's own functions (cybiko/cyos.ld):
 * found on the MP3 cartridge's card (/MP3/) or in the Cybiko's storage.
 * While the game has the machine, a read hands it back to CyOS for the
 * time it takes (cyos_call below).
 *
 * See hardware.md and cyos.md in cybiko-docs
 * (https://github.com/SovGVD/cybiko-docs) for the details. */
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

/* The game runs with its hot code, tables and stack (see link.ld) in
 * the on-chip RAM from IRAM_START to IRAM_END. CyOS's data there is kept in
 * iram_save meanwhile, and the data transfer controller, which works from
 * tables in that RAM, is switched off (DTCER) until the game is done. */
#define IRAM_START 0xffdc00UL
#define IRAM_END 0xfffc00UL
#define DTCER 0xffff30UL
static u8 iram_save[IRAM_END - IRAM_START];
static u8 iram_game[IRAM_END - IRAM_START];  /* the game's, during cyos_call */
static u8 dtcer_save[6];
extern u8 _iram_stack_top[], _iram_code[], _iram_code_end[], _iramfn[], _iramfn_end[];
#define IRAM_CODE 0xfff800UL    /* rt.s's routines (link.ld) */
#define IRAM_FN_BASE 0xffdc00UL      /* the renderer's hot C functions */

void hw_idle(void)
{
    REG16(WDT_TCSR) = 0x5a00;       /* watchdog counter back to 0 */
}

void hw_watchdog(void)
{
    REG16(WDT_TCSR) = 0x5a00;
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
    mcpy((void *)IRAM_FN_BASE, _iramfn, (u32)(_iramfn_end - _iramfn));
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

/* The keyboard is read the way CyOS 1.5.08 reads it: the DMA controller
 * (channel 1, full address mode, burst) reads the row's address 12 times
 * back to back into a word of on-chip RAM, and the last read counts. The
 * matrix is selected by address lines, so it needs the address held on the
 * bus for a while; a single CPU read between instruction fetches from
 * other addresses likely comes too early on a real Cybiko, which read no
 * key at all with it (emulators answer at once). Doom Test (doomtest.c)
 * compares the ways. CyOS also stops the refresh timer (bit 0 of
 * DRAMCR) meanwhile; done the same here. */
#define DMA_MAR1A 0xfffef0
#define DMA_ETCR1A 0xfffef6
#define DMA_MAR1B 0xfffef8
#define DMACR1A 0xffff04
#define DMACR1B 0xffff05
#define DMABCRH 0xffff06
#define DMABCRL 0xffff07
#define DRAMCR 0xfffed7
#define REG32(a) (*(volatile u32 *)(a))
#define KEY_READS 12
static volatile u16 key_word IRAM;

u16 key_read_dma(u16 sel)
{
    u16 t = 2000;
    REG8(DRAMCR) &= 0xfe;
    REG8(DMABCRL) &= 0xbf;
    REG8(DMABCRL) &= 0x7f;
    REG32(DMA_MAR1A) = 0xe00000UL + 2 * (u32)sel;
    REG32(DMA_MAR1B) = (u32)&key_word;
    REG16(DMA_ETCR1A) = KEY_READS;
    REG8(DMABCRH) |= 0x80;
    REG8(DMABCRH) |= 0x08;
    REG8(DMABCRL) &= 0xf7;
    REG8(DMACR1A) = 0x80;       /* words, source and destination fixed */
    REG8(DMACR1B) = 0x07;       /* auto request, burst */
    REG8(DMABCRL) |= 0x80;
    REG8(DMABCRL) |= 0x40;
    while ((REG8(DMABCRL) & 0x40) && --t) ;
    REG8(DRAMCR) |= 0x11;
    return key_word;
}

/* the old way, for the key test: n CPU reads in a row, the last counts */
u16 key_read_cpu(u16 sel, u8 n)
{
    u16 v;
    do v = REG16(0xe00000UL + 2 * (u32)sel); while (--n);
    return v;
}

static u16 row(u8 r)
{
    return (u16)~key_read_dma((u16)(0x3ff & ~(1u << r)));
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
    if (key_read_dma(0) == 0xffff) return 0;   /* all rows at once: none down */
    for (i = 0; i < 10; i++) rows[i] = row(i);
    for (i = 0; i < sizeof keymap / sizeof keymap[0]; i++)
        if (rows[keymap[i].row] & (1u << keymap[i].bit)) k |= keymap[i].key;
    return k;
}

u8 hw_weapon_key(void)
{
    u8 r;
    if (key_read_dma(0) == 0xffff) return 0;
    for (r = 0; r < 7; r++)
        if (row((u8)(6 - r)) & 1) return (u8)(r + 1);           /* F1 is in row 6 */
    return 0;
}

/* hw_present_rows is in rt.s */

void hw_present(void)
{
    hw_present_rows(0, SCREENH);
}

/* text on the serial console (SCI1), where the emulator prints it. SCI1
 * is also the MP3 card's SPI port: once the card driver has used it, it is
 * in clocked synchronous mode (SMR bit 7) and switched off (SCR = 0), and a
 * byte written then leaves TDRE clear for good, so the driver waits on it
 * and no card read works any more until the Cybiko restarts (on a real
 * Cybiko: the next map did not load, after quitting too). So nothing is
 * sent unless SCI1 is an asynchronous port with its transmitter on. */
void hw_debug(const char *s)
{
    if ((REG8(0xffff80) & 0x80) || !(REG8(0xffff82) & 0x20)) return;
    while (*s) {
        u16 t = 2000;
        while (!(REG8(0xffff84) & 0x80) && --t) ;
        if (!t) return;
        REG8(0xffff83) = (u8)*s++;
        REG8(0xffff84) &= 0x7f;
    }
}

/* the level pack's files: on the MP3 card's card, then in storage */
static const char *const pack_dirs[] = { "/MP3/", "" };
static char pack_path[32];
static u8 file_input[96];       /* struct FileInput: 84 bytes */
static struct { u32 off, n; void *buf; } rd;

/* what the reads did, for the screens that say why the pack was not
 * found (hw_file_log) */
#define LOG_LINES 8
static char file_log[LOG_LINES][40];
static u8 log_next;

static char *put_str(char *q, const char *s)
{
    while (*s) *q++ = *s++;
    return q;
}

static char *put_num(char *q, long v)
{
    char t[12];
    int n = 0;
    if (v < 0) {
        *q++ = '-';
        v = -v;
    }
    do t[n++] = (char)('0' + v % 10); while ((v /= 10) && n < 11);
    while (n) *q++ = t[--n];
    return q;
}

#define READ_PIECE 4096L

/* the result of one read; step: 0 open failed, 1 seek failed, 2 read */
static long rd_step, rd_seek, rd_got;

static long pack_read(void *unused)
{
    (void)unused;
    rd_step = 0;
    rd_seek = rd_got = 0;
    cy_FileInput_ctor(file_input, 1);
    if (cy_FileInput_open(file_input, pack_path)) {
        rd_step = 1;
        rd_seek = cy_Input_seek(file_input, (long)rd.off, 0);
        if (rd_seek == (long)rd.off) {
            /* in pieces no bigger than the MP3 Player's reads from the
             * card, in case a card or its driver has trouble with long
             * runs of blocks */
            rd_step = 2;
            while (rd_got < (long)rd.n) {
                long want = (long)rd.n - rd_got, got;
                if (want > READ_PIECE) want = READ_PIECE;
                got = cy_Input_read(file_input, (u8 *)rd.buf + rd_got, want);
                if (got <= 0) break;
                rd_got += got;
            }
        }
    }
    cy_Input_dtor(file_input, LEAVE_MEMORY);
    return rd_step == 2 ? rd_got : -1;
}

static void log_read(void)
{
    char *q = file_log[log_next];
    log_next = (u8)((log_next + 1) % LOG_LINES);
    q = put_str(q, pack_path[0] ? pack_path : "?");
    *q++ = ' ';
    if (rd_step == 0) q = put_str(q, "NOT OPENED");
    else if (rd_step == 1) {
        q = put_str(q, "SEEK ");
        q = put_num(q, (long)rd.off);
        q = put_str(q, ":");
        q = put_num(q, rd_seek);
    } else {
        q = put_num(q, rd_got);
        *q++ = '/';
        q = put_num(q, (long)rd.n);
        if (rd.off) {
            *q++ = '@';
            q = put_num(q, (long)rd.off);
        }
    }
    *q = 0;
}

const char *hw_file_log(int i)
{
    const char *l;
    if (i < 0 || i >= LOG_LINES) return 0;
    l = file_log[(log_next + LOG_LINES - 1 - i) % LOG_LINES];
    return l[0] ? l : 0;
}

int hw_file_read(const char *name, u32 off, void *buf, u32 n)
{
    u8 i;
    for (i = 0; i < sizeof pack_dirs / sizeof pack_dirs[0]; i++) {
        const char *d = pack_dirs[i];
        u8 k = 0;
        long r;
        u32 t = hw_clock();
        while (*d) pack_path[k++] = *d++;
        for (d = name; *d && k < sizeof pack_path - 1; ) pack_path[k++] = *d++;
        pack_path[k] = 0;
        rd.off = off;
        rd.n = n;
        rd.buf = buf;
        r = cyos_call(pack_read, 0);
        clock_hi = t;           /* the time CyOS had does not count */
        clock_last = TCNT5;
        log_read();
        if (r == (long)n) return 1;
    }
    return 0;
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

/* the biggest block CyOS can give, in bytes (in steps of 8 KB, up to
 * 1 MB; a binary search from 2 MB makes CyOS end the application) */
u32 hw_free_memory(void)
{
    u32 kb;
    for (kb = 8; kb <= 1024; kb += 8) {
        void *p = cy_malloc(kb * 1024);
        if (!p) break;
        cy_free(p);
    }
    return (kb - 8) * 1024;
}

#ifdef DOOM_TEST
void test_files(void);
void test_main(void);
#endif

long main(int argc, char **argv, char start)
{
    u8 i;
    (void)argc;
    (void)argv;
    if (!start) return 0;
#ifdef DOOM_TEST
    test_files();
    hw_init();
    run_on_stack(test_main, _iram_stack_top);
#else
    pack_open();
    hw_init();
    run_on_stack(doom_main, _iram_stack_top);
#endif
    hw_shutdown();
    for (i = 0; i < MAX_ALLOCS; i++)
        if (allocs[i]) cy_free(allocs[i]);
    return 0;
}
