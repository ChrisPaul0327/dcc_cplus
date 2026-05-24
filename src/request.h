#pragma once

#include <string>
#include <vector>

namespace dcc {

struct EncryptRequest {
    std::string request_id;
    std::string sm4_key;
    std::string ip;
    std::vector<std::string> fields;
};

EncryptRequest parse_encrypt_request(const std::string& body);
std::string json_escape(const std::string& value);
std::string bee_success_json();
std::string bee_failed_json(const std::string& message);

} // namespace dcc
