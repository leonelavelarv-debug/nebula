// nebula-pak (native): game.pak for the RG35XX H port from the player's own Super Mario Galaxy disc image, on the
// console itself (no Rust, no Termux). The same output and the same checks as crates/nebula-import's `nebula-pak`
// (game ID RMGE01, main.dol's SHA-1, the title metadata, non-overlapping extents): the game partition's decrypted data
// at its own offsets, sparse elsewhere, written to a sibling temporary file and renamed when complete.
//
//   nebula-pak <imagen del disco> <game.pak>
//   nebula-pak --patch <parche.xdelta> <imagen del disco> <game.pak>   con un parche xdelta del ISO (una traducción)
//   nebula-pak --identify <imagen> | --patch-info <parche.xdelta>
//
// --patch: the xdelta3 patch (VCDIFF) is applied on the way, to the disc's raw bytes as an ISO has them: only the
// windows it changes are built (for WIA/RVZ by re-encrypting those few MiB, hashes and exceptions included); the
// untouched rest of the game is read as always. A window whose Adler-32 differs (a scrubbed image) is accepted
// when every patched cluster the game uses passes its H0 hashes.
//
// Reads ISO, WBFS (also split .wbf1...), CISO, GCZ, WIA and RVZ. The WIA/RVZ reading follows Dolphin's DiscIO
// (WIABlob.cpp, WIACompression.cpp, LaggedFibonacciGenerator.cpp; GPL-2.0-or-later, the generator CC0) and its format
// description (docs/WiaAndRvz.md); WBFS, CISO and GCZ follow Dolphin's readers. This file: GPL-3.0-or-later, as Nebula.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#include <sys/auxv.h>
#if defined(__clang__)
#define PAK_TARGET_AES __attribute__((target("aes")))
#else
#define PAK_TARGET_AES __attribute__((target("+crypto")))
#endif
#endif

#include <bzlib.h>
#include <lzma.h>
#include <zlib.h>
#include <zstd.h>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

namespace {

[[noreturn]] __attribute__((format(printf, 1, 2))) void fail(const char* format, ...) {
  char text[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  throw std::runtime_error(text);
}

u16 be16(const u8* p) { return u16(p[0] << 8 | p[1]); }
u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }
u64 be64(const u8* p) { return u64(be32(p)) << 32 | be32(p + 4); }
u32 le32(const u8* p) { return u32(p[3]) << 24 | u32(p[2]) << 16 | u32(p[1]) << 8 | p[0]; }
u64 le64(const u8* p) { return u64(le32(p + 4)) << 32 | le32(p); }
u64 align_up(u64 x, u64 a) { return (x + a - 1) / a * a; }

// ---------------------------------------------------------------------------------------------------------------
// SHA-1 (main.dol's check, WIA's header hashes, PURGE groups).

class Sha1 {
 public:
  Sha1() { h_[0] = 0x67452301; h_[1] = 0xEFCDAB89; h_[2] = 0x98BADCFE; h_[3] = 0x10325476; h_[4] = 0xC3D2E1F0; }
  void update(const void* data, size_t size) {
    const u8* p = static_cast<const u8*>(data);
    length_ += size;
    while (size > 0) {
      const size_t take = std::min(size, size_t(64) - used_);
      memcpy(block_ + used_, p, take);
      used_ += take; p += take; size -= take;
      if (used_ == 64) { compress(block_); used_ = 0; }
    }
  }
  void finish(u8 out[20]) {
    const u64 bits = length_ * 8;
    const u8 one = 0x80, zero = 0;
    update(&one, 1);
    while (used_ != 56) update(&zero, 1);
    u8 tail[8];
    for (int i = 0; i < 8; ++i) tail[i] = u8(bits >> (56 - 8 * i));
    update(tail, 8);
    for (int i = 0; i < 5; ++i) for (int j = 0; j < 4; ++j) out[i * 4 + j] = u8(h_[i] >> (24 - 8 * j));
  }
  static std::string hex(const void* data, size_t size) {
    Sha1 s; s.update(data, size); u8 d[20]; s.finish(d);
    char text[41];
    for (int i = 0; i < 20; ++i) snprintf(text + 2 * i, 3, "%02x", d[i]);
    return text;
  }

 private:
  static u32 rol(u32 x, int n) { return x << n | x >> (32 - n); }
  void compress(const u8* b) {
    u32 w[80];
    for (int i = 0; i < 16; ++i) w[i] = be32(b + 4 * i);
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = h_[0], bb = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; ++i) {
      u32 f, k;
      if (i < 20) { f = (bb & c) | (~bb & d); k = 0x5A827999; }
      else if (i < 40) { f = bb ^ c ^ d; k = 0x6ED9EBA1; }
      else if (i < 60) { f = (bb & c) | (bb & d) | (c & d); k = 0x8F1BBCDC; }
      else { f = bb ^ c ^ d; k = 0xCA62C1D6; }
      const u32 t = rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rol(bb, 30); bb = a; a = t;
    }
    h_[0] += a; h_[1] += bb; h_[2] += c; h_[3] += d; h_[4] += e;
  }
  u32 h_[5];
  u8 block_[64];
  size_t used_ = 0;
  u64 length_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------
// AES-128-CBC decryption (Wii partitions in ISO/WBFS/CISO/GCZ; WIA and RVZ keep them decrypted). Table-driven, with
// the ARMv8 AES instructions when the CPU has them (checked against the tables once at start).

u8 kSbox[256], kInvSbox[256];
u32 kTd[4][256];

u8 gf_mul(u8 a, u8 b) {
  u8 r = 0;
  while (b) { if (b & 1) r ^= a; a = u8(a << 1 ^ (a & 0x80 ? 0x1B : 0)); b >>= 1; }
  return r;
}

void aes_tables() {
  u8 p = 1, q = 1;
  do {  // p runs over the multiplicative group (generator 3), q over its inverses
    p = u8(p ^ (p << 1) ^ (p & 0x80 ? 0x1B : 0));
    q ^= q << 1; q ^= q << 2; q ^= q << 4; if (q & 0x80) q ^= 0x09;
    const u8 x = u8(q ^ (q << 1 | q >> 7) ^ (q << 2 | q >> 6) ^ (q << 3 | q >> 5) ^ (q << 4 | q >> 4));
    kSbox[p] = x ^ 0x63;
  } while (p != 1);
  kSbox[0] = 0x63;
  for (int i = 0; i < 256; ++i) kInvSbox[kSbox[i]] = u8(i);
  for (int i = 0; i < 256; ++i) {
    const u8 s = kInvSbox[i];
    const u32 t = u32(gf_mul(s, 14)) << 24 | u32(gf_mul(s, 9)) << 16 | u32(gf_mul(s, 13)) << 8 | gf_mul(s, 11);
    for (int r = 0; r < 4; ++r) kTd[r][i] = t >> (8 * r) | t << ((32 - 8 * r) & 31);
  }
}

class Aes128Decrypt {
 public:
  explicit Aes128Decrypt(const u8 key[16]) {
    u8 ek[176];
    memcpy(ek, key, 16);
    u8 rcon = 1;
    for (int i = 16; i < 176; i += 4) {
      u8 t[4] = {ek[i - 4], ek[i - 3], ek[i - 2], ek[i - 1]};
      if (i % 16 == 0) {
        const u8 first = t[0];
        t[0] = u8(kSbox[t[1]] ^ rcon); t[1] = kSbox[t[2]]; t[2] = kSbox[t[3]]; t[3] = kSbox[first];
        rcon = gf_mul(rcon, 2);
      }
      for (int j = 0; j < 4; ++j) ek[i + j] = ek[i - 16 + j] ^ t[j];
    }
    // Equivalent inverse cipher: round keys reversed, InvMixColumns on the inner ones.
    for (int r = 0; r <= 10; ++r) {
      const u8* src = ek + 16 * (10 - r);
      u8* dst = dk_ + 16 * r;
      if (r == 0 || r == 10) { memcpy(dst, src, 16); continue; }
      for (int c = 0; c < 4; ++c) {
        const u8* s = src + 4 * c;
        dst[4 * c + 0] = gf_mul(s[0], 14) ^ gf_mul(s[1], 11) ^ gf_mul(s[2], 13) ^ gf_mul(s[3], 9);
        dst[4 * c + 1] = gf_mul(s[0], 9) ^ gf_mul(s[1], 14) ^ gf_mul(s[2], 11) ^ gf_mul(s[3], 13);
        dst[4 * c + 2] = gf_mul(s[0], 13) ^ gf_mul(s[1], 9) ^ gf_mul(s[2], 14) ^ gf_mul(s[3], 11);
        dst[4 * c + 3] = gf_mul(s[0], 11) ^ gf_mul(s[1], 13) ^ gf_mul(s[2], 9) ^ gf_mul(s[3], 14);
      }
    }
    for (int i = 0; i < 44; ++i) dkw_[i] = be32(dk_ + 4 * i);
  }

  void cbc(const u8 iv_in[16], const u8* in, u8* out, size_t size) const {
#if defined(__aarch64__)
    if (use_hardware) { cbc_hardware(iv_in, in, out, size); return; }
#endif
    u8 iv[16], next[16];
    memcpy(iv, iv_in, 16);
    for (size_t at = 0; at < size; at += 16) {
      memcpy(next, in + at, 16);
      block_tables(in + at, out + at);
      for (int i = 0; i < 16; ++i) out[at + i] ^= iv[i];
      memcpy(iv, next, 16);
    }
  }

  static bool use_hardware;

#if defined(__aarch64__)
  PAK_TARGET_AES void cbc_hardware(const u8 iv_in[16], const u8* in, u8* out, size_t size) const {
    uint8x16_t k[11];
    for (int i = 0; i < 11; ++i) k[i] = vld1q_u8(dk_ + 16 * i);
    uint8x16_t iv = vld1q_u8(iv_in);
    for (size_t at = 0; at < size; at += 16) {
      const uint8x16_t c = vld1q_u8(in + at);
      uint8x16_t x = c;
      for (int r = 0; r < 9; ++r) x = vaesimcq_u8(vaesdq_u8(x, k[r]));
      x = veorq_u8(vaesdq_u8(x, k[9]), k[10]);
      vst1q_u8(out + at, veorq_u8(x, iv));
      iv = c;
    }
  }
#endif

 private:
  void block_tables(const u8* in, u8* out) const {
    u32 s0 = be32(in) ^ dkw_[0], s1 = be32(in + 4) ^ dkw_[1], s2 = be32(in + 8) ^ dkw_[2], s3 = be32(in + 12) ^ dkw_[3];
    for (int r = 1; r < 10; ++r) {
      const u32* k = dkw_ + 4 * r;
      const u32 t0 = kTd[0][s0 >> 24] ^ kTd[1][s3 >> 16 & 255] ^ kTd[2][s2 >> 8 & 255] ^ kTd[3][s1 & 255] ^ k[0];
      const u32 t1 = kTd[0][s1 >> 24] ^ kTd[1][s0 >> 16 & 255] ^ kTd[2][s3 >> 8 & 255] ^ kTd[3][s2 & 255] ^ k[1];
      const u32 t2 = kTd[0][s2 >> 24] ^ kTd[1][s1 >> 16 & 255] ^ kTd[2][s0 >> 8 & 255] ^ kTd[3][s3 & 255] ^ k[2];
      const u32 t3 = kTd[0][s3 >> 24] ^ kTd[1][s2 >> 16 & 255] ^ kTd[2][s1 >> 8 & 255] ^ kTd[3][s0 & 255] ^ k[3];
      s0 = t0; s1 = t1; s2 = t2; s3 = t3;
    }
    const u32* k = dkw_ + 40;
    const u32 s[4] = {s0, s1, s2, s3};
    for (int c = 0; c < 4; ++c) {
      const u32 w = u32(kInvSbox[s[c] >> 24]) << 24 | u32(kInvSbox[s[(c + 3) & 3] >> 16 & 255]) << 16 |
                    u32(kInvSbox[s[(c + 2) & 3] >> 8 & 255]) << 8 | kInvSbox[s[(c + 1) & 3] & 255];
      const u32 v = w ^ k[c];
      out[4 * c] = u8(v >> 24); out[4 * c + 1] = u8(v >> 16); out[4 * c + 2] = u8(v >> 8); out[4 * c + 3] = u8(v);
    }
  }

