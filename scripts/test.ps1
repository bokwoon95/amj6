#!/bin/sh
Where-Object "\"<#" 2>/dev/null

# Usage: sh scripts/test.ps1 [-coverage] [CTest arguments] on macOS/Linux;
# ./scripts/test.ps1 [-coverage] [CTest arguments] in Windows PowerShell.
# Normal tests use the platform compiler. Coverage uses LLVM Clang in build/coverage,
# reusing Debug dependencies, and opens build/coverage/html/index.html.
set -eu
project_root=$(CDPATH= cd -P "$(dirname "$0")/.." && pwd)
coverage=false
argument_count=$#
while [ "$argument_count" -gt 0 ]; do
    argument=$1
    shift
    if [ "$argument" = -coverage ]; then
        if [ "$coverage" = true ]; then printf '%s\n' 'Duplicate -coverage flag.' >&2; exit 1; fi
        coverage=true
    else
        set -- "$@" "$argument"
    fi
    argument_count=$((argument_count - 1))
done
printf '%s\n' 'Preparing test build...'
sh "$project_root/scripts/build.ps1" -prepare
build="$project_root/build/debug"
if [ "$coverage" = true ]; then
    sh "$project_root/scripts/build.ps1" -coverage -prepare
    build="$project_root/build/coverage"
    case "$(uname -s)" in
        Darwin) llvm_bin="$(brew --prefix llvm)/bin" ;;
        Linux) llvm_bin=$(dirname "$(readlink -f "$(command -v clang-tidy)")") ;;
    esac
    for tool in llvm-profdata llvm-cov; do
        if [ ! -x "$llvm_bin/$tool" ]; then printf '%s\n' "Missing LLVM tool: $llvm_bin/$tool" >&2; exit 1; fi
    done
    rm -rf "$build/profiles" "$build/html"
    mkdir -p "$build/profiles"
    export LLVM_PROFILE_FILE="$build/profiles/%m-%p.profraw"
fi
cmake --build "$build" --config Debug --parallel --target amj_tests
status=0
ctest --test-dir "$build" --build-config Debug --output-on-failure --no-tests=error "$@" || status=$?
if [ "$coverage" = true ]; then
    set -- "$build"/profiles/*.profraw
    if [ ! -f "$1" ]; then printf '%s\n' 'No coverage profiles were generated.' >&2; exit 1; fi
    "$llvm_bin/llvm-profdata" merge -sparse "$@" -o "$build/coverage.profdata"
    # Explicit source selection keeps dependencies and test code out of reports.
    find "$project_root/src" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) ! -name '*_test.*' ! -path '*/tests/*' > "$build/coverage-sources.txt"
    set --
    while IFS= read -r source; do set -- "$@" "$source"; done < "$build/coverage-sources.txt"
    "$llvm_bin/llvm-cov" report "$build/amj_tests" -instr-profile="$build/coverage.profdata" "$@"
    "$llvm_bin/llvm-cov" show "$build/amj_tests" -instr-profile="$build/coverage.profdata" -format=html -output-dir="$build/html" "$@"
    printf '%s\n' "Coverage report: $build/html/index.html"
    case "$(uname -s)" in
        Darwin) open "$build/html/index.html" ;;
        Linux) xdg-open "$build/html/index.html" ;;
    esac
fi
exit "$status" #>

$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') {
    throw 'On macOS/Linux, run sh ./scripts/test.ps1.'
}
$coverage = $false
$ctestArguments = @()
foreach ($argument in $args) {
    if ($argument -eq '-coverage') {
        if ($coverage) { throw 'Duplicate -coverage flag.' }
        $coverage = $true
    } else {
        $ctestArguments += $argument
    }
}
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build/debug'
Write-Host 'Preparing test build...'
& (Join-Path $PSScriptRoot 'build.ps1') -prepare
if ($coverage) {
    & (Join-Path $PSScriptRoot 'build.ps1') -coverage -prepare
    $build = Join-Path $root 'build/coverage'
    $llvmBin = Join-Path $env:ProgramFiles 'LLVM/bin'
    foreach ($tool in @('llvm-profdata.exe', 'llvm-cov.exe')) {
        if (!(Test-Path -LiteralPath (Join-Path $llvmBin $tool) -PathType Leaf)) {
            throw "Missing LLVM tool: $llvmBin/$tool"
        }
    }
    foreach ($directory in @('profiles', 'html')) {
        $path = Join-Path $build $directory
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
    }
    New-Item -ItemType Directory -Path (Join-Path $build 'profiles') -Force | Out-Null
}
& cmake --build $build --config Debug --parallel --target amj_tests
if ($LASTEXITCODE -ne 0) { throw 'Test build failed.' }

if (!$coverage) {
    # Normal MSVC tests need the ASan runtime beside the executable.
    $compilerDirectory = Join-Path $env:VCToolsInstallDir 'bin/Hostx64/x64'
    $asanDlls = @(Get-ChildItem -LiteralPath $compilerDirectory -Filter 'clang_rt.asan*dynamic-x86_64.dll' -File)
    if ($asanDlls.Count -eq 0) {
        throw 'MSVC AddressSanitizer runtime is missing. Install the C++ AddressSanitizer component through Visual Studio Installer.'
    }
    foreach ($dll in $asanDlls) { Copy-Item -LiteralPath $dll.FullName -Destination $build -Force }
}
$previousProfile = $env:LLVM_PROFILE_FILE
try {
    if ($coverage) { $env:LLVM_PROFILE_FILE = "$build/profiles/%m-%p.profraw" }
    & ctest --test-dir $build --build-config Debug --output-on-failure --no-tests=error @ctestArguments
    $testStatus = $LASTEXITCODE
} finally {
    $env:LLVM_PROFILE_FILE = $previousProfile
}
if ($coverage) {
    $profiles = @(Get-ChildItem -LiteralPath (Join-Path $build 'profiles') -Filter '*.profraw' -File | ForEach-Object { $_.FullName })
    if ($profiles.Count -eq 0) { throw 'No coverage profiles were generated.' }
    $profile = Join-Path $build 'coverage.profdata'
    & (Join-Path $llvmBin 'llvm-profdata.exe') merge -sparse @profiles -o $profile
    if ($LASTEXITCODE -ne 0) { throw 'Coverage profile merge failed.' }
    $sources = @(Get-ChildItem -LiteralPath (Join-Path $root 'src') -Recurse -File | Where-Object { $_.Extension -in @('.cpp', '.hpp', '.h') -and $_.Name -notlike '*_test.*' -and $_.FullName -notmatch '[/\\]tests[/\\]' } | ForEach-Object { $_.FullName })
    $binary = Join-Path $build 'amj_tests.exe'
    & (Join-Path $llvmBin 'llvm-cov.exe') report $binary "-instr-profile=$profile" @sources
    if ($LASTEXITCODE -ne 0) { throw 'Coverage summary failed.' }
    & (Join-Path $llvmBin 'llvm-cov.exe') show $binary "-instr-profile=$profile" -format=html "-output-dir=$build/html" @sources
    if ($LASTEXITCODE -ne 0) { throw 'Coverage HTML generation failed.' }
    $report = Join-Path $build 'html/index.html'
    Write-Host "Coverage report: $report"
    Start-Process -FilePath $report
}
if ($testStatus -ne 0) { throw 'Tests failed.' }
