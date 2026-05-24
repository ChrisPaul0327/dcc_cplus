#include "request.h"

#include <cctype>
#include <stdexcept>

namespace dcc {
namespace {

void skip_ws(const std::string& s, std::size_t& pos) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) {
        ++pos;
    }
}

std::string parse_json_string_at(const std::string& s, std::size_t& pos) {
    skip_ws(s, pos);
    if (pos >= s.size() || s[pos] != '"') {
        throw std::invalid_argument("expected JSON string");
    }
    ++pos;
    std::string out;
    while (pos < s.size()) {
        const char c = s[pos++];
        if (c == '"') {
            return out;
        }
        if (c == '\\') {
            if (pos >= s.size()) {
                throw std::invalid_argument("invalid JSON escape");
            }
            const char e = s[pos++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: throw std::invalid_argument("unsupported JSON escape");
            }
        } else {
            out.push_back(c);
        }
    }
    throw std::invalid_argument("unterminated JSON string");
}

std::size_t find_key_value_start(const std::string& body, const char* key) {
    const std::string quoted = std::string("\"") + key + "\"";
    std::size_t pos = body.find(quoted);
    if (pos == std::string::npos) {
        throw std::invalid_argument(std::string("missing key: ") + key);
    }
    pos += quoted.size();
    skip_ws(body, pos);
    if (pos >= body.size() || body[pos] != ':') {
        throw std::invalid_argument(std::string("missing colon for key: ") + key);
    }
    ++pos;
    skip_ws(body, pos);
    return pos;
}

std::string extract_string(const std::string& body, const char* key) {
    std::size_t pos = find_key_value_start(body, key);
    return parse_json_string_at(body, pos);
}

std::vector<std::string> extract_string_array(const std::string& body, const char* key) {
    std::size_t pos = find_key_value_start(body, key);
    if (pos >= body.size() || body[pos] != '[') {
        throw std::invalid_argument("expected JSON array");
    }
    ++pos;
    std::vector<std::string> out;
    while (true) {
        skip_ws(body, pos);
        if (pos < body.size() && body[pos] == ']') {
            ++pos;
            return out;
        }
        out.push_back(parse_json_string_at(body, pos));
        skip_ws(body, pos);
        if (pos < body.size() && body[pos] == ',') {
            ++pos;
            continue;
        }
        if (pos < body.size() && body[pos] == ']') {
            ++pos;
            return out;
        }
        throw std::invalid_argument("expected comma or array end");
    }
}

} // namespace

EncryptRequest parse_encrypt_request(const std::string& body) {
    EncryptRequest req;
    req.request_id = extract_string(body, "requestId");
    req.sm4_key = extract_string(body, "sm4Key");
    req.ip = extract_string(body, "ip");
    req.fields = extract_string_array(body, "fieldsToEncrypt");
    if (req.request_id.empty() || req.sm4_key.size() != 16 || req.fields.empty()) {
        throw std::invalid_argument("invalid encrypt request");
    }
    return req;
}

std::string json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string bee_success_json() {
    return "{\"returnCode\":\"SUC0000\",\"errorMsg\":null,\"body\":true}";
}

std::string bee_failed_json(const std::string& message) {
    return "{\"returnCode\":\"ERR0000\",\"errorMsg\":\"" + json_escape(message) + "\",\"body\":false}";
}

} // namespace dcc