  u8 dk_[176];
  u32 dkw_[44];
};
[[maybe_unused]] bool Aes128Decrypt::use_hardware = false;

class Aes128Encrypt {
 public:
  explicit Aes128Encrypt(const u8 key[16]) {
    memcpy(rk_, key, 16);
    u8 rcon = 1;
    for (int i = 16; i < 176; i += 4) {
      u8 t[4] = {rk_[i - 4], rk_[i - 3], rk_[i - 2], rk_[i - 1]};
      if (i % 16 == 0) {
        const u8 first = t[0];
        t[0] = u8(kSbox[t[1]] ^ rcon); t[1] = kSbox[t[2]]; t[2] = kSbox[t[3]]; t[3] = kSbox[first];
        rcon = gf_mul(rcon, 2);
      }
      for (int j = 0; j < 4; ++j) rk_[i + j] = rk_[i - 16 + j] ^ t[j];
    }
  }
  void cbc(const u8 iv_in[16], const u8* in, u8* out, size_t size) const {
    u8 iv[16];
    memcpy(iv, iv_in, 16);
    for (size_t at = 0; at < size; at += 16) {
      u8 b[16];
      for (int i = 0; i < 16; ++i) b[i] = in[at + i] ^ iv[i];
      block(b);
      memcpy(out + at, b, 16);
      memcpy(iv, b, 16);
    }
  }

 private:
  void block(u8 s[16]) const {
    for (int i = 0; i < 16; ++i) s[i] ^= rk_[i];
    for (int r = 1; r <= 10; ++r) {
      u8 t[16];
      for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) t[c * 4 + row] = kSbox[s[((c + row) & 3) * 4 + row]];
      if (r < 10) {
        for (int c = 0; c < 4; ++c) {
          const u8 a0 = t[c * 4], a1 = t[c * 4 + 1], a2 = t[c * 4 + 2], a3 = t[c * 4 + 3];
          t[c * 4] = gf_mul(a0, 2) ^ gf_mul(a1, 3) ^ a2 ^ a3;
          t[c * 4 + 1] = a0 ^ gf_mul(a1, 2) ^ gf_mul(a2, 3) ^ a3;
          t[c * 4 + 2] = a0 ^ a1 ^ gf_mul(a2, 2) ^ gf_mul(a3, 3);
          t[c * 4 + 3] = gf_mul(a0, 3) ^ a1 ^ a2 ^ gf_mul(a3, 2);
        }
      }
      for (int i = 0; i < 16; ++i) s[i] = t[i] ^ rk_[16 * r + i];
    }
  }
  u8 rk_[176];
};

