/**
 * @file qb/io/compression.h
 * @brief Data compression utilities for the QB IO library
 * 
 * This file provides data compression and decompression functionality using
 * the zlib library. It includes support for both deflate and gzip algorithms,
 * with comprehensive interfaces for encoding and decoding data streams.
 * 
 * The implementation includes templated functions for flexible buffer management,
 * provider classes for algorithm abstraction, and utility functions for common
 * compression operations.
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

#include <limits>
#include <stdexcept>
#include <string>
#include <functional>

#ifndef QB_IO_COMPRESSION_H
#    define QB_IO_COMPRESSION_H

#    ifndef QB_IO_WITH_ZLIB
#        error "missing Z Library"
#    endif

#    include "qb/system/allocator/pipe.h"
#    include <zlib.h>

namespace qb {
namespace compression {

/**
 * @enum operation_hint
 * @brief Hints for compression/decompression operations
 * 
 * These hints indicate whether an operation is the last in a sequence or whether
 * there are more operations to follow. They help the compression algorithm
 * make optimal decisions about buffer management and flushing.
 */
enum operation_hint {
    is_last, /**< Used for the expected last compress() call, or for an expected single decompress() call */
    has_more /**< Used when further compress() calls will be made, or when multiple decompress() calls may be required */
};

/**
 * @struct operation_result
 * @brief Result of a compression/decompression operation
 * 
 * This structure contains information about the outcome of a compression or
 * decompression operation, including how many bytes were processed and produced,
 * and whether the operation is complete.
 */
struct operation_result {
    std::size_t input_bytes_processed; /**< Number of bytes processed from the input buffer */
    std::size_t output_bytes_produced; /**< Number of bytes written to the output buffer */
    bool done; /**< For compress, set when 'last' is true and there was enough space to
               complete compression; for decompress, set if the end of the
               decompression stream has been reached */
};

/**
 * @class compress_provider
 * @brief Abstract interface for compression algorithm providers
 * 
 * This class defines the interface that all compression algorithm implementations
 * must implement. It provides methods for compression operations and algorithm
 * identification.
 */
class compress_provider {
public:
    /**
     * @brief Get the name of the compression algorithm
     * @return Reference to a string containing the algorithm name
     */
    virtual const std::string &algorithm() const = 0;
    
    /**
     * @brief Compress a block of data
     * 
     * @param input Pointer to the input data to compress
     * @param input_size Size of the input data in bytes
     * @param output Pointer to the output buffer where compressed data will be written
     * @param output_size Size of the output buffer in bytes
     * @param hint Hint about whether this is the last block or more blocks will follow
     * @param input_bytes_processed [out] Number of input bytes actually processed
     * @param done [out] Flag indicating whether compression is complete
     * @return Number of bytes written to the output buffer
     */
    virtual std::size_t compress(const uint8_t *input, std::size_t input_size,
                                 uint8_t *output, std::size_t output_size,
                                 operation_hint hint, std::size_t &input_bytes_processed,
                                 bool &done) = 0;
    
    /**
     * @brief Reset the compressor to its initial state
     * 
     * This method should reset any internal state of the compressor,
     * allowing it to be reused for a new compression stream.
     */
    virtual void reset() = 0;
    
    /**
     * @brief Virtual destructor
     */
    virtual ~compress_provider() = default;
};

/**
 * @class decompress_provider
 * @brief Abstract interface for decompression algorithm providers
 * 
 * This class defines the interface that all decompression algorithm implementations
 * must implement. It provides methods for decompression operations and algorithm
 * identification.
 */
class decompress_provider {
public:
    /**
     * @brief Get the name of the decompression algorithm
     * @return Reference to a string containing the algorithm name
     */
    virtual const std::string &algorithm() const = 0;
    
