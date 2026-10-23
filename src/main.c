/* The game loop, the title menu, the status bar and the automap. */
#include "game.h"
#include "spec.h"
#include "render.h"
#include "hw.h"

static u8 show_fps = 1, always_run;
static u8 detail = 3;           /* 0 high, 1 low, 2 lowest, 3 auto (title menu) */
static u16 fps10;               /* frames a second, times ten */

static const char *const skills[5] = {
    "TOO YOUNG TO DIE", "NOT TOO ROUGH", "HURT ME PLENTY", "ULTRA-VIOLENCE", "NIGHTMARE!"
};

static void clear_screen(int shade)
{
    fill_rect(0, 0, SCREENW, SCREENH, shade);
}

static void center_text(int y, const char *s, int shade)
{
    draw_text((SCREENW - text_width(s)) / 2, y, s, shade);
}

/* wait until all keys are up, so one press is not seen twice */
static void wait_release(void)
{
    while (hw_keys() & (K_USE | K_ESC | K_FIRE | K_UP | K_DOWN | K_LEFT | K_RIGHT | K_MAP))
        hw_idle();
}

static u16 wait_key(void)
{
    u16 k;
    wait_release();
    while (!(k = hw_keys())) hw_idle();
    return k;
}

/* ---- status bar ---- */

static void num3(int x, int y, s16 v)
{
    char buf[5];
    int i = 4;
    buf[4] = 0;
    if (v < 0) v = 0;
    do {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v && i);
    draw_big_text(x + i * 8 - 8, y, buf + i, 6);
}

/* what the status bar shows; it is drawn again only when this changes */
static s16 hud_shown[7];
static u8 hud_valid;

/* draw the status bar if it changed; 1 if it did */
static int render_hud(void)
{
    static const char *const wname[8] = { "FIST", "PISTOL", "SHOTGUN", "CHAINGUN", "ROCKETS",
                                           "PLASMA", "BFG", "SAW" };
    static const s8 wammo[8] = { -1, 0, 1, 0, 2, 3, 3, -1 };
    int y = VIEWH, k;
    s16 now[7];
    now[0] = player.weapon;
    now[1] = wammo[player.weapon] >= 0 ? player.ammo[(int)wammo[player.weapon]] : -1;
    now[2] = player.health;
    now[3] = player.armor;
    now[4] = player.cards;
    now[5] = show_fps ? (s16)(fps10 / 10) : -1;
    now[6] = 0;
    if (hud_valid) {
        for (k = 0; k < 7 && now[k] == hud_shown[k]; k++) ;
        if (k == 7) return 0;
    }
    for (k = 0; k < 7; k++) hud_shown[k] = now[k];
    hud_valid = 1;
    fill_rect(0, y, SCREENW, SCREENH - y, 2);
    fill_rect(0, y, SCREENW, 1, 6);
    if (now[1] >= 0) num3(0, y + 1, now[1]);
    draw_text(2, y + 11, wname[player.weapon], 6);
    num3(44, y + 1, player.health);
    draw_text(76, y + 1, "%", 6);
    draw_text(48, y + 11, "HEALTH", 6);
    num3(90, y + 1, player.armor);
    draw_text(122, y + 1, "%", 6);
    draw_text(94, y + 11, "ARMOR", 6);
    /* keys */
    {
        int k;
        for (k = 0; k < 3; k++)
            if (player.cards & (9 << k)) {
                fill_rect(132 + k * 5, y + 3, 4, 6, 6);
                fill_rect(133 + k * 5, y + 4, 2, 2, k == 0 ? 2 : k == 1 ? 0 : 4);
            }
    }
    if (show_fps) {
        draw_num(136, y + 11, (s16)(fps10 / 10), 2, 6);
        draw_text(148, y + 11, "F", 6);
    }
    return 1;
}

/* messages over the 3D view */
static void render_overlays(void)
{
    if (player.messagetics) {
        render_border_dirty();          /* the frame cleans it next time */
        fill_rect(0, 0, text_width(player.message) + 3, 7, 0);
        draw_text(1, 1, player.message, 6);
    }
    if (player.dead) {
        fill_rect(30, 34, 100, 15, 0);
        center_text(36, "YOU DIED", 6);
        center_text(42, "ENTER: RESTART", 6);
    }
}

/* ---- automap ---- */

