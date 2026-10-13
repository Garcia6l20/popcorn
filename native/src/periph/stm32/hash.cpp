#include "dma.hpp"
#include "dma_signal.hpp"
#include "factories.hpp"
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
constexpr std::uint64_t ps_per_second = 1'000'000'000'000;
constexpr std::size_t   block_words   = 16;
constexpr std::uint32_t fresh_expect  = 17;
constexpr std::size_t   hra_count     = 5;
constexpr std::size_t   hr_count      = 8;
constexpr std::size_t   csr_count     = 54;

constexpr std::uint8_t sha1   = 0;
constexpr std::uint8_t md5    = 1;
constexpr std::uint8_t sha224 = 2;

constexpr std::uint8_t phase_inner_key = 0;
constexpr std::uint8_t phase_message   = 1;
constexpr std::uint8_t phase_outer_key = 2;

constexpr std::uint32_t ipad = 0x3636'3636;
constexpr std::uint32_t opad = 0x5C5C'5C5C;

using chain = std::array<std::uint32_t, 8>;
using block = std::array<std::uint32_t, block_words>;

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

constexpr std::array<std::size_t, 4>   digest_words{5, 4, 7, 8};
constexpr std::array<std::uint64_t, 4> block_cycles{82, 66, 66, 66};

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

/**
 * @brief Compresses one 512-bit block into a SHA-1 chaining value, FIPS 180-4 6.1.2.
 * @param h Chaining value, words 0 to 4.
 * @param m Message block as big-endian words.
 */
constexpr void sha1_compress(chain& h, block const& m) noexcept
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
constexpr void sha256_compress(chain& h, block const& m) noexcept
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
constexpr void md5_compress(chain& h, block const& m) noexcept
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

/**
 * @brief Compresses one block with the algorithm's function.
 * @param algo ALGO field value, 00 SHA-1, 01 MD5, 10 SHA-224, 11 SHA-256.
 * @param h Chaining value.
 * @param m Message block as big-endian words.
 */
constexpr void compress(std::uint8_t algo, chain& h, block const& m) noexcept
{
  if(algo == sha1)
  {
    sha1_compress(h, m);
  }
  else if(algo == md5)
  {
    md5_compress(h, m);
  }
  else
  {
    sha256_compress(h, m);
  }
}

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
  std::uint8_t                 algo{};
  chain                        h{};
  std::uint64_t                blocks{};
  block                        pending{};
  std::uint8_t                 count{};
  std::array<std::uint32_t, 8> digest{};
  std::uint8_t                 digest_size{};
  std::uint64_t                busy_end{};
  std::uint64_t                queued{};
  std::uint8_t                 busy{};
  std::uint8_t                 final_pending{};
  std::uint8_t                 digested{};
  std::uint8_t                 hmac{};
  std::uint8_t                 long_key{};
  std::uint8_t                 phase{};
  std::array<std::uint32_t, 8> inner{};

  friend auto operator==(hash_state const&, hash_state const&) -> bool = default;
};

/**
 * @brief Hash processor of the STM32U5, RM0456 section 51: SHA-1, MD5, SHA-224 and SHA-256, and
 *        HMAC over them, on words written to DIN by the CPU or by DMA.
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
 *        set for 82 HCLK cycles on SHA-1 and 66 on the others, RM0456 Table 487, the final
 *        digest one block time per block it compresses. Work arriving while BUSY queues behind
 *        the current block, its end time fixed on arrival from the core clock, which is HCLK.
 *        One BUSY end event is pending at a time, moved to the queue's end when it fires early.
 *        INIT and reset cancel it, so the queue never holds more than one event of this model.
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
 *        HMAC, RM0456 51.4.7: MODE 1 at INIT selects HMAC and latches LKEY. Three phases follow,
 *        each closed by DCAL: the inner key, the message, the outer key. A key phase ends by
 *        compressing the key, zero-padded to 64 bytes and XORed with ipad or opad, the key being
 *        the words written in that phase with NBLW valid bits in the last one, or their digest
 *        with LKEY 1, whatever their length. The message phase keeps its digest, the outer key
 *        phase hashes it after the key block and shows the HMAC. Between phases BUSY ends with
 *        DINIS 1 and DCIS 0, DCIS rises after the outer key only. Every DCAL closes its phase,
 *        an empty message or key included, the automatic one of a DMA transfer too. A DCAL
 *        after the outer key with no word since keeps the HMAC, and words written after it
 *        start a new inner key. Each phase takes one block time per block it compresses: the
 *        key's digest with LKEY 1, the padded key block, the message's last blocks and the inner
 *        digest's block, not RM0456's approximate factors.
 *
 *        Divergence: a DIN write while BUSY does not stall the bus, the CPU runs on. Not
 *        modelled, each logged once and a model_error under strict_peripherals, thrown before
 *        the write is stored: an HMAC key longer than 64 bytes with LKEY 0, hashed as with
 *        LKEY 1. DMAE 1 with no controller serving hash_in_dma. Context swap, reads of CSR0-53
 *        return 0 and writes are dropped. The RCC clock gate is not modelled.
 */