    /**
     * @brief Decompress a block of data
     * 
     * @param input Pointer to the compressed input data
     * @param input_size Size of the input data in bytes
     * @param output Pointer to the output buffer where decompressed data will be written
     * @param output_size Size of the output buffer in bytes
     * @param hint Hint about whether this is the last block or more blocks will follow
     * @param input_bytes_processed [out] Number of input bytes actually processed
     * @param done [out] Flag indicating whether decompression is complete
     * @return Number of bytes written to the output buffer
     */
    virtual std::size_t decompress(const uint8_t *input, std::size_t input_size,
                                   uint8_t *output, std::size_t output_size,
                                   operation_hint hint,
                                   std::size_t &input_bytes_processed, bool &done) = 0;
    
    /**
     * @brief Reset the decompressor to its initial state
     * 
     * This method should reset any internal state of the decompressor,
     * allowing it to be reused for a new decompression stream.
     */
    virtual void reset() = 0;
    
    /**
     * @brief Virtual destructor
     */
    virtual ~decompress_provider() = default;
};

/**
 * @class compress_factory
 * @brief Factory interface for creating compression providers
 * 
 * This class defines the interface for factories that create compressor
 * instances. It allows the creation of compressors to be abstracted
 * from their implementation and configuration.
 */
class compress_factory {
public:
    /**
     * @brief Get the name of the compression algorithm
     * @return Reference to a string containing the algorithm name
     */
    virtual const std::string &algorithm() const = 0;
    
    /**
     * @brief Create a new compressor instance
     * @return Unique pointer to a newly created compressor
     */
    virtual std::unique_ptr<compress_provider> make_compressor() const = 0;
    
    /**
     * @brief Virtual destructor
     */
    virtual ~compress_factory() = default;
};

/**
 * @class decompress_factory
 * @brief Factory interface for creating decompression providers
 * 
 * This class defines the interface for factories that create decompressor
 * instances. It allows the creation of decompressors to be abstracted
 * from their implementation and configuration.
 */
class decompress_factory {
public:
    /**
     * @brief Get the name of the decompression algorithm
     * @return Reference to a string containing the algorithm name
     */
    virtual const std::string &algorithm() const = 0;
    
    /**
     * @brief Get the weight of this decompression algorithm
     * 
     * The weight is used to prioritize decompression algorithms when
     * multiple algorithms are available for a given input.
     * 
     * @return Weight value (higher values indicate higher priority)
     */
    virtual uint16_t weight() const = 0;
    
    /**
     * @brief Create a new decompressor instance
     * @return Unique pointer to a newly created decompressor
     */
    virtual std::unique_ptr<decompress_provider> make_decompressor() const = 0;
    
    /**
     * @brief Virtual destructor
     */
    virtual ~decompress_factory() = default;
};

/**
 * @namespace builtin
 * @brief Namespace containing built-in compression implementations
 * 
 * This namespace contains pre-configured compression and decompression
 * implementations that are built into the library, including support for
 * common algorithms like gzip and deflate.
 */
