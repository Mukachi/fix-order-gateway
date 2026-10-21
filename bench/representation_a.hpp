// Representation A: the conventional design, measured as the baseline and not
// shipped (note 002). Decoding copies every field into a hash map from tag to
// string; encoding collects fields as strings, then serialises them. Both
// allocate by nature. Written for this comparison from the FIX rules, not
// from any engine's source. Design: docs/design/002-codec.md
#pragma once

#include <fixgw/framing.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fixgw::bench {

// Decode: each field's value copied into the map (a repeated tag keeps its
// last value). Returns false on a malformed field.
inline bool decode_a(const char* p, std::size_t n, std::unordered_map<std::uint32_t, std::string>& out) {
    out.clear();
    std::size_t i = 0;
    while (i < n) {
        std::uint32_t tag = 0;
        const std::size_t start = i;
        while (i < n && p[i] >= '0' && p[i] <= '9') tag = tag * 10 + static_cast<std::uint32_t>(p[i++] - '0');
        if (i == start || i == n || p[i] != '=') return false;
        const std::size_t v = ++i;
        while (i < n && p[i] != soh) ++i;
        if (i == n) return false;
        out[tag] = std::string(p + v, i - v);
        ++i;
    }
    return true;
}

// Encode: fields as (tag, string) pairs in order, serialised with BodyLength
// and CheckSum into a new string.
class message_a {
public:
    explicit message_a(std::string msg_type) { fields_.emplace_back(35, std::move(msg_type)); }
    void set(std::uint32_t tag, std::string value) { fields_.emplace_back(tag, std::move(value)); }
    void set(std::uint32_t tag, std::uint64_t value) { fields_.emplace_back(tag, std::to_string(value)); }
    void set(std::uint32_t tag, char value) { fields_.emplace_back(tag, std::string(1, value)); }

    std::string serialise() const {
        std::string body;
        for (const auto& [tag, value] : fields_) {
            body += std::to_string(tag);
            body += '=';
            body += value;
            body += soh;
        }
        std::string m = "8=FIX.4.4";
        m += soh;
        m += "9=" + std::to_string(body.size());
        m += soh;
        m += body;
        char t[8];
        std::snprintf(t, sizeof(t), "10=%03u", checksum(m.data(), m.size()));
        m += t;
        m += soh;
        return m;
    }

private:
    std::vector<std::pair<std::uint32_t, std::string>> fields_;
};

}  // namespace fixgw::bench
