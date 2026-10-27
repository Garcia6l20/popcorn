#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::h573;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

using bytes = std::vector<std::uint8_t>;

constexpr std::string_view firmware = "h573_rcc_ll";

constexpr std::uint32_t hash_base = 0x420C'0400;
constexpr std::uint32_t cr        = 0x00;
constexpr std::uint32_t din       = 0x04;
constexpr std::uint32_t str       = 0x08;
constexpr std::uint32_t hra0      = 0x0C;
constexpr std::uint32_t imr       = 0x20;
constexpr std::uint32_t sr        = 0x24;
constexpr std::uint32_t csr0      = 0xF8;
constexpr std::uint32_t hr0       = 0x310;
constexpr std::uint32_t cr_init   = 1u << 2;
constexpr std::uint32_t cr_dmae   = 1u << 3;
constexpr std::uint32_t cr_mode   = 1u << 6;
constexpr std::uint32_t cr_mdmat  = 1u << 13;
constexpr std::uint32_t cr_lkey   = 1u << 16;
constexpr std::uint32_t str_dcal  = 1u << 8;
constexpr std::uint32_t sr_dinis  = 1u << 0;
constexpr std::uint32_t sr_dcis   = 1u << 1;
constexpr std::uint32_t sr_busy   = 1u << 3;
constexpr std::uint32_t imr_dcie  = 1u << 1;
constexpr std::uint32_t cr_saved = cr_dmae | (3u << 4) | cr_mode | cr_mdmat | cr_lkey | (15u << 17);
constexpr std::uint32_t hr_count = 42;
constexpr std::uint64_t ms       = 1'000'000'000;

constexpr std::uint32_t sha1       = 0;
constexpr std::uint32_t sha224     = 2;
constexpr std::uint32_t sha256     = 3;
constexpr std::uint32_t sha384     = 12;
constexpr std::uint32_t sha512_224 = 13;
constexpr std::uint32_t sha512_256 = 14;
constexpr std::uint32_t sha512     = 15;

constexpr auto digest_bytes(std::uint32_t algo) -> std::size_t
{
  switch(algo)
  {
    case sha1:
      return 20;
    case sha224:
    case sha512_224:
      return 28;
    case sha384:
      return 48;
    case sha512:
      return 64;
    default:
      return 32;
  }
}

constexpr auto block_bytes(std::uint32_t algo) -> std::size_t
{
  switch(algo)
  {
    case sha384:
    case sha512_224:
    case sha512_256:
    case sha512:
      return 128;
    default:
      return 64;
  }
}

constexpr auto cycles_of(std::uint32_t algo) -> std::uint64_t
{
  switch(algo)
  {
    case sha1:
      return 82;
    case sha384:
    case sha512_224:
    case sha512_256:
    case sha512:
      return 98;
    default:
      return 66;
  }
}

