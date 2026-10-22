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
; pointer at top, then go back to the caller's stack
        .global _run_on_stack
_run_on_stack:
        stm.l   er4-er6,@-er7
        mov.l   er7,er4
        mov.l   er1,er7
        jsr     @er0
        mov.l   er4,er7
        ldm.l   @er7+,er4-er6
        rts

; void irq_restore(u8 ccr)
        .global _irq_restore
_irq_restore:
        ldc     r0l,ccr
        rts

; s16 div32_16(s32 n, s16 d): quotient must fit in 16 bits
        .global _div32_16
_div32_16:
        divxs.w r1,er0
        exts.l  er0
        rts

; s32 ldiv32_16(s32 n, s16 d): full 32-bit quotient
        .global _ldiv32_16
_ldiv32_16:
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

; void cell_run(const u8 *c, s16 r, s16 e): one run of sprite cells in a
; column (see render.c, which has the same in C for the PC). Cells c are
; for cell rows r..e-1; rowtab[] gives their screen rows, cr_tab[] their
; shades; cells of one shade next to each other are filled as one span.
;   er1 rowtab pointer, er2 cell pointer, e3 cells left,
;   r3h cr_ebase, r3l shade of the open span, r4 its top, e4 its bottom,
;   r5 cr_lo, e5 cr_hi, er6 cr_tab index, r0 / e0 this cell's top / bottom
        .global _cell_run
_cell_run:
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
        jsr     @_cr_fill
3:      mov.w   r0,r4
        mov.w   e0,e4
        mov.b   r6l,r3l
.Lnext: adds    #1,er2
        adds    #2,er1
        dec.w   #1,e3
        bne     .Lcell
.Ldone: cmp.w   r4,e4
        blt     4f
        jsr     @_cr_fill
4:      ldm.l   @er7+,er4-er6
        rts

; void vline(s16 x, s16 y1, s16 y2, u8 shade): fill column x (of 80) from
; row y1 to y2 (0 <= y1) with a dithered shade (render.c has it in C too)
        .global _vline
_vline:
        cmp.w   r1,r2
        blt     9f
        push.l  er4
        mov.w   r1,r4
        mov.w   r2,e4
        mov.b   @(11,er7),r3l
        mov.w   r0,r1
        shar.w  r1
        exts.l  er1
        add.l   #_fb,er1
        mov.l   er1,@_cr_col:32
        mov.b   #0x0f,r2l
        btst    #0,r0l
        beq     1f
        mov.b   #0xf0,r2l
1:      mov.b   r2l,@_cr_keep:32
        jsr     @_cr_fill
        pop.l   er4
9:      rts

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
1:      mov.b   @_cr_keep:32,r2h
        mov.b   r2h,r2l
        not.b   r2l
        and.b   r2l,r1l
        and.b   r2l,r1h
        mov.w   e4,r3
        sub.w   r4,r3
        inc.w   #1,r3
2:      mov.b   @er0,r2l
        and.b   r2h,r2l
        or.b    r1l,r2l
        mov.b   r2l,@er0
        dec.w   #1,r3
        beq     3f
        mov.b   @(40,er0),r2l
        and.b   r2h,r2l
        or.b    r1h,r2l
        mov.b   r2l,@(40,er0)
        add.l   #80,er0
        dec.w   #1,r3
        bne     2b
3:      ldm.l   @er7+,er0-er3
        rts
        .end