namespace builtin {

/**
 * @brief Check if compression support is available
 * @return true if compression is supported, false otherwise
 */
bool supported();

/**
 * @namespace algorithm
 * @brief Namespace containing algorithm constants and utilities
 */
namespace algorithm {

/**
 * @brief Identifier for the gzip compression algorithm
 */
constexpr const char *const GZIP = "gzip";

/**
 * @brief Identifier for the deflate compression algorithm
 */
constexpr const char *const DEFLATE = "deflate";

/**
 * @brief Check if a specific compression algorithm is supported
 * @param algorithm Name of the algorithm to check
 * @return true if the algorithm is supported, false otherwise
 */
bool supported(const std::string &algorithm);
} // namespace algorithm

/**
 * @brief Create a compressor for the specified algorithm
 * @param algorithm Name of the compression algorithm
 * @return Unique pointer to a compressor provider, or nullptr if the algorithm is not supported
 */
std::unique_ptr<compress_provider> make_compressor(const std::string &algorithm);

/**
 * @brief Create a decompressor for the specified algorithm
 * @param algorithm Name of the decompression algorithm
 * @return Unique pointer to a decompressor provider, or nullptr if the algorithm is not supported
 */
std::unique_ptr<decompress_provider> make_decompressor(const std::string &algorithm);

/**
 * @brief Get all available compression factories
 * @return Vector of shared pointers to compression factories
 */
const std::vector<std::shared_ptr<compress_factory>> get_compress_factories();

/**
 * @brief Get a specific compression factory by algorithm name
 * @param algorithm Name of the compression algorithm
 * @return Shared pointer to the compression factory, or nullptr if not found
 */
std::shared_ptr<compress_factory> get_compress_factory(const std::string &algorithm);

/**
 * @brief Get all available decompression factories
 * @return Vector of shared pointers to decompression factories
 */
const std::vector<std::shared_ptr<decompress_factory>> get_decompress_factories();

/**
 * @brief Get a specific decompression factory by algorithm name
 * @param algorithm Name of the decompression algorithm
 * @return Shared pointer to the decompression factory, or nullptr if not found
 */
std::shared_ptr<decompress_factory> get_decompress_factory(const std::string &algorithm);

/**
 * @brief Create a gzip compressor with custom parameters
 * 
 * @param compressionLevel Compression level (1-9, where 9 is max compression)
 * @param method Compression method (usually Z_DEFLATED)
 * @param strategy Compression strategy (e.g., Z_DEFAULT_STRATEGY)
 * @param memLevel Memory usage level (1-9, where 9 uses most memory)
 * @return Unique pointer to a gzip compressor provider
 */
std::unique_ptr<compress_provider> make_gzip_compressor(int compressionLevel, int method,
                                                        int strategy, int memLevel);

/**
 * @brief Create a deflate compressor with custom parameters
 * 
 * @param compressionLevel Compression level (1-9, where 9 is max compression)
 * @param method Compression method (usually Z_DEFLATED)
 * @param strategy Compression strategy (e.g., Z_DEFAULT_STRATEGY)
 * @param memLevel Memory usage level (1-9, where 9 uses most memory)
 * @return Unique pointer to a deflate compressor provider
 */
std::unique_ptr<compress_provider>
make_deflate_compressor(int compressionLevel, int method, int strategy, int memLevel);

} // namespace builtin

/**
 * @brief Create a custom compression factory
 * 
 * @param algorithm Name of the compression algorithm
 * @param make_compressor Function that creates compressor instances
 * @return Shared pointer to the created compression factory
 */
std::shared_ptr<compress_factory> make_compress_factory(
    const std::string &algorithm,
    std::function<std::unique_ptr<compress_provider>()> make_compressor);

/**
 * @brief Create a custom decompression factory
 * 
 * @param algorithm Name of the decompression algorithm
 * @param weight Priority weight for the algorithm
 * @param make_decompressor Function that creates decompressor instances
 * @return Shared pointer to the created decompression factory
 */
std::shared_ptr<decompress_factory> make_decompress_factory(
    const std::string &algorithm, uint16_t weight,
    std::function<std::unique_ptr<decompress_provider>()> make_decompressor);

/**
 * @brief Compress data using a generic output container
 * 
 * This template function compresses the provided data using the zlib
 * library and stores the result in an output container that supports
 * resize() and size() operations.
 * 
 * @tparam Output Type of the output container
 * @param output Container for the compressed data (will be resized as needed)
 * @param data Pointer to the data to compress
 * @param size Size of the data in bytes
 * @param level Compression level (1-9, where 9 is max compression)
 * @param window_bits Window size bits with encoding format flag
 * @return Size of the compressed data in bytes
 * @throws std::runtime_error If initialization fails or input is too large
 */
