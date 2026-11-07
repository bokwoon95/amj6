#!/bin/sh
Where-Object "\"<#" 2>/dev/null

# One file supports sh and PowerShell. The preamble opens a PowerShell block
# comment around the Unix section; sh exits before reaching the Windows code.
# The dummy Where-Object command is only meaningful to PowerShell; sh hides
# its expected error. Keep the preamble paired with the comment-ending exit line.
# Usage: sh scripts/setup.ps1 on macOS/Linux; ./scripts/setup.ps1 on Windows.
# This installs build/analysis tools, NOT the project's Qt libraries. CMake
# asks vcpkg to install dependencies declared in vcpkg.json during configuration.
# Reruns reuse installed tools and refuse to overwrite unrelated files.

# Stop on failed commands (-e) and accidental unset variables (-u).
set -eu
brew_path_missing=false
llvm_bin=""
# Expose only clang-tidy and clang-format, not LLVM's compiler/linker binaries.
# mkdir -p accepts an existing directory; ':' delimiters prevent partial PATH
# matches (e.g. ~/.local/bin-other is not the same as ~/.local/bin).
user_bin="$HOME/.local/bin"
mkdir -p "$user_bin"
user_bin_path_missing=false
case ":$PATH:" in
    *":$user_bin:"*) ;;
    *) user_bin_path_missing=true ;;
esac

case "$(uname -s)" in
    Darwin)
        # Apple's tools provide the build compiler, SDK, and sanitizer runtimes.
        # Installation is interactive: exit and let the user finish before retrying.
        if ! xcode-select -p >/dev/null 2>&1 || ! xcrun clang --version >/dev/null 2>&1; then
            xcode-select --install
            printf '%s\n' 'Complete the Command Line Tools installer, then rerun this script.'
            exit 1
        fi

        # Homebrew may be installed but absent from this process's PATH.
        # Apple Silicon normally uses /opt/homebrew; Intel uses /usr/local.
        # shellenv prints shell assignments; eval applies them in this process.
        # Record a persistent shellenv line later so new shells also find brew.
        if ! command -v brew >/dev/null 2>&1; then
            brew_path_missing=true
            if [ -x /opt/homebrew/bin/brew ]; then
                eval "$(/opt/homebrew/bin/brew shellenv)"
            elif [ -x /usr/local/bin/brew ]; then
                eval "$(/usr/local/bin/brew shellenv)"
            else
                /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
                if [ -x /opt/homebrew/bin/brew ]; then
                    eval "$(/opt/homebrew/bin/brew shellenv)"
                else
                    eval "$(/usr/local/bin/brew shellenv)"
                fi
            fi
        fi

        # CMake configures builds, Ninja executes them, and pkg-config locates
        # dependency metadata. --yes avoids Homebrew's dependency prompts.
        for package in cmake ninja pkg-config jq; do
            if ! command -v "$package" >/dev/null 2>&1; then
                brew install --yes "$package"
            fi
        done
        # Apple Clang supplies sanitizers, but clang-tidy comes from Homebrew LLVM.
        # Use the stable Homebrew opt path, not a versioned Cellar directory, so
        # tool symlinks survive upgrades. Never add this whole bin directory to PATH.
        if ! brew list --versions llvm >/dev/null 2>&1; then
            brew install --yes llvm
        fi
        llvm_bin="$(brew --prefix llvm)/bin"
        analysis_bin="$llvm_bin"
        # libb2 (a Qt dependency) uses Autotools. autoconf-archive has no executable,
        # and Homebrew names GNU libtool's executable glibtoolize, so check formulas.
        for package in autoconf autoconf-archive automake libtool; do
            if ! brew list --versions "$package" >/dev/null 2>&1; then
                brew install --yes "$package"
            fi
        done
        # Apple Command Line Tools supply Git as well as Clang.
        git --version >/dev/null
        login_shell=$(dscl . -read "/Users/$(id -un)" UserShell | awk '{print $2}')
        ;;
    Linux)
        # These commands/package names target Debian-family apt systems only.
        # build-essential supplies GCC/G++, make, and development headers;
        # the compiler packages pull in their normal sanitizer runtime dependencies.
        . /etc/os-release
        case "$ID" in
            ubuntu|debian) ;;
            *) printf '%s\n' 'Only Ubuntu and Debian are supported on Linux.' >&2; exit 1 ;;
        esac

        packages=""
        if ! command -v gcc >/dev/null 2>&1 || ! command -v g++ >/dev/null 2>&1 || ! command -v make >/dev/null 2>&1 || ! dpkg-query -W -f='${Status}' build-essential 2>/dev/null | grep -q 'install ok installed'; then
            packages="build-essential"
        fi
        for package in git cmake pkg-config curl zip unzip tar jq; do
            if ! command -v "$package" >/dev/null 2>&1; then
                packages="$packages $package"
            fi
        done
        # Check the real system binaries, not our user-bin symlinks, which may
        # exist but be broken. The unversioned distro packages provide these paths.
        for package in clang clang-format clang-tidy; do
            if [ ! -x "/usr/bin/$package" ]; then
                packages="$packages $package"
            fi
        done
        analysis_bin=/usr/bin
        if ! command -v ninja >/dev/null 2>&1; then
            packages="$packages ninja-build"
        fi
        if [ -n "$packages" ]; then
            # Batch missing packages into one apt transaction. root needs no sudo.
            # Package names above are fixed literals; split them into apt arguments.
            if [ "$(id -u)" -eq 0 ]; then
                apt-get update
                apt-get install -y $packages
            else
                sudo apt-get update
                sudo apt-get install -y $packages
            fi
        fi
        login_shell=$(getent passwd "$(id -un)" | cut -d: -f7)
        ;;
    *) printf '%s\n' 'Unsupported OS. On Windows, run this file with PowerShell.' >&2; exit 1 ;;