void render_automap(void)
{
    s16 i;
    s32 cx = player.mo->x >> FRACBITS, cy = player.mo->y >> FRACBITS;
    fill_rect(0, 0, SCREENW, VIEWH, 0);
    for (i = 0; i < lvl.nlines; i++) {
        line_t *l = &lvl.lines[i];
        vertex_t *a, *b;
        int shade;
        if (!(l->flags & ML_MAPPED) && !player.powers[4]) continue;
        if (l->flags & ML_DONTDRAW) continue;
        a = &lvl.vertexes[l->v1];
        b = &lvl.vertexes[l->v2];
        shade = !(l->flags & ML_MAPPED) ? 2 : (l->sec[1] < 0 || !(l->flags & ML_TWOSIDED)) ? 6 : 3;
        if (l->special && (l->flags & ML_MAPPED)) shade = 4;
        draw_line(80 + (int)((a->x - cx) >> 4), VIEWH / 2 - (int)((a->y - cy) >> 4),
                  80 + (int)((b->x - cx) >> 4), VIEWH / 2 - (int)((b->y - cy) >> 4), shade);
    }
    {
        int dx = fcos(player.mo->angle) >> 11, dy = fsin(player.mo->angle) >> 11;
        draw_line(80 - dx, VIEWH / 2 + dy, 80 + dx, VIEWH / 2 - dy, 6);
        draw_line(80 + dx, VIEWH / 2 - dy, 80 + dy / 2, VIEWH / 2 + dx / 2, 6);
        draw_line(80 + dx, VIEWH / 2 - dy, 80 - dy / 2, VIEWH / 2 - dx / 2, 6);
    }
    draw_text(1, 1, lvl.name, 6);
}

/* ---- screens ---- */

static int title(int *map)
{
    int sel = 0;
    for (;;) {
        u16 k;
        clear_screen(0);
        fill_rect(0, 0, SCREENW, 27, 6);
        draw_big_text(48, 4, "DOOM", 0);
        draw_text(54, 19, "FOR CYBIKO", 1);
        draw_text(10, 28, sel == 0 ? ">" : " ", 6);
        draw_text(18, 28, "MAP", 6);
        draw_text(70, 28, level_name(*map), 6);
        draw_text(10, 35, sel == 1 ? ">" : " ", 6);
        draw_text(18, 35, "SKILL", 6);
        draw_text(70, 35, skills[skill], 6);
        draw_text(10, 42, sel == 2 ? ">" : " ", 6);
        draw_text(18, 42, "ALWAYS RUN", 6);
        draw_text(70, 42, always_run ? "ON" : "OFF", 6);
        draw_text(10, 49, sel == 3 ? ">" : " ", 6);
        draw_text(18, 49, "WEAPON", 6);
        draw_text(70, 49, show_weapon ? "SHOWN" : "HIDDEN (FASTER)", 6);
        draw_text(10, 56, sel == 4 ? ">" : " ", 6);
        draw_text(18, 56, "WALLS", 6);
        draw_text(70, 56, show_textures == 1 ? "TEXTURED" : show_textures ? "TEXTURED NEAR" : "FLAT (FASTEST)", 6);
        draw_text(10, 63, sel == 5 ? ">" : " ", 6);
        draw_text(18, 63, "DETAIL", 6);
        draw_text(70, 63, detail == 3 ? "AUTO" : detail == 2 ? "LOWEST" : detail ? "LOW" : "HIGH", 6);
        draw_text(10, 70, sel == 6 ? ">" : " ", 6);
        draw_text(18, 70, "COLUMNS", 6);
        draw_text(70, 70, interlace ? "ALTERNATE (FASTER)" : "ALL", 6);
        draw_text(10, 77, sel == 7 ? ">" : " ", 6);
        draw_text(18, 77, "VIEW", 6);
        draw_text(70, 77, view_size == 2 ? "SMALL (FASTEST)" : view_size ? "MEDIUM (FASTER)" : "FULL", 6);
        center_text(86, "ENTER: PLAY   ESC: QUIT", 4);
        center_text(93, "HELP KEY: FPS ON/OFF", 4);
        hw_present();
        k = wait_key();
        if (k & K_ESC) return 0;
        if (k & (K_USE | K_FIRE)) return 1;
        if (k & K_UP) sel = sel ? sel - 1 : 7;
        if (k & K_DOWN) sel = sel < 7 ? sel + 1 : 0;
        if (k & (K_LEFT | K_RIGHT)) {
            int d = (k & K_LEFT) ? -1 : 1;
            if (sel == 0) *map = (*map + d + level_count()) % level_count();
            if (sel == 1) skill = (u8)((skill + d + 5) % 5);
            if (sel == 2) always_run ^= 1;
            if (sel == 3) show_weapon ^= 1;
            if (sel == 4) show_textures = (u8)((show_textures + (d < 0 ? 2 : 1)) % 3);
            if (sel == 5) detail = (u8)((detail + (d < 0 ? 3 : 1)) % 4);
            if (sel == 6) interlace ^= 1;
            if (sel == 7) {
                view_size = (u8)((view_size + (d < 0 ? 2 : 1)) % 3);
                render_set_view();
            }
        }
        if (k & K_FPS) show_fps ^= 1;
    }
}

