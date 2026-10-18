"""Generates DEFLATE test vectors and runs the C decoder against them."""
import os, random, subprocess, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
TMP = os.path.join(HERE, "_inflate_tmp")
os.makedirs(TMP, exist_ok=True)

PAYLOADS = {
    "empty":        b"",
    "tiny":         b"A",
    "short_str":    b"hello world",
    "zeros_1k":     b"\x00" * 1024,
    "zeros_256k":   b"\x00" * 262144,
    "text":         (b"the quick brown fox jumps over the lazy dog. " * 500),
    "repeat":       b"ABABABAB" * 20000,
    "random_64k":   bytes(random.Random(1234).randrange(256) for _ in range(65536)),
    "mixed_128k":   (b"A" * 70000 + bytes(random.Random(99).randrange(256) for _ in range(60000))),
    "structured":   b"".join(bytes([i % 251]) * (1 + (i % 7)) for i in range(9000)),
    "big_1m":       (bytes(random.Random(7).randrange(256) for _ in range(200000)) + b"Q" * 800000),
}

# level 0 forces stored blocks; 1/2/6/9 exercise fixed and dynamic Huffman.
LEVELS = [0, 1, 6, 9]


def build(name, level):
    plain = PAYLOADS[name]
    comp = zlib.compress(plain, level)
    body = comp[2:-4]                       # strip zlib header + adler32
    p_path = os.path.join(TMP, "plain.bin")
    c_path = os.path.join(TMP, "comp.bin")
    with open(p_path, "wb") as f: f.write(plain)
    with open(c_path, "wb") as f: f.write(body)
    return p_path, c_path


def main():
    exe = sys.argv[1]
    failures = 0
    total = 0
    for name in PAYLOADS:
        for level in LEVELS:
            p, c = build(name, level)
            total += 1
            r = subprocess.run([exe, p, c, "raw"], capture_output=True, text=True)
            tag = f"{name:14s} level={level}"
            if r.returncode != 0:
                failures += 1
                print(f"FAIL {tag}: {r.stdout.strip()}")
            else:
                print(f"ok   {tag}")
    print(f"\n{total - failures}/{total} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
