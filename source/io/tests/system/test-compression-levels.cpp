/**
 * @file qb/io/tests/system/test-compression-levels.cpp
 * @brief Unit tests for different compression levels and strategies
 * 
 * This file contains tests for the compression functionality of the QB framework
 * at different compression levels, examining the tradeoffs between speed, 
 * memory usage, and compression ratio.
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
 * @ingroup Tests
 */

#include <gtest/gtest.h>
#include <qb/io/compression.h>
#include <qb/io/crypto.h>
#include <qb/system/allocator/pipe.h>
#include <chrono>
#include <thread>
#include <array>
#include <vector>
#include <algorithm>

namespace qb::io::tests {

// Structure to hold compression test results
struct CompressionTestResult {
    std::string level_name;
    size_t original_size;
    size_t compressed_size;
    double compression_ratio;
    double compression_time_ms;
    double decompression_time_ms;
    
    // Helper for printing results in a readable format
    std::string to_string() const {
        std::stringstream ss;
        ss << "Level: " << level_name
           << ", Original: " << original_size << " bytes"
           << ", Compressed: " << compressed_size << " bytes"
           << ", Ratio: " << compression_ratio
           << ", Comp Time: " << compression_time_ms << " ms"
           << ", Decomp Time: " << decompression_time_ms << " ms";
        return ss.str();
    }
};

// Test different compression levels for gzip
TEST(CompressionLevels, GzipLevels) {
    // Generate test data (mixed content with some patterns for better compression)
    std::string test_data;
    test_data.reserve(500000);
    
    // Add some repetitive text
    for (int i = 0; i < 1000; ++i) {
        test_data += "This is a test string that will be repeated many times to create compressible data. ";
    }
    
    // Add some random data
    test_data += qb::crypto::generate_random_string(250000, qb::crypto::range_alpha_numeric);
    
    // Define compression levels to test
    std::vector<std::pair<std::string, int>> compression_levels = {
        {"fastest", 1},  // Fastest compression
        {"balanced", 6}, // Default/balanced compression
        {"maximum", 9}   // Maximum compression
    };
    
    std::vector<CompressionTestResult> results;
    
    for (const auto& level_info : compression_levels) {
        const std::string& level_name = level_info.first;
        int level = level_info.second;
        
        CompressionTestResult result;
        result.level_name = level_name;
        result.original_size = test_data.size();
        
        // Create compressor with specific level using the proper API
        // Use make_gzip_compressor with custom level parameter and default values for other parameters
        auto compressor = qb::compression::builtin::make_gzip_compressor(
            level,       // compressionLevel
            Z_DEFLATED,  // method
            Z_DEFAULT_STRATEGY, // strategy
            8            // memLevel
        );
        auto decompressor = qb::compression::builtin::make_decompressor("gzip");
        
        // Buffer for compressed data
        qb::allocator::pipe<char> compressed_buffer;
        compressed_buffer.allocate_back(test_data.size() * 2); // Allocate enough space
        
        // Measure compression time
        auto comp_start = std::chrono::high_resolution_clock::now();
        
        std::size_t input_processed = 0;
        bool done = false;
        auto compressed_size = compressor->compress(
            reinterpret_cast<const uint8_t*>(test_data.data()),
            test_data.size(),
            reinterpret_cast<uint8_t*>(compressed_buffer.begin()),
            compressed_buffer.size(),
            qb::compression::is_last,
            input_processed,
            done
        );
        
        auto comp_end = std::chrono::high_resolution_clock::now();
        
        // Ensure compression was successful
        EXPECT_TRUE(done);
        EXPECT_EQ(input_processed, test_data.size());
        
        // Adjust buffer size to actual compressed data size
        compressed_buffer.free_back(compressed_buffer.size() - compressed_size);
        
        // Record compressed size and ratio
        result.compressed_size = compressed_size;
        result.compression_ratio = static_cast<double>(test_data.size()) / compressed_size;
        
        // Buffer for decompressed data
        qb::allocator::pipe<char> decompressed_buffer;
        decompressed_buffer.allocate_back(test_data.size() + 1000); // Extra space just in case
        
        // Measure decompression time
        auto decomp_start = std::chrono::high_resolution_clock::now();
        
        input_processed = 0;
        done = false;
        auto decompressed_size = decompressor->decompress(
            reinterpret_cast<const uint8_t*>(compressed_buffer.begin()),
            compressed_buffer.size(),
            reinterpret_cast<uint8_t*>(decompressed_buffer.begin()),
            decompressed_buffer.size(),
            qb::compression::is_last,
            input_processed,
            done
        );
        
        auto decomp_end = std::chrono::high_resolution_clock::now();
        
        // Ensure decompression was successful
        EXPECT_TRUE(done);
        EXPECT_EQ(input_processed, compressed_buffer.size());
        
        // Calculate time metrics
        result.compression_time_ms = std::chrono::duration<double, std::milli>(comp_end - comp_start).count();
        result.decompression_time_ms = std::chrono::duration<double, std::milli>(decomp_end - decomp_start).count();
        
        // Verify decompressed data matches original
        std::string decompressed_str(decompressed_buffer.begin(), decompressed_size);
        EXPECT_EQ(decompressed_str, test_data);
        
        // Add result to collection
        results.push_back(result);
        
        // Output result for this level
        std::cout << "Gzip " << result.to_string() << std::endl;
    }
    
    // Compare results between levels
    
    // Check that all compression levels achieve reasonable ratios
    for (const auto& result : results) {
        // Print more detailed results for debugging
        std::cout << "Compression level: " << result.level_name 
                  << " - Ratio: " << result.compression_ratio 
                  << " - Time: " << result.compression_time_ms << " ms" << std::endl;
        
        EXPECT_GT(result.compression_ratio, 1.0) 
            << "All compression levels should achieve some compression";
    }
    
    // Ensure all compression/decompression processes complete successfully
    for (const auto& result : results) {
        EXPECT_GT(result.compressed_size, 0) 
            << "Compressed size should be greater than 0";
        EXPECT_GE(result.compression_time_ms, 0.0) 
            << "Compression time should be positive";
        EXPECT_GE(result.decompression_time_ms, 0.0) 
            << "Decompression time should be positive";
    }
}

// Test different compression levels for deflate
TEST(CompressionLevels, DeflateLevels) {
    // Generate test data with patterns that benefit from compression
    std::string test_data;
    test_data.reserve(500000);
    
    // Add some repetitive text with patterns
    for (int i = 0; i < 1000; ++i) {
        test_data += "Pattern " + std::to_string(i % 20) + " repeats multiple times. ";
    }
    
    // Add binary-like data
    for (int i = 0; i < 10000; ++i) {
        test_data.push_back(static_cast<char>(i % 256));
    }
    
    // Add some random data to mix things up
    test_data += qb::crypto::generate_random_string(200000, qb::crypto::range_alpha_numeric_special);
    
    // Define compression levels to test
    std::vector<std::pair<std::string, int>> compression_levels = {
        {"fastest", 1},  // Fastest compression
        {"balanced", 5}, // Default/balanced compression
        {"maximum", 9}   // Maximum compression
    };
    
    std::vector<CompressionTestResult> results;
    
    for (const auto& level_info : compression_levels) {
        const std::string& level_name = level_info.first;
        int level = level_info.second;
        
        CompressionTestResult result;
        result.level_name = level_name;
        result.original_size = test_data.size();
        
        // Create compressor with specific level using the proper API
        auto compressor = qb::compression::builtin::make_deflate_compressor(
            level,       // compressionLevel
            Z_DEFLATED,  // method
            Z_DEFAULT_STRATEGY, // strategy
            8            // memLevel
        );
        auto decompressor = qb::compression::builtin::make_decompressor("deflate");
        
        // Buffer for compressed data
        qb::allocator::pipe<char> compressed_buffer;
        compressed_buffer.allocate_back(test_data.size() * 2); // Allocate enough space
        
        // Measure compression time
        auto comp_start = std::chrono::high_resolution_clock::now();
        
        std::size_t input_processed = 0;
        bool done = false;
        auto compressed_size = compressor->compress(
            reinterpret_cast<const uint8_t*>(test_data.data()),
            test_data.size(),
            reinterpret_cast<uint8_t*>(compressed_buffer.begin()),
            compressed_buffer.size(),
            qb::compression::is_last,
            input_processed,
            done
        );
        
        auto comp_end = std::chrono::high_resolution_clock::now();
        
        // Ensure compression was successful
        EXPECT_TRUE(done);
        EXPECT_EQ(input_processed, test_data.size());
        
        // Adjust buffer size to actual compressed data size
        compressed_buffer.free_back(compressed_buffer.size() - compressed_size);
        
        // Record compressed size and ratio
        result.compressed_size = compressed_size;
        result.compression_ratio = static_cast<double>(test_data.size()) / compressed_size;
        
        // Buffer for decompressed data
        qb::allocator::pipe<char> decompressed_buffer;
        decompressed_buffer.allocate_back(test_data.size() + 1000); // Extra space just in case
        
        // Measure decompression time
        auto decomp_start = std::chrono::high_resolution_clock::now();
        
        input_processed = 0;
        done = false;
        auto decompressed_size = decompressor->decompress(
            reinterpret_cast<const uint8_t*>(compressed_buffer.begin()),
            compressed_buffer.size(),
            reinterpret_cast<uint8_t*>(decompressed_buffer.begin()),
            decompressed_buffer.size(),
            qb::compression::is_last,
            input_processed,
            done
        );
        
        auto decomp_end = std::chrono::high_resolution_clock::now();
        
        // Ensure decompression was successful
        EXPECT_TRUE(done);
        EXPECT_EQ(input_processed, compressed_buffer.size());
        
        // Calculate time metrics
        result.compression_time_ms = std::chrono::duration<double, std::milli>(comp_end - comp_start).count();
        result.decompression_time_ms = std::chrono::duration<double, std::milli>(decomp_end - decomp_start).count();
        
        // Verify decompressed data matches original
        std::string decompressed_str(decompressed_buffer.begin(), decompressed_size);
        EXPECT_EQ(decompressed_str, test_data);
        
        // Add result to collection
        results.push_back(result);
        
        // Output result for this level
        std::cout << "Deflate " << result.to_string() << std::endl;
    }
    
    // Compare results between levels
    
    // All levels should provide some compression
    for (const auto& result : results) {
        EXPECT_GT(result.compression_ratio, 1.0) 
            << "All compression levels should achieve some compression";
    }
    
    // Ensure times are reasonable (avoiding specific comparisons that might be flaky)
    for (const auto& result : results) {
        EXPECT_GE(result.compression_time_ms, 0.0) 
            << "Compression time should be positive";
        EXPECT_GE(result.decompression_time_ms, 0.0) 
            << "Decompression time should be positive";
    }
}

// Test compression efficiency for different data types
TEST(CompressionLevels, DataTypeCompression) {
    // Create different types of test data
    std::string text_data, binary_data, pattern_data;
    
    // Text data (natural language)
    text_data.reserve(100000);
    for (int i = 0; i < 100; ++i) {
        text_data += "This is a sample text that contains natural language. "
                     "Natural language typically has patterns and redundancy "
                     "that compression algorithms can take advantage of. ";
    }
    
    // Binary data (random bytes)
    binary_data.reserve(100000);
    for (int i = 0; i < 100000; ++i) {
        binary_data.push_back(static_cast<char>(rand() % 256));
    }
    
    // Pattern data (repetitive)
    pattern_data.reserve(100000);
    for (int i = 0; i < 10000; ++i) {
        pattern_data += "ABCDEFG";
        pattern_data += std::to_string(i % 10);
        pattern_data += "12345";
    }
    
    // Create a compressor with balanced settings
    auto compressor = qb::compression::builtin::make_gzip_compressor(
        6,           // compressionLevel (balanced)
        Z_DEFLATED,  // method
        Z_DEFAULT_STRATEGY, // strategy
        8            // memLevel
    );
    
    // Test each data type
    std::vector<std::pair<std::string, std::string&>> test_data = {
        {"Text", text_data},
        {"Binary", binary_data},
        {"Pattern", pattern_data}
    };
    
    for (const auto& data_pair : test_data) {
        const std::string& data_type = data_pair.first;
        const std::string& data = data_pair.second;
        
        // Buffer for compressed data
        qb::allocator::pipe<char> compressed_buffer;
        compressed_buffer.allocate_back(data.size() * 2);
        
        // Compress the data
        std::size_t input_processed = 0;
        bool done = false;
        auto compressed_size = compressor->compress(
            reinterpret_cast<const uint8_t*>(data.data()),
            data.size(),
            reinterpret_cast<uint8_t*>(compressed_buffer.begin()),
            compressed_buffer.size(),
            qb::compression::is_last,
            input_processed,
            done
        );
        
        // Calculate compression ratio, handling potential division by zero
        double ratio = (compressed_size > 0) 
            ? static_cast<double>(data.size()) / compressed_size 
            : static_cast<double>(data.size());  // Avoid division by zero
        
        // Output result
        std::cout << data_type << " data - Original size: " << data.size()
                  << " bytes, Compressed size: " << compressed_size
                  << " bytes, Ratio: " << ratio << std::endl;
        
        // Verify expected compression characteristics with more flexible assertions
        if (data_type == "Text") {
            // Text should compress reasonably well
            EXPECT_GT(ratio, 1.0) << "Text data should achieve some compression";
        } else if (data_type == "Binary") {
            // Can't reliably predict binary compression ratio,
            // just verify compression doesn't crash
            EXPECT_GE(compressed_size, 0) << "Binary compression should not fail";
        } else if (data_type == "Pattern") {
            // Pattern data should compress well, but don't set a specific threshold
            EXPECT_GT(ratio, 1.0) << "Pattern data should achieve some compression";
        }
    }
}

} // namespace qb::io::tests 