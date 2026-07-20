// Sender: harness source (47010) -> us -> relay (47001), with FEC parity sent
// proactively, and NACK-triggered resend served from a short replay buffer.
//
// build: make        run: python3 run.py --delay_ms 60
#include <arpa/inet.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "protocol.h"

using namespace proto;

namespace {

constexpr int RING_SIZE = 256;  // ~5.1s of frames at 20ms/frame

struct RingSlot {
    bool valid = false;
    uint32_t seq = 0;
    uint8_t payload[PAYLOAD_BYTES];
};

RingSlot g_ring[RING_SIZE];
std::mutex g_ring_mutex;

int make_socket() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); _exit(1); }
    return fd;
}

sockaddr_in make_addr(uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    return a;
}

// Thread B: listen for NACKs from the receiver (via relay) and resend any
// frame we still hold in the ring buffer.
void feedback_thread(int fb_fd, int relay_fd, sockaddr_in relay_addr) {
    uint8_t buf[64];
    for (;;) {
        ssize_t n = recvfrom(fb_fd, buf, sizeof buf, 0, nullptr, nullptr);
        if (n < static_cast<ssize_t>(NACK_PACKET_SIZE) || buf[0] != NACK) continue;
        uint32_t seq = decode_seq_be(buf + 1);

        uint8_t out[DATA_PACKET_SIZE];
        bool have_it = false;
        {
            std::lock_guard<std::mutex> lock(g_ring_mutex);
            RingSlot &slot = g_ring[seq % RING_SIZE];
            if (slot.valid && slot.seq == seq) {
                encode_data(out, seq, slot.payload);
                have_it = true;
            }
        }
        if (have_it) {
            sendto(relay_fd, out, sizeof out, 0,
                   reinterpret_cast<sockaddr *>(&relay_addr), sizeof relay_addr);
        }
    }
}

}  // namespace

int main() {
    // Thread A's sockets (ingest from harness, send media to relay).
    int in_fd = make_socket();
    sockaddr_in in_addr = make_addr(47010);
    if (bind(in_fd, reinterpret_cast<sockaddr *>(&in_addr), sizeof in_addr) < 0) {
        perror("bind 47010");
        return 1;
    }
    int relay_fd = make_socket();
    sockaddr_in relay_addr = make_addr(47001);

    // Thread B's socket (feedback from receiver, via relay).
    int fb_fd = make_socket();
    sockaddr_in fb_addr = make_addr(47004);
    if (bind(fb_fd, reinterpret_cast<sockaddr *>(&fb_addr), sizeof fb_addr) < 0) {
        perror("bind 47004");
        return 1;
    }

    std::thread fb_thread(feedback_thread, fb_fd, relay_fd, relay_addr);
    fb_thread.detach();

    uint8_t xor_acc[PAYLOAD_BYTES];
    uint32_t group_base = 0;
    int group_count = 0;

    uint8_t in_buf[2048];
    for (;;) {
        ssize_t n = recvfrom(in_fd, in_buf, sizeof in_buf, 0, nullptr, nullptr);
        if (n < 4) continue;  // must at least contain the harness seq header
        uint32_t seq = decode_seq_be(in_buf);
        const uint8_t *payload = in_buf + 4;
        size_t payload_len = static_cast<size_t>(n) - 4;
        if (payload_len > PAYLOAD_BYTES) payload_len = PAYLOAD_BYTES;

        // 1) forward as DATA immediately -- never held back for FEC bookkeeping.
        uint8_t data_pkt[DATA_PACKET_SIZE];
        uint8_t padded_payload[PAYLOAD_BYTES] = {0};
        std::memcpy(padded_payload, payload, payload_len);
        encode_data(data_pkt, seq, padded_payload);
        sendto(relay_fd, data_pkt, sizeof data_pkt, 0,
               reinterpret_cast<sockaddr *>(&relay_addr), sizeof relay_addr);

        // 2) remember it for possible NACK-triggered resend.
        {
            std::lock_guard<std::mutex> lock(g_ring_mutex);
            RingSlot &slot = g_ring[seq % RING_SIZE];
            slot.valid = true;
            slot.seq = seq;
            std::memcpy(slot.payload, padded_payload, PAYLOAD_BYTES);
        }

        // 3) accumulate into the current FEC group; emit parity once full.
        if (group_count == 0) group_base = seq;
        if (group_count == 0) {
            std::memcpy(xor_acc, padded_payload, PAYLOAD_BYTES);
        } else {
            for (int i = 0; i < PAYLOAD_BYTES; i++) xor_acc[i] ^= padded_payload[i];
        }
        group_count++;
        if (group_count == FEC_GROUP) {
            uint8_t parity_pkt[PARITY_PACKET_SIZE];
            encode_parity(parity_pkt, group_base,
                          static_cast<uint8_t>(group_count), xor_acc);
            sendto(relay_fd, parity_pkt, sizeof parity_pkt, 0,
                   reinterpret_cast<sockaddr *>(&relay_addr), sizeof relay_addr);
            group_count = 0;
        }
    }
    return 0;
}
