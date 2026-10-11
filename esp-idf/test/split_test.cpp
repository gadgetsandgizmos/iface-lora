/* split_test — the split framing's receive rule (src/lora_split.h), on the host.
 *
 * A small receiver stands in for delivery (lora_bridge.cpp, bridgeFrameDeliver):
 * it holds a first half, acts on each splitStep answer as delivery does, and
 * keeps what would reach rnsd. Frames are built the way this interface sends
 * them: a packet up to RNODE_MAX_PAYLOAD bytes in one frame, a longer one as
 * its first RNODE_MAX_PAYLOAD bytes and then the rest, under one sequence. */

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

int main()
{
    testEverySize();
    testUnsplitEndsHalf();
    testOtherSequenceReplaces();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("split: ok\n");
    return 0;
}
