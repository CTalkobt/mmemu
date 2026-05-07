// KickAssembler 45GS02 Arithmetic Test (Memory Results)
.cpu _45gs02

.const SERIAL_DATA = $d6c1
.const EXIT_TRIGGER = $d6cf
.const MEGA65_KEY   = $d02f  // VIC-IV I/O mode key: $47,$53 unlocks MEGA65 I/O space
.const RESULTS_BASE = $0400

* = $2000 "Program"

start:
    // Initialize results memory to $20 (space)
    lda #$20
    ldx #0
loop_clear:
    sta RESULTS_BASE,x
    inx
    bne loop_clear

    // Test ADC: 1 + 1 = 2
    clc
    lda #1
    adc #1
    cmp #2
    beq adc1_ok
    lda #$FF
    sta RESULTS_BASE
    jmp test_sbc
adc1_ok:
    lda #$01
    sta RESULTS_BASE

test_sbc:
    // Test SBC: 5 - 3 = 2
    sec
    lda #5
    sbc #3
    cmp #2
    beq sbc1_ok
    lda #$FF
    sta RESULTS_BASE + 1
    jmp end
sbc1_ok:
    lda #$01
    sta RESULTS_BASE + 1

end:
    lda #10 // Newline for serial log still (optional)
    sta SERIAL_DATA
    lda #$47        // Unlock MEGA65 I/O mode so $D6CF reaches MEGA65 I/O space
    sta MEGA65_KEY
    lda #$53
    sta MEGA65_KEY
    lda #$42
    sta EXIT_TRIGGER
    rts
