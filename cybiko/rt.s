; Runtime helpers for gcc 3.4 -ms on the H8S (its libgcc is H8/300 only),
; and the fixed-point divisions the game uses.
; Calling convention: arguments in er0, er1, er2; result in er0;
; er0-er3 may be changed, er4-er6 must be kept.
        .h8300s
        .section .text

; u8 irq_off(void): mask interrupts, return the old CCR
        .global _irq_off
_irq_off:
        stc     ccr,r0l
        orc     #0x80,ccr
        rts

; void run_on_stack(void (*fn)(void), void *top): call fn with the stack
; pointer at top, then go back to the caller's stack (kept in cyos_sp)
        .global _run_on_stack
_run_on_stack:
        stm.l   er4-er6,@-er7
        mov.l   er7,er4
        mov.l   er7,@_cyos_sp
        mov.l   er1,er7
        jsr     @er0
        mov.l   er4,er7
        ldm.l   @er7+,er4-er6
        rts

; long on_cyos_stack(long (*fn)(void *), void *arg): fn(arg) on the stack
; run_on_stack was called on, below what is in use there
        .global _on_cyos_stack
_on_cyos_stack:
        stm.l   er4-er6,@-er7
        mov.l   er7,er4
        mov.l   @_cyos_sp,er7
        mov.l   er0,er2
        mov.l   er1,er0
        jsr     @er2
        mov.l   er4,er7
        ldm.l   @er7+,er4-er6
        rts

        .section .data
        .align 2
_cyos_sp:
        .long   0
        .section .text

; void irq_restore(u8 ccr)
        .global _irq_restore
_irq_restore:
        ldc     r0l,ccr
        rts

; unsigned 32 / 32: quotient er0, remainder er2
        .global ___udivsi3
___udivsi3:
        mov.w   e1,e1
        bne     5f
        ; divisor fits in 16 bits: two divxu steps
        mov.w   e0,r2
        extu.l  er2
        divxu.w r1,er2
        mov.w   e2,e0
        divxu.w r1,er0
        mov.w   e0,r3                   ; remainder
        extu.l  er3
        mov.w   r2,e0
        mov.l   er3,er2
        rts
5:      sub.l   er2,er2
        mov.l   #32,er3
6:      shll.l  er0
        rotxl.l er2
        cmp.l   er1,er2
        blo     7f
        sub.l   er1,er2
        inc.l   #1,er0
7:      dec.l   #1,er3
        bne     6b
        rts

        .global ___umodsi3
___umodsi3:
        jsr     @___udivsi3
        mov.l   er2,er0
        rts

        .global ___divsi3
___divsi3:
        push.l  er4
        sub.l   er4,er4
        mov.l   er0,er0
        bpl     1f
        neg.l   er0
        not.b   r4l
1:      mov.l   er1,er1
        bpl     2f
        neg.l   er1
        not.b   r4l
2:      jsr     @___udivsi3
        mov.b   r4l,r4l
        beq     3f
        neg.l   er0
3:      pop.l   er4
        rts

        .global ___modsi3
___modsi3:
        push.l  er4
        sub.l   er4,er4
        mov.l   er0,er0
        bpl     1f
        neg.l   er0
        not.b   r4l
1:      mov.l   er1,er1
        bpl     2f
        neg.l   er1
2:      jsr     @___udivsi3
        mov.l   er2,er0
        mov.b   r4l,r4l
        beq     3f
        neg.l   er0
3:      pop.l   er4
        rts

; 32 x 32 -> low 32
        .global ___mulsi3
___mulsi3:
        mov.l   er0,er2
        mov.l   er1,er3
        mulxu.w r1,er0                  ; a_lo * b_lo
        mov.w   e3,r3
        mulxu.w r2,er3                  ; b_hi * a_lo
        mov.w   e2,r2
        mulxu.w r1,er2                  ; a_hi * b_lo
        add.w   r3,r2
        add.w   r2,e0
        rts

; void mset(void *d, u8 c, u32 n) and memset(d, c, n)
        .global _mset
        .global _memset
