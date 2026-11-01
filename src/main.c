/* The game loop, the title menu, the status bar and the automap. */
#include "game.h"
#include "spec.h"
#include "render.h"
#include "hw.h"
#include "sound.h"

static u8 show_fps = 1, always_run;
static u8 hit_flash = 1;        /* invert the screen briefly on damage (title menu) */
static u8 detail = 3;           /* 0 high, 1 low, 2 lowest, 3 auto (title menu) */
static u16 fps10;               /* frames a second, times ten */

static const char *const skills[5] = {
    "TOO YOUNG TO DIE", "NOT TOO ROUGH", "HURT ME PLENTY", "ULTRA-VIOLENCE", "NIGHTMARE!"
};

static void clear_screen(int shade)
{
    fill_rect(0, 0, SCREENW, SCREENH, shade);
    fb_merge = 0;
}

static void center_text(int y, const char *s, int shade)
{
    draw_text((SCREENW - text_width(s)) / 2, y, s, shade);
}

/* wait until all keys are up, so one press is not seen twice */
static void wait_release(void)
{
    while (hw_keys() & (K_USE | K_ESC | K_FIRE | K_UP | K_DOWN | K_LEFT | K_RIGHT | K_MAP)) {
        sound_update();
        hw_idle();
    }
}

static u16 wait_key(void)
{
    u16 k;
    wait_release();
    while (!(k = hw_keys())) {
        sound_update();
        hw_idle();
    }
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

/* ---- the face (Doom's status bar logic, st_stuff.c) ----
 * face_get(i): per pain level 0-4, FACE_STRAIGHT 3 ways, turned right,
 * turned left, ouch, evil grin, rampage; then god mode and dead
 * (tools/wad2cy.py FACES). */
#define FACE_STRIDE 8
#define FACE_TURN 3             /* +1: turned left */
#define FACE_OUCH 5
#define FACE_EVIL 6
#define FACE_RAMPAGE 7
#define FACE_GOD 40
#define FACE_DEAD 41
#define FACE_MUCHPAIN 20

static s16 face, face_count, face_priority, face_oldhealth = -1, face_attackdown = -1;
static u8 face_owned[8];
static u16 face_seed = 1;

static int face_pain(void)
{
    s16 h = player.health > 100 ? 100 : player.health;
    return FACE_STRIDE * ((100 - h) * 5 / 101);
}

/* once a tic, with the tic's buttons */
static void face_tic(u16 buttons)
{
    int k;
    if (face_oldhealth < 0) face_oldhealth = player.health;
    if (face_priority < 10 && player.health <= 0) {
        face_priority = 9;
        face = FACE_DEAD;
        face_count = 1;
    }
    if (face_priority < 9 && player.bonuscount) {
        /* a new weapon: the evil grin */
        for (k = 0; k < 8; k++)
            if (player.owned[k] != face_owned[k]) {
                face_owned[k] = player.owned[k];
                if (player.owned[k]) {
                    face_priority = 8;
                    face_count = 2 * TICRATE;
                    face = (s16)(face_pain() + FACE_EVIL);
                }
            }
    }
    if (face_priority < 8 && player.damagecount && player.attacker && player.attacker != player.mo) {
        /* hurt by someone: ouch, or look at them */
        face_priority = 7;
        face_count = TICRATE;
        if (face_oldhealth - player.health > FACE_MUCHPAIN)
            face = (s16)(face_pain() + FACE_OUCH);
        else {
            angle_t bad = point_to_angle(player.attacker->x - player.mo->x,
                                         player.attacker->y - player.mo->y);
            angle_t diff = bad - player.mo->angle;
            int right = diff > ANG180;
            if (right) diff = 0 - diff;
            face = (s16)(face_pain() + (diff < ANG45 ? FACE_RAMPAGE : right ? FACE_TURN : FACE_TURN + 1));
        }
    }
    if (face_priority < 7 && player.damagecount) {
        /* hurt by the floor or a crusher */
        face_count = TICRATE;
        if (face_oldhealth - player.health > FACE_MUCHPAIN) {
            face_priority = 7;
            face = (s16)(face_pain() + FACE_OUCH);
        } else {
            face_priority = 6;
            face = (s16)(face_pain() + FACE_RAMPAGE);
        }
    }
    if (face_priority < 6) {
        /* firing for two seconds: rampage */
        if (buttons & BT_FIRE) {
            if (face_attackdown < 0) face_attackdown = 2 * TICRATE;
            else if (!--face_attackdown) {
                face_priority = 5;
                face = (s16)(face_pain() + FACE_RAMPAGE);
                face_count = 1;
                face_attackdown = 1;
            }
        } else
            face_attackdown = -1;
    }
    if (face_priority < 5 && (player.cheat || player.powers[0])) {
        face_priority = 4;
        face = FACE_GOD;
        face_count = 1;
    }
    if (!face_count) {
        /* looking around */
        face_seed = (u16)(face_seed * 25173 + 13849);
        face = (s16)(face_pain() + (face_seed >> 8) % 3);
        face_count = TICRATE / 2;
        face_priority = 0;
    }
    face_count--;
    face_oldhealth = player.health;
}

/* a new level or a new life: the face starts over */
static void face_reset(void)
{
    int k;
    for (k = 0; k < 8; k++) face_owned[k] = player.owned[k];
    face_count = face_priority = 0;
    face_oldhealth = face_attackdown = -1;
    face = 0;
}

/* a face picture: width (even), height, then a nibble a pixel, the high
 * one first (0 white .. 3 black, 15 transparent) */
static void draw_face(int x, int y, const u8 *f)
{
    int w = f[0], h = f[1], i, j;
    f += 2;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i += 2, f++) {
            if ((*f >> 4) != 15) fill_rect(x + i, y + j, 1, 1, (*f >> 4) * 2);
            if ((*f & 15) != 15) fill_rect(x + i + 1, y + j, 1, 1, (*f & 15) * 2);
        }
}