esac

# Publish only the two tools in the user bin directory. Keep installed compilers
# in their original locations so other projects do not silently switch toolchains.
# An already-correct symlink is a no-op. -L also detects broken symlinks, which
# -e alone misses. Never overwrite an existing file or an unrelated symlink.
for tool in clang-tidy clang-format; do
    target="$analysis_bin/$tool"
    link="$user_bin/$tool"
    if [ ! -x "$target" ]; then
        printf '%s\n' "Analysis tool is missing: $target" >&2
        exit 1
    fi
    if [ -L "$link" ] && [ "$(readlink "$link")" = "$target" ]; then
        continue
    fi
    if [ -e "$link" ] || [ -L "$link" ]; then
        printf '%s\n' "Refusing to overwrite $link. Move it aside and rerun setup." >&2
        exit 1
    fi
    ln -s "$target" "$link"
done

# Only choose/edit a startup file when needed (including the old LLVM PATH
# migration on macOS). Use the account's login shell, not the /bin/sh executing
# this script. zsh honors ZDOTDIR; Bash needs both interactive and login startup.
if [ "$brew_path_missing" = true ] || [ "$user_bin_path_missing" = true ] || [ -n "$llvm_bin" ] || ! command -v vcpkg >/dev/null 2>&1; then
    case "$login_shell" in
        */zsh)
            shell_rc="${ZDOTDIR:-$HOME}/.zshrc"
            ;;
        */bash)
            shell_rc="$HOME/.bashrc"
            # Bash login shells read the first existing login profile, not .bashrc.
            login_profile="$HOME/.bash_profile"
            if [ ! -f "$login_profile" ]; then
                if [ -f "$HOME/.bash_login" ]; then
                    login_profile="$HOME/.bash_login"
                elif [ -f "$HOME/.profile" ]; then
                    login_profile="$HOME/.profile"
                fi
            fi
            if [ ! -f "$login_profile" ] || ! grep -Eq '(^|[[:space:];])(\.|source)[[:space:]]+.*\.bashrc' "$login_profile"; then
                printf '\n%s\n' '[ -f "$HOME/.bashrc" ] && . "$HOME/.bashrc"' >> "$login_profile"
            fi
            ;;
        *)
            printf '%s\n' "Unsupported login shell: $login_shell. Configure Homebrew/vcpkg PATH manually." >&2
            exit 1
            ;;
    esac