_memset:
_mset:
        mov.l   er0,er3
        mov.l   er2,er2
        beq     2f
1:      mov.b   r1l,@er3
        inc.l   #1,er3
        dec.l   #1,er2
        bne     1b
2:      rts

; void mcpy(void *d, const void *s, u32 n) and memcpy(d, s, n)
        .global _mcpy
        .global _memcpy
_memcpy:
_mcpy:
        mov.l   er0,er3
        mov.l   er2,er2
        beq     2f
        push.l  er4
1:      mov.b   @er1+,r4l
        mov.b   r4l,@er3
        inc.l   #1,er3
        dec.l   #1,er2
        bne     1b
        pop.l   er4
2:      rts

; The routines from here to __iram_code_end fill the screen. hw_init copies
; them into the on-chip RAM, where the H8S fetches an instruction word in one
; state instead of three; link.ld gives cell_run and vline their addresses
; there. So within them, calls are relative (bsr).
        .align 1
        .global __iram_code
__iram_code:
; void cell_run(const u8 *c, s16 r, s16 e): one run of sprite cells in a
; column (see render.c, which has the same in C for the PC). Cells c are
; for cell rows r..e-1; rowtab[] gives their screen rows, cr_tab[] their
; shades; cells of one shade next to each other are filled as one span.
;   er1 rowtab pointer, er2 cell pointer, e3 cells left,
;   r3h cr_ebase, r3l shade of the open span, r4 its top, e4 its bottom,
;   r5 cr_lo, e5 cr_hi, er6 cr_tab index, r0 / e0 this cell's top / bottom
        .global _cell_run_img
_cell_run_img:
        stm.l   er4-er6,@-er7
        mov.w   r2,r3
        sub.w   r1,r3
        mov.w   r3,e3
        mov.l   er0,er2
        exts.l  er1
        shll.l  er1
        add.l   #_rowtab,er1
        mov.w   @_cr_lo:32,r5
        mov.w   @_cr_hi:32,e5
        mov.b   @_cr_ebase:32,r3h
        sub.b   r3l,r3l
        sub.w   r4,r4
        mov.w   #-1,e4
        sub.l   er6,er6
        mov.w   e3,e3
        beq     .Ldone
.Lcell:
        mov.w   @er1,r0
        mov.w   @(2,er1),e0
        dec.w   #1,e0
        cmp.w   r5,e0
        blt     .Lnext
        cmp.w   e5,r0
        bgt     .Ldone
        cmp.w   r0,e0
        blt     .Lnext
        cmp.w   r5,r0
        bge     1f
        mov.w   r5,r0
1:      cmp.w   e5,e0
        ble     2f
        mov.w   e5,e0
2:      mov.b   @er2,r6l
        and.b   #63,r6l
        or.b    r3h,r6l
        mov.b   @(_cr_tab:32,er6),r6l
        cmp.b   r3l,r6l
        bne     .Lnew
        mov.w   e4,e6
        inc.w   #1,e6
        cmp.w   e6,r0
        bne     .Lnew
        sub.w   e6,e6
        mov.w   e0,e4
        bra     .Lnext
.Lnew:  sub.w   e6,e6
        cmp.w   r4,e4
        blt     3f
        bsr     _cr_fill:16
3:      mov.w   r0,r4
        mov.w   e0,e4
        mov.b   r6l,r3l
.Lnext: adds    #1,er2
        adds    #2,er1
        dec.w   #1,e3
        bne     .Lcell
.Ldone: cmp.w   r4,e4
        blt     4f
        bsr     _cr_fill:16
4:      ldm.l   @er7+,er4-er6
        rts

; void vline(s16 x, s16 y1, s16 y2, u8 shade): fill column x (of 80, or 40
; in low detail) from
; row y1 to y2 (0 <= y1) with a dithered shade (render.c has it in C too)
        .global _vline_img