void aes_init() {
  aes_tables();
  // FIPS-197 C.1: the tables must decrypt it before anything else trusts them.
  const u8 key[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  const u8 cipher[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
  const u8 plain[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
  const u8 zero[16] = {};
  u8 out[16];
  Aes128Decrypt aes(key);
  aes.cbc(zero, cipher, out, 16);
  if (memcmp(out, plain, 16) != 0) fail("AES interno defectuoso");
  Aes128Encrypt(key).cbc(zero, plain, out, 16);
  if (memcmp(out, cipher, 16) != 0) fail("AES interno defectuoso (cifrado)");
#if defined(__aarch64__)
  if ((getauxval(AT_HWCAP) & (1UL << 3)) && getenv("NEBULA_PAK_NO_AES_HW") == nullptr) {  // HWCAP_AES
    Aes128Decrypt::use_hardware = true;
    u8 block[4096], soft[4096], hard[4096], iv[16];
    for (size_t i = 0; i < sizeof(block); ++i) block[i] = u8(i * 131 + 7);
    for (int i = 0; i < 16; ++i) iv[i] = u8(0xA5 ^ i);
    aes.cbc_hardware(iv, block, hard, sizeof(block));
    Aes128Decrypt::use_hardware = false;
    aes.cbc(iv, block, soft, sizeof(block));
    Aes128Decrypt::use_hardware = memcmp(soft, hard, sizeof(block)) == 0;
  }
#endif
}

// The Wii's retail common key (what Dolphin, nod and every Wii tool carry): decrypts each partition's title key.
const u8 kCommonKey[16] = {0xeb, 0xe4, 0x2a, 0x22, 0x5e, 0x85, 0x93, 0xe4,
                           0x48, 0xd9, 0xc5, 0x45, 0x73, 0x81, 0xaa, 0xf7};

// ---------------------------------------------------------------------------------------------------------------
// Files.

class InputFile {
 public:
  explicit InputFile(const std::string& path) : path_(path) {
    fd_ = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) fail("no se puede abrir %s: %s", path.c_str(), strerror(errno));
    struct stat st;
    if (fstat(fd_, &st) != 0) fail("no se puede leer %s: %s", path.c_str(), strerror(errno));
    size_ = u64(st.st_size);
#ifdef POSIX_FADV_SEQUENTIAL
    posix_fadvise(fd_, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
  }
  ~InputFile() { if (fd_ >= 0) close(fd_); }
  InputFile(const InputFile&) = delete;
  InputFile& operator=(const InputFile&) = delete;
  u64 size() const { return size_; }
  const std::string& path() const { return path_; }
  void read(u64 offset, void* out, size_t size) const {
    u8* p = static_cast<u8*>(out);
    while (size > 0) {
      const ssize_t got = pread(fd_, p, size, off_t(offset));
      if (got < 0 && errno == EINTR) continue;
      if (got < 0) fail("error leyendo %s: %s", path_.c_str(), strerror(errno));
      if (got == 0) fail("%s se acaba antes de tiempo (¿la imagen esta incompleta?)", path_.c_str());
      p += got; offset += u64(got); size -= size_t(got);
    }
  }

 private:
  std::string path_;
  int fd_ = -1;
  u64 size_ = 0;
};

// What a disc image gives: the disc's bytes (as on the real disc, encrypted partitions included) and, for formats
// that keep Wii partitions decrypted (WIA, RVZ), the partition's decrypted data directly.
class Disc {
 public:
  virtual ~Disc() = default;
  virtual const char* format() const = 0;
  virtual void read(u64 offset, size_t size, u8* out) = 0;
  // The disc's bytes exactly as on the real disc, partitions encrypted (what an xdelta patch for the ISO applies to).
  // WIA and RVZ rebuild them: hashes, their stored exceptions and the encryption.
  virtual void read_raw(u64 offset, size_t size, u8* out) { read(offset, size, out); }
};

class PartitionData {
 public:
  virtual ~PartitionData() = default;
  virtual void read(u64 offset, size_t size, u8* out) = 0;  // offsets in the partition's data (0x7C00 per cluster)
};

class IsoDisc final : public Disc {
 public:
  explicit IsoDisc(std::unique_ptr<InputFile> file) : file_(std::move(file)) {}
  const char* format() const override { return "ISO"; }
  void read(u64 offset, size_t size, u8* out) override {
    if (offset + size > file_->size()) fail("la ISO esta incompleta: le faltan datos a partir de %llu MiB",
                                            (unsigned long long)(file_->size() >> 20));
    file_->read(offset, out, size);
  }

 private:
  std::unique_ptr<InputFile> file_;
};

class CisoDisc final : public Disc {
 public:
  explicit CisoDisc(std::unique_ptr<InputFile> file) : file_(std::move(file)) {
    std::vector<u8> header(kHeaderSize);
    file_->read(0, header.data(), header.size());
    block_size_ = le32(header.data() + 4);
    if (block_size_ == 0 || block_size_ % 0x8000 != 0) fail("CISO con bloques de tamano invalido");
    u32 count = 0;
    map_.resize(kHeaderSize - 8);
    for (size_t i = 0; i < map_.size(); ++i) map_[i] = header[8 + i] == 1 ? count++ : kUnused;
  }
  const char* format() const override { return "CISO"; }
  void read(u64 offset, size_t size, u8* out) override {
    while (size > 0) {
      const u64 block = offset / block_size_, within = offset % block_size_;
      const size_t take = size_t(std::min<u64>(block_size_ - within, size));
      if (block < map_.size() && map_[block] != kUnused) {
        file_->read(kHeaderSize + u64(map_[block]) * block_size_ + within, out, take);
      } else {
        memset(out, 0, take);
      }
      offset += take; out += take; size -= take;
    }
  }

 private:
  static constexpr u32 kHeaderSize = 0x8000;
  static constexpr u32 kUnused = 0xFFFFFFFF;
  std::unique_ptr<InputFile> file_;
  u32 block_size_ = 0;
  std::vector<u32> map_;
};

class WbfsDisc final : public Disc {
 public:
  explicit WbfsDisc(std::unique_ptr<InputFile> first) {
    const std::string path = first->path();
    parts_.push_back({std::move(first), 0});
    u64 total = parts_[0].file->size();
    // Split images: name.wbfs, name.wbf1, name.wbf2...
    for (char index = '1'; index <= '9' && path.size() > 4; ++index) {
      std::string next = path;
      next.back() = index;
      if (access(next.c_str(), R_OK) != 0) break;
      parts_.push_back({std::make_unique<InputFile>(next), total});
      total += parts_.back().file->size();
    }
    u8 header[12];
    parts_[0].file->read(0, header, sizeof(header));
    hd_sector_shift_ = header[8];
    wbfs_sector_shift_ = header[9];
    if (hd_sector_shift_ < 9 || hd_sector_shift_ > 20 || wbfs_sector_shift_ < 15 || wbfs_sector_shift_ > 30) {
      fail("cabecera WBFS invalida");
    }
    const u64 wbfs_sector = u64(1) << wbfs_sector_shift_;
    const u64 blocks = (kWiiSectorCount * 0x8000 + wbfs_sector - 1) / wbfs_sector;
    std::vector<u8> table(blocks * 2);
    read_file(u64(1) << hd_sector_shift_ | 0x100, table.data(), table.size());
    wlba_.resize(blocks);
    for (u64 i = 0; i < blocks; ++i) wlba_[i] = be16(table.data() + 2 * i);
  }
  const char* format() const override { return "WBFS"; }
  void read(u64 offset, size_t size, u8* out) override {
    const u64 wbfs_sector = u64(1) << wbfs_sector_shift_;
    while (size > 0) {
      const u64 block = offset >> wbfs_sector_shift_, within = offset & (wbfs_sector - 1);
      const size_t take = size_t(std::min<u64>(wbfs_sector - within, size));
      if (block >= wlba_.size()) fail("lectura fuera del disco WBFS");
      if (wlba_[block] == 0) {
        memset(out, 0, take);
      } else {
        read_file(wbfs_sector * wlba_[block] + within, out, take);
      }
      offset += take; out += take; size -= take;
    }
  }

 private:
  static constexpr u64 kWiiSectorCount = 143432 * 2;
  struct Part {
    std::unique_ptr<InputFile> file;
    u64 base;
  };
  void read_file(u64 address, u8* out, size_t size) {
    while (size > 0) {
      const Part* part = nullptr;
      for (const Part& candidate : parts_) {
        if (address >= candidate.base && address < candidate.base + candidate.file->size()) part = &candidate;
      }
      if (part == nullptr) fail("la imagen WBFS esta incompleta (¿falta un .wbf1?)");
      const u64 within = address - part->base;
      const size_t take = size_t(std::min<u64>(part->file->size() - within, size));
      part->file->read(within, out, take);
      address += take; out += take; size -= take;
    }
  }
  std::vector<Part> parts_;
  unsigned hd_sector_shift_ = 0, wbfs_sector_shift_ = 0;
  std::vector<u16> wlba_;
};

class GczDisc final : public Disc {
 public:
  explicit GczDisc(std::unique_ptr<InputFile> file) : file_(std::move(file)) {
    u8 header[32];
    file_->read(0, header, sizeof(header));
    compressed_size_ = le64(header + 8);
    disc_size_ = le64(header + 16);
    block_size_ = le32(header + 24);
    const u32 blocks = le32(header + 28);
    if (blocks == 0 || block_size_ == 0 || block_size_ > (64u << 20)) fail("cabecera GCZ invalida");
    std::vector<u8> table(size_t(blocks) * 12);
    file_->read(32, table.data(), table.size());
    pointers_.resize(blocks);
    hashes_.resize(blocks);
    for (u32 i = 0; i < blocks; ++i) {
      pointers_[i] = le64(table.data() + 8 * i);
      hashes_[i] = le32(table.data() + 8 * size_t(blocks) + 4 * i);
    }
    data_offset_ = 32 + table.size();
    block_.resize(block_size_);
  }
  const char* format() const override { return "GCZ"; }
  void read(u64 offset, size_t size, u8* out) override {
    if (offset + size > disc_size_) fail("lectura fuera del disco GCZ");
    while (size > 0) {
      const u64 index = offset / block_size_, within = offset % block_size_;
      const size_t take = size_t(std::min<u64>(block_size_ - within, size));
      load(index);
      memcpy(out, block_.data() + within, take);
      offset += take; out += take; size -= take;
    }
  }

 private:
  static constexpr u64 kUncompressed = u64(1) << 63;
  void load(u64 index) {
    if (index == cached_) return;
    if (index >= pointers_.size()) fail("lectura fuera del disco GCZ");
    const u64 start = pointers_[index] & ~kUncompressed;
    const u64 end = index + 1 < pointers_.size() ? pointers_[index + 1] & ~kUncompressed : compressed_size_;
    if (end < start || end - start > u64(block_size_) + 64) fail("bloque GCZ %llu danado", (unsigned long long)index);
    std::vector<u8> in(end - start);
    file_->read(data_offset_ + start, in.data(), in.size());
    if (adler32(adler32(0, nullptr, 0), in.data(), uInt(in.size())) != hashes_[index]) {
      fail("el bloque GCZ %llu esta danado (suma de comprobacion)", (unsigned long long)index);
    }
    if (pointers_[index] & kUncompressed) {
      if (in.size() != block_size_) fail("bloque GCZ %llu danado", (unsigned long long)index);
      memcpy(block_.data(), in.data(), block_size_);
    } else {
      z_stream z{};
      if (inflateInit(&z) != Z_OK) fail("zlib no arranca");
      z.next_in = in.data(); z.avail_in = uInt(in.size());
      z.next_out = block_.data(); z.avail_out = block_size_;
      const int status = inflate(&z, Z_FINISH);
      const u32 produced = block_size_ - z.avail_out;
      inflateEnd(&z);
      if ((status != Z_STREAM_END && status != Z_OK && status != Z_BUF_ERROR) || produced != block_size_) {
        fail("el bloque GCZ %llu no se descomprime", (unsigned long long)index);
      }
    }
    cached_ = index;
  }
  std::unique_ptr<InputFile> file_;
  u64 compressed_size_ = 0, disc_size_ = 0, data_offset_ = 0;
  u32 block_size_ = 0;
  std::vector<u64> pointers_;
  std::vector<u32> hashes_;
  std::vector<u8> block_;
  u64 cached_ = ~u64(0);
};

// ---------------------------------------------------------------------------------------------------------------
// WIA and RVZ.

// The padding generator of GameCube and Wii discs (a lagged Fibonacci generator, j = 32, k = 521): RVZ stores its
// seeds instead of the padding. Dolphin's LaggedFibonacciGenerator (CC0), reading side only.
class PaddingGenerator {
 public:
  void set_seed(const u8* seed) {
    position_ = 0;
    for (size_t i = 0; i < kSeedWords; ++i) buffer_[i] = be32(seed + 4 * i);
    for (size_t i = kSeedWords; i < kK; ++i) {
      buffer_[i] = (buffer_[i - 17] << 23) ^ (buffer_[i - 16] >> 9) ^ buffer_[i - 1];
    }
    for (u32& x : buffer_) x = __builtin_bswap32((x & 0xFF00FFFF) | ((x >> 2) & 0x00FF0000));
    for (int i = 0; i < 4; ++i) advance();
  }
  void forward(size_t bytes) {
    position_ += bytes;
    while (position_ >= kK * 4) { advance(); position_ -= kK * 4; }
  }
  void get(size_t count, u8* out) {
    while (count > 0) {
      const size_t take = std::min(count, kK * 4 - position_);
      memcpy(out, reinterpret_cast<const u8*>(buffer_) + position_, take);
      position_ += take; out += take; count -= take;
      if (position_ == kK * 4) { advance(); position_ = 0; }
    }
  }
  static constexpr size_t kSeedWords = 17;

 private:
  static constexpr size_t kK = 521, kJ = 32;
  void advance() {
    for (size_t i = 0; i < kJ; ++i) buffer_[i] ^= buffer_[i + kK - kJ];
    for (size_t i = kJ; i < kK; ++i) buffer_[i] ^= buffer_[i - kJ];
  }
  u32 buffer_[kK];
  size_t position_ = 0;
};

class WiaDisc final : public Disc {
 public:
  enum Compression : u32 { kNone = 0, kPurge = 1, kBzip2 = 2, kLzma = 3, kLzma2 = 4, kZstd = 5 };

  struct DataPart {
    u32 first_sector, sectors, group_index, groups;
  };
  struct Partition {
    u8 key[16];
    DataPart data[2];
  };

  WiaDisc(std::unique_ptr<InputFile> file, bool rvz) : file_(std::move(file)), rvz_(rvz) {
    u8 h1[0x48];
    file_->read(0, h1, sizeof(h1));
    const u32 version = be32(h1 + 4), compatible = be32(h1 + 8);
    const u32 ours = 0x01000000, oldest = rvz ? 0x00030000 : 0x00080000;
    if (ours < compatible || oldest > version) fail("version de %s no soportada (%08x)", format(), version);
    u8 digest[20];
    { Sha1 s; s.update(h1, 0x48 - 20); s.finish(digest); }
    if (memcmp(digest, h1 + 0x34, 20) != 0) fail("la cabecera del %s esta danada", format());
    iso_size_ = be64(h1 + 0x24);
    if (be64(h1 + 0x2C) != file_->size()) {
      fail("el %s mide %llu bytes y deberia medir %llu: la copia esta incompleta o danada", format(),
           (unsigned long long)file_->size(), (unsigned long long)be64(h1 + 0x2C));
    }
    const u32 h2_size = be32(h1 + 12);
    if (h2_size < 0xDC - 7 || h2_size > 0x10000) fail("la cabecera del %s esta danada", format());
    std::vector<u8> h2(std::max<u32>(h2_size, 0xDC), 0);
    file_->read(0x48, h2.data(), h2_size);
    { Sha1 s; s.update(h2.data(), h2_size); s.finish(digest); }
    if (memcmp(digest, h1 + 0x10, 20) != 0) fail("la cabecera del %s esta danada", format());

    compression_ = be32(h2.data() + 4);
    chunk_size_ = be32(h2.data() + 0xC);
    memcpy(disc_header_, h2.data() + 0x10, 0x80);
    const bool power_of_two = (chunk_size_ & (chunk_size_ - 1)) == 0;
    if ((!rvz || chunk_size_ < 0x8000 || !power_of_two) && chunk_size_ % 0x200000 != 0) {
      fail("tamano de bloque del %s invalido", format());
    }
    if (compression_ > (rvz ? kZstd : kLzma2) || (rvz && compression_ == kPurge)) {
      fail("compresion %u del %s no soportada", compression_, format());
    }
    compressor_data_size_ = h2[0xD4];
    if (compressor_data_size_ > 7) fail("la cabecera del %s esta danada", format());
    memcpy(compressor_data_, h2.data() + 0xD5, 7);

    const u32 partitions = be32(h2.data() + 0x90), partition_size = be32(h2.data() + 0x94);
    if (partitions > 64 || partition_size > 0x1000) fail("la tabla de particiones del %s esta danada", format());
    std::vector<u8> table(size_t(partitions) * partition_size);
    file_->read(be64(h2.data() + 0x98), table.data(), table.size());
    { Sha1 s; s.update(table.data(), table.size()); s.finish(digest); }
    if (memcmp(digest, h2.data() + 0xA0, 20) != 0) fail("la tabla de particiones del %s esta danada", format());
    for (u32 i = 0; i < partitions; ++i) {
      u8 entry[0x30] = {};
      memcpy(entry, table.data() + size_t(i) * partition_size, std::min<u32>(partition_size, 0x30));
      Partition p;
      memcpy(p.key, entry, 16);
      for (int j = 0; j < 2; ++j) {
        const u8* d = entry + 16 + 16 * j;
        p.data[j] = {be32(d), be32(d + 4), be32(d + 8), be32(d + 12)};
      }
      partitions_.push_back(p);
    }

    const u32 raw_count = be32(h2.data() + 0xB4);
    const u32 group_count = be32(h2.data() + 0xC4);
    if (raw_count > 0x100000 || group_count > 0x1000000) fail("las tablas del %s estan danadas", format());
    const std::vector<u8> raw = decode(be64(h2.data() + 0xB8), be32(h2.data() + 0xC0), size_t(raw_count) * 24,
                                       compression_, 0, 0, 0);
    for (u32 i = 0; i < raw_count; ++i) {
      const u8* r = raw.data() + 24 * size_t(i);
      raw_.push_back({be64(r), be64(r + 8), be32(r + 16), be32(r + 20)});
    }
    const size_t group_entry = rvz ? 12 : 8;
    const std::vector<u8> groups = decode(be64(h2.data() + 0xC8), be32(h2.data() + 0xD0),
                                          size_t(group_count) * group_entry, compression_, 0, 0, 0);
    for (u32 i = 0; i < group_count; ++i) {
      const u8* g = groups.data() + group_entry * i;
      groups_.push_back({be32(g), be32(g + 4), rvz ? be32(g + 8) : 0});
    }
  }

  const char* format() const override { return rvz_ ? "RVZ" : "WIA"; }

  // Disc bytes outside Wii partition data (headers, partition tables, tickets and TMDs).
  void read(u64 offset, size_t size, u8* out) override {
    if (offset + size > iso_size_) fail("lectura fuera del disco");
    if (offset < 0x80) {
      const size_t take = size_t(std::min<u64>(0x80 - offset, size));
      memcpy(out, disc_header_ + offset, take);
      offset += take; out += take; size -= take;
    }
    while (size > 0) {
      const RawData* entry = nullptr;
      for (const RawData& r : raw_) {
        if (r.size != 0 && offset >= r.offset && offset < r.offset + r.size) { entry = &r; break; }
      }
      if (entry == nullptr) fail("el %s no guarda los datos del disco en %llu", format(), (unsigned long long)offset);
      const u64 skipped = entry->offset % 0x8000;
      const u64 start = entry->offset - skipped, total = entry->size + skipped;
      const u64 end = std::min<u64>(entry->offset + entry->size, offset + size);
      while (offset < end) {
        const u64 index = (offset - start) / chunk_size_;
        if (index >= entry->groups) fail("el %s esta danado (grupo fuera de su tabla)", format());
        const u64 group_start = index * chunk_size_;
        const u64 length = std::min<u64>(chunk_size_, total - group_start);
        const std::vector<u8>& data = group(entry->group_index + index, length, 0, group_start);
        const u64 within = offset - start - group_start;
        const size_t take = size_t(std::min<u64>(length - within, end - offset));
        memcpy(out, data.data() + within, take);
        offset += take; out += take; size -= take;
      }
    }
  }

  // The decrypted data of the partition whose data starts at `data_start` on the disc, if WIA/RVZ holds it.
  std::unique_ptr<PartitionData> partition_data(u64 data_start);

  // A group's data: `length` bytes after any hash exception lists, RVZ packing undone.
  const std::vector<u8>& group(u64 index, u64 length, u32 exception_lists, u64 data_offset) {
    if (index == cached_group_) return cached_;
    if (index >= groups_.size()) fail("el %s esta danado (grupo %llu)", format(), (unsigned long long)index);
    const Group& g = groups_[index];
    u32 size = g.size;
    u32 compression = compression_;
    if (rvz_) {
      if ((size & 0x80000000u) == 0) compression = kNone;
      size &= 0x7FFFFFFF;
    }
    cached_group_ = ~u64(0);
    if (size == 0) {
      cached_.assign(length, 0);
      cached_exceptions_.clear();
    } else {
      cached_ = decode(u64(g.offset4) << 2, size, length, compression, exception_lists, g.packed, data_offset);
      cached_exceptions_ = last_exceptions_;
    }
    cached_group_ = index;
    return cached_;
  }
  // The hash exception lists of the group group() last returned (u16 count, then 22-byte entries, per list).
  const std::vector<u8>& group_exceptions() const { return cached_exceptions_; }

  void read_raw(u64 offset, size_t size, u8* out) override;

 private:
  struct RawData {
    u64 offset, size;
    u32 group_index, groups;
  };
  struct Group {
    u32 offset4, size, packed;
  };

  // Skips `lists` hash exception lists at data[*at] (each a u16 count and 22-byte entries); after the last one,
  // uncompressed data is aligned to 4 bytes.
  void skip_exceptions(const std::vector<u8>& data, size_t* at, u32 lists, bool align) {
    const size_t start = *at;
    for (u32 i = 0; i < lists; ++i) {
      if (*at + 2 > data.size()) fail("el %s esta danado (lista de excepciones)", format());
      *at += 2 + size_t(be16(data.data() + *at)) * 22;
    }
    if (*at > data.size()) fail("el %s esta danado (lista de excepciones)", format());
    last_exceptions_.assign(data.begin() + long(start), data.begin() + long(*at));
    if (lists > 0 && align) *at = size_t(align_up(*at, 4));
    if (*at > data.size()) fail("el %s esta danado (lista de excepciones)", format());
  }

  std::vector<u8> decode(u64 file_offset, u64 stored, u64 length, u32 compression, u32 exception_lists,
                         u32 packed, u64 data_offset) {
    std::vector<u8> in(stored);
    file_->read(file_offset, in.data(), in.size());
    std::vector<u8> payload;
    size_t at = 0;
    const u64 wanted = packed ? packed : length;
    if (compression == kNone) {
      skip_exceptions(in, &at, exception_lists, true);
      payload.assign(in.begin() + long(at), in.end());
    } else if (compression == kPurge) {
      skip_exceptions(in, &at, exception_lists, true);
      payload = purge(in, at, wanted);
    } else {
      std::vector<u8> out = inflate_all(in, compression, wanted + 0x20000);
      skip_exceptions(out, &at, exception_lists, false);
      payload.assign(out.begin() + long(at), out.end());
    }
    if (packed) return unpack(payload, length, data_offset);
    if (payload.size() < length) fail("el %s esta danado (faltan datos en un grupo)", format());
    payload.resize(length);
    return payload;
  }

  std::vector<u8> purge(const std::vector<u8>& in, size_t at, u64 length) const {
    if (in.size() < at + 20) fail("el %s esta danado (grupo PURGE)", format());
    std::vector<u8> out(length, 0);
    const size_t end = in.size() - 20;
    while (at < end) {
      if (at + 8 > end) fail("el %s esta danado (grupo PURGE)", format());
      const u32 offset = be32(in.data() + at), size = be32(in.data() + at + 4);
      at += 8;
      if (u64(offset) + size > length || at + size > end) fail("el %s esta danado (grupo PURGE)", format());
      memcpy(out.data() + offset, in.data() + at, size);
      at += size;
    }
    u8 digest[20];
    Sha1 s; s.update(in.data(), end); s.finish(digest);
    if (memcmp(digest, in.data() + end, 20) != 0) fail("el %s esta danado (SHA-1 de un grupo PURGE)", format());
    return out;
  }

  std::vector<u8> unpack(const std::vector<u8>& in, u64 length, u64 data_offset) const {
    std::vector<u8> out(length);
    size_t at = 0;
    u64 done = 0;
    PaddingGenerator padding;
    while (done < length) {
      if (at + 4 > in.size()) fail("el %s esta danado (datos RVZ empaquetados)", format());
      const u32 word = be32(in.data() + at);
      at += 4;
      const u64 size = std::min<u64>(word & 0x7FFFFFFF, length - done);
      if (word & 0x80000000u) {
        if (at + PaddingGenerator::kSeedWords * 4 > in.size()) fail("el %s esta danado (semilla RVZ)", format());
        padding.set_seed(in.data() + at);
        at += PaddingGenerator::kSeedWords * 4;
        padding.forward(data_offset % 0x8000);
        padding.get(size, out.data() + done);
      } else {
        if (at + size > in.size()) fail("el %s esta danado (datos RVZ empaquetados)", format());
        memcpy(out.data() + done, in.data() + at, size);
        at += size;
      }
      done += size;
      data_offset += size;
    }
    return out;
  }

  std::vector<u8> inflate_all(const std::vector<u8>& in, u32 compression, u64 expected) const {
    std::vector<u8> out(expected);
    size_t produced = 0;
    auto grow = [&] { if (produced == out.size()) out.resize(out.size() * 2 + 0x10000); };
    if (compression == kZstd) {
      ZSTD_DStream* stream = ZSTD_createDStream();
      if (stream == nullptr) fail("zstd no arranca");
      ZSTD_inBuffer input{in.data(), in.size(), 0};
      size_t result = 1;
      while (true) {
        grow();
        ZSTD_outBuffer output{out.data(), out.size(), produced};
        result = ZSTD_decompressStream(stream, &output, &input);
        produced = output.pos;
        if (ZSTD_isError(result)) { ZSTD_freeDStream(stream); fail("el %s esta danado (zstd: %s)", format(), ZSTD_getErrorName(result)); }
        if (result == 0 && input.pos == input.size) break;
        if (input.pos == input.size && output.pos < output.size) break;  // the frame ends with the input
      }
      ZSTD_freeDStream(stream);
    } else if (compression == kBzip2) {
      bz_stream stream{};
      if (BZ2_bzDecompressInit(&stream, 0, 0) != BZ_OK) fail("bzip2 no arranca");
      stream.next_in = const_cast<char*>(reinterpret_cast<const char*>(in.data()));
      stream.avail_in = unsigned(in.size());
      while (true) {
        grow();
        stream.next_out = reinterpret_cast<char*>(out.data() + produced);
        stream.avail_out = unsigned(out.size() - produced);
        const int result = BZ2_bzDecompress(&stream);
        produced = out.size() - stream.avail_out;
        if (result == BZ_STREAM_END) break;
        if (result != BZ_OK) { BZ2_bzDecompressEnd(&stream); fail("el %s esta danado (bzip2 %d)", format(), result); }
        if (stream.avail_in == 0 && stream.avail_out > 0) break;
      }
      BZ2_bzDecompressEnd(&stream);
    } else {
      lzma_options_lzma options{};
      if (compression == kLzma) {
        if (compressor_data_size_ != 5 || compressor_data_[0] >= 9 * 5 * 5) fail("parametros LZMA del %s invalidos", format());
        options.dict_size = le32(compressor_data_ + 1);
        const u8 d = compressor_data_[0];
        options.lc = d % 9;
        options.pb = d / 9 / 5;
        options.lp = d / 9 % 5;
      } else {
        if (compressor_data_size_ != 1 || compressor_data_[0] > 40) fail("parametros LZMA2 del %s invalidos", format());
        const u8 d = compressor_data_[0];
        options.dict_size = d == 40 ? 0xFFFFFFFF : (2u | (d & 1u)) << (d / 2 + 11);
      }
      lzma_filter filters[2] = {{compression == kLzma ? LZMA_FILTER_LZMA1 : LZMA_FILTER_LZMA2, &options},
                                {LZMA_VLI_UNKNOWN, nullptr}};
      lzma_stream stream = LZMA_STREAM_INIT;
      if (lzma_raw_decoder(&stream, filters) != LZMA_OK) fail("LZMA no arranca");
      stream.next_in = in.data();
      stream.avail_in = in.size();
      while (true) {
        grow();
        stream.next_out = out.data() + produced;
        stream.avail_out = out.size() - produced;
        const lzma_ret result = lzma_code(&stream, LZMA_RUN);
        produced = out.size() - stream.avail_out;
        if (result == LZMA_STREAM_END) break;
        if (result != LZMA_OK && result != LZMA_BUF_ERROR) { lzma_end(&stream); fail("el %s esta danado (LZMA %d)", format(), int(result)); }
        if (stream.avail_in == 0 && stream.avail_out > 0) break;
        if (result == LZMA_BUF_ERROR && stream.avail_out > 0) break;
      }
      lzma_end(&stream);
    }
    out.resize(produced);
    return out;
  }

  std::unique_ptr<InputFile> file_;
  bool rvz_;
  u64 iso_size_ = 0;
  u32 compression_ = 0, chunk_size_ = 0;
  u8 disc_header_[0x80];
  u8 compressor_data_[7] = {};
  u8 compressor_data_size_ = 0;
  std::vector<Partition> partitions_;
  std::vector<RawData> raw_;
  std::vector<Group> groups_;
  std::vector<u8> cached_, cached_exceptions_, last_exceptions_;
  u64 cached_group_ = ~u64(0);
  // read_raw's last rebuilt 2 MiB of encrypted partition (64 clusters).
  std::vector<u8> raw_group_;
  u64 raw_group_start_ = ~u64(0);
  void rebuild_group(const Partition& partition, u64 big, u8* out);

  friend class WiaPartitionData;
};

class WiaPartitionData final : public PartitionData {
 public:
  WiaPartitionData(WiaDisc& disc, const WiaDisc::Partition& partition) : disc_(disc), partition_(partition) {}
  void read(u64 offset, size_t size, u8* out) override {
    const u64 chunk = u64(disc_.chunk_size_) * 0x7C00 / 0x8000;
    const u32 lists = std::max<u32>(1, u32(chunk / 0x1F0000));
    const u32 first = partition_.data[0].first_sector;
    while (size > 0) {
      const WiaDisc::DataPart* part = nullptr;
      u64 part_start = 0, part_size = 0;
      for (const WiaDisc::DataPart& d : partition_.data) {
        if (d.sectors == 0) continue;
        const u64 start = u64(d.first_sector - first) * 0x7C00, length = u64(d.sectors) * 0x7C00;
        if (offset >= start && offset < start + length) { part = &d; part_start = start; part_size = length; }
      }
      if (part == nullptr) fail("el %s no guarda los datos de la particion en %llu", disc_.format(), (unsigned long long)offset);
      const u64 index = (offset - part_start) / chunk;
      if (index >= part->groups) fail("el %s esta danado (grupo fuera de su tabla)", disc_.format());
      const u64 group_start = index * chunk;
      const u64 length = std::min<u64>(chunk, part_size - group_start);
      const std::vector<u8>& data = disc_.group(part->group_index + index, length, lists, group_start);
      const u64 within = offset - part_start - group_start;
      const size_t take = size_t(std::min<u64>(length - within, size));
      memcpy(out, data.data() + within, take);
      offset += take; out += take; size -= take;
    }
  }

 private:
  WiaDisc& disc_;
  WiaDisc::Partition partition_;
};

std::unique_ptr<PartitionData> WiaDisc::partition_data(u64 data_start) {
  for (const Partition& p : partitions_) {
    if (u64(p.data[0].first_sector) * 0x8000 == data_start) return std::make_unique<WiaPartitionData>(*this, p);
  }
  return nullptr;
}

// 2 MiB of a WIA/RVZ partition as on the disc: the hash tree recomputed from the data (H0 per KiB, H1 per cluster,
// H2 per 8 clusters), the stored hash exceptions applied over it, then encrypted with the title key (Dolphin's
// VolumeWii::EncryptGroups and WIARVZFileReader::ApplyHashExceptions).
void WiaDisc::rebuild_group(const Partition& p, u64 big, u8* out) {
  const u32 first = p.data[0].first_sector;
  const u64 total = p.data[1].sectors ? u64(p.data[1].first_sector - first) + p.data[1].sectors : p.data[0].sectors;
  const u64 c0 = big * 64;
  if (c0 >= total) fail("lectura fuera de la particion del %s", format());
  const u64 count = std::min<u64>(64, total - c0);
  std::vector<u8> plain(64 * 0x7C00, 0);
  WiaPartitionData data(*this, p);
  data.read(c0 * 0x7C00, size_t(count * 0x7C00), plain.data());
  std::vector<u8> hashes(64 * 0x400, 0);
  for (int c = 0; c < 64; ++c) {
    for (int i = 0; i < 31; ++i) {
      Sha1 h; h.update(plain.data() + c * 0x7C00 + i * 0x400, 0x400); h.finish(hashes.data() + c * 0x400 + i * 20);
    }
  }
  u8 h2[0xA0];
  for (int sg = 0; sg < 8; ++sg) {
    u8 h1[0xA0];
    for (int k = 0; k < 8; ++k) { Sha1 h; h.update(hashes.data() + (sg * 8 + k) * 0x400, 0x26C); h.finish(h1 + k * 20); }
    for (int k = 0; k < 8; ++k) memcpy(hashes.data() + (sg * 8 + k) * 0x400 + 0x280, h1, 0xA0);
    Sha1 h; h.update(h1, 0xA0); h.finish(h2 + sg * 20);
  }
  for (int c = 0; c < 64; ++c) memcpy(hashes.data() + c * 0x400 + 0x340, h2, 0xA0);
  // The exceptions of the chunks over these 2 MiB.
  const u64 chunk = u64(chunk_size_) * 0x7C00 / 0x8000;
  const u32 lists = std::max<u32>(1, u32(chunk / 0x1F0000));
  const u64 g_start = big * 0x1F0000, g_end = g_start + 0x1F0000;
  for (const DataPart& d : p.data) {
    if (d.sectors == 0) continue;
    const u64 entry_start = u64(d.first_sector - first) * 0x7C00, entry_size = u64(d.sectors) * 0x7C00;
    for (u64 i = 0; i < d.groups; ++i) {
      const u64 chunk_start = entry_start + i * chunk;
      if (chunk_start >= entry_start + entry_size) break;
      const u64 length = std::min<u64>(chunk, entry_size - i * chunk);
      if (chunk_start + length <= g_start || chunk_start >= g_end) continue;
      group(d.group_index + i, length, lists, i * chunk);
      const std::vector<u8>& ex = group_exceptions();
      u64 list = 0, additional = 0;
      if (chunk >= 0x1F0000) list = (g_start - chunk_start) / 0x1F0000;
      else additional = chunk_start % 0x1F0000 / 0x7C00 * 0x400;
      size_t at = 0;
      for (u64 l = 0; l < lists && at + 2 <= ex.size(); ++l) {
        const u16 n = be16(ex.data() + at);
        at += 2;
        for (u16 e = 0; e < n && at + 22 <= ex.size(); ++e, at += 22) {
          if (l != list) continue;
          const u64 offset = be16(ex.data() + at) + additional;
          if (offset / 0x400 >= 64 || offset % 0x400 + 20 > 0x400) fail("el %s esta danado (excepcion de hash)", format());
          memcpy(hashes.data() + offset, ex.data() + at + 2, 20);
        }
      }
    }
  }
  const Aes128Encrypt aes(p.key);
  const u8 zero[16] = {};
  for (u64 c = 0; c < count; ++c) {
    u8* cluster = out + c * 0x8000;
    aes.cbc(zero, hashes.data() + c * 0x400, cluster, 0x400);
    aes.cbc(cluster + 0x3D0, plain.data() + c * 0x7C00, cluster + 0x400, 0x7C00);
  }
}

void WiaDisc::read_raw(u64 offset, size_t size, u8* out) {
  while (size > 0) {
    const Partition* inside = nullptr;
    u64 start = 0, end = 0, next = ~u64(0);
    for (const Partition& p : partitions_) {
      const u32 first = p.data[0].first_sector;
      const u64 total = p.data[1].sectors ? u64(p.data[1].first_sector - first) + p.data[1].sectors : p.data[0].sectors;
      const u64 s = u64(first) * 0x8000, e = s + total * 0x8000;
      if (offset >= s && offset < e) { inside = &p; start = s; end = e; }
      else if (s > offset) next = std::min(next, s);
    }
    if (inside == nullptr) {
      const size_t take = size_t(std::min<u64>(size, next - offset));
      read(offset, take, out);
      offset += take; out += take; size -= take;
      continue;
    }
    const u64 group = (offset - start) / 0x200000;
    const u64 group_start = start + group * 0x200000;
    if (raw_group_start_ != group_start) {
      raw_group_.assign(0x200000, 0);
      raw_group_start_ = ~u64(0);
      rebuild_group(*inside, group, raw_group_.data());
      raw_group_start_ = group_start;
    }
    const size_t take = size_t(std::min<u64>({size, group_start + 0x200000 - offset, end - offset}));
    memcpy(out, raw_group_.data() + (offset - group_start), take);
    offset += take; out += take; size -= take;
  }
}

// The decrypted data of a partition on a disc that keeps it encrypted: 0x8000-byte clusters, 0x400 of hashes and
// 0x7C00 of data, each decrypted with the title key and the IV stored in its hash block. Read 64 clusters at a time.
class EncryptedPartitionData final : public PartitionData {
 public:
  EncryptedPartitionData(Disc& disc, u64 data_start, u64 data_size, const u8 key[16])
      : disc_(disc), start_(data_start), clusters_(data_size / 0x8000), aes_(key),
        raw_(kBatch * 0x8000), plain_(kBatch * 0x7C00) {}
  void read(u64 offset, size_t size, u8* out) override {
    while (size > 0) {
      const u64 batch = offset / (kBatch * 0x7C00);
      load(batch);
      const u64 within = offset - batch * kBatch * 0x7C00;
      if (within >= loaded_) fail("lectura fuera de la particion del juego");
      const size_t take = size_t(std::min<u64>(loaded_ - within, size));
      memcpy(out, plain_.data() + within, take);
      offset += take; out += take; size -= take;
    }
  }

 private:
  static constexpr u64 kBatch = 64;
  void load(u64 batch) {
    if (batch == batch_) return;
    const u64 first = batch * kBatch;
    if (first >= clusters_) fail("lectura fuera de la particion del juego");
    const u64 count = std::min<u64>(kBatch, clusters_ - first);
    disc_.read(start_ + first * 0x8000, size_t(count * 0x8000), raw_.data());
    for (u64 i = 0; i < count; ++i) {
      const u8* cluster = raw_.data() + i * 0x8000;
      aes_.cbc(cluster + 0x3D0, cluster + 0x400, plain_.data() + i * 0x7C00, 0x7C00);
    }
    loaded_ = count * 0x7C00;
    batch_ = batch;
  }
  Disc& disc_;
  u64 start_, clusters_;
  Aes128Decrypt aes_;
  std::vector<u8> raw_, plain_;
  u64 batch_ = ~u64(0), loaded_ = 0;
};

// An xdelta3 patch for the ISO (VCDIFF, RFC 3284: default code table, no secondary compression, xdelta3's per-window
// Adler-32). Windows that only copy the same bytes of the source are told apart: the patched disc reads those from
// the source as they are.
class XdeltaPatch {
 public:
  struct Window {
    u64 target, source_pos, source_len;
    u32 length;
    size_t adds, inst, addr;  // offsets into bytes_
    size_t adds_len, inst_len, addr_len;
    bool has_adler, identity;
    u32 adler;
  };

  explicit XdeltaPatch(const std::string& path) : path_(path) {
    InputFile f(path);
    if (f.size() > (512u << 20)) fail("el parche %s es demasiado grande", path.c_str());
    bytes_.resize(size_t(f.size()));
    f.read(0, bytes_.data(), bytes_.size());
    if (bytes_.size() < 5 || bytes_[0] != 0xD6 || bytes_[1] != 0xC3 || bytes_[2] != 0xC4) {
      fail("%s no es un parche xdelta (VCDIFF)", path.c_str());
    }
    size_t at = 4;
    const u8 header = byte(at);
    if (header & 1) fail("el parche usa compresion secundaria (no soportada): rehazlo con xdelta3 -S none");
    if (header & 2) fail("el parche usa una tabla de codigos propia (no soportada)");
    if (header & 4) { const u64 n = varint(at); at += size_t(n); }
    u64 target = 0;
    while (at < bytes_.size()) {
      Window w{};
      const u8 indicator = byte(at);
      if (indicator & 2) fail("el parche copia del destino entre ventanas (VCD_TARGET, no soportado)");
      if (indicator & 1) { w.source_len = varint(at); w.source_pos = varint(at); }
      varint(at);  // delta encoding length
      w.length = u32(varint(at));
      const u8 delta = byte(at);
      if (delta != 0) fail("el parche comprime sus secciones (no soportado): rehazlo con xdelta3 -S none");
      w.adds_len = size_t(varint(at)); w.inst_len = size_t(varint(at)); w.addr_len = size_t(varint(at));
      w.has_adler = indicator & 4;
      if (w.has_adler) { if (at + 4 > bytes_.size()) bad(); w.adler = be32(bytes_.data() + at); at += 4; }
      w.adds = at; w.inst = w.adds + w.adds_len; w.addr = w.inst + w.inst_len;
      at = w.addr + w.addr_len;
      if (at > bytes_.size()) bad();
      w.target = target;
      target += w.length;
      w.identity = false;
      if ((indicator & 1) && w.source_pos == w.target && w.adds_len == 0) {
        // One COPY of the whole window from its start: the same bytes as the source.
        std::vector<Op> ops;
        decode_ops(w, &ops, 2);
        w.identity = ops.size() == 1 && ops[0].kind == 3 && ops[0].size == w.length && ops[0].address == 0;
      }
      windows_.push_back(w);
    }
    target_size_ = target;
  }

  u64 target_size() const { return target_size_; }
  const std::vector<Window>& windows() const { return windows_; }
  size_t modified_windows() const {
    return size_t(std::count_if(windows_.begin(), windows_.end(), [](const Window& w) { return !w.identity; }));
  }
  const Window* window_at(u64 offset) const {
    auto it = std::upper_bound(windows_.begin(), windows_.end(), offset, [](u64 o, const Window& w) { return o < w.target; });
    if (it == windows_.begin()) return nullptr;
    --it;
    return offset < it->target + it->length ? &*it : nullptr;
  }
  bool modifies(u64 offset, u64 size) const {
    for (u64 at = offset; at < offset + size;) {
      const Window* w = window_at(at);
      if (w == nullptr) return true;
      if (!w->identity) return true;
      at = w->target + w->length;
    }
    return false;
  }

  // The window's target bytes from the source disc's raw bytes.
  // False when the window's Adler-32 differs: the source is not byte for byte the patch's (often a scrubbed image,
  // whose unused clusters are zeros); the patched game data is then checked by its own hashes as it is read.
  bool build(const Window& w, Disc& source, std::vector<u8>* out) const {
    std::vector<u8> src(size_t(w.source_len));
    if (!src.empty()) source.read_raw(w.source_pos, src.size(), src.data());
    out->assign(w.length, 0);
    std::vector<Op> ops;
    decode_ops(w, &ops, ~size_t(0));
    for (const Op& op : ops) {
      if (op.here + op.size > w.length) bad();
      u8* dst = out->data() + op.here;
      if (op.kind == 1) memcpy(dst, bytes_.data() + w.adds + op.data, size_t(op.size));
      else if (op.kind == 2) memset(dst, bytes_[w.adds + op.data], size_t(op.size));
      else if (op.address + op.size <= w.source_len) memcpy(dst, src.data() + op.address, size_t(op.size));
      else {
        for (u64 i = 0; i < op.size; ++i) {  // from the target window itself, overlapping allowed
          const u64 a = op.address + i;
          (*out)[size_t(op.here + i)] = a < w.source_len ? src[size_t(a)] : (*out)[size_t(a - w.source_len)];
        }
      }
    }
    return !w.has_adler || adler32(adler32(0, nullptr, 0), out->data(), uInt(out->size())) == w.adler;
  }

 private:
  struct Op {
    int kind;  // 1 ADD, 2 RUN, 3 COPY
    u64 here, size, address;
    size_t data;  // ADD/RUN: offset in the window's data section
  };
  [[noreturn]] void bad() const { fail("el parche %s esta danado", path_.c_str()); }
  u8 byte(size_t& at) const { if (at >= bytes_.size()) bad(); return bytes_[at++]; }
  u64 varint(size_t& at) const {
    u64 v = 0;
    for (int i = 0; i < 10; ++i) {
      const u8 b = byte(at);
      v = v << 7 | (b & 0x7F);
      if (!(b & 0x80)) return v;
    }
    bad();
  }
  struct Code { u8 type1, size1, mode1, type2, size2, mode2; };
  static const std::vector<Code>& table() {
    static const std::vector<Code> t = [] {
      std::vector<Code> c;
      c.push_back({2, 0, 0, 0, 0, 0});
      for (u8 s = 0; s < 18; ++s) c.push_back({1, s, 0, 0, 0, 0});
      for (u8 m = 0; m < 9; ++m) {
        c.push_back({3, 0, m, 0, 0, 0});
        for (u8 s = 4; s < 19; ++s) c.push_back({3, s, m, 0, 0, 0});
      }
      for (u8 m = 0; m < 6; ++m)
        for (u8 a = 1; a < 5; ++a)
          for (u8 k = 4; k < 7; ++k) c.push_back({1, a, 0, 3, k, m});
      for (u8 m = 6; m < 9; ++m)
        for (u8 a = 1; a < 5; ++a) c.push_back({1, a, 0, 3, 4, m});
      for (u8 m = 0; m < 9; ++m) c.push_back({3, 4, m, 1, 1, 0});
      return c;
    }();
    return t;
  }
  void decode_ops(const Window& w, std::vector<Op>* ops, size_t limit) const {
    u64 near[4] = {}, same[3 * 256] = {};
    int near_next = 0;
    size_t ip = w.inst, ap = w.addr, dp = 0;
    const size_t inst_end = w.inst + w.inst_len, addr_end = w.addr + w.addr_len;
    u64 here = 0;
    while (ip < inst_end && ops->size() < limit) {
      const Code& code = table()[byte(ip)];
      for (int half = 0; half < 2; ++half) {
        const u8 type = half ? code.type2 : code.type1;
        if (type == 0) continue;
        u64 size = half ? code.size2 : code.size1;
        const u8 mode = half ? code.mode2 : code.mode1;
        if (size == 0) { size = varint(ip); if (ip > inst_end) bad(); }
        Op op{type, here, size, 0, 0};
        if (type == 1) { op.data = dp; dp += size_t(size); if (dp > w.adds_len) bad(); }
        else if (type == 2) { op.data = dp; dp += 1; if (dp > w.adds_len) bad(); }
        else {
          const u64 current = w.source_len + here;
          u64 address;
          if (mode == 0) address = varint(ap);
          else if (mode == 1) address = current - varint(ap);
          else if (mode < 6) address = near[mode - 2] + varint(ap);
          else address = same[(mode - 6) * 256 + byte(ap)];
          if (ap > addr_end) bad();
          near[near_next] = address; near_next = (near_next + 1) % 4;
          same[address % (3 * 256)] = address;
          op.address = address;
        }
        ops->push_back(op);
        here += size;
      }
    }
  }
  std::string path_;
  std::vector<u8> bytes_;
  std::vector<Window> windows_;
  u64 target_size_ = 0;
};

// The source disc with the patch applied: patched windows are built when read (the last two kept).
class PatchedDisc final : public Disc {
 public:
  PatchedDisc(Disc& source, const XdeltaPatch& patch) : source_(source), patch_(patch) {}
  const char* format() const override { return source_.format(); }
  void read(u64 offset, size_t size, u8* out) override { read_raw(offset, size, out); }
  void read_raw(u64 offset, size_t size, u8* out) override {
    while (size > 0) {
      const XdeltaPatch::Window* w = patch_.window_at(offset);
      if (w == nullptr) fail("lectura fuera del disco parcheado");
      const size_t take = size_t(std::min<u64>(size, w->target + w->length - offset));
      if (w->identity) {
        source_.read_raw(offset, take, out);
      } else {
        const std::vector<u8>& data = built(*w);
        memcpy(out, data.data() + (offset - w->target), take);
      }
      offset += take; out += take; size -= take;
    }
  }
  const XdeltaPatch& patch() const { return patch_; }

 private:
  const std::vector<u8>& built(const XdeltaPatch::Window& w) {
    for (auto& slot : cache_) if (slot.first == w.target && !slot.second.empty()) return slot.second;
    auto& slot = cache_[next_];
    next_ ^= 1;
    slot.first = ~u64(0);
    if (!patch_.build(w, source_, &slot.second) && !warned_) {
      warned_ = true;
      printf("Aviso: tu disco no es identico byte a byte al que espera el parche (en %llu MiB; suele ser una imagen "
             "recortada): se comprueba cada bloque del juego con sus hashes\n", (unsigned long long)(w.target >> 20));
      fflush(stdout);
    }
    slot.first = w.target;
    return slot.second;
  }
  bool warned_ = false;
  Disc& source_;
  const XdeltaPatch& patch_;
  std::pair<u64, std::vector<u8>> cache_[2] = {{~u64(0), {}}, {~u64(0), {}}};
  int next_ = 0;
};

// The patched game partition: clusters the patch touches decrypted from the patched disc, the rest straight from the
// source (for WIA/RVZ without encrypting anything).
class PatchedPartitionData final : public PartitionData {
 public:
  PatchedPartitionData(PartitionData& source, PatchedDisc& disc, u64 data_start, u64 data_size, const u8 key[16])
      : source_(source), disc_(disc), start_(data_start), clusters_(data_size / 0x8000), aes_(key) {}
  void read(u64 offset, size_t size, u8* out) override {
    while (size > 0) {
      const u64 cluster = offset / 0x7C00, within = offset % 0x7C00;
      if (cluster >= clusters_) fail("lectura fuera de la particion del juego");
      // Runs of untouched clusters go to the source in one read.
      u64 end = cluster;
      while (end < clusters_ && !touched(end) && (end - cluster) * 0x7C00 < within + size) ++end;
      if (end > cluster) {
        const size_t take = size_t(std::min<u64>(size, (end - cluster) * 0x7C00 - within));
        source_.read(offset, take, out);
        offset += take; out += take; size -= take;
        continue;
      }
      if (cluster != cached_) {
        u8 raw[0x8000], hashes[0x400];
        cached_ = ~u64(0);
        disc_.read_raw(start_ + cluster * 0x8000, sizeof(raw), raw);
        aes_.cbc(raw + 0x3D0, raw + 0x400, plain_, 0x7C00);
        const u8 zero[16] = {};
        aes_.cbc(zero, raw, hashes, 0x400);
        for (int i = 0; i < 31; ++i) {  // H0: each KiB of the patched data against its hash
          u8 digest[20];
          Sha1 h; h.update(plain_ + i * 0x400, 0x400); h.finish(digest);
          if (memcmp(digest, hashes + i * 20, 20) != 0) {
            fail("el parche no corresponde a este disco: el bloque %llu del juego parcheado no cuadra con sus hashes "
                 "(¿otra version del juego o una imagen danada?)", (unsigned long long)cluster);
          }
        }
        cached_ = cluster;
      }
      const size_t take = size_t(std::min<u64>(size, 0x7C00 - within));
      memcpy(out, plain_ + within, take);
      offset += take; out += take; size -= take;
    }
  }
  bool touched(u64 cluster) const { return disc_.patch().modifies(start_ + cluster * 0x8000, 0x8000); }
  u64 clusters() const { return clusters_; }

 private:
  PartitionData& source_;
  PatchedDisc& disc_;
  u64 start_, clusters_;
  Aes128Decrypt aes_;
  u8 plain_[0x7C00];
  u64 cached_ = ~u64(0);
};

std::unique_ptr<Disc> open_disc(const std::string& path) {
  auto file = std::make_unique<InputFile>(path);
  u8 head[0x20] = {};
  if (file->size() < sizeof(head)) fail("%s no es una imagen de disco (demasiado pequena)", path.c_str());
  file->read(0, head, sizeof(head));
  if (memcmp(head, "WIA\x01", 4) == 0) return std::make_unique<WiaDisc>(std::move(file), false);
  if (memcmp(head, "RVZ\x01", 4) == 0) return std::make_unique<WiaDisc>(std::move(file), true);
  if (memcmp(head, "WBFS", 4) == 0) return std::make_unique<WbfsDisc>(std::move(file));
  if (memcmp(head, "CISO", 4) == 0) return std::make_unique<CisoDisc>(std::move(file));
  if (le32(head) == 0xB10BC001) return std::make_unique<GczDisc>(std::move(file));
  if (be32(head + 0x18) == 0x5D1C9EA3 || be32(head + 0x1C) == 0xC2339F3D) return std::make_unique<IsoDisc>(std::move(file));
  fail("%s no es una imagen de disco de Wii conocida (ISO, RVZ, WIA, WBFS, GCZ o CISO)", path.c_str());
}

// ---------------------------------------------------------------------------------------------------------------
// game.pak.

constexpr const char* kGameId = "RMGE01";
constexpr const char* kDolSha1 = "9a71008ae1ee9010e267fa67d1f0b0d4f0e895dd";
constexpr u64 kPartitionDataBytes = 0xff7c0000;

struct Extent {
  std::string label;
  u64 start, end;
};

struct FileEntry {
  u64 offset, length;
  std::string name;
};

std::vector<FileEntry> parse_fst(const std::vector<u8>& fst) {
  if (fst.size() < 12 || fst[0] != 1) fail("la tabla de ficheros del disco no es valida");
  const u64 count = be32(fst.data() + 8);
  if (count == 0 || count * 12 > fst.size()) fail("la tabla de ficheros del disco no es valida");
  const size_t strings = size_t(count * 12);
  auto name_at = [&](u32 offset) -> std::string {
    if (strings + offset >= fst.size()) fail("la tabla de ficheros del disco no es valida");
    const char* s = reinterpret_cast<const char*>(fst.data() + strings + offset);
    return std::string(s, strnlen(s, fst.size() - strings - offset));
  };
  std::vector<FileEntry> files;
  std::vector<std::pair<u64, std::string>> dirs;  // end index, path
  for (u64 i = 1; i < count; ++i) {
    while (!dirs.empty() && i >= dirs.back().first) dirs.pop_back();
    const u8* e = fst.data() + i * 12;
    const std::string name = name_at(be32(e) & 0xFFFFFF);
    const std::string path = (dirs.empty() ? std::string() : dirs.back().second + "/") + name;
    if (e[0] == 1) {
      const u64 end = be32(e + 8);
      if (end <= i || end > count) fail("la tabla de ficheros del disco no es valida");
      dirs.push_back({end, path});
    } else {
      files.push_back({u64(be32(e + 4)) << 2, be32(e + 8), path});
    }
  }
  return files;
}

u64 validate_extents(std::vector<Extent> extents, u64 partition_size) {
  extents.erase(std::remove_if(extents.begin(), extents.end(), [](const Extent& e) { return e.start == e.end; }),
                extents.end());
  std::sort(extents.begin(), extents.end(),
            [](const Extent& a, const Extent& b) { return a.start != b.start ? a.start < b.start : a.end < b.end; });
  for (const Extent& e : extents) {
    if (e.end < e.start || e.end > partition_size) fail("%s queda fuera de la particion del juego", e.label.c_str());
  }
  for (size_t i = 1; i < extents.size(); ++i) {
    if (extents[i - 1].end > extents[i].start) {
      fail("%s y %s se solapan en el disco", extents[i - 1].label.c_str(), extents[i].label.c_str());
    }
  }
  return extents.empty() ? 0 : extents.back().end;
}

void validate_tmd(const std::vector<u8>& tmd) {
  if (tmd.size() != 0x208 || be32(tmd.data()) != 0x00010001) fail("los metadatos de la particion no son los de RMGE01");
  if (be16(tmd.data() + 0x1DE) != 1) fail("los metadatos de la particion no tienen exactamente un contenido");
  const u8* record = tmd.data() + 0x1E4;
  if (be32(record) != 0 || be16(record + 4) != 0 || be16(record + 6) != 3 || be64(record + 8) != kPartitionDataBytes) {
    fail("el contenido de la particion no es el de RMGE01");
  }
}

// Writes queued blocks in order on its own thread, so reading and decompressing overlap the (slow) SD card. Every
// 64 MiB the written range is flushed and dropped from the page cache, which keeps the console's 1 GB of memory free.
class Writer {
 public:
  Writer(int fd, u64 total) : fd_(fd), total_(total), thread_([this] { run(); }) {}
  ~Writer() {
    try { finish(); } catch (...) {}  // an error already reported (or being unwound) by the reading side
  }
  void push(u64 offset, std::vector<u8> data) {
    std::unique_lock<std::mutex> lock(mutex_);
    space_.wait(lock, [this] { return queue_.size() < 6 || !error_.empty(); });
    if (!error_.empty()) throw std::runtime_error(error_);
    queue_.push_back({offset, std::move(data)});
    ready_.notify_one();
  }
  void finish() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) return;
      closed_ = true;
      ready_.notify_one();
    }
    if (thread_.joinable()) thread_.join();
    if (!error_.empty()) throw std::runtime_error(error_);
  }
  void count(u64 bytes) { report(done_ += bytes); }

 private:
  struct Block {
    u64 offset;
    std::vector<u8> data;
  };
  void report(u64 done) {
    const unsigned percent = total_ ? unsigned(done * 100 / total_) : 0;
    if (percent != last_percent_) {
      last_percent_ = percent;
      // On a terminal one line that updates; into a log or the launcher's pipe, one line per percent.
      printf(isatty(STDOUT_FILENO) ? "\r%3u%% (%llu / %llu MiB)" : "%3u%% (%llu / %llu MiB)\n", percent,
             (unsigned long long)(done >> 20), (unsigned long long)(total_ >> 20));
      fflush(stdout);
    }
  }
  void run() {
    u64 flushed_from = 0, since_flush = 0, last_end = 0;
    while (true) {
      Block block;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return !queue_.empty() || closed_; });
        if (queue_.empty()) return;
        block = std::move(queue_.front());
        queue_.pop_front();
        space_.notify_one();
      }
      try {
        size_t at = 0;
        while (at < block.data.size()) {
          const ssize_t wrote = pwrite(fd_, block.data.data() + at, block.data.size() - at, off_t(block.offset + at));
          if (wrote < 0 && errno == EINTR) continue;
          if (wrote < 0) {
            if (errno == ENOSPC) fail("no queda espacio en la tarjeta para game.pak");
            if (errno == EFBIG) fail("la tarjeta no admite ficheros de mas de 4 GB (FAT32): usa una particion exFAT o ext4");
            fail("error escribiendo game.pak: %s", strerror(errno));
          }
          at += size_t(wrote);
        }
        last_end = block.offset + block.data.size();
        since_flush += block.data.size();
        if (since_flush >= (64u << 20) && last_end > flushed_from) {
#ifdef SYNC_FILE_RANGE_WRITE
          sync_file_range(fd_, off_t(flushed_from), off_t(last_end - flushed_from),
                          SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER);
#endif
#ifdef POSIX_FADV_DONTNEED
          posix_fadvise(fd_, off_t(flushed_from), off_t(last_end - flushed_from), POSIX_FADV_DONTNEED);
#endif
          flushed_from = last_end;
          since_flush = 0;
        }
        count(block.data.size());
      } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = error.what();
        queue_.clear();
        space_.notify_all();
        return;
      }
    }
  }
  int fd_;
  u64 total_;
  std::atomic<u64> done_{0};
  unsigned last_percent_ = ~0u;
  std::mutex mutex_;
  std::condition_variable ready_, space_;
  std::deque<Block> queue_;
  bool closed_ = false;
  std::string error_;
  std::thread thread_;
};

