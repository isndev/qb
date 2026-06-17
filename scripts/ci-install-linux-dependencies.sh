#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND="${DEBIAN_FRONTEND:-noninteractive}"

LLVM_VERSION="${LLVM_VERSION:-22}"

install_gcc=false
install_llvm_clang=false
install_llvm_format=false
install_coverage=false
install_google_test=false
install_google_benchmark=false
install_quic=false

usage() {
  cat <<'EOF'
Usage: ci-install-linux-dependencies.sh [options]

Options:
  --gcc-14        Install GCC/G++ 14.
  --llvm-clang    Install clang/clang++ from apt.llvm.org using LLVM_VERSION.
  --llvm-format   Install clang-format from apt.llvm.org using LLVM_VERSION.
  --coverage      Install gcovr/lcov.
  --google-test   Install system GoogleTest/GoogleMock packages.
  --google-benchmark
                  Install system Google Benchmark package.
  --quic          Install libngtcp2 and the available crypto helper.
EOF
}

while (($#)); do
  case "$1" in
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
  local crypto_backend=""

  if apt_has_package libngtcp2-dev; then
    "${SUDO[@]}" apt-get install -y libngtcp2-dev
  else
    notice "libngtcp2-dev is not available on this runner image; QUIC will stay auto-disabled."
    export_env QB_CI_NGTCP2_CRYPTO_BACKEND none
    return 0
  fi

  if apt_has_package libngtcp2-crypto-ossl-dev; then
    "${SUDO[@]}" apt-get install -y libngtcp2-crypto-ossl-dev
    crypto_backend="ossl"
  elif apt_has_package libngtcp2-crypto-gnutls-dev; then
    "${SUDO[@]}" apt-get install -y libngtcp2-crypto-gnutls-dev
    crypto_backend="gnutls"
    notice "Installed libngtcp2 GnuTLS helper. qb's current native QUIC backend uses ngtcp2 OpenSSL APIs, so QUIC may remain auto-disabled until a GnuTLS backend is implemented."
  else
    notice "No libngtcp2 crypto helper package is available on this runner image; QUIC will stay auto-disabled."
    crypto_backend="none"
  fi

  export_env QB_CI_NGTCP2_CRYPTO_BACKEND "${crypto_backend}"
}

"${SUDO[@]}" apt-get update

packages=(
  ca-certificates
  cmake
  gnupg
  libargon2-dev
  lsb-release
  libssl-dev
  ninja-build
  openssl
  pkg-config
  software-properties-common
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

"${SUDO[@]}" apt-get install -y "${packages[@]}"

if [[ "${install_llvm_clang}" == true || "${install_llvm_format}" == true ]]; then
  install_llvm_repo
  llvm_packages=()
  [[ "${install_llvm_clang}" == true ]] && llvm_packages+=("clang-${LLVM_VERSION}")
  [[ "${install_llvm_format}" == true ]] && llvm_packages+=("clang-format-${LLVM_VERSION}")
  "${SUDO[@]}" apt-get install -y "${llvm_packages[@]}"
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
