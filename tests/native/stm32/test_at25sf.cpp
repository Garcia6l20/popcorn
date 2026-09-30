#include <popcorn/core/errors.hpp>
#include <popcorn/periph/devices/at25sf.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

namespace
{
namespace devices = popcorn::periph::devices;
using popcorn::reset_kind;
using popcorn::vtime;

constexpr std::uint64_t us = 1'000'000;
constexpr std::uint64_t ms = 1'000 * us;

struct host
{
  devices::at25sf flash;
  std::uint64_t   now{us};

  explicit host(devices::at25sf_options options = {}) : flash{std::move(options)}
  {}

  auto command(std::initializer_list<std::uint32_t> out, std::size_t extra = 0)
      -> std::vector<std::uint32_t>
  {
    std::vector<std::uint32_t> in;
    flash.select(true, vtime{now});
    for(auto const value : out)
    {
      now += us;
      in.push_back(flash.exchange(value, 8, vtime{now}));
    }
    for(std::size_t index = 0; index < extra; ++index)
    {
      now += us;
      in.push_back(flash.exchange(0, 8, vtime{now}));
    }
    flash.select(false, vtime{now});
    return {in.begin() + static_cast<std::ptrdiff_t>(out.size()), in.end()};
  }

  auto status() -> std::uint32_t
  {
    return command({0x05}, 1).front();
  }

  auto read(std::uint32_t address, std::size_t count) -> std::vector<std::uint32_t>
  {
    return command({0x03, address >> 16 & 0xFF, address >> 8 & 0xFF, address & 0xFF}, count);
  }

  auto at(std::uint32_t address) const -> std::uint32_t
  {
    return std::to_integer<std::uint32_t>(flash.contents()[address]);
  }

  void program(std::uint32_t address, std::vector<std::uint32_t> const& bytes)
  {
    flash.select(true, vtime{now});
    for(auto const value :
        {std::uint32_t{0x02}, address >> 16 & 0xFF, address >> 8 & 0xFF, address & 0xFF})
    {
      now += us;
      (void)flash.exchange(value, 8, vtime{now});
    }
    for(auto const value : bytes)
    {
      now += us;
      (void)flash.exchange(value, 8, vtime{now});
    }
    flash.select(false, vtime{now});
  }

  void wait(std::uint64_t ps)
  {
    now += ps;
  }
};
}

TEST_CASE("the JEDEC, manufacturer and device IDs of the AT25SF641")
{
  host h;
  CHECK(h.command({0x9F}, 3) == std::vector<std::uint32_t>{0x1F, 0x32, 0x17});
  CHECK(h.command({0x90, 0, 0, 0}, 4) == std::vector<std::uint32_t>{0x1F, 0x16, 0x1F, 0x16});
  CHECK(h.command({0x90, 0, 0, 1}, 2) == std::vector<std::uint32_t>{0x16, 0x1F});
  CHECK(h.command({0xAB, 0, 0, 0}, 2) == std::vector<std::uint32_t>{0x16, 0x16});
  devices::at25sf_options other{
      .part = {0xC2, 0x20, 0x16, 0x15, 4u << 20}
  };
  host h2{other};
  CHECK(h2.command({0x9F}, 3) == std::vector<std::uint32_t>{0xC2, 0x20, 0x16});
  CHECK(h2.flash.contents().size() == 4u << 20);
}

TEST_CASE("write enable and disable drive WEL")
{
  host h;
  CHECK(h.status() == 0);
  h.command({0x06});
  CHECK(h.status() == 0x02);
  h.command({0x04});
  CHECK(h.status() == 0);
}

TEST_CASE("a page program without WEL is ignored")
{
  host h;
  h.program(0x1000, {0x12, 0x34});
  CHECK(h.at(0x1000) == 0xFF);
  CHECK(h.status() == 0);
}

TEST_CASE("a page program wraps within its 256-byte page and ANDs into the array")
{
  host                       h;
  std::vector<std::uint32_t> bytes;
  for(std::uint32_t index = 0; index < 32; ++index)
  {
    bytes.push_back(index);
  }
  h.command({0x06});
  h.program(0x0010F0, bytes);
  h.wait(ms);
  CHECK(h.at(0x0010F0) == 0);
  CHECK(h.at(0x0010FF) == 15);
  CHECK(h.at(0x001000) == 16);
  CHECK(h.at(0x00100F) == 31);
  CHECK(h.at(0x001010) == 0xFF);
  CHECK(h.at(0x001100) == 0xFF);
  h.command({0x06});
  h.program(0x001001, {0xF0});
  h.wait(ms);
  CHECK(h.at(0x001001) == 0x10);
  CHECK(h.read(0x0010FE, 4) == std::vector<std::uint32_t>{14, 15, 0xFF, 0xFF});
}