// Before writing 4 GB: a filesystem that cannot hold the file (FAT32) or too little free space fail now, not at 90 %.
void check_destination(int fd, u64 logical_size, u64 data_bytes) {
  struct statfs fs;
  if (fstatfs(fd, &fs) != 0) return;
  const unsigned long type = (unsigned long)fs.f_type;
  if (type == 0x4d44 && logical_size > 0xFFFFFFFFull) {  // MSDOS_SUPER_MAGIC
    fail("la tarjeta esta en FAT32, que no admite ficheros de mas de 4 GB (game.pak mide %llu MiB): usa la "
         "particion de KNULLI (exFAT o ext4)", (unsigned long long)(logical_size >> 20));
  }
  // ext4, btrfs, xfs, f2fs and tmpfs keep the holes free; exFAT and the rest store them.
  const bool sparse = type == 0xEF53 || type == 0x9123683E || type == 0x58465342 || type == 0xF2F52010 || type == 0x01021994;
  const u64 need = (sparse ? data_bytes : logical_size) + (16u << 20);
  const u64 free_bytes = u64(fs.f_bavail) * u64(fs.f_bsize);
  if (free_bytes < need) {
    fail("no hay espacio para game.pak: hacen falta %llu MiB libres y hay %llu MiB", (unsigned long long)(need >> 20),
         (unsigned long long)(free_bytes >> 20));
  }
}

