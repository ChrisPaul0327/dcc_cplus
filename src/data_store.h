#pragma once

#include "sm4.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace dcc {

enum class FieldKind {
    Sm4,
    Mask
};

struct FieldRef {
    int index = -1;
    FieldKind kind = FieldKind::Sm4;
};

class DataStore {
public:
    void load(const std::string& path);
    std::size_t row_count() const;

    FieldRef resolve_field(std::string_view name) const;
    std::vector<FieldRef> resolve_fields(const std::vector<std::string>& names) const;

    void append_rendered_row(std::size_t row,
                             const std::vector<std::string>& fields,
                             const Sm4KeySchedule& schedule,
                             std::string& out) const;

    void append_rendered_row(std::size_t row,
                             const std::vector<FieldRef>& fields,
                             const Sm4KeySchedule& schedule,
                             std::string& out) const;

private:
    static constexpr std::size_t kColumnCount = 11;

    std::string file_data_;
    std::array<std::vector<std::string_view>, kColumnCount> columns_;
    std::array<std::vector<std::string>, kColumnCount> masked_;
    std::size_t rows_ = 0;

    void parse_csv_data();
    void precompute_masks();
};

} // namespace dcc
