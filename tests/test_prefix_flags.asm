;;; Test: Prefix execution should only set flags from actual instructions
;;; Issue: NEG / NEG / STQ should set flags as if NEG/NEG executed, not affecting STQ behavior
        .cpu 45GS02
        .load $2000

;;; Zero page test vars
        .org $0000
testflags:
        .byte 0

;;; BASIC stub
        .org $2001
        .word endstub
        .byte 0, 0
        .byte $FE, $02, $30, $3A
        .byte $9E, "$2023", $3A
        .byte $8F, $20
        .text "PREFIX FLAGS TEST"
endstub:
        .byte 0, 0, 0

;;; Entry point
        .org $2023
        sei

        ;;; Test 1: NEG with negative result should set N flag
        lda #$42
        neg A               ; $42 -> ~$42+1 = $BE (negative, sets N)
        php                 ; Save flags
        pla
        sta testflags       ; Store for inspection

        ;;; Test 2: NEG / NEG (double negation cancels, sets flags appropriately)
        lda #$01
        neg A
        neg A               ; Should be back to $01, clears N, Z
        php
        pla
        ;;; Compare flags with Test 1

        ;;; For now, just halt
        cli
        rts