std::string temporary_path(const std::string& output) {
  const size_t slash = output.rfind('/');
  const std::string dir = slash == std::string::npos ? std::string() : output.substr(0, slash + 1);
  const std::string name = slash == std::string::npos ? output : output.substr(slash + 1);
  if (name.empty()) fail("el destino no tiene nombre de fichero");
  return dir + "." + name + ".importing";
}

void make_parents(const std::string& path) {
  for (size_t at = path.find('/', 1); at != std::string::npos; at = path.find('/', at + 1)) {
    const std::string dir = path.substr(0, at);
    if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) fail("no se puede crear %s: %s", dir.c_str(), strerror(errno));
  }
}

struct GamePartition {
  u64 offset = 0, data_offset = 0, data_size = 0;
  u8 header[0x2C0] = {};
  std::vector<u8> tmd;
  u8 key[16] = {};
};

// The game partition (type 0) from the partition table, its header, TMD and title key.
GamePartition locate_partition(Disc& disc) {
  GamePartition gp;
  u8 groups[32];
  disc.read(0x40000, sizeof(groups), groups);
  for (int g = 0; g < 4 && gp.offset == 0; ++g) {
    const u32 count = be32(groups + 8 * g);
    const u64 table = u64(be32(groups + 8 * g + 4)) << 2;
    if (count == 0 || count > 64) continue;
    std::vector<u8> entries(size_t(count) * 8);
    disc.read(table, entries.size(), entries.data());
    for (u32 i = 0; i < count; ++i) {
      if (be32(entries.data() + 8 * i + 4) == 0) { gp.offset = u64(be32(entries.data() + 8 * i)) << 2; break; }
    }
  }
  if (gp.offset == 0) fail("el disco no tiene particion de juego");
  disc.read(gp.offset, sizeof(gp.header), gp.header);
  const u32 tmd_size = be32(gp.header + 0x2A4);
  const u64 tmd_offset = u64(be32(gp.header + 0x2A8)) << 2;
  gp.data_offset = u64(be32(gp.header + 0x2B8)) << 2;
  gp.data_size = u64(be32(gp.header + 0x2BC)) << 2;
  if (tmd_size == 0 || tmd_size > 0x10000) fail("la particion del juego no tiene metadatos validos");
  gp.tmd.resize(tmd_size);
  disc.read(gp.offset + tmd_offset, gp.tmd.size(), gp.tmd.data());
  if (gp.header[0x1F1] == 0) {
    u8 iv[16] = {};
    memcpy(iv, gp.header + 0x1DC, 8);
    Aes128Decrypt(kCommonKey).cbc(iv, gp.header + 0x1BF, gp.key, 16);
  }
  return gp;
}