fi

# Single-quoted assignment text keeps startup-file variables/commands deferred
# until the NEW shell runs. grep -Fqx checks an exact whole line to avoid adding
# our same startup command twice on reruns.
if [ "$brew_path_missing" = true ]; then
    brew_entry='eval "$('"$(command -v brew)"' shellenv)"'
    if [ ! -f "$shell_rc" ] || ! grep -Fqx "$brew_entry" "$shell_rc"; then
        printf '\n%s\n' "$brew_entry" >> "$shell_rc"
    fi
fi

# Remove only the exact LLVM PATH line written by older versions of this script.
if [ -n "$llvm_bin" ]; then
    llvm_entry='export PATH="'"$llvm_bin"':$PATH"'
    if [ -f "$shell_rc" ] && grep -Fqx "$llvm_entry" "$shell_rc"; then
        # Filter into a temporary file, then copy back to preserve the original
        # startup file's permissions. Leave all other user-written lines intact.
        temporary_rc=$(mktemp "$shell_rc.XXXXXX")
        awk -v old="$llvm_entry" '$0 != old' "$shell_rc" > "$temporary_rc"
        cp "$temporary_rc" "$shell_rc"
        rm -f "$temporary_rc"
    fi
    # Also remove it from this process without sourcing the user's shell config:
    # that config may have interactive commands or unrelated side effects.
    # awk splits PATH on ':' and removes exact directory matches, not substrings.
    PATH=$(printf '%s' "$PATH" | awk -v old="$llvm_bin" 'BEGIN { RS=":"; ORS="" } $0 != old { if (count++) printf ":"; printf "%s", $0 }')
    export PATH
fi

# Persist the directory for future shells AND export it now for later setup
# commands. A script launched via sh cannot update the invoking parent's PATH;
# unlike sourcing a script, its environment changes stay in the child process.
if [ "$user_bin_path_missing" = true ]; then
    path_entry='export PATH="$HOME/.local/bin:$PATH"'
    if [ ! -f "$shell_rc" ] || ! grep -Fqx "$path_entry" "$shell_rc"; then
        printf '\n%s\n' "$path_entry" >> "$shell_rc"
    fi
    export PATH="$user_bin:$PATH"
    printf '%s\n' "Added $user_bin to $shell_rc and this process. Open a new shell to update your parent shell."
fi

# vcpkg is a reusable user-wide checkout, separate from this repository's build
# directories. Bootstrap installs its command, not Qt; CMake handles libraries.
# Refuse to treat an unrelated ~/vcpkg directory as disposable setup output.
if ! command -v vcpkg >/dev/null 2>&1; then
    vcpkg_root="$HOME/vcpkg"
    if [ ! -e "$vcpkg_root" ]; then
        git clone --depth 1 https://github.com/microsoft/vcpkg.git "$vcpkg_root"
    elif [ ! -d "$vcpkg_root/.git" ] || [ ! -f "$vcpkg_root/bootstrap-vcpkg.sh" ]; then
        printf '%s\n' "$vcpkg_root already exists and is not a vcpkg checkout; leaving it untouched." >&2
        exit 1
    fi
    # Standard bootstrap downloads the executable matching this checkout.
    if [ ! -x "$vcpkg_root/vcpkg" ]; then
        "$vcpkg_root/bootstrap-vcpkg.sh" -disableMetrics
    fi
    "$vcpkg_root/vcpkg" version
    path_entry='export PATH="$HOME/vcpkg:$PATH"'
    if [ ! -f "$shell_rc" ] || ! grep -Fqx "$path_entry" "$shell_rc"; then
        printf '\n%s\n' "$path_entry" >> "$shell_rc"
    fi
    export PATH="$vcpkg_root:$PATH"
    printf '%s\n' "Added vcpkg to $shell_rc. Open a new shell to pick up the change."
