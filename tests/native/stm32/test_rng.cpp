#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/rng.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::periph::stm32::rng_word_at;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;
using popcorn::testing::u575;

constexpr std::uint32_t rng_base   = 0x420C'0800;
constexpr std::uint32_t sec_base   = 0x520C'0800;
constexpr std::uint32_t cr         = 0x00;
constexpr std::uint32_t sr         = 0x04;
constexpr std::uint32_t dr         = 0x08;
constexpr std::uint32_t nscr       = 0x0C;
constexpr std::uint32_t htcr       = 0x10;
constexpr std::uint32_t cr_reset   = 0x0080'0D00;
constexpr std::uint32_t cr_nist    = 0x00F0'0D00;
constexpr std::uint32_t configlock = 1u << 31;
constexpr std::uint32_t condrst    = 1u << 30;
constexpr std::uint32_t ced        = 1u << 5;
constexpr std::uint32_t ie         = 1u << 3;
constexpr std::uint32_t rngen      = 1u << 2;
constexpr std::uint32_t drdy       = 1u << 0;
constexpr std::uint32_t nist_htcr  = 0xA2B0;
constexpr std::uint32_t nist_nscr  = 0x1'7CBB;

constexpr auto clkdiv(std::uint32_t value) -> std::uint32_t
{
  return value << 16;
}

constexpr auto config1(std::uint32_t value) -> std::uint32_t
{
  return value << 20;
}

constexpr std::uint64_t us               = 1'000'000;
constexpr std::uint64_t cycle            = 250'000;
constexpr std::uint64_t init_4mhz        = 964'250'000;
constexpr std::uint64_t round_4mhz       = 101'500'000;
constexpr std::uint64_t clear_4mhz       = 541'667;
constexpr std::uint64_t init_160mhz      = 24'106'250;
constexpr std::uint64_t round_160mhz     = 5'333'334;
constexpr std::uint64_t init_slow_custom = 525'497'917;
constexpr std::uint64_t init_slow_reset  = 521'831'250;

constexpr std::array<std::uint32_t, 4> seed_0_words{
    0xE220'A839, 0x6E78'9E6A, 0x06C4'5D18, 0xF88B'B8A8};

auto words_from(std::uint64_t start, std::uint64_t count) -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(auto position = start; position < start + count; ++position)
  {
    out.push_back(rng_word_at(0, position));
  }
  return out;
}

auto entered(std::string_view entry) -> stm32_fixture
{
  auto options               = popcorn::testing::stm32_options();
  options.strict_peripherals = true;
  stm32_fixture f{u575(), "rng_ll", std::move(options)};
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{100'000}, .until = sym("rng_ll", "main")}).reason
      == stop_reason::address);
  f.set("pc", sym("rng_ll", entry));
  return f;
}

struct rig
{
  explicit rig(std::string_view entry = "idle") : f{entered(entry)}
  {}

  [[nodiscard]] auto at(std::string_view peripheral, std::string_view reg) const -> address
  {
    auto const& desc  = f.target->peripheral(peripheral).description();
    auto const  found = popcorn::find_register(desc, reg);
    REQUIRE(found != nullptr);
    return desc.base + found->offset;
  }

