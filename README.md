# fPKGEx

A PS5 payload that identifies a fake package (fPKG) and extracts it **on the
console**. Launch it, open `http://<PS5_IP>:5911/`, pick a `.pkg` from USB or
`/data`, see what it is, choose a destination, and the PS5 does the work.

No mounting, no ShadowMountPlus, no dependency on Sony's publishing tools.

```
http://<PS5_IP>:5911/
```

## Status

Honest summary of what works and what does not.

**Working**

- Browse any console path (`/mnt/usb0…7`, `/mnt/ext0/1`, `/data`) for `.pkg`
  files, with free space shown per volume.
- Identify a package: title name, version, title ID, content ID, DRM type,
  package size, and the full metadata file list.
- Display artwork straight from the package: `icon0.png`, `pic0.png`, `pic1.png`,
  `pic2.png`, `save_data.png`.
- Extract the metadata layers — `sce_sys` and `sce_suppl` — to any destination,
  with live progress, a free-space pre-check, and cancellation.
- A diagnostic panel (`/api/diagnose`) reporting how far parsing reached.

**Not implemented**

The embedded application volume (the game files) is not yet extracted. fPKGEx
reports its size and why rather than presenting a partial result as success. See
[Application volume](#application-volume) below.

## Verified

Everything above was tested against two real packages from different builders:

| | Minecraft | Call of Duty MWII |
|---|---|---|
| size | 1.0 GB | 58.4 GB |
| FIH magic | `\x7fFIH` | `\x7fFIH` |
| format version | 3 | 3 |
| signed byte | `0x00` (plain) | `0x00` (plain) |
| outer superblock | `0x3d470000` | `0xe8bce0000` |
| outer inodes | 5 | 5 |
| files exposed | 35,267 | 16,888 |

The metadata layer was diffed byte-for-byte against Sony's own
`prospero-pub-cmd img_extract`: **all 25 files it produces are reproduced
identically.**

## How it works

fPKGEx parses the package container directly. There is no intermediate format
and no external binary.

| Layer | Contents |
| --- | --- |
| `\x7fFIH` | outer header: PFS offset/size, superblock offset, CNT offset |
| `\x7fCNT` | entry table + string table; `sce_sys` payloads including `param.json` and all artwork |
| ZIP tail | `sce_suppl` payloads |
| outer PFS | superblock (`version=2`, `magic=20130315`) plus the volume container |

### Inner PFS

The decoded inner image is a data-first flat PFS. The reader in `src/innerfs.c`
implements it:

```
superblock    version i64 == 2, magic i64 == 20130315, block_size u32 @ 0x18
              dinode_count u64 @ 0x30
inode k       @ sb + 0x10000 + (k / 390)*0x10000 + (k % 390)*0xA8
              mode     u16 @ 0x00   (0x4000 dir, 0x8000 file)
              size     i64 @ 0x08
              data_off u64 @ 0x60   absolute byte offset in the image
dirents       @ data_off, for `size` bytes:
              { u32 child_inode, u32 type, u32 name_len, u32 ent_size, name[] }
              ent_size = align8(16 + name_len); type 2 = file, 3 = dir
```

Against the real 1.66 GB Minecraft image this walks **37,546 entries (2,301
directories, 35,245 files) totalling 1,298,055,230 bytes in 1.6 s** — matching
the reference extraction exactly.

## Application volume

Many fPKGs carry a large embedded application volume. Its container is
builder-specific and is described by a `naps_pkg_layout.dat` table
(~769 KB) that maps logical file → ublock → cblock → package offset, with each
cblock carrying its own size and decode form.

The inner image is **not** stored contiguously in the package — offsets in the
inner image do not map linearly onto package offsets — so that mapping is
required before the volume can be read.

Volume content is compressed: the CoD package expands from 58.4 GB to 118.1 GB.
Decoding uses the Kraken decompressor built into the console (available on
firmware 11.60 and below). This step is not implemented yet.

## Requirements

- A PS5 with a payload-capable homebrew environment, firmware 11.60 or below
  for volume decompression.
- `ps5-payload-dev/sdk` (v0.43+) and clang 18 / lld 18 to build.

## Build

GitHub Actions builds the payload:

```bash
git push
```

Locally (Linux or WSL):

```bash
sudo apt-get install -y clang-18 lld-18 llvm-18-dev make python3

export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make
```

This produces `fpkgex.elf`. The web UI in `web/` is embedded into the binary by
`tools/embed_assets.py`, so the payload is self-contained.

Override the port with `make FPKEX_PORT=6000`.

## Deploy

Copy `fpkgex.elf` into your payload folder and launch it from Payload Manager:

```
/data/pldmgr/payloads/fpkgex/fpkgex.elf
```

Then open `http://<PS5_IP>:5911/`.

`payload-manager/fpkgex.elf.json` holds Payload Manager metadata; CI rewrites
its `sha256` after every build.

## Layout

```
src/fpkg.c        container reader: FIH/CNT parsing, ZIP tail, file list, probe
src/innerfs.c     inner PFS: superblock, inodes, dirent walk
src/inflate.c     DEFLATE decoder for ZIP entries
src/jsonutil.c    minimal JSON field extraction from param.json
src/httpd.c       HTTP/1.1 server, one thread per connection
src/api.c         JSON endpoints
src/jobs.c        extraction worker and progress
src/fsops.c       destination paths, free space, directory listing
web/              UI, embedded at build time
tools/fpkgex-cli/ desktop CLI that drives the LibProsperoPkg engine
tests/            host-side verification
```

## Verifying

The parsers are plain portable C and are tested on a desktop against real
packages:

```bash
# metadata layer, byte-compared against Sony's extraction
cc -std=c11 -o tests/test_fpkg tests/test_fpkg.c src/fpkg.c src/inflate.c src/jsonutil.c
tests/test_fpkg /path/to/game.pkg /tmp/out
python tests/verify_against_sony.py /path/to/sony-extraction

# inner PFS tree walk
cc -std=c11 -o tests/test_innerfs tests/test_innerfs.c src/innerfs.c
tests/test_innerfs /path/to/inner.pfs --limit 20

# DEFLATE decoder: stored / fixed / dynamic blocks, 0 B to 1 MB
cc -std=c11 -o tests/test_inflate tests/test_inflate.c src/inflate.c
python tests/run_inflate_tests.py tests/test_inflate
```

CI additionally compiles every source file with `-Wall -Wextra -Werror`
against a hosted libc, so the payload tree stays warning-free.

## Desktop CLI

`tools/fpkgex-cli` is a separate .NET 9 front end that drives the
LibProsperoPkg engine shipped with the FPKG GUI. It extracts full file trees on
Windows and is useful for inspecting packages and producing reference output:

```bash
cd tools/fpkgex-cli
dotnet build -c Release
dotnet run -- info    game.pkg
dotnet run -- list    game.pkg --filter sce_sys
dotnet run -- extract game.pkg D:\out
```

The engine is located via `--engine`, `FPKGEX_ENGINE_DIR`, or a build-time path.
It is a Windows-only convenience and is not required by the payload.

## Disclaimer

Homebrew software for personal use. Not affiliated with Sony, PlayStation or any
game publisher. Keep backups; test on a package you can afford to lose.
