/* One image for a CyOS .e module: code, read-only data, data and the
 * zeroed data all in one block. CyOS's loader only handles a small bss (its
 * own applications have a few KB at most), so the game's big tables are
 * stored as zeros in the image instead; the .app archive compresses them.
 * tools/cybiko_mkapp.py links this twice at different bases to find the
 * words the loader must relocate. */
OUTPUT_FORMAT("coff-h8300")
OUTPUT_ARCH(h8300s)
/* The H8S's 8 KB of on-chip RAM is much faster than the DRAM (one state an
 * access against three), so the game puts its stack, the renderer's hot
 * tables and its hot code there while it runs (hw_cybiko.c copies the code
 * in and keeps CyOS's contents meanwhile):
 *   0xffdc00 .iramfn, the renderer's hot C functions (up to 4000 bytes)
 *   0xffeba0 .iram, the tables
 *   ...      the stack, down from 0xfff800
 *   0xfff800 rt.s's fill routines and the multiply helper
 * The code is linked at its place in the image; the names the callers use
 * are the copies' addresses. */
__iram_stack_top = 0xfff800;
_cell_run = 0xfff800 + (_cell_run_img - __iram_code);
_vline = 0xfff800 + (_vline_img - __iram_code);
_hw_present_rows = 0xfff800 + (_hw_present_rows_img - __iram_code);
_tline = 0xfff800 + (_tline_img - __iram_code);
___mulsi3 = 0xfff800 + (___mulsi3_img - __iram_code);
_div32_16 = 0xfff800 + (_div32_16_img - __iram_code);
_ldiv32_16 = 0xfff800 + (_ldiv32_16_img - __iram_code);
_dot = 0xffdc00 + (_dot_img - __iramfn);
_tex_piece = 0xffdc00 + (_tex_piece_img - __iramfn);
_render_columns = 0xffdc00 + (_render_columns_img - __iramfn);
_draw_cells = 0xffdc00 + (_draw_cells_img - __iramfn);
_clip_seg = 0xffdc00 + (_clip_seg_img - __iramfn);
INCLUDE cybiko/cyos.ld

SECTIONS
{
    .text : {
        *(.text)
        . = ALIGN(2);
        __iramfn = .;
        *(.iramfn)
        __iramfn_end = .;
        *(.rodata)
        *(.rdata)
        *(.data)
        . = ALIGN(4);
        *(.bss)
        *(COMMON)
        . = ALIGN(4);
        __image_end = .;
    }
    .bss : {
        __bss_start = .;
        __bss_end = .;
    }
    .iram 0xffeba0 (NOLOAD) : {
        *(.iram)
        __iram_end = .;
    }
    /* rt.s's tline sets the low byte of tex_lut's address */
    ASSERT((_tex_luts & 31) == 0, "tex_luts must be at a 32-byte boundary")
    ASSERT(__iramfn_end - __iramfn <= 0xffeba0 - 0xffdc00, ".iramfn does not fit under .iram")
    ASSERT(__iram_code_end - __iram_code <= 0xfffc00 - 0xfff800, "rt.s's on-chip code does not fit")
    /DISCARD/ : { *(.comment) *(.stab) *(.stabstr) }
}