static int pct(s16 a, s16 b)
{
    return b ? (int)((s32)a * 100 / b) : 100;
}

static void intermission(u32 tics)
{
    char buf[8];
    s16 secs = (s16)(tics / TICRATE);
    clear_screen(0);
    fill_rect(0, 0, SCREENW, 16, 6);
    draw_text(4, 5, lvl.name, 0);
    draw_text(26, 5, "FINISHED", 0);
    draw_text(20, 26, "KILLS", 6);
    draw_num(100, 26, (s16)pct(player.kills, totalkills), 3, 6);
    draw_text(114, 26, "%", 6);
    draw_text(20, 36, "ITEMS", 6);
    draw_num(100, 36, (s16)pct(player.items, totalitems), 3, 6);
    draw_text(114, 36, "%", 6);
    draw_text(20, 46, "SECRET", 6);
    draw_num(100, 46, (s16)pct(player.secrets, totalsecrets), 3, 6);
    draw_text(114, 46, "%", 6);
    draw_text(20, 56, "TIME", 6);
    draw_num(92, 56, (s16)(secs / 60), 2, 6);
    buf[0] = ':';
    buf[1] = (char)('0' + (secs % 60) / 10);
    buf[2] = (char)('0' + secs % 10);
    buf[3] = 0;
    draw_text(100, 56, buf, 6);
    center_text(84, "ENTER: NEXT LEVEL", 4);
    hw_present();
    while (!(wait_key() & (K_USE | K_FIRE | K_ESC))) ;
}

static int next_map(int secret)
{
    const char *n = lvl.name;
    char want[5];
    int i;
    for (i = 0; i < 5; i++) want[i] = n[i];
    if (n[0] == 'E' && n[2] == 'M') {
        if (secret) want[3] = '9';
        else if (n[3] == '9') want[3] = n[1] == '1' ? '4' : n[1] == '2' ? '6' : n[1] == '3' ? '7' : '3';
        else if (n[3] == '8') return -1;
        else want[3] = (char)(n[3] + 1);
        i = level_find(want);
        if (i >= 0) return i;
        /* its file is missing: the next map there is */
    }
    /* MAPxx */
    if (n[0] == 'M') {
        int num = (n[3] - '0') * 10 + (n[4] ? n[4] - '0' : 0);
        (void)num;
    }
    return levelidx + 1 < level_count() ? levelidx + 1 : -1;
}

static void the_end(void)
{
    clear_screen(6);
    draw_big_text(44, 30, "THE END", 0);
    center_text(60, "YOU HAVE BEATEN THIS EPISODE", 1);
    center_text(84, "ENTER: TITLE", 2);
    hw_present();
    while (!(wait_key() & (K_USE | K_FIRE | K_ESC))) ;
}

/* pause: 1 = quit to the title */
static int pause_menu(void)
{
    fill_rect(30, 30, 100, 24, 0);
    fill_rect(30, 30, 100, 1, 6);
    fill_rect(30, 53, 100, 1, 6);
    center_text(34, "PAUSED", 6);
    center_text(42, "ENTER: GO ON", 6);
    center_text(48, "ESC: QUIT GAME", 6);
    hw_present();
    for (;;) {
        u16 k = wait_key();
        if (k & K_ESC) return 1;
        if (k & (K_USE | K_FIRE)) {
            wait_release();
            return 0;
        }
    }
}

static u16 buttons_from_keys(u16 k)
{
    u16 b = 0;
    if (k & K_UP) b |= BT_FORWARD;
    if (k & K_DOWN) b |= BT_BACK;
    if (k & K_LEFT) b |= BT_LEFT;
    if (k & K_RIGHT) b |= BT_RIGHT;
    if (k & K_FIRE) b |= BT_FIRE;
    if (k & K_USE) b |= BT_USE;
    if (((k & K_RUN) != 0) ^ always_run) b |= BT_RUN;
    if (k & K_STRAFE) b |= BT_STRAFE;
    if (k & K_STRAFEL) b |= BT_STRAFEL;
    if (k & K_STRAFER) b |= BT_STRAFER;
    if (k & K_NEXTW) b |= BT_NEXTWEAPON;
    return b;
}

