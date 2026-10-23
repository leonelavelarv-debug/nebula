# Disc image decompressors (nebula-pak on the console)

Used only by `platform/knulli/pak` (the native `nebula-pak` that turns RVZ/WIA/GCZ images into game.pak on the
RG35XX H). zlib and liblzma come from `third_party/archive/`.

- `zstd/`: Zstandard v1.5.7 (github.com/facebook/zstd, tag v1.5.7, f8745da6), decompression sources only
  (`lib/common`, `lib/decompress`, the public headers). BSD-3-Clause, `zstd/LICENSE`.
- `bzip2/`: bzip2 1.1.0 development tree (github.com/libarchive/bzip2, 1ea1ac18), the library sources only;
  `bz_version.h` written by hand from `bz_version.h.in`. bzip2 license (BSD-style), `bzip2/COPYING`.