template <typename Output>
size_t
compress(Output &output, const char *data, std::size_t size, int level,
         int window_bits) {

#    ifdef DEBUG
    // Verify if size input will fit into unsigned int, type used for zlib's avail_in
    if (size > std::numeric_limits<unsigned int>::max()) {
        throw std::runtime_error("size arg is too large to fit into unsigned int type");
    }
#    endif

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
    // (8 to 15) + 32 to automatically detect gzip/zlib header
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

    deflate_s.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(data));
    deflate_s.avail_in = static_cast<unsigned int>(size);

    std::size_t size_compressed = 0;
    do {
        size_t increase = size / 2 + 1024;
        if (output.size() < (size_compressed + increase)) {
            output.resize(size_compressed + increase);
        }
        // There is no way we see that "increase" would not fit in an unsigned int,
        // hence we use static cast here to avoid -Wshorten-64-to-32 error
        deflate_s.avail_out = static_cast<unsigned int>(increase);
        deflate_s.next_out = reinterpret_cast<Bytef *>((&output[0] + size_compressed));
        // From http://www.zlib.net/zlib_how.html
        // "deflate() has a return value that can indicate errors, yet we do not check it
        // here. Why not? Well, it turns out that deflate() can do no wrong here."
        // Basically only possible error is from deflateInit not working properly
        ::deflate(&deflate_s, Z_FINISH);
        size_compressed += (increase - deflate_s.avail_out);
    } while (deflate_s.avail_out == 0);

    deflateEnd(&deflate_s);
    output.resize(size_compressed);
    return size_compressed;
}

/**
 * @brief Specialization of compress for pipe allocator
 * 
 * @param output Pipe allocator for the compressed data
 * @param data Pointer to the data to compress
 * @param size Size of the data in bytes
 * @param level Compression level (1-9, where 9 is max compression)
 * @param window_bits Window size bits with encoding format flag
 * @return Size of the compressed data in bytes
 */
template <>
size_t compress(qb::allocator::pipe<char> &output, const char *data, std::size_t size,
                int level, int window_bits);

/**
 * @brief Uncompress data using a generic output container
 * 
 * This template function uncompresses the provided data using the zlib
 * library and stores the result in an output container that supports
 * resize() and size() operations.
 * 
 * @tparam Output Type of the output container
 * @param output Container for the uncompressed data (will be resized as needed)
 * @param data Pointer to the compressed data
 * @param size Size of the compressed data in bytes
 * @param max Maximum allowed output size (0 for unlimited)
 * @param window_bits Window size bits with encoding format flag
 * @return Size of the uncompressed data in bytes
 * @throws std::runtime_error If initialization fails, input is too large, or max size exceeded
 */
template <typename Output>
std::size_t
uncompress(Output &output, const char *data, std::size_t size, std::size_t max,
           int window_bits) {
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
    DISABLE_WARNING_OLD_STYLE_CAST
    if (inflateInit2(&inflate_s, window_bits) != Z_OK) {
        throw std::runtime_error("inflate init failed");
    }
    DISABLE_WARNING_POP
    inflate_s.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(data));

#    ifdef DEBUG
    // Verify if size (long type) input will fit into unsigned int, type used for zlib's
    // avail_in
    std::uint64_t size_64 = size * 2;
    if (size_64 > std::numeric_limits<unsigned int>::max()) {
        inflateEnd(&inflate_s);
        throw std::runtime_error(
            "size arg is too large to fit into unsigned int type x2");
    }
#    endif
    if (max && (size > max || (size * 2) > max)) {
        inflateEnd(&inflate_s);
        throw std::runtime_error(
            "size may use more memory than intended when decompressing");
    }
    inflate_s.avail_in = static_cast<unsigned int>(size);
    std::size_t size_uncompressed = 0;
    do {
        std::size_t resize_to = size_uncompressed + 2 * size;
        if (max && resize_to > max) {
            inflateEnd(&inflate_s);
            throw std::runtime_error("size of output string will use more memory then "
                                     "intended when decompressing");
        }
        output.resize(resize_to);
        inflate_s.avail_out = static_cast<unsigned int>(2 * size);
        inflate_s.next_out = reinterpret_cast<Bytef *>(&output[0] + size_uncompressed);
        int ret = inflate(&inflate_s, Z_FINISH);
        if (ret != Z_STREAM_END && ret != Z_OK && ret != Z_BUF_ERROR) {
            std::string error_msg = inflate_s.msg;
            inflateEnd(&inflate_s);
            throw std::runtime_error(error_msg);
        }

        size_uncompressed += (2 * size - inflate_s.avail_out);
    } while (inflate_s.avail_out == 0);
    inflateEnd(&inflate_s);
    output.resize(size_uncompressed);
    return size_uncompressed;
}

