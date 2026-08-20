#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND="${DEBIAN_FRONTEND:-noninteractive}"

LLVM_VERSION="${LLVM_VERSION:-22}"

# apt on a hosted runner, bounded three ways.
#
# Measured twice on the sibling repo's identical step, and the first diagnosis was wrong. The
# step produced ZERO output between the command echo and the cancellation twenty minutes later
# -- not one `Hit:` or `Get:` line -- so it was never fetching: it was waiting on the dpkg/apt
# lock that unattended-upgrades holds while a fresh runner settles, and apt waits for that
# FOREVER by default. Acquire::http::Timeout, the first fix, bounds network fetches and so
# changed nothing; the second run timed out at 20m11s exactly like the first.
#
# DPkg::Lock::Timeout makes apt give up on the lock rather than wait. `timeout` caps the call
# whatever the reason it hangs, which is the guard that does not depend on the diagnosis being
# right. Only then does retrying mean anything, because the first two guarantee there is a
# second turn. `sudo timeout`, never `timeout sudo`: the latter signals sudo, which need not
# pass it on to the apt-get it spawned, so the call would look bounded and not be.
APT_LOCK_OPTS=(-o DPkg::Lock::Timeout=120 -o Acquire::Retries=3)

apt_retry() {
  local attempt limit=300
  [ "${1:-}" = install ] && limit=420
  for attempt in 1 2 3; do
    if "${SUDO[@]}" timeout "$limit" apt-get "${APT_LOCK_OPTS[@]}" "$@"; then
      return 0
    fi
    echo "::warning::apt-get $1 failed or timed out (attempt ${attempt}/3); retrying" >&2
    sleep $((attempt * 15))
  done
  echo "::error::apt-get $1 failed three times" >&2
  return 1
}

install_gcc=false
install_llvm_clang=false
install_llvm_format=false
install_coverage=false
install_google_test=false
install_google_benchmark=false
install_quic=false

apply_profile() {
  case "$1" in
    build)
      install_gcc=true
      install_llvm_clang=true
      install_google_test=true
      install_google_benchmark=true
      install_quic=true
      ;;
    sanitize|sanitize-thread)
      install_llvm_clang=true
      install_google_test=true
      install_quic=true
      ;;
    coverage)
      install_gcc=true
      install_coverage=true
      install_google_test=true
      install_quic=true
      ;;
    format|format-check)
      install_llvm_format=true
      ;;
    *)
      echo "Unknown profile: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
}

usage() {
  cat <<'EOF'
Usage: ci-install-linux-dependencies.sh [options]

Options:
  --profile <name> Select a CI dependency profile:
                   build, sanitize, sanitize-thread, coverage, format-check.
  --gcc-14        Install GCC/G++ 14.
  --llvm-clang    Install clang/clang++ from apt.llvm.org using LLVM_VERSION.
  --llvm-format   Install clang-format from apt.llvm.org using LLVM_VERSION.
  --coverage      Install gcovr/lcov.
  --google-test   Install system GoogleTest/GoogleMock packages.
  --google-benchmark
                  Install system Google Benchmark package.
  --quic          Install libngtcp2 and its OpenSSL crypto helper (needs OpenSSL >= 3.5).
EOF
}

while (($#)); do
  case "$1" in
    --profile)
      if [[ $# -lt 2 ]]; then
        echo "--profile requires a value" >&2
        usage >&2
        exit 2
      fi
      apply_profile "$2"
      shift
      ;;
    --profile=*)
      apply_profile "${1#*=}"
      ;;
    --gcc-14)
      install_gcc=true
      ;;
    --llvm-clang)
      install_llvm_clang=true
      ;;
    --llvm-format)
      install_llvm_format=true
      ;;
    --coverage)
      install_coverage=true
      ;;
    --google-test)
      install_google_test=true
      ;;
    --google-benchmark)
      install_google_benchmark=true
      ;;
    --quic)
      install_quic=true
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

notice() {
  if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
    echo "::notice::$*"
  else
    echo "$*"
  fi
}

if [[ "${EUID}" -eq 0 ]]; then
  SUDO=()
elif command -v sudo >/dev/null 2>&1; then
  SUDO=(sudo)
else
  echo "This script needs root privileges or sudo." >&2
  exit 1
fi

