#!/usr/bin/env bash
set -euo pipefail

install_google_test=false
install_google_benchmark=false
install_quic=false

usage() {
  cat <<'EOF'
Usage: ci-install-macos-dependencies.sh [options]

Options:
  --google-test       Install system GoogleTest/GoogleMock package.
  --google-benchmark  Install system Google Benchmark package.
  --quic              Install libngtcp2.
EOF
}

while (($#)); do
  case "$1" in
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

if ! command -v brew >/dev/null 2>&1; then
  echo "Homebrew is required to install macOS CI dependencies." >&2
  exit 1
fi

export_env() {
  local key="$1"
  local value="$2"
  if [[ -n "${GITHUB_ENV:-}" ]]; then
    echo "${key}=${value}" >> "${GITHUB_ENV}"
  fi
}

packages=(
  argon2
  ninja
  openssl@3
  zlib
)

if [[ "${install_google_test}" == true ]]; then
  packages+=(googletest)
fi

if [[ "${install_google_benchmark}" == true ]]; then
  packages+=(google-benchmark)
fi

if [[ "${install_quic}" == true ]]; then
  packages+=(libngtcp2)
fi

brew install "${packages[@]}"

prefix_path="$(brew --prefix);$(brew --prefix openssl@3);$(brew --prefix zlib)"
pkg_config_path="$(brew --prefix openssl@3)/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

if [[ "${install_quic}" == true ]]; then
  prefix_path="${prefix_path};$(brew --prefix libngtcp2)"
  pkg_config_path="$(brew --prefix libngtcp2)/lib/pkgconfig:${pkg_config_path}"
fi

export_env CMAKE_PREFIX_PATH "${prefix_path}"
export_env PKG_CONFIG_PATH "${pkg_config_path}"
export_env OPENSSL_EXECUTABLE "$(brew --prefix openssl@3)/bin/openssl"
