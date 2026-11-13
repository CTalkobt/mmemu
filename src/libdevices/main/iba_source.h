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
    /** BA is low on the cycle `ahead` cycles from now (0: the next one to run;
     *  -1 the one last run). */
    virtual bool baLow(int ahead) const = 0;
    /** BA as a DMA master (the REU) sees it, which can differ from the CPU's
     *  view: the VIC-II pulls BA a cycle late at the start of sprite 0's DMA. */
    virtual bool baLowForDma(int ahead) const { return baLow(ahead); }
    /** What the device put on the data bus in the first half (Phi1) of the
     *  current cycle, or -1 if it doesn't say. A 6510 write to $00/$01
     *  stores it in the RAM there (VICE zero_store, testprogs REU/cpuport). */
    virtual int phi1Value() const { return -1; }
};