/**
 * @brief Specialization of uncompress for pipe allocator
 * 
 * @param output Pipe allocator for the uncompressed data
 * @param data Pointer to the compressed data
 * @param size Size of the compressed data in bytes
 * @param max Maximum allowed output size (0 for unlimited)
 * @param window_bits Window size bits with encoding format flag
 * @return Size of the uncompressed data in bytes
 */
template <>
size_t uncompress(qb::allocator::pipe<char> &output, const char *data, std::size_t size,
                  std::size_t max, int window_bits);

/**
 * @namespace deflate
 * @brief Namespace containing deflate compression utilities
 * 
 * This namespace provides functions and types for performing deflate
 * compression and decompression operations, which is a common algorithm
 * used in ZIP files and HTTP compression.
 */
namespace deflate {

/**
 * @brief Compress data using deflate algorithm with a generic output container
 * 
 * @tparam Output Type of the output container
 * @param output Container for the compressed data (will be resized as needed)
 * @param data Pointer to the data to compress
 * @param size Size of the data in bytes
 * @param level Compression level (1-9, where 9 is max compression)
 * @return Size of the compressed data in bytes
 */
template <typename Output>
size_t
compress(Output &output, const char *data, std::size_t size,
         int level = Z_DEFAULT_COMPRESSION) {
    constexpr int window_bits = 15; // gzip with windowbits of 15
    return compression::compress(output, data, size, level, window_bits);
}

/**
 * @brief Specialization of deflate compress for pipe allocator
 */
template <>
size_t compress(qb::allocator::pipe<char> &output, const char *data, std::size_t size,
                int level);

/**
 * @struct to_compress
 * @brief Structure for passing compression parameters
 * 
 * This structure encapsulates the parameters for a compression operation
 * and provides a place to store the result.
 */
struct to_compress {
    const char *data;         /**< Pointer to the data to compress */
    std::size_t size;         /**< Size of the data in bytes */
    int level = Z_DEFAULT_COMPRESSION; /**< Compression level */
    size_t size_compressed;   /**< [out] Size of the compressed data */
};

/**
 * @brief Compress data using the parameters in a to_compress structure
 * 
 * @tparam Output Type of the output container
 * @param output Container for the compressed data
 * @param info Structure containing compression parameters and results
 * @return Reference to the output container
 */
template <typename Output>
Output &
compress(Output &output, to_compress &info) {
    info.size_compressed = compress<Output>(output, info.data, info.size, info.level);
    return output;
}

/**
 * @brief Compress data to a string
 * 
 * @param data Pointer to the data to compress
 * @param size Size of the data in bytes
 * @param level Compression level (1-9, where 9 is max compression)
 * @return String containing the compressed data
 */
std::string compress(const char *data, std::size_t size,
                     int level = Z_DEFAULT_COMPRESSION);

/**
 * @brief Uncompress data using deflate algorithm with a generic output container
 * 
 * @tparam Output Type of the output container
 * @param output Container for the uncompressed data
 * @param data Pointer to the compressed data
 * @param size Size of the compressed data in bytes
 * @param max Maximum allowed output size (0 for unlimited)
 * @return Size of the uncompressed data in bytes
 */
template <typename Output>
std::size_t
uncompress(Output &output, const char *data, std::size_t size, std::size_t max = 0) {
    constexpr int window_bits = 0; // deflate

    return compression::uncompress(output, data, size, max, window_bits);
}

/**
 * @brief Specialization of deflate uncompress for pipe allocator
 */
template <>
size_t uncompress(qb::allocator::pipe<char> &output, const char *data, std::size_t size,
                  std::size_t level);

/**
 * @struct to_uncompress
 * @brief Structure for passing decompression parameters
 * 
 * This structure encapsulates the parameters for a decompression operation
 * and provides a place to store the result.
 */
struct to_uncompress {
    const char *data;         /**< Pointer to the compressed data */
    std::size_t size;         /**< Size of the compressed data in bytes */
    std::size_t max = 0;      /**< Maximum allowed output size (0 for unlimited) */
    std::size_t size_uncompressed; /**< [out] Size of the uncompressed data */
};

/**
 * @brief Uncompress data using the parameters in a to_uncompress structure
 * 
 * @tparam Output Type of the output container
 * @param output Container for the uncompressed data
 * @param info Structure containing decompression parameters and results
 * @return Reference to the output container
 */
template <typename Output>
Output &
uncompress(Output &output, to_uncompress &info) {
    info.size_uncompressed = uncompress<Output>(output, info.data, info.size, info.max);
    return output;
}

/**
 * @brief Uncompress data to a string
 * 
 * @param data Pointer to the compressed data
 * @param size Size of the compressed data in bytes
 * @return String containing the uncompressed data
 */
std::string uncompress(const char *data, std::size_t size);
} // namespace deflate

