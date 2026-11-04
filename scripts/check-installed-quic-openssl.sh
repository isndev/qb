#!/usr/bin/env bash
#
# check-installed-quic-openssl.sh -- an installed qb built with QUIC compares, at its consumer, the OpenSSL that
# libngtcp2_crypto_ossl links with the one the consumer links, on this host's real libraries (Huly QB-1219; the
# comparison is QB-1183's, its report QB-1256's).
#
# Such a qb re-runs find_dependency(Ngtcp2) at its consumer; the FindNgtcp2.cmake installed beside its qbConfig.cmake
# reads which libssl and libcrypto the found helper links -- from the library's dependency list: `readelf -d` on ELF,
# `otool -L` on Mach-O -- and qbConfig.cmake reads the consumer's own the same way, then reports qb not found on two
# majors, with the remedy: they crash at the first QUIC handshake, and QUIC is compiled into qb. The unit test
# qb-io-test-unit-installed-quic-openssl plants the readings; this reads real files, in both senses:
#   1. the consumer as installed -- libngtcp2 and the consumer on one major: found, nothing said, and the readings
#      FindNgtcp2.cmake made are not empty and equal this script's own reading of the same files (a reader that read
#      nothing would say nothing either) -- on ELF through `objdump -p` too, readelf made to fail; the consumer then
#      builds and runs, loads libngtcp2_crypto_ossl, and the libssl and the libcrypto majors it loads are one each,
#      that major (`ldd` on Linux, DYLD_PRINT_LIBRARIES on macOS, the system's LibreSSL set aside);
#   2. the consumer of a libngtcp2_crypto_ossl that links the NEXT major -- a stand-in built here in the host's format,
#      the found helper's place taken by hand -- does not find qb, the reason naming both majors and libngtcp2 by the
#      path found, its remedy a libngtcp2 on the consumer's major; and so with one whose libssl alone, then whose
#      libcrypto alone, is on the next major;
#   3. a consumer whose own libssl and libcrypto are on the next major (stand-ins too), against the real helper, does
#      not find qb either, its remedy this project's OpenSSL entries; and so with its libssl alone, then its libcrypto.
# Each of the four libraries is thus once the only one on another major. A report is find_package()'s own error giving
# qbConfig.cmake's reason, its remedy on a line of its own, not the same words said otherwise. Senses 2 and 3 only
# configure: nothing is linked against a stand-in.
#
#   check-installed-quic-openssl.sh --prefix <installed qb> --work <scratch directory> [-- <consumer cmake arguments>]
#
# The prefix must hold a qb installed with QUIC (its share/qb/abi-fingerprint.txt says quic=TRUE); the consumer finds it
# there (qb_DIR) and is configured with the arguments after `--` (e.g. a CMAKE_PREFIX_PATH naming where its OpenSSL,
# libngtcp2 or nlohmann_json lives), with the generator CMAKE_GENERATOR names, else CMake's default (a multi-config one
# builds and runs Release). Exit 0 when every sense holds, 1 when one does not (each said; the script stops early when
# the consumer as installed does not configure, when its readings are empty or disagree or are not one major on both
# sides -- everything after compares with that major --, and when a stand-in it built misreads or does not build), 2 on
# a usage error (a missing argument prints the usage; each refusal says why: a prefix that is no directory or cannot be
# entered; a scratch directory, or the directory of the script's own in it, that cannot be entered, made or written, or
# that is a link; one that holds, is or sits inside the prefix; a name of the script's there that cannot be removed; an
# unknown argument), an internal error, an unsupported host, or a prefix where no qbConfig.cmake of a qb installed with
# QUIC is found or readable: nothing to prove there. The script works in a directory of its own inside the scratch
# directory, check-installed-quic-openssl/ (never a link), and removes from it first the names it writes there
# (`written`, the configured ones' logs and the build's, below); nothing else is touched.
set -euo pipefail
unset CDPATH # cd prints the directory a CDPATH entry gave it, and the paths below are read from cd