fi

printf '%s\n' 'Setup complete.'
exit #>

# WINDOWS SECTION: PowerShell skips the Unix block and resumes here.
# Stop catches PowerShell failures; native executables require explicit checks
# of $LASTEXITCODE. Environment assignments below affect this PowerShell process.
$ErrorActionPreference = "Stop"
if ($env:OS -ne "Windows_NT") {
    throw "On macOS/Linux, run this script with sh ./scripts/setup.ps1."
}

# Generic per-user tools directory, independent of this repository's name.
# Create it without needing admin privileges; only two LLVM wrappers go here.
$userBin = Join-Path $env:LOCALAPPDATA "bin"
[void](New-Item -Path $userBin -ItemType Directory -Force)
$llvmBin = Join-Path $env:ProgramFiles "LLVM\bin"

# Discover installations even if this session predates their PATH changes.
# These are known installer locations; WinGet Links also exposes packaged tools.
# Do not include LLVM bin: finding the linter must not expose a new compiler.
$toolDirectories = @("$env:ProgramFiles\Git\cmd", "$env:ProgramFiles\CMake\bin", "$env:LOCALAPPDATA\Microsoft\WinGet\Links", $userBin)
foreach ($directory in $toolDirectories) {
    if ((Test-Path $directory) -and ($env:Path -split ';' -notcontains $directory)) {
        $env:Path = "$directory;$env:Path"
    }
}

# Require Visual Studio 2026 (version 18) for Glaze's supported MSVC toolchain.
# Require ASan explicitly; it is an optional component.
# Check SDK headers too, since cl.exe alone cannot build a Windows application.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$visualStudio = ""
if (Test-Path $vswhere) {
    $visualStudio = & $vswhere -latest -version '[18.0,19.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.ASAN -property installationPath
}
$hasWindowsSDK = Test-Path "${env:ProgramFiles(x86)}\Windows Kits\10\Include\*\um\Windows.h"
if (!$visualStudio -or !$hasWindowsSDK) {
    if (!(Get-Command winget -ErrorAction SilentlyContinue)) {
        throw "Install Microsoft's App Installer (winget), then rerun setup."
    }
    # The C++ workload supplies compiler/libraries; includeRecommended supplies
    # its usual SDK/tools. --wait keeps us from checking while setup still runs.
    # Installation may need elevation; exit code 3010 means a restart is required.
    # This package installs VS 2026 alongside VS 2022; it does not replace it.
    & winget install --id Microsoft.VisualStudio.BuildTools --exact --source winget --accept-package-agreements --accept-source-agreements --override '--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.ASAN --includeRecommended'
    if ($LASTEXITCODE -eq 3010) {
        throw "Restart Windows to finish installing Build Tools, then rerun setup."
    }
    if ($LASTEXITCODE -ne 0) {
        throw "Visual Studio 2026 Build Tools installation failed (exit $LASTEXITCODE). If already installed, add the C++ workload and Windows SDK through Visual Studio Installer."
    }
    $visualStudio = & $vswhere -latest -version '[18.0,19.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.ASAN -property installationPath
    $hasWindowsSDK = Test-Path "${env:ProgramFiles(x86)}\Windows Kits\10\Include\*\um\Windows.h"
    if (!$visualStudio -or !$hasWindowsSDK) {
        throw "Visual Studio 2026 C++ Build Tools, AddressSanitizer, or the Windows SDK are still missing. Add these through Visual Studio Installer, then rerun setup."
    }
}
# The build script initializes MSVC's environment; do not permanently add cl.exe to PATH.

