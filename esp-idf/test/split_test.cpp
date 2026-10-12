/* split_test — the split framing's receive rules (src/lora_split.h), on the host,
 * built twice: with SUPE and without (CONFIG_LORA_NO_SUPE).
 *
 * A small receiver stands in for the dispatch and delivery (lora_bridge.cpp,
 * handleRxDone and bridgeFrameDeliver): it takes only what rxSort calls a
 * Reticulum frame, holds a first half, acts on each splitStep answer as
 * delivery does, and keeps what would reach rnsd. Frames are built the way
 * this interface sends them: a packet up to RNODE_MAX_PAYLOAD bytes in one
 * frame, a longer one as its first RNODE_MAX_PAYLOAD bytes and then the rest,
 * under one sequence. */

#include "../src/lora_split.h"

#include <cstdio>
#include <vector>

typedef std::vector<uint8_t> Bytes;

static int failures = 0;

static void ck(bool ok, const char* what)
{
    if (ok) return;
    printf("FAIL  %s\n", what);
    failures++;
}

struct Receiver {
    bool  held = false;
    uint8_t seq = 0;
    Bytes half;
    std::vector<Bytes> got;     /* what reached rnsd, in order */

    void push(const Bytes& f)
    {
        if (rxSort(f[0]) != SORT_RNS) return;   /* SUPE's, or somebody else's */
        Bytes data(f.begin() + 1, f.end());
        SplitStep step = splitStep(held, seq, f[0], data.size());
        if (step == SPLIT_WHOLE) {
            held = false;
            got.push_back(data);
        } else if (step == SPLIT_SECOND) {
            half.insert(half.end(), data.begin(), data.end());
            got.push_back(half);
            held = false;
        } else if (step == SPLIT_FIRST) {
            half = data;
            seq  = f[0] & 0xF0;
            held = true;
        } else if (!data.empty()) {
            held = false;       /* a second half with no first: nothing to complete */
        }
    }
};

static std::vector<Bytes> frames(const Bytes& packet, uint8_t seq)
{
    std::vector<Bytes> out;
    bool split = packet.size() > RNODE_MAX_PAYLOAD;
    uint8_t header = (uint8_t)((seq << 4) | (split ? RNODE_FLAG_SPLIT : 0));
    for (size_t at = 0; at < packet.size(); at += RNODE_MAX_PAYLOAD) {
        size_t n = packet.size() - at < RNODE_MAX_PAYLOAD ? packet.size() - at : RNODE_MAX_PAYLOAD;
        Bytes f(1, header);
        f.insert(f.end(), packet.begin() + at, packet.begin() + at + n);
        out.push_back(f);
    }
    return out;
}

static Bytes bytes(size_t n, unsigned salt = 3)
{
    Bytes b(n);
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(i * 7 + salt);
    return b;
}

static void testEverySize()
{
    /* Up to the MTU of 500: no Reticulum packet is longer. */
    const size_t sizes[] = {1, 20, 253, 254, 255, 300, 499, 500};
    for (size_t n : sizes) {
        Receiver r;
        Bytes p = bytes(n);
        for (const Bytes& f : frames(p, 5)) r.push(f);
        char what[64];
        snprintf(what, sizeof what, "a packet of %zu bytes arrives whole", n);
        ck(r.got.size() == 1 && r.got[0] == p, what);
        ck(!r.held, "and nothing is held after it");
    }
}

static void testUnsplitEndsHalf()
{
    /* A first half whose partner did not come, then a short packet: the short
     * one is delivered, and the half is gone, not completed by what follows. */
    Receiver r;
    r.push(frames(bytes(300), 1)[0]);
    Bytes shortOne = bytes(30, 9);
    r.push(frames(shortOne, 1)[0]);
    ck(r.got.size() == 1 && r.got[0] == shortOne, "an unsplit frame is a packet of its own");
    ck(!r.held, "and it ends the half held");
}

static void testOtherSequenceReplaces()
{
    /* A first half under another sequence replaces the one held. */
    Receiver r;
    std::vector<Bytes> a = frames(bytes(300), 1), b = frames(bytes(301, 5), 2);
    r.push(a[0]);
    r.push(b[0]);
    r.push(b[1]);
    ck(r.got.size() == 1 && r.got[0] == bytes(301, 5), "the second packet arrives whole");
}