prefix=""
work=""
extra=()
usage() {
  echo "usage: $0 --prefix <installed qb> --work <scratch directory> [-- <cmake arguments>]" >&2
  exit 2
}
while (($#)); do
  case "$1" in
    --prefix|--work)
      (($# >= 2)) || usage
      if [[ "$1" == --prefix ]]; then prefix="$2"; else work="$2"; fi
      shift ;;
    --) shift; extra=("$@"); break ;;
    -h|--help) sed -n '2,/^set -euo pipefail/p' "$0" | sed '$d'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage ;;
  esac
  shift
done
[[ -n "${prefix}" && -n "${work}" ]] || usage
[[ -d "${prefix}" ]] || { echo "refusing --prefix ${prefix}: it is not a directory" >&2; usage; }
# A directory's real path, read on disk: cd -P resolves each link before a "..", and /bin/pwd -P gives the case a file
# system that ignores case stores, where the shell's keeps the case typed. A path that begins with "-" is read as one
# (cd - is the previous directory).
real_dir() {
  local dir="$1"
  [[ "${dir}" != -* ]] || dir="./${dir}"
  (cd -P -- "${dir}" && if [[ -x /bin/pwd ]]; then /bin/pwd -P; else pwd -P; fi) # NixOS has no /bin/pwd
}
given="${prefix}"
prefix="$(real_dir "${given}")" || { echo "refusing --prefix ${given}: it cannot be entered" >&2; usage; }

# quic=TRUE in the fingerprint the install wrote: qbConfig.cmake names find_dependency(Ngtcp2) either way, under
# if(TRUE) or if(FALSE).
# Five levels at most (lib/<triplet>/cmake/qb); a directory find cannot read is no answer, not an end.
config="$(find "${prefix}" -maxdepth 5 -path '*/cmake/qb/qbConfig.cmake' -print -quit 2>/dev/null || true)"
fingerprint="${prefix}/share/qb/abi-fingerprint.txt"
if [[ -z "${config}" || ! -r "${config}" || ! -f "${fingerprint}" || ! -r "${fingerprint}" ]] \
   || ! grep -Eq '(^|[[:space:]])quic=(TRUE|ON|1)([[:space:]]|$)' "${fingerprint}"; then
  echo "${prefix} holds no qb installed with QUIC that can be read (no qbConfig.cmake found or readable, or no quic=TRUE in ${fingerprint}): nothing to prove" >&2
  exit 2
fi
qb_dir="$(dirname "${config}")"

case "$(uname -s)" in
  Linux) format=ELF ;;
  Darwin) format=Mach-O ;;
  *) echo "unsupported host $(uname -s): ELF (Linux) and Mach-O (macOS) only" >&2; exit 2 ;;
esac
# The scratch directory, resolved: what this script writes there is removed first, and nothing else -- and it is
# neither the root, nor the prefix, nor inside it, nor around it.
if [[ -d "${work}" ]]; then
  given="${work}"
  work="$(real_dir "${given}")" || { echo "refusing --work ${given}: it cannot be entered" >&2; usage; }
elif [[ -e "${work}" || -L "${work}" ]]; then
  echo "refusing --work ${work}: it exists and is not a directory" >&2
  usage
else
  # Its parent resolved on its own: an assignment takes the status of its last substitution, which would hide this one.
  parent="$(dirname -- "${work}")"
  [[ -d "${parent}" ]] || { echo "refusing --work ${work}: its parent ${parent} is not a directory" >&2; usage; }
  parent="$(real_dir "${parent}")" || { echo "refusing --work ${work}: its parent cannot be entered" >&2; usage; }
  work="${parent%/}/$(basename -- "${work}")"
fi
[[ "${work}" != / ]] || { echo "refusing --work /: it holds every prefix" >&2; usage; }
# Compared with one trailing slash each: a prefix at the root, "/", holds every scratch directory.
case "${prefix%/}/" in
  "${work}/"*) echo "refusing --work ${work}: it holds the prefix ${prefix}" >&2; usage ;;
esac
case "${work}/" in
  "${prefix%/}/"*) echo "refusing --work ${work}: it is inside the prefix ${prefix}" >&2; usage ;;
