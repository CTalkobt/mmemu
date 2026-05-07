// KickAssembler 45GS02 Full single-byte + Addressing Modes + Quad Test
.cpu _45gs02

.const RESULTS = $0400
.const EXIT = $d6cf
.const SUCCESS = $55
.const MEGA65_KEY = $d02f

* = $2000 "Program"

start:
    lda #$20
    ldx #0
loop_clear:
    sta RESULTS,x
    inx
    bne loop_clear

    // 1. TSB/TRB abs
    lda #$00
    sta $1000
    lda #$FF
    tsb $1000
    lda $1000
    cmp #$FF
    beq tsb_abs_ok
    lda #$E1
    sta RESULTS + 0
    jmp end
tsb_abs_ok:
    lda #SUCCESS
    sta RESULTS + 0

    // 2. ADC abs,X
    lda #$10
    sta $1005
    ldx #5
    clc
    lda #$02
    adc $1000,x // ADC $1005
    cmp #$12
    beq adc_absx_ok
    lda #$E2
    sta RESULTS + 1
    jmp end
adc_absx_ok:
    lda #SUCCESS
    sta RESULTS + 1

    // 3. AND (zp),Y
    lda #$AA
    sta $1000
    lda #$00
    sta $20
    lda #$10
    sta $21    // $20 points to $1000
    ldy #0
    lda #$FF
    and ($20),y
    cmp #$AA
    beq and_ind_ok
    lda #$E3
    sta RESULTS + 2
    jmp end
and_ind_ok:
    lda #SUCCESS
    sta RESULTS + 2

    // 4. Quad LDQ/STQ abs
    lda #$11
    sta $1100
    lda #$22
    sta $1101
    lda #$33
    sta $1102
    lda #$44
    sta $1103
    
    // LDQ abs
    .byte $42, $42, $AD, $00, $11 // LDQ $1100
    
    // STQ zp
    .byte $42, $42, $85, $30 // STQ $30
    
    lda $30
    cmp #$11
    bne quad_fail
    lda $31
    cmp #$22
    bne quad_fail
    lda $32
    cmp #$33
    bne quad_fail
    lda $33
    cmp #$44
    bne quad_fail
    lda #SUCCESS
    sta RESULTS + 3
    jmp quad_ok
quad_fail:
    lda #$E4
    sta RESULTS + 3
    jmp end
quad_ok:

    // 5. INQ A (Quad INC A)
    lda #$FF
    ldx #0
    ldy #0
    ldz #0 // Q = $000000FF
    .byte $42, $42, $1A // INQ A
    cmp #$00
    bne inq_fail
    cpx #$01
    bne inq_fail
    lda #SUCCESS
    sta RESULTS + 4
    jmp inq_ok
inq_fail:
    lda #$E5
    sta RESULTS + 4
inq_ok:

end:
    lda #$47        // Unlock MEGA65 I/O mode
    sta MEGA65_KEY
    lda #$53
    sta MEGA65_KEY
    lda #$42
    sta EXIT
    jmp start
