#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dcc {

struct Sm4KeySchedule {
    std::array<uint32_t, 32> rk{};
};

std::vector<unsigned char> bytes_from_hex(std::string_view hex);
std::string hex_upper(const unsigned char* data, std::size_t len);
void hex_upper_append(const unsigned char* data, std::size_t len, std::string& out);
char* hex_upper_write(const unsigned char* data, std::size_t len, char* out);

void sm4_set_encrypt_key(const unsigned char key[16], Sm4KeySchedule& schedule);
void sm4_encrypt_block(const unsigned char in[16], unsigned char out[16], const Sm4KeySchedule& schedule);

std::string sm4_cbc_pkcs7_hex(std::string_view plain, const Sm4KeySchedule& schedule);
void sm4_cbc_pkcs7_hex_append(std::string_view plain, const Sm4KeySchedule& schedule, std::string& out);
char* sm4_cbc_pkcs7_hex_write(std::string_view plain, const Sm4KeySchedule& schedule, char* out);

constexpr const char* kSm4CbcIv = "1234567890123456";

} // namespace dcc
