Param(
    [ValidateSet("", "build", "sanitize", "sanitize-thread", "coverage", "format", "format-check")]
    [string]$Profile = "",
    [switch]$GoogleTest,
    [switch]$GoogleBenchmark,
    [switch]$Quic,
    [switch]$Llvm
)

$ErrorActionPreference = "Stop"

$installBase = $true
$installGoogleTest = [bool]$GoogleTest
$installGoogleBenchmark = [bool]$GoogleBenchmark
$installQuic = [bool]$Quic
$installLlvm = [bool]$Llvm

switch ($Profile) {
    "" {
    }
    "build" {
        $installGoogleTest = $true
        $installGoogleBenchmark = $true
        $installQuic = $true
    }
    { $_ -in @("sanitize", "sanitize-thread") } {
        $installGoogleTest = $true
        $installQuic = $true
    }
    "coverage" {
        $installGoogleTest = $true
        $installQuic = $true
    }
    { $_ -in @("format", "format-check") } {
        $installBase = $false
        $installLlvm = $true
    }
}

function Write-GitHubEnv {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Value
    )

    if ($env:GITHUB_ENV) {
        "$Name=$Value" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
    }
}

function Write-GitHubPath {
    param(
        [Parameter(Mandatory = $true)][string]$Value
    )

    if ($env:GITHUB_PATH) {
        $Value | Out-File -FilePath $env:GITHUB_PATH -Append -Encoding utf8
    }
}

if ($installLlvm) {
    if (-not (Get-Command choco -ErrorAction SilentlyContinue)) {
        throw "Chocolatey is required to install Windows LLVM tools."
    }

    choco install llvm -y --no-progress

    $llvmBin = Join-Path $env:ProgramFiles "LLVM/bin"
    if (Test-Path $llvmBin) {
        $env:PATH = "$llvmBin;$env:PATH"
        Write-GitHubPath -Value $llvmBin
    }

    $clangFormat = Get-Command clang-format.exe -ErrorAction SilentlyContinue
    if ($clangFormat) {
        Write-GitHubEnv -Name "CLANG_FORMAT" -Value $clangFormat.Path
    }

    $clangTidy = Get-Command clang-tidy.exe -ErrorAction SilentlyContinue
    if ($clangTidy) {
        Write-GitHubEnv -Name "CLANG_TIDY" -Value $clangTidy.Path
    }
}

$packages = @()

if ($installBase) {
    $packages += @(
        "openssl:x64-windows",
        "argon2:x64-windows",
        "zlib:x64-windows"
    )
}

if ($installGoogleTest) {
    $packages += "gtest:x64-windows"
}

if ($installGoogleBenchmark) {
    $packages += "benchmark:x64-windows"
}

if ($installQuic) {
    $packages += "ngtcp2[openssl]:x64-windows"
}

if ($packages.Count -gt 0) {
    if (-not (Get-Command vcpkg -ErrorAction SilentlyContinue)) {
        throw "vcpkg is required to install Windows CI dependencies."
    }

    vcpkg install @packages

    if (-not $env:VCPKG_INSTALLATION_ROOT) {
        throw "VCPKG_INSTALLATION_ROOT is not set by the runner."
    }

    Write-GitHubEnv -Name "VCPKG_TOOLCHAIN" -Value "$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake"
}