/**
 * @namespace gzip
 * @brief Namespace containing gzip compression utilities
 * 
 * This namespace provides functions and types for performing gzip
 * compression and decompression operations, which is a common algorithm
 * used in web content and file compression.
 */
namespace gzip {
/**
 * @brief Check if data is compressed using gzip or zlib format
 * 
 * Examines the data header to determine if it's in a recognized compressed format.
 * 
 * @param data Pointer to the data to check
 * @param size Size of the data in bytes
 * @return true if the data appears to be in gzip or zlib format, false otherwise
 */
inline bool
is_compressed(const char *data, std::size_t size) {
    return size > 2 && (
                           // zlib
                           (static_cast<uint8_t>(data[0]) == 0x78 &&
                            (static_cast<uint8_t>(data[1]) == 0x9C ||
                             static_cast<uint8_t>(data[1]) == 0x01 ||
                             static_cast<uint8_t>(data[1]) == 0xDA ||
                             static_cast<uint8_t>(data[1]) == 0x5E)) ||
                           // gzip
                           (static_cast<uint8_t>(data[0]) == 0x1F &&
                            static_cast<uint8_t>(data[1]) == 0x8B));
}

/**
 * @brief Compress data using gzip algorithm with a generic output container
 * 
 * @tparam Output Type of the output container
 * @param output Container for the compressed data (will be resized as needed)
 * @param data Pointer to the data to compress
 * @param size Size of the data in bytes
 * @param level Compression level (1-9, where 9 is max compression)
 * @return Size of the compressed data in bytes
 */
template <typename Output>
size_t
compress(Output &output, const char *data, std::size_t size,
         int level = Z_DEFAULT_COMPRESSION) {
    constexpr int window_bits = 15 + 16; // gzip with windowbits of 15
    return compression::compress(output, data, size, level, window_bits);
}

/**
 * @brief Specialization of gzip compress for pipe allocator
 */
template <>
size_t compress(qb::allocator::pipe<char> &output, const char *data, std::size_t size,
                int level);

/**
 * @struct to_compress
 * @brief Structure for passing gzip compression parameters
 * 
 * This structure encapsulates the parameters for a compression operation
 * and provides a place to store the result.
 */
struct to_compress {
    const char *data;         /**< Pointer to the data to compress */
    std::size_t size;         /**< Size of the data in bytes */
    int level = Z_DEFAULT_COMPRESSION; /**< Compression level */
    size_t size_compressed;   /**< [out] Size of the compressed data */
};

/**
 * @brief Compress data using the parameters in a to_compress structure
 * 
 * @tparam Output Type of the output container
 * @param output Container for the compressed data
 * @param info Structure containing compression parameters and results
 * @return Reference to the output container
 */
template <typename Output>
Output &
compress(Output &output, to_compress &info) {
    info.size_compressed = compress<Output>(output, info.data, info.size, info.level);
    return output;
}

/**
 * @brief Compress data to a string using gzip
 * 
 * @param data Pointer to the data to compress
 * @param size Size of the data in bytes
 * @param level Compression level (1-9, where 9 is max compression)
 * @return String containing the compressed data
 */
std::string compress(const char *data, std::size_t size,
                     int level = Z_DEFAULT_COMPRESSION);

/**
 * @brief Uncompress data using gzip algorithm with a generic output container
 * 
 * @tparam Output Type of the output container
 * @param output Container for the uncompressed data
 * @param data Pointer to the compressed data
 * @param size Size of the compressed data in bytes
 * @param max Maximum allowed output size (0 for unlimited)
 * @return Size of the uncompressed data in bytes
 */
template <typename Output>
std::size_t
uncompress(Output &output, const char *data, std::size_t size, std::size_t max = 0) {
    constexpr int window_bits = 15 + 32; // auto with windowbits of 15

    return compression::uncompress(output, data, size, max, window_bits);
}

/**
 * @brief Specialization of gzip uncompress for pipe allocator
 */
template <>
size_t uncompress(qb::allocator::pipe<char> &output, const char *data, std::size_t size,
                  std::size_t level);

/**
 * @struct to_uncompress
 * @brief Structure for passing gzip decompression parameters
 * 
 * This structure encapsulates the parameters for a decompression operation
 * and provides a place to store the result.
 */
struct to_uncompress {
    const char *data;         /**< Pointer to the compressed data */
    std::size_t size;         /**< Size of the compressed data in bytes */
    std::size_t max = 0;      /**< Maximum allowed output size (0 for unlimited) */
    std::size_t size_uncompressed; /**< [out] Size of the uncompressed data */
};

/**
 * @brief Uncompress data using the parameters in a to_uncompress structure
 * 
 * @tparam Output Type of the output container
 * @param output Container for the uncompressed data
 * @param info Structure containing decompression parameters and results
 * @return Reference to the output container
 */
template <typename Output>
Output &
uncompress(Output &output, to_uncompress &info) {
    info.size_uncompressed = uncompress<Output>(output, info.data, info.size, info.max);
    return output;
}

/**
 * @brief Uncompress gzip data to a string
 * 
 * @param data Pointer to the compressed data
 * @param size Size of the compressed data in bytes
 * @return String containing the uncompressed data
 */
std::string uncompress(const char *data, std::size_t size);
} // namespace gzip

} // namespace compression
} // namespace qb

