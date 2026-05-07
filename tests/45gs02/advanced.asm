// KickAssembler 45GS02 Advanced Instructions Test
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

    // 1. Test INW / DEW
    lda #$FF
    sta $10
    lda #$00
    sta $11
    
    inw $10         // $00FF -> $0100
    
    lda $10
    cmp #$00
    bne word_fail
    lda $11
    cmp #$01
    bne word_fail
    
    dew $10         // $0100 -> $00FF
    
    lda $10
    cmp #$FF
    bne word_fail
    lda $11
    cmp #$00
    bne word_fail
    
    lda #$01
    sta RESULTS_BASE

test_bits:
    // 2. Test SMB / RMB / BBS / BBR
    lda #%01010101
    sta $20
    smb1 $20      // Set bit 1 -> %01010111
    lda $20
    cmp #%01010111
    bne bit_fail
    
    rmb0 $20      // Clear bit 0 -> %01010110
    lda $20
    cmp #%01010110
    bne bit_fail
    
    bbs1 $20, b1_set
    jmp bit_fail
b1_set:
    bbr0 $20, b0_clr
    jmp bit_fail
b0_clr:
    lda #$01
    sta RESULTS_BASE + 1
    jmp end

word_fail:
    lda #$FF
    sta RESULTS_BASE
    jmp end

bit_fail:
    lda #$FF
    sta RESULTS_BASE + 1
    jmp end

end:
    lda #$47        // Unlock MEGA65 I/O mode so $D6CF reaches MEGA65 I/O space
    sta MEGA65_KEY
    lda #$53
    sta MEGA65_KEY
    lda #$42
    sta EXIT_TRIGGER
    rts
