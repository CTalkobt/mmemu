// KickAssembler 45GS02 Register Transfer Test (Memory Results)
.cpu _45gs02

.const SERIAL_DATA = $d6c1
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

    // Test TAZ / TZA
    lda #$55
    taz
    lda #$00
    tza
    cmp #$55
    beq z_ok
    lda #$FF
    sta RESULTS_BASE
    jmp test_b
z_ok:
    lda #$01
    sta RESULTS_BASE

test_b:
    // Test TAB / TBA
    lda #$AA
    tab
    lda #$00
    tba
    cmp #$AA
    beq b_ok
    lda #$FF
    sta RESULTS_BASE + 1
    jmp end
b_ok:
    lda #$01
    sta RESULTS_BASE + 1

end:
    lda #$47        // Unlock MEGA65 I/O mode so $D6CF reaches MEGA65 I/O space
    sta MEGA65_KEY
    lda #$53
    sta MEGA65_KEY
    lda #$42
    sta EXIT_TRIGGER
    rts