esac
mkdir -p "${work}" 2>/dev/null || { echo "refusing --work ${work}: it cannot be made" >&2; usage; }
[[ -w "${work}" ]] || { echo "refusing --work ${work}: it cannot be written" >&2; usage; }
# Everything goes in a directory of the script's own there: a consumer/ or same/ of the caller's is never touched. Not a
# link -- the checks above hold for the scratch directory, and a link would carry every write and removal elsewhere.
own="${work}/check-installed-quic-openssl"
[[ ! -L "${own}" ]] || { echo "refusing --work ${work}: its check-installed-quic-openssl is a symbolic link" >&2; usage; }
[[ ! -e "${own}" || -d "${own}" ]] \
  || { echo "refusing --work ${work}: its check-installed-quic-openssl is not a directory" >&2; usage; }
mkdir -p "${own}" 2>/dev/null || { echo "refusing --work ${work}: its check-installed-quic-openssl cannot be made" >&2; usage; }
[[ -w "${own}" ]] || { echo "refusing --work ${work}: its check-installed-quic-openssl cannot be written" >&2; usage; }
work="${own}"

failures=0
fail() { echo "FAIL: $*" >&2; failures=$((failures + 1)); }

# The file the loader opens for <file>, which FindNgtcp2.cmake reads (_ngtcp2_loaded_file): on Mach-O the install name
# `otool -D` gives, when it is an absolute path that exists; the file itself otherwise.
loaded_file() {
  local id
  if [[ "${format}" == Mach-O ]]; then
    id="$(otool -D "$1" 2>/dev/null | sed -n '2,$p' | tail -n 1 || true)"
    if [[ "${id}" == /* && -e "${id}" ]]; then echo "${id}"; return; fi
  fi
  echo "$1"
}
# The major the first lib<name> entry of the dependency list of the file the loader opens for <file> carries:
# libssl.so.3 (NEEDED, or its own SONAME), libssl.4.dylib (an otool -L line). Empty when the list names none. The same
# files FindNgtcp2.cmake reads, read here with the tool itself, not through CMake.
major_of() {
  local file name="$2" listing
  file="$(loaded_file "$1")"
  if [[ "${format}" == ELF ]]; then
    listing="$(readelf -d "${file}" 2>/dev/null || true)"
    sed -nE "s#.*(NEEDED|SONAME).*[[ /]lib${name}\.so\.([0-9]+).*#\2#p" <<<"${listing}" | head -n 1
  else
    listing="$(otool -L "${file}" 2>/dev/null || true)"
    sed -nE "s#^[[:space:]]*(.*/)?lib${name}\.([0-9]+)(\.[0-9]+)*\.dylib .*#\2#p" <<<"${listing}" | head -n 1
  fi
}

# Exactly what this script writes there: each configured directory and its .log (configure() refuses a directory
# not named here), the other names, and the build's log.
configured=(same objdump helper-next helper-ssl helper-crypto consumer-next consumer-libssl consumer-libcrypto)
written=(consumer plant readelf-fails readelf-called "${configured[@]}")
for name in "${written[@]}" "${configured[@]/%/.log}" same.build.log; do
  rm -rf "${work:?}/${name}" 2>/dev/null || { echo "refusing --work ${work}: its ${name} cannot be removed" >&2; usage; }
done
mkdir -p "${work}/consumer" "${work}/plant"
cat > "${work}/consumer/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.24)
project(qb_installed_quic_consumer LANGUAGES CXX)
find_package(qb CONFIG REQUIRED)
# What the installed FindNgtcp2.cmake read, and the consumer's OpenSSL, for the script to compare with its own reading.
file(WRITE "${CMAKE_BINARY_DIR}/readings.txt"
     "HELPER=${NGTCP2_CRYPTO_OSSL_LIBRARY}\nHELPER_SSL=${Ngtcp2_OPENSSL_MAJOR}\n"
     "HELPER_CRYPTO=${Ngtcp2_OPENSSL_CRYPTO_MAJOR}\nSSL=${OPENSSL_SSL_LIBRARY}\nCRYPTO=${OPENSSL_CRYPTO_LIBRARY}\n"
     "VERSION=${OPENSSL_VERSION}\n")
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE qb::io)
# Where the consumer is built: Release's under a multi-config generator, the one configuration otherwise -- whatever
# CMAKE_BUILD_TYPE the arguments after `--` give.
get_property(_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_multi)
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/consumer.path" CONTENT "$<TARGET_FILE:consumer>" CONDITION "$<CONFIG:Release>")
else()
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/consumer.path" CONTENT "$<TARGET_FILE:consumer>")
endif()
EOF
# The consumer uses qb's TLS (its own OpenSSL) and libngtcp2's OpenSSL helper, so both are loaded when it runs.
cat > "${work}/consumer/main.cpp" <<'EOF'
#include <qb/io/tcp/ssl/context.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <openssl/crypto.h>
#include <cstdio>
int main() {
    auto const context = qb::io::ssl::Context::client();
    if (ngtcp2_crypto_ossl_init() != 0 || !context) {
        std::puts("libngtcp2's OpenSSL helper or qb's TLS context did not initialise");
        return 1;
    }
    std::printf("consumer of an installed QUIC qb: %s\n", OpenSSL_version(OPENSSL_VERSION));
    return 0;
}
EOF

