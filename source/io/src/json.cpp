

#include "qb/json.h"

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
                *this << '\"';
                *this << i.key();
                *this << "\":";
                put(i.value());
                *this << ',';
            }

            // last element
            *this << '\"';
            *this << i.key();
            *this << "\":";
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

        case qb::json::value_t::number_integer: {
            *this << val.get<int>();
            return *this;
        }

        case qb::json::value_t::number_unsigned: {
            *this << val.get<unsigned int>();
            return *this;
        }

        case qb::json::value_t::number_float: {
            *this << val.get<float>();
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

        default:
            break; // LCOV_EXCL_LINE
    }
    return *this;
}

} // namespace qb::allocator

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