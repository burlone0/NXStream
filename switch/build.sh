#!/bin/bash
set -e
export DEVKITPRO=/opt/devkitpro DEVKITA64=/opt/devkitpro/devkitA64
export PATH=/opt/devkitpro/tools/bin:/opt/devkitpro/devkitA64/bin:$PATH
root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -n "${2:-}" ]]; then export NXSTREAM_MODULE_ID="$2"; fi
if [[ -n "${3:-}" ]]; then export NXSTREAM_MANAGER_ID="$3"; fi
if [[ -n "${1:-}" ]]; then
    sys_version=(VERSION="$1")
    app_version=(APP_VERSION="$1")
else
    sys_version=()
    app_version=()
fi
make -C "$root/manager" -j8 "${app_version[@]}"
make -C "$root/sysmodule" -j8 "${sys_version[@]}" dist
bash "$root/tap/build.sh"
find "$root/dist" -type f
