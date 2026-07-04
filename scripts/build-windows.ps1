#!/usr/bin/env pwsh
# scripts/build-windows.ps1 — one-command local Windows build of the
# BSFChat client (voice + RTP video enabled).
#
# Bootstraps the toolchain that isn't on PATH (CMake, nasm, Perl,
# OpenSSL, Qt) and configures + builds with CMake, then runs the tests.
# Every path can be overridden with the matching BSFCHAT_* environment
# variable, so this works on any machine once the tools exist somewhere.
#
# Usage:
#   powershell -File scripts/build-windows.ps1                 # Release build + tests
#   powershell -File scripts/build-windows.ps1 -Clean          # wipe build dir first
#   powershell -File scripts/build-windows.ps1 -BuildType Debug -NoTests
#   powershell -File scripts/build-windows.ps1 -Generator Ninja
#
# Generator: defaults to the Visual Studio generator, which is what CI
# uses. Ninja is faster but trips over a libyuv quirk on Windows — it
# declares a static `yuv` and a shared `yuv_shared` (OUTPUT_NAME "yuv"),
# so both want to emit yuv.lib. Ninja rejects the duplicate output even
# though the shared target is excluded from the build; MSBuild doesn't.
#
# Requires (defaults reflect this machine's local setup):
#   Qt 6.10.2 msvc2022_64 + qtmultimedia   -> C:\Qt\6.10.2\msvc2022_64
#   OpenSSL 3.x dev (headers+libs, x64)    -> C:\tools\openssl\x64
#   CMake >= 3.20                          -> C:\tools\cmake-*/bin
#   nasm (for libaom / AV1 lossless tier)  -> C:\tools\nasm-*
#   Perl (for libaom)                      -> Git for Windows bundles one
#   MSVC (VS 2019+ BuildTools)
[CmdletBinding()]
param(
    [string]$BuildType = 'Release',
    [string]$BuildDir  = 'build',
    [string]$Generator = 'Visual Studio 16 2019',
    [switch]$Clean,
    [switch]$NoTests
)
$ErrorActionPreference = 'Stop'

$QtDir      = if ($env:BSFCHAT_QT_DIR)      { $env:BSFCHAT_QT_DIR }      else { 'C:\Qt\6.10.2\msvc2022_64' }
$OpenSSLDir = if ($env:BSFCHAT_OPENSSL_DIR) { $env:BSFCHAT_OPENSSL_DIR } else { 'C:\tools\openssl\x64' }
$CMakeBin   = if ($env:BSFCHAT_CMAKE_BIN)   { $env:BSFCHAT_CMAKE_BIN }   else { 'C:\tools\cmake-3.31.6-windows-x86_64\bin' }
$NasmBin    = if ($env:BSFCHAT_NASM_BIN)    { $env:BSFCHAT_NASM_BIN }    else { 'C:\tools\nasm-2.16.03' }
$NinjaBin   = if ($env:BSFCHAT_NINJA_BIN)   { $env:BSFCHAT_NINJA_BIN }   else { 'C:\tools\ninja' }
$VcVars     = if ($env:BSFCHAT_VCVARS)      { $env:BSFCHAT_VCVARS }      else { 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat' }

# libaom (AV1 lossless tier) needs Perl at configure + build time. We
# pass it by absolute path via -DPERL_EXECUTABLE rather than putting it
# on PATH: Git's usr\bin also ships a coreutils link.exe that would
# shadow MSVC's linker and break the build. Prefer a perl already on
# PATH, else fall back to the one Git for Windows bundles.
$Perl = if ($env:BSFCHAT_PERL) { $env:BSFCHAT_PERL }
        elseif (Get-Command perl -ErrorAction SilentlyContinue) { (Get-Command perl).Source }
        elseif (Test-Path 'C:\Program Files\Git\usr\bin\perl.exe') { 'C:\Program Files\Git\usr\bin\perl.exe' }
        else { $null }
if (-not $Perl) { throw "Perl not found (needed by libaom). Install Strawberry Perl or set BSFCHAT_PERL." }

# Repo root is the parent of this script's directory.
$Root = Split-Path -Parent $PSScriptRoot

$UsingNinja = $Generator -eq 'Ninja'

# --- sanity checks ---------------------------------------------------
$checks = @(
    @{ n = 'Qt';      p = "$QtDir\bin\qmake.exe" },
    @{ n = 'OpenSSL'; p = "$OpenSSLDir\include\openssl\ssl.h" },
    @{ n = 'CMake';   p = "$CMakeBin\cmake.exe" },
    @{ n = 'nasm';    p = "$NasmBin\nasm.exe" }
)
if ($UsingNinja) {
    $checks += @{ n = 'Ninja';    p = "$NinjaBin\ninja.exe" }
    $checks += @{ n = 'vcvars64'; p = $VcVars }
}
foreach ($chk in $checks) {
    if (-not (Test-Path $chk.p)) {
        throw "$($chk.n) not found at '$($chk.p)'. Install it or set the matching BSFCHAT_* env var."
    }
}

# --- MSVC environment (Ninja only) ----------------------------------
# The Visual Studio generator drives MSBuild and sets up the toolset
# itself; Ninja needs cl.exe/INCLUDE/LIB in the environment, so import
# them from vcvars64.bat into this session.
if ($UsingNinja) {
    Write-Host "==> Importing MSVC environment from vcvars64.bat"
    $vcOut = cmd /c "call `"$VcVars`" >nul 2>&1 && set"
    foreach ($line in $vcOut) {
        if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
    }
}

# Prepend our off-PATH tools so cmake/nasm and the OpenSSL DLLs resolve
# regardless of the caller's PATH (nasm must be found by libaom's ASM
# language step under either generator). Qt's bin is added too so the
# test executables (and the app, if launched from here) find the Qt
# runtime DLLs.
$env:PATH = "$CMakeBin;$NasmBin;$OpenSSLDir\bin;$QtDir\bin;$env:PATH"

# --- configure -------------------------------------------------------
$build = Join-Path $Root $BuildDir
if ($Clean -and (Test-Path $build)) {
    Write-Host "==> Removing $build"
    Remove-Item -Recurse -Force $build
}

$genArgs = @('-G', $Generator)
if (-not $UsingNinja) { $genArgs += @('-A', 'x64') }

Write-Host "==> Configuring ($BuildType, $Generator) in $build"
& cmake -S $Root -B $build @genArgs `
    "-DCMAKE_BUILD_TYPE=$BuildType" `
    "-DCMAKE_PREFIX_PATH=$QtDir" `
    "-DOPENSSL_ROOT_DIR=$OpenSSLDir" `
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" `
    "-DPERL_EXECUTABLE=$Perl" `
    "-DBSFCHAT_VERSION=0.0.0-dev" `
    -Wno-dev
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }

# --- build -----------------------------------------------------------
Write-Host "==> Building"
& cmake --build $build --config $BuildType -j
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }

# --- test ------------------------------------------------------------
if (-not $NoTests) {
    Write-Host "==> Running tests"
    & ctest --test-dir $build --output-on-failure -C $BuildType
    if ($LASTEXITCODE -ne 0) { throw "Tests failed ($LASTEXITCODE)" }
}

$exe = Get-ChildItem -Path $build -Recurse -Filter bsfchat-app.exe -ErrorAction SilentlyContinue | Select-Object -First 1
Write-Host ""
if ($exe) { Write-Host "==> Done. App binary: $($exe.FullName)" }
else      { Write-Host "==> Build finished but bsfchat-app.exe was not found under $build" }
