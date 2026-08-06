#!/usr/bin/env bash
set -euo pipefail

install_google_test=false
install_google_benchmark=false
install_quic=false
install_coverage=false
install_llvm=false

apply_profile() {
  case "$1" in
    build)
      install_google_test=true
      install_google_benchmark=true
      install_quic=true
      ;;
    sanitize|sanitize-thread)
      install_google_test=true
      install_quic=true
      ;;
    coverage)
      install_google_test=true
      install_quic=true
      install_coverage=true
      ;;
    format|format-check)
      install_llvm=true
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
Usage: ci-install-macos-dependencies.sh [options]

Options:
  --profile <name>    Select a CI dependency profile:
                      build, sanitize, sanitize-thread, coverage, format-check.
  --google-test       Install system GoogleTest/GoogleMock package.
  --google-benchmark  Install system Google Benchmark package.
  --coverage          Install coverage report tools.
  --llvm              Install Homebrew LLVM tools.
  --quic              Install libngtcp2.
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
    --google-test)
      install_google_test=true
      ;;
    --google-benchmark)
      install_google_benchmark=true
      ;;
    --coverage)
      install_coverage=true
      ;;
    --llvm)
      install_llvm=true
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
  # qb::json IS nlohmann::json, and since 3.0 qb no longer vendors a fallback copy. A build without
  # it FetchContent's the pinned tag, which works but clones on every run; an INSTALLABLE build
  # hard-stops, because a fetched nlohmann is in no export set and shipping its headers would put
  # nlohmann/ back in the consumer's include root. Provide the real package on both platforms.
  nlohmann-json
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

if [[ "${install_coverage}" == true ]]; then
  packages+=(gcovr lcov)
fi

if [[ "${install_llvm}" == true ]]; then
  packages+=(llvm)
fi

brew install "${packages[@]}"

prefix_path="$(brew --prefix);$(brew --prefix openssl@3);$(brew --prefix zlib)"
pkg_config_path="$(brew --prefix openssl@3)/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

if [[ "${install_quic}" == true ]]; then
  prefix_path="${prefix_path};$(brew --prefix libngtcp2)"
  pkg_config_path="$(brew --prefix libngtcp2)/lib/pkgconfig:${pkg_config_path}"
fi

if [[ "${install_llvm}" == true ]]; then
  llvm_prefix="$(brew --prefix llvm)"
  prefix_path="${prefix_path};${llvm_prefix}"
  export_env LLVM_ROOT "${llvm_prefix}"
  export_env CLANG_FORMAT "${llvm_prefix}/bin/clang-format"
  export_env CLANG_TIDY "${llvm_prefix}/bin/clang-tidy"
fi

export_env CMAKE_PREFIX_PATH "${prefix_path}"
export_env PKG_CONFIG_PATH "${pkg_config_path}"
export_env OPENSSL_EXECUTABLE "$(brew --prefix openssl@3)/bin/openssl"