class hash final : public dispatch_model<hash, hash_state>, public dma_requester
{
public:
  hash(model_context& ctx, peripheral_desc const& desc);

  void reset(register_file& regs, reset_kind kind) override;
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
  void               data_written(std::uint32_t value);
  void               status_written(std::uint32_t value);
  void               initialise(std::uint32_t control);
  void               finish_digest();
  [[nodiscard]] auto last_bits() const -> std::uint32_t;
  auto               pad(std::uint32_t last_bits) -> std::size_t;
  [[nodiscard]] auto output() const -> std::array<std::uint32_t, 8>;
  void               restart();
  auto               open_hmac(std::uint32_t pad_word) -> std::size_t;
  [[nodiscard]] auto key_overflows() const -> bool;
  void               calculate();
  void               request();
  void               work(std::uint64_t cycles, bool final);
  void               finish_work();
  void               publish();
  void               show_digest(bool visible);
  void               interrupts();
  [[nodiscard]] auto is_csr(std::uint32_t offset) const -> bool;
  void unmodelled(bool& logged, std::string_view name, std::uint32_t offset, std::string message);

  register_file&                       regs_;
  std::uint32_t                        cr_;
  std::uint32_t                        din_;
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
  std::array<std::uint32_t, hr_count>  hr_{};
  std::array<std::uint32_t, hra_count> hra_{};
  irq_line*                            line_;
  dma_signal                           dma_;
  bool                                 long_key_logged_{};
  bool                                 dma_logged_{};
  bool                                 swap_logged_{};
};

auto hash_writes() -> std::span<write_entry<hash> const>
{
  static std::deque<std::string> const names = [] {
    std::deque<std::string> out;
    for(std::size_t i = 0; i < hra_count; ++i)
    {
      out.push_back(std::format("HRA{}", i));
    }
    for(std::size_t i = 0; i < hr_count; ++i)
    {
      out.push_back(std::format("HR{}", i));
    }
    for(std::size_t i = 0; i < csr_count; ++i)
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

hash::hash(model_context& ctx, peripheral_desc const& desc)
    : dispatch_model{ctx, desc, "HASH", hash_writes()}, regs_{ctx.regs}, cr_{reg("CR").offset},
      din_{reg("DIN").offset}, sr_{reg("SR").offset}, csr0_{reg("CSR0").offset},
      init_{field("CR", "INIT")}, dmae_{field("CR", "DMAE")}, mdmat_{field("CR", "MDMAT")},
      dmas_{field("SR", "DMAS")}, datatype_{field("CR", "DATATYPE")}, mode_{field("CR", "MODE")},
      lkey_{field("CR", "LKEY")}, algo_{field("CR", "ALGO")}, nbw_{field("CR", "NBW")},
      cr_dinne_{field("CR", "DINNE")}, nblw_{field("STR", "NBLW")}, dcal_{field("STR", "DCAL")},
      dinie_{field("IMR", "DINIE")}, dcie_{field("IMR", "DCIE")}, dinis_{field("SR", "DINIS")},
      dcis_{field("SR", "DCIS")}, busy_{field("SR", "BUSY")}, nbwp_{field("SR", "NBWP")},
      dinne_{field("SR", "DINNE")}, nbwe_{field("SR", "NBWE")}, line_{&irq("HASH_IRQn")},
      dma_{ctx.peers, "hash_in_dma"}
{
  for(std::size_t i = 0; i < hr_count; ++i)
  {
    hr_[i] = reg(std::format("HR{}", i)).offset;
  }
  for(std::size_t i = 0; i < hra_count; ++i)
  {
    hra_[i] = reg(std::format("HRA{}", i)).offset;
  }
}

void hash::reset(register_file& regs, reset_kind)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
  withdraw();
  state_ = {};
  line_->lower();
  dma_.resolve();
}

auto hash::is_csr(std::uint32_t offset) const -> bool
{
  return offset >= csr0_ and offset < csr0_ + 4 * csr_count;
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
    unmodelled(
        swap_logged_, std::format("CSR{}", (offset - csr0_) / 4), offset,
        "context swap is not modelled: CSR0-53 read 0 and ignore writes");
    return 0u;
  }
  return dispatch_model::on_read(regs, offset, size, ctx);
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
    unmodelled(
        swap_logged_, std::format("CSR{}", (offset - csr0_) / 4), offset,
        "context swap is not modelled: CSR0-53 read 0 and ignore writes");
    return {};
  }
  if(offset == din_)
  {
    if(key_overflows())
    {
      unmodelled(
          long_key_logged_, "DIN", offset,
          "an HMAC key longer than 64 bytes needs LKEY 1: the key is hashed as with LKEY 1");
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
  }
  return dispatch_model::on_write(regs, offset, size, old, value, ctx);
}