TEST_CASE("BUSY lasts exactly the page program time, other instructions ignored meanwhile")
{
  host h;
  h.command({0x06});
  h.program(0x2000, {0x00});
  auto const end = h.now;
  CHECK(h.status() == 0x01);
  CHECK(h.read(0x2000, 1) == std::vector<std::uint32_t>{0xFF});
  h.command({0x06});
  h.now = end + 600 * us - 1;
  h.flash.select(true, vtime{h.now});
  (void)h.flash.exchange(0x05, 8, vtime{h.now});
  CHECK(h.flash.exchange(0, 8, vtime{h.now}) == 0x01);
  CHECK(h.flash.exchange(0, 8, vtime{end + 600 * us}) == 0x00);
  h.flash.select(false, vtime{end + 600 * us});
  h.now = end + 600 * us;
  CHECK(h.read(0x2000, 1) == std::vector<std::uint32_t>{0x00});
}

TEST_CASE("sector, block and chip erases fill 0xFF for their time")
{
  host h;
  h.command({0x06});
  h.command({0x60});
  h.wait(31'000 * ms);
  auto erase = [&](std::uint32_t op, std::uint32_t address, std::uint64_t time) {
    h.command({0x06});
    if(op == 0xC7)
    {
      h.command({op});
    }
    else
    {
      h.command({op, address >> 16 & 0xFF, address >> 8 & 0xFF, address & 0xFF});
    }
    auto const end = h.now;
    h.now          = end + time - 2 * us - 1;
    CHECK(h.status() == 0x01);
    h.now = end + time - 2 * us;
    CHECK(h.status() == 0x00);
  };
  for(std::uint32_t address : {0x00F000u, 0x010000u, 0x017FFFu, 0x020000u, 0x02FFFFu, 0x030000u})
  {
    h.command({0x06});
    h.program(address, {0x00});
    h.wait(ms);
  }
  erase(0x20, 0x00F123, 60 * ms);
  CHECK(h.at(0x00F000) == 0xFF);
  CHECK(h.at(0x010000) == 0x00);
  erase(0x52, 0x012345, 200 * ms);
  CHECK(h.at(0x010000) == 0xFF);
  CHECK(h.at(0x017FFF) == 0xFF);
  CHECK(h.at(0x020000) == 0x00);
  erase(0xD8, 0x02ABCD, 300 * ms);
  CHECK(h.at(0x020000) == 0xFF);
  CHECK(h.at(0x02FFFF) == 0xFF);
  CHECK(h.at(0x030000) == 0x00);
  erase(0xC7, 0, 30'000 * ms);
  CHECK(h.at(0x030000) == 0xFF);
}

TEST_CASE("an erase not ending at the address byte boundary does not run")
{
  host h;
  h.command({0x06});
  h.program(0x4000, {0x00});
  h.wait(ms);
  h.command({0x06});
  h.command({0x20, 0x00, 0x40, 0x00, 0x00});
  CHECK(h.status() == 0x02);
  CHECK(h.at(0x4000) == 0x00);
}

TEST_CASE("deep power down ignores everything but release, which takes tRES1")
{
  host h;
  h.command({0xB9});
  CHECK(h.command({0x9F}, 3) == std::vector<std::uint32_t>{0x1F, 0x32, 0x17});
  h.command({0xB9});
  h.wait(3 * us);
  CHECK(h.command({0x9F}, 3) == std::vector<std::uint32_t>{0xFF, 0xFF, 0xFF});
  CHECK(h.status() == 0xFF);
  h.command({0xAB});
  h.wait(us);
  CHECK(h.command({0x9F}, 3) == std::vector<std::uint32_t>{0xFF, 0xFF, 0xFF});
  h.wait(3 * us);
  CHECK(h.command({0x9F}, 3) == std::vector<std::uint32_t>{0x1F, 0x32, 0x17});
}

TEST_CASE("fast read skips a dummy byte and reads wrap at the end of the array")
{
  host h;
  h.command({0x06});
  h.program(0, {0x55});
  h.wait(ms);
  CHECK(h.command({0x0B, 0x7F, 0xFF, 0xFF, 0x00}, 2) == std::vector<std::uint32_t>{0xFF, 0x55});
  CHECK(h.read(0x7FFFFF, 2) == std::vector<std::uint32_t>{0xFF, 0x55});
}

TEST_CASE("16-bit frames carry two bytes, a 12-bit frame ends the instruction")
{
  host h;
  h.flash.select(true, vtime{h.now});
  CHECK(h.flash.exchange(0x9F00, 16, vtime{h.now}) == 0xFF1F);
  CHECK(h.flash.exchange(0x0000, 16, vtime{h.now}) == 0x3217);
  h.flash.select(false, vtime{h.now});
  h.flash.select(true, vtime{h.now});
  CHECK(h.flash.exchange(0x06, 8, vtime{h.now}) == 0xFF);
  CHECK(h.flash.exchange(0, 12, vtime{h.now}) == 0xFFF);
  h.flash.select(false, vtime{h.now});
  CHECK(h.status() == 0);
}

TEST_CASE("status writes need WEL or 50h, and a reset needs 66h right before 99h")
{
  host h;
  h.command({0x01, 0x1C, 0x02});
  CHECK(h.status() == 0);
  h.command({0x06});
  h.command({0x01, 0x1C, 0x02});
  CHECK(h.status() == 0x1D);
  h.wait(5 * ms);
  CHECK(h.status() == 0x1C);
  CHECK(h.command({0x35}, 1) == std::vector<std::uint32_t>{0x02});
  h.command({0x50});
  h.command({0x31, 0x00});
  CHECK(h.command({0x35}, 1) == std::vector<std::uint32_t>{0x00});
  CHECK(h.status() == 0x1C);

  h.command({0x06});
  h.command({0x99});
  CHECK(h.status() == 0x1E);
  h.command({0x66});
  h.command({0x05}, 1);
  h.command({0x99});
  CHECK(h.status() == 0x1E);
  h.command({0x66});
  h.command({0x99});
  CHECK(h.command({0x05}, 1) == std::vector<std::uint32_t>{0xFF});
  h.wait(30 * us);
  CHECK(h.status() == 0x1C);
  h.command({0x06});
  h.command({0x01, 0xFF, 0xFF});
  h.wait(5 * ms);
  CHECK(h.status() == 0xFC);
  CHECK(h.command({0x35}, 1) == std::vector<std::uint32_t>{0x43});
  h.command({0x50});
  h.command({0x01, 0xFF});
  CHECK(h.status() == 0xFC);
}

TEST_CASE("save keeps only the changed pages and load restores them mid-instruction")
{
  host h;
  h.command({0x06});
  h.program(0x123400, {0xA5, 0x5A});
  h.wait(ms);
  h.flash.select(true, vtime{h.now});
  for(std::uint32_t value : {0x03u, 0x12u, 0x34u, 0x00u})
  {
    (void)h.flash.exchange(value, 8, vtime{h.now});
  }
  std::vector<std::byte> saved;
  h.flash.save(saved);
  CHECK(saved.size() < 2'048);
  auto const first = h.flash.exchange(0, 8, vtime{h.now});

  host other;
  other.flash.load(saved);
  other.now = h.now;
  CHECK(other.flash.exchange(0, 8, vtime{other.now}) == first);
  CHECK(first == 0xA5);
  CHECK(other.flash.exchange(0, 8, vtime{other.now}) == 0x5A);
  CHECK(std::ranges::equal(other.flash.contents(), h.flash.contents()));
  other.flash.select(false, vtime{other.now});
  CHECK(other.status() == 0x00);
}

TEST_CASE("an image file gives the initial contents, and load restores relative to it")
{
  auto const path = std::filesystem::current_path() / "test_at25sf_image.bin";
  {
    std::ofstream file{path, std::ios::binary};
    file.put(0x12).put(0x34);
  }
  host h{{.image = path}};
  CHECK(h.at(0) == 0x12);
  CHECK(h.at(1) == 0x34);
  CHECK(h.at(2) == 0xFF);
  std::vector<std::byte> saved;
  h.flash.save(saved);
  host other{{.image = path}};
  other.flash.load(saved);
  CHECK(other.at(0) == 0x12);

  devices::at25sf_options small{
      .part = {0x1F, 0x32, 0x17, 0x16, 1},
        .image = path
  };
  CHECK_THROWS_AS(devices::at25sf{small}, popcorn::config_error);
  CHECK_THROWS_AS(
      devices::at25sf{{.image = std::filesystem::current_path() / "missing.bin"}},
      popcorn::config_error);
  std::filesystem::remove(path);
}

TEST_CASE("initial bytes give the initial contents, the rest erased")
{
  host h{{.initial = {std::byte{0x12}, std::byte{0x34}}}};
  CHECK(h.at(0) == 0x12);
  CHECK(h.at(1) == 0x34);
  CHECK(h.at(2) == 0xFF);
  CHECK(h.flash.contents().size() == 8u << 20);

  devices::at25sf_options full;
  full.part.size = 64u << 10;
  full.initial.assign(64u << 10, std::byte{0x5A});
  devices::at25sf const exact{full};
  CHECK(std::ranges::all_of(exact.contents(), [](std::byte b) { return b == std::byte{0x5A}; }));
}

TEST_CASE("initial bytes are refused with an image or when larger than the part")
{
  auto const refused_key = [](devices::at25sf_options const& options) {
    try
    {
      devices::at25sf const flash{options};
    }
    catch(popcorn::config_error const& error)
    {
      return std::string{error.key()};
    }
    return std::string{};
  };
  auto const path = std::filesystem::current_path() / "test_at25sf_initial.bin";
  {
    std::ofstream file{path, std::ios::binary};
    file.put(0x12);
  }
  CHECK(refused_key({.image = path, .initial = {std::byte{0x12}}}) == "initial");
  std::filesystem::remove(path);

  devices::at25sf_options longer;
  longer.part.size = 64u << 10;
  longer.initial.assign((64u << 10) + 1, std::byte{0x00});
  CHECK(refused_key(longer) == "initial");
  longer.initial.pop_back();
  CHECK(refused_key(longer).empty());
}

TEST_CASE("load checks the initial array, whichever of image or initial bytes gave it")
{
  host                   h{{.initial = {std::byte{0x12}, std::byte{0x34}}}};
  std::vector<std::byte> saved;
  h.flash.save(saved);

  devices::at25sf same{{.initial = {std::byte{0x12}, std::byte{0x34}}}};
  CHECK_NOTHROW(same.load(saved));
  devices::at25sf other{{.initial = {std::byte{0x12}, std::byte{0x35}}}};
  CHECK_THROWS_AS(other.load(saved), popcorn::config_error);
  devices::at25sf erased;
  CHECK_THROWS_AS(erased.load(saved), popcorn::config_error);

  auto const path = std::filesystem::current_path() / "test_at25sf_same.bin";
  {
    std::ofstream file{path, std::ios::binary};
    file.put(0x12).put(0x34);
  }
  devices::at25sf from_file{{.image = path}};
  CHECK_NOTHROW(from_file.load(saved));
  std::filesystem::remove(path);
}

TEST_CASE("a reset while BUSY ends the program, keeps its data and takes tRST")
{
  host h;
  h.command({0x06});
  h.program(0x3000, {0x00});
  CHECK(h.status() == 0x01);
  h.command({0x66});
  h.command({0x99});
  CHECK(h.command({0x05}, 1) == std::vector<std::uint32_t>{0xFF});
  h.wait(30 * us);
  CHECK(h.status() == 0x00);
  CHECK(h.at(0x3000) == 0x00);
}

TEST_CASE("a volatile status write changes the status in use until a reset restores it")
{
  host h;
  h.command({0x06});
  h.command({0x01, 0x1C, 0x02});
  h.wait(5 * ms);
  h.command({0x50});
  h.command({0x01, 0x00, 0x00});
  CHECK(h.status() == 0x00);
  CHECK(h.command({0x35}, 1) == std::vector<std::uint32_t>{0x00});
  h.command({0x66});
  h.command({0x99});
  h.wait(30 * us);
  CHECK(h.status() == 0x1C);
  CHECK(h.command({0x35}, 1) == std::vector<std::uint32_t>{0x02});
}

TEST_CASE("load refuses a snapshot of another part size or initial image")
{
  host                   h;
  std::vector<std::byte> saved;
  h.flash.save(saved);
  auto const path = std::filesystem::current_path() / "test_at25sf_other.bin";
  {
    std::ofstream file{path, std::ios::binary};
    file.put(0x00);
  }
  devices::at25sf other{{.image = path}};
  CHECK_THROWS_AS(other.load(saved), popcorn::config_error);
  devices::at25sf_options small;
  small.part.size = 4u << 20;
  devices::at25sf smaller{small};
  CHECK_THROWS_AS(smaller.load(saved), popcorn::config_error);
  devices::at25sf same;
  CHECK_NOTHROW(same.load(saved));
  std::filesystem::remove(path);
}

TEST_CASE("a part size of 0 or not a multiple of 64 KB is refused")
{
  auto const make = [](std::uint32_t size) {
    devices::at25sf_options options;
    options.part.size = size;
    devices::at25sf const flash{options};
    return flash.contents().size();
  };
  for(std::uint32_t const size : {0u, 100'000u, (64u << 10) + 256u})
  {
    CAPTURE(size);
    CHECK_THROWS_AS(make(size), popcorn::config_error);
  }
  CHECK(make(64u << 10) == 64u << 10);
}

TEST_CASE("a 4 KB erase at an unaligned address erases exactly its sector")
{
  host h;
  for(std::uint32_t const address : {0x000FFFu, 0x001000u, 0x001FFFu, 0x002000u})
  {
    h.command({0x06});
    h.program(address, {0x00});
    h.wait(ms);
  }
  h.command({0x06});
  h.command({0x20, 0x00, 0x12, 0x34});
  h.wait(60 * ms);
  CHECK(h.at(0x000FFF) == 0x00);
  CHECK(h.at(0x002000) == 0x00);
  auto const sector = h.flash.contents().subspan(0x1000, 0x1000);
  CHECK(std::ranges::all_of(sector, [](std::byte value) { return value == std::byte{0xFF}; }));
}

TEST_CASE("a power-on reset ends BUSY and clears WEL, the new timeline starting at 0")
{
  host h;
  h.now = 50 * ms;
  h.command({0x06});
  h.program(0x2000, {0x00});
  CHECK(h.status() == 0x01);
  h.command({0x06});
  CHECK(h.status() == 0x01);
  h.flash.reset(reset_kind::power_on);
  h.now = us;
  CHECK(h.status() == 0x00);
  CHECK(h.at(0x2000) == 0x00);
  h.command({0x06});
  CHECK(h.status() == 0x02);
  h.flash.reset(reset_kind::brown_out);
  CHECK(h.status() == 0x00);
}

TEST_CASE("a reset of another kind leaves the device as it was")
{
  for(auto const kind : {reset_kind::pin, reset_kind::software, reset_kind::watchdog})
  {
    CAPTURE(kind);
    host h;
    h.command({0x06});
    h.flash.reset(kind);
    CHECK(h.status() == 0x02);
    h.program(0x2000, {0x00});
    h.flash.reset(kind);
    CHECK(h.status() == 0x01);
    h.command({0xB9});
    h.wait(3 * us);
    h.flash.reset(kind);
    CHECK(h.command({0x9F}, 3) == std::vector<std::uint32_t>{0xFF, 0xFF, 0xFF});
  }
}

TEST_CASE("a power-on reset ends the tRST wait of a 66h 99h reset")
{
  host h;
  h.now = 50 * ms;
  h.command({0x66});
  h.command({0x99});
  CHECK(h.command({0x05}, 1) == std::vector<std::uint32_t>{0xFF});
  h.flash.reset(reset_kind::power_on);
  h.now = us;
  CHECK(h.status() == 0x00);
}

TEST_CASE("a power-on reset leaves deep power down, entered or still entering")
{
  host asleep;
  asleep.now = 50 * ms;
  asleep.command({0xB9});
  asleep.wait(3 * us);
  CHECK(asleep.command({0x9F}, 3) == std::vector<std::uint32_t>{0xFF, 0xFF, 0xFF});
  asleep.flash.reset(reset_kind::power_on);
  asleep.now = us;
  CHECK(asleep.command({0x9F}, 3) == std::vector<std::uint32_t>{0x1F, 0x32, 0x17});

  host entering;
  entering.now = 50 * ms;
  entering.command({0xB9});
  entering.flash.reset(reset_kind::power_on);
  entering.now = us;
  entering.wait(10 * us);
  CHECK(entering.command({0x9F}, 3) == std::vector<std::uint32_t>{0x1F, 0x32, 0x17});
  entering.now = 60 * ms;
  CHECK(entering.command({0x9F}, 3) == std::vector<std::uint32_t>{0x1F, 0x32, 0x17});
}

TEST_CASE("a power-on reset reloads the stored status and drops the instruction in progress")
{
  host h;
  h.command({0x06});
  h.command({0x01, 0x1C, 0x02});
  h.wait(5 * ms);
  h.command({0x50});
  h.command({0x01, 0x00, 0x00});
  CHECK(h.status() == 0x00);
  h.flash.select(true, vtime{h.now += us});
  (void)h.flash.exchange(0x9F, 8, vtime{h.now += us});
  h.flash.reset(reset_kind::power_on);
  h.now = us;
  CHECK(h.flash.exchange(0, 8, vtime{h.now += us}) == 0xFF);
  h.flash.select(false, vtime{h.now += us});
  CHECK(h.status() == 0x1C);
  CHECK(h.command({0x35}, 1) == std::vector<std::uint32_t>{0x02});
  h.command({0x01, 0x00, 0x00});
  CHECK(h.status() == 0x1C);
  h.command({0x50});
  h.command({0x01, 0x00, 0x00});
  h.command({0x66});
  h.command({0x99});
  h.wait(30 * us);
  CHECK(h.status() == 0x1C);
}
