#!/bin/sh
Where-Object "\"<#" 2>/dev/null

# -quiet prints only the final status on success, or all output on failure.
set -u
work=""
trap '
    exit_status=$?
    if [ -n "$work" ]; then rm -rf "$work"; fi
    if [ "$exit_status" -eq 0 ]; then
        printf "%s\n" "linting passed"
    else
        printf "%s\n" "linting failed"
    fi
    exit "$exit_status"
' EXIT
caller_directory=$(pwd -P)
project_root=$(CDPATH= cd -P "$(dirname "$0")/.." && pwd)
cd "$project_root" || exit 1
fix=false
changed=false
quiet=false
explicit_paths=false
for argument in "$@"; do
    case "$argument" in
        -fix)
            if [ "$fix" = true ]; then
                printf '%s\n' 'Usage: sh scripts/lint.ps1 [-fix] [-changed] [-quiet] [source.cpp ...]' >&2
                exit 1
            fi
            fix=true
            ;;
        -changed)
            if [ "$changed" = true ]; then
                printf '%s\n' '-changed may only be provided once.' >&2
                exit 1
            fi
            changed=true
            ;;
        -quiet)
            if [ "$quiet" = true ]; then
                printf '%s\n' '-quiet may only be provided once.' >&2
                exit 1
            fi
            quiet=true
            ;;
        -*) printf '%s\n' "Unknown argument: $argument" >&2; exit 1 ;;
        *) explicit_paths=true ;;
    esac
done
# Capture the whole run, including tooling preparation and successful operations.
# Keep caller-relative paths intact when re-running without -quiet.
if [ "$quiet" = true ]; then
    work=$(mktemp -d) || exit 1
    trap 'exit 1' HUP INT TERM
    for argument in "$@"; do
        shift
        if [ "$argument" != -quiet ]; then set -- "$@" "$argument"; fi
    done
    status=0
    (cd "$caller_directory" && sh "$project_root/scripts/lint.ps1" "$@") > "$work/output.log" 2>&1 || status=$?
    if [ "$status" -ne 0 ]; then
        # The outer exit trap prints the final status exactly once, at the end.
        while IFS= read -r line || [ -n "$line" ]; do
            case "$line" in
                'linting passed'|'linting failed') continue ;;
            esac
            printf '%s\n' "$line"
        done < "$work/output.log"
    fi
    exit "$status"
