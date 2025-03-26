/**
 * @file qb/io/src/compression.cpp
 * @brief Implementation of compression and decompression interfaces
 * 
 * @details This file provides implementations for various compression algorithms including 
 * GZIP and DEFLATE using zlib. It includes compressors, decompressors, and factory classes 
 * for creating compression/decompression providers. The implementation supports both streaming 
 * and one-shot compression operations.
 * 
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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
 * limitations under the License.
 * @ingroup IO
 */

#include <qb/io/compression.h>

#if defined(QB_IO_WITH_ZLIB)
#    include <zlib.h>
// zconf.h may define compress
#    ifdef compress
#        undef compress
#    endif
#endif
#define _XPLATSTR(x) x

static bool
iequals(const std::string &a, const std::string &b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](char a, char b) { return tolower(a) == tolower(b); });
}

namespace qb {
namespace compression {
namespace builtin {
#if defined(QB_IO_WITH_ZLIB)
// A shared base class for the gzip and deflate compressors
class zlib_compressor_base : public compress_provider {
public:
    static const std::string GZIP;
    static const std::string DEFLATE;

    zlib_compressor_base(int windowBits, int compressionLevel = Z_DEFAULT_COMPRESSION,
                         int method = Z_DEFLATED, int strategy = Z_DEFAULT_STRATEGY,
                         int memLevel = MAX_MEM_LEVEL)
        : m_algorithm(windowBits >= 16 ? GZIP : DEFLATE) {
        m_state = deflateInit2(&m_stream, compressionLevel, method, windowBits, memLevel,
                               strategy);
    }

    const std::string &
    algorithm() const {
        return m_algorithm;
    }

    size_t
    compress(const uint8_t *input, size_t input_size, uint8_t *output,
             size_t output_size, operation_hint hint, size_t &input_bytes_processed,
             bool &done) {
        if (m_state == Z_STREAM_END ||
            (hint != operation_hint::is_last && !input_size)) {
            input_bytes_processed = 0;
            done = (m_state == Z_STREAM_END);
            return 0;
        }

        if (m_state != Z_OK && m_state != Z_BUF_ERROR && m_state != Z_STREAM_ERROR) {
            throw std::runtime_error("Prior unrecoverable compression stream error " +
                                     std::to_string(m_state));
        }

#    if defined(__clang__)
#        pragma clang diagnostic push
#        pragma clang diagnostic ignored "-Wtautological-constant-compare"
#    endif // __clang__
        if (input_size > (std::numeric_limits<unsigned int>::max)() ||
            output_size > (std::numeric_limits<unsigned int>::max)())
#    if defined(__clang__)
#        pragma clang diagnostic pop
#    endif // __clang__
        {
            throw std::runtime_error("Compression input or output size out of range");
        }

        m_stream.next_in = const_cast<uint8_t *>(input);
        m_stream.avail_in = static_cast<unsigned int>(input_size);
        m_stream.next_out = const_cast<uint8_t *>(output);
        m_stream.avail_out = static_cast<unsigned int>(output_size);

        m_state = ::deflate(
            &m_stream, (hint == operation_hint::is_last) ? Z_FINISH : Z_PARTIAL_FLUSH);
        if (m_state != Z_OK && m_state != Z_STREAM_ERROR &&
            !(hint == operation_hint::is_last &&
              (m_state == Z_STREAM_END || m_state == Z_BUF_ERROR))) {
            throw std::runtime_error("Unrecoverable compression stream error " +
                                     std::to_string(m_state));
        }

        input_bytes_processed = input_size - m_stream.avail_in;
        done = (m_state == Z_STREAM_END);
        return output_size - m_stream.avail_out;
    }

    void
    reset() {
        m_state = deflateReset(&m_stream);
        if (m_state != Z_OK) {
            throw std::runtime_error("Failed to reset zlib compressor " +
                                     std::to_string(m_state));
        }
    }

    ~zlib_compressor_base() {
        (void)deflateEnd(&m_stream);
    }

private:
    int m_state{Z_BUF_ERROR};
    z_stream m_stream{};
    const std::string &m_algorithm;
};

const std::string zlib_compressor_base::GZIP(algorithm::GZIP);
const std::string zlib_compressor_base::DEFLATE(algorithm::DEFLATE);

// A shared base class for the gzip and deflate decompressors
class zlib_decompressor_base : public decompress_provider {
public:
    zlib_decompressor_base(int windowBits)
        : m_algorithm(windowBits >= 16 ? zlib_compressor_base::GZIP
                                       : zlib_compressor_base::DEFLATE) {
        m_state = inflateInit2(&m_stream, windowBits);
    }