void hash::control_written(std::uint32_t, std::uint32_t now)
{
  if(init_.of(now) != 0)
  {
    regs_.poke(cr_, init_.with(now, 0));
    initialise(now);
  }
  request();
}

void hash::request()
{
  auto const enabled = get(regs_, dmae_);
  update(regs_, dmas_, enabled);
  dma_.set(enabled != 0);
}

void hash::transfer_completed(dma_controller const& by, std::uint32_t number)
{
  if(not dma_.carries(by, number) or get(regs_, dmae_) == 0 or get(regs_, mdmat_) != 0)
  {
    return;
  }
  update(regs_, dmae_, 0);
  request();
  calculate();
}

void hash::initialise(std::uint32_t control)
{
  state_.algo          = static_cast<std::uint8_t>(algo_.of(control));
  state_.h             = initial_values[state_.algo];
  state_.blocks        = 0;
  state_.count         = 0;
  state_.busy          = 0;
  state_.final_pending = 0;
  state_.digested      = 0;
  state_.digest        = {};
  state_.digest_size   = 0;
  state_.hmac          = static_cast<std::uint8_t>(mode_.of(control));
  state_.long_key      = static_cast<std::uint8_t>(state_.hmac != 0 ? lkey_.of(control) : 0);
  state_.phase         = phase_inner_key;
  state_.inner         = {};
  withdraw();
  update(regs_, nbwe_, fresh_expect);
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
  if(state_.count == block_words)
  {
    compress(state_.algo, state_.h, state_.pending);
    ++state_.blocks;
    state_.count = 0;
    work(block_cycles[state_.algo], false);
  }
  state_.pending[state_.count++] = word;
  state_.digested                = 0;
  update(regs_, dinis_, 0);
  update(regs_, dinne_, 1);
  if(auto const expected = get(regs_, nbwe_); expected > 0)
  {
    update(regs_, nbwe_, expected - 1);
  }
  publish();
  interrupts();
}

void hash::start_written(std::uint32_t old, std::uint32_t now)
{
  if(dcal_.of(now) == 0)
  {
    return;
  }
  regs_.poke(nblw_.offset, dcal_.with(nblw_.with(now, nblw_.of(old)), 0));
  calculate();
}

void hash::calculate()
{
  if(state_.digested == 0)
  {
    finish_digest();
  }
}

auto hash::key_overflows() const -> bool
{
  return state_.hmac != 0
     and state_.long_key == 0
     and state_.phase != phase_message
     and state_.blocks == 0
     and state_.count == block_words;
}

auto hash::last_bits() const -> std::uint32_t
{
  auto const valid = get(regs_, nblw_);
  return valid == 0 ? 32u : valid;
}

