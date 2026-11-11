#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/../.."

content_id() {
    local path
    for path in "$@"; do
        printf '%s %s\n' "$path" "$(git rev-parse "HEAD:$path")"
    done | sha256sum | cut -c1-20
}

echo "windows_id=$(content_id pc/NXStream pc/build-windows.ps1)"
echo "linux_id=$(content_id pc/NXStream pc/NXStream.Linux pc/packaging pc/build-linux.sh)"
echo "module_id=$(content_id switch/sysmodule switch/build.sh)"
echo "manager_id=$(content_id switch/manager)"
