#include "sm4.h"

#include <array>
#include <cstring>
#include <stdexcept>

namespace dcc {
namespace {

constexpr uint8_t SBOX[256] = {
    0xd6, 0x90, 0xe9, 0xfe, 0xcc, 0xe1, 0x3d, 0xb7, 0x16, 0xb6, 0x14, 0xc2, 0x28, 0xfb, 0x2c, 0x05,
    0x2b, 0x67, 0x9a, 0x76, 0x2a, 0xbe, 0x04, 0xc3, 0xaa, 0x44, 0x13, 0x26, 0x49, 0x86, 0x06, 0x99,
    0x9c, 0x42, 0x50, 0xf4, 0x91, 0xef, 0x98, 0x7a, 0x33, 0x54, 0x0b, 0x43, 0xed, 0xcf, 0xac, 0x62,
    0xe4, 0xb3, 0x1c, 0xa9, 0xc9, 0x08, 0xe8, 0x95, 0x80, 0xdf, 0x94, 0xfa, 0x75, 0x8f, 0x3f, 0xa6,
    0x47, 0x07, 0xa7, 0xfc, 0xf3, 0x73, 0x17, 0xba, 0x83, 0x59, 0x3c, 0x19, 0xe6, 0x85, 0x4f, 0xa8,
    0x68, 0x6b, 0x81, 0xb2, 0x71, 0x64, 0xda, 0x8b, 0xf8, 0xeb, 0x0f, 0x4b, 0x70, 0x56, 0x9d, 0x35,
    0x1e, 0x24, 0x0e, 0x5e, 0x63, 0x58, 0xd1, 0xa2, 0x25, 0x22, 0x7c, 0x3b, 0x01, 0x21, 0x78, 0x87,
    0xd4, 0x00, 0x46, 0x57, 0x9f, 0xd3, 0x27, 0x52, 0x4c, 0x36, 0x02, 0xe7, 0xa0, 0xc4, 0xc8, 0x9e,
    0xea, 0xbf, 0x8a, 0xd2, 0x40, 0xc7, 0x38, 0xb5, 0xa3, 0xf7, 0xf2, 0xce, 0xf9, 0x61, 0x15, 0xa1,
    0xe0, 0xae, 0x5d, 0xa4, 0x9b, 0x34, 0x1a, 0x55, 0xad, 0x93, 0x32, 0x30, 0xf5, 0x8c, 0xb1, 0xe3,
    0x1d, 0xf6, 0xe2, 0x2e, 0x82, 0x66, 0xca, 0x60, 0xc0, 0x29, 0x23, 0xab, 0x0d, 0x53, 0x4e, 0x6f,
    0xd5, 0xdb, 0x37, 0x45, 0xde, 0xfd, 0x8e, 0x2f, 0x03, 0xff, 0x6a, 0x72, 0x6d, 0x6c, 0x5b, 0x51,
    0x8d, 0x1b, 0xaf, 0x92, 0xbb, 0xdd, 0xbc, 0x7f, 0x11, 0xd9, 0x5c, 0x41, 0x1f, 0x10, 0x5a, 0xd8,
    0x0a, 0xc1, 0x31, 0x88, 0xa5, 0xcd, 0x7b, 0xbd, 0x2d, 0x74, 0xd0, 0x12, 0xb8, 0xe5, 0xb4, 0xb0,
    0x89, 0x69, 0x97, 0x4a, 0x0c, 0x96, 0x77, 0x7e, 0x65, 0xb9, 0xf1, 0x09, 0xc5, 0x6e, 0xc6, 0x84,
    0x18, 0xf0, 0x7d, 0xec, 0x3a, 0xdc, 0x4d, 0x20, 0x79, 0xee, 0x5f, 0x3e, 0xd7, 0xcb, 0x39, 0x48
};

constexpr uint32_t FK[4] = {0xa3b1bac6U, 0x56aa3350U, 0x677d9197U, 0xb27022dcU};

constexpr uint32_t CK[32] = {
    0x00070e15U, 0x1c232a31U, 0x383f464dU, 0x545b6269U,
    0x70777e85U, 0x8c939aa1U, 0xa8afb6bdU, 0xc4cbd2d9U,
    0xe0e7eef5U, 0xfc030a11U, 0x181f262dU, 0x343b4249U,
    0x50575e65U, 0x6c737a81U, 0x888f969dU, 0xa4abb2b9U,
    0xc0c7ced5U, 0xdce3eaf1U, 0xf8ff060dU, 0x141b2229U,
    0x30373e45U, 0x4c535a61U, 0x686f767dU, 0x848b9299U,
    0xa0a7aeb5U, 0xbcc3cad1U, 0xd8dfe6edU, 0xf4fb0209U,
    0x10171e25U, 0x2c333a41U, 0x484f565dU, 0x646b7279U
};

constexpr char HEX[] = "0123456789ABCDEF";

struct RoundTables {
    std::array<uint32_t, 256> t0{};
    std::array<uint32_t, 256> t1{};
    std::array<uint32_t, 256> t2{};
    std::array<uint32_t, 256> t3{};
};

struct HexPairs {
    std::array<std::array<char, 2>, 256> pair{};
};

inline uint32_t rotl(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

inline uint32_t load_be(const unsigned char* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

inline void store_be(uint32_t v, unsigned char* p) {
    p[0] = static_cast<unsigned char>(v >> 24);
    p[1] = static_cast<unsigned char>(v >> 16);
    p[2] = static_cast<unsigned char>(v >> 8);
    p[3] = static_cast<unsigned char>(v);
}

inline uint32_t tau(uint32_t x) {
    return (static_cast<uint32_t>(SBOX[(x >> 24) & 0xff]) << 24) |
           (static_cast<uint32_t>(SBOX[(x >> 16) & 0xff]) << 16) |
           (static_cast<uint32_t>(SBOX[(x >> 8) & 0xff]) << 8) |
           static_cast<uint32_t>(SBOX[x & 0xff]);
}

inline uint32_t l_transform(uint32_t b) {
    return b ^ rotl(b, 2) ^ rotl(b, 10) ^ rotl(b, 18) ^ rotl(b, 24);
}

inline uint32_t key_l_transform(uint32_t b) {
    return b ^ rotl(b, 13) ^ rotl(b, 23);
}

RoundTables make_round_tables() {
    RoundTables tables;
    for (int i = 0; i < 256; ++i) {
        const uint32_t b = SBOX[i];
        tables.t0[static_cast<std::size_t>(i)] = l_transform(b << 24);
        tables.t1[static_cast<std::size_t>(i)] = l_transform(b << 16);
        tables.t2[static_cast<std::size_t>(i)] = l_transform(b << 8);
        tables.t3[static_cast<std::size_t>(i)] = l_transform(b);
    }
    return tables;
}

HexPairs make_hex_pairs() {
    HexPairs pairs;
    for (int i = 0; i < 256; ++i) {
        pairs.pair[static_cast<std::size_t>(i)][0] = HEX[(i >> 4) & 0x0f];
        pairs.pair[static_cast<std::size_t>(i)][1] = HEX[i & 0x0f];
    }
    return pairs;
}

inline uint32_t round_t(uint32_t x) {
    static const RoundTables tables = make_round_tables();
    return tables.t0[(x >> 24) & 0xff] ^
           tables.t1[(x >> 16) & 0xff] ^
           tables.t2[(x >> 8) & 0xff] ^
           tables.t3[x & 0xff];
}

inline uint32_t key_t(uint32_t x) {
    return key_l_transform(tau(x));
}

} // namespace

std::vector<unsigned char> bytes_from_hex(std::string_view hex) {
    if (hex.size() % 2 != 0) {
        throw std::invalid_argument("hex string must have even length");
    }
    auto val = [](char c) -> unsigned char {
        if (c >= '0' && c <= '9') return static_cast<unsigned char>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned char>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<unsigned char>(c - 'A' + 10);
        throw std::invalid_argument("invalid hex character");
    };
    std::vector<unsigned char> out(hex.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<unsigned char>((val(hex[i * 2]) << 4) | val(hex[i * 2 + 1]));
    }
    return out;
}

std::string hex_upper(const unsigned char* data, std::size_t len) {
    std::string out;
    out.reserve(len * 2);
    hex_upper_append(data, len, out);
    return out;
}

void hex_upper_append(const unsigned char* data, std::size_t len, std::string& out) {
    const std::size_t base = out.size();
    out.resize(base + len * 2);
    (void)hex_upper_write(data, len, out.data() + base);
}

char* hex_upper_write(const unsigned char* data, std::size_t len, char* out) {
    static const HexPairs pairs = make_hex_pairs();
    char* dst = out;
    for (std::size_t i = 0; i < len; ++i) {
        std::memcpy(dst + i * 2, pairs.pair[data[i]].data(), 2);
    }
    return out + len * 2;
}

void sm4_set_encrypt_key(const unsigned char key[16], Sm4KeySchedule& schedule) {
    uint32_t k[36];
    for (int i = 0; i < 4; ++i) {
        k[i] = load_be(key + i * 4) ^ FK[i];
    }
    for (int i = 0; i < 32; ++i) {
        k[i + 4] = k[i] ^ key_t(k[i + 1] ^ k[i + 2] ^ k[i + 3] ^ CK[i]);
        schedule.rk[i] = k[i + 4];
    }
}

void sm4_encrypt_block(const unsigned char in[16], unsigned char out[16], const Sm4KeySchedule& schedule) {
    uint32_t x0 = load_be(in);
    uint32_t x1 = load_be(in + 4);
    uint32_t x2 = load_be(in + 8);
    uint32_t x3 = load_be(in + 12);
    for (int i = 0; i < 32; ++i) {
        const uint32_t next = x0 ^ round_t(x1 ^ x2 ^ x3 ^ schedule.rk[i]);
        x0 = x1;
        x1 = x2;
        x2 = x3;
        x3 = next;
    }
    store_be(x3, out);
    store_be(x2, out + 4);
    store_be(x1, out + 8);
    store_be(x0, out + 12);
}

std::string sm4_cbc_pkcs7_hex(std::string_view plain, const Sm4KeySchedule& schedule) {
    std::string out;
    out.reserve(((plain.size() / 16) + 1) * 32);
    sm4_cbc_pkcs7_hex_append(plain, schedule, out);
    return out;
}

void sm4_cbc_pkcs7_hex_append(std::string_view plain, const Sm4KeySchedule& schedule, std::string& out) {
    const std::size_t base = out.size();
    out.resize(base + ((plain.size() / 16) + 1) * 32);
    (void)sm4_cbc_pkcs7_hex_write(plain, schedule, out.data() + base);
}

char* sm4_cbc_pkcs7_hex_write(std::string_view plain, const Sm4KeySchedule& schedule, char* out) {
    if (plain.size() < 16) {
        const unsigned char pad = static_cast<unsigned char>(16 - plain.size());
        unsigned char block[16];
        unsigned char cipher[16];
        std::memcpy(block, kSm4CbcIv, 16);
        for (std::size_t i = 0; i < plain.size(); ++i) {
            block[i] ^= static_cast<unsigned char>(plain[i]);
        }
        for (std::size_t i = plain.size(); i < 16; ++i) {
            block[i] ^= pad;
        }
        sm4_encrypt_block(block, cipher, schedule);
        return hex_upper_write(cipher, 16, out);
    }

    unsigned char prev[16];
    std::memcpy(prev, kSm4CbcIv, 16);

    const std::size_t full_blocks = plain.size() / 16;
    const std::size_t rem = plain.size() % 16;
    const std::size_t total_blocks = full_blocks + 1;
    unsigned char block[16];
    unsigned char cipher[16];

    for (std::size_t b = 0; b < total_blocks; ++b) {
        std::memset(block, 0, sizeof(block));
        if (b < full_blocks) {
            std::memcpy(block, plain.data() + b * 16, 16);
        } else {
            const unsigned char pad = static_cast<unsigned char>(16 - rem);
            if (rem > 0) {
                std::memcpy(block, plain.data() + b * 16, rem);
            }
            std::memset(block + rem, pad, 16 - rem);
        }

        for (int i = 0; i < 16; ++i) {
            block[i] ^= prev[i];
        }
        sm4_encrypt_block(block, cipher, schedule);
        out = hex_upper_write(cipher, 16, out);
        std::memcpy(prev, cipher, 16);
    }
    return out;
}

} // namespace dcc
