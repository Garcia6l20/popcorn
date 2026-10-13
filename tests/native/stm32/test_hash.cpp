#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>

#include <doctest/doctest.h>
#include <reflex/hash/sha256.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
namespace core = popcorn::core;
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

auto idle(bool strict) -> stm32_fixture
{
  auto options               = popcorn::testing::stm32_options();
  options.strict_peripherals = strict;
  stm32_fixture f{u575(), "hash_ll", options};
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("hash_ll", "main")})
          .reason
      == stop_reason::address);
  f.set("pc", sym("hash_ll", "idle"));
  return f;
}

struct hash_rig
{
  explicit hash_rig(bool strict = true) : f{idle(strict)}
  {}

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

TEST_CASE("MODE 1, DMAE 1 and the context swap registers log once each and hash on")
{
  hash_rig const r{false};
  r.write(cr, cr_init | cr_mode | (sha256 << 17) | (2u << 4));
  r.write(cr, cr_init | cr_mode | (sha256 << 17) | (2u << 4));
  CHECK((r.read(cr) & cr_mode) != 0);
  r.feed(text("abc"));
  r.finish(24);
  r.settle();
  CHECK(r.digest(sha256) == abc(sha256));
  REQUIRE(r.logged.size() == 1);
  CHECK(r.logged[0].kind == core::log_kind::model_warning);
  CHECK(r.logged[0].reg == "CR");

  r.write(cr, cr_dmae | (sha256 << 17));
  r.write(cr, cr_dmae | (sha256 << 17));
  CHECK((r.read(cr) & cr_dmae) != 0);
  REQUIRE(r.logged.size() == 2);
  CHECK(r.logged[1].reg == "CR");

  r.write(csr0 + 4, 0x1234'5678);
  CHECK(r.read(csr0 + 4) == 0);
  CHECK(r.read(csr0) == 0);
  r.write(csr0 + 4 * 53, 1);
  REQUIRE(r.logged.size() == 3);
  CHECK(r.logged[2].reg == "CSR1");
}

TEST_CASE("a refused CR write under strict peripherals stores nothing")
{
  hash_rig const r;
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
  throws(cr_init | cr_mode | (sha256 << 17));
  throws(cr_dmae | (sha256 << 17));
  throws(cr_init | cr_dmae | (sha256 << 17));
  CHECK(field(r.read(sr), 9, 5) == 1);
  r.finish(24);
  r.settle();
  CHECK(r.digest(sha224) == abc(sha224));
  CHECK(r.hash(sha256, text("abc")) == abc(sha256));
  CHECK_THROWS_AS(
      static_cast<void>(r.f.target->mem().write(hash_base + csr0, 4, 1)), popcorn::model_error);
  CHECK_THROWS_AS(
      static_cast<void>(r.f.target->mem().read(hash_base + csr0, 4)), popcorn::model_error);
}
