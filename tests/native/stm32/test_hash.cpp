#include "dma.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>

#include <popcorn/periph/model.hpp>

#include <doctest/doctest.h>
#include <reflex/hash/sha256.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace periph = popcorn::periph;
namespace stm32  = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;
using popcorn::testing::u575;

using bytes = std::vector<std::uint8_t>;

constexpr std::uint32_t hash_base     = 0x420C'0400;
constexpr std::uint32_t sec_hash_base = 0x520C'0400;
constexpr std::uint32_t cr            = 0x00;
constexpr std::uint32_t din           = 0x04;
constexpr std::uint32_t str           = 0x08;
constexpr std::uint32_t hra0          = 0x0C;
constexpr std::uint32_t imr           = 0x20;
constexpr std::uint32_t sr            = 0x24;
constexpr std::uint32_t csr0          = 0xF8;
constexpr std::uint32_t hr0           = 0x310;
constexpr std::uint32_t cr_init       = 1u << 2;
constexpr std::uint32_t cr_dmae       = 1u << 3;
constexpr std::uint32_t cr_mode       = 1u << 6;
constexpr std::uint32_t cr_lkey       = 1u << 16;
constexpr std::uint32_t str_dcal      = 1u << 8;
constexpr std::uint32_t sr_dinis      = 1u << 0;
constexpr std::uint32_t sr_dcis       = 1u << 1;
constexpr std::uint32_t sr_busy       = 1u << 3;
constexpr std::uint32_t imr_dinie     = 1u << 0;
constexpr std::uint32_t imr_dcie      = 1u << 1;
constexpr std::uint32_t nvic_ispr3    = 0xE000'E20C;
constexpr std::uint32_t nvic_icpr3    = 0xE000'E28C;
constexpr std::uint32_t hash_irq_bit  = 1u << (96 % 32);
constexpr std::uint64_t ms            = 1'000'000'000;

constexpr std::uint32_t sha1   = 0;
constexpr std::uint32_t md5    = 1;
constexpr std::uint32_t sha224 = 2;
constexpr std::uint32_t sha256 = 3;

constexpr std::array<std::size_t, 4>   digest_bytes{20, 16, 28, 32};
constexpr std::array<std::uint64_t, 4> block_cycles{82, 66, 66, 66};

auto pattern(std::size_t size) -> bytes
{
  bytes out(size);
  for(std::size_t index = 0; index < size; ++index)
  {
    out[index] = static_cast<std::uint8_t>(index * 37 + 11);
  }
  return out;
}

auto text(std::string_view message) -> bytes
{
  return {message.begin(), message.end()};
}

auto unhex(std::string_view hex) -> bytes
{
  bytes out;
  for(std::size_t index = 0; index + 1 < hex.size(); index += 2)
  {
    auto const nibble = [](char c) {
      return static_cast<std::uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10);
    };
    out.push_back(static_cast<std::uint8_t>(nibble(hex[index]) << 4 | nibble(hex[index + 1])));
  }
  return out;
}

constexpr auto bit_reversed(std::uint32_t value) -> std::uint32_t
{
  std::uint32_t out = 0;
  for(unsigned bit = 0; bit < 32; ++bit)
  {
    out = (out << 1) | ((value >> bit) & 1u);
  }
  return out;
}

constexpr auto din_word(std::uint32_t message_word, std::uint32_t datatype) -> std::uint32_t
{
  switch(datatype)
  {
    case 1:
      return std::rotl(message_word, 16);
    case 2:
      return std::byteswap(message_word);
    case 3:
      return bit_reversed(message_word);
    default:
      return message_word;
  }
}

constexpr std::uint32_t hash_request = 89;
constexpr std::uint32_t cr_mdmat     = 1u << 13;
constexpr std::uint32_t sr_dmas      = 1u << 2;

class recorder final : public periph::model, public stm32::dma_controller
{
public:
  explicit recorder(bool knows_hash) : knows_hash_{knows_hash}
  {}

  std::vector<std::pair<std::uint32_t, bool>> levels;

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }
  void reset(periph::register_file&, popcorn::reset_kind) override
  {}
  auto on_read(periph::register_file&, std::uint32_t, std::uint8_t, periph::access_ctx)
      -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    return 0u;
  }
  auto on_write(
      periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t,
      periph::access_ctx) -> std::expected<void, popcorn::bus_error> override
  {
    return {};
  }
  void on_event(std::uint32_t, std::uint64_t) override
  {}
  void save(std::vector<std::byte>&) const override
  {}
  void load(std::span<std::byte const>) override
  {}
  void on_request(std::uint32_t request, bool level) override
  {
    levels.emplace_back(request, level);
  }
  [[nodiscard]] auto request_of(std::string_view signal) const
      -> std::optional<std::uint32_t> override
  {
    if(knows_hash_ and signal == "hash_in_dma")
    {
      return hash_request;
    }
    return std::nullopt;
  }

private:
  bool knows_hash_;
};

enum class controller
{
  gpdma,
  recording,
  unrouted,
};

auto idle(bool strict, controller dma = controller::gpdma) -> stm32_fixture
{
  auto options               = popcorn::testing::stm32_options();
  options.strict_peripherals = strict;
  if(dma != controller::gpdma)
  {
    options.models.emplace(
        "GPDMA1", [known = dma == controller::recording](
                      periph::model_context&, popcorn::peripheral_desc const&) {
          return std::make_shared<recorder>(known);
        });
    options.models.emplace("LPDMA1", [](periph::model_context&, popcorn::peripheral_desc const&) {
      return std::make_shared<recorder>(false);
    });
  }
  stm32_fixture f{u575(), "hash_ll", options};
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("hash_ll", "main")})
          .reason
      == stop_reason::address);
  f.set("pc", sym("hash_ll", "idle"));
  return f;
}

constexpr std::uint32_t swap_tag  = 0x4853'0001;
constexpr std::size_t   hash_csrs = 38;
constexpr std::size_t   hmac_csrs = 54;
constexpr std::uint32_t cr_saved  = cr_dmae | (3u << 4) | cr_mode | cr_mdmat | cr_lkey | (3u << 17);

struct context
{
  std::uint32_t              imr;
  std::uint32_t              str;
  std::uint32_t              cr;
  std::vector<std::uint32_t> csr;
};

struct hash_rig
{
  explicit hash_rig(bool strict = true, controller dma = controller::gpdma) : f{idle(strict, dma)}
  {}