# configure <directory> <cmake arguments>...: the consumer configured there, its output in <directory>.log.
configure() {
  local dir="$1"
  shift
  [[ " ${configured[*]} " == *" $(basename -- "${dir}") "* ]] \
    || { echo "internal error: ${dir} is not among the names removed first (configured)" >&2; exit 2; }
  env -u CLICOLOR_FORCE cmake -S "${work}/consumer" -B "${dir}" "-Dqb_DIR=${qb_dir}" -DCMAKE_BUILD_TYPE=Release \
        ${extra[@]+"${extra[@]}"} "$@" > "${dir}.log" 2>&1
}
reading() { if [[ -f "$1/readings.txt" ]]; then sed -n "s/^$2=//p" "$1/readings.txt"; fi; } # empty when missing
# The remedies qbConfig.cmake prints on a line of their own, which CMake does not wrap: libngtcp2's five entries, or this
# project's three OpenSSL ones.
ngtcp2_remedy='cmake -U NGTCP2_INCLUDE_DIR -U NGTCP2_LIBRARY -U NGTCP2_CRYPTO_OSSL_LIBRARY -U NGTCP2_CRYPTO_INCLUDE_DIR -U NGTCP2_CRYPTO_OSSL_INCLUDE_DIR -DNgtcp2_ROOT=<it> <build directory>'
openssl_remedy='cmake -U OPENSSL_INCLUDE_DIR -U OPENSSL_SSL_LIBRARY -U OPENSSL_CRYPTO_LIBRARY -DOPENSSL_ROOT_DIR=<it> <build directory>'
# reported <directory> <regex> <helper> <remedy> <other remedy>: the configure logged in <directory>.log stopped on
# find_package(qb), whose package reported qb not found for two majors -- CMake's own error giving qbConfig.cmake's
# reason, not the same words said otherwise, and the configure's only error: qbConfig.cmake returned there, nothing of
# it ran on -- the reason matching <regex> too, naming libngtcp2 by <helper>, the path found (NGTCP2_CRYPTO_OSSL_LIBRARY;
# on Mach-O the file read is the one the loader opens for it), with <remedy> printed as a line of its own and <other
# remedy>, the other side's, not printed. The log is read whole first: a grep -q that stops early would end the reader
# of a pipe under pipefail; each error is one line -- the log's lines from one that begins "CMake Error" to the next
# such line or to "-- Configuring incomplete", joined (CMake indents a message's lines, so none of them begins so) --
# and its runs of spaces are squeezed, CMake reflowing a message's text, as are <helper>'s. Its status says which part
# is wrong: 1 no report, 2 the text, 3 the path, 4 the remedy missing, 5 the other side's printed, 6 another error;
# missing_part names it.
reported() {
  local errors needle segment status=1
  errors="$(awk '/^CMake (Error|Warning)/ { if (e != "") print e; e = $0; next }
                 /^-- Configuring incomplete/ { if (e != "") print e; e = ""; next }
                 e != "" { e = e " " $0 }
                 END { if (e != "") print e }' "$1.log" | tr -s ' ')"
  needle="$(tr -s ' ' <<<"libngtcp2 (NGTCP2_CRYPTO_OSSL_LIBRARY ${3}) links")"
  while IFS= read -r segment; do
    grep -Eq '^CMake Error at [^ ]*CMakeLists\.txt:[0-9]+ \(find_package\): Found package configuration file: .*/qbConfig\.cmake but it set qb_FOUND to FALSE so package "qb" is considered to be NOT FOUND\. Reason given by package: libngtcp2 .*: two OpenSSL majors in one process crash at the first QUIC handshake' <<<"${segment}" \
      || continue
    if ! grep -Eq "Reason given by package: libngtcp2 .*$2" <<<"${segment}"; then
      status=2
    elif ! grep -qF -- "${needle}" <<<"${segment}"; then
      status=3
    elif ! grep -qxF -- "    $4" "$1.log"; then
      status=4
    elif grep -qxF -- "    $5" "$1.log"; then
      status=5
    elif [[ "$(grep -c '^CMake Error' "$1.log")" != 1 ]]; then
      status=6
    else
      return 0
    fi
  done <<<"${errors}"
  return "${status}"
}
missing_part() { # <reported's status> <helper> <remedy> <other remedy>
  case "$1" in
    1) echo "no report of two majors from qbConfig.cmake: qb not found, for that reason" ;;
    2) echo "the report does not name each major" ;;
    3) echo "the report does not name libngtcp2 by the path found ($2)" ;;
    4) echo "the report does not print its remedy on a line of its own ($3)" ;;
    5) echo "the report prints the other side's remedy too ($4)" ;;
    *) echo "another CMake Error beside the report: qbConfig.cmake ran on after it" ;;
  esac
}