    const std::string &
    algorithm() const {
        return m_algorithm;
    }

    size_t
    decompress(const uint8_t *input, size_t input_size, uint8_t *output,
               size_t output_size, operation_hint hint, size_t &input_bytes_processed,
               bool &done) {
        if (m_state == Z_STREAM_END || !input_size) {
            input_bytes_processed = 0;
            done = (m_state == Z_STREAM_END);
            return 0;
        }

        if (m_state != Z_OK && m_state != Z_BUF_ERROR && m_state != Z_STREAM_ERROR) {
            throw std::runtime_error("Prior unrecoverable decompression stream error " +
                                     std::to_string(m_state));
        }

#    if defined(__clang__)
#        pragma clang diagnostic push
#        pragma clang diagnostic ignored "-Wtautological-constant-compare"
#    endif // __clang__
        if (input_size > (std::numeric_limits<unsigned int>::max)() ||
            output_size > (std::numeric_limits<unsigned int>::max)())
#    if defined(__clang__)
#        pragma clang diagnostic pop
#    endif // __clang__
        {
            throw std::runtime_error("Compression input or output size out of range");
        }

        m_stream.next_in = const_cast<uint8_t *>(input);
        m_stream.avail_in = static_cast<unsigned int>(input_size);
        m_stream.next_out = const_cast<uint8_t *>(output);
        m_stream.avail_out = static_cast<unsigned int>(output_size);

        m_state = inflate(
            &m_stream, (hint == operation_hint::is_last) ? Z_FINISH : Z_PARTIAL_FLUSH);
        if (m_state != Z_OK && m_state != Z_STREAM_ERROR && m_state != Z_STREAM_END &&
            m_state != Z_BUF_ERROR) {
            // Z_BUF_ERROR is a success code for Z_FINISH, and the caller can continue as
            // if operation_hint::is_last was not given
            throw std::runtime_error("Unrecoverable decompression stream error " +
                                     std::to_string(m_state));
        }

        input_bytes_processed = input_size - m_stream.avail_in;
        done = (m_state == Z_STREAM_END);
        return output_size - m_stream.avail_out;
    }

    void
    reset() {
        m_state = inflateReset(&m_stream);
        if (m_state != Z_OK) {
            throw std::runtime_error("Failed to reset zlib decompressor " +
                                     std::to_string(m_state));
        }
    }

    ~zlib_decompressor_base() {
        (void)inflateEnd(&m_stream);
    }

private:
    int m_state{Z_BUF_ERROR};
    z_stream m_stream{};
    const std::string &m_algorithm;
};

class gzip_compressor : public zlib_compressor_base {
public:
    gzip_compressor()
        : zlib_compressor_base(31) // 15 is MAX_WBITS in zconf.h; add 16 for gzip
    {}

    gzip_compressor(int compressionLevel, int method, int strategy, int memLevel)
        : zlib_compressor_base(31, compressionLevel, method, strategy, memLevel) {}
};

class gzip_decompressor : public zlib_decompressor_base {
public:
    gzip_decompressor()
        : zlib_decompressor_base(31) // 15 is MAX_WBITS in zconf.h; add 16 for gzip
    {}
};

class deflate_compressor : public zlib_compressor_base {
public:
    deflate_compressor()
        : zlib_compressor_base(15) // 15 is MAX_WBITS in zconf.h
    {}

    deflate_compressor(int compressionLevel, int method, int strategy, int memLevel)
        : zlib_compressor_base(15, compressionLevel, method, strategy, memLevel) {}
};

class deflate_decompressor : public zlib_decompressor_base {
public:
    deflate_decompressor()
        : zlib_decompressor_base(0) // deflate auto-detect
    {}
};

#endif // QB_IO_WITH_ZLIB

// Generic internal implementation of the compress_factory API
class generic_compress_factory : public compress_factory {
public:
    ~generic_compress_factory() noexcept {}
    generic_compress_factory(
        const std::string &algorithm,
        std::function<std::unique_ptr<compress_provider>()> make_compressor)
        : m_algorithm(algorithm)
        , _make_compressor(make_compressor) {}

    const std::string &
    algorithm() const {
        return m_algorithm;
    }

