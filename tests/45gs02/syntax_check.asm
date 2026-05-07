.cpu _45gs02

.const EXIT_TRIGGER = $d6cf
.const RESULTS_BASE = $0400

* = $2000 "Program"

start:
    lda #$01
    sta RESULTS_BASE

    jsr label

    lda #$42
    sta EXIT_TRIGGER
    jmp start

label:
    rts
