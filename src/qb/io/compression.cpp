/**
 * @file qb/io/compression.cpp
 * @brief Implementation of compression and decompression interfaces
 *
 * @details This file provides implementations for various compression algorithms
 * including GZIP and DEFLATE using zlib. It includes compressors, decompressors, and
 * factory classes for creating compression/decompression providers. The implementation
 * supports both streaming and one-shot compression operations.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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

#include <cstring> // For std::memset

#if defined(QB_HAS_COMPRESSION)
#include <zlib.h>
// zconf.h may define compress
#ifdef compress
#undef compress
#endif
#endif
#if defined(QB_HAS_COMPRESSION) && defined(QB_HAS_ZSTD)
#include <zstd.h>
#endif
#if defined(QB_HAS_COMPRESSION) && defined(QB_HAS_BROTLI)
#include <brotli/decode.h>
#include <brotli/encode.h>
#endif
#define _XPLATSTR(x) x

static bool
iequals(const std::string &a, const std::string &b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char ca, char cb) {
        // Cast to unsigned char: tolower() with a negative
        // char is UB per the C standard.
        return tolower(static_cast<unsigned char>(ca)) == tolower(static_cast<unsigned char>(cb));
    });
}

namespace qb {
namespace compression {
namespace builtin {
#if defined(QB_HAS_COMPRESSION)
// A shared base class for the gzip and deflate compressors
class zlib_compressor_base : public compress_provider {
public:
    static const std::string GZIP;
    static const std::string DEFLATE;

    zlib_compressor_base(int windowBits, int compressionLevel = Z_DEFAULT_COMPRESSION, int method = Z_DEFLATED,
                         int strategy = Z_DEFAULT_STRATEGY, int memLevel = MAX_MEM_LEVEL)
        : m_state{Z_STREAM_ERROR} // Initialize to invalid state
        , m_algorithm(windowBits >= 16 ? GZIP : DEFLATE)
        , m_initialized{false} {
        // Initialize the z_stream structure to zero before use
        std::memset(&m_stream, 0, sizeof(m_stream));

        m_state = deflateInit2(&m_stream, compressionLevel, method, windowBits, memLevel, strategy);
        if (m_state != Z_OK) {
            // Initialization failed - ensure we don't try to cleanup
            m_initialized = false;
            throw std::runtime_error("Failed to initialize zlib compressor: error " + std::to_string(m_state));
        }
        m_initialized = true;
    }

    const std::string &
    algorithm() const {
        return m_algorithm;
    }

    size_t
    compress(const uint8_t *input, size_t input_size, uint8_t *output, size_t output_size, operation_hint hint, size_t &input_bytes_processed,
             bool &done) {
        if (!m_initialized) {
            throw std::runtime_error("Compressor not properly initialized");
        }

        if (m_state == Z_STREAM_END || (hint != operation_hint::is_last && !input_size)) {
            input_bytes_processed = 0;
            done                  = (m_state == Z_STREAM_END);
            return 0;
        }

        if (m_state != Z_OK && m_state != Z_BUF_ERROR && m_state != Z_STREAM_ERROR) {
            throw std::runtime_error("Prior unrecoverable compression stream error " + std::to_string(m_state));
        }

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wtautological-constant-compare"
#endif // __clang__
        if (input_size > (std::numeric_limits<unsigned int>::max)() || output_size > (std::numeric_limits<unsigned int>::max)())
#if defined(__clang__)
#pragma clang diagnostic pop
#endif // __clang__
        {
            throw std::runtime_error("Compression input or output size out of range");
        }

        m_stream.next_in   = const_cast<uint8_t *>(input);
        m_stream.avail_in  = static_cast<unsigned int>(input_size);
        m_stream.next_out  = const_cast<uint8_t *>(output);
        m_stream.avail_out = static_cast<unsigned int>(output_size);

        m_state = ::deflate(&m_stream, (hint == operation_hint::is_last) ? Z_FINISH : Z_PARTIAL_FLUSH);
        if (m_state != Z_OK && m_state != Z_STREAM_ERROR
            && !(hint == operation_hint::is_last && (m_state == Z_STREAM_END || m_state == Z_BUF_ERROR))) {
            throw std::runtime_error("Unrecoverable compression stream error " + std::to_string(m_state));
        }

        input_bytes_processed = input_size - m_stream.avail_in;
        done                  = (m_state == Z_STREAM_END);
        return output_size - m_stream.avail_out;
    }

    void
    reset() {
        if (!m_initialized) {
            throw std::runtime_error("Compressor not properly initialized");
        }
        m_state = deflateReset(&m_stream);
        if (m_state != Z_OK) {
            throw std::runtime_error("Failed to reset zlib compressor " + std::to_string(m_state));
        }
    }

    ~zlib_compressor_base() {
        // Only call deflateEnd if initialization was successful
        // Calling deflateEnd on an uninitialized stream can cause crashes
        if (m_initialized) {
            (void) deflateEnd(&m_stream);
        }
    }

private:
    int                m_state;
    z_stream           m_stream;
    const std::string &m_algorithm;
    bool               m_initialized;
};

const std::string zlib_compressor_base::GZIP(algorithm::GZIP);
const std::string zlib_compressor_base::DEFLATE(algorithm::DEFLATE);

// A shared base class for the gzip and deflate decompressors
class zlib_decompressor_base : public decompress_provider {
public:
    zlib_decompressor_base(int windowBits)
        : m_state{Z_STREAM_ERROR}
        , m_algorithm(windowBits >= 16 ? zlib_compressor_base::GZIP : zlib_compressor_base::DEFLATE)
        , m_initialized{false} {
        // Initialize the z_stream structure to zero before use
        std::memset(&m_stream, 0, sizeof(m_stream));

        m_state = inflateInit2(&m_stream, windowBits);
        if (m_state != Z_OK) {
            m_initialized = false;
            throw std::runtime_error("Failed to initialize zlib decompressor: error " + std::to_string(m_state));
        }
        m_initialized = true;
    }

    const std::string &
    algorithm() const {
        return m_algorithm;
    }

    size_t
    decompress(const uint8_t *input, size_t input_size, uint8_t *output, size_t output_size, operation_hint hint, size_t &input_bytes_processed,
               bool &done) {
        if (!m_initialized) {
            throw std::runtime_error("Decompressor not properly initialized");
        }

        // An empty input is not "nothing to do": inflate may still hold output the previous call had no room for
        // (a match copy cut by a full buffer). Only a finished stream returns at once; an empty call with nothing
        // pending comes back from inflate as Z_BUF_ERROR, which is not an error, and produces nothing.
        if (m_state == Z_STREAM_END) {
            input_bytes_processed = 0;
            done                  = true;
            return 0;
        }

        if (m_state != Z_OK && m_state != Z_BUF_ERROR && m_state != Z_STREAM_ERROR) {
            throw std::runtime_error("Prior unrecoverable decompression stream error " + std::to_string(m_state));
        }

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wtautological-constant-compare"
#endif // __clang__
        if (input_size > (std::numeric_limits<unsigned int>::max)() || output_size > (std::numeric_limits<unsigned int>::max)())
#if defined(__clang__)
#pragma clang diagnostic pop
#endif // __clang__
        {
            throw std::runtime_error("Compression input or output size out of range");
        }

        m_stream.next_in   = const_cast<uint8_t *>(input);
        m_stream.avail_in  = static_cast<unsigned int>(input_size);
        m_stream.next_out  = const_cast<uint8_t *>(output);
        m_stream.avail_out = static_cast<unsigned int>(output_size);

        m_state = inflate(&m_stream, (hint == operation_hint::is_last) ? Z_FINISH : Z_PARTIAL_FLUSH);
        if (m_state != Z_OK && m_state != Z_STREAM_ERROR && m_state != Z_STREAM_END && m_state != Z_BUF_ERROR) {
            // Z_BUF_ERROR is a success code for Z_FINISH, and the caller can continue as
            // if operation_hint::is_last was not given
            throw std::runtime_error("Unrecoverable decompression stream error " + std::to_string(m_state));
        }

        input_bytes_processed = input_size - m_stream.avail_in;
        done                  = (m_state == Z_STREAM_END);
        return output_size - m_stream.avail_out;
    }

    void
    reset() {
        if (!m_initialized) {
            throw std::runtime_error("Decompressor not properly initialized");
        }
        m_state = inflateReset(&m_stream);
        if (m_state != Z_OK) {
            throw std::runtime_error("Failed to reset zlib decompressor " + std::to_string(m_state));
        }
    }

    ~zlib_decompressor_base() {
        // Only call inflateEnd if initialization was successful
        if (m_initialized) {
            (void) inflateEnd(&m_stream);
        }
    }

private:
    int                m_state;
    z_stream           m_stream;
    const std::string &m_algorithm;
    bool               m_initialized;
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

// zstd and brotli (Huly QB-79) honour the provider contract the zlib pair defines above: an `is_last` call finishes
// the stream and is repeated until `done`; a call without `is_last` flushes what the input produced; a decompress
// call with no input still hands back output an earlier call had no room for; a finished stream returns at once.
#if defined(QB_HAS_ZSTD)
static const std::string g_zstd_name(algorithm::ZSTD);

class zstd_compressor final : public compress_provider {
public:
    explicit zstd_compressor(int level)
        : m_ctx(ZSTD_createCCtx()) {
        if (!m_ctx)
            throw std::runtime_error("Failed to create a zstd compression context");
        const std::size_t r = ZSTD_CCtx_setParameter(m_ctx, ZSTD_c_compressionLevel, level);
        if (ZSTD_isError(r)) {
            ZSTD_freeCCtx(m_ctx);
            throw std::runtime_error(std::string("zstd refused compression level ") + std::to_string(level) + ": " + ZSTD_getErrorName(r));
        }
    }

    zstd_compressor(const zstd_compressor &)            = delete;
    zstd_compressor &operator=(const zstd_compressor &) = delete;

    ~zstd_compressor() override {
        ZSTD_freeCCtx(m_ctx);
    }

    const std::string &
    algorithm() const override {
        return g_zstd_name;
    }

    std::size_t
    compress(const uint8_t *input, std::size_t input_size, uint8_t *output, std::size_t output_size, operation_hint hint,
             std::size_t &input_bytes_processed, bool &done) override {
        if (m_done || (hint != operation_hint::is_last && !input_size)) {
            input_bytes_processed = 0;
            done                  = m_done;
            return 0;
        }
        ZSTD_inBuffer           in{input, input_size, 0};
        ZSTD_outBuffer          out{output, output_size, 0};
        const ZSTD_EndDirective mode      = (hint == operation_hint::is_last) ? ZSTD_e_end : ZSTD_e_flush;
        const std::size_t       remaining = ZSTD_compressStream2(m_ctx, &out, &in, mode);
        if (ZSTD_isError(remaining))
            throw std::runtime_error(std::string("zstd compression error: ") + ZSTD_getErrorName(remaining));
        input_bytes_processed = in.pos;
        m_done                = (mode == ZSTD_e_end && remaining == 0 && in.pos == in.size);
        done                  = m_done;
        return out.pos;
    }

    void
    reset() override {
        ZSTD_CCtx_reset(m_ctx, ZSTD_reset_session_only);
        m_done = false;
    }

private:
    ZSTD_CCtx *m_ctx;
    bool       m_done = false;
};

class zstd_decompressor final : public decompress_provider {
public:
    zstd_decompressor()
        : m_ctx(ZSTD_createDCtx()) {
        if (!m_ctx)
            throw std::runtime_error("Failed to create a zstd decompression context");
    }

    zstd_decompressor(const zstd_decompressor &)            = delete;
    zstd_decompressor &operator=(const zstd_decompressor &) = delete;

    ~zstd_decompressor() override {
        ZSTD_freeDCtx(m_ctx);
    }

    const std::string &
    algorithm() const override {
        return g_zstd_name;
    }

    std::size_t
    decompress(const uint8_t *input, std::size_t input_size, uint8_t *output, std::size_t output_size, operation_hint,
               std::size_t &input_bytes_processed, bool &done) override {
        if (m_done) {
            input_bytes_processed = 0;
            done                  = true;
            return 0;
        }
        ZSTD_inBuffer     in{input, input_size, 0};
        ZSTD_outBuffer    out{output, output_size, 0};
        const std::size_t r = ZSTD_decompressStream(m_ctx, &out, &in);
        if (ZSTD_isError(r))
            throw std::runtime_error(std::string("zstd decompression error: ") + ZSTD_getErrorName(r));
        input_bytes_processed = in.pos;
        m_done                = (r == 0); // a frame fully decoded AND flushed
        done                  = m_done;
        return out.pos;
    }

    void
    reset() override {
        ZSTD_DCtx_reset(m_ctx, ZSTD_reset_session_only);
        m_done = false;
    }

private:
    ZSTD_DCtx *m_ctx;
    bool       m_done = false;
};
#endif // QB_HAS_ZSTD

#if defined(QB_HAS_BROTLI)
static const std::string g_brotli_name(algorithm::BROTLI);

class brotli_compressor final : public compress_provider {
public:
    brotli_compressor(int quality, int window_bits)
        : m_quality(quality)
        , m_window_bits(window_bits) {
        open();
    }

    brotli_compressor(const brotli_compressor &)            = delete;
    brotli_compressor &operator=(const brotli_compressor &) = delete;

    ~brotli_compressor() override {
        BrotliEncoderDestroyInstance(m_state);
    }

    const std::string &
    algorithm() const override {
        return g_brotli_name;
    }

    std::size_t
    compress(const uint8_t *input, std::size_t input_size, uint8_t *output, std::size_t output_size, operation_hint hint,
             std::size_t &input_bytes_processed, bool &done) override {
        if (m_done || (hint != operation_hint::is_last && !input_size && !m_flushing)) {
            input_bytes_processed = 0;
            done                  = m_done;
            return 0;
        }
        // brotli refuses a change of operation, or a longer input, while a flush or the finish is still draining: a
        // flush that did not fit goes on draining -- fed only what is left of ITS input -- before anything else, and
        // once the finish has begun every call finishes.
        if (!m_started) {
            m_started = true;
#if defined(_WIN32)
            if (hint == operation_hint::is_last)
                fit_window(input_size);
#endif
        }
        BrotliEncoderOperation op   = BROTLI_OPERATION_FLUSH;
        std::size_t            feed = input_size;
        if (m_flushing)
            feed = (std::min) (input_size, m_flush_left);
        else if (m_finishing || hint == operation_hint::is_last) {
            op          = BROTLI_OPERATION_FINISH;
            m_finishing = true;
        }
        std::size_t    avail_in  = feed;
        const uint8_t *next_in   = input;
        std::size_t    avail_out = output_size;
        uint8_t       *next_out  = output;
        if (!BrotliEncoderCompressStream(m_state, op, &avail_in, &next_in, &avail_out, &next_out, nullptr))
            throw std::runtime_error("brotli compression error");
        input_bytes_processed = feed - avail_in;
        if (op == BROTLI_OPERATION_FLUSH) {
            m_flush_left = avail_in;
            m_flushing   = avail_in > 0 || BrotliEncoderHasMoreOutput(m_state);
        }
        m_done = (op == BROTLI_OPERATION_FINISH && BrotliEncoderIsFinished(m_state));
        done   = m_done;
        return output_size - avail_out;
    }

    void
    reset() override {
        // brotli has no reset: a fresh encoder with the same parameters
        BrotliEncoderDestroyInstance(m_state);
        m_state = nullptr;
        open();
        m_started    = false;
        m_done       = false;
        m_flushing   = false;
        m_finishing  = false;
        m_flush_left = 0;
    }

private:
#if defined(_WIN32)
    // A stream given whole in its first call -- a response body -- needs no window wider than itself, and at 16 bits
    // or under brotli takes a far lighter hasher. On Windows the wide hasher's allocation lands on fresh pages at every
    // stream: fitted, a 4 KiB body compressed 7 times faster (268 -> 38 us) and a 64 KiB one 20 % faster, at the same
    // ratio. glibc reuses the block, and there the light hasher is the slower one (4 KiB 30 -> 33 us, measured through
    // brotli's own API), so only Windows fits (Huly QB-93). Nothing is encoded yet, so the parameter still applies;
    // were it refused, the configured window would stay, as correct and only slower.
    void
    fit_window(std::size_t input_size) {
        int bits = BROTLI_MIN_WINDOW_BITS;
        while (bits < m_window_bits && (std::size_t{1} << bits) < input_size)
            ++bits;
        static_cast<void>(BrotliEncoderSetParameter(m_state, BROTLI_PARAM_LGWIN, static_cast<uint32_t>(bits)));
    }
#endif

    void
    open() {
        m_state = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
        if (!m_state)
            throw std::runtime_error("Failed to create a brotli encoder");
        if (!BrotliEncoderSetParameter(m_state, BROTLI_PARAM_QUALITY, static_cast<uint32_t>(m_quality))
            || !BrotliEncoderSetParameter(m_state, BROTLI_PARAM_LGWIN, static_cast<uint32_t>(m_window_bits))) {
            BrotliEncoderDestroyInstance(m_state);
            m_state = nullptr;
            throw std::runtime_error("brotli refused quality " + std::to_string(m_quality) + " / window " + std::to_string(m_window_bits));
        }
    }

    BrotliEncoderState *m_state = nullptr;
    int                 m_quality;
    int                 m_window_bits;
    bool                m_started    = false; ///< the first call has come (the window is fixed from then on)
    bool                m_done       = false;
    bool                m_flushing   = false; ///< a flush is still draining
    bool                m_finishing  = false; ///< the finish has begun
    std::size_t         m_flush_left = 0;     ///< what the draining flush has not taken of its own input
};

class brotli_decompressor final : public decompress_provider {
public:
    brotli_decompressor() {
        open();
    }

    brotli_decompressor(const brotli_decompressor &)            = delete;
    brotli_decompressor &operator=(const brotli_decompressor &) = delete;

    ~brotli_decompressor() override {
        BrotliDecoderDestroyInstance(m_state);
    }

    const std::string &
    algorithm() const override {
        return g_brotli_name;
    }

    std::size_t
    decompress(const uint8_t *input, std::size_t input_size, uint8_t *output, std::size_t output_size, operation_hint,
               std::size_t &input_bytes_processed, bool &done) override {
        if (m_done) {
            input_bytes_processed = 0;
            done                  = true;
            return 0;
        }
        std::size_t               avail_in  = input_size;
        const uint8_t            *next_in   = input;
        std::size_t               avail_out = output_size;
        uint8_t                  *next_out  = output;
        const BrotliDecoderResult r         = BrotliDecoderDecompressStream(m_state, &avail_in, &next_in, &avail_out, &next_out, nullptr);
        if (r == BROTLI_DECODER_RESULT_ERROR)
            throw std::runtime_error(std::string("brotli decompression error: ")
                                     + BrotliDecoderErrorString(BrotliDecoderGetErrorCode(m_state)));
        input_bytes_processed = input_size - avail_in;
        m_done                = (r == BROTLI_DECODER_RESULT_SUCCESS);
        done                  = m_done;
        return output_size - avail_out;
    }

    void
    reset() override {
        BrotliDecoderDestroyInstance(m_state);
        m_state = nullptr;
        open();
        m_done = false;
    }

private:
    void
    open() {
        m_state = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
        if (!m_state)
            throw std::runtime_error("Failed to create a brotli decoder");
    }

    BrotliDecoderState *m_state = nullptr;
    bool                m_done  = false;
};
#endif // QB_HAS_BROTLI

#endif // QB_HAS_COMPRESSION

// Generic internal implementation of the compress_factory API
class generic_compress_factory : public compress_factory {
public:
    ~generic_compress_factory() noexcept {}
    generic_compress_factory(const std::string &algorithm, std::function<std::unique_ptr<compress_provider>()> make_compressor)
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
    const std::string                                   m_algorithm;
    std::function<std::unique_ptr<compress_provider>()> _make_compressor;
};

// Generic internal implementation of the decompress_factory API
class generic_decompress_factory : public decompress_factory {
public:
    ~generic_decompress_factory() noexcept {}
    generic_decompress_factory(const std::string &algorithm, uint16_t weight,
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
    const std::string                                     m_algorithm;
    uint16_t                                              m_weight;
    std::function<std::unique_ptr<decompress_provider>()> _make_decompressor;
};

// "Private" algorithm-to-factory tables for namespace static helpers. The order is the server's preference where a
// client leaves the choice to it (qbm-http answers `Accept-Encoding: *` with the first): gzip and deflate first, so a
// build with the opt-in codecs (Huly QB-79) negotiates exactly what it did before unless a client asks for them.
static const std::vector<std::shared_ptr<compress_factory>> g_compress_factories = [] {
    std::vector<std::shared_ptr<compress_factory>> factories;
#if defined(QB_HAS_COMPRESSION)
    factories.push_back(std::make_shared<generic_compress_factory>(
        algorithm::GZIP, []() -> std::unique_ptr<compress_provider> { return std::make_unique<gzip_compressor>(); }));
    factories.push_back(std::make_shared<generic_compress_factory>(
        algorithm::DEFLATE, []() -> std::unique_ptr<compress_provider> { return std::make_unique<deflate_compressor>(); }));
#if defined(QB_HAS_ZSTD)
    factories.push_back(std::make_shared<generic_compress_factory>(
        algorithm::ZSTD, []() -> std::unique_ptr<compress_provider> { return std::make_unique<zstd_compressor>(3); }));
#endif
#if defined(QB_HAS_BROTLI)
    factories.push_back(std::make_shared<generic_compress_factory>(
        algorithm::BROTLI, []() -> std::unique_ptr<compress_provider> { return std::make_unique<brotli_compressor>(5, 22); }));
#endif
#endif // QB_HAS_COMPRESSION
    return factories;
}();

static const std::vector<std::shared_ptr<decompress_factory>> g_decompress_factories = [] {
    std::vector<std::shared_ptr<decompress_factory>> factories;
#if defined(QB_HAS_COMPRESSION)
    factories.push_back(std::make_shared<generic_decompress_factory>(
        algorithm::GZIP, 500, []() -> std::unique_ptr<decompress_provider> { return std::make_unique<gzip_decompressor>(); }));
    factories.push_back(std::make_shared<generic_decompress_factory>(
        algorithm::DEFLATE, 500, []() -> std::unique_ptr<decompress_provider> { return std::make_unique<deflate_decompressor>(); }));
#if defined(QB_HAS_ZSTD)
    factories.push_back(std::make_shared<generic_decompress_factory>(
        algorithm::ZSTD, 500, []() -> std::unique_ptr<decompress_provider> { return std::make_unique<zstd_decompressor>(); }));
#endif
#if defined(QB_HAS_BROTLI)
    factories.push_back(std::make_shared<generic_decompress_factory>(
        algorithm::BROTLI, 500, []() -> std::unique_ptr<decompress_provider> { return std::make_unique<brotli_decompressor>(); }));
#endif
#endif // QB_HAS_COMPRESSION
    return factories;
}();

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
_make_compressor(const std::vector<std::shared_ptr<compress_factory>> &factories, const std::string &algorithm) {
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
_make_decompressor(const std::vector<std::shared_ptr<decompress_factory>> &factories, const std::string &algorithm) {
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
#if defined(QB_HAS_COMPRESSION)
    return std::make_unique<gzip_compressor>(compressionLevel, method, strategy, memLevel);
#else  // QB_HAS_COMPRESSION
    (void) compressionLevel;
    (void) method;
    (void) strategy;
    (void) memLevel;
    return std::unique_ptr<compress_provider>();
#endif // QB_HAS_COMPRESSION
}

std::unique_ptr<compress_provider>
make_deflate_compressor(int compressionLevel, int method, int strategy, int memLevel) {
#if defined(QB_HAS_COMPRESSION)
    return std::make_unique<deflate_compressor>(compressionLevel, method, strategy, memLevel);
#else  // QB_HAS_COMPRESSION
    (void) compressionLevel;
    (void) method;
    (void) strategy;
    (void) memLevel;
    return std::unique_ptr<compress_provider>();
#endif // QB_HAS_COMPRESSION
}

std::unique_ptr<compress_provider>
make_zstd_compressor(int level) {
#if defined(QB_HAS_COMPRESSION) && defined(QB_HAS_ZSTD)
    return std::make_unique<zstd_compressor>(level);
#else
    (void) level;
    return std::unique_ptr<compress_provider>();
#endif
}

std::unique_ptr<compress_provider>
make_brotli_compressor(int quality, int window_bits) {
#if defined(QB_HAS_COMPRESSION) && defined(QB_HAS_BROTLI)
    return std::make_unique<brotli_compressor>(quality, window_bits);
#else
    (void) quality;
    (void) window_bits;
    return std::unique_ptr<compress_provider>();
#endif
}
} // namespace builtin

std::shared_ptr<compress_factory>
make_compress_factory(const std::string &algorithm, std::function<std::unique_ptr<compress_provider>()> make_compressor) {
    return std::make_shared<builtin::generic_compress_factory>(algorithm, make_compressor);
}

std::shared_ptr<decompress_factory>
make_decompress_factory(const std::string &algorithm, uint16_t weight,
                        std::function<std::unique_ptr<decompress_provider>()> make_decompressor) {
    return std::make_shared<builtin::generic_decompress_factory>(algorithm, weight, make_decompressor);
}
} // namespace compression
} // namespace qb

namespace qb::compression {

template <>
size_t
compress(qb::allocator::pipe<char> &output, const char *data, std::size_t size, int level, int window_bits) {
#ifdef DEBUG
    // Verify if size input will fit into unsigned int, type used for zlib's avail_in
    if (size > std::numeric_limits<unsigned int>::max()) {
        throw std::runtime_error("size arg is too large to fit into unsigned int type");
    }
#endif

    z_stream deflate_s;
    deflate_s.zalloc   = Z_NULL;
    deflate_s.zfree    = Z_NULL;
    deflate_s.opaque   = Z_NULL;
    deflate_s.avail_in = 0;
    deflate_s.next_in  = Z_NULL;

    constexpr int mem_level = 8;
    // The memory requirements for deflate are (in bytes):
    // (1 << (window_bits+2)) +  (1 << (mem_level+9))
    // with a default value of 8 for mem_level and our window_bits of 15
    // this is 128Kb

    DISABLE_WARNING_PUSH
    DISABLE_WARNING_OLD_STYLE_CAST
    if (deflateInit2(&deflate_s, level, Z_DEFLATED, window_bits, mem_level, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("deflate init failed");
    }
    DISABLE_WARNING_POP

    const std::size_t out_size = output.size();
    deflate_s.next_in          = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(data));
    deflate_s.avail_in         = static_cast<unsigned int>(size);

    std::size_t size_compressed = 0;
    do {
        size_t increase = size / 2 + 1024;
        if ((output.size() - out_size) < (size_compressed + increase)) {
            output.allocate_back(increase);
        }
        // There is no way we see that "increase" would not fit in an unsigned int,
        // hence we use static cast here to avoid -Wshorten-64-to-32 error
        deflate_s.avail_out = static_cast<unsigned int>(increase);
        deflate_s.next_out  = reinterpret_cast<Bytef *>((output.begin() + out_size + size_compressed));
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
uncompress(qb::allocator::pipe<char> &output, const char *data, std::size_t size, std::size_t max, int window_bits) {
    // Empty input decompresses to nothing. Return early: with size == 0 the
    // decode loop below computes chunk = 2*size = 0, so avail_out stays 0 every
    // iteration and `while (inflate_s.avail_out == 0)` never terminates (hang).
    if (size == 0)
        return 0;

    z_stream inflate_s;

    inflate_s.zalloc   = Z_NULL;
    inflate_s.zfree    = Z_NULL;
    inflate_s.opaque   = Z_NULL;
    inflate_s.avail_in = 0;
    inflate_s.next_in  = Z_NULL;

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
        throw std::runtime_error("size arg is too large to fit into unsigned int type x2");
    }
#endif
    // Overflow-safe budget check (2 * size without wrapping size_t).
    if (max && size > max / 2) {
        inflateEnd(&inflate_s);
        throw std::runtime_error("size may use more memory than intended when decompressing");
    }
    inflate_s.avail_in                  = static_cast<unsigned int>(size);
    const std::size_t out_size          = output.size();
    std::size_t       size_uncompressed = 0;
    int               ret               = Z_OK;
    do {
        // chunk = 2*size; check size_uncompressed + chunk against the budget
        // overflow-safe (decompression-bomb guard).
        const std::size_t chunk = 2 * size;
        if (max && (size_uncompressed > max || chunk > max - size_uncompressed)) {
            inflateEnd(&inflate_s);
            throw std::runtime_error("size of output string will use more memory then"
                                     "intended when decompressing");
        }
        output.allocate_back(chunk);
        inflate_s.avail_out = static_cast<unsigned int>(chunk);
        inflate_s.next_out  = reinterpret_cast<Bytef *>(output.begin() + out_size + size_uncompressed);
        ret                 = inflate(&inflate_s, Z_FINISH);
        if (ret != Z_STREAM_END && ret != Z_OK && ret != Z_BUF_ERROR) {
            // inflate_s.msg may be null; never construct std::string from nullptr.
            std::string error_msg = inflate_s.msg ? inflate_s.msg : "inflate error";
            inflateEnd(&inflate_s);
            throw std::runtime_error(error_msg);
        }

        size_uncompressed += (chunk - inflate_s.avail_out);
    } while (inflate_s.avail_out == 0);
    inflateEnd(&inflate_s);
    // Reject truncated/incomplete streams instead of returning partial output.
    if (ret != Z_STREAM_END) {
        throw std::runtime_error("incomplete or truncated compressed stream");
    }
    output.free_back(output.size() - (size_uncompressed + out_size));
    return size_uncompressed;
}

namespace deflate {

template <>
size_t
compress(qb::allocator::pipe<char> &output, const char *data, std::size_t size, int level) {
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
uncompress(qb::allocator::pipe<char> &output, const char *data, std::size_t size, std::size_t max) {
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
compress(qb::allocator::pipe<char> &output, const char *data, std::size_t size, int level) {
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
uncompress(qb::allocator::pipe<char> &output, const char *data, std::size_t size, std::size_t max) {
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