static void start_level(int map)
{
    clear_screen(0);
    center_text(46, "LOADING...", 6);
    hw_present();
    render_border_dirty();
    game_start_level(map);
    hud_valid = 0;
}

/* play from map index map until the player quits or the episode ends */
static void play(int map)
{
    u32 last, fpsstart, frames = 0, tics_total = 0;
    u16 prevkeys = 0;
    u8 automap = 0;
    player_reset(0);
    start_level(map);
    low_detail = detail == 3 ? 0 : detail;
    last = fpsstart = hw_clock();
    wait_release();
    for (;;) {
        u32 now = hw_clock();
        u16 keys = hw_keys();
        u16 pressed = keys & ~prevkeys;
        s16 tics = (s16)(((now - last) * TICRATE) / HW_HZ);
        prevkeys = keys;
        if (pressed & K_ESC) {
            if (pause_menu()) return;
            render_border_dirty();
            last = hw_clock();
            continue;
        }
        if (pressed & K_MAP) {
            automap ^= 1;
            render_border_dirty();
        }
        if (pressed & K_FPS) show_fps ^= 1;
        if (pressed & K_TOGGLERUN) {
            always_run ^= 1;
            set_message(always_run ? "ALWAYS RUN ON" : "ALWAYS RUN OFF");
        }
        if (player.dead && (pressed & K_USE) && player.viewheight <= 6 * FRACUNIT) {
            player_reset(0);
            start_level(levelidx);
            last = hw_clock();
            continue;
        }
        if (tics < 1) {
            hw_idle();
            continue;
        }
        last += (u32)tics * HW_HZ / TICRATE;
        if (tics > 4) {
            tics = 4;
            last = now;
        }
        while (tics--) {
            u16 b = buttons_from_keys(keys);
            if (!(pressed & K_NEXTW)) b &= ~BT_NEXTWEAPON;
            game_tic(b, hw_weapon_key());
            pressed = 0;
            tics_total++;
            if (game_exit_requested()) break;
        }
        if (game_exit_requested()) {
            int secret = game_exit_requested() == 2, next;
            intermission(tics_total);
            next = next_map(secret);
            if (next < 0) {
                the_end();
                return;
            }
            player_reset(1);
            start_level(next);
            tics_total = 0;
            last = hw_clock();
            continue;
        }
        if (automap) render_automap();
        else render_view();
        render_overlays();
        if (render_hud()) hw_present();
        else hw_present_rows(automap || player.messagetics ? 0 : render_rows_from(), VIEWH);
        frames++;
        if (detail == 3 && !automap) {
            /* auto detail: low when a frame takes over 1/15 s, high again
             * under 1/24 s (the frame time is the time since this frame's
             * tics were run) */
            u32 took = hw_clock() - now;
            if (took > HW_HZ / 15) low_detail = 1;
            else if (took < HW_HZ / 24) low_detail = 0;
        }
        if (now - fpsstart >= HW_HZ) {
            fps10 = (u16)((frames * 10 * HW_HZ) / (now - fpsstart));
            frames = 0;
            fpsstart = now;
            {
                char buf[16];
                int i = 0;
                buf[i++] = 'f'; buf[i++] = 'p'; buf[i++] = 's'; buf[i++] = ' ';
                if (fps10 >= 100) buf[i++] = (char)('0' + fps10 / 100 % 10);
                buf[i++] = (char)('0' + fps10 / 10 % 10);
                buf[i++] = '.';
                buf[i++] = (char)('0' + fps10 % 10);
                buf[i++] = '\r';
                buf[i++] = '\n';
                buf[i] = 0;
                hw_debug(buf);
            }
        }
    }
}

void doom_main(void)
{
    int map = 0;
    render_init();
    if (!level_count()) {
        /* no pack: tell where it goes (see hw_cybiko.c) */
        clear_screen(6);
        draw_big_text(48, 8, "DOOM", 0);
        center_text(36, "NO DOOM.DAT OR NO MAPS", 1);
        center_text(50, "PUT DOOM.DAT AND THE MAP FILES", 2);
        center_text(58, "ON THE MP3 CARD OR THE CYBIKO", 2);
        center_text(84, "ANY KEY: QUIT", 2);
        hw_present();
        wait_key();
        return;
    }
    while (title(&map)) {
        play(map);
        map = levelidx;
    }
}
