

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "qb/json.h"
#include "qb/utility/functional.h"

namespace qb::allocator {

template <>
pipe<char> &
pipe<char>::put<qb::json>(const qb::json &val) {
    switch (val.type()) {
        case qb::json::value_t::object: {
            if (val.empty()) {
                *this << "{}";
                return *this;
            }

            *this << '{';

            // first n-1 elements
            auto i = val.cbegin();
            for (std::size_t cnt = 0; cnt < val.size() - 1; ++cnt, ++i) {
                // Escape the key: a raw key containing '"', '\\' or a control
                // char would otherwise emit syntactically-invalid JSON (and be a
                // JSON-injection vector). `json(key).dump()` yields the fully
                // quoted+escaped string. Values already go through `dump()`.
                *this << qb::json(i.key()).dump();
                *this << ':';
                put(i.value());
                *this << ',';
            }

            // last element
            *this << qb::json(i.key()).dump();
            *this << ':';
            put(i.value());

            *this << '}';

            return *this;
        }

        case qb::json::value_t::array: {
            if (val.empty()) {
                *this << "[]";
                return *this;
            }
            *this << '[';

            // first n-1 elements
            for (auto i = val.cbegin(); i != val.cend() - 1; ++i) {
                put(i.value());
                *this << ',';
            }

            // last element
            put(val.back());

            *this << ']';

            return *this;
        }

        case qb::json::value_t::string: {
            *this << val.dump();
            return *this;
        }

        case qb::json::value_t::boolean: {
            if (val.get<bool>()) {
                *this << "true";
            } else {
                *this << "false";
            }
            return *this;
        }

        case qb::json::value_t::number_integer:
        case qb::json::value_t::number_unsigned:
        case qb::json::value_t::number_float: {
            // Serialize numbers via nlohmann's own writer: it preserves the full
            // 64-bit integer range and emits shortest round-trip floats. The
            // previous get<int>/get<unsigned int>/get<float> truncated every value
            // to 32 bits (a 64-bit timestamp came out negative) and lost double
            // precision.
            *this << val.dump();
            return *this;
        }

        case qb::json::value_t::discarded: {
            *this << "<discarded>";
            return *this;
        }

        case qb::json::value_t::null: {
            *this << "null";
            return *this;
        }

        case qb::json::value_t::binary: {
            // What dump() writes for a binary value: {"bytes":[...],"subtype":n|null}. Without this case the
            // switch fell to its default and wrote NOTHING -- `{"a":}` for a member, `[]` for an array of one.
            *this << val.dump();
            return *this;
        }
    }
    return *this;
}

} // namespace qb::allocator

namespace qb::detail {

// A number hashes as the double it compares as: nlohmann compares an integer with a float through
// static_cast<double>, so 1, 1u and 1.0 are equal and must hash alike; -0.0 == 0.0; NaN equals nothing.
static std::size_t
json_hash_number(double number) noexcept {
    if (std::isnan(number))
        return 0;
    if (number == 0.0)
        number = 0.0;
    std::uint64_t bits = 0;
    std::memcpy(&bits, &number, sizeof bits);
    return std::hash<std::uint64_t>{}(bits);
}

std::size_t
json_hash(const nlohmann::json &value) noexcept {
    using json    = nlohmann::json;
    using value_t = json::value_t;
    // One tag per kind that compares equal only within itself; the three number kinds share one.
    enum : std::size_t { tag_null = 1, tag_boolean, tag_number, tag_string, tag_array, tag_object, tag_binary, tag_discarded };

    std::size_t seed = 0;
    switch (value.type()) {
        case value_t::null:
            hash_combine_one(seed, std::size_t{tag_null});
            break;
        case value_t::boolean:
            hash_combine_one(seed, std::size_t{tag_boolean});
            hash_combine_one(seed, *value.get_ptr<const json::boolean_t *>());
            break;
        case value_t::number_integer:
            hash_combine_one(seed, std::size_t{tag_number});
            hash_combine_one(seed, json_hash_number(static_cast<double>(*value.get_ptr<const json::number_integer_t *>())));
            break;
        case value_t::number_unsigned:
            hash_combine_one(seed, std::size_t{tag_number});
            hash_combine_one(seed, json_hash_number(static_cast<double>(*value.get_ptr<const json::number_unsigned_t *>())));
            break;
        case value_t::number_float:
            hash_combine_one(seed, std::size_t{tag_number});
            hash_combine_one(seed, json_hash_number(*value.get_ptr<const json::number_float_t *>()));
            break;
        case value_t::string:
            // The bytes as stored -- never dump(), which throws on a string that is not valid UTF-8.
            hash_combine_one(seed, std::size_t{tag_string});
            hash_combine_one(seed, std::string_view(*value.get_ptr<const json::string_t *>()));
            break;
        case value_t::array:
            hash_combine_one(seed, std::size_t{tag_array});
            hash_combine_one(seed, value.size());
            for (const json &element : *value.get_ptr<const json::array_t *>())
                hash_combine_one(seed, json_hash(element));
            break;
        case value_t::object:
            // object_t is a sorted map and compares member by member in key order: hash in that same order.
            hash_combine_one(seed, std::size_t{tag_object});
            hash_combine_one(seed, value.size());
            for (const auto &[key, member] : *value.get_ptr<const json::object_t *>()) {
                hash_combine_one(seed, std::string_view(key));
                hash_combine_one(seed, json_hash(member));
            }
            break;
        case value_t::binary: {
            // byte_container_with_subtype compares the bytes, the subtype and whether there is one.
            const json::binary_t &bytes = *value.get_ptr<const json::binary_t *>();
            hash_combine_one(seed, std::size_t{tag_binary});
            hash_combine_one(seed, bytes.has_subtype());
            hash_combine_one(seed, static_cast<std::uint64_t>(bytes.subtype()));
            hash_combine_one(seed, std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
            break;
        }
        case value_t::discarded:
            hash_combine_one(seed, std::size_t{tag_discarded});
            break;
    }
    return seed;
}

} // namespace qb::detail

namespace uuids {
void
to_json(qb::json &obj, qb::uuid const &id) {
    obj = uuids::to_string(id);
}

void
from_json(qb::json const &obj, qb::uuid &id) {
    id = qb::uuid::from_string(obj.get_ref<const std::string &>()).value();
}
} // namespace uuids