    std::unique_ptr<compress_provider>
    make_compressor() const {
        return _make_compressor();
    }

private:
    const std::string m_algorithm;
    std::function<std::unique_ptr<compress_provider>()> _make_compressor;
};

// Generic internal implementation of the decompress_factory API
class generic_decompress_factory : public decompress_factory {
public:
    ~generic_decompress_factory() noexcept {}
    generic_decompress_factory(
        const std::string &algorithm, uint16_t weight,
        std::function<std::unique_ptr<decompress_provider>()> make_decompressor)
        : m_algorithm(algorithm)
        , m_weight(weight)
        , _make_decompressor(make_decompressor) {}

    const std::string &
    algorithm() const {
        return m_algorithm;
    }

    uint16_t
    weight() const {
        return m_weight;
    }

    std::unique_ptr<decompress_provider>
    make_decompressor() const {
        return _make_decompressor();
    }

private:
    const std::string m_algorithm;
    uint16_t m_weight;
    std::function<std::unique_ptr<decompress_provider>()> _make_decompressor;
};

// "Private" algorithm-to-factory tables for namespace static helpers
static const std::vector<std::shared_ptr<compress_factory>> g_compress_factories
#if defined(QB_IO_WITH_ZLIB)
    = {std::make_shared<generic_compress_factory>(
           algorithm::GZIP,
           []() -> std::unique_ptr<compress_provider> {
               return std::make_unique<gzip_compressor>();
           }),
       std::make_shared<generic_compress_factory>(
           algorithm::DEFLATE, []() -> std::unique_ptr<compress_provider> {
               return std::make_unique<deflate_compressor>();
           })};
#else  // QB_IO_WITH_ZLIB
    ;
#endif // QB_IO_WITH_ZLIB

static const std::vector<std::shared_ptr<decompress_factory>> g_decompress_factories
#if defined(QB_IO_WITH_ZLIB)
    = {std::make_shared<generic_decompress_factory>(
           algorithm::GZIP, 500,
           []() -> std::unique_ptr<decompress_provider> {
               return std::make_unique<gzip_decompressor>();
           }),
       std::make_shared<generic_decompress_factory>(
           algorithm::DEFLATE, 500, []() -> std::unique_ptr<decompress_provider> {
               return std::make_unique<deflate_decompressor>();
           })};
#else  // QB_IO_WITH_ZLIB
    ;
#endif // QB_IO_WITH_ZLIB

bool
supported() {
    return !g_compress_factories.empty();
}

bool
algorithm::supported(const std::string &algorithm) {
    for (auto &factory : g_compress_factories) {
        if (iequals(algorithm, factory->algorithm())) {
            return true;
        }
    }

    return false;
}

static std::unique_ptr<compress_provider>
_make_compressor(const std::vector<std::shared_ptr<compress_factory>> &factories,
                 const std::string &algorithm) {
    for (auto &factory : factories) {
        if (factory && iequals(algorithm, factory->algorithm())) {
            return factory->make_compressor();
        }
    }

    return std::unique_ptr<compress_provider>();
}

std::unique_ptr<compress_provider>
make_compressor(const std::string &algorithm) {
    return _make_compressor(g_compress_factories, algorithm);
}

static std::unique_ptr<decompress_provider>
_make_decompressor(const std::vector<std::shared_ptr<decompress_factory>> &factories,
                   const std::string &algorithm) {
    for (auto &factory : factories) {
        if (factory && iequals(algorithm, factory->algorithm())) {
            return factory->make_decompressor();
        }
    }

    return std::unique_ptr<decompress_provider>();
}

std::unique_ptr<decompress_provider>
make_decompressor(const std::string &algorithm) {
    return _make_decompressor(g_decompress_factories, algorithm);
}

const std::vector<std::shared_ptr<compress_factory>>
get_compress_factories() {
    return qb::compression::builtin::g_compress_factories;
}

std::shared_ptr<compress_factory>
get_compress_factory(const std::string &algorithm) {
    for (auto &factory : g_compress_factories) {
        if (iequals(algorithm, factory->algorithm())) {
            return factory;
        }
    }

    return std::shared_ptr<compress_factory>();
}

const std::vector<std::shared_ptr<decompress_factory>>
get_decompress_factories() {
    return qb::compression::builtin::g_decompress_factories;
}

std::shared_ptr<decompress_factory>
get_decompress_factory(const std::string &algorithm) {
    for (auto &factory : g_decompress_factories) {
        if (iequals(algorithm, factory->algorithm())) {
            return factory;
        }
    }

    return std::shared_ptr<decompress_factory>();
}

std::unique_ptr<compress_provider>
make_gzip_compressor(int compressionLevel, int method, int strategy, int memLevel) {
#if defined(QB_IO_WITH_ZLIB)
    return std::make_unique<gzip_compressor>(compressionLevel, method, strategy,
                                             memLevel);
#else  // QB_IO_WITH_ZLIB
    (void)compressionLevel;
    (void)method;
    (void)strategy;
    (void)memLevel;
    return std::unique_ptr<compress_provider>();
#endif // QB_IO_WITH_ZLIB
}

std::unique_ptr<compress_provider>
make_deflate_compressor(int compressionLevel, int method, int strategy, int memLevel) {
#if defined(QB_IO_WITH_ZLIB)
    return std::make_unique<deflate_compressor>(compressionLevel, method, strategy,
                                                memLevel);
#else  // QB_IO_WITH_ZLIB
    (void)compressionLevel;
    (void)method;
    (void)strategy;
    (void)memLevel;
    return std::unique_ptr<compress_provider>();
#endif // QB_IO_WITH_ZLIB
}
} // namespace builtin

std::shared_ptr<compress_factory>
make_compress_factory(
    const std::string &algorithm,
    std::function<std::unique_ptr<compress_provider>()> make_compressor) {
    return std::make_shared<builtin::generic_compress_factory>(algorithm,
                                                               make_compressor);
}

std::shared_ptr<decompress_factory>
make_decompress_factory(
    const std::string &algorithm, uint16_t weight,
    std::function<std::unique_ptr<decompress_provider>()> make_decompressor) {
    return std::make_shared<builtin::generic_decompress_factory>(algorithm, weight,
                                                                 make_decompressor);
}
} // namespace compression
} // namespace qb