# 1. As installed: one major on both sides, read for real, qb found and nothing said; built, run, one libssl major
#    loaded.
same="${work}/same"
if ! configure "${same}"; then
  cat "${same}.log" >&2
  fail "the consumer of ${prefix} did not configure"
  exit 1
fi
helper="$(reading "${same}" HELPER)"
helper_ssl="$(reading "${same}" HELPER_SSL)"
helper_crypto="$(reading "${same}" HELPER_CRYPTO)"
ssl="$(reading "${same}" SSL)"
crypto="$(reading "${same}" CRYPTO)"
echo "libngtcp2's OpenSSL helper: ${helper}, the loader opening $(loaded_file "${helper}")"
if [[ "${format}" == ELF ]]; then readelf -d "${helper}" || true; ldd "${helper}" || true; else otool -L "$(loaded_file "${helper}")" || true; fi
read_ssl="$(major_of "${helper}" ssl)"
read_crypto="$(major_of "${helper}" crypto)"
have_ssl="$(major_of "${ssl}" ssl)"
have_crypto="$(major_of "${crypto}" crypto)"
echo "read by FindNgtcp2.cmake: libssl ${helper_ssl:-none}, libcrypto ${helper_crypto:-none};" \
     "read here: libssl ${read_ssl:-none}, libcrypto ${read_crypto:-none};" \
     "the consumer's ${ssl} is libssl ${have_ssl:-none}, ${crypto} libcrypto ${have_crypto:-none}"
if [[ -z "${helper_ssl}" || -z "${helper_crypto}" ]]; then
  fail "FindNgtcp2.cmake read no OpenSSL major in ${helper}: the comparison would be vacuous"
elif [[ "${helper_ssl}" != "${read_ssl}" || "${helper_crypto}" != "${read_crypto}" ]]; then
  fail "FindNgtcp2.cmake read libssl ${helper_ssl} / libcrypto ${helper_crypto}, the ${format} listing says ${read_ssl:-none} / ${read_crypto:-none}"
fi
if [[ -z "${have_ssl}" || -z "${have_crypto}" ]]; then
  fail "no major read in the consumer's libssl (${ssl}) or libcrypto (${crypto})"
elif [[ "${have_ssl}" != "${read_ssl}" || "${have_crypto}" != "${read_ssl}" || "${read_crypto}" != "${read_ssl}" ]]; then
  fail "this host's libngtcp2 and its OpenSSL are not one major (helper ${read_ssl}/${read_crypto}," \
       "consumer ${have_ssl}/${have_crypto}): the one-major sense cannot be proven here"
fi
major="${read_ssl}"
# Everything below compares with that major, one on both sides: unknown, or two on this host, it would only blame the
# report that is right.
if ((failures)) || [[ -z "${major}" ]]; then
  echo "check-installed-quic-openssl (${format}): ${failures} check(s) failed, before any stand-in" >&2
  exit 1
