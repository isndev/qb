/**
 * @file qb/source/io/src/crypto_advanced.cpp
 * @brief Implementation of advanced cryptographic utilities for the QB IO library
 * 
 * This file provides implementations of advanced cryptographic operations such as:
 * - HKDF (HMAC-based Key Derivation Function) per RFC 5869
 * - Argon2 key derivation (password hashing)
 * - Secure token generation and verification
 * - Constant-time comparison algorithms
 * - Password hashing and verification
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

#include <qb/io/crypto.h>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <random>
#include <ctime>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/kdf.h>
#endif

// Argon2 library inclusion
#if defined(QB_IO_WITH_ARGON2)
#include <argon2.h>
#endif

namespace qb {

// Implementation of HKDF (HMAC-based Key Derivation Function)
std::vector<unsigned char> 
crypto::hkdf(
    const std::vector<unsigned char>& input_key_material,
    const std::vector<unsigned char>& salt,
    const std::vector<unsigned char>& info,
    size_t output_length,
    DigestAlgorithm digest) {
    
    // Get the EVP_MD for the chosen digest algorithm
    const EVP_MD* md = get_evp_md(digest);
    if (!md) {
        throw std::runtime_error("Invalid digest algorithm for HKDF");
    }
    
    // Create output buffer
    std::vector<unsigned char> output(output_length, 0);
    
    // Extract phase: Use HMAC with salt as key to extract entropy from input
    std::vector<unsigned char> prk;
    if (salt.empty()) {
        // Use a buffer of zeros as salt if none provided (per RFC 5869)
        std::vector<unsigned char> default_salt(EVP_MD_size(md), 0);
        prk = hmac(input_key_material, default_salt, digest);
    } else {
        prk = hmac(input_key_material, salt, digest);
    }
    
    // Expand phase: Use HMAC with PRK as key to generate output key material
    std::vector<unsigned char> previous;
    std::vector<unsigned char> temp;
    
    size_t digest_len = EVP_MD_size(md);
    size_t n = (output_length + digest_len - 1) / digest_len; // Ceiling division
    
    for (size_t i = 1; i <= n; i++) {
        // T(i) = HMAC-Hash(PRK, T(i-1) | info | i)
        std::vector<unsigned char> data = previous;
        data.insert(data.end(), info.begin(), info.end());
        data.push_back(static_cast<unsigned char>(i));
        
        temp = hmac(data, prk, digest);
        
        // Copy to output
        size_t copy_size = std::min(output_length - ((i - 1) * digest_len), digest_len);
        std::copy(temp.begin(), temp.begin() + copy_size, 
                  output.begin() + ((i - 1) * digest_len));
        
        previous = temp;
    }
    
    return output;
}

// Implementation of Argon2 key derivation
std::vector<unsigned char> 
crypto::argon2_kdf(
    const std::string& password,
    size_t key_length,
    const Argon2Params& params,
    Argon2Variant variant) {
    
    // Create output buffer
    std::vector<unsigned char> output(key_length, 0);
    
#if defined(QB_IO_WITH_ARGON2)
    // Determine the variant
    argon2_type type;
    switch (variant) {
        case Argon2Variant::Argon2d:
            type = Argon2_d;
            break;
        case Argon2Variant::Argon2i:
            type = Argon2_i;
            break;
        case Argon2Variant::Argon2id:
        default:
            type = Argon2_id;
            break;
    }
    
    // Get salt
    std::vector<unsigned char> salt_bytes;
    if (params.salt.empty()) {
        // Generate a random salt if none provided
        salt_bytes = generate_salt(16);  // 16-byte salt
    } else {
        salt_bytes.assign(params.salt.begin(), params.salt.end());
    }
    
    // Run Argon2
    int result = argon2_hash(
        params.t_cost,                               // t_cost (iterations)
        params.m_cost,                               // m_cost (memory in KiB)
        params.parallelism,                          // parallelism
        password.c_str(),                            // password
        password.length(),                           // password length
        salt_bytes.data(),                           // salt
        salt_bytes.size(),                           // salt length
        output.data(),                               // output hash
        output.size(),                               // output hash length
        nullptr,                                     // encoded (not used)
        0,                                           // encoded length
        type,                                        // variant
        ARGON2_VERSION_13                            // version
    );
    
    if (result != ARGON2_OK) {
        throw std::runtime_error("Argon2 key derivation failed: " + 
                                 std::string(argon2_error_message(result)));
    }
#else
    // Fallback implementation using PBKDF2 with higher iterations when Argon2 is not available
    std::vector<unsigned char> salt_bytes;
    if (params.salt.empty()) {
        // Generate a random salt if none provided
        salt_bytes = generate_salt(16);  // 16-byte salt
    } else {
        salt_bytes.assign(params.salt.begin(), params.salt.end());
    }
    
    // Use higher iteration count as fallback
    int iterations = 100000;  // Higher iterations to compensate for weaker memory hardness
    
    if (PKCS5_PBKDF2_HMAC(
            password.c_str(),
            password.length(),
            salt_bytes.data(),
            salt_bytes.size(),
            iterations,
            EVP_sha256(),
            key_length,
            output.data()) != 1) {
        throw std::runtime_error("PBKDF2 key derivation failed (Argon2 fallback)");
    }
#endif
    
    return output;
}

// Implementation of unified key derivation function
std::vector<unsigned char> 
crypto::derive_key(
    const std::string& password,
    const std::vector<unsigned char>& salt,
    size_t key_length,
    KdfAlgorithm algorithm,
    int iterations,
    const Argon2Params& argon2_params) {
    
    switch (algorithm) {
        case KdfAlgorithm::PBKDF2: {
            // Utiliser PKCS5_PBKDF2_HMAC au lieu de l'API EVP_PKEY
            std::vector<unsigned char> output(key_length, 0);
            
            if (PKCS5_PBKDF2_HMAC(
                    password.c_str(),
                    password.length(),
                    salt.data(),
                    salt.size(),
                    iterations,
                    EVP_sha256(),
                    key_length,
                    output.data()) != 1) {
                throw std::runtime_error("PBKDF2 key derivation failed");
            }
            
            return output;
        }
        case KdfAlgorithm::HKDF: {
            // Empty info for this high-level interface
            std::vector<unsigned char> empty_info;
            
            // Convert password to bytes if it's provided as a string
            std::vector<unsigned char> input_key_material(password.begin(), password.end());
            
            return hkdf(input_key_material, salt, empty_info, key_length, DigestAlgorithm::SHA256);
        }
        case KdfAlgorithm::Argon2:
        default: {
            // Create a copy of the params that includes the provided salt
            Argon2Params params = argon2_params;
            if (!salt.empty()) {
                params.salt.assign(reinterpret_cast<const char*>(salt.data()), salt.size());
            }
            
            return argon2_kdf(password, key_length, params, Argon2Variant::Argon2id);
        }
    }
}

// Implementation of constant-time comparison
bool 
crypto::constant_time_compare(
    const std::vector<unsigned char>& a,
    const std::vector<unsigned char>& b) {
    
    // If lengths differ, return false (but still do the comparison to avoid timing attacks)
    bool result = (a.size() == b.size());
    size_t max_len = std::max(a.size(), b.size());
    
    unsigned char diff = 0;
    for (size_t i = 0; i < max_len; i++) {
        // Get byte i if it exists, otherwise 0
        unsigned char byte_a = (i < a.size()) ? a[i] : 0;
        unsigned char byte_b = (i < b.size()) ? b[i] : 0;
        
        // XOR the bytes and OR with the running diff
        diff |= byte_a ^ byte_b;
    }
    
    // Result is true only if both lengths are equal AND diff is 0
    return result && (diff == 0);
}

// Implementation of secure token generation
std::string 
crypto::generate_token(
    const std::string& payload,
    const std::vector<unsigned char>& key,
    uint64_t ttl) {
    
    using json = nlohmann::json;
    
    // Create token data
    json token_data;
    token_data["payload"] = payload;
    
    // Add timestamp and expiration if TTL is provided
    uint64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    token_data["iat"] = now;  // Issued at
    
    if (ttl > 0) {
        token_data["exp"] = now + ttl;  // Expiration
    }
    
    // Generate a random IV
    std::vector<unsigned char> iv = generate_iv(SymmetricAlgorithm::AES_256_GCM);
    
    // Convert token data to string
    std::string token_str = token_data.dump();
    std::vector<unsigned char> token_bytes(token_str.begin(), token_str.end());
    
    // Encrypt the token data
    std::vector<unsigned char> encrypted = encrypt(
        token_bytes, key, iv, SymmetricAlgorithm::AES_256_GCM);
    
    // Combine IV and encrypted data
    std::vector<unsigned char> combined;
    combined.insert(combined.end(), iv.begin(), iv.end());
    combined.insert(combined.end(), encrypted.begin(), encrypted.end());
    
    // Encode to Base64URL
    return base64url_encode(combined);
}

// Implementation of token verification
std::string 
crypto::verify_token(
    const std::string& token,
    const std::vector<unsigned char>& key) {
    
    using json = nlohmann::json;
    
    try {
        // Decode the token from Base64URL
        std::vector<unsigned char> decoded = base64url_decode(token);
        
        // Extract IV and ciphertext
        if (decoded.size() < 12) {  // Minimum size for AES-GCM IV
            return "";
        }
        
        std::vector<unsigned char> iv(decoded.begin(), decoded.begin() + 12);
        std::vector<unsigned char> ciphertext(decoded.begin() + 12, decoded.end());
        
        // Decrypt the token
        std::vector<unsigned char> decrypted = decrypt(
            ciphertext, key, iv, SymmetricAlgorithm::AES_256_GCM);
        
        if (decrypted.empty()) {
            return "";  // Decryption failed (authentication tag didn't match)
        }
        
        // Parse the token data
        std::string token_str(decrypted.begin(), decrypted.end());
        json token_data = json::parse(token_str);
        
        // Check expiration
        if (token_data.contains("exp")) {
            uint64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            
            if (now > token_data["exp"].get<uint64_t>()) {
                return "";  // Token has expired
            }
        }
        
        // Return the payload
        return token_data["payload"].get<std::string>();
        
    } catch (const std::exception&) {
        return "";  // Any exception results in token rejection
    }
}

// Implementation of Base64URL encoding
std::string 
crypto::base64url_encode(const std::vector<unsigned char>& data) {
    // First perform regular base64 encoding
    std::string base64 = base64_encode(data.data(), data.size());
    
    // Replace '+' with '-', '/' with '_', and remove padding '='
    for (char& c : base64) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    
    // Remove padding
    size_t padding_pos = base64.find_last_not_of('=');
    if (padding_pos != std::string::npos) {
        base64.erase(padding_pos + 1);
    }
    
    return base64;
}

// Implementation of Base64URL decoding
std::vector<unsigned char> 
crypto::base64url_decode(const std::string& input) {
    // Convert from Base64URL to standard Base64
    std::string base64 = input;
    
    // Replace '-' with '+' and '_' with '/'
    for (char& c : base64) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    
    // Add padding if necessary
    while (base64.size() % 4 != 0) {
        base64.push_back('=');
    }
    
    // Perform standard Base64 decoding
    return base64_decode(base64);
}

// Implementation of salt generation
std::vector<unsigned char> 
crypto::generate_salt(size_t length) {
    std::vector<unsigned char> salt(length);
    if (!secure_random_fill(salt)) {
        throw std::runtime_error("Failed to generate secure random salt");
    }
    return salt;
}

// Implementation of password hashing
std::string 
crypto::hash_password(
    const std::string& password,
    Argon2Variant variant) {
    
#if defined(QB_IO_WITH_ARGON2)
    // Generate a random salt
    std::vector<unsigned char> salt = generate_salt(16);
    
    // Determine the variant
    argon2_type type;
    switch (variant) {
        case Argon2Variant::Argon2d:
            type = Argon2_d;
            break;
        case Argon2Variant::Argon2i:
            type = Argon2_i;
            break;
        case Argon2Variant::Argon2id:
        default:
            type = Argon2_id;
            break;
    }
    
    // Set default parameters
    uint32_t t_cost = 3;                  // 3 iterations
    uint32_t m_cost = 1 << 16;            // 64 MiB
    uint32_t parallelism = 1;             // 1 thread
    
    // Allocate encoded hash string (large enough for all parameters and values)
    const size_t encoded_len = 128;
    char encoded[encoded_len];
    
    // Hash the password
    int result = argon2_hash(
        t_cost,                           // t_cost (iterations)
        m_cost,                           // m_cost (memory in KiB)
        parallelism,                      // parallelism
        password.c_str(),                 // password
        password.length(),                // password length
        salt.data(),                      // salt
        salt.size(),                      // salt length
        nullptr,                          // raw hash (not used)
        0,                                // raw hash length
        encoded,                          // encoded hash output
        encoded_len,                      // encoded hash length
        type,                             // variant
        ARGON2_VERSION_13                 // version
    );
    
    if (result != ARGON2_OK) {
        throw std::runtime_error("Password hashing failed: " + 
                                std::string(argon2_error_message(result)));
    }
    
    return std::string(encoded);
#else
    // Fallback implementation using PBKDF2-HMAC-SHA256
    std::vector<unsigned char> salt = generate_salt(16);
    
    // Higher iterations for better security
    int iterations = 100000;
    int key_length = 32;  // 256 bits
    
    // Generate the hash with PBKDF2
    std::vector<unsigned char> hash_bytes(key_length);
    if (PKCS5_PBKDF2_HMAC(
            password.c_str(),
            password.length(),
            salt.data(),
            salt.size(),
            iterations,
            EVP_sha256(),
            key_length,
            hash_bytes.data()) != 1) {
        throw std::runtime_error("PBKDF2 password hashing failed (Argon2 fallback)");
    }
    
    // Format the hash for storage with algorithm, iterations, salt, and hash
    // Format: $pbkdf2-sha256$i=100000$salt_base64$hash_base64
    std::string salt_base64 = base64_encode(salt.data(), salt.size());
    std::string hash_base64 = base64_encode(hash_bytes.data(), hash_bytes.size());
    
    std::stringstream ss;
    ss << "$pbkdf2-sha256$i=" << iterations << "$" << salt_base64 << "$" << hash_base64;
    
    return ss.str();
#endif
}

// Implementation of password verification
bool 
crypto::verify_password(
    const std::string& password,
    const std::string& hash) {
    
#if defined(QB_IO_WITH_ARGON2)
    // Verify the password against the stored hash
    int result = argon2_verify(
        hash.c_str(),                     // Encoded hash
        password.c_str(),                 // Password
        password.length(),                // Password length
        Argon2_id                         // Argon2 type (default to id)
    );
    
    return (result == ARGON2_OK);
#else
    // Fallback implementation for PBKDF2 format
    // Parse the hash string format: $pbkdf2-sha256$i=iterations$salt$hash
    if (hash.substr(0, 13) != "$pbkdf2-sha256") {
        return false;  // Unsupported format
    }
    
    size_t iter_start = hash.find("i=");
    if (iter_start == std::string::npos) return false;
    
    size_t iter_end = hash.find('$', iter_start);
    if (iter_end == std::string::npos) return false;
    
    size_t salt_start = iter_end + 1;
    size_t salt_end = hash.find('$', salt_start);
    if (salt_end == std::string::npos) return false;
    
    size_t hash_start = salt_end + 1;
    
    // Parse iterations
    std::string iter_str = hash.substr(iter_start, iter_end - iter_start);
    if (iter_str.substr(0, 2) != "i=") return false;
    int iterations = std::stoi(iter_str.substr(2));
    
    // Extract salt and stored hash
    std::string salt_base64 = hash.substr(salt_start, salt_end - salt_start);
    std::string stored_hash_base64 = hash.substr(hash_start);
    
    std::vector<unsigned char> salt;
    std::vector<unsigned char> stored_hash;
    
    try {
        salt = base64_decode(salt_base64);
        stored_hash = base64_decode(stored_hash_base64);
    } catch (const std::exception&) {
        return false; // Invalid base64 encoding
    }
    
    // Generate hash with the same parameters
    std::vector<unsigned char> computed_hash(stored_hash.size());
    if (PKCS5_PBKDF2_HMAC(
            password.c_str(),
            password.length(),
            salt.data(),
            salt.size(),
            iterations,
            EVP_sha256(),
            stored_hash.size(),
            computed_hash.data()) != 1) {
        return false;
    }
    
    // Compare in constant time
    return constant_time_compare(computed_hash, stored_hash);
#endif
}

// Implementation of unique IV generation
std::vector<unsigned char> 
crypto::generate_unique_iv(size_t size) {
    // Create output buffer
    std::vector<unsigned char> iv(size);
    
    // Fill most of the IV with random data
    if (!secure_random_fill(iv)) {
        throw std::runtime_error("Failed to generate secure random IV");
    }
    
    // Use the last 8 bytes for timestamp and counter to ensure uniqueness
    if (size >= 8) {
        // Get current time
        uint64_t timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        // Use static counter for additional uniqueness
        static std::atomic<uint32_t> counter(0);
        uint32_t current_count = counter.fetch_add(1, std::memory_order_relaxed);
        
        // Combine timestamp and counter
        uint64_t unique_value = (timestamp << 32) | current_count;
        
        // XOR the last 8 bytes with the unique value
        for (size_t i = 0; i < 8 && i < size; i++) {
            iv[size - i - 1] ^= (unique_value >> (i * 8)) & 0xFF;
        }
    }
    
    return iv;
}

// Implementation of authenticated encryption with metadata
std::string 
crypto::encrypt_with_metadata(
    const std::vector<unsigned char>& plaintext,
    const std::vector<unsigned char>& key,
    const std::string& metadata,
    SymmetricAlgorithm algorithm) {
    
    using json = nlohmann::json;
    
    try {
        // Generate a unique IV
        std::vector<unsigned char> iv = generate_unique_iv(12);  // 12 bytes for GCM
        
        // Convert metadata to bytes
        std::vector<unsigned char> metadata_bytes(metadata.begin(), metadata.end());
        
        // Encrypt the plaintext with metadata as AAD
        std::vector<unsigned char> ciphertext = encrypt(
            plaintext, key, iv, algorithm, metadata_bytes);
        
        // Create the output structure
        json output;
        output["iv"] = base64_encode(iv.data(), iv.size());
        output["ciphertext"] = base64_encode(ciphertext.data(), ciphertext.size());
        output["metadata"] = metadata;
        output["alg"] = static_cast<int>(algorithm);
        
        return output.dump();
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Encryption with metadata failed: " + std::string(e.what()));
    }
}

// Implementation of authenticated decryption with metadata verification
std::optional<std::pair<std::vector<unsigned char>, std::string>>
crypto::decrypt_with_metadata(
    const std::string& ciphertext,
    const std::vector<unsigned char>& key,
    SymmetricAlgorithm algorithm) {
    
    using json = nlohmann::json;
    
    try {
        // Parse the input JSON
        json input = json::parse(ciphertext);
        
        // Extract the components
        std::vector<unsigned char> iv = base64_decode(input["iv"].get<std::string>());
        std::vector<unsigned char> encrypted_data = base64_decode(input["ciphertext"].get<std::string>());
        std::string metadata = input["metadata"].get<std::string>();
        
        // Check algorithm if specified
        if (input.contains("alg")) {
            SymmetricAlgorithm stored_alg = static_cast<SymmetricAlgorithm>(input["alg"].get<int>());
            if (stored_alg != algorithm) {
                algorithm = stored_alg;  // Use the algorithm specified in the ciphertext
            }
        }
        
        // Convert metadata to bytes
        std::vector<unsigned char> metadata_bytes(metadata.begin(), metadata.end());
        
        // Decrypt the data with metadata as AAD
        std::vector<unsigned char> plaintext = decrypt(
            encrypted_data, key, iv, algorithm, metadata_bytes);
        
        if (plaintext.empty()) {
            return std::nullopt;  // Authentication failed
        }
        
        return std::make_pair(plaintext, metadata);
        
    } catch (const std::exception&) {
        return std::nullopt;  // Any exception results in decryption failure
    }
}

} // namespace qb 