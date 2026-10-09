#!/usr/bin/env bash
# Prove that FindNgtcp2 keeps the prerequisites of a static crypto helper after it.
set -euo pipefail

qb_source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fixture_dir="$(mktemp -d)"
trap 'rm -rf "$fixture_dir"' EXIT
mkdir -p "$fixture_dir/include/ngtcp2" "$fixture_dir/lib"

for header in ngtcp2.h ngtcp2_crypto.h ngtcp2_crypto_ossl.h; do
    : > "$fixture_dir/include/ngtcp2/$header"
done

cat > "$fixture_dir/ngtcp2.c" <<'EOF'
void ngtcp2_conn_new(void) {}
EOF
cat > "$fixture_dir/crypto.c" <<'EOF'
void crypto_anchor(void) {}
EOF
cat > "$fixture_dir/ssl.c" <<'EOF'
extern void crypto_anchor(void);
void SSL_set_quic_tls_cbs(void) { crypto_anchor(); }
void SSL_set_quic_tls_transport_params(void) {}
EOF
cat > "$fixture_dir/crypto_ossl.c" <<'EOF'
extern void ngtcp2_conn_new(void);
extern void SSL_set_quic_tls_cbs(void);
extern void SSL_set_quic_tls_transport_params(void);
void ngtcp2_crypto_ossl_init(void) {
    ngtcp2_conn_new();
    SSL_set_quic_tls_cbs();
    SSL_set_quic_tls_transport_params();
}
EOF
cat > "$fixture_dir/main.c" <<'EOF'
extern void ngtcp2_crypto_ossl_init(void);
int main(void) { ngtcp2_crypto_ossl_init(); return 0; }
EOF

"${CC:-cc}" -c "$fixture_dir/ngtcp2.c" -o "$fixture_dir/ngtcp2.o"
"${CC:-cc}" -c "$fixture_dir/crypto.c" -o "$fixture_dir/crypto.o"
"${CC:-cc}" -c "$fixture_dir/ssl.c" -o "$fixture_dir/ssl.o"
"${CC:-cc}" -c "$fixture_dir/crypto_ossl.c" -o "$fixture_dir/crypto_ossl.o"
"${AR:-ar}" rcs "$fixture_dir/lib/libngtcp2.a" "$fixture_dir/ngtcp2.o"
"${AR:-ar}" rcs "$fixture_dir/lib/libcrypto.a" "$fixture_dir/crypto.o"
"${AR:-ar}" rcs "$fixture_dir/lib/libssl.a" "$fixture_dir/ssl.o"
"${AR:-ar}" rcs "$fixture_dir/lib/libngtcp2_crypto_ossl.a" "$fixture_dir/crypto_ossl.o"

cat > "$fixture_dir/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.24)
project(qb_ngtcp2_static_link LANGUAGES C)

add_library(OpenSSL::SSL UNKNOWN IMPORTED)
set_target_properties(OpenSSL::SSL PROPERTIES IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/lib/libssl.a")
add_library(OpenSSL::Crypto UNKNOWN IMPORTED)
set_target_properties(OpenSSL::Crypto PROPERTIES IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/lib/libcrypto.a")

list(PREPEND CMAKE_MODULE_PATH "${QB_SOURCE_DIR}/cmake")
set(NGTCP2_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/include" CACHE PATH "")
set(NGTCP2_CRYPTO_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/include" CACHE PATH "")
set(NGTCP2_CRYPTO_OSSL_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/include" CACHE PATH "")
set(NGTCP2_LIBRARY "${CMAKE_SOURCE_DIR}/lib/libngtcp2.a" CACHE FILEPATH "")
set(NGTCP2_CRYPTO_OSSL_LIBRARY "${CMAKE_SOURCE_DIR}/lib/libngtcp2_crypto_ossl.a" CACHE FILEPATH "")
find_package(Ngtcp2 REQUIRED)

get_target_property(crypto_links Ngtcp2::crypto_ossl INTERFACE_LINK_LIBRARIES)
foreach(dependency IN ITEMS Ngtcp2::ngtcp2 OpenSSL::SSL OpenSSL::Crypto)
    list(FIND crypto_links "${dependency}" dependency_index)
    if(dependency_index EQUAL -1)
        message(FATAL_ERROR "Ngtcp2::crypto_ossl is missing ${dependency}")
    endif()
endforeach()

if(NGTCP2_NEGATIVE_CONTROL)
    set_property(TARGET Ngtcp2::crypto_ossl PROPERTY INTERFACE_LINK_LIBRARIES "")
endif()

add_executable(quic_static_link main.c)
# Match qb::io's direct dependency order: OpenSSL and ngtcp2 precede the helper.
target_link_libraries(quic_static_link PRIVATE
    OpenSSL::SSL OpenSSL::Crypto Ngtcp2::ngtcp2 Ngtcp2::crypto_ossl)
EOF

cmake -S "$fixture_dir" -B "$fixture_dir/positive" -DQB_SOURCE_DIR="$qb_source_dir"
cmake --build "$fixture_dir/positive"
"$fixture_dir/positive/quic_static_link"

# Darwin's linker may rescan earlier archives. GNU ld must reject the original order.
if [[ "$(uname -s)" == Linux ]] && ld --version | grep -q 'GNU ld'; then
    cmake -S "$fixture_dir" -B "$fixture_dir/negative" \
        -DQB_SOURCE_DIR="$qb_source_dir" -DNGTCP2_NEGATIVE_CONTROL=ON
    if cmake --build "$fixture_dir/negative" > "$fixture_dir/negative.log" 2>&1; then
        echo "negative control unexpectedly linked without transitive dependencies" >&2
        exit 1
    fi
    grep -q 'SSL_set_quic_tls_cbs' "$fixture_dir/negative.log"
    grep -q 'SSL_set_quic_tls_transport_params' "$fixture_dir/negative.log"
    echo "static ngtcp2 link: positive and GNU ld negative control passed"
else
    echo "static ngtcp2 link: positive passed; GNU ld negative control requires Linux"
fi