/* ---- the status bar ----
 * 20 rows under the view: ammo, health, the face, armor, and on the right
 * the arms owned (2-7), the keys, the frame rate and the weapon. Doom's
 * layout without its frames. */

/* what the status bar shows; it is drawn again only when this changes */
#define HUD_ITEMS 8
static s16 hud_shown[HUD_ITEMS];
static u8 hud_valid;

/* draw the status bar if it changed; 1 if it did */
static int render_hud(void)
{
    static const char *const wname[8] = { "FIST", "PISTOL", "SHOTGUN", "CHAINGUN", "ROCKETS",
                                           "PLASMA", "BFG", "SAW" };
    static const s8 wammo[8] = { -1, 0, 1, 0, 2, 3, 3, -1 };
    int y = VIEWH, k;
    s16 now[HUD_ITEMS];
    const u8 *f;
    now[0] = player.weapon;
    now[1] = wammo[player.weapon] >= 0 ? player.ammo[(int)wammo[player.weapon]] : -1;
    now[2] = player.health;
    now[3] = player.armor;
    now[4] = player.cards;
    now[5] = show_fps ? (s16)(fps10 / 10) : -1;
    now[6] = 0;
    for (k = 0; k < 8; k++) now[6] |= (s16)(player.owned[k] << k);
    now[7] = face;
    if (hud_valid) {
        for (k = 0; k < HUD_ITEMS - 1 && now[k] == hud_shown[k]; k++) ;
        if (k == HUD_ITEMS - 1) {
            /* only the face (it changes twice a second): just its cell */
            if (now[7] == hud_shown[7]) return 0;
            hud_shown[7] = now[7];
            fill_rect(72, y + 1, 16, SCREENH - y - 1, 2);
            f = face_get(face);
            if (f) draw_face(80 - f[0] / 2, y + 3, f);
            return 1;
        }
    }
    for (k = 0; k < HUD_ITEMS; k++) hud_shown[k] = now[k];
    hud_valid = 1;
    fill_rect(0, y, SCREENW, SCREENH - y, 2);
    fill_rect(0, y, SCREENW, 1, 6);
    fill_rect(35, y + 1, 1, SCREENH - y - 1, 4);
    fill_rect(124, y + 1, 1, SCREENH - y - 1, 4);
    if (now[1] >= 0) num3(2, y + 3, now[1]);
    draw_text(5, y + 14, "AMMO", 6);
    num3(37, y + 3, player.health);
    draw_text(64, y + 3, "%", 6);
    draw_text(40, y + 14, "HEALTH", 6);
    f = face_get(face);
    if (f) draw_face(80 - f[0] / 2, y + 3, f);
    num3(90, y + 3, player.armor);
    draw_text(117, y + 3, "%", 6);
    draw_text(93, y + 14, "ARMOR", 6);
    /* the arms: 2 pistol .. 7 BFG, dark when owned */
    for (k = 1; k <= 6; k++) {
        char d[2];
        d[0] = (char)('1' + k);
        d[1] = 0;
        draw_text(127 + (k - 1) * 4, y + 2, d, player.owned[k] ? 6 : 4);
    }
    /* keys (card or skull) */
    for (k = 0; k < 3; k++)
        if (player.cards & (9 << k)) fill_rect(152 + k * 3, y + 2, 2, 5, 6);
    if (show_fps) {
        draw_num(127, y + 8, (s16)(fps10 / 10), 2, 6);
        draw_text(136, y + 8, "FPS", 6);
    }
    draw_text(127, y + 14, wname[player.weapon], 6);
    return 1;
}

