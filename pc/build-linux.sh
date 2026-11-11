#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(dirname "$root")"
build="$root/build/linux"
appdir="$build/NXStream.AppDir"
cache="$build/cache"
out="$root/dist-linux"
bundle_ffmpeg=0
version_args=()
content_args=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bundle-ffmpeg) bundle_ffmpeg=1 ;;
        --version)
            [[ $# -ge 2 ]] || { echo "error: --version needs a value" >&2; exit 1; }
            version_args=("-p:Version=$2")
            shift
            ;;
        --content-id)
            [[ $# -ge 2 ]] || { echo "error: --content-id needs a value" >&2; exit 1; }
            content_args=("-p:ContentId=$2")
            shift
            ;;
        *) echo "error: unknown argument: $1" >&2; exit 1 ;;
    esac
    shift
done

fail() { echo "error: $*" >&2; exit 1; }

find_lib() {
    local path
    path="$(ldconfig -p | awk -v n="$1" '$1 == n && /x86-64/ { print $NF; exit }')"
    [[ -n "$path" ]] || fail "$1 not found (install libsdl2-2.0-0, libsdl2-image-2.0-0, libsdl2-ttf-2.0-0 and libusb-1.0-0)"
    readlink -f "$path"
}

mkdir -p "$cache"
rm -rf "$appdir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/lib" "$appdir/usr/share/icons/hicolor/256x256/apps" "$appdir/usr/share/nxstream/fonts"

dotnet publish "$root/NXStream.Linux/NXStream.Linux.csproj" -c Release -o "$build/publish" "${version_args[@]}" "${content_args[@]}"
[[ -f "$build/publish/nxstream" ]] || fail "publish did not produce nxstream"
install -m 755 "$build/publish/nxstream" "$appdir/usr/bin/nxstream"

install -m 755 "$root/packaging/linux/AppRun" "$appdir/AppRun"
install -m 644 "$root/packaging/linux/nxstream.desktop" "$appdir/nxstream.desktop"
install -m 644 "$repo/pc/packaging/linux/nxstream.png" "$appdir/nxstream.png"
install -m 644 "$repo/pc/packaging/linux/nxstream.png" "$appdir/usr/share/icons/hicolor/256x256/apps/nxstream.png"

install -m 644 "$root"/packaging/linux/fonts/* "$appdir/usr/share/nxstream/fonts/"

for soname in libSDL2-2.0.so.0 libSDL2_image-2.0.so.0 libSDL2_ttf-2.0.so.0 libusb-1.0.so.0; do
    install -m 755 "$(find_lib "$soname")" "$appdir/usr/lib/$soname"
done

if [[ $bundle_ffmpeg -eq 1 ]]; then
    ffmpeg_path="$(command -v ffmpeg || true)"
    if [[ -n "$ffmpeg_path" ]] && { ldd "$ffmpeg_path" 2>&1 | grep -qE "not a dynamic executable|statically linked" || ! ldd "$ffmpeg_path" 2>/dev/null | awk '{print $1}' | grep -vE '^(linux-vdso\.so|libm\.so|libdl\.so|librt\.so|libpthread\.so|libmvec\.so|libc\.so|libgcc_s\.so|/lib64/ld-linux-x86-64\.so)' | grep -q .; }; then
        install -m 755 "$ffmpeg_path" "$appdir/usr/bin/ffmpeg"
    else
        tarball="$cache/ffmpeg-release-amd64-static.tar.xz"
        if [[ ! -f "$tarball" ]]; then
            curl -fL --connect-timeout 20 --retry 3 -o "$tarball" "https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-amd64-static.tar.xz"
        fi
        extract="$cache/ffmpeg-extract"
        rm -rf "$extract"
        mkdir -p "$extract"
        tar -xf "$tarball" -C "$extract" --wildcards '*/ffmpeg' --strip-components=1
        [[ -f "$extract/ffmpeg" ]] || fail "ffmpeg not found in $tarball"
        install -m 755 "$extract/ffmpeg" "$appdir/usr/bin/ffmpeg"
    fi
fi

tool="$(command -v appimagetool || true)"
if [[ -z "$tool" ]]; then
    tool="$cache/appimagetool-1.9.1-x86_64.AppImage"
    if [[ ! -x "$tool" ]]; then
        curl -fL --retry 3 -o "$tool.part" "https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage"
        echo "ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0  $tool.part" | sha256sum -c - || { rm -f "$tool.part"; fail "appimagetool checksum mismatch"; }
        chmod +x "$tool.part"
        mv "$tool.part" "$tool"
    fi
fi

mkdir -p "$out"
rm -f "$out/NXStream-x86_64.AppImage"
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$tool" "$appdir" "$out/NXStream-x86_64.AppImage"
echo "built $out/NXStream-x86_64.AppImage"