std::unique_ptr<PartitionData> open_partition_data(Disc& disc, const GamePartition& gp) {
  if (auto* wia = dynamic_cast<WiaDisc*>(&disc)) {
    std::unique_ptr<PartitionData> data = wia->partition_data(gp.offset + gp.data_offset);
    if (!data) fail("el %s no guarda la particion del juego descifrada", disc.format());
    return data;
  }
  if (gp.header[0x1F1] != 0) fail("la particion del juego usa una clave comun que no es la de Wii (indice %u)", gp.header[0x1F1]);
  return std::make_unique<EncryptedPartitionData>(disc, gp.offset + gp.data_offset, gp.data_size, gp.key);
}

std::vector<u8> read_dol(PartitionData& data) {
  u8 boot[0x440];
  data.read(0, sizeof(boot), boot);
  const u64 offset = u64(be32(boot + 0x420)) << 2;
  u8 header[0x100];
  data.read(offset, sizeof(header), header);
  u64 size = 0x100;
  for (int i = 0; i < 18; ++i) {
    const u32 o = be32(header + 4 * i), n = be32(header + 0x90 + 4 * i);
    if (n != 0) size = std::max<u64>(size, u64(o) + n);
  }
  if (size > (64u << 20)) fail("main.dol tiene un tamano inesperado");
  std::vector<u8> dol(size);
  data.read(offset, dol.size(), dol.data());
  return dol;
}

