#!/usr/bin/env python3
"""Hard-link an image set, add appinst/signer CDHashes to owner trust, and update the manifest entry."""
import hashlib
import json
import os
import shutil
import sys
from pathlib import Path

if len(sys.argv) != 4:
    raise SystemExit("usage: prepare-image-set.py BASE_IMAGE_SET APPINST_BUILD OUT_IMAGE_SET")
base, build, out = map(Path, sys.argv[1:])
for p in (base / "manifest.json", base / "overlay/userland/owner-trust.json", build / "cdhashes.txt"):
    if not p.is_file():
        raise SystemExit(f"missing {p}")
if out.exists():
    shutil.rmtree(out)
shutil.copytree(base, out, copy_function=os.link, symlinks=True)

owner_path = out / "overlay/userland/owner-trust.json"
owner = json.loads(owner_path.read_text())
records = []
for line in (build / "cdhashes.txt").read_text().splitlines():
    fields = line.split()
    if not fields or len(fields[0]) != 40:
        continue
    cdhash = fields[0]
    if "id=com.oneios.appinst" in line:
        records.append((cdhash, "com.oneios.appinst", "/private/var/oneios/bin/appinst"))
    elif "id=org.unixnu.ldid" in line:
        records.append((cdhash, "org.unixnu.ldid", "/private/var/oneios/bin/libunixnu-ldid.dylib"))
if len(records) != 2:
    raise SystemExit("cdhashes.txt does not name appinst and org.unixnu.ldid")
known = set(owner.get("cdhashes", []))
files = owner.setdefault("files", [])
for cdhash, identifier, path in records:
    known.add(cdhash)
    files = [f for f in files if f.get("path") != path]
    files.append({"path": path, "identifier": identifier, "cdhash": cdhash, "arch": "arm64e",
                  "page_size": 4096, "hash": "sha256", "adhoc": True, "warnings": []})
owner["cdhashes"] = sorted(known)
owner["files"] = sorted(files, key=lambda f: f.get("path", ""))
owner_path.write_text(json.dumps(owner, indent=2, sort_keys=True) + "\n")

manifest_path = out / "manifest.json"
manifest = json.loads(manifest_path.read_text())
raw = owner_path.read_bytes()
found = False
for item in manifest.get("files", []):
    if item.get("path") == "overlay/userland/owner-trust.json":
        item["size"] = len(raw)
        item["sha256"] = hashlib.sha256(raw).hexdigest()
        found = True
if not found:
    raise SystemExit("manifest has no owner-trust entry")
manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
print(f"{out}: owner trust {len(owner['cdhashes'])} CDHashes; appinst and signer added")