fi
# silent <directory>: the configure logged in <directory>.log said nothing of qbConfig.cmake's two-major report -- nor
# its words said any other way: "libngtcp2 (NGTCP2_CRYPTO_OSSL_LIBRARY" begins that reason, and "[qb] libngtcp2" began
# the warning it replaced, where a message begins (a status line's "-- ", an indented body, a bare line), not inside a
# path. What it said is printed, with its context.
silent() {
  local said='^[[:space:]]*(-- )?(\[qb\][[:space:]]+libngtcp2|libngtcp2 \(NGTCP2_CRYPTO_OSSL_LIBRARY)'
  if grep -Eq "${said}" "$1.log"; then
    grep -E -B 2 -A 4 "${said}" "$1.log" >&2 || true
    return 1
  fi
}
if ! silent "${same}"; then
  fail "one major on both sides, and the consumer was told of two"
fi
if [[ "${format}" == ELF ]]; then # the same reading through `objdump -p`, which FindNgtcp2.cmake falls back on
  objdump_dir="${work}/objdump"
  # A readelf that runs and fails, as FindNgtcp2.cmake's fallback is for: an absolute path to a program exiting 1 (the
  # word `false` would be a CMake false constant, readelf merely absent), which writes the files it was asked to read
  # into the mark its environment names -- the helper must be among them.
  printf '#!/bin/sh\nprintf "%%s\\n" "$@" >> "$QB_READELF_MARK"\nexit 1\n' > "${work}/readelf-fails"
  chmod +x "${work}/readelf-fails"
  rm -f "${work}/readelf-called"
  export QB_READELF_MARK="${work}/readelf-called"
  objdump_configured=true
  configure "${objdump_dir}" "-DCMAKE_READELF=${work}/readelf-fails" || objdump_configured=false
  unset QB_READELF_MARK
  if ! "${objdump_configured}"; then
    cat "${objdump_dir}.log" >&2
    fail "the consumer did not configure with readelf failing"
  elif ! grep -qxF -- "$(reading "${objdump_dir}" HELPER)" "${work}/readelf-called" 2>/dev/null; then
    fail "the failing readelf given was never asked to read libngtcp2's helper: the objdump case read it otherwise"
  elif [[ "$(reading "${objdump_dir}" HELPER_SSL)" != "${major}" || "$(reading "${objdump_dir}" HELPER_CRYPTO)" != "${major}" ]] \
       || ! silent "${objdump_dir}"; then
    fail "through objdump -p FindNgtcp2.cmake read libssl '$(reading "${objdump_dir}" HELPER_SSL)'," \
         "libcrypto '$(reading "${objdump_dir}" HELPER_CRYPTO)', want ${major}, with nothing said"
  fi
fi
# majors_loaded <loader output> <name>: the majors of the lib<name> the consumer loaded, sorted, space-separated.
majors_loaded() {
  if [[ "${format}" == ELF ]]; then
    sed -nE "s#.*lib$2\.so\.([0-9]+).*#\1#p" <<<"$1" | sort -u | tr '\n' ' '
  else
    # Not the system's /usr/lib/lib<name>.<N>.dylib (LibreSSL), which Apple's frameworks load under their own two-level
    # namespace: no symbol of the consumer's OpenSSL resolves to it.
    grep -v ' /usr/lib/' <<<"$1" | sed -nE "s#.*/lib$2\.([0-9]+)(\.[0-9]+)*\.dylib.*#\1#p" | sort -u | tr '\n' ' '
  fi
}
consumer=""
if ! cmake --build "${same}" --config Release > "${same}.build.log" 2>&1; then
  cat "${same}.build.log" >&2
  fail "the consumer did not build"
elif ! consumer="$(cat "${same}/consumer.path")" || [[ ! -x "${consumer}" ]]; then
  fail "the consumer built is not where its build says (${same}/consumer.path: '${consumer}')"
elif ! output="$(cd "${same}" && if [[ "${format}" == ELF ]]; then "${consumer}"; else DYLD_PRINT_LIBRARIES=1 "${consumer}" 2>&1; fi)"; then
  echo "${output}" >&2
  fail "the consumer did not run"
