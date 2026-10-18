"""
Compares fPKGEx's own extraction against Sony's prospero-pub-cmd output.

Verifies that every file Sony produces is reproduced byte-identically by the
native parser, and reports any extras.
"""
import hashlib
import os
import subprocess
import sys

SONY_EXE = r"D:\- PS5\FPKG SDK\toolchain\prospero-pub-cmd.exe"
OUR_EXE = r"D:\OpenCode\fPKGEx\tests\_fpkg_test.exe"
SAMPLE = r"D:\- PPSA17221\UP4433-PPSA17221_00-MINECRAFTPS50000-A0144-V0144.pkg"
OUR_OUT = os.path.join(os.environ["TEMP"], "fpkgex_out")


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def snapshot(root):
    out = {}
    for dirpath, _dirs, files in os.walk(root):
        for name in files:
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root).replace("\\", "/")
            out[rel] = (os.path.getsize(full), digest(full))
    return out


def main():
    sony = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.environ["TEMP"], "pkgroot")
    if not os.path.isdir(sony):
        print(f"Sony reference tree not found: {sony}")
        print("Run prospero-pub-cmd img_extract first.")
        return 2

    ref = snapshot(sony)
    ours = snapshot(OUR_OUT)

    print(f"Sony files : {len(ref)}")
    print(f"fPKGEx files: {len(ours)}")
    print()

    missing, mismatched, extra = [], [], []
    for rel, (size, dgst) in sorted(ref.items()):
        if rel not in ours:
            missing.append(rel)
        elif ours[rel] != (size, dgst):
            mismatched.append((rel, size, ours[rel][0]))
        else:
            print(f"  MATCH  {rel}")
    for rel in sorted(ours):
        if rel not in ref:
            extra.append(rel)

    print()
    if missing:
        print(f"MISSING ({len(missing)}):")
        for r in missing:
            print(f"  {r}")
    if mismatched:
        print(f"MISMATCHED ({len(mismatched)}):")
        for r, a, b in mismatched:
            print(f"  {r}: sony={a} ours={b}")
    if extra:
        print(f"EXTRA in fPKGEx ({len(extra)}) - additional records Sony omits:")
        for r in extra:
            print(f"  {r}")

    ok = not missing and not mismatched
    total_ref = sum(s for s, _ in ref.values())
    print()
    print(f"reference bytes: {total_ref}")
    if ok:
        print(f"PASS: all {len(ref)} reference files reproduced byte-identically")
    else:
        print("FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
