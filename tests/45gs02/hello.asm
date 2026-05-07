// KickAssembler 45GS02 test
.cpu _45gs02

.const SERIAL_DATA = $d6c1
.const EXIT_TRIGGER = $d6cf
.const RESULTS_BASE = $0400

* = $2000 "Program"

start:
    lda #'H'
    sta RESULTS_BASE
    sta SERIAL_DATA
    lda #'E'
    sta RESULTS_BASE + 1
    sta SERIAL_DATA
    lda #'L'
    sta RESULTS_BASE + 2
    sta SERIAL_DATA
    sta RESULTS_BASE + 3
    sta SERIAL_DATA
    lda #'O'
    sta RESULTS_BASE + 4
    sta SERIAL_DATA
    lda #10 // Newline
    sta RESULTS_BASE + 5
    sta SERIAL_DATA

    lda #$42
    sta EXIT_TRIGGER
    jmp start // Should never hit this
