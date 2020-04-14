/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2020 isndev (www.qbaf.io). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 *         limitations under the License.
 */

#ifndef QB_IO_STREAM_H_
#define QB_IO_STREAM_H_
#include <qb/system/allocator/pipe.h>
#include <qb/utility/type_traits.h>

namespace qb::io {

template <typename _IO_>
class istream {
public:
    using input_io_type = _IO_;
    using input_buffer_type = qb::allocator::pipe<char>;

protected:
    _IO_ _in;
    input_buffer_type _in_buffer;

public:
    ~istream() noexcept {
        close();
    }

    [[nodiscard]] _IO_ &
    transport() noexcept {
        return _in;
    }

    [[nodiscard]] const _IO_ &
    transport() const noexcept {
        return _in;
    }

    [[nodiscard]] input_buffer_type &
    in() noexcept {
        return _in_buffer;
    }

    [[nodiscard]] std::size_t
    pendingRead() const noexcept {
        return _in_buffer.size();
    }

    template <typename Available = void>
    [[nodiscard]] int
    read(std::enable_if_t<has_method_read<_IO_, int, char *, std::size_t>::value, Available> * = nullptr) noexcept {
        static constexpr const std::size_t bucket_read = 4096;
        const auto ret = _in.read(_in_buffer.allocate_back(bucket_read), bucket_read);
        if (likely(ret >= 0))
            _in_buffer.free_back(bucket_read - ret);
        return ret;
    }

    void
    flush(std::size_t size) noexcept {
        _in_buffer.free_front(size);
    }

    void
    eof() noexcept {
        if (!_in_buffer.size())
            _in_buffer.reset();
        else
            _in_buffer.reorder();
    }

    void
    close() noexcept {
        _in_buffer.reset();
        if constexpr (has_member_func_disconnect<_IO_>::value)
            _in.disconnect();
        else
            _in.close();
    }
};

template <typename _IO_>
class ostream {
public:
    using output_io_type = _IO_;
    using output_buffer_type = qb::allocator::pipe<char>;

protected:
    _IO_ _out;
    output_buffer_type _out_buffer;

public:
    ~ostream() noexcept {
        close();
    }

    [[nodiscard]] _IO_ &
    transport() noexcept {
        return _out;
    }

    [[nodiscard]] const _IO_ &
    transport() const noexcept {
        return _out;
    }

    [[nodiscard]] output_buffer_type &
    out() noexcept {
        return _out_buffer;
    }

    [[nodiscard]] std::size_t
    pendingWrite() const noexcept {
        return _out_buffer.size();
    }

    template <typename Available = void>
    [[nodiscard]] int
    write(std::enable_if_t<has_method_write<_IO_, int, const char *, std::size_t>::value, Available> * = nullptr) noexcept {
        const auto ret = _out.write(_out_buffer.begin(), _out_buffer.size());

        if (likely(ret > 0)) {
            if (ret != _out_buffer.size()) {
                _out_buffer.free_front(ret);
                _out_buffer.reorder();
            } else
                _out_buffer.reset();
        }

        return ret;
    }

    char *
    publish(char const *data, std::size_t size) noexcept {
        return static_cast<char *>(
            std::memcpy(_out_buffer.allocate_back(size), data, size));
    }

    void
    close() noexcept {
        _out_buffer.reset();
        if constexpr (has_member_func_disconnect<_IO_>::value)
            _out.disconnect();
        else
            _out.close();
    }
};

template <typename _IO_>
struct stream : public istream<_IO_> {
public:
    using output_io_type = _IO_;
    using output_buffer_type = qb::allocator::pipe<char>;
    constexpr static const bool has_reset_on_pending_read = false;

protected:
    output_buffer_type _out_buffer;

public:
    [[nodiscard]] output_buffer_type &
    out() noexcept {
        return _out_buffer;
    }

    [[nodiscard]] std::size_t
    pendingWrite() const noexcept {
        return _out_buffer.size();
    }

    template <typename Available = void>
    [[nodiscard]] int
    write(std::enable_if_t<has_method_write<_IO_, int, const char *, std::size_t>::value, Available> * = nullptr) noexcept {
        const auto ret = this->_in.write(_out_buffer.begin(), _out_buffer.size());
        if (likely(ret > 0)) {
            if (ret != _out_buffer.size()) {
                _out_buffer.free_front(ret);
                _out_buffer.reorder();
            } else
                _out_buffer.reset();
        }
        return ret;
    }

    char *
    publish(char const *data, std::size_t size) noexcept {
        return static_cast<char *>(
            std::memcpy(_out_buffer.allocate_back(size), data, size));
    }

    void
    close() noexcept {
        _out_buffer.reset();
        static_cast<istream<_IO_> &>(*this).close();
    }
};

} // namespace qb::io

#endif // QB_IO_STREAM_H_