// SIGTERM/SIGINT (Cactus Galaxy Wrapper's "Cancelar"): stop between blocks, removing the unfinished file.
std::atomic<bool> g_cancel{false};
void on_cancel(int) { g_cancel.store(true); }

int identify(const std::string& image) {
  std::unique_ptr<Disc> disc = open_disc(image);
  u8 header[0x60];
  disc->read(0, sizeof(header), header);
  std::string title(reinterpret_cast<const char*>(header + 0x20), strnlen(reinterpret_cast<const char*>(header + 0x20), 0x40));
  for (char& c : title) if (c == '\t' || c == '\n') c = ' ';
  const bool wii = be32(header + 0x18) == 0x5D1C9EA3;
  const std::string game_id(reinterpret_cast<const char*>(header), 6);
  // One line for programs: id, format, Wii or GC, title. Exit 0 only for the game this port plays.
  printf("%s\t%s\t%s\t%s\n", game_id.c_str(), disc->format(), wii ? "Wii" : "GC", title.c_str());
  return wii && game_id == kGameId ? 0 : 1;
}

int run(const std::string& image, const std::string& output, const std::string& patch_path) {
  aes_init();
  std::unique_ptr<Disc> disc = open_disc(image);
  u8 header[0x60];
  disc->read(0, sizeof(header), header);
  const std::string game_id(reinterpret_cast<const char*>(header), 6);
  std::string title(reinterpret_cast<const char*>(header + 0x20), strnlen(reinterpret_cast<const char*>(header + 0x20), 0x40));
  if (be32(header + 0x18) != 0x5D1C9EA3) {
    printf("%s es un juego de GameCube, no Super Mario Galaxy.\n", title.c_str());
    return 1;
  }
  if (game_id != kGameId) {
    printf("%s (%s) no es el Super Mario Galaxy norteamericano (RMGE01).\n", title.c_str(), game_id.c_str());
    return 1;
  }
  printf("Super Mario Galaxy (%s), %s\n", game_id.c_str(), disc->format());
  fflush(stdout);

  GamePartition source_partition = locate_partition(*disc);
  std::unique_ptr<PartitionData> source_data = open_partition_data(*disc, source_partition);
  PartitionData* data = source_data.get();
  GamePartition gp = source_partition;
  std::unique_ptr<XdeltaPatch> patch;
  std::unique_ptr<PatchedDisc> patched;
  std::unique_ptr<PatchedPartitionData> patched_data;
  if (!patch_path.empty()) {
    // The patch is for the original disc: check that first (main.dol), then read the game through the patch.
    const std::string original = Sha1::hex(read_dol(*source_data).data(), read_dol(*source_data).size());
    const char* expected_original = getenv("NEBULA_PAK_DOL_SHA1");
    if (original != (expected_original ? expected_original : kDolSha1)) {
      fail("main.dol no es el de Super Mario Galaxy (SHA-1 %s): la imagen puede estar danada", original.c_str());
    }
    patch = std::make_unique<XdeltaPatch>(patch_path);
    printf("Parche: %s (%zu de %zu ventanas cambian el disco)\n", patch_path.c_str(), patch->modified_windows(),
           patch->windows().size());
    fflush(stdout);
    patched = std::make_unique<PatchedDisc>(*disc, *patch);
    gp = locate_partition(*patched);
    if (gp.offset != source_partition.offset || gp.data_offset != source_partition.data_offset ||
        gp.data_size != source_partition.data_size) {
      fail("el parche mueve la particion del juego (no soportado)");
    }
    patched_data = std::make_unique<PatchedPartitionData>(*source_data, *patched, gp.offset + gp.data_offset, gp.data_size,
                                                          gp.key);
    data = patched_data.get();
  }
  const std::vector<u8>& tmd = gp.tmd;
  const u64 data_size = gp.data_size;

  auto read_vec = [&](u64 offset, size_t size) {
    std::vector<u8> v(size);
    data->read(offset, size, v.data());
    return v;
  };
  const std::vector<u8> boot = read_vec(0, 0x440);
  if (memcmp(boot.data(), kGameId, 6) != 0) {
    fail("la cabecera de la particion no es la de RMGE01 (¿clave o imagen danada?)");
  }
  const std::vector<u8> bi2 = read_vec(0x440, 0x2000);
  const std::vector<u8> apploader_header = read_vec(0x2440, 0x20);
  const u64 apploader_size = 0x20 + u64(be32(apploader_header.data() + 0x14)) + be32(apploader_header.data() + 0x18);
  if (apploader_size > (16u << 20)) fail("el apploader de la particion tiene un tamano inesperado");
  const std::vector<u8> apploader = read_vec(0x2440, size_t(apploader_size));
  const u64 dol_offset = u64(be32(boot.data() + 0x420)) << 2;
  const u64 fst_offset = u64(be32(boot.data() + 0x424)) << 2;
  const u64 fst_size = u64(be32(boot.data() + 0x428)) << 2;
  const u64 fst_max = u64(be32(boot.data() + 0x42C)) << 2;
  if (dol_offset == 0 || fst_offset == 0 || fst_max < fst_size || fst_size > (64u << 20)) {
    fail("la cabecera de la particion no cuadra con su tabla de ficheros");
  }
  const std::vector<u8> dol_header = read_vec(dol_offset, 0x100);
  u64 dol_size = 0x100;
  for (int i = 0; i < 18; ++i) {
    const u32 offset = be32(dol_header.data() + 4 * i), size = be32(dol_header.data() + 0x90 + 4 * i);
    if (size != 0) dol_size = std::max<u64>(dol_size, u64(offset) + size);
  }
  if (dol_size > (64u << 20)) fail("main.dol tiene un tamano inesperado");
  const std::vector<u8> dol = read_vec(dol_offset, size_t(dol_size));
  const char* expected = getenv("NEBULA_PAK_DOL_SHA1");  // tests with synthetic discs only
  const std::string dol_sha1 = Sha1::hex(dol.data(), dol.size());
  if (dol_sha1 != (expected ? expected : kDolSha1)) {
    if (!patch) fail("main.dol no es el de Super Mario Galaxy (SHA-1 %s): la imagen puede estar danada", dol_sha1.c_str());
    printf("El parche cambia main.dol (SHA-1 %s)\n", dol_sha1.c_str());
  }
  const std::vector<u8> fst = read_vec(fst_offset, size_t(fst_size));
  validate_tmd(tmd);
  std::vector<FileEntry> files = parse_fst(fst);

  struct System {
    const char* label;
    u64 offset;
    const std::vector<u8>* bytes;
  };
  const System system[5] = {{"boot.bin", 0, &boot}, {"bi2.bin", 0x440, &bi2}, {"apploader.img", 0x2440, &apploader},
                            {"main.dol", dol_offset, &dol}, {"fst.bin", fst_offset, &fst}};
  std::vector<Extent> extents;
  u64 total = 0;
  for (const System& s : system) {
    extents.push_back({s.label, s.offset, s.offset + s.bytes->size()});
    total += s.bytes->size();
  }
  if (fst_max > fst_size) extents.push_back({"la reserva de la tabla de ficheros", fst_offset + fst_size, fst_offset + fst_max});
  for (const FileEntry& f : files) {
    extents.push_back({f.name, f.offset, f.offset + f.length});
    total += f.length;
  }
  const u64 logical_size = validate_extents(extents, kPartitionDataBytes);
  if (files.empty() || logical_size > data_size / 0x8000 * 0x7C00) {
    fail("la tabla de ficheros apunta fuera de la particion (¿imagen recortada?)");
  }

  make_parents(output);
  const std::string temporary = temporary_path(output);
  const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) fail("no se puede crear %s: %s", temporary.c_str(), strerror(errno));
  try {
    check_destination(fd, logical_size, total);
    Writer writer(fd, total);
    for (const System& s : system) writer.push(s.offset, *s.bytes);
    std::sort(files.begin(), files.end(), [](const FileEntry& a, const FileEntry& b) { return a.offset < b.offset; });
    constexpr size_t kChunk = 4u << 20;
    for (const FileEntry& f : files) {
      for (u64 at = 0; at < f.length; at += kChunk) {
        if (g_cancel.load()) fail("conversion cancelada");
        const size_t take = size_t(std::min<u64>(kChunk, f.length - at));
        std::vector<u8> buffer(take);
        try {
          data->read(f.offset + at, take, buffer.data());
        } catch (const std::exception& error) {
          fail("%s no se puede leer del disco: %s", f.name.c_str(), error.what());
        }
        writer.push(f.offset + at, std::move(buffer));
      }
    }
    writer.finish();
    // Written in ascending order with holes left unwritten (sparse on ext4; filled by exFAT): no 4 GB of zeros
    // written up front. The end is set last, in case the table ends past the last file.
    if (ftruncate(fd, off_t(logical_size)) != 0) fail("no se puede dimensionar game.pak: %s", strerror(errno));
    if (fsync(fd) != 0) fail("game.pak no se puede guardar en la tarjeta: %s", strerror(errno));
    struct stat st;
    if (fstat(fd, &st) != 0 || u64(st.st_size) != logical_size) fail("game.pak quedo con un tamano inesperado");
    close(fd);
  } catch (...) {
    close(fd);
    unlink(temporary.c_str());
    throw;
  }
  if (rename(temporary.c_str(), output.c_str()) != 0) {
    const int error = errno;
    unlink(temporary.c_str());
    fail("no se puede reemplazar %s: %s", output.c_str(), strerror(error));
  }
  printf("\ngame.pak listo: %llu MiB de datos\n", (unsigned long long)(logical_size >> 20));
  if (patched_data) {
    // What the patch changed: the files over clusters that differ from the original, and the game language whose
    // files those are (UsEnglish, UsSpanish, UsFrench), for the launcher.
    std::vector<std::pair<u64, u64>> changed;  // data ranges
    std::vector<u8> a(0x7C00), b(0x7C00);
    for (u64 c = 0; c < patched_data->clusters(); ++c) {
      if (!patched_data->touched(c)) continue;
      try {
        patched_data->read(c * 0x7C00, a.size(), a.data());
        source_data->read(c * 0x7C00, b.size(), b.data());
      } catch (const std::exception&) {
        continue;  // a cluster no file uses (scrubbed source): not the patch's business
      }
      if (a == b) continue;
      if (!changed.empty() && changed.back().second == c * 0x7C00) changed.back().second += 0x7C00;
      else changed.push_back({c * 0x7C00, (c + 1) * 0x7C00});
    }
    int english = 0, spanish = 0, french = 0, listed = 0;
    for (const FileEntry& f : files) {
      bool hit = false;
      for (const auto& r : changed) if (f.length && f.offset < r.second && f.offset + f.length > r.first) hit = true;
      if (!hit) continue;
      if (listed++ < 64) printf("PATCHED\t%s\n", f.name.c_str());
      const std::string l = [&] { std::string x = f.name; for (char& ch : x) ch = char(tolower((unsigned char)ch)); return x; }();
      english += l.find("usenglish") != std::string::npos;
      spanish += l.find("usspanish") != std::string::npos;
      french += l.find("usfrench") != std::string::npos;
    }
    const char* language = english >= spanish && english >= french ? (english ? "english" : nullptr)
                                                                     : (spanish >= french ? "spanish" : "french");
    printf("Parche aplicado: %d ficheros cambiados\n", listed);
    if (language) printf("PATCH_LANGUAGE\t%s\n", language);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  const bool identify_only = argc == 3 && strcmp(argv[1], "--identify") == 0;
  std::string patch_path;
  if (argc == 5 && strcmp(argv[1], "--patch") == 0) {
    patch_path = argv[2];
    argv += 2;
    argc -= 2;
  }
  if (argc == 3 && strcmp(argv[1], "--patch-info") == 0) {  // what an xdelta patch changes
    try {
      XdeltaPatch patch(argv[2]);
      printf("ventanas %zu, cambian %zu, disco de %llu bytes\n", patch.windows().size(), patch.modified_windows(),
             (unsigned long long)patch.target_size());
      for (const auto& w : patch.windows()) {
        if (!w.identity) printf("  %#llx +%#x (%zu bytes nuevos)\n", (unsigned long long)w.target, w.length, w.adds_len);
      }
      return 0;
    } catch (const std::exception& error) {
      fprintf(stderr, "Error: %s\n", error.what());
      return 1;
    }
  }
  if (argc == 6 && strcmp(argv[1], "--dump-raw") == 0) {  // tests: the disc's raw (encrypted) bytes
    try {
      aes_init();
      std::unique_ptr<Disc> disc = open_disc(argv[2]);
      const u64 offset = strtoull(argv[3], nullptr, 0), size = strtoull(argv[4], nullptr, 0);
      std::vector<u8> buf(size);
      disc->read_raw(offset, buf.size(), buf.data());
      FILE* f = fopen(argv[5], "wb");
      if (!f || fwrite(buf.data(), 1, buf.size(), f) != buf.size()) return 1;
      fclose(f);
      return 0;
    } catch (const std::exception& error) {
      fprintf(stderr, "Error: %s\n", error.what());
      return 1;
    }
  }
  if (argc != 3) {
    fprintf(stderr, "uso: nebula-pak <imagen del disco> <game.pak>\n"
                    "     nebula-pak --patch <parche.xdelta> <imagen del disco> <game.pak>\n"
                    "     nebula-pak --identify <imagen del disco>\n"
                    "  imagen: ISO, RVZ, WIA, WBFS, GCZ o CISO de Super Mario Galaxy (USA, RMGE01)\n");
    return 2;
  }
  signal(SIGTERM, on_cancel);
  signal(SIGINT, on_cancel);
  try {
    if (identify_only) return identify(argv[2]);
    return run(argv[1], argv[2], patch_path);
  } catch (const std::exception& error) {
    fflush(stdout);
    fprintf(stderr, "\nError: %s\n", error.what());
    return identify_only ? 3 : 1;
  }
}