else
  echo "${output}" | grep '^consumer of' || true
  loader="${output}"
  if [[ "${format}" == ELF ]] && ! loader="$(ldd "${consumer}")"; then
    fail "ldd could not list what the consumer loads"
  fi
  [[ "${format}" == ELF ]] && echo "${loader}"
  for lib in ssl crypto; do
    if [[ "$(majors_loaded "${loader}" "${lib}")" != "${major} " ]]; then
      fail "the consumer loads lib${lib} majors '$(majors_loaded "${loader}" "${lib}")', want '${major}' alone"
    fi
  done
  if ! grep -q 'libngtcp2_crypto_ossl' <<<"${loader}"; then
    fail "the consumer does not load libngtcp2_crypto_ossl: the helper's OpenSSL is not in its process"
  fi
fi

# Stand-ins built here in the host's format: libssl and libcrypto of the next major and of this one, and three
# libngtcp2_crypto_ossl -- both libraries on the next major, its libssl alone, its libcrypto alone.
next=$((major + 1))
plant="${work}/plant"
# A C compiler for the stand-ins, CC as CMake takes it unquoted: the whole string when it names a program (a path may
# hold a space), else a command and its arguments split on spaces; cc when CC is unset.
if [[ -z "${CC:-}" ]]; then
  cc=(cc)
elif [[ -f "${CC}" && -x "${CC}" ]] || type -P -- "${CC}" > /dev/null 2>&1; then
  cc=("${CC}")
else
  read -r -a cc <<<"${CC}"
fi
echo 'int qb_planted_stand_in(void) { return 0; }' > "${plant}/stand_in.c"
# lib <name> <major>: the path of the stand-in lib<name> of OpenSSL <major>, built on first use.
lib() {
  local file
  if [[ "${format}" == ELF ]]; then
    file="${plant}/lib$1.so.$2"
    [[ -f "${file}" ]] || command "${cc[@]}" -shared -fPIC -o "${file}" "${plant}/stand_in.c" "-Wl,-soname,lib$1.so.$2" \
      || { fail "the stand-in ${file} did not build (CC: ${cc[*]})"; exit 1; }
  else
    file="${plant}/lib$1.$2.dylib"
    [[ -f "${file}" ]] || command "${cc[@]}" -dynamiclib -o "${file}" "${plant}/stand_in.c" -install_name "${file}" \
      || { fail "the stand-in ${file} did not build (CC: ${cc[*]})"; exit 1; }
  fi
  echo "${file}"
}
# helper <directory> <libssl major> <libcrypto major>: a stand-in libngtcp2_crypto_ossl linking those two.
helper() {
  local ssl_lib crypto_lib file
  ssl_lib="$(lib ssl "$2")"
  crypto_lib="$(lib crypto "$3")"
  mkdir -p "${plant}/$1"
  if [[ "${format}" == ELF ]]; then
    file="${plant}/$1/libngtcp2_crypto_ossl.so"
    command "${cc[@]}" -shared -fPIC -o "${file}" "${plant}/stand_in.c" -Wl,--no-as-needed "${ssl_lib}" "${crypto_lib}" \
      || { fail "the stand-in ${file} did not build (CC: ${cc[*]})"; exit 1; }
  else
    file="${plant}/$1/libngtcp2_crypto_ossl.dylib"
    command "${cc[@]}" -dynamiclib -o "${file}" "${plant}/stand_in.c" -install_name "${file}" "${ssl_lib}" "${crypto_lib}" \
      || { fail "the stand-in ${file} did not build (CC: ${cc[*]})"; exit 1; }
  fi
  # A stand-in says what it must before any verdict about it counts.
  if [[ "$(major_of "${file}" ssl)" != "$2" || "$(major_of "${file}" crypto)" != "$3" ]]; then
    fail "the stand-in ${file} reads as libssl '$(major_of "${file}" ssl)' / libcrypto '$(major_of "${file}" crypto)', built for $2 / $3"
    exit 1
  fi
  echo "${file}"
}
for name in ssl crypto; do
  file="$(lib "${name}" "${next}")"
  if [[ "$(major_of "${file}" "${name}")" != "${next}" ]]; then
    fail "the stand-in ${file} reads as lib${name} '$(major_of "${file}" "${name}")', built for ${next}"
    exit 1
  fi
