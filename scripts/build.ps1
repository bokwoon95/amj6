#!/bin/sh
Where-Object "\"<#" 2>/dev/null

# One file, two interpreters: sh exits before PowerShell's section below.
# Usage: ./scripts/build.ps1 [-release|-clang|-coverage] [-configure|-prepare] [-quiet]
# macOS/Linux can also invoke this explicitly with sh; Windows uses PowerShell.
# Tests are built by test.ps1. There is no application to package yet.
set -eu
project_root=$(CDPATH= cd -P "$(dirname "$0")/.." && pwd)
configuration=Debug
sanitizers=ON
mode=debug
configure_only=false
prepare_only=false
quiet=false
for argument in "$@"; do
    case "$argument" in
        -release|-clang|-coverage)
            if [ "$mode" != debug ]; then printf '%s\n' 'Choose only one of -release, -clang, -coverage.' >&2; exit 1; fi
            mode=${argument#-}
            sanitizers=OFF
            if [ "$mode" = release ]; then configuration=Release; fi
            ;;
        -configure) configure_only=true ;;
        -prepare) prepare_only=true ;;
        -quiet) quiet=true ;;
        *) printf '%s\n' "Unknown argument: $argument" >&2; exit 1 ;;
    esac
done
if [ "$quiet" = true ]; then
    log=$(mktemp)
    trap 'rm -f "$log"' 0
    trap 'exit 1' HUP INT TERM
    set --
    if [ "$mode" != debug ]; then set -- "$@" "-$mode"; fi
    if [ "$configure_only" = true ]; then set -- "$@" -configure; fi
    if [ "$prepare_only" = true ]; then set -- "$@" -prepare; fi
    status=0
    sh "$0" "$@" >"$log" 2>&1 || status=$?
    if [ "$status" -ne 0 ]; then
        while IFS= read -r line || [ -n "$line" ]; do printf '%s\n' "$line"; done < "$log"
    else
        grep -E '^(Configured:|Clang tooling database:|Built:)' "$log" || :
    fi
    exit "$status"
fi
for command in cmake ninja vcpkg; do
    if ! command -v "$command" >/dev/null 2>&1; then
        printf '%s\n' "$command is required. Run scripts/setup.ps1 and open a new shell." >&2
        exit 1
    fi
done
case "$(uname -s):$(uname -m)" in
    Darwin:arm64) triplet=arm64-osx ;;
    Darwin:x86_64) triplet=x64-osx ;;
    Linux:aarch64) triplet=arm64-linux ;;
    Linux:x86_64) triplet=x64-linux ;;
    *) printf '%s\n' 'Unsupported platform; use PowerShell on Windows.' >&2; exit 1 ;;
esac
build="$project_root/build/$mode"
coverage=OFF
set --
if [ "$mode" = clang ] || [ "$mode" = coverage ]; then
    case "$(uname -s)" in
        Darwin) llvm_bin="$(brew --prefix llvm)/bin" ;;
        Linux) llvm_bin=$(dirname "$(readlink -f "$(command -v clang-tidy)")") ;;
    esac
    for tool in clang clang++ clang-tidy; do
        if [ ! -x "$llvm_bin/$tool" ]; then printf '%s\n' "Missing LLVM tool: $llvm_bin/$tool" >&2; exit 1; fi
    done
    installed="$project_root/build/debug/vcpkg_installed"
    if [ ! -d "$installed/$triplet" ]; then
        printf '%s\n' 'Build Debug first to install the development dependencies.' >&2
        exit 1
    fi
    set -- "-DCMAKE_C_COMPILER:FILEPATH=$llvm_bin/clang" "-DCMAKE_CXX_COMPILER:FILEPATH=$llvm_bin/clang++" "-DVCPKG_INSTALLED_DIR:PATH=$installed" -DVCPKG_MANIFEST_INSTALL=OFF
    if [ "$mode" = coverage ]; then coverage=ON; fi
fi
# Configure incrementally on every invocation so tooling always sees current sources.
cmake -S "$project_root" -B "$build" -G Ninja "-DCMAKE_BUILD_TYPE=$configuration" "-DENABLE_ASAN_UBSAN=$sanitizers" "-DENABLE_COVERAGE:BOOL=$coverage" "-DVCPKG_TARGET_TRIPLET:STRING=$triplet" -DBUILD_SHARED_LIBS:BOOL=OFF "$@"
if [ "$configure_only" = true ] || [ "$prepare_only" = true ] || [ "$mode" = coverage ]; then
    printf '%s\n' "Configured: $build"
    exit 0
fi
if [ "$mode" = clang ]; then
    printf '%s\n' "Clang tooling database: $build"
    exit 0
fi
cmake --build "$build" --config "$configuration" --parallel
printf '%s\n' "Built: $build"
exit #>

