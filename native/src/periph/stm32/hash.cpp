#include "dma.hpp"
#include "dma_signal.hpp"
#include "factories.hpp"
#include "hash_wide.hpp"
#include "model_base.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
namespace
{
constexpr std::uint32_t busy_done     = 0;
constexpr std::uint32_t digest_done   = 1;
constexpr std::uint64_t ps_per_second = 1'000'000'000'000;
constexpr std::size_t   block_words   = 16;
constexpr std::size_t   max_block     = 42;
constexpr std::size_t   inner_words   = 16;
constexpr std::size_t   hra_count     = 5;
constexpr std::uint32_t swap_tag      = 0x4853'0001;
constexpr std::size_t   csr_config    = 1;
constexpr std::size_t   csr_blocks    = 2;
constexpr std::size_t   csr_chain     = 4;
constexpr std::size_t   csr_pending   = 12;
constexpr std::size_t   csr_wide_pend = 20;
constexpr std::size_t   csr_last      = 54;

constexpr std::uint8_t phase_inner_key = 0;
constexpr std::uint8_t phase_message   = 1;
constexpr std::uint8_t phase_outer_key = 2;

constexpr std::uint8_t suspend_none     = 0;
constexpr std::uint8_t suspend_held     = 1;
constexpr std::uint8_t suspend_launched = 2;

constexpr std::uint32_t ipad = 0x3636'3636;
constexpr std::uint32_t opad = 0x5C5C'5C5C;

using chain = std::array<std::uint32_t, 8>;
using block = std::span<std::uint32_t const, block_words>;
using words = std::array<std::uint32_t, max_block>;

enum class function : std::uint8_t
{
  sha1,
  md5,
  sha256,
  sha512,
  keccak,
};

/**
 * @brief One ALGO code of a HASH family.
 */
struct algorithm
{
  function      kind;
  std::uint8_t  block_words;
  std::uint8_t  digest_words;
  std::uint8_t  init;
  std::uint8_t  csr_inner;
  std::uint8_t  suffix;
  std::uint16_t cycles;
  std::uint8_t  tail;
  std::uint8_t  lag;
  bool          hmac;
  bool          extendable;
  bool          absent;
};

/**
 * @brief The ALGO codes and register counts of one HASH IP version.
 */
struct hash_family
{
  std::span<algorithm const> algorithms;
  std::size_t                hr_count;
  std::size_t                csr_count;
};

constexpr std::array<chain, 4> initial_values{
    {
     {0x6745'2301, 0xEFCD'AB89, 0x98BA'DCFE, 0x1032'5476, 0xC3D2'E1F0},
     {0x6745'2301, 0xEFCD'AB89, 0x98BA'DCFE, 0x1032'5476},
     {0xC105'9ED8, 0x367C'D507, 0x3070'DD17, 0xF70E'5939, 0xFFC0'0B31, 0x6858'1511, 0x64F9'8FA7,
         0xBEFA'4FA4},
     {0x6A09'E667, 0xBB67'AE85, 0x3C6E'F372, 0xA54F'F53A, 0x510E'527F, 0x9B05'688C, 0x1F83'D9AB,
         0x5BE0'CD19},
     }
};

constexpr std::array<std::uint32_t, 64> sha256_k{
    0x428A'2F98, 0x7137'4491, 0xB5C0'FBCF, 0xE9B5'DBA5, 0x3956'C25B, 0x59F1'11F1, 0x923F'82A4,
    0xAB1C'5ED5, 0xD807'AA98, 0x1283'5B01, 0x2431'85BE, 0x550C'7DC3, 0x72BE'5D74, 0x80DE'B1FE,
    0x9BDC'06A7, 0xC19B'F174, 0xE49B'69C1, 0xEFBE'4786, 0x0FC1'9DC6, 0x240C'A1CC, 0x2DE9'2C6F,
    0x4A74'84AA, 0x5CB0'A9DC, 0x76F9'88DA, 0x983E'5152, 0xA831'C66D, 0xB003'27C8, 0xBF59'7FC7,
    0xC6E0'0BF3, 0xD5A7'9147, 0x06CA'6351, 0x1429'2967, 0x27B7'0A85, 0x2E1B'2138, 0x4D2C'6DFC,
    0x5338'0D13, 0x650A'7354, 0x766A'0ABB, 0x81C2'C92E, 0x9272'2C85, 0xA2BF'E8A1, 0xA81A'664B,
    0xC24B'8B70, 0xC76C'51A3, 0xD192'E819, 0xD699'0624, 0xF40E'3585, 0x106A'A070, 0x19A4'C116,
    0x1E37'6C08, 0x2748'774C, 0x34B0'BCB5, 0x391C'0CB3, 0x4ED8'AA4A, 0x5B9C'CA4F, 0x682E'6FF3,
    0x748F'82EE, 0x78A5'636F, 0x84C8'7814, 0x8CC7'0208, 0x90BE'FFFA, 0xA450'6CEB, 0xBEF9'A3F7,
    0xC671'78F2,
};

constexpr std::array<std::uint32_t, 64> md5_k{
    0xD76A'A478, 0xE8C7'B756, 0x2420'70DB, 0xC1BD'CEEE, 0xF57C'0FAF, 0x4787'C62A, 0xA830'4613,
    0xFD46'9501, 0x6980'98D8, 0x8B44'F7AF, 0xFFFF'5BB1, 0x895C'D7BE, 0x6B90'1122, 0xFD98'7193,
    0xA679'438E, 0x49B4'0821, 0xF61E'2562, 0xC040'B340, 0x265E'5A51, 0xE9B6'C7AA, 0xD62F'105D,
    0x0244'1453, 0xD8A1'E681, 0xE7D3'FBC8, 0x21E1'CDE6, 0xC337'07D6, 0xF4D5'0D87, 0x455A'14ED,
    0xA9E3'E905, 0xFCEF'A3F8, 0x676F'02D9, 0x8D2A'4C8A, 0xFFFA'3942, 0x8771'F681, 0x6D9D'6122,
    0xFDE5'380C, 0xA4BE'EA44, 0x4BDE'CFA9, 0xF6BB'4B60, 0xBEBF'BC70, 0x289B'7EC6, 0xEAA1'27FA,
    0xD4EF'3085, 0x0488'1D05, 0xD9D4'D039, 0xE6DB'99E5, 0x1FA2'7CF8, 0xC4AC'5665, 0xF429'2244,
    0x432A'FF97, 0xAB94'23A7, 0xFC93'A039, 0x655B'59C3, 0x8F0C'CC92, 0xFFEF'F47D, 0x8584'5DD1,
    0x6FA8'7E4F, 0xFE2C'E6E0, 0xA301'4314, 0x4E08'11A1, 0xF753'7E82, 0xBD3A'F235, 0x2AD7'D2BB,
    0xEB86'D391,
};

constexpr std::array<int, 16> md5_shifts{7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};

constexpr algorithm sha1_code{function::sha1, 16, 5, 0, 38, 0, 82, 0, 0, true, false, false};
constexpr algorithm h5_sha1_code{function::sha1, 16, 5, 0, 38, 0, 82, 15, 11, true, false, false};
constexpr algorithm md5_code{function::md5, 16, 4, 1, 38, 0, 66, 0, 0, true, false, false};
constexpr algorithm sha224_code{function::sha256, 16, 7, 2, 38, 0, 66, 0, 0, true, false, false};
constexpr algorithm sha256_code{function::sha256, 16, 8, 3, 38, 0, 66, 0, 0, true, false, false};
constexpr algorithm h5_sha224_code{
    function::sha256, 16, 7, 2, 38, 0, 64, 15, 11, true, false, false};
constexpr algorithm h5_sha256_code{
    function::sha256, 16, 8, 3, 38, 0, 64, 15, 11, true, false, false};
constexpr algorithm sha384_code{function::sha512, 32, 12, 0, 52, 0, 82, 27, 11, true, false, false};
constexpr algorithm sha512_224_code{
    function::sha512, 32, 7, 2, 52, 0, 82, 27, 11, true, false, false};
constexpr algorithm sha512_256_code{
    function::sha512, 32, 8, 3, 52, 0, 82, 27, 11, true, false, false};
constexpr algorithm sha512_code{function::sha512, 32, 16, 1, 52, 0, 82, 27, 11, true, false, false};
constexpr algorithm sha3_224_code{
    function::keccak, 36, 7, 0, 55, 0x06, 60, 0, 0, true, false, false};
constexpr algorithm sha3_256_code{
    function::keccak, 34, 8, 0, 55, 0x06, 58, 0, 0, true, false, false};
constexpr algorithm sha3_384_code{
    function::keccak, 26, 12, 0, 72, 0x06, 50, 0, 0, true, false, false};
constexpr algorithm sha3_512_code{
    function::keccak, 18, 16, 0, 72, 0x06, 42, 0, 0, true, false, false};
constexpr algorithm shake128_code{
    function::keccak, 42, 42, 0, 0, 0x1F, 66, 0, 0, false, true, false};
constexpr algorithm shake256_code{
    function::keccak, 34, 34, 0, 0, 0x1F, 58, 0, 0, false, true, false};
constexpr algorithm absent_code{function::sha1, 16, 5, 0, 38, 0, 82, 15, 11, true, false, true};

constexpr std::array<algorithm, 4> u5_codes{sha1_code, md5_code, sha224_code, sha256_code};

constexpr std::array<algorithm, 16> h5_codes{
    h5_sha1_code, absent_code,     h5_sha224_code,  h5_sha256_code, absent_code, absent_code,
    absent_code,  absent_code,     absent_code,     absent_code,    absent_code, absent_code,
    sha384_code,  sha512_224_code, sha512_256_code, sha512_code,
};

constexpr std::array<algorithm, 16> h5_sha3_codes{
    h5_sha1_code,  absent_code,     h5_sha224_code,  h5_sha256_code, sha3_224_code, sha3_256_code,
    sha3_384_code, sha3_512_code,   shake128_code,   shake256_code,  absent_code,   absent_code,
    sha384_code,   sha512_224_code, sha512_256_code, sha512_code,
};

constexpr hash_family u5_hash{u5_codes, 8, 54};
constexpr hash_family h5_hash{h5_codes, 42, 103};
constexpr hash_family h5_sha3_hash{h5_sha3_codes, 42, 103};

/**
 * @brief Compresses one 512-bit block into a SHA-1 chaining value, FIPS 180-4 6.1.2.
 * @param h Chaining value, words 0 to 4.
 * @param m Message block as big-endian words.
 */
constexpr void sha1_compress(chain& h, block m) noexcept
{
  std::array<std::uint32_t, 80> w{};
  std::ranges::copy(m, w.begin());
  for(std::size_t t = 16; t < 80; ++t)
  {
    w[t] = std::rotl(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
  }
  auto a = h[0];
  auto b = h[1];
  auto c = h[2];
  auto d = h[3];
  auto e = h[4];
  for(std::size_t t = 0; t < 80; ++t)
  {
    std::uint32_t f = 0;
    std::uint32_t k = 0;
    if(t < 20)
    {
      f = (b & c) | (~b & d);
      k = 0x5A82'7999;
    }
    else if(t < 40)
    {
      f = b ^ c ^ d;
      k = 0x6ED9'EBA1;
    }
    else if(t < 60)
    {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1B'BCDC;
    }
    else
    {
      f = b ^ c ^ d;
      k = 0xCA62'C1D6;
    }
    auto const temp = std::rotl(a, 5) + f + e + k + w[t];
    e               = d;
    d               = c;
    c               = std::rotl(b, 30);
    b               = a;
    a               = temp;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
}

/**
 * @brief Compresses one 512-bit block into a SHA-224 or SHA-256 chaining value, FIPS 180-4
 *        6.2.2.
 * @param h Chaining value.
 * @param m Message block as big-endian words.
 */
constexpr void sha256_compress(chain& h, block m) noexcept
{
  std::array<std::uint32_t, 64> w{};
  std::ranges::copy(m, w.begin());
  for(std::size_t t = 16; t < 64; ++t)
  {
    auto const s0 = std::rotr(w[t - 15], 7) ^ std::rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
    auto const s1 = std::rotr(w[t - 2], 17) ^ std::rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
    w[t]          = w[t - 16] + s0 + w[t - 7] + s1;
  }
  auto v = h;
  for(std::size_t t = 0; t < 64; ++t)
  {
    auto const& [a, b, c, d, e, f, g, x] = v;
    auto const s1                        = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
    auto const ch                        = (e & f) ^ (~e & g);
    auto const temp1                     = x + s1 + ch + sha256_k[t] + w[t];
    auto const s0                        = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
    auto const maj                       = (a & b) ^ (a & c) ^ (b & c);
    v                                    = {temp1 + s0 + maj, a, b, c, d + temp1, e, f, g};
  }
  for(std::size_t i = 0; i < h.size(); ++i)
  {
    h[i] += v[i];
  }
}

/**
 * @brief Compresses one 512-bit block into an MD5 chaining value, RFC 1321 3.4.
 * @param h Chaining value, words 0 to 3, A to D.
 * @param m Message block as big-endian words, byte-swapped here to RFC 1321's little-endian
 *        words.
 */
constexpr void md5_compress(chain& h, block m) noexcept
{
  auto a = h[0];
  auto b = h[1];
  auto c = h[2];
  auto d = h[3];
  for(std::size_t i = 0; i < 64; ++i)
  {
    std::uint32_t f = 0;
    std::size_t   g = 0;
    switch(i / 16)
    {
      case 0:
        f = (b & c) | (~b & d);
        g = i;
        break;
      case 1:
        f = (d & b) | (~d & c);
        g = (5 * i + 1) % 16;
        break;
      case 2:
        f = b ^ c ^ d;
        g = (3 * i + 5) % 16;
        break;
      default:
        f = c ^ (b | ~d);
        g = (7 * i) % 16;
        break;
    }
    f += a + md5_k[i] + std::byteswap(m[g]);
    a = d;
    d = c;
    c = b;
    b += std::rotl(f, md5_shifts[(i / 16) * 4 + i % 4]);
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
}

constexpr auto inner_width(algorithm const& a) noexcept -> std::size_t
{
  return a.digest_words > 8 ? inner_words : 8;
}

constexpr auto state_end(algorithm const& a) noexcept -> std::size_t
{
  switch(a.kind)
  {
    case function::sha512:
      return csr_wide_pend + 32;
    case function::keccak:
      return csr_last + 1;
    default:
      return csr_pending + block_words;
  }
}

constexpr auto inner_clear(hash_family const& family) noexcept -> bool
{
  return std::ranges::all_of(family.algorithms, [&](algorithm const& a) {
    return not a.hmac
        or (a.csr_inner >= state_end(a) and a.csr_inner + inner_width(a) <= family.csr_count);
  });
}

static_assert(inner_clear(u5_hash));
static_assert(inner_clear(h5_hash));
static_assert(inner_clear(h5_sha3_hash));

/**
 * @brief The DIN word as message bits in big-endian order, RM0456 Figure 502.
 * @param value Word written to DIN.
 * @param datatype DATATYPE field, 00 none, 01 half-words swapped, 10 bytes swapped, 11 bits
 *        reversed.
 */
constexpr auto swapped(std::uint32_t value, std::uint32_t datatype) noexcept -> std::uint32_t
{
  switch(datatype)
  {
    case 1:
      return std::rotl(value, 16);
    case 2:
      return std::byteswap(value);
    case 3:
    {
      std::uint32_t out = 0;
      for(unsigned bit = 0; bit < 32; ++bit)
      {
        out = (out << 1) | ((value >> bit) & 1u);
      }
      return out;
    }
    default:
      return value;
  }
}

static_assert(swapped(0x1122'3344, 1) == 0x3344'1122);
static_assert(swapped(0x1122'3344, 2) == 0x4433'2211);
static_assert(swapped(0x0000'0001, 3) == 0x8000'0000);

struct hash_state
{
  std::uint8_t                             algo{};
  chain                                    h{};
  std::array<std::uint64_t, keccak::lanes> lanes{};
  std::uint64_t                            blocks{};
  words                                    pending{};
  std::uint8_t                             count{};
  std::uint32_t                            last{};
  words                                    digest{};
  std::uint8_t                             digest_size{};
  std::uint64_t                            busy_end{};
  std::uint64_t                            queued{};
  std::uint64_t                            flagged{};
  std::uint8_t                             busy{};
  std::uint8_t                             final_pending{};
  std::uint8_t                             digested{};
  std::uint8_t                             squeezing{};
  std::uint8_t                             hmac{};
  std::uint8_t                             long_key{};
  std::uint8_t                             phase{};
  std::array<std::uint32_t, inner_words>   inner{};
  std::uint8_t                             refused{};
  std::uint8_t                             suspend{};

  friend auto operator==(hash_state const&, hash_state const&) -> bool = default;
};

/**
 * @brief Hash processor of the STM32U5, RM0456 section 51: SHA-1, MD5, SHA-224 and SHA-256, and
 *        HMAC over them, on words written to DIN by the CPU or by DMA. The STM32H5 processor,
 *        RM0481 section 40, adds SHA2-384, SHA2-512, SHA2-512/224 and SHA2-512/256, on the
 *        STM32H543 and H553 also SHA3-224 to SHA3-512, SHAKE128 and SHAKE256, and drops MD5,
 *        see the H5 paragraph below.
 *
 *        Each DIN word is swapped by the DATATYPE in CR at the time of the write into message
 *        bits in big-endian order. A write that finds 16 words pending compresses them first, so
 *        block k starts on word 16k+1. A DIN write clears DINIS. An STR write with DCAL 1 keeps
 *        the previous NBLW, pads the message with the last word holding NBLW valid bits from its
 *        MSB, NBLW 0 meaning 32, the bits after them ignored, and computes the digest. A DCAL with
 *        no DIN word since the last digest keeps it, as drivers that write DCAL after the
 *        automatic one read the right digest on silicon. INIT latches ALGO, resets the
 *        computation, NBWP to 0, NBWE to 0x11, DINIS to 1, DCIS to 0 and the digest registers to
 *        0, and keeps NBLW. CR keeps every rw field as written, INIT and STR.DCAL read 0, CR.NBW
 *        shows the low four bits of SR.NBWP and CR.DINNE SR.DINNE. ALGO, MODE and LKEY act at INIT
 *        only.
 *
 *        The arithmetic is done at the write, only the flags follow time. Each block keeps BUSY
 *        set for 82 HCLK cycles on SHA-1 and 66 on the others, RM0456 Table 486, the final
 *        digest one block time per block it compresses. Work arriving while BUSY queues behind
 *        the current block, its end time fixed on arrival from the core clock, which is HCLK.
 *        One BUSY end event is pending at a time, moved to the queue's end when it fires early,
 *        and on the STM32H5 one digest event after it, see below. INIT and reset cancel both, so
 *        the queue never holds more than two events of this model.
 *        At the end of a partial block DINIS rises when at most one word is pending and NBWE
 *        reads 17 minus NBWP. At the end of a final digest DCIS and DINIS rise, NBWE reads 0x11
 *        and HR0-7 and HRA0-4 show the digest, digest bytes 4i to 4i+3 in HRi as a big-endian
 *        word. They read 0 while DCIS is 0, and past the algorithm's digest size. SR.DINIS and
 *        SR.DCIS clear by a write of 0. HASH_IRQn follows DINIS with DINIE and DCIS with DCIE.
 *        Accesses narrower than 32 bits return a bus error, RM0456 51.6.
 *
 *        DMA input, RM0456 51.4.9: CR.DMAE drives the hash_in_dma request and SR.DMAS reads it.
 *        The request stays high while DMAE is 1, words never wait. When the channel serving the
 *        request completes with MDMAT 0, DMAE clears and DCAL starts with the NBLW written
 *        before, RM0456 51.4.5 step 3b. With MDMAT 1 the message continues with the next
 *        transfer.
 *
 *        DMA suspend, RM0456 51.4.8: software clearing DMAE while the pending words do not yet
 *        form a launched block keeps the request and SR.DMAS high up to the DIN write that
 *        compresses the block, the one that leaves one word pending. They drop at that write,
 *        and the BUSY end raises DINIS whatever the count, so words of a burst already started
 *        stay pending in the context. With no word pending, DINIS 1, or one word pending behind
 *        a running block, they drop at the CR write. A channel completing while the request is
 *        held drops it too, and with MDMAT 0 starts DCAL as at any transfer end, so DCIS rises
 *        as RM0456 step 4 foresees. Setting DMAE or INIT ends the hold.
 *
 *        HMAC, RM0456 51.4.7: MODE 1 at INIT selects HMAC and latches LKEY. Three phases follow,
 *        each closed by DCAL: the inner key, the message, the outer key. A key phase ends by
 *        compressing the key, zero-padded to 64 bytes and XORed with ipad or opad, the key being
 *        the words written in that phase with NBLW valid bits in the last one, or their digest
 *        with LKEY 1, whatever their length. The message phase keeps its digest, the outer key
 *        phase hashes it after the key block and shows the HMAC. Between phases BUSY ends with
 *        DINIS 1 and DCIS 0, DCIS rises after the outer key only. Every DCAL closes its phase,
 *        an empty message or key included, the automatic one of a DMA transfer too. A DCAL
 *        after the outer key with no word since keeps the HMAC, and words written after it
 *        start a new inner key. That last rule is an emulator choice: RM0456 does not say, the
 *        HAL always writes INIT first. Each phase takes one block time per block it compresses:
 *        the key's digest with LKEY 1, the padded key block, the message's last blocks and the
 *        inner digest's block, not RM0456's approximate factors.
 *
 *        Context swap, RM0456 51.4.8 and 51.6.7: CSR0-53 hold an emulator-defined serialisation of
 *        the state, not silicon's layout, and read 0 while DINIS is 0. CSR0 reads 0x48530001, a tag
 *        and a version, out of reset too, where RM0456 gives 0x00000002. CSR1 holds ALGO bits 1:0
 *        in bits 1:0 and, on the H5, ALGO bits 3:2 in bits 15:14, MODE in bit 2, LKEY in bit 3, the
 *        HMAC phase in bits 5:4 and the pending word count in bits 13:8. CSR2 and CSR3 hold the
 *        count of compressed blocks, low word first, CSR4-11 the chaining value, CSR12-27 the
 *        pending block, CSR38-45 the inner HMAC digest, the others read 0. A hash needs CSR0-37 and
 *        an HMAC CSR0-53, as RM0456 says. Each CSR write loads its word into the state at once,
 *        NBWP, NBWE and DINNE follow CSR1. Restoring IMR, STR, CR, then INIT, then the CSRs resumes
 *        the message where it was saved. NBLW is not part of the context, STR holds it. Reads while
 *        BUSY show the state after the queued work. A CSR1 write while BUSY without INIT first is
 *        outside RM0456's sequence: it replaces the count under the pending BUSY end, which then
 *        sets DINIS and NBWE from the new count.
 *
 *        STM32H5, RM0481 40.4: ALGO is four bits, selecting the algorithm of the variant's code
 *        table, RM0481 Table 397. A CR write of a code outside it stores ALGO 0000, with or without
 *        INIT, and the hash runs SHA-1, as read back on an STM32H573: 0001 and 0100 to 1011 there,
 *        0001, 1010 and 1011 on the STM32H543 and H553. The write logs once and raises a
 *        model_error under strict_peripherals before CR changes. The SHA-2 block is 64 bytes for
 *        SHA-1, SHA2-224 and SHA2-256, 128 bytes for the SHA-512 family with a 128-bit length
 *        field. The SHA-3 block is the sponge rate, Table 399: 144, 136, 104 and 72 bytes, SHAKE128
 *        168 and SHAKE256 136. NBWE reads the block size in words plus 1 after INIT, six bits wide.
 *        A DIN word is XORed into the Keccak state at once, the first message byte of the word in
 *        its most significant byte going to the lowest byte of the lane, and the permutation of a
 *        full rate runs at the next DIN word or at DCAL. DCAL pads with 0x06 for SHA-3 and 0x1F for
 *        SHAKE, 0x80 in the last byte of the rate. NBLW counts valid bits of the last word and
 *        reads bits 2:0 as zero under SHA-3 and SHAKE, the message being bytes. The digest is HR0
 *        to HR6 for SHA3-224, HR0 to HR7, HR0 to HR11 and HR0 to HR15 for SHA3-256, SHA3-384 and
 *        SHA3-512, and the rate for SHAKE, byte 0 in HR0[31:24]. HRA0-4 read zero under SHA-3 and
 *        SHAKE. A DCAL with no DIN word since a SHAKE digest runs one more permutation and shows
 *        the next rate bytes. A block keeps BUSY from its launching write for 82 cycles under SHA-1
 *        and the SHA-512 family and 64 under SHA2-224 and SHA2-256, as measured on an STM32H573,
 *        where Table 401 gives 82, 98 and 66. A final digest keeps BUSY 15 cycles past its blocks
 *        under SHA-1, SHA2-224 and SHA2-256 and 27 under the SHA-512 family, and DCIS, DINIS, NBWE
 *        and the digest registers follow 11 cycles after BUSY clears, as measured on an STM32H573
 *        for SHA-1, SHA2-256 and SHA2-512, DCAL to BUSY 0 in 97, 79 and 109 cycles and to DCIS in
 *        108, 90 and 120. SHA2-224 and SHA2-384 to SHA2-512/256 follow the measured algorithm of
 *        their block size. SHA-3 and SHAKE digests take their blocks only, DCIS with BUSY 0.
 *        SHA3-224 to SHA3-512 take 60, 58, 50 and 42, Table 401, and SHAKE128 and SHAKE256 66 and
 *        58 by the same rule, one permutation of a squeeze taking a block time. HMAC takes the
 *        block size as its key size, for every algorithm but SHAKE. Context swap CSRs hold the
 *        Keccak lanes at CSR4-53, the last word at CSR54, and the inner HMAC digest at CSR55 for
 *        SHA3-224 and SHA3-256, CSR72 for SHA3-384 and SHA3-512, the SHA-512 chaining value at
 *        CSR4-19, the pending block at CSR20-51 and the inner digest at CSR52.
 *
 *        Divergence: a DIN write while BUSY does not stall the bus, the CPU runs on.
 *
 *        Not modelled, each logged once and a model_error under strict_peripherals, thrown before
 *        the write is stored:
 *        - An HMAC key longer than the block with LKEY 0 is hashed as with LKEY 1.
 *        - MODE 1 with SHAKE is ignored.
 *        - A DIN word after a SHAKE digest with no INIT starts a new message.
 *        - A DCAL while BUSY of a SHAKE digest or squeeze squeezes the next rate at once.
 *        - DMAE 1 with no controller serving hash_in_dma requests nothing.
 *        - A CSR0 write other than the tag, or a CSR1 write whose ALGO, MODE or LKEY differ from
 *          those latched at INIT, with a count above the block size in words or a phase out of
 *          range, drops the CSR writes that follow up to the next INIT or CSR0 tag write, without
 *          strict_peripherals.
 *
 *        The RCC clock gate is not modelled.
 */
class hash final : public dispatch_model<hash, hash_state>,
                   public dma_requester,
                   public bus_reset_listener
{
public:
  hash(
      model_context&                     ctx,
      peripheral_desc const&             desc,
      hash_family const&                 family,
      std::span<write_entry<hash> const> writes);

  void reset(register_file& regs, reset_kind kind) override;
  void on_bus_reset() override;
  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
      -> std::expected<std::uint32_t, bus_error> override;
  auto on_write(
      register_file& regs,
      std::uint32_t  offset,
      std::uint8_t   size,
      std::uint32_t  old,
      std::uint32_t  value,
      access_ctx     ctx) -> std::expected<void, bus_error> override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
  void transfer_completed(dma_controller const& by, std::uint32_t request) override;

  void post();
  void withdraw();
  void control_written(std::uint32_t old, std::uint32_t now);
  void start_written(std::uint32_t old, std::uint32_t now);
  void mask_written(std::uint32_t, std::uint32_t);
  void stored(std::uint32_t, std::uint32_t)
  {}

private:
  void               restore_reset_values();
  void               data_written(std::uint32_t value);
  void               status_written(std::uint32_t value);
  void               initialise(std::uint32_t control);
  void               finish_digest();
  [[nodiscard]] auto last_bits() const -> std::uint32_t;
  auto               pad(std::uint32_t last_bits) -> std::size_t;
  auto               pad_keccak(std::uint32_t last_bits) -> std::size_t;
  [[nodiscard]] auto output() const -> words;
  [[nodiscard]] auto alg() const -> algorithm const&;
  [[nodiscard]] auto code_of(std::uint32_t control) const -> algorithm const&;
  [[nodiscard]] auto keccak_selected() const -> bool;
  void               compress_block(std::span<std::uint32_t const> m);
  void               process_full();
  void               push(std::uint32_t word);
  void               xor_word(std::size_t index, std::uint32_t word);
  void               xor_byte(std::size_t index, std::uint8_t value);
  void               drop_garbage(std::uint32_t last_bits);
  void               squeeze();
  void               conclude(std::size_t compressed, bool final);
  [[nodiscard]] auto lane_word(std::size_t index) const -> std::uint32_t;
  void               set_lane_word(std::size_t index, std::uint32_t value);
  [[nodiscard]] auto algo_of_config(std::uint32_t value) const -> std::uint32_t;
  void               restart();
  auto               open_hmac(std::uint32_t pad_word) -> std::size_t;
  [[nodiscard]] auto key_overflows() const -> bool;
  [[nodiscard]] auto block_launched() const -> bool;
  void               calculate();
  void               request();
  void               work(std::uint64_t cycles, bool final);
  void               complete_digest();
  void               finish_work();
  void               publish();
  void               show_digest(bool visible);
  void               interrupts();
  [[nodiscard]] auto is_csr(std::uint32_t offset) const -> bool;
  [[nodiscard]] auto context_word(std::size_t index) const -> std::uint32_t;
  void               context_written(std::size_t index, std::uint32_t value);
  [[nodiscard]] auto config_valid(std::uint32_t value) const -> bool;
  void               load_config(std::uint32_t value);
  void unmodelled(bool& logged, std::string_view name, std::uint32_t offset, std::string message);

  register_file&                       regs_;
  hash_family const&                   family_;
  std::uint32_t                        cr_;
  std::uint32_t                        din_;
  std::uint32_t                        str_;
  std::uint32_t                        sr_;
  std::uint32_t                        csr0_;
  field_ref                            init_;
  field_ref                            dmae_;
  field_ref                            mdmat_;
  field_ref                            dmas_;
  field_ref                            datatype_;
  field_ref                            mode_;
  field_ref                            lkey_;
  field_ref                            algo_;
  field_ref                            nbw_;
  field_ref                            cr_dinne_;
  field_ref                            nblw_;
  field_ref                            dcal_;
  field_ref                            dinie_;
  field_ref                            dcie_;
  field_ref                            dinis_;
  field_ref                            dcis_;
  field_ref                            busy_;
  field_ref                            nbwp_;
  field_ref                            dinne_;
  field_ref                            nbwe_;
  std::array<std::uint32_t, max_block> hr_{};
  std::array<std::uint32_t, hra_count> hra_{};
  irq_line*                            line_;
  dma_signal                           dma_;
  bool                                 long_key_logged_{};
  bool                                 code_logged_{};
  bool                                 squeeze_logged_{};
  bool                                 hmac_logged_{};
  bool                                 dcal_busy_logged_{};
  bool                                 dma_logged_{};
  bool                                 swap_logged_{};
};

template <hash_family const& Family> auto hash_writes() -> std::span<write_entry<hash> const>
{
  static std::deque<std::string> const names = [] {
    std::deque<std::string> out;
    for(std::size_t i = 0; i < hra_count; ++i)
    {
      out.push_back(std::format("HRA{}", i));
    }
    for(std::size_t i = 0; i < Family.hr_count; ++i)
    {
      out.push_back(std::format("HR{}", i));
    }
    for(std::size_t i = 0; i < Family.csr_count; ++i)
    {
      out.push_back(std::format("CSR{}", i));
    }
    return out;
  }();
  static std::vector<write_entry<hash>> const table = [] {
    std::vector<write_entry<hash>> out{
        {"CR",  &hash::control_written},
        {"DIN", &hash::stored         },
        {"STR", &hash::start_written  },
        {"IMR", &hash::mask_written   },
        {"SR",  &hash::stored         },
    };
    for(auto const& name : names)
    {
      out.push_back({name, &hash::stored});
    }
    return out;
  }();
  return table;
}

hash::hash(
    model_context&                     ctx,
    peripheral_desc const&             desc,
    hash_family const&                 family,
    std::span<write_entry<hash> const> writes)
    : dispatch_model{ctx, desc, "HASH", writes}, regs_{ctx.regs}, family_{family},
      cr_{reg("CR").offset}, din_{reg("DIN").offset}, str_{reg("STR").offset},
      sr_{reg("SR").offset}, csr0_{reg("CSR0").offset}, init_{field("CR", "INIT")},
      dmae_{field("CR", "DMAE")}, mdmat_{field("CR", "MDMAT")}, dmas_{field("SR", "DMAS")},
      datatype_{field("CR", "DATATYPE")}, mode_{field("CR", "MODE")}, lkey_{field("CR", "LKEY")},
      algo_{field("CR", "ALGO")}, nbw_{field("CR", "NBW")}, cr_dinne_{field("CR", "DINNE")},
      nblw_{field("STR", "NBLW")}, dcal_{field("STR", "DCAL")}, dinie_{field("IMR", "DINIE")},
      dcie_{field("IMR", "DCIE")}, dinis_{field("SR", "DINIS")}, dcis_{field("SR", "DCIS")},
      busy_{field("SR", "BUSY")}, nbwp_{field("SR", "NBWP")}, dinne_{field("SR", "DINNE")},
      nbwe_{field("SR", "NBWE")}, line_{&irq("HASH_IRQn")}, dma_{ctx.peers, "hash_in_dma"}
{
  if((std::size_t{1} << algo_.width) > family_.algorithms.size())
  {
    throw config_error(
        desc.name, 0, "CR.ALGO",
        std::format(
            "a field of at most {} bits, the family has {} codes",
            std::bit_width(family_.algorithms.size() - 1), family_.algorithms.size()));
  }
  for(std::size_t i = 0; i < family_.hr_count; ++i)
  {
    hr_[i] = reg(std::format("HR{}", i)).offset;
  }
  for(std::size_t i = 0; i < hra_count; ++i)
  {
    hra_[i] = reg(std::format("HRA{}", i)).offset;
  }
}

void hash::reset(register_file&, reset_kind)
{
  restore_reset_values();
}

void hash::on_bus_reset()
{
  dma_.set(false);
  restore_reset_values();
}

void hash::restore_reset_values()
{
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
  withdraw();
  state_ = {};
  line_->lower();
  dma_.resolve();
}

auto hash::is_csr(std::uint32_t offset) const -> bool
{
  return offset >= csr0_ and offset < csr0_ + 4 * family_.csr_count;
}

auto hash::on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
    -> std::expected<std::uint32_t, bus_error>
{
  if(size != 4 or offset % 4 != 0)
  {
    return std::unexpected{bus_error::denied};
  }
  if(is_csr(offset))
  {
    return get(regs_, dinis_) != 0 ? context_word((offset - csr0_) / 4) : 0u;
  }
  auto const value = dispatch_model::on_read(regs, offset, size, ctx);
  if(value and offset == str_ and keccak_selected())
  {
    return *value & ~std::uint32_t{7};
  }
  return value;
}

auto hash::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     ctx) -> std::expected<void, bus_error>
{
  if(size != 4 or offset % 4 != 0)
  {
    return std::unexpected{bus_error::denied};
  }
  if(is_csr(offset))
  {
    context_written((offset - csr0_) / 4, value);
    return {};
  }
  if(offset == din_)
  {
    if(state_.squeezing != 0)
    {
      unmodelled(
          squeeze_logged_, "DIN", offset,
          "a DIN write after a SHAKE digest with no INIT starts a new message");
    }
    if(key_overflows())
    {
      unmodelled(
          long_key_logged_, "DIN", offset,
          std::format(
              "an HMAC key longer than {} bytes needs LKEY 1: the key is hashed as with LKEY 1",
              4 * alg().block_words));
    }
    data_written(value);
    return {};
  }
  if(offset == sr_)
  {
    status_written(value);
    return {};
  }
  if(offset == cr_)
  {
    if(dmae_.of(value) != 0 and not dma_.routed())
    {
      unmodelled(
          dma_logged_, "CR", offset,
          "no DMA controller serves hash_in_dma: DMAE 1 requests nothing");
    }
    if(code_of(value).absent)
    {
      unmodelled(
          code_logged_, "CR", offset,
          std::format(
              "ALGO {:#x} selects no algorithm of this part: ALGO reads 0 and the hash runs SHA-1",
              algo_.of(value)));
      value = algo_.with(value, 0);
    }
    if(init_.of(value) != 0)
    {
      auto const& chosen = code_of(value);
      if(mode_.of(value) != 0 and not chosen.hmac)
      {
        unmodelled(
            hmac_logged_, "CR", offset,
            std::format("ALGO {:#x} has no HMAC mode: MODE 1 is ignored", algo_.of(value)));
      }
    }
  }
  if(offset == str_ and keccak_selected())
  {
    value &= ~std::uint32_t{7};
  }
  return dispatch_model::on_write(regs, offset, size, old, value, ctx);
}

auto hash::alg() const -> algorithm const&
{
  return family_.algorithms[state_.algo];
}

auto hash::code_of(std::uint32_t control) const -> algorithm const&
{
  return family_.algorithms[algo_.of(control)];
}

auto hash::keccak_selected() const -> bool
{
  return code_of(regs_.peek(cr_)).kind == function::keccak;
}

auto hash::algo_of_config(std::uint32_t value) const -> std::uint32_t
{
  return (value & 3u) | (((value >> 14) & 3u) << 2);
}

auto hash::lane_word(std::size_t index) const -> std::uint32_t
{
  auto const lane = state_.lanes[index / 2];
  return static_cast<std::uint32_t>(index % 2 == 0 ? lane : lane >> 32);
}

void hash::set_lane_word(std::size_t index, std::uint32_t value)
{
  auto& lane = state_.lanes[index / 2];
  lane       = index % 2 == 0 ? (lane & ~std::uint64_t{0xFFFF'FFFF}) | value
                              : (lane & 0xFFFF'FFFF) | std::uint64_t{value} << 32;
}

auto hash::context_word(std::size_t index) const -> std::uint32_t
{
  if(index == 0)
  {
    return swap_tag;
  }
  if(index == csr_config)
  {
    return (state_.algo & 3u)
         | std::uint32_t{state_.hmac} << 2
         | std::uint32_t{state_.long_key} << 3
         | std::uint32_t{state_.phase} << 4
         | std::uint32_t{state_.count} << 8
         | std::uint32_t{static_cast<std::uint32_t>(state_.algo >> 2)} << 14;
  }
  if(index == csr_blocks)
  {
    return static_cast<std::uint32_t>(state_.blocks);
  }
  if(index == csr_blocks + 1)
  {
    return static_cast<std::uint32_t>(state_.blocks >> 32);
  }
  auto const& a = alg();
  if(index >= a.csr_inner and index < a.csr_inner + inner_width(a) and a.hmac)
  {
    return state_.inner[index - a.csr_inner];
  }
  switch(a.kind)
  {
    case function::sha512:
      if(index >= csr_chain and index < csr_chain + 16)
      {
        return lane_word(index - csr_chain);
      }
      if(index >= csr_wide_pend and index < csr_wide_pend + 32)
      {
        return state_.pending[index - csr_wide_pend];
      }
      return 0;
    case function::keccak:
      if(index >= csr_chain and index < csr_chain + 2 * keccak::lanes)
      {
        return lane_word(index - csr_chain);
      }
      return index == csr_last ? state_.last : 0u;
    default:
      if(index >= csr_chain and index < csr_chain + state_.h.size())
      {
        return state_.h[index - csr_chain];
      }
      if(index >= csr_pending and index < csr_pending + block_words)
      {
        return state_.pending[index - csr_pending];
      }
      return 0;
  }
}

auto hash::config_valid(std::uint32_t value) const -> bool
{
  auto const phase = (value >> 4) & 3u;
  auto const hmac  = (value >> 2) & 1u;
  return (value & ~0xFF3Fu) == 0
     and algo_of_config(value) == state_.algo
     and hmac == state_.hmac
     and ((value >> 3) & 1u) == state_.long_key
     and ((value >> 8) & 0x3Fu) <= alg().block_words
     and phase <= phase_outer_key
     and (hmac != 0 or phase == phase_inner_key);
}

void hash::load_config(std::uint32_t value)
{
  state_.phase    = static_cast<std::uint8_t>((value >> 4) & 3u);
  state_.count    = static_cast<std::uint8_t>((value >> 8) & 0x3Fu);
  state_.digested = 0;
  update(regs_, dinne_, state_.count != 0 ? 1u : 0u);
  update(regs_, nbwe_, alg().block_words + 1u - state_.count);
  publish();
}

void hash::context_written(std::size_t index, std::uint32_t value)
{
  auto const refuse = [&](std::string message) {
    unmodelled(swap_logged_, std::format("CSR{}", index), csr0_ + 4 * index, std::move(message));
    state_.refused = 1;
  };
  if(index == 0)
  {
    if(value != swap_tag)
    {
      refuse(
          std::format(
              "CSR0 {:#010x} is not a context saved by this model, which writes {:#010x}: the CSR "
              "writes that follow are dropped",
              value, swap_tag));
      return;
    }
    state_.refused = 0;
    return;
  }
  if(state_.refused != 0)
  {
    return;
  }
  if(index == csr_config)
  {
    if(not config_valid(value))
    {
      refuse(
          std::format(
              "CSR1 {:#010x} does not match the ALGO, MODE and LKEY latched at INIT or holds an "
              "invalid phase or count: the CSR writes that follow are dropped",
              value));
      return;
    }
    load_config(value);
    return;
  }
  if(index == csr_blocks)
  {
    state_.blocks = (state_.blocks & ~std::uint64_t{0xFFFF'FFFF}) | value;
    return;
  }
  if(index == csr_blocks + 1)
  {
    state_.blocks = (state_.blocks & 0xFFFF'FFFF) | std::uint64_t{value} << 32;
    return;
  }
  auto const& a = alg();
  if(index >= a.csr_inner and index < a.csr_inner + inner_width(a) and a.hmac)
  {
    state_.inner[index - a.csr_inner] = value;
    return;
  }
  switch(a.kind)
  {
    case function::sha512:
      if(index >= csr_chain and index < csr_chain + 16)
      {
        set_lane_word(index - csr_chain, value);
      }
      else if(index >= csr_wide_pend and index < csr_wide_pend + 32)
      {
        state_.pending[index - csr_wide_pend] = value;
      }
      break;
    case function::keccak:
      if(index >= csr_chain and index < csr_chain + 2 * keccak::lanes)
      {
        set_lane_word(index - csr_chain, value);
      }
      else if(index == csr_last)
      {
        state_.last = value;
      }
      break;
    default:
      if(index >= csr_chain and index < csr_chain + state_.h.size())
      {
        state_.h[index - csr_chain] = value;
      }
      else if(index >= csr_pending and index < csr_pending + block_words)
      {
        state_.pending[index - csr_pending] = value;
      }
      break;
  }
}

void hash::control_written(std::uint32_t old, std::uint32_t now)
{
  if(init_.of(now) != 0)
  {
    regs_.poke(cr_, init_.with(now, 0));
    initialise(now);
  }
  if(dmae_.of(now) != 0)
  {
    state_.suspend = suspend_none;
  }
  else if(dmae_.of(old) != 0 and not block_launched())
  {
    state_.suspend = suspend_held;
  }
  request();
}

auto hash::block_launched() const -> bool
{
  return state_.count == 0 or get(regs_, dinis_) != 0 or (state_.busy != 0 and state_.count == 1);
}

void hash::request()
{
  auto const active = get(regs_, dmae_) != 0 or state_.suspend == suspend_held;
  update(regs_, dmas_, active ? 1u : 0u);
  dma_.set(active);
}

void hash::transfer_completed(dma_controller const& by, std::uint32_t number)
{
  if(not dma_.carries(by, number) or (get(regs_, dmae_) == 0 and state_.suspend == suspend_none))
  {
    return;
  }
  if(get(regs_, mdmat_) != 0)
  {
    if(state_.suspend == suspend_held)
    {
      state_.suspend = suspend_none;
      request();
    }
    return;
  }
  state_.suspend = suspend_none;
  update(regs_, dmae_, 0);
  request();
  calculate();
}

void hash::initialise(std::uint32_t control)
{
  state_.algo          = static_cast<std::uint8_t>(algo_.of(control));
  state_.busy          = 0;
  state_.final_pending = 0;
  state_.digested      = 0;
  state_.digest        = {};
  state_.digest_size   = 0;
  state_.hmac          = static_cast<std::uint8_t>(mode_.of(control) != 0 and alg().hmac);
  state_.long_key      = static_cast<std::uint8_t>(state_.hmac != 0 ? lkey_.of(control) : 0);
  state_.phase         = phase_inner_key;
  state_.inner         = {};
  state_.refused       = 0;
  state_.suspend       = suspend_none;
  restart();
  withdraw();
  update(regs_, nbwe_, alg().block_words + 1u);
  update(regs_, dinne_, 0);
  update(regs_, dcis_, 0);
  update(regs_, dinis_, 1);
  update(regs_, busy_, 0);
  publish();
  show_digest(false);
  interrupts();
}

void hash::data_written(std::uint32_t value)
{
  auto const word = swapped(value, get(regs_, datatype_));
  if(state_.squeezing != 0)
  {
    restart();
  }
  if(state_.count == alg().block_words)
  {
    process_full();
    ++state_.blocks;
    state_.count = 0;
    work(alg().cycles, false);
  }
  push(word);
  state_.digested = 0;
  update(regs_, dinis_, 0);
  update(regs_, dinne_, 1);
  if(auto const expected = get(regs_, nbwe_); expected > 0)
  {
    update(regs_, nbwe_, expected - 1);
  }
  publish();
  interrupts();
  if(state_.suspend == suspend_held and block_launched())
  {
    state_.suspend = suspend_launched;
    request();
  }
}

void hash::push(std::uint32_t word)
{
  if(alg().kind == function::keccak)
  {
    xor_word(state_.count, word);
    state_.last = word;
  }
  else
  {
    state_.pending[state_.count] = word;
  }
  ++state_.count;
}

void hash::xor_word(std::size_t index, std::uint32_t word)
{
  state_.lanes[index / 2] ^= std::uint64_t{std::byteswap(word)} << (32 * (index % 2));
}

void hash::xor_byte(std::size_t index, std::uint8_t value)
{
  state_.lanes[index / 8] ^= std::uint64_t{value} << (8 * (index % 8));
}

void hash::drop_garbage(std::uint32_t last_bits)
{
  if(state_.count != 0 and last_bits < 32)
  {
    xor_word(state_.count - 1u, state_.last & (~std::uint32_t{} >> last_bits));
  }
}

void hash::process_full()
{
  if(alg().kind == function::keccak)
  {
    keccak::permute(state_.lanes);
  }
  else
  {
    compress_block(std::span{state_.pending}.first(alg().block_words));
  }
}

void hash::compress_block(std::span<std::uint32_t const> m)
{
  switch(alg().kind)
  {
    case function::sha1:
      sha1_compress(state_.h, m.first<block_words>());
      break;
    case function::md5:
      md5_compress(state_.h, m.first<block_words>());
      break;
    case function::sha256:
      sha256_compress(state_.h, m.first<block_words>());
      break;
    case function::sha512:
      sha512_compress(std::span<std::uint64_t, 8>{state_.lanes.data(), 8}, m.first<32>());
      break;
    case function::keccak:
      break;
  }
}

void hash::start_written(std::uint32_t old, std::uint32_t now)
{
  if(dcal_.of(now) == 0)
  {
    return;
  }
  regs_.poke(nblw_.offset, dcal_.with(nblw_.with(now, nblw_.of(old)), 0));
  if(state_.busy != 0 and state_.squeezing != 0)
  {
    unmodelled(
        dcal_busy_logged_, "STR", str_,
        "DCAL while BUSY squeezes past a rate the firmware has not read");
  }
  calculate();
}

void hash::calculate()
{
  if(state_.digested == 0)
  {
    finish_digest();
  }
  else if(state_.squeezing != 0)
  {
    squeeze();
  }
}

auto hash::key_overflows() const -> bool
{
  return state_.hmac != 0
     and state_.long_key == 0
     and state_.phase != phase_message
     and state_.blocks == 0
     and state_.count == alg().block_words;
}

auto hash::last_bits() const -> std::uint32_t
{
  auto const valid = get(regs_, nblw_) & (alg().kind == function::keccak ? ~7u : ~0u);
  return valid == 0 ? 32u : valid;
}

void hash::restart()
{
  auto const& a = alg();
  state_.h      = {};
  state_.lanes  = {};
  if(a.kind == function::sha512)
  {
    std::ranges::copy(sha512_initial[a.init], state_.lanes.begin());
  }
  else if(a.kind != function::keccak)
  {
    state_.h = initial_values[a.init];
  }
  state_.blocks    = 0;
  state_.count     = 0;
  state_.last      = 0;
  state_.squeezing = 0;
}

auto hash::output() const -> words
{
  words       out{};
  auto const& a = alg();
  for(std::size_t i = 0; i < a.digest_words; ++i)
  {
    switch(a.kind)
    {
      case function::md5:
        out[i] = std::byteswap(state_.h[i]);
        break;
      case function::sha512:
        out[i] = lane_word(i ^ 1u);
        break;
      case function::keccak:
        out[i] = std::byteswap(lane_word(i));
        break;
      default:
        out[i] = state_.h[i];
        break;
    }
  }
  return out;
}

auto hash::open_hmac(std::uint32_t pad_word) -> std::size_t
{
  auto const& a = alg();
  words       key{};
  std::size_t compressed = 0;
  bool        in_place   = false;
  if(state_.long_key == 0 and state_.blocks == 0)
  {
    if(a.kind == function::keccak)
    {
      drop_garbage(last_bits());
      in_place = true;
    }
    else
    {
      std::copy_n(state_.pending.begin(), state_.count, key.begin());
      if(auto const bits = last_bits(); state_.count != 0 and bits < 32)
      {
        key[state_.count - 1] &= ~(~std::uint32_t{} >> bits);
      }
    }
  }
  else
  {
    compressed = pad(last_bits());
    key        = output();
  }
  if(not in_place)
  {
    restart();
  }
  if(a.kind == function::keccak)
  {
    for(std::size_t i = 0; i < a.block_words; ++i)
    {
      xor_word(i, (in_place ? 0u : key[i]) ^ pad_word);
    }
    keccak::permute(state_.lanes);
    state_.count = 0;
    state_.last  = 0;
  }
  else
  {
    for(std::size_t i = 0; i < a.block_words; ++i)
    {
      key[i] ^= pad_word;
    }
    compress_block(std::span{key}.first(a.block_words));
  }
  state_.blocks = 1;
  return compressed + 1;
}

void hash::finish_digest()
{
  std::size_t compressed = 0;
  bool        final      = true;
  if(state_.hmac == 0)
  {
    compressed    = pad(last_bits());
    state_.digest = output();
  }
  else if(state_.phase == phase_inner_key)
  {
    compressed   = open_hmac(ipad);
    state_.phase = phase_message;
    final        = false;
  }
  else if(state_.phase == phase_message)
  {
    compressed = pad(last_bits());
    std::copy_n(output().begin(), state_.inner.size(), state_.inner.begin());
    state_.phase = phase_outer_key;
    restart();
    final = false;
  }
  else
  {
    compressed      = open_hmac(opad);
    auto const size = alg().digest_words;
    state_.count    = 0;
    for(std::size_t i = 0; i < size; ++i)
    {
      push(state_.inner[i]);
    }
    compressed += pad(32);
    state_.digest = output();
    state_.phase  = phase_inner_key;
  }
  conclude(compressed, final);
}

void hash::conclude(std::size_t compressed, bool final)
{
  if(final)
  {
    state_.digest_size = alg().digest_words;
    state_.digested    = 1;
    if(alg().extendable)
    {
      state_.squeezing = 1;
      state_.count     = 0;
      state_.last      = 0;
    }
    else
    {
      restart();
    }
  }
  update(regs_, dinne_, 0);
  update(regs_, dcis_, 0);
  show_digest(false);
  publish();
  work(compressed * alg().cycles + (final ? alg().tail : 0u), final);
  interrupts();
}

void hash::squeeze()
{
  keccak::permute(state_.lanes);
  state_.digest = output();
  conclude(1, true);
}

auto hash::pad(std::uint32_t last_bits) -> std::size_t
{
  auto const& a = alg();
  if(a.kind == function::keccak)
  {
    return pad_keccak(last_bits);
  }
  std::array<std::uint32_t, 2 * 32> tail{};
  std::copy_n(state_.pending.begin(), state_.count, tail.begin());
  std::size_t   at   = state_.count;
  std::uint64_t bits = std::uint64_t{32} * a.block_words * state_.blocks;
  if(at == 0)
  {
    tail[at++] = 0x8000'0000;
  }
  else
  {
    bits += 32 * (at - 1) + last_bits;
    auto& last = tail[at - 1];
    if(last_bits == 32)
    {
      tail[at++] = 0x8000'0000;
    }
    else
    {
      last = (last & ~(~std::uint32_t{} >> last_bits)) | (0x8000'0000u >> last_bits);
    }
  }
  std::size_t const length = a.kind == function::sha512 ? 4 : 2;
  std::size_t const blocks = at > a.block_words - length ? 2 : 1;
  auto const        end    = blocks * a.block_words;
  auto const        high   = static_cast<std::uint32_t>(bits >> 32);
  auto const        low    = static_cast<std::uint32_t>(bits);
  tail[end - 2]            = a.kind == function::md5 ? std::byteswap(low) : high;
  tail[end - 1]            = a.kind == function::md5 ? std::byteswap(high) : low;
  for(std::size_t i = 0; i < blocks; ++i)
  {
    compress_block(std::span{tail}.subspan(i * a.block_words, a.block_words));
  }
  return blocks;
}

auto hash::pad_keccak(std::uint32_t last_bits) -> std::size_t
{
  auto const&       a      = alg();
  std::size_t const rate   = 4 * std::size_t{a.block_words};
  std::size_t       offset = 0;
  std::size_t       passes = 0;
  if(state_.count != 0)
  {
    drop_garbage(last_bits);
    offset = 4 * (state_.count - 1u) + last_bits / 8;
  }
  if(offset == rate)
  {
    keccak::permute(state_.lanes);
    ++passes;
    offset = 0;
  }
  xor_byte(offset, a.suffix);
  xor_byte(rate - 1, 0x80);
  keccak::permute(state_.lanes);
  return passes + 1;
}

void hash::work(std::uint64_t cycles, bool final)
{
  auto const now   = ctx_.clock.time().ps;
  auto const start = state_.busy != 0 ? std::max(now, state_.busy_end) : now;
  state_.busy_end  = start + cycles * ps_per_second / ctx_.clock.core_hz();
  if(final)
  {
    state_.final_pending = 1;
  }
  if(std::exchange(state_.busy, 1) != 0)
  {
    return;
  }
  update(regs_, busy_, 1);
  post();
}

void hash::post()
{
  state_.queued = ctx_.events.schedule_at(vtime{state_.busy_end}, busy_done, 0, ctx_.clock) + 1;
}

void hash::withdraw()
{
  for(auto* queued : {&state_.queued, &state_.flagged})
  {
    if(auto const seq = std::exchange(*queued, 0); seq != 0)
    {
      ctx_.events.cancel(seq - 1);
    }
  }
}

void hash::on_event(std::uint32_t kind, std::uint64_t)
{
  if(kind == digest_done)
  {
    state_.flagged = 0;
    complete_digest();
    interrupts();
    return;
  }
  if(kind != busy_done)
  {
    return;
  }
  state_.queued = 0;
  if(ctx_.clock.time().ps < state_.busy_end)
  {
    post();
    return;
  }
  finish_work();
}

void hash::finish_work()
{
  auto const launched = state_.suspend == suspend_launched;
  if(launched)
  {
    state_.suspend = suspend_none;
  }
  state_.busy = 0;
  update(regs_, busy_, 0);
  if(std::exchange(state_.final_pending, 0) != 0)
  {
    if(alg().lag == 0)
    {
      complete_digest();
    }
    else
    {
      auto const lag = alg().lag * ps_per_second / ctx_.clock.core_hz();
      state_.flagged =
          ctx_.events.schedule_at(vtime{ctx_.clock.time().ps + lag}, digest_done, 0, ctx_.clock)
          + 1;
    }
  }
  else
  {
    if(state_.count <= 1 or launched)
    {
      update(regs_, dinis_, 1);
    }
    update(regs_, nbwe_, alg().block_words + 1u - state_.count);
  }
  interrupts();
}

void hash::complete_digest()
{
  update(regs_, dcis_, 1);
  update(regs_, dinis_, 1);
  update(regs_, nbwe_, alg().block_words + 1u);
  show_digest(true);
}

void hash::status_written(std::uint32_t value)
{
  auto const cleared = ~value & (dinis_.mask() | dcis_.mask());
  auto const before  = regs_.peek(sr_);
  regs_.poke(sr_, before & ~cleared);
  if(dcis_.of(before) != 0 and dcis_.of(value) == 0)
  {
    show_digest(false);
  }
  interrupts();
}

void hash::mask_written(std::uint32_t, std::uint32_t)
{
  interrupts();
}

void hash::publish()
{
  update(regs_, nbwp_, state_.count);
  update(regs_, nbw_, state_.count);
  update(regs_, cr_dinne_, get(regs_, dinne_));
}

void hash::show_digest(bool visible)
{
  auto const aliased = alg().kind != function::keccak;
  for(std::size_t i = 0; i < family_.hr_count; ++i)
  {
    auto const value = visible and i < state_.digest_size ? state_.digest[i] : 0u;
    regs_.poke(hr_[i], value);
    if(i < hra_count)
    {
      regs_.poke(hra_[i], aliased ? value : 0u);
    }
  }
}

void hash::interrupts()
{
  auto const level = (get(regs_, dinis_) != 0 and get(regs_, dinie_) != 0)
                  or (get(regs_, dcis_) != 0 and get(regs_, dcie_) != 0);
  if(level)
  {
    line_->raise();
  }
  else
  {
    line_->lower();
  }
}

void hash::unmodelled(
    bool&            logged,
    std::string_view name,
    std::uint32_t    offset,
    std::string      message)
{
  if(not std::exchange(logged, true))
  {
    warn(name, std::move(message));
  }
  if(ctx_.strict_peripherals)
  {
    throw model_error(desc_.name, std::string{name}, offset);
  }
}
}

auto hash_factory() -> model_factory
{
  return [](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<hash>(ctx, desc, u5_hash, hash_writes<u5_hash>());
  };
}

auto hash_h5_factory(hash_variant variant) -> model_factory
{
  return [variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    if(variant.sha3)
    {
      return std::make_shared<hash>(ctx, desc, h5_sha3_hash, hash_writes<h5_sha3_hash>());
    }
    return std::make_shared<hash>(ctx, desc, h5_hash, hash_writes<h5_hash>());
  };
}
}