# Only install missing commands. Git fetches vcpkg, CMake configures the project,
# and Ninja is the build backend. LLVM is handled separately by absolute paths.
$packages = @(
    @{ Command = "git"; ID = "Git.Git" },
    @{ Command = "cmake"; ID = "Kitware.CMake" },
    @{ Command = "ninja"; ID = "Ninja-build.Ninja" },
    @{ Command = "jq"; ID = "jqlang.jq" }
)
foreach ($package in $packages) {
    if (!(Get-Command $package.Command -ErrorAction SilentlyContinue)) {
        if (!(Get-Command winget -ErrorAction SilentlyContinue)) {
            throw "Install Microsoft's App Installer (winget), then rerun setup."
        }
        & winget install --id $package.ID --exact --source winget --accept-package-agreements --accept-source-agreements
        if ($LASTEXITCODE -ne 0) {
            throw "$($package.ID) installation failed (exit $LASTEXITCODE)."
        }
        # Installers modify persisted PATH, not an already-running shell's copy.
        # Refresh now, then rediscover known tool locations before checking again.
        $env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [Environment]::GetEnvironmentVariable("Path", "User")
        foreach ($directory in $toolDirectories) {
            if ((Test-Path $directory) -and ($env:Path -split ';' -notcontains $directory)) {
                $env:Path = "$directory;$env:Path"
            }
        }
        if (!(Get-Command $package.Command -ErrorAction SilentlyContinue)) {
            throw "$($package.Command) is still unavailable. Open a new PowerShell session and rerun setup."
        }
    }
}

# Check the actual executables, not PATH: existing wrappers may be stale.
if (!(Test-Path -LiteralPath (Join-Path $llvmBin "clang-tidy.exe")) -or !(Test-Path -LiteralPath (Join-Path $llvmBin "clang-format.exe"))) {
    if (!(Get-Command winget -ErrorAction SilentlyContinue)) {
        throw "Install Microsoft's App Installer (winget), then rerun setup."
    }
    & winget install --id LLVM.LLVM --exact --source winget --accept-package-agreements --accept-source-agreements
    if ($LASTEXITCODE -ne 0) {
        throw "LLVM installation failed (exit $LASTEXITCODE)."
    }
}