done

# 2. libngtcp2's OpenSSL on the next major, both libraries, its libssl alone, its libcrypto alone: qb not found, the
#    reason naming each major (what FindNgtcp2.cmake read in the stand-in) and libngtcp2's remedy.
for case in "next|${next}|${next}|links OpenSSL ${next}, and this project links the libssl of OpenSSL ${major} " \
            "ssl|${next}|${major}|links the libssl of OpenSSL ${next} and the libcrypto of OpenSSL ${major}, and this project links the libssl of OpenSSL ${major} " \
            "crypto|${major}|${next}|links the libssl of OpenSSL ${major} and the libcrypto of OpenSSL ${next}, and this project links the libssl of OpenSSL ${major} "; do
  IFS='|' read -r name ssl_major crypto_major text <<<"${case}"
  dir="${work}/helper-${name}"
  planted="$(helper "helper-${name}" "${ssl_major}" "${crypto_major}")" # exits the script on a stand-in that misreads
  if configure "${dir}" "-DNGTCP2_CRYPTO_OSSL_LIBRARY=${planted}"; then
    fail "the consumer of a libngtcp2 on libssl ${ssl_major} / libcrypto ${crypto_major} configured: qb was found"
  else
    why=0
    reported "${dir}" "${text}" "${planted}" "${ngtcp2_remedy}" "${openssl_remedy}" || why=$?
    if ((why)); then
      cat "${dir}.log" >&2
      fail "libngtcp2 on libssl ${ssl_major} / libcrypto ${crypto_major}, the consumer on ${major}: $(missing_part "${why}" "${planted}" "${ngtcp2_remedy}" "${openssl_remedy}")"
    fi
  fi
done

# 3. The consumer's own OpenSSL on the next major, both libraries, its libssl alone, its libcrypto alone, against the
#    real helper: qb not found, the reason naming each major and this project's OpenSSL remedy.
# consumer_case <name> <text the reason must hold> <cmake argument>...
consumer_case() {
  local name="$1" text="$2" dir="${work}/consumer-$1"
  shift 2
  if configure "${dir}" "$@"; then
    fail "the consumer with its ${name} OpenSSL on ${next} configured: qb was found"
  else
    why=0
    reported "${dir}" "${text}" "${helper}" "${openssl_remedy}" "${ngtcp2_remedy}" || why=$?
    if ((why)); then
      cat "${dir}.log" >&2
      fail "libngtcp2 on OpenSSL ${major}, the consumer's ${name} OpenSSL on ${next}: $(missing_part "${why}" "${helper}" "${openssl_remedy}" "${ngtcp2_remedy}")"
    fi
  fi
}
next_ssl="$(lib ssl "${next}")"
next_crypto="$(lib crypto "${next}")"
consumer_case next \
  "links OpenSSL ${major}, and this project links the libssl of OpenSSL ${next} .* and the libcrypto of OpenSSL ${next} " \
  "-DOPENSSL_SSL_LIBRARY=${next_ssl}" "-DOPENSSL_CRYPTO_LIBRARY=${next_crypto}"
consumer_case libssl \
  "links OpenSSL ${major}, and this project links the libssl of OpenSSL ${next} .* and the libcrypto of OpenSSL ${major} " \
  "-DOPENSSL_SSL_LIBRARY=${next_ssl}"
consumer_case libcrypto \
  "links OpenSSL ${major}, and this project links the libssl of OpenSSL ${major} .* and the libcrypto of OpenSSL ${next} " \
  "-DOPENSSL_CRYPTO_LIBRARY=${next_crypto}"

if ((failures)); then
  echo "check-installed-quic-openssl (${format}): ${failures} check(s) failed" >&2
  exit 1
fi
echo "check-installed-quic-openssl (${format}): libngtcp2 and the consumer on OpenSSL ${major}, read for real -- silent," \
     "built, run, the helper and one libssl and libcrypto major loaded; libngtcp2's OpenSSL on ${next}, its libssl alone," \
     "its libcrypto alone -- qb not found, libngtcp2's remedy; the consumer's OpenSSL on ${next}, its libssl alone, its" \
     "libcrypto alone -- qb not found, its OpenSSL's remedy"
