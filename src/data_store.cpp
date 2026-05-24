#include "data_store.h"

#include "mask.h"

#include <fstream>
#include <stdexcept>

namespace dcc {
namespace {

constexpr const char* FIELD_NAMES[11] = {
    "user_id", "serial_no", "user_code", "business_key", "id_card", "phone",
    "name", "email", "device_id", "trans_id", "secret_code"
};

constexpr FieldKind FIELD_KINDS[11] = {
    FieldKind::Sm4, FieldKind::Sm4, FieldKind::Sm4, FieldKind::Sm4,
    FieldKind::Mask, FieldKind::Mask, FieldKind::Mask, FieldKind::Mask,
    FieldKind::Sm4, FieldKind::Sm4, FieldKind::Sm4
};

} // namespace

void DataStore::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to open CSV: " + path);
    }
    file_data_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (file_data_.empty()) {
        throw std::runtime_error("CSV is empty: " + path);
    }
    parse_csv_data();
    precompute_masks();
}

std::size_t DataStore::row_count() const {
    return rows_;
}

FieldRef DataStore::resolve_field(std::string_view name) const {
    for (int i = 0; i < 11; ++i) {
        if (name == FIELD_NAMES[i]) {
            return FieldRef{i, FIELD_KINDS[i]};
        }
    }
    throw std::invalid_argument("unknown field: " + std::string(name));
}

std::vector<FieldRef> DataStore::resolve_fields(const std::vector<std::string>& names) const {
    std::vector<FieldRef> out;
    out.reserve(names.size());
    for (const auto& name : names) {
        out.push_back(resolve_field(name));
    }
    return out;
}

void DataStore::append_rendered_row(std::size_t row,
                                    const std::vector<std::string>& fields,
                                    const Sm4KeySchedule& schedule,
                                    std::string& out) const {
    append_rendered_row(row, resolve_fields(fields), schedule, out);
}

void DataStore::append_rendered_row(std::size_t row,
                                    const std::vector<FieldRef>& fields,
                                    const Sm4KeySchedule& schedule,
                                    std::string& out) const {
    if (row >= rows_) {
        throw std::out_of_range("row out of range");
    }
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        const FieldRef field = fields[i];
        if (field.kind == FieldKind::Sm4) {
            sm4_cbc_pkcs7_hex_append(columns_[field.index][row], schedule, out);
        } else {
            out.append(masked_[field.index][row]);
        }
    }
    out.push_back('\n');
}

void DataStore::parse_csv_data() {
    for (auto& col : columns_) {
        col.clear();
    }
    rows_ = 0;

    std::size_t pos = 0;
    while (pos < file_data_.size() && file_data_[pos] != '\n') {
        ++pos;
    }
    if (pos < file_data_.size()) {
        ++pos;
    }

    std::array<std::string_view, kColumnCount> row{};
    while (pos < file_data_.size()) {
        if (file_data_[pos] == '\n' || file_data_[pos] == '\r') {
            ++pos;
            continue;
        }
        for (std::size_t col = 0; col < kColumnCount; ++col) {
            const std::size_t begin = pos;
            while (pos < file_data_.size() && file_data_[pos] != ',' && file_data_[pos] != '\n' && file_data_[pos] != '\r') {
                ++pos;
            }
            row[col] = std::string_view(file_data_.data() + begin, pos - begin);
            if (col + 1 < kColumnCount) {
                if (pos >= file_data_.size() || file_data_[pos] != ',') {
                    throw std::runtime_error("malformed CSV: expected comma");
                }
                ++pos;
            }
        }
        while (pos < file_data_.size() && file_data_[pos] != '\n') {
            if (file_data_[pos] != '\r') {
                throw std::runtime_error("malformed CSV: too many columns");
            }
            ++pos;
        }
        if (pos < file_data_.size() && file_data_[pos] == '\n') {
            ++pos;
        }
        for (std::size_t col = 0; col < kColumnCount; ++col) {
            columns_[col].push_back(row[col]);
        }
        ++rows_;
    }
}

void DataStore::precompute_masks() {
    for (std::size_t col = 0; col < kColumnCount; ++col) {
        masked_[col].clear();
        if (FIELD_KINDS[col] != FieldKind::Mask) {
            continue;
        }
        masked_[col].reserve(rows_);
        for (std::size_t row = 0; row < rows_; ++row) {
            masked_[col].push_back(mask_value(columns_[col][row]));
        }
    }
}

} // namespace dcc
