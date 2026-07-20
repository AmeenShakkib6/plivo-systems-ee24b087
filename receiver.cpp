// Receiver: relay (47002) -> us -> harness player (47020).
// Jitter buffer + dedup + XOR-FEC reconstruction. No NACK/retransmit yet
// (that's a later, optional layer once this core path is proven valid).
//
// build: make        run: python3 run.py --delay_ms 60
#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

#include "protocol.h"

using namespace proto;

namespace {

constexpr int BUF_SIZE = 256;  // ~5.1s of frames at 20ms/frame

struct FrameSlot {
    bool have = false;
    bool delivered = false;
    uint32_t seq = 0;
    uint8_t payload[PAYLOAD_BYTES];
};

struct GroupSlot {
    bool have = false;
    uint32_t base_seq = 0;
    uint8_t count = 0;
    uint8_t xor_payload[PAYLOAD_BYTES];
};

FrameSlot g_frames[BUF_SIZE];
GroupSlot g_groups[BUF_SIZE];  // indexed by (base_seq / FEC_GROUP) % BUF_SIZE

FrameSlot &frame_slot(uint32_t seq) {
    FrameSlot &s = g_frames[seq % BUF_SIZE];
    if (s.seq != seq) {  // stale entry from a previous wrap -- reset it
        s.have = false;
        s.delivered = false;
        s.seq = seq;
    }
    return s;
}

GroupSlot &group_slot(uint32_t base_seq) {
    GroupSlot &g = g_groups[(base_seq / FEC_GROUP) % BUF_SIZE];
    if (g.base_seq != base_seq) {
        g.have = false;
        g.base_seq = base_seq;
    }
    return g;
}

int g_player_fd;
sockaddr_in g_player_addr;

void deliver(uint32_t seq, const uint8_t *payload) {
    FrameSlot &s = frame_slot(seq);
    if (s.delivered) return;
    s.have = true;
    s.delivered = true;
    std::memcpy(s.payload, payload, PAYLOAD_BYTES);

    uint8_t out[4 + PAYLOAD_BYTES];
    uint32_t seq_be = htonl(seq);
    std::memcpy(out, &seq_be, 4);
    std::memcpy(out + 4, payload, PAYLOAD_BYTES);
    sendto(g_player_fd, out, sizeof out, 0,
           reinterpret_cast<sockaddr *>(&g_player_addr), sizeof g_player_addr);
}

// After a new DATA or PARITY arrival, see if the group it belongs to can now
// be completed: if exactly one member is still missing, XOR-recover it.
void try_reconstruct_group(uint32_t base_seq) {
    GroupSlot &g = group_slot(base_seq);
    if (!g.have) return;

    int missing_count = 0;
    uint32_t missing_seq = 0;
    uint8_t acc[PAYLOAD_BYTES];
    std::memcpy(acc, g.xor_payload, PAYLOAD_BYTES);

    for (uint8_t k = 0; k < g.count; k++) {
        uint32_t seq = base_seq + k;
        FrameSlot &s = frame_slot(seq);
        if (s.have) {
            for (int i = 0; i < PAYLOAD_BYTES; i++) acc[i] ^= s.payload[i];
        } else {
            missing_count++;
            missing_seq = seq;
        }
    }
    if (missing_count == 1) {
        deliver(missing_seq, acc);  // acc now equals the missing payload
    }
}

}  // namespace

int main() {
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in in_addr{};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, reinterpret_cast<sockaddr *>(&in_addr), sizeof in_addr) < 0) {
        perror("bind 47002");
        return 1;
    }

    g_player_fd = socket(AF_INET, SOCK_DGRAM, 0);
    g_player_addr = sockaddr_in{};
    g_player_addr.sin_family = AF_INET;
    g_player_addr.sin_port = htons(47020);
    g_player_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    uint8_t buf[512];
    for (;;) {
        ssize_t n = recvfrom(in_fd, buf, sizeof buf, 0, nullptr, nullptr);
        if (n <= 0) continue;
        uint8_t type = buf[0];

        if (type == DATA && n == static_cast<ssize_t>(DATA_PACKET_SIZE)) {
            uint32_t seq = decode_seq_be(buf + 1);
            FrameSlot &s = frame_slot(seq);
            if (s.delivered) continue;  // duplicate -- drop
            deliver(seq, buf + 5);
            try_reconstruct_group(seq - (seq % FEC_GROUP));

        } else if (type == PARITY && n == static_cast<ssize_t>(PARITY_PACKET_SIZE)) {
            uint32_t base_seq = decode_seq_be(buf + 1);
            uint8_t count = buf[5];
            GroupSlot &g = group_slot(base_seq);
            g.have = true;
            g.count = count;
            std::memcpy(g.xor_payload, buf + 6, PAYLOAD_BYTES);
            try_reconstruct_group(base_seq);
        }
        // NACK is sender-side only; unrecognized/short packets are ignored.
    }
    return 0;
}