$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'On macOS/Linux, run sh ./scripts/build.ps1.' }
$mode = 'debug'
$configureOnly = $false
$prepareOnly = $false
$quiet = $false
foreach ($argument in $args) {
    switch ($argument) {
        { $_ -in @('-release', '-clang', '-coverage') } {
            if ($mode -ne 'debug') { throw 'Choose only one of -release, -clang, -coverage.' }
            $mode = $argument.Substring(1)
        }
        '-configure' { $configureOnly = $true }
        '-prepare' { $prepareOnly = $true }
        '-quiet' { $quiet = $true }
        default { throw "Unknown argument: $argument" }
    }
}
if ($quiet) {
    $log = [System.IO.Path]::GetTempFileName()
    $buildArguments = @($args | Where-Object { $_ -ne '-quiet' })
    try {
        & $PSCommandPath @buildArguments *> $log
        Get-Content -LiteralPath $log | Where-Object { $_ -match '^(Configured:|Clang tooling database:|Built:)' } | Out-Host
    } catch {
        Get-Content -LiteralPath $log | Out-Host
        throw
    } finally {
        Remove-Item -LiteralPath $log -Force
    }
    return
}
foreach ($command in @('cmake', 'ninja', 'vcpkg')) {
    if (!(Get-Command $command -ErrorAction SilentlyContinue)) { throw "$command is required. Run scripts/setup.ps1 and open a new shell." }
}
# Reuse or import the Visual Studio 2026 x64 developer environment.
$hasMsvcEnvironment = $env:VSCMD_VER -match '^18\.' -and $env:VSCMD_ARG_TGT_ARCH -eq 'x64' -and $env:VSCMD_ARG_HOST_ARCH -eq 'x64' -and $env:VCToolsInstallDir -and $env:INCLUDE -and $env:LIB
if (!$hasMsvcEnvironment) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio 2026 C++ Build Tools are required. Run scripts/setup.ps1.' }
    $visualStudio = & $vswhere -latest -version '[18.0,19.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or !$visualStudio) { throw 'Visual Studio 2026 C++ Build Tools were not found.' }
    $vcvars = Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars64.bat'
    $variables = & $env:ComSpec /d /c "call `"$vcvars`" >nul && set"
    if ($LASTEXITCODE -ne 0) { throw 'Could not initialize the MSVC x64 environment.' }
    foreach ($line in $variables) {
        if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
    }
}
$compiler = Join-Path $env:VCToolsInstallDir 'bin/Hostx64/x64/cl.exe'
if (!(Test-Path -LiteralPath $compiler) -or [version]$env:VCToolsVersion -lt [version]'14.50') {
    throw 'MSVC toolset 14.50 or newer is required. Run scripts/setup.ps1.'
}
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root "build/$mode"
$configuration = 'Debug'
$sanitizers = 'ON'
$coverage = 'OFF'
$triplet = 'x64-windows-static-md'
if ($mode -ne 'debug') { $sanitizers = 'OFF' }
if ($mode -eq 'release') {
    $configuration = 'Release'
    $triplet = 'x64-windows-static'
}
$clangArguments = @()
if ($mode -in @('clang', 'coverage')) {
    $llvmBin = Join-Path $env:ProgramFiles 'LLVM/bin'
    $compiler = Join-Path $llvmBin 'clang-cl.exe'
    foreach ($tool in @('clang-cl.exe', 'clang-tidy.exe')) {
        if (!(Test-Path -LiteralPath (Join-Path $llvmBin $tool))) { throw "Missing LLVM tool: $llvmBin/$tool" }
    }
    $installed = Join-Path $root 'build/debug/vcpkg_installed'
    if (!(Test-Path -LiteralPath (Join-Path $installed $triplet))) { throw 'Build Debug first to install the development dependencies.' }
    $clangArguments = @("-DVCPKG_INSTALLED_DIR:PATH=$installed", '-DVCPKG_MANIFEST_INSTALL=OFF')
    if ($mode -eq 'coverage') { $coverage = 'ON' }
}
$compiler = $compiler.Replace('\', '/')
# Only discard generated metadata if the selected compiler changes.
$cache = Join-Path $build 'CMakeCache.txt'
if (Test-Path -LiteralPath $cache) {
    $cachedCompiler = Select-String -LiteralPath $cache -Pattern '^CMAKE_CXX_COMPILER:[^=]+=(.*)$'
    if (!$cachedCompiler -or $cachedCompiler.Matches[0].Groups[1].Value.Replace('\', '/') -ne $compiler) {
        Remove-Item -LiteralPath $cache -Force
        $cmakeFiles = Join-Path $build 'CMakeFiles'
        if (Test-Path -LiteralPath $cmakeFiles) { Remove-Item -LiteralPath $cmakeFiles -Recurse -Force }
    }
}
& cmake -S $root -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$configuration" "-DENABLE_ASAN_UBSAN=$sanitizers" "-DENABLE_COVERAGE:BOOL=$coverage" "-DCMAKE_C_COMPILER:FILEPATH=$compiler" "-DCMAKE_CXX_COMPILER:FILEPATH=$compiler" "-DVCPKG_TARGET_TRIPLET:STRING=$triplet" -DBUILD_SHARED_LIBS:BOOL=OFF @clangArguments
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
if ($configureOnly -or $prepareOnly -or $mode -eq 'coverage') {
    Write-Host "Configured: $build"
    return
}
if ($mode -eq 'clang') {
    Write-Host "Clang tooling database: $build"
    return
}
& cmake --build $build --config $configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "$configuration build failed." }
Write-Host "Built: $build"
