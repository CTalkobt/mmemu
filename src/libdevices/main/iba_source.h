#pragma once

/**
 * A device that takes the bus from the CPU by pulling BA (RDY) low: the
 * VIC-II's character and sprite fetches.
 *
 * The scheduler asks before each CPU cycle it accounts for: while BA is low
 * a read cycle waits (the 6502 stops on RDY only when it reads; up to three
 * writes in a row go on), and a DMA master (an REU) waits too.
 */
class IBaSource {
public:
    virtual ~IBaSource() = default;
    /** BA is low on the cycle `ahead` cycles from now (0: the next one to run). */
    virtual bool baLow(int ahead) const = 0;
};
