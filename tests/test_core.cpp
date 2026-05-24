#include "data_store.h"
#include "mask.h"
#include "request.h"
#include "sm4.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}

void require_eq(const std::string& actual, const std::string& expected, const char* message) {
    if (actual != expected) {
        std::cerr << message << "\nexpected: " << expected << "\nactual:   " << actual << "\n";
        throw std::runtime_error(message);
    }
}

std::string write_fixture_csv() {
    const std::string path = "/tmp/dcc_cpp_test_table_data.csv";
    std::ofstream out(path, std::ios::binary);
    out << "user_id,serial_no,user_code,business_key,id_card,phone,name,email,device_id,trans_id,secret_code\n";
    out << "00001,1000000000001,DINP,U$,000110101199001021,13000000001,尹柱京,e5b0b9e60001@163.com,11:2E:4B:68:85:A2,202600000000000001,K2AJTM\n";
    out << "00002,1000000000002,UEVY,$A_,000110101199001032,13000000002,鲍帝昨蜡,e9b28de50002@cmbchina.com,22:3F:5C:79:96:B3,202600000000000002,9EB2UF\n";
    return path;
}

void test_sm4_standard_block_vector() {
    const auto key = dcc::bytes_from_hex("0123456789ABCDEFFEDCBA9876543210");
    const auto plain = dcc::bytes_from_hex("0123456789ABCDEFFEDCBA9876543210");
    dcc::Sm4KeySchedule schedule;
    dcc::sm4_set_encrypt_key(key.data(), schedule);

    unsigned char out[16];
    dcc::sm4_encrypt_block(plain.data(), out, schedule);

    require_eq(dcc::hex_upper(out, 16), "681EDF34D206965E86B3E94F536E4246",
               "SM4 block encryption must match the official vector");
}

void test_mask_matches_baseline_rules() {
    require_eq(dcc::mask_value(""), "", "empty strings stay empty");
    require_eq(dcc::mask_value("1823"), "1#####", "short ASCII values use first char plus five hashes");
    require_eq(dcc::mask_value("zhangsan@qq.com"), "z####m", "long ASCII values keep first and last char");
    require_eq(dcc::mask_value("尹柱京"), "尹#####", "short UTF-8 values count Unicode code points");
    require_eq(dcc::mask_value("阿蕾奇诺ABC"), "阿####C", "long UTF-8 values keep first and last code point");
}

void test_request_json_parser_preserves_field_order() {
    const std::string body =
        "{ \"requestId\" : \"REQ_1\", \"sm4Key\" : \"2123433411630000\", "
        "\"ip\" : \"55.51.53.74\", \"fieldsToEncrypt\" : [ \"phone\", \"user_code\", \"name\" ] }";

    const dcc::EncryptRequest req = dcc::parse_encrypt_request(body);
    require_eq(req.request_id, "REQ_1", "requestId parsed");
    require_eq(req.sm4_key, "2123433411630000", "sm4Key parsed");
    require_eq(req.ip, "55.51.53.74", "ip parsed");
    require(req.fields.size() == 3, "field count parsed");
    require_eq(req.fields[0], "phone", "field order 0");
    require_eq(req.fields[1], "user_code", "field order 1");
    require_eq(req.fields[2], "name", "field order 2");
}

void test_data_store_renders_rows_in_requested_order() {
    const std::string csv_path = write_fixture_csv();
    dcc::DataStore store;
    store.load(csv_path);
    require(store.row_count() == 2, "fixture has two data rows");

    const std::vector<std::string> fields = {"phone", "user_code", "name"};
    dcc::Sm4KeySchedule schedule;
    dcc::sm4_set_encrypt_key(reinterpret_cast<const unsigned char*>("2123433411630000"), schedule);

    std::string row;
    store.append_rendered_row(0, fields, schedule, row);

    std::string expected = "1####1,";
    expected += dcc::sm4_cbc_pkcs7_hex("DINP", schedule);
    expected += ",尹#####\n";
    require_eq(row, expected, "rendered row keeps requested field order and baseline output format");
}

} // namespace

int main() {
    try {
        test_sm4_standard_block_vector();
        test_mask_matches_baseline_rules();
        test_request_json_parser_preserves_field_order();
        test_data_store_renders_rows_in_requested_order();
    } catch (const std::exception& e) {
        std::cerr << "test failed: " << e.what() << "\n";
        return 1;
    }
    std::cout << "all core tests passed\n";
    return 0;
}