  [[nodiscard]] auto dma() const -> recorder&
  {
    auto* const found = f.model_of<recorder>("GPDMA1");
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto other_dma() const -> recorder&
  {
    auto* const found = f.model_of<recorder>("LPDMA1");
    REQUIRE(found != nullptr);
    return *found;
  }

  void complete(std::uint32_t request = hash_request) const
  {
    complete(dma(), request);
  }

  void complete(stm32::dma_controller const& by, std::uint32_t request) const
  {
    auto* const requester = f.model_of<stm32::dma_requester>("HASH");
    REQUIRE(requester != nullptr);
    requester->transfer_completed(by, request);
  }

  [[nodiscard]] auto read(std::uint32_t offset, std::uint32_t base = hash_base) const
      -> std::uint32_t
  {
    auto const value = f.target->mem().read(base + offset, 4);
    REQUIRE(value.has_value());
    return *value;
  }

  void write(std::uint32_t offset, std::uint32_t value, std::uint32_t base = hash_base) const
  {
    REQUIRE(f.target->mem().write(base + offset, 4, value).has_value());
  }

  void begin(std::uint32_t algo, std::uint32_t datatype = 2, std::uint32_t base = hash_base) const
  {
    write(cr, cr_init | (algo << 17) | (datatype << 4), base);
  }

  void feed_bits(
      bytes const&  message,
      std::size_t   bits,
      std::uint32_t datatype = 2,
      std::uint32_t base     = hash_base) const
  {
    for(std::size_t at = 0; at * 32 < bits; ++at)
    {
      std::uint32_t word = 0;
      for(std::size_t byte = 0; byte < 4; ++byte)
      {
        auto const index = 4 * at + byte;
        auto const value =
            index < message.size() ? message[index] : static_cast<std::uint8_t>(0x5Au ^ index);
        word = (word << 8) | value;
      }
      auto const valid = std::min<std::size_t>(32, bits - 32 * at);
      if(valid < 32)
      {
        word |= 0xA5C3'96F0u >> valid;
      }
      write(din, din_word(word, datatype), base);
    }
  }

  void feed(bytes const& message, std::uint32_t datatype = 2) const
  {
    feed_bits(message, 8 * message.size(), datatype);
  }

  void finish(std::size_t bits, std::uint32_t base = hash_base) const
  {
    write(str, static_cast<std::uint32_t>(bits % 32), base);
    write(str, str_dcal, base);
  }

  [[nodiscard]] auto now() const -> std::uint64_t
  {
    return f.target->virtual_clock().time().ps;
  }

  [[nodiscard]] auto cycle_ps() const -> std::uint64_t
  {
    return 1'000'000'000'000 / f.target->virtual_clock().core_hz();
  }

  void until(std::uint64_t ps) const
  {
    auto const stopped =
        f.run_to_stop({.max_instructions = icount{1'000'000'000}, .until_time = vtime{ps}});
    REQUIRE(stopped.reason == stop_reason::time);
  }

  void settle() const
  {
    until(now() + ms);
  }

  [[nodiscard]] auto digest(std::uint32_t algo) const -> bytes
  {
    REQUIRE((read(sr) & sr_dcis) != 0);
    bytes out;
    for(std::uint32_t index = 0; index < 8; ++index)
    {
      auto const word = read(hr0 + 4 * index);
      if(index < 5)
      {
        CHECK(read(hra0 + 4 * index) == word);
      }
      if(4 * index >= digest_bytes[algo])
      {
        CHECK(word == 0);
        continue;
      }
      for(int shift = 24; shift >= 0; shift -= 8)
      {
        out.push_back(static_cast<std::uint8_t>(word >> shift));
      }
    }
    return out;
  }

  [[nodiscard]] auto
      hash(std::uint32_t algo, bytes const& message, std::uint32_t datatype = 2) const -> bytes
  {
    begin(algo, datatype);
    feed(message, datatype);
    finish(8 * message.size());
    settle();
    return digest(algo);
  }

  void hmac_begin(std::uint32_t algo, bool long_key, std::uint32_t datatype = 2) const
  {
    write(cr, cr_init | cr_mode | (long_key ? cr_lkey : 0u) | (algo << 17) | (datatype << 4));
  }

  void phase(bytes const& data, std::uint32_t datatype = 2) const
  {
    feed(data, datatype);
    finish(8 * data.size());
    settle();
  }

  [[nodiscard]] auto hmac(
      std::uint32_t algo,
      bytes const&  key,
      bytes const&  data,
      bool          long_key,
      std::uint32_t datatype = 2) const -> bytes
  {
    hmac_begin(algo, long_key, datatype);
    phase(key, datatype);
    phase(data, datatype);
    phase(key, datatype);
    return digest(algo);
  }

  [[nodiscard]] auto save(std::size_t count) const -> context
  {
    REQUIRE((read(sr) & (sr_busy | sr_dinis)) == sr_dinis);
    context out{read(imr) & (imr_dinie | imr_dcie), read(str) & 0x1Fu, read(cr) & cr_saved, {}};
    for(std::uint32_t index = 0; index < count; ++index)
    {
      out.csr.push_back(read(csr0 + 4 * index));
    }
    return out;
  }

  void reinit(context const& saved) const
  {
    write(imr, saved.imr);
    write(str, saved.str);
    write(cr, saved.cr);
    write(cr, saved.cr | cr_init);
  }

  void load(context const& saved, std::size_t from, std::size_t to) const
  {
    for(auto index = from; index < to; ++index)
    {
      write(csr0 + 4 * static_cast<std::uint32_t>(index), saved.csr[index]);
    }
  }

  void restore(context const& saved) const
  {
    reinit(saved);
    load(saved, 0, saved.csr.size());
  }

  [[nodiscard]] auto pending() const -> bool
  {
    auto const word = f.target->mem().read(nvic_ispr3, 4);
    REQUIRE(word.has_value());
    return (*word & hash_irq_bit) != 0;
  }

  void unpend() const
  {
    REQUIRE(f.target->mem().write(nvic_icpr3, 4, hash_irq_bit).has_value());
  }

  stm32_fixture                 f;
  std::vector<core::log_record> logged;
  core::hook_handle             sink =
      f.target->add_log_sink([this](core::log_record const& record) { logged.push_back(record); });
};

struct vector_case
{
  std::uint32_t    algo;
  std::string_view message;
  std::string_view digest;
};

constexpr std::string_view fips448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
constexpr std::string_view fips896 =
    "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopq"
    "rsmnopqrstnopqrstu";

constexpr std::array<vector_case, 18> published{
    {
     {sha1, "abc", "a9993e364706816aba3e25717850c26c9cd0d89d"},
     {sha1, "", "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
     {sha1, fips448, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"},
     {sha1, fips896, "a49b2446a02c645bf419f995b67091253a04a259"},
     {md5, "abc", "900150983cd24fb0d6963f7d28e17f72"},
     {md5, "", "d41d8cd98f00b204e9800998ecf8427e"},
     {md5, "message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
     {md5, "abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"},
     {md5, fips448, "8215ef0796a20bcaaae116d3876c664a"},
     {md5, fips896, "03dd8807a93175fb062dfb55dc7d359c"},
     {sha224, "abc", "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7"},
     {sha224, "", "d14a028c2a3a2bc9476102bb288234c415a2b01f828ea62ac5b3e42f"},
     {sha224, fips448, "75388b16512776cc5dba5da1fd890150b0c6455cb4f58b1952522525"},
     {sha224, fips896, "c97ca9a559850ce97a04a96def6d99a9e0e0e2ab14e6b8df265fc0b3"},
     {sha256, "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
     {sha256, "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
     {sha256, fips448, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
     {sha256, fips896, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"},
     }
};

struct pattern_case
{
  std::uint32_t    algo;
  std::size_t      size;
  std::string_view digest;
};

constexpr std::array<pattern_case, 32> edges{
    {
     {sha1, 55, "c4622048cfef59b72875839ee7ae1cbcf55e7658"},
     {sha1, 56, "ddc12942656468475970fa4fa49161f52ed138e4"},
     {sha1, 63, "7f8c3fa49f1297bd8b9feb964b6b419987f9f0d1"},
     {sha1, 64, "a334b47180c61fd522f99905ec02c36f9e848211"},
     {sha1, 65, "dd27d9eb923d39687e10872c3e8133ba2f0a68a1"},
     {sha1, 68, "80c2b4bbb7ad0c69259a99e209828cfe5f26016a"},
     {sha1, 300, "0d42342d302223ad22cb3506b14f0d3032bce103"},
     {md5, 55, "d872aa0473a24da995ce4ac518ade767"},
     {md5, 56, "e23567645846677c205de80f9779081b"},
     {md5, 63, "4775b66278a8fc132ff80923378216cd"},
     {md5, 64, "71e123b70c7aa64826fcfe472694cd1c"},
     {md5, 65, "5949948f26e35203661075214faa3966"},
     {md5, 68, "147a846b45ebf9ff8e6fd7d3c2384bed"},
     {md5, 300, "1cacd39d2680cccfccf8c3265c8d037e"},
     {sha224, 55, "c6a93fabf6b9c69da008ed39b9b28bb104cdfa4244aae625eb355307"},
     {sha224, 56, "f5b9574ed655e155c6990c76920bf76ebf5e97a7e059e971f43865c6"},
     {sha224, 63, "2a5214c3584c18d8010714ee27f7bdb640a5bd88b80ffc4e3c7d0b9b"},
     {sha224, 64, "a4887c68ee6f6191ee77160a3725ab626f1887ddc1462be04df2d8c6"},
     {sha224, 65, "2bb05279298fe4f546d499d4befe9b794afb2c54bf1157c0a3c3f833"},
     {sha224, 68, "74bd1acb5e8ec307f1655923043258d731b7b0b52b2a42891d253269"},
     {sha224, 300, "3449fd4f4fa71f09b052bb028dbbb9bf86371e169f65a87cb75243f9"},
     {sha256, 55, "2900465fcb533e05a158fd2b3be0e5e3b03740d83060aa3580e0d98a96bf2384"},
     {sha256, 56, "31454ff48ef36af2f08fd511bdc37d9d5855ac23e992e5ff5445cb6b7674a674"},
     {sha256, 63, "5f6401b96532c36de4e65beec0409b69b1d181864c8009b7a04f43e5d56350d1"},
     {sha256, 64, "94eb5de4943613fd048dc93393ab06877405faa39c11f53e9386083339833e7e"},
     {sha256, 65, "fc518669b6eb4b4dd91827ecacef86689c725bd5bab888fd3b26dbb196eec954"},
     {sha256, 68, "82ca07354ec5f5f758372e2dc930e31aa6c6b4f488233e4e4333e6e3c373966e"},
     {sha256, 300, "9b854f0a59eabeac0b0ecaee1f5cd7ab3bfbc93e9b33e2a89ac338b237f300f2"},
     {sha1, 0, "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
     {md5, 0, "d41d8cd98f00b204e9800998ecf8427e"},
     {sha224, 0, "d14a028c2a3a2bc9476102bb288234c415a2b01f828ea62ac5b3e42f"},
     {sha256, 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
     }
};

auto edge(std::uint32_t algo, std::size_t size) -> bytes
{
  auto const found = std::ranges::find_if(
      edges, [&](pattern_case const& c) { return c.algo == algo and c.size == size; });
  REQUIRE(found != edges.end());
  return unhex(found->digest);
}

struct bit_case
{
  std::uint32_t    algo;
  std::size_t      bits;
  std::string_view message;
  std::string_view digest;
};

constexpr std::array<bit_case, 9> cavp_bits{
    {
     {sha1, 1, "00", "bb6b3e18f0115b57925241676f5b1ae88747b08a"},
     {sha1, 5, "48", "3320540d1c28b96ddd03eee1b186a8f2ae883fbe"},
     {sha1, 31, "3d34ab5c", "4f86b0dfee0b1f1c065488e57eb5f4b472625e7e"},
     {sha224, 1, "80", "0d05096bca2a4a77a2b47a05a59618d01174b37892376135c1b6e957"},
     {sha224, 5, "68", "e3b048552c3c387bcab37f6eb06bb79b96a4aee5ff27f51531a9551c"},
     {sha224, 31, "668a3fae", "c936ff6ddc1cb924e89911cf80ab827192a5c123be610eba1bc0bb2f"},
     {sha256, 1, "00", "bd4f9e98beb68c6ead3243b1b4c7fed75fa4feaab1f84795cbd8a98676a2a375"},
     {sha256, 5, "98", "8f136783ea6f000dccc4295d4db99b648f1c8f483b27248db103ba7cd567dbba"},
     {sha256, 31, "0d52a37e",
         "330d13a178882cb450319cf03370a05e5d65ad84713a8222b60c04301aa2e996"},
     }
};

auto abc(std::uint32_t algo) -> bytes
{
  auto const found = std::ranges::find_if(
      published, [&](vector_case const& c) { return c.algo == algo and c.message == "abc"; });
  REQUIRE(found != published.end());
  return unhex(found->digest);
}

auto field(std::uint32_t word, unsigned lsb, unsigned width) -> std::uint32_t
{
  return (word >> lsb) & ((1u << width) - 1u);
}
}

TEST_CASE("HASH registers read their reset values")
{
  hash_rig const r;
  REQUIRE(r.f.target->model("HASH") != nullptr);
  CHECK(r.read(sr) == 0x0000'0001);
  CHECK(r.read(cr) == 0);
  CHECK(r.read(str) == 0);
  CHECK(r.read(imr) == 0);
  for(std::uint32_t index = 0; index < 8; ++index)
  {
    CHECK(r.read(hr0 + 4 * index) == 0);
  }
  CHECK(r.logged.empty());
}

TEST_CASE("FIPS 180-4 and RFC 1321 vectors hash through byte-swapped DIN words")
{
  hash_rig const r;
  for(auto const& c : published)
  {
    CAPTURE(c.algo);
    CAPTURE(c.message);
    CHECK(r.hash(c.algo, text(c.message)) == unhex(c.digest));
  }
  CHECK(r.logged.empty());
}

TEST_CASE("messages at the padding edges hash right with garbage after the last valid byte")
{
  hash_rig const r;
  for(auto const& c : edges)
  {
    CAPTURE(c.algo);
    CAPTURE(c.size);
    CHECK(r.hash(c.algo, pattern(c.size)) == unhex(c.digest));
  }
  for(auto const size : {0uz, 3uz, 55uz, 56uz, 63uz, 64uz, 65uz, 119uz, 300uz})
  {
    CAPTURE(size);
    auto const message = pattern(size);
    auto const digest  = reflex::sha256_of(std::as_bytes(std::span{message}));
    bytes      expected(32);
    std::ranges::transform(
        digest, expected.begin(), [](std::byte b) { return std::to_integer<std::uint8_t>(b); });
    CHECK(r.hash(sha256, message) == expected);
  }
  CHECK(r.logged.empty());
}

TEST_CASE("every DATATYPE hashes the same message to the same digest")
{
  hash_rig const r;
  for(std::uint32_t datatype = 0; datatype < 4; ++datatype)
  {
    CAPTURE(datatype);
    for(auto const algo : {sha1, md5, sha224, sha256})
    {
      CAPTURE(algo);
      CHECK(r.hash(algo, pattern(65), datatype) == edge(algo, 65));
      CHECK(r.hash(algo, pattern(63), datatype) == edge(algo, 63));
    }
  }
  CHECK(r.logged.empty());
}

TEST_CASE("NIST CAVP bit-oriented vectors hash with NBLW 1, 5 and 31")
{
  hash_rig const r;
  for(auto const& c : cavp_bits)
  {
    CAPTURE(c.algo);
    CAPTURE(c.bits);
    for(std::uint32_t datatype = 0; datatype < 4; ++datatype)
    {
      CAPTURE(datatype);
      r.begin(c.algo, datatype);
      r.feed_bits(unhex(c.message), c.bits, datatype);
      r.finish(c.bits);
      r.settle();
      CHECK(r.digest(c.algo) == unhex(c.digest));
    }
  }
  CHECK(r.logged.empty());
}

TEST_CASE("an STR write with DCAL keeps the NBLW written before")
{
  hash_rig const r;
  for(auto const algo : {sha1, md5, sha224, sha256})
  {
    CAPTURE(algo);
    r.begin(algo);
    r.feed(pattern(64));
    r.write(str, 0);
    r.write(str, str_dcal | 24);
    CHECK(r.read(str) == 0);
    r.settle();
    CHECK(r.digest(algo) == edge(algo, 64));
  }
  r.write(str, 24);
  CHECK(r.read(str) == 24);
  r.begin(sha256);
  CHECK(r.read(str) == 24);
}

TEST_CASE("one block keeps BUSY for its cycle count and DINIS rises at its end")
{
  for(auto const algo : {sha1, md5, sha224, sha256})
  {
    CAPTURE(algo);
    hash_rig const r;
    r.begin(algo);
    CHECK((r.read(sr) & (sr_dinis | sr_busy)) == sr_dinis);
    r.feed(pattern(4));
    CHECK((r.read(sr) & sr_dinis) == 0);
    r.feed(pattern(64));
    auto const start = r.now();
    auto const end   = start + block_cycles[algo] * r.cycle_ps();
    CHECK((r.read(sr) & (sr_dinis | sr_busy)) == sr_busy);
    r.until(end - r.cycle_ps());
    CHECK((r.read(sr) & (sr_dinis | sr_busy)) == sr_busy);
    r.until(end);
    CHECK((r.read(sr) & (sr_dinis | sr_busy | sr_dcis)) == sr_dinis);
    CHECK(field(r.read(sr), 16, 5) == 16);
  }
}

TEST_CASE("a final digest takes one block time per padded block")
{
  for(auto const& [algo, size, blocks] : {
          std::tuple{sha256, 55uz, 1u},
          std::tuple{sha256, 56uz, 2u},
          std::tuple{sha1,   56uz, 2u},
          std::tuple{md5,    0uz,  1u},
          std::tuple{sha224, 64uz, 2u},
  })
  {
    CAPTURE(algo);
    CAPTURE(size);
    hash_rig const r;
    r.begin(algo);
    r.feed(pattern(size));
    r.finish(8 * size);
    auto const end = r.now() + blocks * block_cycles[algo] * r.cycle_ps();
    CHECK((r.read(sr) & (sr_busy | sr_dcis)) == sr_busy);
    r.until(end - r.cycle_ps());
    CHECK((r.read(sr) & (sr_busy | sr_dcis)) == sr_busy);
    CHECK(r.read(hr0) == 0);
    r.until(end);
    CHECK((r.read(sr) & (sr_busy | sr_dcis | sr_dinis)) == (sr_dcis | sr_dinis));
    CHECK(r.digest(algo) == edge(algo, size));
  }
}

TEST_CASE("300 bytes written back to back queue their blocks behind BUSY")
{
  for(auto const algo : {sha1, md5, sha224, sha256})
  {
    CAPTURE(algo);
    hash_rig const r;
    r.begin(algo);
    r.feed(pattern(300));
    r.finish(8 * 300);
    auto const end = r.now() + 5 * block_cycles[algo] * r.cycle_ps();
    r.until(end - r.cycle_ps());
    CHECK((r.read(sr) & (sr_busy | sr_dcis)) == sr_busy);
    r.until(end);
    CHECK((r.read(sr) & (sr_busy | sr_dcis)) == sr_dcis);
    CHECK(r.digest(algo) == edge(algo, 300));
  }
}

TEST_CASE("DCAL written while the 17th word's block is BUSY completes after both blocks")
{
  for(auto const algo : {sha1, md5, sha224, sha256})
  {
    CAPTURE(algo);
    hash_rig const r;
    r.begin(algo);
    r.feed(pattern(68));
    REQUIRE((r.read(sr) & sr_busy) != 0);
    r.finish(8 * 68);
    auto const end = r.now() + 2 * block_cycles[algo] * r.cycle_ps();
    r.until(end - r.cycle_ps());
    CHECK((r.read(sr) & sr_dcis) == 0);
    r.until(end);
    CHECK(r.digest(algo) == edge(algo, 68));
  }
}

TEST_CASE("NBWE, NBWP, DINNE and their CR mirrors follow the words written")
{
  hash_rig const r;
  CHECK(field(r.read(sr), 16, 5) == 0);
  auto const check = [&](std::uint32_t expected, std::uint32_t pushed, std::uint32_t not_empty) {
    auto const status  = r.read(sr);
    auto const control = r.read(cr);
    CHECK(field(status, 16, 5) == expected);
    CHECK(field(status, 9, 5) == pushed);
    CHECK(field(status, 15, 1) == not_empty);
    CHECK(field(control, 8, 4) == (pushed & 0xFu));
    CHECK(field(control, 12, 1) == not_empty);
  };
  r.begin(sha256);
  check(17, 0, 0);
  r.feed(pattern(4));
  check(16, 1, 1);
  r.feed(pattern(60));
  check(1, 16, 1);
  r.feed(pattern(4));
  check(0, 1, 1);
  r.settle();
  check(16, 1, 1);
  r.feed(pattern(64));
  check(0, 1, 1);
  r.settle();
  check(16, 1, 1);
  r.finish(0);
  check(16, 0, 0);
  r.settle();
  check(17, 0, 0);
}

TEST_CASE("CR keeps every field as written and only INIT latches ALGO")
{
  hash_rig const r;
  auto const     message = pattern(65);
  r.begin(sha256);
  r.feed(bytes(message.begin(), message.begin() + 32));
  auto const written = (sha1 << 17) | (2u << 4) | (1u << 13);
  r.write(cr, written);
  CHECK((r.read(cr) & ~0x1F00u) == written);
  r.feed(bytes(message.begin() + 32, message.end()));
  r.finish(8 * message.size());
  r.settle();
  CHECK(r.digest(sha256) == edge(sha256, 65));

  r.begin(md5, 0);
  r.write(cr, (md5 << 17) | (2u << 4));
  r.feed(message);
  r.finish(8 * message.size());
  r.settle();
  CHECK(r.digest(md5) == edge(md5, 65));
  CHECK(r.logged.empty());
}

TEST_CASE("HR reads 0 while DCIS is 0, after INIT and past the digest size")
{
  hash_rig const r;
  CHECK(r.hash(sha256, text("abc")) == abc(sha256));
  CHECK(r.hash(sha1, text("abc")) == abc(sha1));
  CHECK(r.read(hr0 + 20) == 0);
  CHECK(r.hash(md5, text("abc")) == abc(md5));
  CHECK(r.read(hr0 + 16) == 0);
  CHECK(r.read(hra0 + 16) == 0);
  CHECK(r.hash(sha224, text("abc")) == abc(sha224));
  CHECK(r.read(hr0 + 28) == 0);

  r.write(sr, ~sr_dcis);
  CHECK((r.read(sr) & sr_dcis) == 0);
  CHECK(r.read(hr0) == 0);
  CHECK(r.read(hra0) == 0);
  r.write(sr, 0xFFFF'FFFF);
  CHECK((r.read(sr) & sr_dcis) == 0);

  CHECK(r.hash(sha256, text("abc")) == abc(sha256));
  r.begin(sha256);
  CHECK((r.read(sr) & sr_dcis) == 0);
  for(std::uint32_t index = 0; index < 8; ++index)
  {
    CHECK(r.read(hr0 + 4 * index) == 0);
  }
}

TEST_CASE("DINIS and DCIS clear by a write of 0 and a DIN write clears DINIS")
{
  hash_rig const r;
  r.begin(sha256);
  CHECK((r.read(sr) & sr_dinis) != 0);
  r.write(sr, sr_dinis);
  CHECK((r.read(sr) & sr_dinis) != 0);
  r.write(sr, 0);
  CHECK((r.read(sr) & sr_dinis) == 0);
  r.begin(sha256);
  CHECK((r.read(sr) & sr_dinis) != 0);
  r.feed(pattern(4));
  CHECK((r.read(sr) & sr_dinis) == 0);
}

TEST_CASE("HASH_IRQn follows DINIS with DINIE and DCIS with DCIE")
{
  {
    hash_rig const r;
    r.write(imr, imr_dcie);
    r.begin(sha256);
    CHECK_FALSE(r.pending());
    r.feed(text("abc"));
    r.finish(24);
    CHECK_FALSE(r.pending());
    r.settle();
    CHECK(r.pending());
    r.write(sr, ~sr_dcis);
    r.unpend();
    CHECK_FALSE(r.pending());
  }
  {
    hash_rig const r;
    r.begin(sha256);
    r.write(imr, imr_dinie);
    CHECK(r.pending());
    r.feed(pattern(4));
    r.unpend();
    CHECK_FALSE(r.pending());
    r.feed(pattern(64));
    r.settle();
    CHECK(r.pending());
  }
  {
    hash_rig const r;
    r.begin(sha256);
    r.feed(pattern(68));
    r.finish(8 * 68);
    r.settle();
    REQUIRE((r.read(sr) & (sr_dinis | sr_dcis)) == (sr_dinis | sr_dcis));
    CHECK_FALSE(r.pending());
  }
}

TEST_CASE("INIT while BUSY drops the old block end")
{
  for(auto const algo : {sha1, md5, sha224, sha256})
  {
    CAPTURE(algo);
    hash_rig const r;
    r.begin(sha256);
    r.feed(pattern(300));
    r.finish(8 * 300);
    REQUIRE((r.read(sr) & sr_busy) != 0);
    r.begin(algo);
    CHECK((r.read(sr) & sr_busy) == 0);
    r.feed(text("abc"));
    r.settle();
    CHECK((r.read(sr) & (sr_busy | sr_dcis | sr_dinis)) == 0);
    CHECK(r.read(hr0) == 0);
    r.finish(24);
    r.settle();
    CHECK(r.digest(algo) == abc(algo));
  }
}

TEST_CASE("a snapshot taken while BUSY restores the pending end and the digest")
{
  hash_rig const r;
  r.begin(sha256);
  r.feed(pattern(300));
  r.finish(8 * 300);
  REQUIRE((r.read(sr) & sr_busy) != 0);
  auto const saved = r.f.target->snapshot();
  auto const end   = r.now() + 5 * block_cycles[sha256] * r.cycle_ps();
  auto const check = [&](hash_rig const& at) {
    at.until(end - at.cycle_ps());
    CHECK((at.read(sr) & (sr_busy | sr_dcis)) == sr_busy);
    at.until(end);
    CHECK((at.read(sr) & (sr_busy | sr_dcis)) == sr_dcis);
    CHECK(at.digest(sha256) == edge(sha256, 300));
  };
  check(r);
  r.f.target->restore(saved);
  check(r);

  hash_rig const other;
  other.f.target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  check(other);
  CHECK(r.logged.empty());
  CHECK(other.logged.empty());
}

TEST_CASE("SEC_HASH reaches the same unit as HASH")
{
  hash_rig const r;
  r.begin(sha256, 2, sec_hash_base);
  r.feed_bits(text("abc"), 24, 2, sec_hash_base);
  r.finish(24, sec_hash_base);
  r.settle();
  CHECK(r.read(sr, sec_hash_base) == r.read(sr));
  CHECK(r.digest(sha256) == abc(sha256));
  CHECK(r.read(hra0, sec_hash_base) == r.read(hra0));
  CHECK(r.logged.empty());
}

TEST_CASE("byte and half-word accesses to HASH registers are bus errors")
{
  hash_rig const r;
  auto const&    mem = r.f.target->mem();
  CHECK_FALSE(mem.write(hash_base + din, 1, 0x61).has_value());
  CHECK_FALSE(mem.write(hash_base + din + 2, 2, 0x6162).has_value());
  CHECK_FALSE(mem.write(hash_base + cr, 2, cr_init).has_value());
  CHECK_FALSE(mem.read(hash_base + sr, 1).has_value());
  CHECK_FALSE(mem.read(hash_base + hr0 + 2, 2).has_value());
  CHECK(r.read(sr) == 0x0000'0001);
  CHECK(field(r.read(sr), 9, 5) == 0);
  CHECK(r.logged.empty());
}

TEST_CASE("DMAE 1 with no controller logs once")
{
  hash_rig const r{false, controller::unrouted};
  r.write(cr, cr_dmae | (sha256 << 17));
  r.write(cr, cr_dmae | (sha256 << 17));
  CHECK((r.read(cr) & cr_dmae) != 0);
  REQUIRE(r.logged.size() == 1);
  CHECK(r.logged[0].kind == core::log_kind::model_warning);
  CHECK(r.logged[0].reg == "CR");
}

TEST_CASE("a refused CR write under strict peripherals stores nothing")
{
  hash_rig const r{true, controller::unrouted};
  auto const     before = (sha1 << 17) | (2u << 4);
  r.write(cr, before);
  auto const throws = [&](std::uint32_t value) {
    CHECK_THROWS_AS(
        static_cast<void>(r.f.target->mem().write(hash_base + cr, 4, value)), popcorn::model_error);
    CHECK((r.read(cr) & ~0x1F00u) == before);
  };
  r.begin(sha224);
  r.feed(text("abc"));
  r.write(cr, before);
  throws(cr_dmae | (sha256 << 17));
  throws(cr_init | cr_dmae | (sha256 << 17));
  CHECK(field(r.read(sr), 9, 5) == 1);
  r.finish(24);
  r.settle();
  CHECK(r.digest(sha224) == abc(sha224));
  CHECK(r.hash(sha256, text("abc")) == abc(sha256));
}

TEST_CASE("a DCAL with no word since the last digest keeps that digest")
{
  hash_rig const r;
  for(auto const algo : {sha1, md5, sha224, sha256})
  {
    CAPTURE(algo);
    REQUIRE(r.hash(algo, text("abc")) == abc(algo));
    r.write(str, str_dcal);
    CHECK((r.read(sr) & (sr_busy | sr_dcis)) == sr_dcis);
    r.settle();
    CHECK(r.digest(algo) == abc(algo));
    r.begin(algo);
    r.write(str, str_dcal);
    r.settle();
    CHECK(r.digest(algo) == edge(algo, 0));
  }
}

TEST_CASE("DMAE drives the hash_in_dma request and SR.DMAS")
{
  hash_rig const r{true, controller::recording};
  r.dma().levels.clear();
  r.write(cr, cr_init | cr_dmae | cr_mdmat | (sha256 << 17) | (2u << 4));
  CHECK((r.read(sr) & sr_dmas) != 0);
  REQUIRE_FALSE(r.dma().levels.empty());
  CHECK(r.dma().levels.back() == std::pair{hash_request, true});
  r.write(cr, (sha256 << 17) | (2u << 4));
  CHECK((r.read(sr) & sr_dmas) == 0);
  CHECK(r.dma().levels.back() == std::pair{hash_request, false});
  r.write(cr, cr_init | cr_dmae | (sha256 << 17) | (2u << 4));
  r.feed(pattern(20));
  r.write(cr, (sha256 << 17) | (2u << 4));
  CHECK((r.read(sr) & sr_dmas) != 0);
  CHECK(r.dma().levels.back() == std::pair{hash_request, true});
  r.write(cr, cr_init | (sha256 << 17) | (2u << 4));
  CHECK((r.read(sr) & sr_dmas) == 0);
  CHECK(r.dma().levels.back() == std::pair{hash_request, false});
  CHECK(r.logged.empty());
}

TEST_CASE("a transfer completed with MDMAT 0 clears DMAE and computes the digest")
{
  for(auto const algo : {sha1, md5, sha224, sha256})
  {
    CAPTURE(algo);
    hash_rig const r{true, controller::recording};
    r.write(str, 0);
    r.write(cr, cr_init | cr_dmae | (algo << 17) | (2u << 4));
    r.feed(pattern(300));
    r.complete(hash_request + 1);
    CHECK((r.read(cr) & cr_dmae) != 0);
    CHECK((r.read(sr) & sr_busy) != 0);
    r.complete(r.other_dma(), hash_request);
    CHECK((r.read(cr) & cr_dmae) != 0);
    CHECK((r.read(sr) & sr_busy) != 0);
    r.settle();
    CHECK((r.read(sr) & sr_dcis) == 0);
    r.complete();
    CHECK((r.read(cr) & cr_dmae) == 0);
    CHECK((r.read(sr) & sr_dmas) == 0);
    CHECK(r.dma().levels.back() == std::pair{hash_request, false});
    r.settle();
    CHECK(r.digest(algo) == edge(algo, 300));
    r.write(str, str_dcal);
    r.settle();
    CHECK(r.digest(algo) == edge(algo, 300));
    r.complete();
    CHECK(r.digest(algo) == edge(algo, 300));
  }
}

TEST_CASE("the automatic DCAL of a transfer keeps NBLW for a partial last word")
{
  for(auto const algo : {sha1, md5, sha224, sha256})
  {
    CAPTURE(algo);
    hash_rig const r{true, controller::recording};
    r.write(str, 24);
    r.write(cr, cr_init | cr_dmae | (algo << 17) | (2u << 4));
    r.feed_bits(text("abc"), 24);
    r.complete();
    CHECK((r.read(cr) & cr_dmae) == 0);
    r.settle();
    CHECK(r.digest(algo) == abc(algo));

    r.write(str, 8);
    r.write(cr, cr_init | cr_dmae | (algo << 17) | (2u << 4));
    r.feed_bits(pattern(65), 8 * 65);
    r.complete();
    r.settle();
    CHECK(r.digest(algo) == edge(algo, 65));
  }
}

TEST_CASE("MDMAT 1 keeps the message open across transfers")
{
  for(auto const algo : {sha1, md5, sha224, sha256})
  {
    CAPTURE(algo);
    hash_rig const r{true, controller::recording};
    auto const     message = pattern(300);
    r.write(str, 0);
    r.write(cr, cr_init | cr_dmae | cr_mdmat | (algo << 17) | (2u << 4));
    r.feed(bytes(message.begin(), message.begin() + 128));
    r.complete();
    r.settle();
    CHECK((r.read(cr) & cr_dmae) != 0);
    CHECK((r.read(sr) & sr_dcis) == 0);
    r.feed(bytes(message.begin() + 128, message.end()));
    r.write(cr, cr_dmae | (algo << 17) | (2u << 4));
    r.complete();
    r.settle();
    CHECK(r.digest(algo) == edge(algo, 300));
  }
}

namespace
{
struct hmac_case
{
  std::uint32_t    algo;
  bytes            key;
  bytes            data;
  std::string_view digest;
};

auto filled(std::uint8_t value, std::size_t count) -> bytes
{
  return bytes(count, value);
}

auto counting(std::size_t count) -> bytes
{
  bytes out(count);
  for(std::size_t index = 0; index < count; ++index)
  {
    out[index] = static_cast<std::uint8_t>(index + 1);
  }
  return out;
}

constexpr std::string_view large_key_first =
    "Test Using Larger Than Block-Size Key - Hash Key First";
constexpr std::string_view large_both_2202 =
    "Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data";
constexpr std::string_view large_both_4231 =
    "This is a test using a larger than block-size key and a larger than block-size data. The key "
    "needs to be hashed before being used by the HMAC algorithm.";

auto rfc_family(
    std::uint32_t                          algo,
    std::size_t                            short_key,
    std::size_t                            long_key,
    std::string_view                       large_both,
    std::array<std::string_view, 7> const& digests) -> std::vector<hmac_case>
{
  return {
      {algo, filled(0x0B, short_key), text("Hi There"), digests[0]},
      {algo, text("Jefe"), text("what do ya want for nothing?"), digests[1]},
      {algo, filled(0xAA, short_key), filled(0xDD, 50), digests[2]},
      {algo, counting(25), filled(0xCD, 50), digests[3]},
      {algo, filled(0x0C, short_key), text("Test With Truncation"), digests[4]},
      {algo, filled(0xAA, long_key), text(large_key_first), digests[5]},
      {algo, filled(0xAA, long_key), text(large_both), digests[6]},
  };
}

auto rfc_vectors() -> std::vector<hmac_case>
{
  std::vector<hmac_case> out;
  auto const             add = [&](std::vector<hmac_case> family) {
    std::ranges::move(family, std::back_inserter(out));
  };
  add(rfc_family(
      md5, 16, 80, large_both_2202,
      {"9294727a3638bb1c13f48ef8158bfc9d", "750c783e6ab0b503eaa86e310a5db738",
       "56be34521d144c88dbb8c733f0e8b3f6", "697eaf0aca3a3aea3a75164746ffaa79",
       "56461ef2342edc00f9bab995690efd4c", "6b1ab7fe4bd7bf8f0b62e6ce61b9d0cd",
       "6f630fad67cda0ee1fb1f562db3aa53e"}));
  add(rfc_family(
      sha1, 20, 80, large_both_2202,
      {"b617318655057264e28bc0b6fb378c8ef146be00", "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79",
       "125d7342b9ac11cd91a39af48aa17b4f63f175d3", "4c9007f4026250c6bc8414f9bf50c86c2d7235da",
       "4c1a03424b55e07fe7f27be1d58bb9324a9a5a04", "aa4ae5e15272d00e95705637ce8a3b55ed402112",
       "e8e99d0f45237d786d6bbaa7965c7808bbff1a91"}));
  add(rfc_family(
      sha224, 20, 131, large_both_4231,
      {"896fb1128abbdf196832107cd49df33f47b4b1169912ba4f53684b22",
       "a30e01098bc6dbbf45690f3a7e9e6d0f8bbea2a39e6148008fd05e44",
       "7fb3cb3588c6c1f6ffa9694d7d6ad2649365b0c1f65d69d1ec8333ea",
       "6c11506874013cac6a2abc1bb382627cec6a90d86efc012de7afec5a",
       "0e2aea68a90c8d37c988bcdb9fca6fa8099cd857c7ec4a1815cac54c",
       "95e9a0db962095adaebe9b2d6f0dbce2d499f112f2d2b7273fa6870e",
       "3a854166ac5d9f023f54d517d0b39dbd946770db9c2b95c9f6f565d1"}));
  add(rfc_family(
      sha256, 20, 131, large_both_4231,
      {"b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
       "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
       "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe",
       "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b",
       "a3b6167473100ee06e0c796c2955552bfa6f7c0a6a8aef8b93f860aab0cd20c5",
       "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
       "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"}));
  return out;
}

auto rfc_vector(std::uint32_t algo, std::size_t test) -> hmac_case
{
  auto const all = rfc_vectors();
  return all.at(7 * (algo == md5 ? 0 : algo == sha1 ? 1 : algo) + test - 1);
}

constexpr std::array<std::string_view, 4> jefe_hashed_key{
    "4547faa9ce151d58a36288bdac6809f7615dc1ec",
    "5ef35b04eb414e85e2f4795aa25cac36",
    "7b1e22819aa09c76541e6da666176b7e3c17d62ae6c06cb00cefc489",
    "142c7fede7587937a94a739156b6e8176dc3ff6d5dc58b43b5700a605fac8df3",
};

constexpr std::array<std::string_view, 4> empty_message{
    "123fd78bda0100786ae86b76f50f01bd18e477f3",
    "c9e99a43cd8fa24a840aa85c7cca0061",
    "ba7d81028e07b30466b867d8fefaa52db111d56b45df5a0e1465bf39",
    "999a901219f032cd497cadb5e6051e97b6a29ab297bd6ae722bd6062a2f59542",
};

constexpr std::array<std::string_view, 4> empty_key{
    "69536cc84eee5fe51c5b051aff8485f5c9ef0b58",
    "72c33c78cac0b7a581ac263a344ed01d",
    "da8f94de91d62154b55ea4e8d6eb133f6d553bcd1f1ba205b9488945",
    "e48411262715c8370cd5e7bf8e82bef53bd53712d007f3429351843b77c7bb9b",
};
}

TEST_CASE("RFC 2202 and RFC 4231 HMAC vectors hash through the three phases")
{
  hash_rig const r;
  for(auto const& c : rfc_vectors())
  {
    CAPTURE(c.algo);
    CAPTURE(c.key.size());
    CAPTURE(c.data.size());
    CHECK(r.hmac(c.algo, c.key, c.data, c.key.size() > 64) == unhex(c.digest));
  }
  for(auto const algo : {sha1, md5, sha224, sha256})
  {
    auto const c = rfc_vector(algo, 7);
    for(std::uint32_t datatype = 0; datatype < 4; ++datatype)
    {
      CAPTURE(algo);
      CAPTURE(datatype);
      CHECK(r.hmac(algo, c.key, c.data, true, datatype) == unhex(c.digest));
    }
  }
  CHECK(r.logged.empty());
}

TEST_CASE("the RM0456 HMAC-SHA1 example gives its digest with DATATYPE 0")
{
  hash_rig const r;
  bytes          key(64);
  for(std::size_t index = 0; index < key.size(); ++index)
  {
    key[index] = static_cast<std::uint8_t>(index);
  }
  auto const message = text("Sample message for keylen=blocklen");
  REQUIRE(message.size() == 34);
  r.hmac_begin(sha1, false, 0);
  r.phase(key, 0);
  r.phase(message, 0);
  r.phase(key, 0);
  constexpr std::array<std::uint32_t, 5> expected{
      0x5FD5'96EE, 0x78D5'553C, 0x8FF4'E72D, 0x266D'FD19, 0x2366'DA29};
  for(std::uint32_t index = 0; index < expected.size(); ++index)
  {
    CAPTURE(index);
    CHECK(r.read(hr0 + 4 * index) == expected[index]);
    CHECK(r.read(hra0 + 4 * index) == expected[index]);
  }
  CHECK(r.read(hr0 + 4 * 5) == 0);
}

TEST_CASE("LKEY 1 hashes the key whatever its length")
{
  hash_rig const r;
  for(auto const algo : {sha1, md5, sha224, sha256})
  {
    CAPTURE(algo);
    auto const c = rfc_vector(algo, 2);
    CHECK(r.hmac(algo, c.key, c.data, true) == unhex(jefe_hashed_key[algo]));
    CHECK(r.hmac(algo, c.key, c.data, false) == unhex(c.digest));
  }
}

TEST_CASE("between HMAC phases BUSY ends with DINIS 1 and DCIS 0")
{
  for(auto const algo : {sha1, md5, sha224, sha256})
  {
    CAPTURE(algo);
    hash_rig const r;
    auto const     c     = rfc_vector(algo, 1);
    auto const     block = block_cycles[algo] * r.cycle_ps();
    auto const     step  = [&](bytes const& data, std::uint64_t blocks, bool last) {
      r.feed(data);
      CHECK((r.read(sr) & sr_dinis) == 0);
      r.finish(8 * data.size());
      auto const end = r.now() + blocks * block;
      CHECK((r.read(sr) & (sr_busy | sr_dcis)) == sr_busy);
      r.until(end - r.cycle_ps());
      CHECK((r.read(sr) & (sr_busy | sr_dcis | sr_dinis)) == sr_busy);
      CHECK_FALSE(r.pending());
      r.until(end);
      if(last)
      {
        CHECK((r.read(sr) & (sr_busy | sr_dcis | sr_dinis)) == (sr_dcis | sr_dinis));
        CHECK(r.pending());
        return;
      }
      CHECK((r.read(sr) & (sr_busy | sr_dcis | sr_dinis)) == sr_dinis);
      CHECK(field(r.read(sr), 16, 5) == 0x11);
      CHECK(field(r.read(sr), 9, 5) == 0);
      CHECK(r.read(hr0) == 0);
      CHECK(r.read(hra0) == 0);
      CHECK_FALSE(r.pending());
    };
    r.write(imr, imr_dcie);
    r.hmac_begin(algo, false);
    step(c.key, 1, false);
    step(c.data, 1, false);
    step(c.key, 2, true);
    CHECK(r.digest(algo) == unhex(c.digest));

    auto const l = rfc_vector(algo, 6);
    r.write(imr, 0);
    r.hmac_begin(algo, true);
    r.feed(l.key);
    r.settle();
    r.write(sr, ~sr_dinis);
    r.unpend();
    r.finish(8 * l.key.size());
    r.write(imr, imr_dinie);
    auto const key_end = r.now() + 2 * block;
    r.until(key_end - r.cycle_ps());
    CHECK((r.read(sr) & (sr_busy | sr_dinis)) == sr_busy);
    CHECK_FALSE(r.pending());
    r.until(key_end);
    CHECK((r.read(sr) & (sr_busy | sr_dcis | sr_dinis)) == sr_dinis);
    CHECK(r.pending());
    r.write(imr, 0);
    r.unpend();
    r.phase(l.data);
    r.feed(l.key);
    r.settle();
    r.finish(8 * l.key.size());
    auto const outer_end = r.now() + 3 * block;
    r.until(outer_end - r.cycle_ps());
    CHECK((r.read(sr) & (sr_busy | sr_dcis)) == sr_busy);
    r.until(outer_end);
    CHECK(r.digest(algo) == unhex(l.digest));
  }
}

TEST_CASE("a DCAL closes every HMAC phase, empty ones included, and keeps the HMAC after the last")
{
  hash_rig const r;
  for(auto const algo : {sha1, md5, sha224, sha256})
  {
    CAPTURE(algo);
    auto const c = rfc_vector(algo, 1);
    CHECK(r.hmac(algo, c.key, {}, false) == unhex(empty_message[algo]));
    CHECK(r.hmac(algo, {}, c.data, false) == unhex(empty_key[algo]));

    REQUIRE(r.hmac(algo, c.key, c.data, false) == unhex(c.digest));
    r.write(str, str_dcal);
    CHECK((r.read(sr) & (sr_busy | sr_dcis)) == sr_dcis);
    r.settle();
    CHECK(r.digest(algo) == unhex(c.digest));

    auto const next = rfc_vector(algo, 4);
    r.phase(next.key);
    CHECK((r.read(sr) & sr_dcis) == 0);
    r.phase(next.data);
    r.phase(next.key);
    CHECK(r.digest(algo) == unhex(next.digest));
  }
}

TEST_CASE("INIT ends an HMAC between its phases")
{
  hash_rig const r;
  for(auto const algo : {sha1, md5, sha224, sha256})
  {
    CAPTURE(algo);
    auto const c = rfc_vector(algo, 3);
    r.hmac_begin(algo, false);
    r.phase(c.key);
    r.feed(c.data);
    CHECK(r.hmac(algo, c.key, c.data, false) == unhex(c.digest));

    r.hmac_begin(algo, false);
    r.phase(c.key);
    r.phase(c.data);
    CHECK(r.hash(algo, text("abc")) == abc(algo));
    CHECK(r.hmac(algo, c.key, c.data, false) == unhex(c.digest));

    r.hmac_begin(algo, false);
    r.phase(c.key);
    r.write(cr, cr_init | cr_lkey | (algo << 17) | (2u << 4));
    CHECK(r.hash(algo, text("abc")) == abc(algo));
  }
}

TEST_CASE("an HMAC key longer than 64 bytes with LKEY 0 logs once and is hashed")
{
  {
    hash_rig const r{false};
    for(auto const algo : {sha1, md5, sha224, sha256})
    {
      CAPTURE(algo);
      auto const c = rfc_vector(algo, 7);
      CHECK(r.hmac(algo, c.key, c.data, false) == unhex(c.digest));
    }
    REQUIRE(r.logged.size() == 1);
    CHECK(r.logged[0].kind == core::log_kind::model_warning);
    CHECK(r.logged[0].reg == "DIN");
    auto const c = rfc_vector(sha256, 4);
    CHECK(r.hmac(sha256, c.key, c.data, false) == unhex(c.digest));
    CHECK(r.logged.size() == 1);
  }
  {
    hash_rig const r;
    auto const     c = rfc_vector(sha256, 6);
    r.hmac_begin(sha256, false);
    r.feed(bytes(c.key.begin(), c.key.begin() + 64));
    CHECK(field(r.read(sr), 9, 5) == 16);
    CHECK_THROWS_AS(
        static_cast<void>(r.f.target->mem().write(hash_base + din, 4, 0xAAAA'AAAA)),
        popcorn::model_error);
    CHECK(field(r.read(sr), 9, 5) == 16);
    CHECK((r.read(sr) & sr_busy) == 0);
    r.finish(512);
    r.settle();
    r.phase(c.data);
    r.phase(filled(0xAA, 64));
    CHECK(
        r.digest(sha256)
        == unhex("84332a7580ed3cf75de83c644c8d2c1c262ad90e0190e5c5ae4b82b2102e8e75"));
    CHECK(r.hmac(sha256, c.key, c.data, true) == unhex(c.digest));
  }
}

TEST_CASE("each HMAC phase fed by DMA closes at its transfer's end")
{
  for(auto const algo : {sha1, md5, sha224, sha256})
  {
    CAPTURE(algo);
    hash_rig const r{true, controller::recording};
    auto const     c         = rfc_vector(algo, 7);
    auto const     control   = cr_mode | cr_lkey | (algo << 17) | (2u << 4);
    auto const     key_phase = [&] {
      r.write(str, static_cast<std::uint32_t>(8 * c.key.size() % 32));
      r.write(cr, control | cr_dmae);
      r.feed(c.key);
      r.complete();
      CHECK((r.read(cr) & cr_dmae) == 0);
      CHECK((r.read(sr) & (sr_busy | sr_dmas)) == sr_busy);
      r.settle();
    };
    r.write(cr, control | cr_init);
    key_phase();
    CHECK((r.read(sr) & (sr_dinis | sr_dcis)) == sr_dinis);

    auto const half = c.data.size() / 64 * 32;
    r.write(str, static_cast<std::uint32_t>(8 * c.data.size() % 32));
    r.write(cr, control | cr_dmae | cr_mdmat);
    r.feed(bytes(c.data.begin(), c.data.begin() + static_cast<std::ptrdiff_t>(half)));
    r.complete();
    CHECK((r.read(cr) & cr_dmae) != 0);
    r.settle();
    r.write(cr, control | cr_dmae);
    r.feed(bytes(c.data.begin() + static_cast<std::ptrdiff_t>(half), c.data.end()));
    r.complete();
    CHECK((r.read(cr) & cr_dmae) == 0);
    r.settle();
    CHECK((r.read(sr) & (sr_dinis | sr_dcis)) == sr_dinis);

    key_phase();
    CHECK(r.digest(algo) == unhex(c.digest));
    r.write(str, str_dcal);
    r.settle();
    CHECK(r.digest(algo) == unhex(c.digest));
  }
}

TEST_CASE("a snapshot taken between HMAC phases restores the phase and the inner digest")
{
  hash_rig const r;
  auto const     c = rfc_vector(sha256, 7);
  r.hmac_begin(sha256, true);
  r.phase(c.key);
  r.feed(c.data);
  r.finish(8 * c.data.size());
  REQUIRE((r.read(sr) & sr_busy) != 0);
  auto const saved  = r.f.target->snapshot();
  auto const finish = [&](hash_rig const& at) {
    at.settle();
    CHECK((at.read(sr) & (sr_dinis | sr_dcis)) == sr_dinis);
    at.phase(c.key);
    CHECK(at.digest(sha256) == unhex(c.digest));
  };
  finish(r);
  r.f.target->restore(saved);
  finish(r);

  hash_rig const other;
  other.f.target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  finish(other);
  CHECK(r.logged.empty());
  CHECK(other.logged.empty());
}

TEST_CASE("CSR0-53 read the state while DINIS is 1 and 0 while it is 0")
{
  hash_rig const r;
  auto const     message = pattern(300);
  r.begin(sha256);
  CHECK(r.read(csr0) == swap_tag);
  CHECK(r.read(csr0 + 4) == sha256);
  CHECK(r.read(csr0 + 4 * 4) == 0x6A09'E667);
  r.feed(bytes(message.begin(), message.begin() + 4));
  REQUIRE((r.read(sr) & sr_dinis) == 0);
  for(std::uint32_t index = 0; index < hmac_csrs; ++index)
  {
    CAPTURE(index);
    CHECK(r.read(csr0 + 4 * index) == 0);
  }
  r.feed(bytes(message.begin() + 4, message.begin() + 68));
  r.settle();
  REQUIRE((r.read(sr) & (sr_busy | sr_dinis)) == sr_dinis);
  CHECK(r.read(csr0) == swap_tag);
  CHECK(r.read(csr0 + 4) == (sha256 | 1u << 8));
  CHECK(r.read(csr0 + 4 * 2) == 1);
  CHECK(r.read(csr0 + 4 * 3) == 0);
  CHECK(r.read(csr0 + 4 * 4) != 0x6A09'E667);
  CHECK(r.read(csr0 + 4 * 12) == 0x4B70'95BA);
  for(std::uint32_t index = 28; index < hmac_csrs; ++index)
  {
    CAPTURE(index);
    CHECK(r.read(csr0 + 4 * index) == 0);
  }
  r.finish(8 * 68);
  r.settle();
  CHECK(r.digest(sha256) == edge(sha256, 68));

  auto const c = rfc_vector(sha1, 1);
  r.hmac_begin(sha1, false);
  CHECK(r.read(csr0 + 4) == (sha1 | 1u << 2));
  r.phase(c.key);
  CHECK(r.read(csr0 + 4) == (sha1 | 1u << 2 | 1u << 4));
  CHECK(r.read(csr0 + 4 * 2) == 1);
  r.phase(c.data);
  CHECK(r.read(csr0 + 4) == (sha1 | 1u << 2 | 2u << 4));
  CHECK(r.read(csr0 + 4 * 38) != 0);
  CHECK(r.read(csr0 + 4 * 43) == 0);
  r.phase(c.key);
  CHECK(r.digest(sha1) == unhex(c.digest));
  CHECK(r.logged.empty());
}

TEST_CASE("a hash saved at a DINIS point resumes after another hash")
{
  auto const message = pattern(300);
  for(auto const algo : {sha1, md5, sha224, sha256})
  {
    for(std::size_t const words : {0, 17, 33, 65})
    {
      for(auto const count : {hash_csrs, hmac_csrs})
      {
        CAPTURE(algo);
        CAPTURE(words);
        CAPTURE(count);
        hash_rig const r;
        auto const     at = static_cast<std::ptrdiff_t>(4 * words);
        r.write(imr, imr_dcie);
        r.begin(algo);
        r.feed(bytes(message.begin(), message.begin() + at));
        r.settle();
        auto const saved = r.save(count);
        auto const other = (algo + 1) % 4;
        CHECK(r.hash(other, text("abc")) == abc(other));
        auto const c = rfc_vector(other, 2);
        CHECK(r.hmac(other, c.key, c.data, false) == unhex(c.digest));
        r.restore(saved);
        r.unpend();
        CHECK(r.read(imr) == imr_dcie);
        CHECK(r.read(cr) == (algo << 17 | 2u << 4 | (words == 0 ? 0u : 1u << 12 | 1u << 8)));
        CHECK(field(r.read(sr), 9, 5) == (words == 0 ? 0 : 1));
        CHECK(field(r.read(sr), 16, 5) == (words == 0 ? 0x11 : 0x10));
        CHECK(field(r.read(sr), 15, 1) == (words == 0 ? 0 : 1));
        CHECK_FALSE(r.pending());
        r.feed(bytes(message.begin() + at, message.end()));
        r.finish(8 * message.size());
        r.settle();
        CHECK(r.pending());
        CHECK(r.digest(algo) == edge(algo, 300));
        CHECK(r.logged.empty());
      }
    }
  }
}

TEST_CASE("an HMAC saved in each phase resumes after another HMAC")
{
  for(auto const algo : {sha1, md5, sha224, sha256})
  {
    auto const c = rfc_vector(algo, 7);
    REQUIRE(c.key.size() >= 68);
    REQUIRE(c.data.size() >= 68);
    for(std::size_t phase = 0; phase < 3; ++phase)
    {
      for(std::size_t const words : {0, 17})
      {
        CAPTURE(algo);
        CAPTURE(phase);
        CAPTURE(words);
        hash_rig const r;
        r.hmac_begin(algo, true);
        for(std::size_t step = 0; step < 3; ++step)
        {
          auto const& data = step == 1 ? c.data : c.key;
          if(step != phase)
          {
            r.phase(data);
            continue;
          }
          auto const at = static_cast<std::ptrdiff_t>(4 * words);
          r.feed(bytes(data.begin(), data.begin() + at));
          r.settle();
          auto const saved = r.save(hmac_csrs);
          auto const other = (algo + 2) % 4;
          auto const o     = rfc_vector(other, 6);
          CHECK(r.hmac(other, o.key, o.data, true) == unhex(o.digest));
          CHECK(r.hash(algo, text("abc")) == abc(algo));
          r.restore(saved);
          r.feed(bytes(data.begin() + at, data.end()));
          r.finish(8 * data.size());
          r.settle();
          CHECK((r.read(sr) & sr_dcis) == (step == 2 ? sr_dcis : 0u));
        }
        CHECK(r.digest(algo) == unhex(c.digest));
        CHECK(r.logged.empty());
      }
    }
  }
}

namespace
{
constexpr std::uint32_t channel10 = 0x4002'0550;
constexpr std::uint32_t ch_cfcr   = 0x0C;
constexpr std::uint32_t ch_csr    = 0x10;
constexpr std::uint32_t ch_ccr    = 0x14;
constexpr std::uint32_t ch_ctr1   = 0x40;
constexpr std::uint32_t ch_ctr2   = 0x44;
constexpr std::uint32_t ch_cbr1   = 0x48;
constexpr std::uint32_t ch_csar   = 0x4C;
constexpr std::uint32_t ch_cdar   = 0x50;
constexpr std::uint32_t ch_cllr   = 0x7C;
constexpr std::uint32_t ccr_en    = 1u << 0;
constexpr std::uint32_t ccr_reset = 1u << 1;
constexpr std::uint32_t ccr_susp  = 1u << 2;
constexpr std::uint32_t csr_suspf = 1u << 13;
constexpr std::uint32_t staged    = 0x2002'0000;

struct dma_feed
{
  void stage(bytes const& data) const
  {
    for(std::size_t at = 0; at < data.size(); at += 4)
    {
      std::uint32_t word = 0;
      for(std::size_t byte = 0; byte < 4 and at + byte < data.size(); ++byte)
      {
        word |= std::uint32_t{data[at + byte]} << (8 * byte);
      }
      REQUIRE(
          r.f.target->mem().write(staged + static_cast<std::uint32_t>(at), 4, word).has_value());
    }
  }

  void start(std::uint32_t from, std::uint32_t words)
  {
    first = from;
    count = words;
    r.write(ch_cfcr, 0x7F00, channel10);
    r.write(ch_ctr1, 1u << 3 | 2u | 2u << 16 | (beats - 1) << 4, channel10);
    r.write(ch_ctr2, 1u << 10 | hash_request, channel10);
    r.write(ch_cbr1, 4 * words, channel10);
    r.write(ch_csar, staged + 4 * from, channel10);
    r.write(ch_cdar, hash_base + din, channel10);
    r.write(ch_cllr, 0, channel10);
    r.write(ch_ccr, ccr_en, channel10);
  }

  [[nodiscard]] auto sent() const -> std::uint32_t
  {
    return first + count - (r.read(ch_cbr1, channel10) & 0xFFFFu) / 4;
  }

  [[nodiscard]] auto enabled() const -> bool
  {
    return (r.read(ch_ccr, channel10) & ccr_en) != 0;
  }

  template <typename Done> [[nodiscard]] auto until(Done done) const -> bool
  {
    for(int step = 0; step < 4000; ++step)
    {
      if(done())
      {
        return true;
      }
      r.until(r.now() + r.cycle_ps());
    }
    return done();
  }

  void abort() const
  {
    r.write(ch_ccr, r.read(ch_ccr, channel10) | ccr_susp, channel10);
    REQUIRE(until([&] { return (r.read(ch_csr, channel10) & csr_suspf) != 0; }));
    r.write(ch_ccr, r.read(ch_ccr, channel10) | ccr_reset, channel10);
    CHECK_FALSE(enabled());
  }

  hash_rig const& r;
  std::uint32_t   beats{1};
  std::uint32_t   first{};
  std::uint32_t   count{};
};

struct suspend_case
{
  std::uint32_t beats;
  std::uint32_t at;
  std::uint32_t held;
};
}

TEST_CASE("a DMA suspend per RM0456 51.4.8 completes the current block before DMAS drops")
{
  auto const message = pattern(300);
  for(auto const algo : {sha1, md5, sha224, sha256})
  {
    for(auto const s : {
            suspend_case{1, 21, 33},
            suspend_case{4, 21, 36},
            suspend_case{1, 17, 17}
    })
    {
      CAPTURE(algo);
      CAPTURE(s.beats);
      CAPTURE(s.at);
      hash_rig const r;
      dma_feed       d{r, s.beats};
      auto const     control = (algo << 17) | (2u << 4);
      d.stage(message);
      r.write(str, 0);
      r.write(cr, cr_init | cr_dmae | control);
      d.start(0, 75);
      REQUIRE(d.until([&] { return d.sent() >= s.at; }));
      REQUIRE(d.sent() < 32);
      if(s.at == s.held)
      {
        REQUIRE(d.sent() == s.at);
      }
      r.write(cr, control);
      CHECK((r.read(sr) & sr_dmas) == (s.at == s.held ? 0u : sr_dmas));
      REQUIRE(d.until([&] { return (r.read(sr) & sr_dmas) == 0; }));
      CHECK(d.sent() == s.held);
      r.until(r.now() + 200 * r.cycle_ps());
      CHECK(d.sent() == s.held);
      CHECK(d.enabled());
      d.abort();
      CHECK((r.read(sr) & sr_dcis) == 0);
      REQUIRE(d.until([&] { return (r.read(sr) & sr_dinis) != 0; }));
      auto const saved = r.save(hash_csrs);
      CHECK(field(saved.csr[1], 8, 5) == s.held - 16 * ((s.held - 1) / 16));
      auto const other = (algo + 1) % 4;
      CHECK(r.hash(other, text("abc")) == abc(other));
      r.restore(saved);
      CHECK((r.read(sr) & sr_dmas) == 0);
      d.start(s.held, 75 - s.held);
      r.write(cr, saved.cr | cr_dmae);
      r.settle();
      CHECK_FALSE(d.enabled());
      CHECK(r.digest(algo) == edge(algo, 300));
      CHECK(r.logged.empty());
    }
  }
}

TEST_CASE("a snapshot taken while a DMA suspend holds or launched its block resumes elsewhere")
{
  auto const message = pattern(300);
  for(auto const algo : {sha1, md5, sha224, sha256})
  {
    for(auto const s : {
            suspend_case{1, 21, 33},
            suspend_case{4, 21, 36}
    })
    {
      for(auto const launched : {false, true})
      {
        CAPTURE(algo);
        CAPTURE(s.beats);
        CAPTURE(launched);
        hash_rig const r;
        dma_feed       d{r, s.beats};
        auto const     control = (algo << 17) | (2u << 4);
        d.stage(message);
        r.write(str, 0);
        r.write(cr, cr_init | cr_dmae | control);
        d.start(0, 75);
        REQUIRE(d.until([&] { return d.sent() >= s.at; }));
        REQUIRE(d.sent() < 32);
        r.write(cr, control);
        REQUIRE((r.read(sr) & sr_dmas) != 0);
        if(launched)
        {
          REQUIRE(d.until([&] { return (r.read(sr) & sr_dmas) == 0; }));
          REQUIRE((r.read(sr) & (sr_busy | sr_dinis)) == sr_busy);
        }
        auto const saved = r.f.target->snapshot();

        hash_rig const other;
        dma_feed       e{other, s.beats, 0, 75};
        other.f.target->restore(
            core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
        CHECK((other.read(sr) & sr_dmas) == (launched ? 0u : sr_dmas));
        REQUIRE(e.until([&] { return (other.read(sr) & sr_dmas) == 0; }));
        CHECK(e.sent() == s.held);
        other.until(other.now() + 200 * other.cycle_ps());
        CHECK(e.sent() == s.held);
        e.abort();
        REQUIRE(e.until([&] { return (other.read(sr) & sr_dinis) != 0; }));
        auto const context = other.save(hash_csrs);
        CHECK(other.hash((algo + 1) % 4, text("abc")) == abc((algo + 1) % 4));
        other.restore(context);
        e.start(s.held, 75 - s.held);
        other.write(cr, context.cr | cr_dmae);
        other.settle();
        CHECK_FALSE(e.enabled());
        CHECK(other.digest(algo) == edge(algo, 300));
        CHECK(r.logged.empty());
        CHECK(other.logged.empty());
      }
    }
  }
}

TEST_CASE("an HMAC message phase fed by DMA suspends and resumes per RM0456 51.4.8")
{
  for(auto const algo : {sha1, md5, sha224, sha256})
  {
    CAPTURE(algo);
    auto const     c     = rfc_vector(algo, 7);
    auto const     words = static_cast<std::uint32_t>((c.data.size() + 3) / 4);
    hash_rig const r;
    dma_feed       d{r};
    auto const     control = cr_mode | cr_lkey | (algo << 17) | (2u << 4);
    REQUIRE(words > 17);
    d.stage(c.data);
    r.hmac_begin(algo, true);
    r.phase(c.key);
    r.write(str, static_cast<std::uint32_t>(8 * (c.data.size() % 4)));
    r.write(cr, control | cr_dmae);
    d.start(0, words);
    REQUIRE(d.until([&] { return d.sent() >= 5; }));
    REQUIRE(d.sent() < 16);
    r.write(cr, control);
    CHECK((r.read(sr) & sr_dmas) != 0);
    REQUIRE(d.until([&] { return (r.read(sr) & sr_dmas) == 0; }));
    CHECK(d.sent() == 17);
    d.abort();
    CHECK((r.read(sr) & sr_dcis) == 0);
    REQUIRE(d.until([&] { return (r.read(sr) & sr_dinis) != 0; }));
    auto const saved = r.save(hmac_csrs);
    auto const other = (algo + 2) % 4;
    auto const o     = rfc_vector(other, 6);
    CHECK(r.hmac(other, o.key, o.data, true) == unhex(o.digest));
    r.restore(saved);
    d.start(17, words - 17);
    r.write(cr, saved.cr | cr_dmae);
    r.settle();
    CHECK_FALSE(d.enabled());
    CHECK((r.read(sr) & sr_dcis) == 0);
    r.phase(c.key);
    CHECK(r.digest(algo) == unhex(c.digest));
    CHECK(r.logged.empty());
  }
}

TEST_CASE("a channel completing while a suspend holds the request drops it and ends the transfer")
{
  auto const message = pattern(300);
  for(auto const algo : {sha1, md5, sha224, sha256})
  {
    for(auto const multiple : {false, true})
    {
      CAPTURE(algo);
      CAPTURE(multiple);
      hash_rig const r;
      dma_feed       d{r};
      auto const     control = (multiple ? cr_mdmat : 0u) | (algo << 17) | (2u << 4);
      d.stage(message);
      r.write(str, 0);
      r.write(cr, cr_init | cr_dmae | control);
      d.start(0, 75);
      REQUIRE(d.until([&] { return d.sent() >= 68; }));
      REQUIRE(d.sent() < 75);
      r.write(cr, control);
      CHECK((r.read(sr) & sr_dmas) != 0);
      REQUIRE(d.until([&] { return (r.read(sr) & sr_dmas) == 0; }));
      CHECK(d.sent() == 75);
      CHECK_FALSE(d.enabled());
      r.settle();
      if(multiple)
      {
        CHECK((r.read(sr) & (sr_dcis | sr_dinis)) == 0);
        r.write(str, str_dcal);
        r.settle();
      }
      CHECK(r.digest(algo) == edge(algo, 300));
      CHECK(r.logged.empty());
    }
  }
}

TEST_CASE("NBLW comes back with STR, not with the context")
{
  auto const     message = pattern(300);
  hash_rig const reference;
  reference.begin(sha256);
  reference.feed(message);
  reference.write(str, 24);
  reference.write(str, str_dcal);
  reference.settle();
  auto const short_last = reference.digest(sha256);
  REQUIRE(short_last != edge(sha256, 300));

  for(std::uint32_t const nblw : {0u, 24u})
  {
    CAPTURE(nblw);
    hash_rig const r;
    r.begin(sha256);
    r.write(str, nblw);
    r.feed(bytes(message.begin(), message.begin() + 68));
    r.settle();
    auto const saved = r.save(hash_csrs);
    CHECK(saved.str == nblw);
    CHECK(r.hash(sha1, text("abc")) == abc(sha1));
    REQUIRE(r.read(str) == 24);
    r.write(str, 0);
    r.restore(saved);
    CHECK(r.read(str) == nblw);
    r.feed(bytes(message.begin() + 68, message.end()));
    r.write(str, str_dcal);
    r.settle();
    CHECK(r.digest(sha256) == (nblw == 0 ? edge(sha256, 300) : short_last));
  }
}

TEST_CASE("a context restored under a CR it was not saved with is refused")
{
  auto const message = pattern(300);
  auto const saved   = [&](hash_rig const& r, std::uint32_t algo, bool hmac) {
    if(hmac)
    {
      r.hmac_begin(algo, false);
      r.phase(rfc_vector(algo, 1).key);
    }
    else
    {
      r.begin(algo);
    }
    r.feed(bytes(message.begin(), message.begin() + 68));
    r.settle();
    return r.save(hmac_csrs);
  };
  auto const config = [](context c, std::uint32_t value) {
    c.csr[1] = value;
    return c;
  };
  auto const control = [](context c, std::uint32_t value) {
    c.cr = value;
    return c;
  };
  hash_rig const                              source;
  auto const                                  plain = saved(source, sha256, false);
  auto const                                  keyed = saved(source, sha256, true);
  std::vector<std::pair<context, bool>> const bad{
      {control(plain, sha224 << 17 | 2u << 4),                     false},
      {control(plain, sha256 << 17 | cr_mode | 2u << 4),           false},
      {control(keyed, sha256 << 17 | cr_mode | cr_lkey | 2u << 4), true },
      {control(keyed, sha256 << 17 | 2u << 4),                     true },
      {config(plain,  plain.csr[1] | 1u << 2),                     false},
      {config(plain,  plain.csr[1] | 1u << 4),                     false},
      {config(keyed,  (keyed.csr[1] & ~0x30u) | 3u << 4),          true },
      {config(plain,  (plain.csr[1] & ~0x1F00u) | 17u << 8),       false},
      {config(plain,  plain.csr[1] | 1u << 13),                    false},
      {config(plain,  plain.csr[1] | 1u << 6),                     false},
  };
  for(std::size_t index = 0; index < bad.size(); ++index)
  {
    CAPTURE(index);
    auto const& [wrong, hmac] = bad[index];
    auto const& right         = hmac ? keyed : plain;
    {
      hash_rig const r;
      r.reinit(wrong);
      r.load(wrong, 0, 1);
      auto const before = r.save(hmac_csrs);
      CHECK_THROWS_AS(
          static_cast<void>(r.f.target->mem().write(hash_base + csr0 + 4, 4, wrong.csr[1])),
          popcorn::model_error);
      CHECK(r.save(hmac_csrs).csr == before.csr);
      CHECK(field(r.read(sr), 9, 5) == 0);
      r.restore(right);
      r.feed(bytes(message.begin() + 68, message.end()));
      r.finish(8 * message.size());
      r.settle();
      if(not hmac)
      {
        CHECK(r.digest(sha256) == edge(sha256, 300));
      }
      else
      {
        CHECK(r.save(hmac_csrs).csr[1] == (sha256 | 1u << 2 | 2u << 4));
      }
    }
    {
      hash_rig const r{false};
      r.reinit(wrong);
      auto const fresh = r.save(hmac_csrs);
      r.load(wrong, 0, wrong.csr.size());
      CHECK(r.save(hmac_csrs).csr == fresh.csr);
      r.load(wrong, 2, wrong.csr.size());
      CHECK(r.save(hmac_csrs).csr == fresh.csr);
      REQUIRE(r.logged.size() == 1);
      CHECK(r.logged[0].kind == core::log_kind::model_warning);
      CHECK(r.logged[0].reg == "CSR1");
    }
  }
}

TEST_CASE("a CSR0 other than the tag drops the CSR writes up to INIT or the tag")
{
  auto const     message = pattern(300);
  hash_rig const source;
  source.begin(md5);
  source.feed(bytes(message.begin(), message.begin() + 132));
  source.settle();
  auto const saved = source.save(hash_csrs);
  auto       wrong = saved;
  wrong.csr[0] ^= 0x100;
  {
    hash_rig const r;
    r.reinit(saved);
    auto const fresh = r.save(hash_csrs);
    CHECK_THROWS_AS(
        static_cast<void>(r.f.target->mem().write(hash_base + csr0, 4, wrong.csr[0])),
        popcorn::model_error);
    r.load(saved, 0, hash_csrs);
    CHECK(r.save(hash_csrs).csr == saved.csr);
    CHECK(fresh.csr != saved.csr);
    r.feed(bytes(message.begin() + 132, message.end()));
    r.finish(8 * message.size());
    r.settle();
    CHECK(r.digest(md5) == edge(md5, 300));
  }
  hash_rig const r{false};
  r.reinit(saved);
  auto const fresh = r.save(hash_csrs);
  r.load(wrong, 0, hash_csrs);
  CHECK(r.save(hash_csrs).csr == fresh.csr);
  r.load(wrong, 0, hash_csrs);
  REQUIRE(r.logged.size() == 1);
  CHECK(r.logged[0].reg == "CSR0");
  r.load(saved, 0, hash_csrs);
  CHECK(r.save(hash_csrs).csr == saved.csr);
  r.load(wrong, 0, 1);
  r.reinit(saved);
  r.load(saved, 1, hash_csrs);
  CHECK(r.save(hash_csrs).csr == saved.csr);
  r.feed(bytes(message.begin() + 132, message.end()));
  r.finish(8 * message.size());
  r.settle();
  CHECK(r.digest(md5) == edge(md5, 300));
  CHECK(r.logged.size() == 1);
}

TEST_CASE("a snapshot taken during a restore keeps the words loaded and a refusal")
{
  auto const     message = pattern(300);
  hash_rig const r{false};
  r.begin(sha224);
  r.feed(bytes(message.begin(), message.begin() + 196));
  r.settle();
  auto const saved = r.save(hash_csrs);
  CHECK(r.hash(sha256, text("abc")) == abc(sha256));
  r.reinit(saved);
  r.load(saved, 0, 20);
  auto const half   = r.f.target->snapshot();
  auto const finish = [&](hash_rig const& at) {
    at.load(saved, 20, hash_csrs);
    CHECK(at.save(hash_csrs).csr == saved.csr);
    at.feed(bytes(message.begin() + 196, message.end()));
    at.finish(8 * message.size());
    at.settle();
    CHECK(at.digest(sha224) == edge(sha224, 300));
  };
  finish(r);
  r.f.target->restore(half);
  finish(r);
  hash_rig const other{false};
  other.f.target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(half.data()))});
  finish(other);
  CHECK(other.logged.empty());

  r.reinit(saved);
  auto const fresh = r.save(hash_csrs);
  r.write(csr0, 0);
  auto const     refused = r.f.target->snapshot();
  hash_rig const again{false};
  again.f.target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(refused.data()))});
  again.load(saved, 1, hash_csrs);
  CHECK(again.save(hash_csrs).csr == fresh.csr);
  again.load(saved, 0, hash_csrs);
  CHECK(again.save(hash_csrs).csr == saved.csr);
}

namespace
{
void rcc_reset(stm32_fixture const& f, std::uint32_t level)
{
  auto const& desc  = f.target->peripheral("RCC").description();
  auto const* found = popcorn::find_register(desc, "RCC_AHB2RSTR1");
  REQUIRE(found != nullptr);
  auto const bit =
      std::ranges::find(found->fields, std::string_view{"HASHRST"}, &popcorn::field_desc::name);
  REQUIRE(bit != found->fields.end());
  REQUIRE(f.target->mem().write(desc.base + found->offset, 4, level << bit->lsb).has_value());
}
}

TEST_CASE("HASHRST lowers the hash_in_dma request on the controller")
{
  hash_rig const r{true, controller::recording};
  r.write(cr, cr_init | cr_dmae | cr_mdmat | (sha256 << 17) | (2u << 4));
  REQUIRE(r.dma().levels.back() == std::pair{hash_request, true});
  rcc_reset(r.f, 1);
  rcc_reset(r.f, 0);
  CHECK(r.dma().levels.back() == std::pair{hash_request, false});
  CHECK(r.read(cr) == 0);
}

TEST_CASE("HASHRST held ignores DIN and CSR writes, the digest after release is the reset one")
{
  hash_rig const r{false};
  rcc_reset(r.f, 1);
  r.write(cr, cr_init | (sha256 << 17) | (2u << 4));
  r.write(din, 0x6162'6300);
  r.write(csr0, 0x1234'5678);
  rcc_reset(r.f, 0);
  CHECK(r.read(cr) == 0);
  CHECK(r.f.target->peripheral("HASH").peek(csr0) == 0);
  CHECK_FALSE(r.pending());
  r.begin(sha256);
  r.feed(text("abc"));
  r.finish(24);
  r.settle();
  CHECK(
      r.digest(sha256)
      == unhex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

TEST_CASE("HASHRST with a digest busy and DINIS raised drops the busy event and the line")
{
  hash_rig const r{false};
  r.write(imr, imr_dinie | imr_dcie);
  r.begin(sha256);
  r.feed(text("abc"));
  r.finish(24);
  REQUIRE((r.read(sr) & sr_busy) != 0);
  REQUIRE(r.pending());
  r.unpend();
  rcc_reset(r.f, 1);
  rcc_reset(r.f, 0);
  CHECK(r.read(sr) == 0x1);
  CHECK(r.read(imr) == 0);
  r.write(imr, imr_dcie);
  r.settle();
  CHECK(r.read(sr) == 0x1);
  CHECK(r.read(hr0) == 0);
  CHECK_FALSE(r.pending());
}