  [[nodiscard]] auto read(std::uint32_t offset, std::uint32_t base = rng_base) const
      -> std::uint32_t
  {
    auto const value = f.target->mem().read(base + offset, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void write(std::uint32_t offset, std::uint32_t value, std::uint32_t base = rng_base) const
  {
    REQUIRE(f.target->mem().write(base + offset, 4, value).has_value());
  }

  void set_field(
      std::string_view peripheral,
      std::string_view reg,
      std::string_view field,
      std::uint32_t    value) const
  {
    auto const& desc  = f.target->peripheral(peripheral).description();
    auto const  found = popcorn::find_register(desc, reg);
    REQUIRE(found != nullptr);
    auto const bits = std::ranges::find(found->fields, field, &popcorn::field_desc::name);
    REQUIRE(bits != found->fields.end());
    auto const mask = ((std::uint32_t{1} << bits->width) - 1u) << bits->lsb;
    auto const old  = f.target->mem().read(desc.base + found->offset, 4);
    REQUIRE(old.has_value());
    auto const word = (static_cast<std::uint32_t>(*old) & ~mask) | ((value << bits->lsb) & mask);
    REQUIRE(f.target->mem().write(desc.base + found->offset, 4, word).has_value());
  }

  [[nodiscard]] auto cpu_dr() const -> std::uint32_t
  {
    auto* const model = f.target->model("RNG");
    REQUIRE(model != nullptr);
    auto const value = model->on_read(
        f.target->peripheral("RNG"), dr, 4,
        {.pc = 0, .now = icount{0}, .by = popcorn::initiator::cpu});
    REQUIRE(value.has_value());
    return *value;
  }

  [[nodiscard]] auto now() const -> std::uint64_t
  {
    return f.target->virtual_clock().time().ps;
  }

  void run_until(std::uint64_t ps) const
  {
    REQUIRE(ps >= now());
    if(ps == now())
    {
      return;
    }
    auto const stopped = f.run(
        {.max_instructions = icount{10'000'000},
         .wall_budget      = std::chrono::seconds{60},
         .until_time       = vtime{ps}});
    REQUIRE(stopped.stopped.has_value());
    REQUIRE(stopped.stopped->reason == stop_reason::time);
    REQUIRE(now() < ps + cycle);
  }

  void run_for(std::uint64_t ps) const
  {
    run_until(now() + ps);
  }

  [[nodiscard]] auto pending() const -> std::vector<std::uint64_t>
  {
    std::vector<std::uint64_t> out;
    for(auto const& event : f.target->snapshot().data().events.pending)
    {
      out.push_back(event.due_ps);
    }
    return out;
  }

  [[nodiscard]] auto due(std::uint64_t ps) const -> bool
  {
    return std::ranges::contains(pending(), ps);
  }

  [[nodiscard]] auto line() const -> std::uint8_t
  {
    auto const  saved  = f.target->snapshot();
    auto const& images = saved.data().peripherals;
    auto const  image =
        std::ranges::find(images, std::string{"RNG"}, &core::peripheral_image::peripheral);
    REQUIRE(image != images.end());
    REQUIRE(image->irq_levels.size() == 1);
    return image->irq_levels[0];
  }

  auto hsi48(bool on) const -> std::uint64_t
  {
    auto const start = now();
    set_field("RCC", "RCC_CR", "HSI48ON", on ? 1 : 0);
    if(on)
    {
      run_for(100 * us);
      REQUIRE(f.field("RCC", "RCC_CR", "HSI48RDY") == 1);
    }
    return start + 100 * us;
  }

  void clocks() const
  {
    static_cast<void>(hsi48(true));
    set_field("RCC", "RCC_AHB2ENR1", "RNGEN", 1);
  }

  [[nodiscard]] auto ready() const -> bool
  {
    return (read(sr) & drdy) != 0;
  }

  [[nodiscard]] auto log() const -> std::vector<std::uint32_t>
  {
    return f.log();
  }

  auto finish(std::uint64_t instructions) const -> popcorn::stop
  {
    return f.run_to_stop({.max_instructions = icount{instructions}});
  }

  stm32_fixture                 f;
  std::vector<core::log_record> logged;
  core::hook_handle             sink =
      f.target->add_log_sink([this](core::log_record const& record) { logged.push_back(record); });
};

auto enabled_at_4mhz(rig const& r) -> std::uint64_t
{
  r.clocks();
  r.write(cr, cr_reset | rngen);
  return r.now();
}

auto full_pipeline(rig const& r) -> std::uint64_t
{
  auto const start = enabled_at_4mhz(r);
  r.run_until(start + init_4mhz + round_4mhz);
  return start + init_4mhz + round_4mhz;
}
}

TEST_CASE("RNG registers read their RM0456 reset values through both aliases")
{
  rig const r;
  REQUIRE(r.f.target->model("RNG") != nullptr);
  for(auto const base : {rng_base, sec_base})
  {
    CHECK(r.read(cr, base) == cr_reset);
    CHECK(r.read(sr, base) == 0);
    CHECK(r.read(dr, base) == 0);
    CHECK(r.read(nscr, base) == 0x0003'FFFF);
    CHECK(r.read(htcr, base) == 0x0000'72AC);
  }
  CHECK(r.logged.empty());
}

TEST_CASE("a byte or half-word RNG access is a bus error and a write changes nothing")
{
  rig const r;
  auto      mem = r.f.target->mem();
  for(auto const base : {rng_base, sec_base})
  {
    for(auto const offset : {cr, sr, dr, nscr, htcr})
    {
      CAPTURE(base + offset);
      CHECK_FALSE(mem.read(base + offset, 1).has_value());
      CHECK_FALSE(mem.read(base + offset + 2, 2).has_value());
      CHECK_FALSE(mem.write(base + offset, 1, 0xFF).has_value());
      CHECK_FALSE(mem.write(base + offset + 2, 2, 0xFFFF).has_value());
    }
    CHECK_FALSE(mem.write(base + cr, 2, condrst >> 16).has_value());
  }
  CHECK(r.read(cr) == cr_reset);
  CHECK(r.read(nscr) == 0x0003'FFFF);
  CHECK(r.read(htcr) == 0x0000'72AC);
  CHECK(r.logged.empty());
}

TEST_CASE("CR configuration bits are taken only with CONDRST in the same write")
{
  rig const r;
  r.write(cr, cr_nist);
  CHECK(r.read(cr) == cr_reset);
  r.write(cr, cr_nist | condrst | ced | clkdiv(2));
  CHECK(r.read(cr) == (cr_nist | condrst | ced | clkdiv(2)));
  r.write(cr, cr_nist | condrst | rngen);
  CHECK(r.read(cr) == (cr_nist | condrst | rngen));
  r.write(cr, config1(0x10) | 0xD00 | condrst | rngen | ced | clkdiv(3));
  CHECK(r.read(cr) == (config1(0x10) | 0xD00 | condrst | rngen));
  r.write(cr, config1(0x10) | 0xD00 | condrst);
  r.write(cr, config1(0x10) | 0xD00 | condrst | ced | clkdiv(3));
  CHECK(r.read(cr) == (config1(0x10) | 0xD00 | condrst | ced | clkdiv(3)));
  CHECK(r.logged.empty());
}

TEST_CASE("CONFIGLOCK is set-once and freezes CR bits 29:4, NSCR and HTCR")
{
  rig const r;
  r.write(cr, cr_reset | configlock);
  r.write(cr, 0);
  CHECK(r.read(cr) == (cr_reset | configlock));
  r.write(cr, cr_nist | configlock | condrst);
  CHECK(r.read(cr) == (cr_reset | configlock | condrst));
  r.write(nscr, nist_nscr);
  r.write(htcr, nist_htcr);
  CHECK(r.read(nscr) == 0x0003'FFFF);
  CHECK(r.read(htcr) == 0x0000'72AC);

  rig const fw{"locked"};
  REQUIRE(fw.finish(100'000).reason == stop_reason::user);
  CHECK(fw.log() == std::vector<std::uint32_t>{cr_reset | configlock | condrst, 0x72AC, 0x3'FFFF});
  CHECK(r.logged.empty());
  CHECK(fw.logged.empty());
}

TEST_CASE("NSCR and HTCR writes are taken only while CONDRST reads 1")
{
  rig const r;
  r.write(nscr, nist_nscr);
  r.write(htcr, nist_htcr);
  CHECK(r.read(nscr) == 0x0003'FFFF);
  CHECK(r.read(htcr) == 0x0000'72AC);
  r.write(cr, cr_reset | condrst);
  r.write(nscr, nist_nscr);
  r.write(htcr, nist_htcr, sec_base);
  CHECK(r.read(nscr) == nist_nscr);
  CHECK(r.read(htcr) == nist_htcr);

  rig const fw{"nist_init"};
  REQUIRE(fw.finish(100'000).reason == stop_reason::user);
  auto const log = fw.log();
  REQUIRE(log.size() == 6);
  CHECK(log[0] == cr_nist);
  CHECK(log[1] == nist_htcr);
  CHECK(log[2] == nist_nscr);
  CHECK(log[3] == 2);
  CHECK(log[4] < 2000);
  CHECK(log[5] == drdy);
  CHECK(r.logged.empty());
  CHECK(fw.logged.empty());
}

TEST_CASE("CONDRST written back to 0 reads 1 for 2 AHB plus 2 RNG cycles")
{
  SUBCASE("kernel clock running")
  {
    rig const r;
    r.clocks();
    r.write(cr, cr_reset | condrst);
    r.write(cr, cr_reset);
    auto const done = r.now() + clear_4mhz;
    CHECK(r.read(cr) == (cr_reset | condrst));
    CHECK(r.due(done));
    r.write(cr, cr_reset);
    CHECK(r.due(done));
    r.run_until(done - cycle);
    CHECK(r.read(cr) == (cr_reset | condrst));
    r.run_until(done);
    CHECK(r.read(cr) == cr_reset);
    CHECK(r.logged.empty());
  }
  SUBCASE("kernel clock stopped, then started")
  {
    rig const r;
    r.set_field("RCC", "RCC_AHB2ENR1", "RNGEN", 1);
    r.write(cr, cr_reset | condrst);
    r.write(cr, cr_reset);
    auto const before = r.pending();
    r.run_for(1000 * us);
    CHECK(r.read(cr) == (cr_reset | condrst));
    auto const ready = r.hsi48(true);
    auto const done  = ready + clear_4mhz;
    CHECK(r.read(cr) == (cr_reset | condrst));
    CHECK(r.due(done));
    CHECK_FALSE(std::ranges::contains(before, done));
    r.run_until(done);
    CHECK(r.read(cr) == cr_reset);
    CHECK(r.logged.empty());
  }
}

TEST_CASE("no data without the AHB enable, the kernel clock source or RNGEN")
{
  SUBCASE("AHB enable off")
  {
    rig const r;
    static_cast<void>(r.hsi48(true));
    r.write(cr, cr_reset | rngen);
    r.run_for(2 * init_4mhz);
    CHECK_FALSE(r.ready());
    r.set_field("RCC", "RCC_AHB2ENR1", "RNGEN", 1);
    auto const start = r.now();
    CHECK(r.due(start + init_4mhz));
    CHECK(r.logged.empty());
  }
  SUBCASE("HSI48 off with RNGSEL HSI48")
  {
    rig const r;
    r.set_field("RCC", "RCC_AHB2ENR1", "RNGEN", 1);
    r.write(cr, cr_reset | rngen);
    r.run_for(2 * init_4mhz);
    CHECK_FALSE(r.ready());
    CHECK(r.logged.empty());
  }
  SUBCASE("CR.RNGEN 0")
  {
    rig const r;
    r.clocks();
    r.run_for(2 * init_4mhz);
    CHECK_FALSE(r.ready());
    CHECK(r.logged.empty());
  }
  SUBCASE("RNGSEL reserved")
  {
    rig const r;
    r.clocks();
    r.set_field("RCC", "RCC_CCIPR2", "RNGSEL", 3);
    r.write(cr, cr_reset | rngen);
    r.run_for(2 * init_4mhz);
    CHECK_FALSE(r.ready());
    CHECK(r.logged.empty());
  }
  SUBCASE("firmware without HSI48")
  {
    rig const r{"no_kernel_clock"};
    REQUIRE(r.finish(100'000).reason == stop_reason::user);
    CHECK(r.log() == std::vector<std::uint32_t>{2000, 0, 0});
    CHECK(r.logged.empty());
  }
  SUBCASE("firmware on HSI16")
  {
    rig const r{"hsi16_source"};
    REQUIRE(r.finish(200'000).reason == stop_reason::user);
    auto const log = r.log();
    REQUIRE(log.size() == 49);
    for(std::size_t word = 0; word < 16; ++word)
    {
      CAPTURE(word);
      CHECK(log[3 * word] == drdy);
      CHECK(log[3 * word + 1] < 2000);
      CHECK(log[3 * word + 2] == rng_word_at(0, word));
    }
    CHECK(log[48] == 0);
    CHECK(r.logged.empty());
  }
}

TEST_CASE("init ends after the Table 463 time from the clocks at its start")
{
  SUBCASE("HCLK 4 MHz, HSI48")
  {
    rig const  r;
    auto const start = enabled_at_4mhz(r);
    CHECK(r.due(start + init_4mhz));
    r.run_until(start + init_4mhz - cycle);
    CHECK_FALSE(r.ready());
    r.run_until(start + init_4mhz);
    CHECK(r.ready());
    CHECK(r.logged.empty());
  }
  SUBCASE("HCLK 160 MHz, HSI48")
  {
    rig const r{"pll_160"};
    REQUIRE(r.finish(100'000).reason == stop_reason::user);
    r.f.set("pc", sym("rng_ll", "idle"));
    r.write(cr, cr_reset | rngen);
    auto const start = r.now();
    CHECK(r.due(start + init_160mhz));
    r.run_until(start + init_160mhz - cycle);
    CHECK_FALSE(r.ready());
    r.run_until(start + init_160mhz);
    CHECK(r.ready());
    CHECK(r.due(start + init_160mhz + round_160mhz));
    CHECK(r.logged.empty());
  }
  for(auto const& [config, expected] : {
          std::pair{cr_nist,  init_slow_custom},
          std::pair{cr_reset, init_slow_reset }
  })
  {
    CAPTURE(config);
    rig const r{"pll_160"};
    REQUIRE(r.finish(100'000).reason == stop_reason::user);
    r.f.set("pc", sym("rng_ll", "idle"));
    r.write(cr, config | clkdiv(15) | condrst);
    r.write(cr, config | clkdiv(15));
    r.run_for(us);
    REQUIRE(r.read(cr) == (config | clkdiv(15)));
    r.write(cr, config | clkdiv(15) | rngen);
    auto const start = r.now();
    CHECK(r.due(start + expected));
    CHECK(r.logged.empty());
  }
}

TEST_CASE("the FIFO serves four words, DRDY stays 1 until it is empty")
{
  rig const  r;
  auto const start = enabled_at_4mhz(r);
  auto const ended = start + init_4mhz;
  r.run_until(ended);
  REQUIRE(r.ready());
  CHECK(r.due(ended + round_4mhz));
  std::vector<std::uint32_t> got;
  for(int read = 0; read < 4; ++read)
  {
    CHECK(r.ready());
    got.push_back(r.cpu_dr());
  }
  CHECK(got == words_from(0, 4));
  CHECK(std::ranges::equal(got, seed_0_words));
  CHECK_FALSE(r.ready());
  CHECK(r.cpu_dr() == 0);
  CHECK(r.read(dr) == 0);
  r.run_until(ended + round_4mhz - cycle);
  CHECK_FALSE(r.ready());
  r.run_until(ended + round_4mhz);
  CHECK(r.ready());
  CHECK(r.cpu_dr() == rng_word_at(0, 4));
  CHECK(r.logged.empty());
}

TEST_CASE("the conditioning register refills the FIFO when its fourth word is read")
{
  rig const  r;
  auto const full = full_pipeline(r);
  CHECK_FALSE(std::ranges::any_of(r.pending(), [&](std::uint64_t ps) { return ps > full; }));
  std::vector<std::uint32_t> got;
  for(int read = 0; read < 8; ++read)
  {
    CHECK(r.ready());
    got.push_back(r.cpu_dr());
  }
  CHECK(got == words_from(0, 8));
  CHECK_FALSE(r.ready());
  CHECK(r.due(r.now() + round_4mhz));
  CHECK(r.logged.empty());
}

TEST_CASE("RNGEN 0 pauses init and rounds, the FIFO and register still deliver")
{
  SUBCASE("mid-init")
  {
    rig const  r;
    auto const start = enabled_at_4mhz(r);
    r.run_until(start + 300 * us);
    r.write(cr, cr_reset);
    CHECK_FALSE(r.due(start + init_4mhz));
    r.run_for(200 * us);
    r.write(cr, cr_reset | rngen);
    CHECK(r.due(start + init_4mhz + 200 * us));
    r.run_until(start + init_4mhz + 200 * us - cycle);
    CHECK_FALSE(r.ready());
    r.run_until(start + init_4mhz + 200 * us);
    CHECK(r.ready());
    CHECK(r.logged.empty());
  }
  SUBCASE("FIFO and register full")
  {
    rig const r;
    full_pipeline(r);
    r.write(cr, cr_reset);
    std::vector<std::uint32_t> got;
    for(int read = 0; read < 8; ++read)
    {
      CHECK(r.ready());
      got.push_back(r.cpu_dr());
    }
    CHECK(got == words_from(0, 8));
    CHECK_FALSE(r.ready());
    auto const now = r.now();
    CHECK_FALSE(std::ranges::any_of(r.pending(), [&](std::uint64_t ps) { return ps > now; }));
    r.run_for(10 * round_4mhz);
    CHECK_FALSE(r.ready());
    CHECK(r.logged.empty());
  }
}

TEST_CASE("HSI48 stopped mid-round pauses it, restarted resumes with the remaining time")
{
  rig const  r;
  auto const start = enabled_at_4mhz(r);
  auto const ended = start + init_4mhz;
  r.run_until(ended);
  for(int read = 0; read < 4; ++read)
  {
    static_cast<void>(r.cpu_dr());
  }
  auto const due = ended + round_4mhz;
  r.run_until(due - 50 * us);
  r.set_field("RCC", "RCC_CR", "HSI48ON", 0);
  CHECK_FALSE(r.due(due));
  r.run_for(300 * us);
  CHECK_FALSE(r.ready());
  auto const resumed = r.hsi48(true) + 50 * us;
  CHECK(r.due(resumed));
  r.run_until(resumed - cycle);
  CHECK_FALSE(r.ready());
  r.run_until(resumed);
  CHECK(r.ready());
  CHECK(r.cpu_dr() == rng_word_at(0, 4));
  CHECK(r.logged.empty());
}

TEST_CASE("CONDRST empties the FIFO and the register, clears SR and restarts init")
{
  rig const r;
  full_pipeline(r);
  REQUIRE(r.ready());
  r.write(cr, cr_reset | rngen | condrst);
  CHECK(r.read(sr) == 0);
  CHECK(r.read(dr) == 0);
  CHECK(r.cpu_dr() == 0);
  r.write(cr, cr_reset | rngen);
  auto const cleared = r.now() + clear_4mhz;
  r.run_until(cleared);
  REQUIRE(r.read(cr) == (cr_reset | rngen));
  CHECK(r.due(cleared + init_4mhz));
  r.run_until(cleared + init_4mhz - cycle);
  CHECK_FALSE(r.ready());
  r.run_until(cleared + init_4mhz);
  CHECK(r.ready());
  CHECK(r.cpu_dr() == rng_word_at(0, 8));
  CHECK(r.logged.empty());
}

TEST_CASE("a debugger read of DR shows the head and pops nothing")
{
  rig const  r;
  auto const start = enabled_at_4mhz(r);
  r.run_until(start + init_4mhz);
  CHECK(r.read(dr) == rng_word_at(0, 0));
  CHECK(r.read(dr, sec_base) == rng_word_at(0, 0));
  CHECK(r.cpu_dr() == rng_word_at(0, 0));
  CHECK(r.read(dr) == rng_word_at(0, 1));
  CHECK(r.cpu_dr() == rng_word_at(0, 1));
  CHECK(r.logged.empty());
}

TEST_CASE("the RNG line follows IE, RNGEN and DRDY with no read between")
{
  rig const  r;
  auto const start = enabled_at_4mhz(r);
  r.run_until(start + init_4mhz);
  CHECK(r.line() == 0);
  r.write(cr, cr_reset | rngen | ie);
  CHECK(r.line() == 1);
  r.write(sr, 0);
  r.write(sr, 0xFFFF'FFFF);
  CHECK(r.read(sr) == drdy);
  CHECK(r.line() == 1);
  r.write(cr, cr_reset | ie);
  CHECK(r.line() == 0);
  r.write(cr, cr_reset | rngen | ie);
  CHECK(r.line() == 1);
  for(int read = 0; read < 4; ++read)
  {
    static_cast<void>(r.cpu_dr());
  }
  CHECK(r.line() == 0);
  r.run_until(start + init_4mhz + round_4mhz);
  CHECK(r.line() == 1);
  CHECK(r.logged.empty());
}

TEST_CASE("a snapshot mid-init, paused, mid-round or mid-CONDRST restores the same words and times")
{
  struct leg
  {
    std::string_view name;
    std::uint64_t    at;
    std::uint32_t    control;
  };
  for(auto const& [name, at, control] : {
          leg{"mid-init",        500 * us,            cr_reset | rngen          },
          leg{"paused mid-init", 500 * us,            cr_reset                  },
          leg{"mid-round",       init_4mhz + 40 * us, cr_reset | rngen          },
          leg{"mid-condrst",     init_4mhz + 40 * us, cr_reset | rngen | condrst}
  })
  {
    CAPTURE(name);
    rig const  r;
    auto const start = enabled_at_4mhz(r);
    r.run_until(start + at);
    if((control & condrst) != 0)
    {
      r.write(cr, cr_reset | rngen | condrst);
      r.write(cr, cr_reset | rngen);
    }
    else
    {
      r.write(cr, control);
    }
    auto const saved   = r.f.target->snapshot();
    auto const pending = r.pending();
    auto const replay  = [](rig const& on) {
      std::vector<std::uint64_t> at;
      std::vector<std::uint32_t> got;
      on.run_for(100 * us);
      on.write(cr, cr_reset | rngen);
      while(got.size() < 12)
      {
        on.run_for(us);
        while(on.ready())
        {
          at.push_back(on.now());
          got.push_back(on.cpu_dr());
        }
      }
      return std::pair{at, got};
    };
    auto const first = replay(r);
    rig const  other;
    other.f.target->restore(
        core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
    CHECK(other.pending() == pending);
    CHECK(other.read(cr) == control);
    CHECK(replay(other) == first);
    CHECK(first.second == words_from((control & condrst) != 0 ? 4 : 0, 12));
    CHECK(r.logged.empty());
    CHECK(other.logged.empty());
  }
}

TEST_CASE("a system reset clears the FIFO and flags and keeps the generator position")
{
  rig const  r;
  auto const start = enabled_at_4mhz(r);
  r.run_until(start + init_4mhz);
  for(int read = 0; read < 2; ++read)
  {
    static_cast<void>(r.cpu_dr());
  }
  r.f.target->reset(popcorn::reset_kind::software);
  r.f.set("pc", sym("rng_ll", "idle"));
  CHECK(r.read(cr) == cr_reset);
  CHECK(r.read(sr) == 0);
  CHECK(r.read(dr) == 0);
  CHECK(r.cpu_dr() == 0);
  auto const again = enabled_at_4mhz(r);
  r.run_until(again + init_4mhz);
  std::vector<std::uint32_t> got;
  for(int read = 0; read < 4; ++read)
  {
    got.push_back(r.cpu_dr());
  }
  CHECK(got == words_from(4, 4));
  CHECK(r.logged.empty());
}

TEST_CASE("the LL flow reads 16 words of seed 0, polls DRDY and leaves CR at reset")
{
  rig const r{"enable_read"};
  REQUIRE(r.finish(200'000).reason == stop_reason::user);
  auto const log = r.log();
  REQUIRE(log.size() == 49);
  for(std::size_t word = 0; word < 16; ++word)
  {
    CAPTURE(word);
    CHECK(log[3 * word] == drdy);
    CHECK(log[3 * word + 1] < 2000);
    CHECK(log[3 * word + 2] == rng_word_at(0, word));
  }
  CHECK(log[1] > 100);
  CHECK(log[48] == cr_reset);
  CHECK(r.logged.empty());
}
