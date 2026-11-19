#!/bin/sh
Where-Object "\"<#" 2>/dev/null

# Usage: sh scripts/format.ps1 [-check] on macOS/Linux;
# ./scripts/format.ps1 [-check] in Windows PowerShell.
# Format all repository C/C++ sources and headers under src, including tests.
# -check reports formatting violations without modifying files.
set -eu
project_root=$(CDPATH= cd -P "$(dirname "$0")/.." && pwd)
check=false
for argument in "$@"; do
    case "$argument" in
        -check)
            if [ "$check" = true ]; then printf '%s\n' 'Duplicate -check flag.' >&2; exit 1; fi
            check=true
            ;;
        *) printf '%s\n' 'Usage: sh scripts/format.ps1 [-check]' >&2; exit 1 ;;
    esac
done
if ! command -v clang-format >/dev/null 2>&1; then
    printf '%s\n' 'clang-format is required. Run scripts/setup.ps1 and open a new shell.' >&2
    exit 1
fi
if [ "$check" = true ]; then
    set -- --dry-run --Werror
else
    set -- -i
fi
find "$project_root/src" -type f \( -iname '*.c' -o -iname '*.h' -o -iname '*.cpp' -o -iname '*.hpp' -o -iname '*.cc' -o -iname '*.hh' -o -iname '*.cxx' -o -iname '*.hxx' \) -exec clang-format --style=file "$@" {} +
printf '%s\n' 'formatting passed'
exit 0 #>

$ErrorActionPreference = 'Stop'
$check = $false
foreach ($argument in $args) {
    if ($argument -eq '-check') {
        if ($check) { throw 'Duplicate -check flag.' }
        $check = $true
    } else {
        throw 'Usage: ./scripts/format.ps1 [-check]'
    }
}
if (!(Get-Command clang-format -ErrorAction SilentlyContinue)) {
    throw 'clang-format is required. Run scripts/setup.ps1 and open a new shell.'
}
$root = Split-Path $PSScriptRoot -Parent
$formatArguments = @('-i')
if ($check) {
    $formatArguments = @('--dry-run', '--Werror')
}
$files = Get-ChildItem -Path (Join-Path $root 'src') -Recurse -File | Where-Object { $_.Extension -in @('.c', '.h', '.cpp', '.hpp', '.cc', '.hh', '.cxx', '.hxx') }
$status = 0
foreach ($file in $files) {
    & clang-format --style=file @formatArguments $file.FullName
    if ($LASTEXITCODE -ne 0) {
        $status = 1
    }
}
if ($status -ne 0) {
    throw 'formatting failed'
}
Write-Host 'formatting passed'
