/* One image for a CyOS .e module: code, read-only data, data and the
 * zeroed data all in one block. CyOS's loader only handles a small bss (its
 * own applications have a few KB at most), so the game's big tables are
 * stored as zeros in the image instead; the .app archive compresses them.
 * tools/cybiko_mkapp.py links this twice at different bases to find the
 * words the loader must relocate. */
OUTPUT_FORMAT("coff-h8300")
OUTPUT_ARCH(h8300s)
/* The H8S's 8 KB of on-chip RAM is much faster than the DRAM (one state an
 * access against three), so the game puts its screen buffer and its stack
 * there while it runs; hw_cybiko.c keeps CyOS's contents meanwhile. */
_fb = 0xffdc00;                 /* 4000 bytes */
__iram_stack_top = 0xfff900;    /* the stack grows down to the .iram data */
/* rt.s's fill routines run from 0xfff900 (hw_cybiko.c copies them there) */
_cell_run = 0xfff900 + (_cell_run_img - __iram_code);
_vline = 0xfff900 + (_vline_img - __iram_code);
_hw_present_rows = 0xfff900 + (_hw_present_rows_img - __iram_code);
_tline = 0xfff900 + (_tline_img - __iram_code);
INCLUDE cybiko/cyos.ld

SECTIONS
{
    .text : {
        *(.text)
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
    /DISCARD/ : { *(.comment) *(.stab) *(.stabstr) }
}