/* messages over the 3D view */
static void render_overlays(void)
{
    if (player.messagetics) {
        /* at the bottom, over the status bar: easier to see on the
         * Cybiko's screen than at the top (r12freon) */
        s16 w = (s16)(text_width(player.message) + 3);
        render_border_dirty();          /* the frame cleans it next time */
        fill_rect(0, VIEWH - 7, w, 7, 0);
        draw_text(1, VIEWH - 6, player.message, 6);
        render_drawn_over(0, VIEWH - 7, w, 7);
    }
    if (player.dead) {
        fill_rect(30, 34, 100, 15, 0);
        center_text(36, "YOU DIED", 6);
        center_text(42, "ENTER: RESTART", 6);
        render_drawn_over(30, 34, 100, 15);
    }
}

/* ---- automap ---- */

void render_automap(void)
{
    s16 i;
    s32 cx = player.mo->x >> FRACBITS, cy = player.mo->y >> FRACBITS;
    fill_rect(0, 0, SCREENW, VIEWH, 0);
    fb_merge = 0;
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
    int sel = 0, i;
    for (;;) {
        u16 k;
        const char *val[11];
        static const char *const names[11] = {
            "MAP", "SKILL", "ALWAYS RUN", "WEAPON", "WALLS", "SMOOTHING", "DETAIL",
            "COLUMNS", "VIEW", "HIT FLASH", "SOUND"
        };
        val[0] = level_name(*map);
        val[1] = skills[skill];
        val[2] = always_run ? "ON" : "OFF";
        val[3] = show_weapon ? "SHOWN" : "HIDDEN (FASTER)";
        val[4] = show_textures == 1 ? "TEXTURED" : show_textures ? "TEXTURED NEAR" : "FLAT (FASTEST)";
        val[5] = smooth_near ? "NEAR WALLS" : "OFF (FASTER)";
        val[6] = detail == 3 ? "AUTO" : detail == 2 ? "LOWEST" : detail ? "LOW" : "HIGH";
        val[7] = interlace ? "ALTERNATE (FASTER)" : "ALL";
        val[8] = view_size == 2 ? "SMALL (FASTEST)" : view_size ? "MEDIUM (FASTER)" : "FULL";
        val[9] = hit_flash ? "ON" : "OFF";
        val[10] = !sound_loaded() ? "NO DOOM_SND.DAT" : sound_on ? "SPEAKER" : "OFF";
        clear_screen(0);
        fill_rect(0, 0, SCREENW, 24, 6);
        draw_big_text(48, 2, "DOOM", 0);
        draw_text(54, 17, "FOR CYBIKO", 1);
        for (i = 0; i < 11; i++) {
            draw_text(10, 25 + 6 * i, sel == i ? ">" : " ", 6);
            draw_text(18, 25 + 6 * i, names[i], 6);
            draw_text(70, 25 + 6 * i, val[i], 6);
        }
        center_text(93, "ENTER: PLAY  ESC: QUIT  HELP: FPS", 4);
        hw_present();
        k = wait_key();
        if (k & K_ESC) return 0;
        if (k & (K_USE | K_FIRE)) return 1;
        if (k & K_UP) sel = sel ? sel - 1 : 10;
        if (k & K_DOWN) sel = sel < 10 ? sel + 1 : 0;
        if (k & (K_LEFT | K_RIGHT)) {
            int d = (k & K_LEFT) ? -1 : 1;
            if (sel == 0) *map = (*map + d + level_count()) % level_count();
            if (sel == 1) skill = (u8)((skill + d + 5) % 5);
            if (sel == 2) always_run ^= 1;
            if (sel == 3) show_weapon ^= 1;
            if (sel == 4) show_textures = (u8)((show_textures + (d < 0 ? 2 : 1)) % 3);
            if (sel == 5) smooth_near ^= 1;
            if (sel == 6) detail = (u8)((detail + (d < 0 ? 3 : 1)) % 4);
            if (sel == 7) interlace ^= 1;
            if (sel == 8) {
                view_size = (u8)((view_size + (d < 0 ? 2 : 1)) % 3);
                render_set_view();
            }
            if (sel == 9) hit_flash ^= 1;
            if (sel == 10) {
                sound_on ^= 1;
                sound_start(SFX_PISTOL);
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
    render_drawn_over(30, 30, 100, 24);
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
    if (k & K_PREVW) b |= BT_PREVWEAPON;
    return b;
}

/* 0 if the map does not load: then it says so, with the last file reads,
 * and waits for a key */
static int start_level(int map)
{
    int i;
    const char *l;
    screen_xor = 0;
    sound_stop();
    clear_screen(0);
    center_text(46, "LOADING...", 6);
    hw_present();
    render_border_dirty();
    hud_valid = 0;
    face_reset();
    if (game_start_level(map)) return 1;
    clear_screen(6);
    center_text(8, "THE MAP DOES NOT LOAD", 0);
    for (i = 0; i < 8 && (l = hw_file_log(i)) != NULL; i++)
        draw_text(2, 20 + 7 * i, l, 1);
    center_text(92, "ANY KEY: BACK TO THE TITLE", 2);
    hw_present();
    wait_key();
    wait_release();
    return 0;
}

/* play from map index map until the player quits or the episode ends */
static void play(int map)
{
    u32 last, fpsstart, frames = 0, tics_total = 0;
    u16 prevkeys = 0;
    u8 automap = 0;
    s16 seen_damage = 0;
    u32 flash_until = 0;
    player_reset(0);
    if (!start_level(map)) return;
    low_detail = detail == 3 ? 0 : detail;
    last = fpsstart = hw_clock();
    wait_release();
    for (;;) {
        u32 now = hw_clock();
        u16 keys = hw_keys();
        u16 pressed = keys & ~prevkeys;
        s16 tics = (s16)(((now - last) * TICRATE) / HW_HZ);
        prevkeys = keys;
        sound_update();
        if (pressed & K_ESC) {
            screen_xor = 0;
            sound_stop();
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
        if (player.dead && (pressed & (K_USE | K_FIRE)) && player.viewheight <= 6 * FRACUNIT) {
            player_reset(0);
            if (!start_level(levelidx)) return;
            seen_damage = 0;
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
            if (!(pressed & K_PREVW)) b &= ~BT_PREVWEAPON;
            game_tic(b, hw_weapon_key());
            face_tic(b);
            pressed = 0;
            tics_total++;
            if (game_exit_requested()) break;
        }
        if (game_exit_requested()) {
            int secret = game_exit_requested() == 2, next;
            screen_xor = 0;
            intermission(tics_total);
            next = next_map(secret);
            if (next < 0) {
                the_end();
                return;
            }
            player_reset(1);
            if (!start_level(next)) return;
            seen_damage = 0;
            tics_total = 0;
            last = hw_clock();
            continue;
        }
        /* the hit flash: the screen inverted for 1/8 s when the player is
         * hurt (r12freon's idea; the FSTN screen keeps a short one) */
        {
            u16 x;
            u8 changed, hud;
            if (player.damagecount > seen_damage && hit_flash) flash_until = now + HW_HZ / 8;
            seen_damage = player.damagecount;
            x = (s32)(flash_until - now) > 0 ? 0xffff : 0;
            changed = x != screen_xor;
            screen_xor = x;
            if (automap) render_automap();
            else render_view();
            render_overlays();
            /* the status bar's rows go too when it changed (the face
             * looks around twice a second) */
            hud = (u8)render_hud();
            if (changed) hw_present();
            else hw_present_rows(automap || player.messagetics ? 0 : render_rows_from(),
                                 hud ? SCREENH : VIEWH);
        }
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

/* "text" and n / 1024 rounded up, then "KB", at b + n; the new length */
static int put_kb(char *b, int n, const char *text, u32 bytes)
{
    char t[8];
    int k = 0;
    u32 kb = (bytes + 1023) / 1024;
    while (*text) b[n++] = *text++;
    do t[k++] = (char)('0' + kb % 10); while ((kb /= 10) && k < 7);
    while (k) b[n++] = t[--k];
    b[n++] = ' ';
    b[n++] = 'K';
    b[n++] = 'B';
    return n;
}

void doom_main(void)
{
    int map = 0;
    render_init();
    if (!level_count()) {
        /* no pack: say why and where it goes (see hw_cybiko.c), with
         * what the last file reads did */
        int i;
        const char *l;
        clear_screen(6);
        draw_big_text(48, 2, "DOOM", 0);
        center_text(16, pack_error ? pack_error : "NO MAPS", 0);
        center_text(24, "PUT DOOM.DAT AND THE MAP FILES", 2);
        center_text(30, "ON THE MP3 CARD OR THE CYBIKO", 2);
        if (pack_need_sprites) {
            char b[40];
            int n = 0;
            n = put_kb(b, n, "NEEDS ", pack_need_sprites);
            if (pack_need_map) n = put_kb(b, n, " + ", pack_need_map);
            b[n] = 0;
            center_text(36, b, 1);
        }
        for (i = 0; i < 6 && (l = hw_file_log(i)) != NULL; i++)
            draw_text(2, 44 + 7 * i, l, 1);
        center_text(92, "ANY KEY: QUIT", 2);
        hw_present();
        wait_key();
        return;
    }
    while (title(&map)) {
        play(map);
        screen_xor = 0;
        sound_stop();
        map = levelidx;
    }
}
