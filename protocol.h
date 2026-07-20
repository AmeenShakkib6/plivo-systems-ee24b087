// Wire protocol between OUR sender and OUR receiver (ports 47001-47004).
// Entirely our own design -- unrelated to the fixed harness format on 47010/47020.
#pragma once

#include <arpa/inet.h>
#include <cstdint>
#include <cstring>

namespace proto {

constexpr int PAYLOAD_BYTES = 160;
constexpr int FEC_GROUP = 2;      // frames per parity packet (tunable)

enum PacketType : uint8_t {
    DATA   = 0x01,
    PARITY = 0x02,
    NACK   = 0x03,
};

#pragma pack(push, 1)
struct DataPacket {
    uint8_t  type;              // = DATA
    uint32_t seq;                // network byte order on the wire
    uint8_t  payload[PAYLOAD_BYTES];
};

struct ParityPacket {
    uint8_t  type;               // = PARITY
    uint32_t base_seq;            // network byte order on the wire
    uint8_t  count;               // number of frames XORed (<= FEC_GROUP)
    uint8_t  xor_payload[PAYLOAD_BYTES];
};

struct NackPacket {
    uint8_t  type;               // = NACK
    uint32_t seq;                 // network byte order on the wire
};
#pragma pack(pop)

constexpr size_t DATA_PACKET_SIZE   = sizeof(DataPacket);
constexpr size_t PARITY_PACKET_SIZE = sizeof(ParityPacket);
constexpr size_t NACK_PACKET_SIZE   = sizeof(NackPacket);

// Pack/unpack helpers keep htonl/ntohl calls in one place instead of
// scattered across sender.cpp/receiver.cpp.

inline void encode_data(uint8_t *buf, uint32_t seq, const uint8_t *payload) {
    buf[0] = DATA;
    uint32_t seq_be = htonl(seq);
    std::memcpy(buf + 1, &seq_be, 4);
    std::memcpy(buf + 5, payload, PAYLOAD_BYTES);
}

inline void encode_parity(uint8_t *buf, uint32_t base_seq, uint8_t count,
                           const uint8_t *xor_payload) {
    buf[0] = PARITY;
    uint32_t base_be = htonl(base_seq);
    std::memcpy(buf + 1, &base_be, 4);
    buf[5] = count;
    std::memcpy(buf + 6, xor_payload, PAYLOAD_BYTES);
}

inline void encode_nack(uint8_t *buf, uint32_t seq) {
    buf[0] = NACK;
    uint32_t seq_be = htonl(seq);
    std::memcpy(buf + 1, &seq_be, 4);
}

inline uint32_t decode_seq_be(const uint8_t *field) {
    uint32_t v;
    std::memcpy(&v, field, 4);
    return ntohl(v);
}

}  // namespace proto
