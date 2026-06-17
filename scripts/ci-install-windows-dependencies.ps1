Param(
    [switch]$GoogleTest,
    [switch]$GoogleBenchmark,
    [switch]$Quic
)

$ErrorActionPreference = "Stop"

function Write-GitHubEnv {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Value
    )

    if ($env:GITHUB_ENV) {
        "$Name=$Value" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
    }
}

if (-not (Get-Command vcpkg -ErrorAction SilentlyContinue)) {
    throw "vcpkg is required to install Windows CI dependencies."
}

$packages = @(
    "openssl:x64-windows",
    "argon2:x64-windows",
    "zlib:x64-windows"
)

if ($GoogleTest) {
    $packages += "gtest:x64-windows"
}

if ($GoogleBenchmark) {
    $packages += "benchmark:x64-windows"
}

if ($Quic) {
    $packages += "ngtcp2[openssl]:x64-windows"
}

vcpkg install @packages

if (-not $env:VCPKG_INSTALLATION_ROOT) {
    throw "VCPKG_INSTALLATION_ROOT is not set by the runner."
}

Write-GitHubEnv -Name "VCPKG_TOOLCHAIN" -Value "$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake"