static void testFullFrameUnderHeldSequence()
{
    /* From a mixed sim: a receiver held a first half under header 0x51 and
     * missed its second. Another sender's 259-byte packet came next under the
     * same sequence nibble. Its first half completed the held one, 508 bytes
     * nobody sent, and its 5-byte second half, c4 42 90 db 1f, was left with
     * nothing to complete: that packet was lost as well. */
    Bytes held = {0x51, 0x50, 0x00, 0x6d, 0x0d, 0xdb, 0x4c, 0x9c, 0x33};
    held.resize(1 + RNODE_MAX_PAYLOAD, 0xa5);
    Bytes next = {0x00, 0x01, 0xd3, 0x8b, 0x7a, 0xff, 0xe1, 0x8b};
    next.resize(RNODE_MAX_PAYLOAD, 0x5a);
    next.insert(next.end(), {0xc4, 0x42, 0x90, 0xdb, 0x1f});
    std::vector<Bytes> f = frames(next, 5);
    ck(f[1] == Bytes({0x51, 0xc4, 0x42, 0x90, 0xdb, 0x1f}), "the second half as it flew");
    Receiver r;
    r.push(held);
    r.push(f[0]);
    ck(r.got.empty(), "a full frame under the held sequence completes nothing");
    r.push(f[1]);
    ck(r.got.size() == 1 && r.got[0] == next, "it is a first half, and its packet arrives whole");
}

static void testLoneSecondHalf()
{
    /* From a mixed sim: a 259-byte link packet, sent as 254 + 5 bytes under
     * header 0x21. A receiver lost the first half and held the 6-byte second
     * frame as a first half. A relay forwarded the packet under the same
     * sequence, and its first half completed the held one: a packet starting
     * 0xb1 that nobody sent. */
    Bytes orphan = {0x21, 0xb1, 0x95, 0xd7, 0x72, 0x7a};
    Bytes relayed = {0x0c, 0x01, 0xa6, 0x3c, 0xdd, 0x9f, 0xf0, 0xed};
    relayed.resize(RNODE_MAX_PAYLOAD, 0x5a);
    relayed.insert(relayed.end(), orphan.begin() + 1, orphan.end());
    std::vector<Bytes> f = frames(relayed, 2);
    ck(f[1] == orphan, "the relay's second half is the orphan");
    Receiver r;
    r.push(orphan);
    ck(r.got.empty() && !r.held, "a short split frame with no first half is dropped, not held");
    r.push(f[0]);
    r.push(f[1]);
    ck(r.got.size() == 1 && r.got[0] == relayed, "the relayed copy arrives whole");
}

static void testHeaderOnlyFrame()
{
    /* Stock RNode firmware follows a packet of exactly 508 bytes with a split
     * header and no bytes. No Reticulum packet is 508 bytes, so its two full
     * frames are two first halves here, and the empty one is no half: the next
     * packet under the same sequence is not cut short by it. */
    Receiver r;
    for (const Bytes& f : frames(bytes(2 * RNODE_MAX_PAYLOAD), 9)) r.push(f);
    r.push(Bytes(1, (uint8_t)((9 << 4) | RNODE_FLAG_SPLIT)));
    ck(r.got.empty(), "508 bytes and an empty frame deliver nothing");
    Bytes next = bytes(300, 11);
    for (const Bytes& f : frames(next, 9)) r.push(f);
    ck(r.got.size() == 1 && r.got[0] == next, "the next packet arrives whole");
}

static void testSort()
{
    /* Byte 0 of every frame this interface sends with a packet in it, as
     * frames() builds them: a Reticulum frame's, with SUPE built in or not.
     * SUPE's type bytes are 0xC0-0xDF less the four a header can be (supe.h):
     * SUPE's where it is built in, somebody else's where not. Every other byte
     * is somebody else's. */
    bool header[256] = {};
    for (unsigned seq = 0; seq < 16; seq++) {
        header[seq << 4] = true;
        header[(seq << 4) | RNODE_FLAG_SPLIT] = true;
    }
    int rns = 0, other = 0;
    for (unsigned b = 0; b < 256; b++) {
        RxSort want = header[b] ? SORT_RNS : SORT_FOREIGN;
#if !defined(CONFIG_LORA_NO_SUPE)
        if (!header[b] && b >= 0xC0 && b <= 0xDF) want = SORT_SUPE;
#endif
        if (rxSort((uint8_t)b) != want) (header[b] ? rns : other)++;
    }
    ck(rns == 0, "every split header begins a Reticulum frame");
    ck(other == 0, "and every other byte is SUPE's or somebody else's");
}

int main()
{
    testSort();
    testEverySize();
    testUnsplitEndsHalf();
    testOtherSequenceReplaces();
    testFullFrameUnderHeldSequence();
    testLoneSecondHalf();
    testHeaderOnlyFrame();
#if defined(CONFIG_LORA_NO_SUPE)
    const char* build = " (no SUPE)";
#else
    const char* build = "";
#endif
    if (failures) { printf("split%s: %d failure(s)\n", build, failures); return 1; }
    printf("split%s: ok\n", build);
    return 0;
}