_vline_img:
        cmp.w   r1,r2
        blt     9f
        push.l  er4
        mov.w   r1,r4
        mov.w   r2,e4
        mov.b   @(11,er7),r3l
        mov.w   r0,r1
        mov.b   #0xff,r2l               ; low detail: column x is byte x of fb
        mov.b   @_low_detail:32,r2h
        bne     2f
        shar.w  r1                      ; high: half of byte x/2 of fb (even x)
        mov.b   #0xf0,r2l               ; or of fb2 (odd x), see render.c
        btst    #0,r0l
        beq     2f
        mov.b   #0x0f,r2l
        exts.l  er1
        add.l   #_fb2,er1
        bra     1f
2:      exts.l  er1
        add.l   #_fb,er1
1:      mov.l   er1,@_cr_col:32
        mov.b   r2l,@_cr_mask:32
        bsr     _cr_fill:16
        pop.l   er4
9:      rts

; void tline(s16 x, s16 y1, s16 y2): a textured column (render.c has it in
; C too): rows y1..y2 of column x (of 80, or 40 in low detail) from the
; texels at tl_col, texel (tl_v >> 8) & tl_hmask, tl_v += tl_step a row;
; tl_lut[row & 1][texel] is the dither byte. A texture column is aligned
; to its size (level.c) and the luts to 32 bytes, so a texel's address is
; the column's with the low bits set, and a dither byte's is the lut's
; with the low byte set (parity | texel).
;   er2 column, er3 lut, r1h the lut's low byte | parity (0 / 16),
;   r1l hmask, er4 screen byte, r5l our bits of it, r6 v, e6 step,
;   e5 rows left, r0l texel index, r0h texel / dither byte
        .global _tline_img
_tline_img:
        cmp.w   r1,r2
        bge     0f
        rts
0:      stm.l   er4-er6,@-er7
        sub.w   r1,r2
        inc.w   #1,r2
        mov.w   r2,e5
        mov.w   r1,r3
        extu.l  er3
        mov.w   #40,r4
        mulxu.w r4,er3
        mov.w   r0,r4
        mov.b   @_low_detail:32,r5h
        bne     4f
        shar.w  r4
        btst    #0,r0l                  ; odd columns are in fb2
        beq     4f
        exts.l  er4
        add.l   er3,er4
        add.l   #_fb2,er4
        bra     5f
4:      exts.l  er4
        add.l   er3,er4
        add.l   #_fb,er4
5:      mov.l   @_tl_lut:32,er3
        mov.b   r3l,r1h                 ; low byte of the lut
        btst    #0,r1l
        beq     2f
        or.b    #16,r1h                 ; odd first row
2:      mov.w   @_tl_v:32,r6
        mov.w   @_tl_step:32,e6
        mov.l   @_tl_col:32,er2
        mov.b   @_tl_hmask:32,r1l
        mov.b   r5h,r5h
        beq     7f
        cmp.b   #2,r5h
        beq     9f
; low detail: a column is a whole byte, two rows an iteration
5:      mov.b   r6h,r0l
        and.b   r1l,r0l
        or.b    r0l,r2l
        mov.b   @er2,r0h
        xor.b   r0l,r2l
        or.b    r1h,r0h
        mov.b   r0h,r3l
        mov.b   @er3,r0h
        mov.b   r0h,@er4
        add.w   e6,r6
        dec.w   #1,e5
        beq     8f
        mov.b   r6h,r0l
        and.b   r1l,r0l
        or.b    r0l,r2l
        mov.b   @er2,r0h
        xor.b   r0l,r2l
        or.b    r1h,r0h
        xor.b   #16,r0h
        mov.b   r0h,r3l
        mov.b   @er3,r0h
        mov.b   r0h,@(40,er4)
        add.l   #80,er4                 ; .l: fb may cross a 64 KB line
        add.w   e6,r6
        dec.w   #1,e5
        bne     5b
8:      ldm.l   @er7+,er4-er6
        rts