namespace qb::compression {

template <>
size_t
compress(qb::allocator::pipe<char> &output, const char *data, std::size_t size,
         int level, int window_bits) {

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
    deflate_s.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(data));
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
        ::deflate(&deflate_s, Z_FINISH);
        size_compressed += (increase - deflate_s.avail_out);
    } while (deflate_s.avail_out == 0);

    deflateEnd(&deflate_s);
    output.free_back(output.size() - (size_compressed + out_size));
    return size_compressed;
}

template <>
size_t
uncompress(qb::allocator::pipe<char> &output, const char *data, std::size_t size,
           std::size_t max, int window_bits) {
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

    DISABLE_WARNING_PUSH
    DISABLE_WARNING_OLD_STYLE_CAST if (inflateInit2(&inflate_s, window_bits) != Z_OK) {
        throw std::runtime_error("inflate init failed");
    }
    DISABLE_WARNING_POP
    inflate_s.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(data));

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
            throw std::runtime_error("size of output string will use more memory then"
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

namespace deflate {

template <>
size_t
compress(qb::allocator::pipe<char> &output, const char *data, std::size_t size,
         int level) {
    constexpr int window_bits = 15; // bits for deflate

    return compression::compress(output, data, size, level, window_bits);
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
    constexpr int window_bits = 0; // deflate

    return compression::uncompress(output, data, size, max, window_bits);
}

std::string
uncompress(const char *data, std::size_t size) {
    std::string output;
    uncompress(output, data, size);
    return output;
}

} // namespace deflate

namespace gzip {

template <>
size_t
compress(qb::allocator::pipe<char> &output, const char *data, std::size_t size,
         int level) {
    constexpr int window_bits = 15 + 16; // gzip with windowbits of 15

    return compression::compress(output, data, size, level, window_bits);
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
    constexpr int window_bits = 15 + 32; // auto with windowbits of 15

    return compression::uncompress(output, data, size, max, window_bits);
}

std::string
uncompress(const char *data, std::size_t size) {
    std::string output;
    uncompress(output, data, size);
    return output;
}

} // namespace gzip

} // namespace qb::compression

namespace qb::allocator {

template <>
pipe<char> &
pipe<char>::put(qb::compression::deflate::to_compress &info) {
    qb::compression::deflate::compress(*this, info);
    return *this;
}

template <>
pipe<char> &
pipe<char>::put(qb::compression::deflate::to_uncompress &info) {
    qb::compression::deflate::uncompress(*this, info);
    return *this;
}

template <>
pipe<char> &
pipe<char>::put(qb::compression::gzip::to_compress &info) {
    qb::compression::gzip::compress(*this, info);
    return *this;
}

template <>
pipe<char> &
pipe<char>::put(qb::compression::gzip::to_uncompress &info) {
    qb::compression::gzip::uncompress(*this, info);
    return *this;
}

} // namespace qb::allocator