auto pattern(std::size_t size, std::uint32_t seed = 0) -> bytes
{
  bytes out(size);
  for(std::size_t index = 0; index < size; ++index)
  {
    out[index] = static_cast<std::uint8_t>(index * 37 + 11 + seed * 13);
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

auto field(std::uint32_t word, unsigned lsb, unsigned width) -> std::uint32_t
{
  return (word >> lsb) & ((1u << width) - 1u);
}

struct digest_case
{
  std::uint32_t    algo;
  std::string_view message;
  std::size_t      size;
  std::string_view digest;

  [[nodiscard]] auto input() const -> bytes
  {
    return message.empty() ? pattern(size) : text(message);
  }
};

struct hmac_case
{
  std::uint32_t    algo;
  std::size_t      key;
  std::size_t      data;
  bool             long_key;
  std::string_view digest;
};

constexpr std::array<digest_case, 32> digests{
    {
     {0, "abc", 3, "a9993e364706816aba3e25717850c26c9cd0d89d"},
     {2, "abc", 3, "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7"},
     {3, "abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
     {12, "abc", 3,
         "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca1"
         "34c825a7"},
     {12,
         {},
         0,
         "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f1"
         "4898b95b"},
     {13, "abc", 3, "4634270f707b6a54daae7530460842e20e37ed265ceee9a43e8924aa"},
     {13, {}, 0, "6ed0dd02806fa89e25de060c19d3ac86cabb87d6a0ddd05c333b84f4"},
     {14, "abc", 3, "53048e2681941ef99b2e29b76b4c7dabe4c2d0c634fc6d46e0e2f13107e7af23"},
     {14, {}, 0, "c672b8d1ef56ed28ab87c3622c5114069bdd3ad7b8f9737498d0c01ecef0967a"},
     {15, "abc", 3,
         "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23"
         "a3feebbd454d4423643ce80e2a9ac94fa54ca49f"},
     {15,
         {},
         0,
         "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2"
         "877eec2f63b931bd47417a81a538327af927da3e"},
     {3,
         "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrl"
         "mnopqrsmnopqrstnopqrstu",
         112, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"},
     {12,
         "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrl"
         "mnopqrsmnopqrstnopqrstu",
         112,
         "09330c33f71147e83d192fc782cd1b4753111b173b3b05d22fa08086e3b0f712fcc7c71a557e2db966c3e9fa"
         "91746039"},
     {13,
         "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrl"
         "mnopqrsmnopqrstnopqrstu",
         112, "23fec5bb94d60b23308192640b0c453335d664734fe40e7268674af9"},
     {14,
         "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrl"
         "mnopqrsmnopqrstnopqrstu",
         112, "3928e184fb8690f840da3988121d31be65cb9d3ef83ee6146feac861e19b563a"},
     {15,
         "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrl"
         "mnopqrsmnopqrstnopqrstu",
         112,
         "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99de"
         "c4b5433ac7d329eeb6dd26545e96e55b874be909"},
     {12,
         {},
         111,
         "d6517d210fc664af0e137a4ca19d7a299465109f40cc0fa9ac7f9d4ac9c77e6687f3cb060890ef7e96f9f525"
         "2dc528ab"},
     {12,
         {},
         112,
         "76078618f7c580d9ca1f9990c2adb38a899f7feda86153dda507b72f1bbafb046ab835aa8d325a89a67e3deb"
         "81ad1ad5"},
     {12,
         {},
         128,
         "13274b7eaabb6a3203608240e792e825ddb816853a66fde047b368cc7638d61b9f215323c4160677301ae4d1"
         "78cd8579"},
     {12,
         {},
         300,
         "981948ac75788a432936364b4d2c89bb8a5e0f0637e46eb05a845b3a4cc2ac1bbbe829cccf3ba46bd33a15d0"
         "c4a3cdf7"},
     {13, {}, 111, "a58fca1508a10042d2726853fd87baedc738d8c9353f05c18e3d917f"},
     {13, {}, 112, "910acbee9104e54d812d15d96b7c0e6ece09a62b144c3ffec072fdc9"},
     {13, {}, 128, "ed05e62e49b2d9345131438d9da5b9e764910ba482f503132abe92d9"},
     {13, {}, 300, "197368fb2b7a9ee40696e4f82c69ba117ad2b2bb6f9de87eaaceb3ad"},
     {14, {}, 111, "84d71ab5b507d9440277cc05cdde25e57e396e3a880ee24b0009fade06bc4cd7"},
     {14, {}, 112, "ff0114959575adb2a8a59ddecba0e51855b385eb4a24017c923a0998062698f2"},
     {14, {}, 128, "aa399434cc7ac88f2a667659396d5e9659ccbdc53d8f0d0c9ccd78dbfd970921"},
     {14, {}, 300, "8524b162773b4a5458e47e73ac4fc4acaa85c0a410ced934b33a19f15c5ae79d"},
     {15,
         {},
         111,
         "4d1db900250c96436052fbca79c13acbf378aad9c35b87d94c3803264df61fd22cbd327c8938d024db372abf"
         "4208934ee09367d571d6c670bf74ee07b83e7506"},
     {15,
         {},
         112,
         "dfb715ca3478a894302ace39c42d1d6646e1044f2247a6274d8b42d155d2fdbe7017195e85cfba96bedc51f8"
         "4c44638978a540039ff09c64cef6c0c5ccc8f7b6"},
     {15,
         {},
         128,
         "0b4815d35f9d07b1a30de2790e1be2a720234295cd7b4d9e9af51719ff90019f1fe6d4e402a7dcc417708502"
         "3dc460ab743dad9b2c1dda42662bda5d3b2e155b"},
     {15,
         {},
         300,
         "ddbe2ce66a8112ce9a18335aca0c7bbf14bbc8102281e28fdc2d7d9b8d99957d14182c566d3faeac4f40c6ca"
         "e9b5c87d8e4d4d12dac695a79641b952955f1dee"},
     }
};

constexpr std::array<hmac_case, 5> hmacs{
    {
     {15, 200, 150, true,
         "b268aeeb18961a142921813129f58a6d76752361215c581f123709f33d82be3f1f0053577bbdfefec3d0c251"
         "92ca5854825db440cb44c247949655e7de73216d"},
     {15, 20, 3, false,
         "b71c905d2fee85fe33de568c2cfe5875df7cbb065fed7e2cde16c6c61fc7377ff4aefafe837e8b55652df500"
         "d804a3134aca60957f5467da31fe8d1ea940d7d5"},
     {12, 100, 10, false,
         "23ff73f012eee74226e37bb094311940e2f813ee9362773900dc4d0e0bf0c953e951f4df8cc6b4b3a61a45a3"
         "0a24f51b"},
     {14, 128, 40, false, "e1576f99dd659c21decc48e7c6b639c62840555356da7658512da6c560eeefe3"},
     {15, 20, 300, false,
         "c5919ea737cfbb09daaace6a54bac786c800123874531c724562d97ddd19178bbf8d420f3798bd41766b5ab1"
         "f33285e5625aef5205071ffffe55af2502bbd1fe"},
     }
};

constexpr std::string_view fips896 =
    "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopq"
    "rsmnopqrstnopqrstu";

struct context
{
  std::uint32_t              imr;
  std::uint32_t              str;
  std::uint32_t              cr;
  std::vector<std::uint32_t> csr;
};

struct rig
{
  explicit rig(bool strict = true)
  {
    auto options               = popcorn::testing::stm32_options();
    options.strict_peripherals = strict;
    f = std::make_unique<stm32_fixture>(h573(), firmware, std::move(options));
    f->set("pc", sym(firmware, "idle"));
    sink = f->target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
  }

  [[nodiscard]] auto read(std::uint32_t offset) const -> std::uint32_t
  {
    auto const value = f->target->mem().read(hash_base + offset, 4);
    REQUIRE(value.has_value());
    return *value;
  }

  void write(std::uint32_t offset, std::uint32_t value) const
  {
    REQUIRE(f->target->mem().write(hash_base + offset, 4, value).has_value());
  }

  void begin(std::uint32_t algo, std::uint32_t datatype = 2) const
  {
    write(cr, cr_init | (algo << 17) | (datatype << 4));
  }

  void feed(bytes const& message, std::uint32_t datatype = 2) const
  {
    for(std::size_t at = 0; 4 * at < message.size(); ++at)
    {
      std::uint32_t word = 0;
      for(std::size_t byte = 0; byte < 4; ++byte)
      {
        auto const index = 4 * at + byte;
        auto const value =
            index < message.size() ? message[index] : static_cast<std::uint8_t>(0x5Au ^ index);
        word = (word << 8) | value;
      }
      write(din, unswap(word, datatype));
    }
  }

  static auto unswap(std::uint32_t word, std::uint32_t datatype) -> std::uint32_t
  {
    switch(datatype)
    {
      case 1:
        return std::rotl(word, 16);
      case 2:
        return std::byteswap(word);
      case 3:
      {
        std::uint32_t out = 0;
        for(unsigned bit = 0; bit < 32; ++bit)
        {
          out = (out << 1) | ((word >> bit) & 1u);
        }
        return out;
      }
      default:
        return word;
    }
  }

  void finish(std::size_t size) const
  {
    write(str, static_cast<std::uint32_t>(8 * (size % 4)));
    write(str, str_dcal);
  }

  [[nodiscard]] auto now() const -> std::uint64_t
  {
    return f->target->virtual_clock().time().ps;
  }

  [[nodiscard]] auto cycle_ps() const -> std::uint64_t
  {
    return 1'000'000'000'000 / f->target->virtual_clock().core_hz();
  }

  void until(std::uint64_t ps) const
  {
    auto const stopped =
        f->run_to_stop({.max_instructions = icount{1'000'000'000}, .until_time = vtime{ps}});
    REQUIRE(stopped.reason == stop_reason::time);
  }

  void settle() const
  {
    until(now() + ms);
  }

  [[nodiscard]] auto words(std::size_t count) const -> bytes
  {
    bytes out;
    for(std::uint32_t index = 0; index < count / 4; ++index)
    {
      auto const word = read(hr0 + 4 * index);
      for(int shift = 24; shift >= 0; shift -= 8)
      {
        out.push_back(static_cast<std::uint8_t>(word >> shift));
      }
    }
    return out;
  }

  [[nodiscard]] auto digest(std::uint32_t algo) const -> bytes
  {
    REQUIRE((read(sr) & sr_dcis) != 0);
    auto const size = digest_bytes(algo);
    for(std::uint32_t index = size / 4; index < hr_count; ++index)
    {
      CHECK(read(hr0 + 4 * index) == 0);
    }
    for(std::uint32_t index = 0; index < 5; ++index)
    {
      CHECK(read(hra0 + 4 * index) == read(hr0 + 4 * index));
    }
    return words(size);
  }

  [[nodiscard]] auto
      hash(std::uint32_t algo, bytes const& message, std::uint32_t datatype = 2) const -> bytes
  {
    begin(algo, datatype);
    feed(message, datatype);
    finish(message.size());
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
    finish(data.size());
    settle();
  }

  [[nodiscard]] auto hmac(hmac_case const& c, std::uint32_t datatype = 2) const -> bytes
  {
    auto const key  = pattern(c.key, 1);
    auto const data = pattern(c.data, 2);
    hmac_begin(c.algo, c.long_key, datatype);
    phase(key, datatype);
    phase(data, datatype);
    phase(key, datatype);
    return digest(c.algo);
  }

  [[nodiscard]] auto save(std::size_t count) const -> context
  {
    REQUIRE((read(sr) & (sr_busy | sr_dinis)) == sr_dinis);
    context out{read(imr) & 3u, read(str) & 0x1Fu, read(cr) & cr_saved, {}};
    for(std::uint32_t index = 0; index < count; ++index)
    {
      out.csr.push_back(read(csr0 + 4 * index));
    }
    return out;
  }

  void restore(context const& saved) const
  {
    write(imr, saved.imr);
    write(str, saved.str);
    write(cr, saved.cr);
    write(cr, saved.cr | cr_init);
    for(std::uint32_t index = 0; index < saved.csr.size(); ++index)
    {
      write(csr0 + 4 * index, saved.csr[index]);
    }
  }

  std::unique_ptr<stm32_fixture> f;
  std::vector<core::log_record>  logged;
  core::hook_handle              sink;
};

auto find_hmac(std::uint32_t algo, std::size_t key, std::size_t data) -> hmac_case const&
{
  auto const found = std::ranges::find_if(hmacs, [&](hmac_case const& c) {
    return c.algo == algo and c.key == key and c.data == data;
  });
  REQUIRE(found != hmacs.end());
  return *found;
}

auto find_digest(std::uint32_t algo, std::size_t size, std::string_view message = {})
    -> digest_case const&
{
  auto const found = std::ranges::find_if(digests, [&](digest_case const& c) {
    return c.algo == algo and c.size == size and c.message == message;
  });
  REQUIRE(found != digests.end());
  return *found;
}
}

TEST_CASE("the H5 HASH has ALGO in four bits and HR0 to HR41")
{
  rig const r;
  r.write(cr, 15u << 17);
  CHECK(field(r.read(cr), 17, 4) == 15);
  CHECK(r.read(sr) == 0x0011'0001);
  CHECK(r.read(hr0 + 4 * 41) == 0);
  CHECK(r.logged.empty());
}

TEST_CASE("FIPS 180-4 and pattern vectors hash through byte-swapped DIN words")
{
  rig const r;
  for(auto const& c : digests)
  {
    CAPTURE(c.algo);
    CAPTURE(c.size);
    CHECK(r.hash(c.algo, c.input()) == unhex(c.digest));
  }
  CHECK(r.logged.empty());
}

TEST_CASE("the SHA-512 family hashes the 896-bit FIPS message")
{
  rig const r;
  for(auto const algo : {sha256, sha384, sha512_224, sha512_256, sha512})
  {
    CAPTURE(algo);
    CHECK(r.hash(algo, text(fips896)) == unhex(find_digest(algo, 112, fips896).digest));
  }
}

TEST_CASE("every DATATYPE hashes the same message to the same digest")
{
  rig const  r;
  auto const expected = r.hash(sha512, pattern(300));
  for(std::uint32_t datatype = 0; datatype < 4; ++datatype)
  {
    CAPTURE(datatype);
    CHECK(r.hash(sha512, pattern(300), datatype) == expected);
  }
}

TEST_CASE("NBWE after INIT is the block size in words plus 1 in six bits")
{
  rig const r;
  for(auto const algo : {sha1, sha224, sha256, sha384, sha512_224, sha512_256, sha512})
  {
    CAPTURE(algo);
    r.begin(algo);
    CHECK(field(r.read(sr), 16, 6) == block_bytes(algo) / 4 + 1);
  }
  r.begin(sha512);
  CHECK(field(r.read(sr), 16, 6) == 33);
}

TEST_CASE("one SHA-512 block keeps BUSY for its cycles")
{
  rig const r;
  for(auto const algo : {sha384, sha512})
  {
    CAPTURE(algo);
    r.begin(algo);
    r.feed(pattern(block_bytes(algo)));
    CHECK((r.read(sr) & (sr_dinis | sr_busy)) == 0);
    r.feed(pattern(4));
    auto const end = r.now() + cycles_of(algo) * r.cycle_ps();
    CHECK((r.read(sr) & (sr_dinis | sr_busy)) == sr_busy);
    r.until(end - r.cycle_ps());
    CHECK((r.read(sr) & (sr_dinis | sr_busy)) == sr_busy);
    r.until(end);
    CHECK((r.read(sr) & (sr_dinis | sr_busy)) == sr_dinis);
  }
}

TEST_CASE("HRA0 to HRA4 mirror HR0 to HR4 under SHA-2")
{
  rig const  r;
  auto const sha2 = r.hash(sha512, text("abc"));
  CHECK(r.read(hra0) == r.read(hr0));
  CHECK(r.read(hra0 + 16) == r.read(hr0 + 16));
  CHECK(r.digest(sha512) == sha2);
}

TEST_CASE("HMAC vectors hash through the three phases")
{
  rig const r;
  for(auto const& c : hmacs)
  {
    CAPTURE(c.algo);
    CAPTURE(c.key);
    CAPTURE(c.data);
    CHECK(r.hmac(c) == unhex(c.digest));
  }
  CHECK(r.hmac(find_hmac(sha512, 20, 3), 0) == unhex(find_hmac(sha512, 20, 3).digest));
  CHECK(r.logged.empty());
}

TEST_CASE("an HMAC key longer than the block with LKEY 0 logs once and is hashed")
{
  rig const r{false};
  auto      c = find_hmac(sha512, 200, 150);
  c.long_key  = false;
  CHECK(r.hmac(c) == unhex(c.digest));
  REQUIRE(r.logged.size() == 1);
  CHECK(r.logged[0].reg == "DIN");
  CHECK(r.logged[0].message.contains("128 bytes"));
}

TEST_CASE(
    "the reserved and SHA-3 ALGO codes are refused under strict peripherals and run as SHA2-256")
{
  {
    rig const r;
    for(auto const algo : {1u, 4u, 9u, 10u, 11u})
    {
      CAPTURE(algo);
      CHECK_THROWS_AS(
          static_cast<void>(r.f->target->mem().write(hash_base + cr, 4, cr_init | (algo << 17))),
          popcorn::model_error);
      CHECK(r.read(cr) == 0);
    }
  }
  rig const r{false};
  CHECK(r.hash(1, text("abc")) == unhex(find_digest(sha256, 3, "abc").digest));
  CHECK(r.hash(10, text("abc")) == unhex(find_digest(sha256, 3, "abc").digest));
  REQUIRE(r.logged.size() == 1);
  CHECK(r.logged[0].reg == "CR");
}

TEST_CASE("a SHA-512 hash saved at a DINIS point resumes after another hash")
{
  auto const message = pattern(300);
  for(std::size_t const words : {0, 33, 65})
  {
    for(auto const count : {91, 103})
    {
      CAPTURE(words);
      CAPTURE(count);
      rig const  r;
      auto const at = static_cast<std::ptrdiff_t>(4 * words);
      r.begin(sha512);
      r.feed(bytes(message.begin(), message.begin() + at));
      r.settle();
      auto const saved = r.save(count);
      CHECK(r.hash(sha256, text("abc")) == unhex(find_digest(sha256, 3, "abc").digest));
      r.restore(saved);
      CHECK(field(r.read(sr), 16, 6) == (words == 0 ? 33 : 32));
      r.feed(bytes(message.begin() + at, message.end()));
      r.finish(message.size());
      r.settle();
      CHECK(r.digest(sha512) == unhex(find_digest(sha512, 300).digest));
      CHECK(r.logged.empty());
    }
  }
}

TEST_CASE("a SHA-512 HMAC saved in its message phase and after it resumes after another HMAC")
{
  auto const& c = find_hmac(sha512, 20, 300);
  for(auto const after : {false, true})
  {
    CAPTURE(after);
    rig const  r;
    auto const key   = pattern(c.key, 1);
    auto const data  = pattern(c.data, 2);
    auto const split = after ? data.size() : 132;
    r.hmac_begin(c.algo, c.long_key);
    r.phase(key);
    if(after)
    {
      r.phase(data);
    }
    else
    {
      r.feed(bytes(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(split)));
      r.settle();
    }
    auto const saved = r.save(103);
    CHECK(r.hash(sha256, text("abc")) == unhex(find_digest(sha256, 3, "abc").digest));
    r.restore(saved);
    if(not after)
    {
      r.feed(bytes(data.begin() + static_cast<std::ptrdiff_t>(split), data.end()));
      r.finish(data.size());
      r.settle();
    }
    r.phase(key);
    CHECK(r.digest(c.algo) == unhex(c.digest));
    CHECK(r.logged.empty());
  }
}

TEST_CASE("a context saved under one algorithm is refused under another")
{
  rig const r{false};
  r.begin(sha512);
  r.feed(pattern(132));
  r.settle();
  auto const saved = r.save(103);
  r.begin(sha384);
  r.write(csr0, saved.csr[0]);
  r.write(csr0 + 4, saved.csr[1]);
  REQUIRE(r.logged.size() == 1);
  CHECK(r.logged[0].reg == "CSR1");
}