export_env() {
  local key="$1"
  local value="$2"
  if [[ -n "${GITHUB_ENV:-}" ]]; then
    echo "${key}=${value}" >> "${GITHUB_ENV}"
  fi
}

apt_has_package() {
  apt-cache show "$1" >/dev/null 2>&1
}

install_llvm_repo() {
  wget -O /tmp/llvm.sh https://apt.llvm.org/llvm.sh
  chmod +x /tmp/llvm.sh
  "${SUDO[@]}" /tmp/llvm.sh "${LLVM_VERSION}"
}

install_ngtcp2() {
  # qb's QUIC transport is built solely on the ngtcp2 *OpenSSL* crypto helper
  # (ngtcp2_crypto_ossl), which requires OpenSSL >= 3.5 (the native QUIC-TLS API).
  # qb has no GnuTLS/BoringSSL/wolfSSL backend, so only the OpenSSL helper is usable;
  # when it is unavailable, QUIC simply stays auto-disabled (QB_WITH_QUIC=AUTO).
  local crypto_backend="none"

  if ! apt_has_package libngtcp2-dev; then
    notice "libngtcp2-dev not available on this image; QUIC stays auto-disabled."
    export_env QB_CI_NGTCP2_CRYPTO_BACKEND none
    return 0
  fi
  apt_retry install -y libngtcp2-dev

  if apt_has_package libngtcp2-crypto-ossl-dev; then
    apt_retry install -y libngtcp2-crypto-ossl-dev
    crypto_backend="ossl"
  else
    # Do NOT install the GnuTLS helper: qb cannot use it, so it would only add a
    # useless package and a misleading "fallback".
    notice "libngtcp2-crypto-ossl-dev not available (needs OpenSSL >= 3.5); QUIC stays auto-disabled."
  fi

  export_env QB_CI_NGTCP2_CRYPTO_BACKEND "${crypto_backend}"
}

apt_retry update

# software-properties-common is an Ubuntu package that does not exist on Debian -- and the
# self-hosted Debian runner proved it: three retries of "Unable to locate package". It only
# ever mattered for add-apt-repository, which llvm.sh does not need on Debian. Install it
# where the archive has it, skip it loudly where it does not.
if apt-cache show software-properties-common >/dev/null 2>&1; then
  extra_repo_pkgs=(software-properties-common)
else
  echo "software-properties-common not in this distribution's archive (Debian) - skipping"
  extra_repo_pkgs=()
fi

packages=(
  ca-certificates
  cmake
  gnupg
  libargon2-dev
  lsb-release
  libssl-dev
  ninja-build
  # qb::json IS nlohmann::json, so this is not optional -- and since 3.0 qb no longer vendors a
  # fallback copy. Without it a build FetchContent's the pinned tag (fine for build+test) but an
  # INSTALLABLE build hard-stops, because a fetched nlohmann cannot be exported and shipping its
  # headers would put nlohmann/ back in the consumer's include root. install-consume and
  # package-consume both install, so both need the real package here.
  nlohmann-json3-dev
  openssl
  pkg-config
  wget
  zlib1g-dev
)

if [[ "${install_gcc}" == true ]]; then
  packages+=(gcc-14 g++-14)
fi

if [[ "${install_coverage}" == true ]]; then
  packages+=(gcovr lcov)
fi

if [[ "${install_google_test}" == true ]]; then
  packages+=(libgtest-dev libgmock-dev)
fi

if [[ "${install_google_benchmark}" == true ]]; then
  packages+=(libbenchmark-dev)
fi

apt_retry install -y "${packages[@]}" "${extra_repo_pkgs[@]}"

if [[ "${install_llvm_clang}" == true || "${install_llvm_format}" == true ]]; then
  install_llvm_repo
  llvm_packages=()
  [[ "${install_llvm_clang}" == true ]] && llvm_packages+=("clang-${LLVM_VERSION}")
  [[ "${install_llvm_format}" == true ]] && llvm_packages+=("clang-format-${LLVM_VERSION}")
  apt_retry install -y "${llvm_packages[@]}"
fi

if [[ "${install_quic}" == true ]]; then
  install_ngtcp2
fi

if command -v openssl >/dev/null 2>&1; then
  export_env OPENSSL_EXECUTABLE "$(command -v openssl)"
fi

if [[ "${install_coverage}" == true ]]; then
  export_env GCOV_PATH "$(command -v gcov-14 || command -v gcov)"
fi
