/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2021 isndev (www.qbaf.io). All rights reserved.
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

#include "qb/io/gzip.h"
#include "qb/system/allocator/pipe.h"

namespace qb::gzip {

template <>
size_t
compress(qb::allocator::pipe<char> &output, const char *data, std::size_t size,
         int level) {

#ifdef DEBUG
    // Verify if size input will fit into unsigned int, type used for zlib's avail_in
    if (size > std::numeric_limits<unsigned int>::max()) {
        throw std::runtime_error("size arg is too large to fit into unsigned int type");
    }
#endif

    z_stream deflate_s;
    deflate_s.zalloc = Z_NULL;
    deflate_s.zfree = Z_NULL;
    deflate_s.opaque = Z_NULL;
    deflate_s.avail_in = 0;
    deflate_s.next_in = Z_NULL;

    // The windowBits parameter is the base two logarithm of the window size (the size of
    // the history buffer). It should be in the range 8..15 for this version of the
    // library. Larger values of this parameter result in better compression at the
    // expense of memory usage. This range of values also changes the decoding type:
    //  -8 to -15 for raw deflate
    //  8 to 15 for zlib
    // (8 to 15) + 16 for gzip
    // (8 to 15) + 32 to automatically detect gzip/zlib header (decompression/inflate
    // only)
    constexpr int window_bits = 15 + 16; // gzip with window_bits of 15

    constexpr int mem_level = 8;
    // The memory requirements for deflate are (in bytes):
    // (1 << (window_bits+2)) +  (1 << (mem_level+9))
    // with a default value of 8 for mem_level and our window_bits of 15
    // this is 128Kb

DISABLE_WARNING_PUSH
DISABLE_WARNING_OLD_STYLE_CAST
    if (deflateInit2(&deflate_s, level, Z_DEFLATED, window_bits, mem_level,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("deflate init failed");
    }
DISABLE_WARNING_POP

    const std::size_t out_size = output.size();
    deflate_s.next_in = reinterpret_cast<z_const Bytef *>(data);
    deflate_s.avail_in = static_cast<unsigned int>(size);

    std::size_t size_compressed = 0;
    do {
        size_t increase = size / 2 + 1024;
        if ((output.size() - out_size) < (size_compressed + increase)) {
            output.allocate_back(increase);
        }
        // There is no way we see that "increase" would not fit in an unsigned int,
        // hence we use static cast here to avoid -Wshorten-64-to-32 error
        deflate_s.avail_out = static_cast<unsigned int>(increase);
        deflate_s.next_out =
            reinterpret_cast<Bytef *>((output.begin() + out_size + size_compressed));
        // From http://www.zlib.net/zlib_how.html
        // "deflate() has a return value that can indicate errors, yet we do not check it
        // here. Why not? Well, it turns out that deflate() can do no wrong here."
        // Basically only possible error is from deflateInit not working properly
        deflate(&deflate_s, Z_FINISH);
        size_compressed += (increase - deflate_s.avail_out);
    } while (deflate_s.avail_out == 0);

    deflateEnd(&deflate_s);
    output.free_back(output.size() - (size_compressed + out_size));
    return size_compressed;
}

std::string
compress(const char *data, std::size_t size, int level) {
    std::string output;
    compress(output, data, size, level);
    return output;
}

template <>
size_t
uncompress(qb::allocator::pipe<char> &output, const char *data, std::size_t size,
           std::size_t max) {
    z_stream inflate_s;

    inflate_s.zalloc = Z_NULL;
    inflate_s.zfree = Z_NULL;
    inflate_s.opaque = Z_NULL;
    inflate_s.avail_in = 0;
    inflate_s.next_in = Z_NULL;

    // The windowBits parameter is the base two logarithm of the window size (the size of
    // the history buffer). It should be in the range 8..15 for this version of the
    // library. Larger values of this parameter result in better compression at the
    // expense of memory usage. This range of values also changes the decoding type:
    //  -8 to -15 for raw deflate
    //  8 to 15 for zlib
    // (8 to 15) + 16 for gzip
    // (8 to 15) + 32 to automatically detect gzip/zlib header
    constexpr int window_bits = 15 + 32; // auto with windowbits of 15

DISABLE_WARNING_PUSH
DISABLE_WARNING_OLD_STYLE_CAST
    if (inflateInit2(&inflate_s, window_bits) != Z_OK) {
        throw std::runtime_error("inflate init failed");
    }
DISABLE_WARNING_POP
    inflate_s.next_in = reinterpret_cast<z_const Bytef *>(data);

#ifdef DEBUG
    // Verify if size (long type) input will fit into unsigned int, type used for zlib's
    // avail_in
    std::uint64_t size_64 = size * 2;
    if (size_64 > std::numeric_limits<unsigned int>::max()) {
        inflateEnd(&inflate_s);
        throw std::runtime_error(
            "size arg is too large to fit into unsigned int type x2");
    }
#endif
    if (max && (size > max || (size * 2) > max)) {
        inflateEnd(&inflate_s);
        throw std::runtime_error(
            "size may use more memory than intended when decompressing");
    }
    inflate_s.avail_in = static_cast<unsigned int>(size);
    const std::size_t out_size = output.size();
    std::size_t size_uncompressed = 0;
    do {
        std::size_t resize_to = size_uncompressed + 2 * size;
        if (max && resize_to > max) {
            inflateEnd(&inflate_s);
            throw std::runtime_error("size of output string will use more memory then "
                                     "intended when decompressing");
        }
        output.allocate_back(2 * size);
        inflate_s.avail_out = static_cast<unsigned int>(2 * size);
        inflate_s.next_out =
            reinterpret_cast<Bytef *>(output.begin() + out_size + size_uncompressed);
        int ret = inflate(&inflate_s, Z_FINISH);
        if (ret != Z_STREAM_END && ret != Z_OK && ret != Z_BUF_ERROR) {
            std::string error_msg = inflate_s.msg;
            inflateEnd(&inflate_s);
            throw std::runtime_error(error_msg);
        }

        size_uncompressed += (2 * size - inflate_s.avail_out);
    } while (inflate_s.avail_out == 0);
    inflateEnd(&inflate_s);
    output.free_back(output.size() - (size_uncompressed + out_size));
    return size_uncompressed;
}

std::string
uncompress(const char *data, std::size_t size) {
    std::string output;
    uncompress(output, data, size);
    return output;
}

} // namespace qb::gzip

namespace qb::allocator {

template <>
pipe<char> &
pipe<char>::put(qb::gzip::to_compress &info) {
    qb::gzip::compress(*this, info);
    return *this;
}

template <>
pipe<char> &pipe<char>::put(qb::gzip::to_uncompress &info){
    qb::gzip::uncompress(*this, info);
    return *this;
}

} // namespace qb::allocator