namespace qb::allocator {

/**
 * @brief Template specialization for putting compressed data into a character pipe
 * 
 * This specialization allows direct use of deflate compression within a pipe's put operation.
 * 
 * @param info Compression parameters and results structure
 * @return Reference to this pipe for method chaining
 */
template <>
pipe<char> &pipe<char>::put(qb::compression::deflate::to_compress &);

/**
 * @brief Template specialization for putting decompressed data into a character pipe
 * 
 * This specialization allows direct use of deflate decompression within a pipe's put operation.
 * 
 * @param info Decompression parameters and results structure
 * @return Reference to this pipe for method chaining
 */
template <>
pipe<char> &pipe<char>::put(qb::compression::deflate::to_uncompress &);

/**
 * @brief Template specialization for putting gzip compressed data into a character pipe
 * 
 * This specialization allows direct use of gzip compression within a pipe's put operation.
 * 
 * @param info Compression parameters and results structure
 * @return Reference to this pipe for method chaining
 */
template <>
pipe<char> &pipe<char>::put(qb::compression::gzip::to_compress &);

/**
 * @brief Template specialization for putting gzip decompressed data into a character pipe
 * 
 * This specialization allows direct use of gzip decompression within a pipe's put operation.
 * 
 * @param info Decompression parameters and results structure
 * @return Reference to this pipe for method chaining
 */
template <>
pipe<char> &pipe<char>::put(qb::compression::gzip::to_uncompress &);

} // namespace qb::allocator

/**
 * @namespace qb::gzip
 * @brief Namespace alias for gzip compression utilities
 * 
 * This namespace provides a convenient alias to access gzip compression
 * and decompression functions directly from the qb namespace.
 */
namespace qb::gzip {
using namespace qb::compression::gzip;
}

/**
 * @namespace qb::deflate
 * @brief Namespace alias for deflate compression utilities
 * 
 * This namespace provides a convenient alias to access deflate compression
 * and decompression functions directly from the qb namespace.
 */
namespace qb::deflate {
using namespace qb::compression::deflate;
}

#endif // QB_IO_COMPRESSION_H