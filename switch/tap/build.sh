#!/bin/bash
set -e
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}" DEVKITA64="${DEVKITA64:-/opt/devkitpro/devkitA64}"
export PATH="$DEVKITPRO/tools/bin:$DEVKITA64/bin:$PATH"
tap="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$tap/.." && pwd)"
tag="${NXTAP_AMS_TAG:-1.12.0}"
libnx_rev=b156b4774fb2df7b93402169509266da7547fc4c
ams="$root/build/cache/Atmosphere-$tag-${libnx_rev:0:8}"
dist="$root/dist/atmosphere/contents"
mitm_id=4200000000004E54
donor_id=4200000000004E55

if [ ! -f "$ams/libraries/libstratosphere/libstratosphere.mk" ]; then
    rm -rf "$ams"
    mkdir -p "$(dirname "$ams")"
    git clone --quiet --depth 1 --branch "$tag" https://github.com/Atmosphere-NX/Atmosphere "$ams"
fi

sdk="$root/build/cache/devkitpro-libnx-${libnx_rev:0:8}"
src="$root/build/cache/libnx-src"
if [ ! -f "$sdk/libnx/lib/libnx.a" ]; then
    rm -rf "$sdk" "$src"
    mkdir -p "$sdk/libnx"
    for entry in "$DEVKITPRO"/*; do
        [ "$(basename "$entry")" = libnx ] || ln -s "$entry" "$sdk/"
    done
    git clone --quiet https://github.com/switchbrew/libnx "$src"
    git -C "$src" checkout --quiet "$libnx_rev"
    make -C "$src/nx" -j"$(nproc)" lib/libnx.a lib/libnxd.a >/dev/null
    (cd "$src/nx" && cp default_icon.jpg switch_rules switch.ld switch.specs "$sdk/libnx/" && cp -r include lib "$sdk/libnx/" && cp -r external/bsd/include/. "$sdk/libnx/include/")
fi
export DEVKITPRO="$sdk"

python3 "$tap/applet-mitm/patch_libstrat.py" "$ams"

dst="$ams/stratosphere/applet-mitm"
rm -rf "$dst"
mkdir -p "$dst"
cp -r "$tap/applet-mitm/source" "$tap/applet-mitm/Makefile" "$tap/applet-mitm/system_module.mk" "$tap/applet-mitm/applet-mitm.json" "$dst/"
make -C "$dst" -j"$(nproc)" nx_release

rm -rf "$tap/donor/build"
make -C "$tap/donor"

rm -rf "$dist/$mitm_id" "$dist/$donor_id"
mkdir -p "$dist/$mitm_id" "$dist/$donor_id"
cp "$dst/out/nintendo_nx_arm64_armv8a/release/applet-mitm.nsp" "$dist/$mitm_id/exefs.nsp"
cp "$tap/donor/sft-donor.nsp" "$dist/$donor_id/exefs.nsp"
cat > "$dist/$donor_id/toolbox.json" <<JSON
{
    "name": "NXStream Native",
    "tid": "$donor_id",
    "requires_reboot": true
}
JSON
for id in $mitm_id $donor_id; do
    cp "$tap/LICENSE" "$dist/$id/LICENSE.txt"
    cp "$tap/NOTICE" "$dist/$id/NOTICE.txt"
done
rm -rf "$tap/donor/build" "$tap/donor/sft-donor".{elf,nso,npdm,nsp,map}