fi
changed_paths=""
changed_header=false
if [ "$changed" = true ]; then
    # Bash reads NUL-delimited Git paths; the existing source lists use lines.
    # Reject embedded newlines rather than silently selecting the wrong file.
    changed_paths=$(bash -c '
        set -euo pipefail
        work=$(mktemp -d)
        trap '\''rm -rf "$work"'\'' EXIT
        git diff --no-relative --name-only --diff-filter=ACMR -z HEAD -- > "$work/paths"
        git ls-files --others --exclude-standard --full-name -z >> "$work/paths"
        while IFS= read -r -d "" path; do
            case "$path" in
                *$'\''\n'\''*) printf "%s\n" "Newlines in Git paths are not supported." >&2; exit 1 ;;
                src/*.cpp|src/*.cc|src/*.cxx|src/*.h|src/*.hh|src/*.hpp|src/*.hxx)
                    if [ -f "$path" ]; then printf "%s\n" "$path"; fi
                    ;;
            esac
        done < "$work/paths"
    ') || exit 1
    if [ -z "$changed_paths" ] && [ "$explicit_paths" = false ]; then
        printf '%s\n' 'No changed repository C++ sources or headers; nothing to lint.'
        exit 0
    fi
    while IFS= read -r path; do
        case "$path" in
            *.h|*.hh|*.hpp|*.hxx) changed_header=true ;;
        esac
    done <<EOF
$changed_paths
EOF
fi
# Refresh the dedicated Clang compilation database before linting.
sh "$project_root/scripts/build.ps1" -clang || exit 1
case "$(uname -s)" in
    Darwin) tidy="$(brew --prefix llvm)/bin/clang-tidy" ;;
    Linux) tidy=$(readlink -f "$(command -v clang-tidy)") ;;
esac
build="$project_root/build/clang"
if [ ! -d "$build" ] || [ -z "$(find "$build" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null)" ]; then
    printf '%s\n' "Build directory is missing or empty: $build. Run sh scripts/build.ps1 -clang first." >&2
    exit 1
fi
build=$(CDPATH= cd -P "$build" && pwd) || exit 1
if [ ! -f "$build/compile_commands.json" ]; then
    printf '%s\n' "Compilation database not found: $build/compile_commands.json. Run sh scripts/build.ps1 -clang first." >&2
    exit 1
fi
if [ "$fix" = true ] && ! command -v clang-format >/dev/null 2>&1; then
    printf '%s\n' 'clang-format is required. Run scripts/setup.ps1 and open a new shell.' >&2
    exit 1
fi
for command in clang-tidy jq; do
    if ! command -v "$command" >/dev/null 2>&1; then
        printf '%s\n' "$command is required. Run scripts/setup.ps1 and open a new shell." >&2
        exit 1
    fi
done

# clang-tidy discovers the shared .clang-tidy configuration automatically.
# --fix is not a semantic-safety guarantee: review changes and rebuild.
status=0
# Compilation databases include generated sources; only lint repository sources.
work=$(mktemp -d) || exit 1
source_list="$work/sources"
pids=""
trap 'for pid in $pids; do kill "$pid" 2>/dev/null || :; done; exit 1' HUP INT TERM
if ! jq -er 'if type != "array" then error("Expected a compilation database array") else [.[] | if (.file | type) != "string" or (.directory | type) != "string" then error("Entries require file and directory strings") else if (.file | startswith("/")) then .file else .directory + "/" + .file end end] | unique | .[] end' "$build/compile_commands.json" > "$source_list"; then
    printf '%s\n' "Could not read source files from $build/compile_commands.json. Run sh scripts/build.ps1 -clang first." >&2
    exit 1
fi
# Optional paths are relative to the caller; the shell expands globs before entry.
selected=false
selected_list="$work/selected"
: > "$selected_list"
for argument in "$@"; do
    if [ "$argument" = -fix ] || [ "$argument" = -changed ]; then continue; fi
    selected=true
    source="$argument"
    case "$source" in
        /*) ;;
        *) source="$caller_directory/$source" ;;
    esac
    if [ ! -f "$source" ]; then
        printf '%s\n' "Source file not found: $argument" >&2
        exit 1
    fi
    directory=$(CDPATH= cd -P "$(dirname "$source")" && pwd) || exit 1
    source="$directory/$(basename "$source")"
    case "$source" in
        "$project_root"/src/*) ;;
        *) printf '%s\n' "Not a repository source: $argument" >&2; exit 1 ;;
    esac
    if ! grep -Fxq -- "$source" "$source_list"; then
        printf '%s\n' "No compilation database entry for: $argument (pass compiled source files, not headers)" >&2
        exit 1
    fi
    printf '%s\n' "$source" >> "$selected_list"
done
if [ "$changed" = true ] && [ "$changed_header" = false ]; then
    selected=true
    while IFS= read -r path; do
        if [ -z "$path" ]; then continue; fi
        source="$project_root/$path"
        if grep -Fxq -- "$source" "$source_list"; then
            printf '%s\n' "$source" >> "$selected_list"
        else
            printf '%s\n' "Skipping changed source without a compilation database entry: $path"
        fi
    done <<EOF
$changed_paths
EOF
fi
# Changed headers require all translation units, even with explicit paths.
if [ "$changed_header" = true ]; then selected=false; fi
if [ "$selected" = true ]; then
    sort -u "$selected_list" > "$source_list"
fi
if [ "$changed" = true ] && [ ! -s "$source_list" ]; then
    printf '%s\n' 'No changed compiled sources; nothing to lint.'
    exit 0
fi
# Cap concurrent clang-tidy processes to limit memory usage.
workers=$(getconf _NPROCESSORS_ONLN 2>/dev/null) || workers=4
if [ "$workers" -gt 8 ]; then workers=8; fi
if [ "$workers" -lt 1 ]; then workers=1; fi
active=0
source_count=0
while IFS= read -r source; do
    case "$source" in
        /*) ;;
        *) source="$build/$source" ;;
    esac
    # Generated sources may not exist: tooling preparation only configures CMake.
    case "$source" in
        "$project_root"/src/*) ;;
        *) continue ;;
    esac
    directory=$(CDPATH= cd -P "$(dirname "$source")" && pwd) || { status=1; continue; }
    source="$directory/$(basename "$source")"
    case "$source" in
        "$project_root"/src/*)
            source_count=$((source_count + 1))
            if [ "$fix" = true ]; then
                printf '\nLinting: %s\n' "$source"
                "$tidy" -p "$build" --fix "$source" || status=1
            else
                # Serialize completed diagnostics so file blocks stay intact.
                (
                    tidy_status=0
                    "$tidy" -p "$build" "$source" > "$work/$source_count.log" 2>&1 || tidy_status=$?
                    while ! mkdir "$work/output.lock" 2>/dev/null; do sleep 0.1; done
                    printf '\nLinting: %s\n' "$source"
                    while IFS= read -r line || [ -n "$line" ]; do
                        printf '%s\n' "$line"
                    done < "$work/$source_count.log"
                    rmdir "$work/output.lock"
                    exit "$tidy_status"
                ) &
                pids="$pids $!"
                active=$((active + 1))
                if [ "$active" -ge "$workers" ]; then
                    for pid in $pids; do wait "$pid" || status=1; done
                    pids=""
                    active=0
                fi
            fi
            ;;
    esac
done < "$source_list"
for pid in $pids; do wait "$pid" || status=1; done
pids=""
if [ "$source_count" -eq 0 ]; then
    printf '%s\n' "No repository src/ files were found in $build/compile_commands.json. Run sh scripts/build.ps1 -clang first." >&2
    exit 1
fi
# Formatting modifies files, so it belongs only to sequential fix mode.
if [ "$fix" = true ]; then
    if [ "$selected" = true ]; then
        while IFS= read -r source; do
            clang-format -i "$source" || status=1
        done < "$source_list"
    else
        find "$project_root/src" -type f \( -iname '*.hpp' -o -iname '*.cpp' \) -exec clang-format -i {} + || status=1
    fi
fi
exit "$status" #>

$ErrorActionPreference = "Stop"
trap {
    Write-Error $_ -ErrorAction Continue
    Write-Host 'linting failed'
    exit 1
}
$fix = $false
$changed = $false
$quiet = $false
$requestedPaths = @()
foreach ($argument in $args) {
    switch ($argument) {
        "-fix" {
            if ($fix) {
                throw "Usage: ./scripts/lint.ps1 [-fix] [-changed] [-quiet] [source.cpp ...]"
            }
            $fix = $true
        }
        "-changed" {
            if ($changed) {
                throw '-changed may only be provided once.'
            }
            $changed = $true
        }
        "-quiet" {
            if ($quiet) {
                throw '-quiet may only be provided once.'
            }
            $quiet = $true
        }
        default {
            if ($argument.StartsWith('-')) {
                throw "Unknown argument: $argument. Usage: ./scripts/lint.ps1 [-fix] [-changed] [-quiet] [source.cpp ...]"
            }
            $requestedPaths += $argument
        }
    }
}

# Use a separate PowerShell process so the failure trap's exit code cannot be
# swallowed by nested script invocation. Capture the whole run before replaying.
if ($quiet) {
    $log = [System.IO.Path]::GetTempFileName()
    $lintArguments = @($args | Where-Object { $_ -ne '-quiet' })
    $status = 1
    try {
        & (Join-Path $PSHOME 'pwsh.exe') -NoProfile -File $PSCommandPath @lintArguments *> $log
        $status = $LASTEXITCODE
        if ($status -ne 0) {
            Get-Content -LiteralPath $log | Out-Host
        } else {
            Write-Host 'linting passed'
        }
    } catch {
        Get-Content -LiteralPath $log | Where-Object { $_ -ne 'linting failed' } | Out-Host
        throw
    } finally {
        Remove-Item -LiteralPath $log -Force
    }
    exit $status
}

$root = Split-Path $PSScriptRoot -Parent
$changedFiles = @()
$changedHeader = $false
if ($changed) {
    $gitOutput = @(& git -C $root diff --no-relative --name-only --diff-filter=ACMR -z HEAD --)
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not find changed tracked files.'
    }
    $gitOutput += @(& git -C $root ls-files --others --exclude-standard --full-name -z)
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not find untracked files.'
    }
    foreach ($record in $gitOutput) {
        foreach ($path in $record.Split([char]0)) {
            if (!$path -or $path -notmatch '^src/.*\.(cpp|cc|cxx|h|hh|hpp|hxx)$') {
                continue
            }
            $file = [System.IO.Path]::GetFullPath((Join-Path $root $path))
            if (!(Test-Path -LiteralPath $file -PathType Leaf)) {
                continue
            }
            $changedFiles += $file
            if ($path -match '\.(h|hh|hpp|hxx)$') {
                $changedHeader = $true
            }
        }
    }
    if ($changedFiles.Count -eq 0 -and $requestedPaths.Count -eq 0) {
        Write-Host 'No changed repository C++ sources or headers; nothing to lint.'
        Write-Host 'linting passed'
        exit 0
    }
}
& (Join-Path $PSScriptRoot 'build.ps1') -clang
if ($LASTEXITCODE -ne 0) {
    throw 'Clang tooling preparation failed.'
}
$tidy = Join-Path $env:ProgramFiles 'LLVM/bin/clang-tidy.exe'
$build = Join-Path $root "build/clang"
if (!(Test-Path -LiteralPath $build -PathType Container) -or !(Get-ChildItem -LiteralPath $build -Force | Select-Object -First 1)) {
    throw "Build directory is missing or empty: $build. Run scripts/build.ps1 -clang first."
}
$build = (Resolve-Path -LiteralPath $build).Path
$database = Join-Path $build "compile_commands.json"
if (!(Test-Path -LiteralPath $database -PathType Leaf)) {
    throw "Compilation database not found: $database. Run scripts/build.ps1 -clang first."
}
try {
    $entries = Get-Content -LiteralPath $database -Raw | ConvertFrom-Json -NoEnumerate
} catch {
    throw "Could not parse compilation database ${database}: $($_.Exception.Message)"
}
if ($entries -isnot [array] -or $entries.Count -eq 0) {
    throw "Compilation database must be a nonempty array: $database. Run scripts/build.ps1 -clang first."
}
$files = @()
$sourceRoot = [System.IO.Path]::GetFullPath((Join-Path $root "src")) + [System.IO.Path]::DirectorySeparatorChar
foreach ($entry in $entries) {
    if ($entry.file -isnot [string] -or !$entry.file -or $entry.directory -isnot [string] -or !$entry.directory) {
        throw "Invalid compilation database entry in ${database}: expected file and directory strings."
    }
    $directory = $entry.directory
    if (![System.IO.Path]::IsPathRooted($directory)) {
        $directory = Join-Path $build $directory
    }
    $file = $entry.file
    if (![System.IO.Path]::IsPathRooted($file)) {
        $file = Join-Path $directory $file
    }
    $file = [System.IO.Path]::GetFullPath($file)
    if ($file.StartsWith($sourceRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        $files += $file
    }
}
$files = @($files | Sort-Object -Unique)
if ($files.Count -eq 0) {
    throw "No repository src/ files were found in $database. Run scripts/build.ps1 -clang first."
}
# PowerShell does not expand native-style file globs, so expand them here.
$selectedFiles = @()
if ($requestedPaths.Count -gt 0) {
    foreach ($path in $requestedPaths) {
        $matches = @(Get-ChildItem -Path $path -File)
        if ($matches.Count -eq 0) {
            throw "Source file not found: $path"
        }
        foreach ($match in $matches) {
            $file = $match.FullName
            if ($files -notcontains $file) {
                throw "No repository compilation database entry for: $file (pass compiled source files, not headers)"
            }
            $selectedFiles += $file
        }
    }
}
if ($changed -and !$changedHeader) {
    foreach ($file in $changedFiles) {
        if ($files -contains $file) {
            $selectedFiles += $file
        } else {
            Write-Host "Skipping changed source without a compilation database entry: $file"
        }
    }
}
if (!$changedHeader -and ($changed -or $requestedPaths.Count -gt 0)) {
    $files = @($selectedFiles | Sort-Object -Unique)
    if ($files.Count -eq 0) {
        Write-Host 'No selected compiled sources; nothing to lint.'
        Write-Host 'linting passed'
        exit 0
    }
}
$commands = @('clang-tidy')
if ($fix) {
    $commands += 'clang-format'
}
foreach ($command in $commands) {
    if (!(Get-Command $command -ErrorAction SilentlyContinue)) {
        throw "$command is required. Run scripts/setup.ps1 and open a new shell."
    }
}

# build.ps1 already initialized or reused the MSVC environment above.

# clang-tidy discovers the shared .clang-tidy configuration automatically.
# --fix is not a semantic-safety guarantee: review changes and rebuild.
$status = 0
if ($fix) {
    try {
        foreach ($file in $files) {
            Write-Host "`nLinting: $file"
            & $tidy -p $build --fix $file
            if ($LASTEXITCODE -ne 0) {
                $status = 1
            }
        }
    } finally {
        $formatFiles = $files
        if ($changedHeader -or (!$changed -and $requestedPaths.Count -eq 0)) {
            $formatFiles = @(Get-ChildItem -Path (Join-Path $root "src") -Recurse -Include *.hpp, *.cpp -File | Select-Object -ExpandProperty FullName)
        }
        foreach ($file in $formatFiles) {
            & clang-format -i $file
            if ($LASTEXITCODE -ne 0) {
                $status = 1
            }
        }
    }
} else {
    $workers = [Math]::Max(1, [Math]::Min(8, [Environment]::ProcessorCount))
    $files | ForEach-Object -Parallel {
        $file = $_
        $exitCode = 1
        $output = ''
        try {
            $output = (& $using:tidy -p $using:build $file 2>&1 | Out-String)
            $exitCode = $LASTEXITCODE
        } catch {
            $output += "$_`n"
        }
        [pscustomobject]@{ File = $file; Output = $output; ExitCode = $exitCode }
    } -ThrottleLimit $workers | ForEach-Object {
        Write-Host "`nLinting: $($_.File)"
        Write-Host $_.Output -NoNewline
        if ($_.ExitCode -ne 0) {
            $status = 1
        }
    }
}
if ($status -ne 0) {
    throw 'Lint failed.'
}
Write-Host 'linting passed'
