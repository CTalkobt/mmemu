// KickAssembler 45GS02 Quad Instructions Test
.cpu _45gs02

.const EXIT_TRIGGER = $d6cf
.const MEGA65_KEY   = $d02f  // VIC-IV I/O mode key: $47,$53 unlocks MEGA65 I/O space
.const RESULTS_BASE = $0400

* = $2000 "Program"

start:
    lda #$20
    ldx #0
loop_clear:
    sta RESULTS_BASE,x
    inx
    bne loop_clear

    // 1. Test LDQ / STQ Zero Page
    lda #$11
    ldx #$22
    ldy #$33
    ldz #$44
    stq $10         // $10=$11, $11=$22, $12=$33, $13=$44
    
    lda #0
    ldx #0
    ldy #0
    ldz #0
    
    ldq $10
    
    cpx #$22
    lbne zp_fail
    cpy #$33
    lbne zp_fail
    cpz #$44
    lbne zp_fail
    cmp #$11
    lbne zp_fail

    lda #$01
    sta RESULTS_BASE

    // 2. Test LDQ / STQ Absolute
    lda #$55
    ldx #$66
    ldy #$77
    ldz #$88
    stq $1000       // $1000=$55, $1001=$66, $1002=$77, $1003=$88
    
    lda #0
    ldx #0
    ldy #0
    ldz #0
    
    ldq $1000
    
    cpx #$66
    lbne abs_fail
    cpy #$77
    lbne abs_fail
    cpz #$88
    lbne abs_fail
    cmp #$55
    lbne abs_fail

    lda #$01
    sta RESULTS_BASE + 1

    // 3. Test LDQ / STQ 32-bit Indirect [zp],z
    // We'll point to $2000
    lda #$00
    sta $20
    lda #$20
    sta $21
    lda #$00
    sta $22
    lda #$00
    sta $23

    lda #$AA
    ldx #$BB
    ldy #$CC
    ldz #$DD
    
    ldz #0
    .byte $42, $42, $EA, $92, $20  // STQ [$20],z
    
    lda #0
    ldx #0
    ldy #0
    ldz #0
    
    ldz #0
    .byte $42, $42, $EA, $B2, $20  // LDQ [$20],z
    
    cpx #$BB
    lbne ind_fail
    cpy #$CC
    lbne ind_fail
    cpz #$00          
    lbne ind_fail
    cmp #$AA
    lbne ind_fail

    lda #$01
    sta RESULTS_BASE + 2

    // 4. Test ADCQ
    lda #$01
    ldx #$00
    ldy #$00
    ldz #$00
    stq $10         // $10 = 0x00000001
    
    lda #$FF
    ldx #$00
    ldy #$00
    ldz #$00
    clc
    adcq $10        // Q = 0x000000FF + 0x00000001 = 0x00000100
    
    cpx #$01        // X should be $01
    lbne arith_fail
    cmp #$00        // A should be $00
    lbne arith_fail
    
    lda #$01
    sta RESULTS_BASE + 3

    // 5. Test ASLQ
    lda #$00
    ldx #$00
    ldy #$00
    ldz #$80
    // Q = 0x80000000
    
    aslq            // Q = 0x00000000, Carry = 1
    
    lbcc shift_fail // Should have carry
    cmp #$00
    lbne shift_fail
    cpz #$00
    lbne shift_fail

    lda #$01
    sta RESULTS_BASE + 4
    jmp end

zp_fail:
    lda #$FF
    sta RESULTS_BASE
    jmp end

abs_fail:
    lda #$FF
    sta RESULTS_BASE + 1
    jmp end

ind_fail:
    lda #$FF
    sta RESULTS_BASE + 2
    jmp end

arith_fail:
    lda #$FF
    sta RESULTS_BASE + 3
    jmp end

shift_fail:
    lda #$FF
    sta RESULTS_BASE + 4
    jmp end

end:
    lda #$47        // Unlock MEGA65 I/O mode so $D6CF reaches MEGA65 I/O space
    sta MEGA65_KEY
    lda #$53
    sta MEGA65_KEY
    lda #$42
    sta EXIT_TRIGGER
    rts
