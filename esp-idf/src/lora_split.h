#pragma once
/**
 * lora_split — the on-air split framing's receive rule: what one received
 * Reticulum frame does to the first half held.
 *
 * Every Reticulum frame on this interface starts with one header byte: a
 * random sequence in the high nibble, SPLIT in bit 0. A packet longer than
 * RNODE_MAX_PAYLOAD flies as two frames under one sequence, back to back: its
 * first RNODE_MAX_PAYLOAD bytes, then the rest.
 *
 * Free of ESP-IDF, RadioLib and FreeRTOS, like supe.h, so the host tests reach
 * it (test/split_test.cpp). Delivery (lora_bridge.cpp) and the LoRaMon record
 * (lora_mon.cpp) both answer by it and keep their own state.
 */
#include <stddef.h>
#include <stdint.h>

#define RNODE_MAX_PAYLOAD    254
#define RNODE_FLAG_SPLIT     0x01

enum SplitStep : uint8_t {
    SPLIT_WHOLE,    /* not split: a packet alone; a half held ends unfinished */
    SPLIT_FIRST,    /* a first half: held, in place of any half held */
    SPLIT_SECOND,   /* the partner of the half held: the packet is whole */
};

/* `held`: a first half is held, under the sequence nibble `heldSeq` (the
 * header's high nibble, in place). `payloadLen`: the frame's bytes after its
 * header. */
static inline SplitStep splitStep(bool held, uint8_t heldSeq, uint8_t header,
                                  size_t payloadLen) {
    (void)payloadLen;
    if (!(header & RNODE_FLAG_SPLIT)) return SPLIT_WHOLE;
    if (held && heldSeq == (uint8_t)(header & 0xF0)) return SPLIT_SECOND;
    return SPLIT_FIRST;
}
