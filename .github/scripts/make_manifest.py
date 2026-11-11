import argparse
import hashlib
import json
import os
import sys

parser = argparse.ArgumentParser()
parser.add_argument("--side", required=True, choices=["switch", "client"])
parser.add_argument("--tag", required=True)
parser.add_argument("--version", required=True)
parser.add_argument("--repo", required=True)
parser.add_argument("--prev", required=True)
parser.add_argument("--assets", required=True)
parser.add_argument("--windows-id")
parser.add_argument("--linux-id")
parser.add_argument("--module-id")
parser.add_argument("--manager-id")
args = parser.parse_args()

v = args.version
required = {
    "switch": {"--module-id": args.module_id, "--manager-id": args.manager_id},
    "client": {"--windows-id": args.windows_id, "--linux-id": args.linux_id},
}[args.side]
missing = [name for name, value in required.items() if not value]
if missing:
    parser.error(f"--side {args.side} requires " + ", ".join(missing))
if args.tag != f"{args.side}-v{v}":
    parser.error(f"--tag must be {args.side}-v{v}")

base = f"https://github.com/{args.repo}/releases/download/{args.tag}"

with open(args.prev, encoding="utf-8") as f:
    prev = json.load(f).get("components", {})


def entry(name, asset):
    path = os.path.join(args.assets, asset)
    with open(path, "rb") as f:
        data = f.read()
    return {
        "name": name,
        "url": f"{base}/{asset}",
        "sha256": hashlib.sha256(data).hexdigest(),
        "size": len(data),
    }


spec = {
    "switch": {
        "switch-sysmodule": (args.module_id, [
            ("exefs.nsp", f"NXStream-{v}-switch-exefs.nsp"),
            ("toolbox.json", f"NXStream-{v}-switch-toolbox.json"),
            ("nxstream.id", f"NXStream-{v}-switch-nxstream.id"),
        ]),
        "switch-manager": (args.manager_id, [
            ("nxstream.nro", f"NXStream-{v}-switch-nxstream.nro"),
        ]),
    },
    "client": {
        "windows": (args.windows_id, [
            ("NXStream.exe", f"NXStream-{v}-windows-x64.exe"),
        ]),
        "linux": (args.linux_id, [
            ("NXStream.AppImage", f"NXStream-{v}-linux-x86_64.AppImage"),
        ]),
    },
}

components = {}
for key, (cid, files) in spec[args.side].items():
    old = prev.get(key, {})
    version = old.get("version", v) if old.get("id") == cid else v
    components[key] = {"id": cid, "version": version, "files": [entry(n, a) for n, a in files]}

for other in spec["client" if args.side == "switch" else "switch"]:
    if other in prev:
        components[other] = prev[other]
    else:
        print(f"warning: no previous entry for {other}, omitted from the manifest", file=sys.stderr)

manifest = {"schema": 1, "release": v, "tag": args.tag, "components": components}
with open(os.path.join(args.assets, "nxstream-manifest.json"), "w", encoding="utf-8") as f:
    json.dump(manifest, f, indent=2)
    f.write("\n")