void hash::restart()
{
  state_.h      = initial_values[state_.algo];
  state_.blocks = 0;
  state_.count  = 0;
}

auto hash::output() const -> std::array<std::uint32_t, 8>
{
  std::array<std::uint32_t, 8> out{};
  auto const                   size = digest_words[state_.algo];
  for(std::size_t i = 0; i < size; ++i)
  {
    out[i] = state_.algo == md5 ? std::byteswap(state_.h[i]) : state_.h[i];
  }
  return out;
}

auto hash::open_hmac(std::uint32_t pad_word) -> std::size_t
{
  block       key{};
  std::size_t compressed = 0;
  if(state_.long_key == 0 and state_.blocks == 0)
  {
    std::copy_n(state_.pending.begin(), state_.count, key.begin());
    if(auto const bits = last_bits(); state_.count != 0 and bits < 32)
    {
      key[state_.count - 1] &= ~(~std::uint32_t{} >> bits);
    }
  }
  else
  {
    compressed = pad(last_bits());
    std::ranges::copy(output(), key.begin());
  }
  restart();
  for(auto& word : key)
  {
    word ^= pad_word;
  }
  compress(state_.algo, state_.h, key);
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
    compressed   = pad(last_bits());
    state_.inner = output();
    state_.phase = phase_outer_key;
    restart();
    final = false;
  }
  else
  {
    compressed      = open_hmac(opad);
    auto const size = digest_words[state_.algo];
    std::copy_n(state_.inner.begin(), size, state_.pending.begin());
    state_.count = static_cast<std::uint8_t>(size);
    compressed += pad(32);
    state_.digest = output();
    state_.phase  = phase_inner_key;
  }
  if(final)
  {
    state_.digest_size = static_cast<std::uint8_t>(digest_words[state_.algo]);
    state_.digested    = 1;
    restart();
  }
  update(regs_, dinne_, 0);
  update(regs_, dcis_, 0);
  show_digest(false);
  publish();
  work(compressed * block_cycles[state_.algo], final);
  interrupts();
}

auto hash::pad(std::uint32_t last_bits) -> std::size_t
{
  std::array<std::uint32_t, 2 * block_words> tail{};
  std::copy_n(state_.pending.begin(), state_.count, tail.begin());
  std::size_t   at   = state_.count;
  std::uint64_t bits = 512 * state_.blocks;
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
  std::size_t const blocks = at > block_words - 2 ? 2 : 1;
  auto const        end    = blocks * block_words;
  auto const        high   = static_cast<std::uint32_t>(bits >> 32);
  auto const        low    = static_cast<std::uint32_t>(bits);
  tail[end - 2]            = state_.algo == md5 ? std::byteswap(low) : high;
  tail[end - 1]            = state_.algo == md5 ? std::byteswap(high) : low;
  for(std::size_t i = 0; i < blocks; ++i)
  {
    block piece{};
    std::copy_n(
        tail.begin() + static_cast<std::ptrdiff_t>(i * block_words), block_words, piece.begin());
    compress(state_.algo, state_.h, piece);
  }
  return blocks;
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
  if(auto const seq = std::exchange(state_.queued, 0); seq != 0)
  {
    ctx_.events.cancel(seq - 1);
  }
}

void hash::on_event(std::uint32_t kind, std::uint64_t)
{
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
  state_.busy = 0;
  update(regs_, busy_, 0);
  if(std::exchange(state_.final_pending, 0) != 0)
  {
    update(regs_, dcis_, 1);
    update(regs_, dinis_, 1);
    update(regs_, nbwe_, fresh_expect);
    show_digest(true);
  }
  else
  {
    if(state_.count <= 1)
    {
      update(regs_, dinis_, 1);
    }
    update(regs_, nbwe_, fresh_expect - state_.count);
  }
  interrupts();
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
  for(std::size_t i = 0; i < hr_count; ++i)
  {
    auto const value = visible and i < state_.digest_size ? state_.digest[i] : 0u;
    regs_.poke(hr_[i], value);
    if(i < hra_count)
    {
      regs_.poke(hra_[i], value);
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
    return std::make_shared<hash>(ctx, desc);
  };
}
}
