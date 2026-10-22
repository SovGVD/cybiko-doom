/* PC test harness: runs the game without a screen. Input comes from a
 * script, and the screen is saved as PGM pictures.
 *
 *   doom_pc PACK.bin [--script FILE] [--frames N] [--shots DIR] [--every K]
 *
 * The script has lines "FIRST LAST KEYS": frames FIRST..LAST hold KEYS, a
 * list of letters: u d l r (arrows) f (fire) e (use) x (esc) s (run)
 * a (strafe left) b (strafe right) m (map) t (next weapon) 1-7 (weapons).
 * Each frame is one game tic (1/35 s). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "../src/hw.h"
#include "../src/render.h"

extern const u8 *pack;
void doom_main(void);

const u32 HW_HZ = 35;
static u32 frame, maxframes = 200, every = 10;
static const char *shotdir = NULL;
static jmp_buf done;
static struct { u32 a, b; u16 keys; u8 w; } script[256];
static int nscript;

u32 hw_clock(void) { return frame; }

u16 hw_keys(void)
{
    u16 k = 0;
    int i;
    for (i = 0; i < nscript; i++)
        if (frame >= script[i].a && frame <= script[i].b) k |= script[i].keys;
    return k;
}

u8 hw_weapon_key(void)
{
    int i;
    for (i = 0; i < nscript; i++)
        if (frame >= script[i].a && frame <= script[i].b && script[i].w) return script[i].w;
    return 0;
}

static void save(const char *path)
{
    FILE *f = fopen(path, "wb");
    int x, y;
    static const unsigned char lv[4] = { 230, 160, 90, 20 };
    if (!f) return;
    fprintf(f, "P5\n%d %d\n255\n", SCREENW, SCREENH);
    for (y = 0; y < SCREENH; y++)
        for (x = 0; x < SCREENW; x++) {
            int v = fb[y * ROWBYTES + x / 4] >> (6 - 2 * (x & 3)) & 3;
            fputc(lv[v], f);
        }
    fclose(f);
}

void hw_present(void)
{
    char path[512];
    if (shotdir && frame % every == 0) {
        snprintf(path, sizeof path, "%s/f%05u.pgm", shotdir, (unsigned)frame);
        save(path);
    }
    frame++;
    if (frame >= maxframes) longjmp(done, 1);
}

void hw_idle(void)
{
    frame++;
    if (frame >= maxframes) longjmp(done, 1);
}

void hw_init(void) {}
void hw_shutdown(void) {}
void hw_debug(const char *s) { (void)s; }

int main(int argc, char **argv)
{
    FILE *f;
    long n;
    u8 *buf;
    int i;
    if (argc < 2) {
        fprintf(stderr, "usage: %s PACK.bin [--script F] [--frames N] [--shots DIR] [--every K]\n", argv[0]);
        return 1;
    }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(n);
    if (fread(buf, 1, n, f) != (size_t)n) return 1;
    fclose(f);
    pack = buf;
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) maxframes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc) shotdir = argv[++i];
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) {
            FILE *s = fopen(argv[++i], "r");
            char line[256], keys[64];
            unsigned a, b;
            if (!s) { perror(argv[i]); return 1; }
            while (fgets(line, sizeof line, s) && nscript < 256) {
                char *p;
                u16 k = 0;
                u8 w = 0;
                keys[0] = 0;
                if (sscanf(line, "%u %u %63s", &a, &b, keys) < 2) continue;
                for (p = keys; *p; p++)
                    switch (*p) {
                    case 'u': k |= K_UP; break;
                    case 'd': k |= K_DOWN; break;
                    case 'l': k |= K_LEFT; break;
                    case 'r': k |= K_RIGHT; break;
                    case 'f': k |= K_FIRE; break;
                    case 'e': k |= K_USE; break;
                    case 'x': k |= K_ESC; break;
                    case 's': k |= K_RUN; break;
                    case 'a': k |= K_STRAFEL; break;
                    case 'b': k |= K_STRAFER; break;
                    case 'm': k |= K_MAP; break;
                    case 't': k |= K_NEXTW; break;
                    default:
                        if (*p >= '1' && *p <= '7') w = (u8)(*p - '0');
                    }
                script[nscript].a = a;
                script[nscript].b = b;
                script[nscript].keys = k;
                script[nscript].w = w;
                nscript++;
            }
            fclose(s);
        }
    }
    if (!setjmp(done)) doom_main();
    fprintf(stderr, "ran %u frames\n", (unsigned)frame);
    return 0;
}

void hw_present_rows(s16 rows)
{
    (void)rows;
    hw_present();
}