# .cmd wrappers work without Administrator privileges or Developer Mode, unlike
# Windows symlinks. Quote the executable for spaces in "Program Files"; %* passes
# the caller's arguments through. The command's exit status propagates naturally.
# Reuse only byte-identical wrappers; do not replace another user's command.
foreach ($tool in @("clang-tidy", "clang-format")) {
    $executable = Join-Path $llvmBin "$tool.exe"
    if (!(Test-Path -LiteralPath $executable)) {
        throw "LLVM tool is missing: $executable"
    }
    $wrapper = Join-Path $userBin "$tool.cmd"
    $content = "@`"$executable`" %*`r`n"
    $existingWrapper = Get-Item -LiteralPath $wrapper -Force -ErrorAction SilentlyContinue
    if ($existingWrapper) {
        if ($existingWrapper.PSIsContainer -or $existingWrapper.LinkType -or (Get-Content -LiteralPath $wrapper -Raw) -cne $content) {
            throw "Refusing to overwrite $wrapper. Move it aside and rerun setup."
        }
    } else {
        Set-Content -LiteralPath $wrapper -Value $content -Encoding Ascii -NoNewline
    }
}

# Remove the full LLVM bin entry previously added to the user PATH. Preserve
# unrelated entries and their original spelling. Expand %VARIABLES% for matching,
# trim trailing backslashes, and use PowerShell's case-insensitive comparison.
# User PATH is persisted for future processes; machine PATH is shared system-wide
# and must not be rewritten by a per-user setup script.
$oldUserPath = [Environment]::GetEnvironmentVariable("Path", "User")
$userEntries = @()
foreach ($entry in ($oldUserPath -split ';')) {
    $expandedEntry = [Environment]::ExpandEnvironmentVariables($entry).Trim().TrimEnd('\')
    if ($entry -and $expandedEntry -ne $llvmBin.TrimEnd('\')) {
        $userEntries += $entry
    }
}
$userPath = $userEntries -join ';'
$machinePath = [Environment]::GetEnvironmentVariable("Path", "Machine")
# Add existing directories only if absent from BOTH persistent PATH scopes.
# Do not duplicate a machine-level entry in the user's registry value.
foreach ($directory in $toolDirectories) {
    $alreadyInPath = $false
    foreach ($entry in (("$machinePath;$userPath") -split ';')) {
        $expandedEntry = [Environment]::ExpandEnvironmentVariables($entry).Trim().TrimEnd('\')
        if ($expandedEntry -eq $directory.TrimEnd('\')) {
            $alreadyInPath = $true
            break
        }
    }
    if ((Test-Path -LiteralPath $directory) -and !$alreadyInPath) {
        if ($userPath) {
            $userPath = "$userPath;$directory"
        } else {
            $userPath = $directory
        }
    }
}
if ($userPath -ne $oldUserPath) {
    [Environment]::SetEnvironmentVariable("Path", $userPath, "User")
}

# Refresh this process immediately; no Chocolatey refreshenv dependency.
# Read machine + user values to emulate the environment of a newly launched shell.
# Expanding %VARIABLES% makes the process value directly usable for lookup.
# Leave machine-wide settings alone, but exclude LLVM bin from this process.
$processEntries = @()
foreach ($entry in (("$machinePath;$userPath") -split ';')) {
    $expandedEntry = [Environment]::ExpandEnvironmentVariables($entry).Trim().TrimEnd('\')
    if ($expandedEntry -eq $llvmBin.TrimEnd('\')) {
        Write-Warning "LLVM bin is still in the machine PATH. Remove it in Windows Environment Variables to stop exposing LLVM compilers in new shells."
    } elseif ($entry) {
        $processEntries += [Environment]::ExpandEnvironmentVariables($entry)
    }
}
$env:Path = $processEntries -join ';'

# Share one vcpkg checkout across projects. Only bootstrap/validate its executable;
# project dependencies are installed later by CMake using vcpkg.json.
if (!(Get-Command vcpkg -ErrorAction SilentlyContinue)) {
    $vcpkgRoot = Join-Path $HOME "vcpkg"
    if (!(Test-Path $vcpkgRoot)) {
        & git clone --depth 1 https://github.com/microsoft/vcpkg.git $vcpkgRoot
        if ($LASTEXITCODE -ne 0) {
            throw "Cloning vcpkg failed (exit $LASTEXITCODE)."
        }
    } elseif (!(Test-Path "$vcpkgRoot\.git") -or !(Test-Path "$vcpkgRoot\bootstrap-vcpkg.bat")) {
        throw "$vcpkgRoot already exists and is not a vcpkg checkout; leaving it untouched."
    }
    # Standard bootstrap downloads the executable matching this checkout.
    if (!(Test-Path "$vcpkgRoot\vcpkg.exe")) {
        & "$vcpkgRoot\bootstrap-vcpkg.bat" -disableMetrics
        if ($LASTEXITCODE -ne 0) {
            throw "Bootstrapping vcpkg failed (exit $LASTEXITCODE)."
        }
    }
    & "$vcpkgRoot\vcpkg.exe" version
    if ($LASTEXITCODE -ne 0) {
        throw "vcpkg validation failed (exit $LASTEXITCODE)."
    }
    if (($userPath -split ';' -notcontains $vcpkgRoot) -and ($machinePath -split ';' -notcontains $vcpkgRoot)) {
        if ($userPath) {
            $userPath = "$userPath;$vcpkgRoot"
        } else {
            $userPath = $vcpkgRoot
        }
        [Environment]::SetEnvironmentVariable("Path", $userPath, "User")
    }
    $env:Path = "$vcpkgRoot;$env:Path"
    Write-Host "Added vcpkg to your user PATH. Open a new shell to pick up the change."
}

# On Windows, vcpkg automatically acquires its own architecture-specific pkg-config.
Write-Host "Setup complete. vcpkg will acquire pkg-config when a port needs it."
