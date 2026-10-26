/* Doom Test (build/doomtest.app): checks on a real Cybiko what Doom needs
 * from it, for when the game does not start or does not react to keys.
 *
 * Page 1, files: the free memory, what Doom finds of its level pack
 * (doom.dat and the map files, on the MP3 card's /MP3/ or in storage) and
 * whether each map reads and unpacks, with the last file reads.
 * Page 2, keys: the key matrix read three ways, live: one CPU read (how
 * Doom read it up to now), twelve CPU reads in a row, and the DMA reads
 * CyOS itself uses (how Doom reads it now). Each row shows the keys down
 * as a hex mask; "SEEN" says which way has ever seen a key.
 *
 * hw_cybiko.c's main() runs test_files() while CyOS still has the machine,
 * then test_main() the way it runs the game. */
#include "../src/hw.h"
#include "../src/render.h"
#include "../src/level.h"

u16 key_read_dma(u16 sel);
u16 key_read_cpu(u16 sel, u8 n);
u32 hw_free_memory(void);

static u32 free_before, free_after;
static u8 pack_ok;
static u8 map_result[16];       /* level_read's answer per map found */

static const char hexd[] = "0123456789ABCDEF";

static int put_str(char *b, int n, const char *s)
{
    while (*s) b[n++] = *s++;
    return n;
}

static int put_num(char *b, int n, u32 v)
{
    char t[11];
    int k = 0;
    do t[k++] = (char)('0' + v % 10); while ((v /= 10) && k < 10);
    while (k) b[n++] = t[--k];
    return n;
}

static int put_hex(char *b, int n, u16 v)
{
    int s;
    for (s = 12; s >= 0; s -= 4) b[n++] = hexd[(v >> s) & 15];
    return n;
}

static void line(int y, const char *s, int shade)
{
    draw_text(2, y, s, shade);
}

void test_files(void)
{
    int i;
    free_before = hw_free_memory();
    pack_ok = (u8)pack_open();
    if (pack_ok)
        for (i = 0; i < level_count() && i < (int)sizeof map_result; i++)
            map_result[i] = (u8)level_read(i);
    free_after = hw_free_memory();
}

static int any_key(void)
{
    u8 r;
    for (r = 0; r < 10; r++) {
        u16 sel = (u16)(0x3ff & ~(1u << r));
        if ((u16)~key_read_dma(sel)) return 1;
    }
    return 0;
}

/* Only the DMA reads count for turning the page and quitting: on a real
 * Cybiko the CPU reads give noise (the first test there quit by itself,
 * taking it for Esc). */

/* wait up to secs seconds or until a key goes down */
static void wait_page(u16 secs)
{
    u32 start = hw_clock();
    while (any_key() && hw_clock() - start < (u32)secs * HW_HZ) hw_idle();
    while (!any_key() && hw_clock() - start < (u32)secs * HW_HZ) hw_idle();
}

static void page_files(void)
{
    char b[48];
    int n, i, y;
    const char *l;
    fill_rect(0, 0, SCREENW, SCREENH, 6);
    line(0, "DOOM TEST 1/2: FILES", 0);
    n = put_num(b, put_str(b, 0, "FREE MEMORY "), (free_before + 1023) / 1024);
    n = put_num(b, put_str(b, n, " KB, LEFT "), (free_after + 1023) / 1024);
    b[put_str(b, n, " KB")] = 0;
    line(8, b, 1);
    if (!pack_ok) n = put_str(b, 0, pack_error ? pack_error : "NO PACK");
    else {
        n = put_num(b, put_str(b, 0, "PACK OK, "), (u32)level_count());
        n = put_str(b, n, " MAPS");
        if (pack_error) n = put_str(b, put_str(b, n, ": "), pack_error);
    }
    b[n] = 0;
    line(15, b, pack_ok && !pack_error ? 1 : 0);
    n = put_num(b, put_str(b, 0, "NEEDS "), (pack_need_sprites + 1023) / 1024);
    n = put_num(b, put_str(b, n, " + "), (pack_need_map + 1023) / 1024);
    b[put_str(b, n, " KB")] = 0;
    line(22, b, 1);
    /* the maps: name and OK, READ or UNPACK, four to a line */
    y = 29;
    n = 0;
    for (i = 0; i < level_count() && i < (int)sizeof map_result; i++) {
        n = put_str(b, n, level_name(i));
        n = put_str(b, n, map_result[i] == 0 ? " OK  " : map_result[i] == 1 ? " READ " : " UNPK ");
        if (n > 30 || i + 1 == level_count()) {
            b[n] = 0;
            line(y, b, 1);
            y += 7;
            n = 0;
        }
    }
    line(y, "LAST FILE READS:", 2);
    y += 7;
    for (i = 0; y < 86 && (l = hw_file_log(i)) != NULL; i++, y += 7) line(y, l, 1);
    line(93, "ANY KEY OR 30 S: KEY TEST", 2);
    hw_present();
}

