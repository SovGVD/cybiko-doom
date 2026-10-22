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
        .end