; lowest detail: one texel for two rows
9:      add.w   e6,e6
6:      mov.b   r6h,r0l
        and.b   r1l,r0l
        or.b    r0l,r2l
        mov.b   @er2,r0h
        xor.b   r0l,r2l
        or.b    r1h,r0h
        mov.b   r0h,r3l
        mov.b   @er3,r0h
        mov.b   r0h,@er4
        dec.w   #1,e5
        beq     8b
        xor.b   #16,r3l
        mov.b   @er3,r0h
        mov.b   r0h,@(40,er4)
        add.l   #80,er4
        add.w   e6,r6
        dec.w   #1,e5
        bne     6b
        ldm.l   @er7+,er4-er6
        rts
; high detail: a column is half a byte, the other half stays 0
7:      mov.b   #0xf0,r5l
        btst    #0,r0l
        beq     3f
        mov.b   #0x0f,r5l
3:      mov.b   r6h,r0l
        and.b   r1l,r0l
        or.b    r0l,r2l
        mov.b   @er2,r0h
        xor.b   r0l,r2l
        or.b    r1h,r0h
        mov.b   r0h,r3l
        mov.b   @er3,r0h
        and.b   r5l,r0h
        mov.b   r0h,@er4
        add.l   #40,er4
        xor.b   #16,r1h
        add.w   e6,r6
        dec.w   #1,e5
        bne     3b
        ldm.l   @er7+,er4-er6
        rts

; fill rows r4..e4 of the column at cr_col with shade r3l; keeps er0-er6
_cr_fill:
        stm.l   er0-er3,@-er7
        mov.w   r4,r0
        extu.l  er0
        mov.w   #40,r1
        mulxu.w r1,er0
        mov.l   @_cr_col:32,er1
        add.l   er1,er0
        sub.l   er2,er2
        mov.b   r3l,r2l
        shll.b  r2l
        add.l   #_pat,er2
        mov.b   @er2,r1l
        mov.b   @(1,er2),r1h
        btst    #0,r4l
        beq     1f
        mov.b   r1l,r2l
        mov.b   r1h,r1l
        mov.b   r2l,r1h
1:      mov.b   @_cr_mask:32,r2l
        and.b   r2l,r1l
        and.b   r2l,r1h
        mov.w   e4,r3
        sub.w   r4,r3
        inc.w   #1,r3
2:      mov.b   r1l,@er0
        dec.w   #1,r3
        beq     3f
        mov.b   r1h,@(40,er0)
        add.l   #80,er0
        dec.w   #1,r3
        bne     2b
3:      ldm.l   @er7+,er0-er3
        rts

; void hw_present_rows(s16 from, s16 to): rows from..to-1 of fb to the LCD
; (HD66421: index register at 100000, data at 100001; its row 0 is the
; bottom one), rows fb_merge_y0..fb_merge_y1-1 or'ed with fb2 when
; fb_merge is set (render.c), then reset the watchdog
        .global _hw_present_rows_img
_hw_present_rows_img:
        mov.w   r1,r3
        sub.w   r0,r3                   ; rows to copy
        ble     3f:16
        stm.l   er4-er6,@-er7
        mov.w   #99,e3
        sub.w   r0,e3                   ; the LCD's row: the top one is 99
        mov.w   #40,r1
        mulxu.w r1,er0                  ; er0 = from * 40
        mov.l   er0,er4
        add.l   #_fb2,er4
        mov.l   er0,er1
        add.l   #_fb,er1
        mov.l   #0x100000,er2
        mov.w   @_fb_merge_y1:32,r0     ; merged LCD rows: r5 .. e5
        mov.w   #100,r5
        sub.w   r0,r5
        mov.w   @_fb_merge_y0:32,r0
        mov.w   #99,e5
        sub.w   r0,e5
        mov.b   @_fb_merge:32,r0l
        bne     1f
        mov.w   #0x7fff,r5              ; none
1:      mov.b   #2,r0h
        mov.b   r0h,@er2
        sub.b   r0h,r0h
        mov.b   r0h,@(1,er2)
        mov.b   #3,r0h
        mov.b   r0h,@er2
        mov.w   e3,r0
        mov.b   r0l,@(1,er2)
        mov.b   #4,r0h
        mov.b   r0h,@er2
        mov.w   #10,e0
        cmp.w   r5,e3
        blt     2f
        cmp.w   e5,e3
        bgt     2f