static const char *const key_names[] = {
    "UP", "DOWN", "LEFT", "RIGHT", "FIRE", "USE", "ESC", "RUN",
    "STRAFE", "SL", "SR", "MAP", "WEAPON", "HELP", "SELECT"
};

static void page_keys(void)
{
    u8 seen[3] = { 0, 0, 0 };
    u32 last_key = hw_clock(), esc_since = 0;
    /* the key that turned the page does not count */
    while (any_key() && hw_clock() - last_key < 2 * HW_HZ) hw_idle();
    for (;;) {
        char b[48];
        int n, r, k;
        u16 any = 0, keys;
        u8 esc = 0;
        u32 now;
        fill_rect(0, 0, SCREENW, SCREENH, 6);
        line(0, "DOOM TEST 2/2: KEYS DOWN, BY READ", 0);
        line(7, "ROW  CPU   X12      DMA (CYOS WAY)", 2);
        for (r = 0; r <= 10; r++) {
            /* rows 0-9 one at a time, then all of them at once */
            u16 sel = r < 10 ? (u16)(0x3ff & ~(1u << r)) : 0;
            u16 v[3];
            v[0] = (u16)~key_read_cpu(sel, 1);
            v[1] = (u16)~key_read_cpu(sel, 12);
            v[2] = (u16)~key_read_dma(sel);
            n = r < 10 ? put_num(b, put_str(b, 0, "  "), (u32)r) : put_str(b, 0, "ALL");
            for (k = 0; k < 3; k++) {
                n = put_hex(b, put_str(b, n, k == 0 ? "  " : k == 1 ? "  " : "     "), v[k]);
                if (v[k]) seen[k] = 1;
            }
            any |= v[2];
            if (r == 5 && (v[2] & (1u << 10))) esc = 1;
            b[n] = 0;
            line(14 + 6 * r, b, v[0] | v[1] | v[2] ? 0 : 1);
        }
        keys = hw_keys();
        n = put_str(b, 0, "DOOM KEYS:");
        for (k = 0; k < 15 && n < 34; k++)
            if (keys & (1u << k)) n = put_str(b, put_str(b, n, " "), key_names[k]);
        b[n] = 0;
        line(81, b, 0);
        n = put_str(b, 0, "SEEN: CPU ");
        n = put_str(b, n, seen[0] ? "YES" : "NO");
        n = put_str(b, n, ", X12 ");
        n = put_str(b, n, seen[1] ? "YES" : "NO");
        n = put_str(b, n, ", DMA ");
        n = put_str(b, n, seen[2] ? "YES" : "NO");
        b[n] = 0;
        line(87, b, 0);
        line(94, "HOLD ESC 1 S, OR NO KEY 60 S: QUIT", 2);
        hw_present();
        now = hw_clock();
        if (any) last_key = now;
        if (!esc) esc_since = 0;
        else if (!esc_since) esc_since = now;
        else if (now - esc_since > HW_HZ) return;
        if (now - last_key > 60 * HW_HZ) return;
        hw_idle();
    }
}

void test_main(void)
{
    render_init();
    page_files();
    wait_page(30);
    page_keys();
}
