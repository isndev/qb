/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
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

#ifndef             QB_IO_STREAM_H_
# define            QB_IO_STREAM_H_
#include <qb/system/allocator/pipe.h>
#include "system/file.h"

namespace qb {
    namespace io {

        template<typename _IO_>
        class istream {
        protected:
            _IO_ _in;
            qb::allocator::pipe<char> _in_buffer;
        public:

            _IO_ &in() {
                return _in;
            }

            int read() {
                static constexpr const std::size_t bucket_read = 4096;
                const auto ret = _in.read(
                        _in_buffer.allocate_back(bucket_read),
                        bucket_read
                );
                if (likely(ret >= 0))
                    _in_buffer.free_back(bucket_read - ret);
                return ret;
            }

            void flush(std::size_t size) {
                _in_buffer.free_front(size);
                if (!_in_buffer.size())
                    _in_buffer.reset();
            }

            void close() {
                _in_buffer.reset();
                _in.close();
            }
        };

        template<typename _IO_, bool _IsFile = std::is_same_v<_IO_, sys::file>>
        class ostream {
        protected:
            _IO_ _out;
            qb::allocator::pipe<char> _out_buffer;
        public:

            _IO_ &out() {
                return _out;
            }

            std::size_t pendingWrite() const {
                return _out_buffer.size();
            }

            int write() {
                static constexpr const std::size_t bucket_write = 2048;
                const auto ret = _out.write(
                        _out_buffer.data() + _out_buffer.begin(),
                        std::min(_out_buffer.size(), bucket_write)
                );
                if (likely(ret > 0)) {
                    _out_buffer.reset(_out_buffer.begin() + ret);
                }
                return ret;
            }

            char *publish(char const *data, std::size_t size) {
                return static_cast<char *>(std::memcpy(_out_buffer.allocate_back(size), data, size));
            }

            void close() {
                _out_buffer.reset();
                _out.close();
            }
        };

        template<typename _IO_>
        class ostream<_IO_, true> {
        protected:
            _IO_ _out;
        public:

            // unused
            _IO_ &out() { return _out; }
            std::size_t pendingWrite() const { return 0; }
            int write() { return 0; }


            char *publish(char const *data, std::size_t size) {
                _out.write(data, size);
                return const_cast<char *>(data);
            }

            void close() {
                _out.close();
            }
        };

        template<typename _IO_, bool _IsFile = std::is_same_v<_IO_, sys::file>>
        struct stream : public istream<_IO_> {
        protected:
            qb::allocator::pipe<char> _out_buffer;
        public:

            _IO_ &out() {
                return this->_in;
            }

            std::size_t pendingWrite() const {
                return _out_buffer.size();
            }

            int write() {
                static constexpr const std::size_t bucket_write = 2048;
                const auto ret = this->_in.write(
                        _out_buffer.data() + _out_buffer.begin(),
                        std::min(_out_buffer.size(), bucket_write)
                );
                if (likely(ret > 0)) {
                    _out_buffer.reset(_out_buffer.begin() + ret);
                }
                return ret;
            }

            char *publish(char const *data, std::size_t size) {
                return static_cast<char *>(std::memcpy(_out_buffer.allocate_back(size), data, size));
            }

            void close() {
                _out_buffer.reset();
                static_cast<istream<_IO_> &>(*this).close();
                this->_in_buffer.reset();
            }
        };

        template<typename _IO_>
        class stream<_IO_, true> : public istream<_IO_> {
        public:
            // unused
            _IO_ &out() { return this->_in; }
            std::size_t pendingWrite() const { return 0; }
            int write() { return 0; }


            char *publish(char const *data, std::size_t size) {
                this->_in.write(data, size);
                return const_cast<char *>(data);
            }
        };

    } // namespace io
} // namespace qb

#endif // QB_IO_STREAM_H_