4:      mov.w   @er1+,r0                ; a word read costs what a byte does
        mov.w   @er4+,r6
        or.w    r6,r0
        mov.b   r0h,@(1,er2)
        mov.b   r0l,@(1,er2)
        mov.w   @er1+,r0
        mov.w   @er4+,r6
        or.w    r6,r0
        mov.b   r0h,@(1,er2)
        mov.b   r0l,@(1,er2)
        dec.w   #1,e0
        bne     4b
        bra     5f
2:      mov.w   @er1+,r0
        mov.b   r0h,@(1,er2)
        mov.b   r0l,@(1,er2)
        mov.w   @er1+,r0
        mov.b   r0h,@(1,er2)
        mov.b   r0l,@(1,er2)
        dec.w   #1,e0
        bne     2b
        add.l   #40,er4
5:      dec.w   #1,e3
        dec.w   #1,r3
        bne     1b:16
        ldm.l   @er7+,er4-er6
3:      mov.w   #0x5a00,r0
        mov.w   r0,@0xffbc:16
        rts

;; void tsmooth(u8 *p, s16 pairs, u16 tone): 2*pairs rows of a smoothed
; wall column (render.c tline_smooth): the dither byte of tone >> 8 from
; ts_lut (this row's half of tex_luts[l]; the next row's is the other half),
; and'ed with ts_mask, then tone += ts_dt a row.
        .global _tsmooth_img
_tsmooth_img:
        stm.l   er4-er5,@-er7
        mov.l   @_ts_lut:32,er3
        mov.b   r3l,r4h                 ; this row's half
        mov.b   r4h,r4l
        xor.b   #16,r4l                 ; the next row's
        mov.w   @_ts_dt:32,e1
        mov.b   @_ts_mask:32,r5l
1:      mov.b   r2h,r3l
        or.b    r4h,r3l
        mov.b   @er3,r5h
        and.b   r5l,r5h
        mov.b   r5h,@er0
        add.w   e1,r2
        mov.b   r2h,r3l
        or.b    r4l,r3l
        mov.b   @er3,r5h
        and.b   r5l,r5h
        mov.b   r5h,@(40,er0)
        add.w   e1,r2
        add.l   #80,er0
        dec.w   #1,r1
        bne     1b
        ldm.l   @er7+,er4-er5
        rts

; s16 div32_16(s32 n, s16 d): quotient must fit in 16 bits (the renderer
; divides once or twice a column, so these two run from on-chip RAM too)
        .global _div32_16_img
_div32_16_img:
        divxs.w r1,er0
        exts.l  er0
        rts

; s32 ldiv32_16(s32 n, s16 d): full 32-bit quotient
        .global _ldiv32_16_img
_ldiv32_16_img:
        mov.b   #0,r3h                  ; r3h: result sign
        mov.l   er0,er0
        bpl     1f
        neg.l   er0
        not.b   r3h
1:      mov.w   r1,r1
        bpl     2f
        neg.w   r1
        not.b   r3h
2:      mov.w   e0,r2                   ; high word / d
        extu.l  er2
        divxu.w r1,er2                  ; r2 = q_hi, e2 = remainder
        mov.w   e2,e0                   ; (rem << 16) | low word
        divxu.w r1,er0                  ; r0 = q_lo
        mov.w   r2,e0                   ; er0 = q_hi:q_lo
        mov.b   r3h,r3h
        beq     3f
        neg.l   er0
3:      rts

; long __mulsi3(long a, long b): gcc's 32-bit multiply (libgcc's, copied
; here so that it runs from the on-chip RAM too; link.ld names the copy)
        .global ___mulsi3_img
___mulsi3_img:
        mov.l   er0,er2
        mov.l   er1,er3
        mulxu.w r1,er0
        mov.w   e3,r3
        mulxu.w r2,er3
        mov.w   e2,r2
        mulxu.w r1,er2
        add.w   r3,r2
        add.w   r2,e0
        rts

        .global __iram_code_end
__iram_code_end:
        .end
