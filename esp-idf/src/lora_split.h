#pragma once
/**
 * lora_split — the on-air split framing's receive rules: which received frames
 * are its, and what one does to the first half held.
 *
 * Every Reticulum frame on this interface starts with one header byte: a
 * random sequence in the high nibble, SPLIT in bit 0. A packet longer than
 * RNODE_MAX_PAYLOAD flies as two frames under one sequence, back to back: its
 * first RNODE_MAX_PAYLOAD bytes, then the rest.
 *
 * Free of ESP-IDF, RadioLib and FreeRTOS, like supe.h, so the host tests reach
 * it (test/split_test.cpp, built with SUPE and without). Delivery
 * (lora_bridge.cpp) and the LoRaMon record (lora_mon.cpp) both answer by it
 * and keep their own state.
 */
#include <stddef.h>
#include <stdint.h>
#if !defined(CONFIG_LORA_NO_SUPE)
#include "supe.h"
#endif

#define RNODE_MAX_PAYLOAD    254
#define RNODE_FLAG_SPLIT     0x01

/* What byte 0 makes of a received frame that nothing sorted out before it (a
 * power request, a short path request). The split header can be any byte whose
 * low nibble is 0 or 1 and no other, and SUPE's type bytes concede those values
 * (supe.h). A build without SUPE sorts by the same rule less SUPE's share:
 * there a SUPE frame is somebody else's. */
enum RxSort : uint8_t {
    SORT_RNS,       /* a split header: a Reticulum frame */
    SORT_SUPE,      /* a SUPE frame, in a build with SUPE */
    SORT_FOREIGN,   /* anything else: somebody else's, discarded */
};

static inline RxSort rxSort(uint8_t byte0) {
    if ((byte0 & 0x0F) <= RNODE_FLAG_SPLIT) return SORT_RNS;
#if !defined(CONFIG_LORA_NO_SUPE)
    if (supeIsTypeByte(byte0)) return SORT_SUPE;
#endif
    return SORT_FOREIGN;
}

enum SplitStep : uint8_t {
    SPLIT_WHOLE,    /* not split: a packet alone; a half held ends unfinished */
    SPLIT_FIRST,    /* a first half: held, in place of any half held */
    SPLIT_SECOND,   /* the partner of the half held: the packet is whole */
    SPLIT_LONE,     /* a second half whose first was not heard: dropped, and a
                     * half held, another packet's, ends unfinished */
    SPLIT_EMPTY,    /* a split frame with no bytes: nothing, the half held stays */
};

/* `held`: a first half is held, under the sequence nibble `heldSeq` (the
 * header's high nibble, in place). `payloadLen`: the frame's bytes after its
 * header.
 *
 * A first half is always exactly RNODE_MAX_PAYLOAD bytes, and a second half
 * shorter: the one packet size whose halves are both full, 508 bytes, is over
 * Reticulum's MTU of 500. So the length says which half a frame is, and the
 * sequence only whether a second half belongs to the half held. The sequence
 * alone cannot: it is 4 random bits, and two packets share one every sixteenth
 * time.
 * - A full frame under the held sequence is a new first half, which replaces
 *   the one held. Joined, the two made 508 bytes nobody sent, and the new
 *   packet's own second half then had nothing to complete.
 * - A short frame with no half held under its sequence is a second half whose
 *   first was lost. Held as a first half, it was completed by the next first
 *   half under that sequence, typically a relay's copy of the same packet, and
 *   the two went up as one packet nobody sent.
 * - A split frame with no bytes is no half at all. Stock RNode firmware sends
 *   one, header only, after a packet of exactly 508 bytes. */
static inline SplitStep splitStep(bool held, uint8_t heldSeq, uint8_t header,
                                  size_t payloadLen) {
    if (!(header & RNODE_FLAG_SPLIT)) return SPLIT_WHOLE;
    if (payloadLen == 0) return SPLIT_EMPTY;
    if (payloadLen >= RNODE_MAX_PAYLOAD) return SPLIT_FIRST;
    if (held && heldSeq == (uint8_t)(header & 0xF0)) return SPLIT_SECOND;
    return SPLIT_LONE;
}
