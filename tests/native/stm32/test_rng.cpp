#include "model_base.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/rng.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
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
constexpr std::uint32_t cecs       = 1u << 1;
constexpr std::uint32_t secs       = 1u << 2;
constexpr std::uint32_t ceis       = 1u << 5;
constexpr std::uint32_t seis       = 1u << 6;
constexpr std::uint32_t ardis      = 1u << 7;
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
constexpr std::uint64_t init_4mhz        = 966'916'667;
constexpr std::uint64_t round_4mhz       = 101'500'000;
constexpr std::uint64_t clear_4mhz       = 541'667;
constexpr std::uint64_t init_160mhz      = 36'497'917;
constexpr std::uint64_t round_160mhz     = 5'333'334;
constexpr std::uint64_t init_config_c    = 40'164'584;
constexpr std::uint64_t init_config_c_d1 = 74'831'250;

constexpr std::array<std::uint32_t, 4> seed_0_words{
    0xE220'A839, 0x6E78'9E6A, 0x06C4'5D18, 0xF88B'B8A8};

auto words_from(std::uint64_t start, std::uint64_t count, std::uint64_t seed = 0)
    -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(auto position = start; position < start + count; ++position)
  {
    out.push_back(rng_word_at(seed, position));
  }
  return out;
}

class clock_probe final : public popcorn::periph::model
{
public:
  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }
  void reset(popcorn::periph::register_file&, popcorn::reset_kind) override
  {}
  auto on_read(
      popcorn::periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      popcorn::periph::access_ctx) -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    return 0u;
  }
  auto on_write(
      popcorn::periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t,
      popcorn::periph::access_ctx) -> std::expected<void, popcorn::bus_error> override
  {
    return {};
  }
  void on_event(std::uint32_t, std::uint64_t) override
  {}
  void save(std::vector<std::byte>&) const override
  {}
  void load(std::span<std::byte const>) override
  {}
};

auto entered(std::string_view entry, std::uint64_t seed = 0, core::clock** probe = nullptr)
    -> stm32_fixture
{
  auto options               = popcorn::testing::stm32_options();
  options.strict_peripherals = true;
  if(probe != nullptr)
  {
    options.models.insert_or_assign(
        "CORDIC",
        [probe](popcorn::periph::model_context& ctx, popcorn::peripheral_desc const&)
            -> std::shared_ptr<popcorn::periph::model> {
          *probe = &ctx.clock;
          return std::make_shared<clock_probe>();
        });
  }
  if(seed != 0)
  {
    popcorn::periph::stm32::board board{};
    board.rng.seed = seed;
    options.models.insert_or_assign(
        "RNG", popcorn::periph::stm32::factories(board, u575()).at("RNG"));
  }
  stm32_fixture f{u575(), "rng_ll", std::move(options)};
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{100'000}, .until = sym("rng_ll", "main")}).reason
      == stop_reason::address);
  f.set("pc", sym("rng_ll", entry));
  return f;
}

struct rig
{
  explicit rig(
      std::string_view entry = "idle",
      std::uint64_t    seed  = 0,
      core::clock**    probe = nullptr)
      : f{entered(entry, seed, probe)}
  {}

  void until(std::string_view symbol, std::uint64_t instructions) const
  {
    REQUIRE(
        f.run_to_stop({.max_instructions = icount{instructions}, .until = sym("rng_ll", symbol)})
            .reason
        == stop_reason::address);
  }

  [[nodiscard]] auto source() const -> popcorn::periph::stm32::entropy_source&
  {
    auto* const found =
        dynamic_cast<popcorn::periph::stm32::entropy_source*>(f.target->model("RNG"));
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto fw_words() const -> std::vector<std::uint32_t>
  {
    auto const log = f.log();
    REQUIRE(log.size() == 49);
    std::vector<std::uint32_t> out;
    for(std::size_t word = 0; word < 16; ++word)
    {
      CHECK(log[3 * word] == drdy);
      out.push_back(log[3 * word + 2]);
    }
    return out;
  }

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
  r.write(cr, cr_nist | condrst | rngen);
  REQUIRE(r.read(cr) == (cr_nist | condrst | rngen));
  r.write(cr, cr_nist | clkdiv(2) | ced | condrst);
  CHECK(r.read(cr) == (cr_nist | clkdiv(2) | ced | condrst));
  r.write(cr, cr_nist | clkdiv(2) | ced | rngen);
  CHECK(r.read(cr) == (cr_nist | clkdiv(2) | ced | rngen | condrst));
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
  SUBCASE("written 1 again while the clear runs")
  {
    rig const r;
    r.clocks();
    r.write(cr, cr_reset | condrst);
    r.write(cr, cr_reset);
    auto const done = r.now() + clear_4mhz;
    REQUIRE(r.due(done));
    r.write(cr, cr_reset | condrst);
    CHECK_FALSE(r.due(done));
    r.run_until(done + 10 * cycle);
    CHECK(r.read(cr) == (cr_reset | condrst));
    r.write(cr, cr_reset);
    auto const again = r.now() + clear_4mhz;
    CHECK(r.due(again));
    r.run_until(again);
    CHECK(r.read(cr) == cr_reset);
    CHECK(r.logged.empty());
  }
  SUBCASE("kernel clock stopped while the clear runs")
  {
    rig const r;
    r.clocks();
    r.write(cr, cr_reset | condrst);
    r.write(cr, cr_reset);
    auto const done = r.now() + clear_4mhz;
    REQUIRE(r.due(done));
    r.set_field("RCC", "RCC_CR", "HSI48ON", 0);
    CHECK_FALSE(r.due(done));
    r.run_for(1000 * us);
    CHECK(r.read(cr) == (cr_reset | condrst));
    auto const ready = r.hsi48(true);
    CHECK(r.due(ready + clear_4mhz));
    r.run_until(ready + clear_4mhz);
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
    CHECK(r.log() == std::vector<std::uint32_t>{2000, ceis | cecs, 0});
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
          std::pair{cr_nist,             init_config_c   },
          std::pair{cr_nist | clkdiv(1), init_config_c_d1}
  })
  {
    CAPTURE(config);
    rig const r{"pll_160"};
    REQUIRE(r.finish(100'000).reason == stop_reason::user);
    r.f.set("pc", sym("rng_ll", "idle"));
    r.write(cr, config | condrst);
    r.write(cr, config);
    r.run_for(us);
    REQUIRE(r.read(cr) == config);
    r.write(cr, config | rngen);
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
  auto const left = due - r.now();
  r.set_field("RCC", "RCC_CR", "HSI48ON", 0);
  CHECK_FALSE(r.due(due));
  r.run_for(300 * us);
  CHECK_FALSE(r.ready());
  auto const resumed = r.hsi48(true) + left;
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

TEST_CASE("a system reset clears the FIFO, flags and CONFIGLOCK and keeps the generator position")
{
  rig const  r;
  auto const start = enabled_at_4mhz(r);
  r.write(cr, cr_reset | rngen | configlock);
  REQUIRE(r.read(cr) == (cr_reset | rngen | configlock));
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
  r.write(cr, cr_nist | condrst | rngen);
  CHECK(r.read(cr) == (cr_nist | condrst | rngen));
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

TEST_CASE("a board seed gives the firmware the words of that seed")
{
  rig const r{"enable_read", 0x5EED};
  CHECK(r.source().seed() == 0x5EED);
  REQUIRE(r.finish(200'000).reason == stop_reason::user);
  CHECK(r.fw_words() == words_from(0, 16, 0x5EED));
  CHECK(r.fw_words() != words_from(0, 16));
  CHECK(r.logged.empty());
}

TEST_CASE("queued words come before the generator and do not move its position")
{
  rig const                        r{"enable_read"};
  std::vector<std::uint32_t> const queued{
      0x0000'0000, 0x0000'0001, 0xFFFF'FFFF, 0x1234'5678, 0x0BAD'F00D, 0xDEAD'BEEF, 0x5555'AAAA,
      0xAAAA'5555, 0x0101'0101, 0x8000'0000, 0x7FFF'FFFF, 0xCAFE'BABE, 0x1111'1111, 0x2222'2222};
  r.source().queue(queued);
  CHECK(r.source().queued() == 14);
  REQUIRE(r.finish(200'000).reason == stop_reason::user);
  auto expected = queued;
  expected.push_back(rng_word_at(0, 0));
  expected.push_back(rng_word_at(0, 1));
  auto const log = r.f.log();
  REQUIRE(log.size() == 49);
  std::vector<std::uint32_t> got;
  for(std::size_t word = 0; word < 16; ++word)
  {
    got.push_back(log[3 * word + 2]);
  }
  CHECK(got == expected);
  CHECK(r.source().queued() == 0);
  CHECK(r.source().drawn() == 2);
  CHECK(r.logged.empty());
}

TEST_CASE("a stuck word fills every new word, the words already produced keep theirs")
{
  SUBCASE("set before the LL flow")
  {
    rig const r{"enable_read"};
    r.source().set_stuck(0xA5A5'A5A5);
    CHECK(r.source().stuck() == 0xA5A5'A5A5);
    REQUIRE(r.finish(200'000).reason == stop_reason::user);
    CHECK(r.fw_words() == std::vector<std::uint32_t>(16, 0xA5A5'A5A5));
    CHECK(r.source().drawn() == 0);
    CHECK(r.logged.empty());
  }
  SUBCASE("cleared with the FIFO and the register full")
  {
    rig const r;
    r.source().set_stuck(0xA5A5'A5A5);
    full_pipeline(r);
    r.source().set_stuck(std::nullopt);
    CHECK_FALSE(r.source().stuck().has_value());
    std::vector<std::uint32_t> got;
    for(int read = 0; read < 8; ++read)
    {
      got.push_back(r.cpu_dr());
    }
    CHECK(got == std::vector<std::uint32_t>(8, 0xA5A5'A5A5));
    r.run_for(round_4mhz);
    got.clear();
    for(int read = 0; read < 4; ++read)
    {
      got.push_back(r.cpu_dr());
    }
    CHECK(got == words_from(0, 4));
    CHECK(r.logged.empty());
  }
}

TEST_CASE("a snapshot keeps the queue, the stuck word and the generator position")
{
  rig const                        r;
  std::vector<std::uint32_t> const queued{11, 22, 33, 44, 55, 66};
  r.source().queue(queued);
  r.source().set_stuck(0x0F0F'0F0F);
  auto const start = enabled_at_4mhz(r);
  r.run_until(start + init_4mhz);
  r.source().set_stuck(std::nullopt);
  for(int read = 0; read < 4; ++read)
  {
    static_cast<void>(r.cpu_dr());
  }
  r.source().set_stuck(0x0F0F'0F0F);
  auto const saved  = r.f.target->snapshot();
  auto const replay = [](rig const& on) {
    std::vector<std::uint32_t> got;
    on.run_for(round_4mhz);
    on.source().set_stuck(std::nullopt);
    for(int round = 0; round < 3; ++round)
    {
      for(int read = 0; read < 4; ++read)
      {
        got.push_back(on.cpu_dr());
      }
      on.run_for(round_4mhz);
    }
    return std::tuple{got, on.source().drawn(), on.source().queued()};
  };
  CHECK(r.source().queued() == 6);
  auto const first = replay(r);
  rig const  other;
  other.f.target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.source().queued() == 6);
  CHECK(other.source().stuck() == 0x0F0F'0F0F);
  CHECK(replay(other) == first);
  auto expected = std::vector<std::uint32_t>(4, 0x0F0F'0F0F);
  expected.insert(expected.end(), queued.begin(), queued.end());
  expected.push_back(rng_word_at(0, 0));
  expected.push_back(rng_word_at(0, 1));
  CHECK(std::get<0>(first) == expected);
  CHECK(r.logged.empty());
  CHECK(other.logged.empty());
}

TEST_CASE("every reset kind keeps the queue, the stuck word and the generator position")
{
  for(auto const kind :
      {popcorn::reset_kind::software, popcorn::reset_kind::power_on, popcorn::reset_kind::watchdog})
  {
    CAPTURE(static_cast<int>(kind));
    rig const  r;
    auto const start = enabled_at_4mhz(r);
    r.run_until(start + init_4mhz);
    std::vector<std::uint32_t> const queued{1, 2, 3, 4, 5};
    r.source().queue(queued);
    r.source().set_stuck(0x77);
    r.source().set_seed_fault_after(5);
    r.source().set_clock_fault_after(7);
    r.source().set_clock_fault(true);
    r.source().set_seed_fault(true);
    r.f.target->reset(kind);
    r.f.set("pc", sym("rng_ll", "idle"));
    CHECK(r.source().queued() == 5);
    CHECK(r.source().stuck() == 0x77);
    CHECK(r.source().drawn() == 4);
    CHECK(r.source().seed_fault_after() == 5u);
    CHECK(r.source().clock_fault_after() == 7u);
    CHECK(r.source().clock_fault());
    CHECK(r.source().seed_fault());
    CHECK(r.read(sr) == 0);
    r.source().set_stuck(std::nullopt);
    r.source().set_seed_fault(false);
    r.source().set_clock_fault(false);
    r.source().set_seed_fault_after(std::nullopt);
    r.source().set_clock_fault_after(std::nullopt);
    auto const again = enabled_at_4mhz(r);
    r.run_until(again + init_4mhz);
    std::vector<std::uint32_t> got;
    for(int read = 0; read < 4; ++read)
    {
      got.push_back(r.cpu_dr());
    }
    CHECK(got == std::vector<std::uint32_t>{1, 2, 3, 4});
    CHECK(r.source().queued() == 1);
    CHECK(r.logged.empty());
  }
}

TEST_CASE("a queue past its bound is refused before any change")
{
  rig const                        r;
  std::vector<std::uint32_t> const half(popcorn::periph::stm32::rng_max_queued / 2, 9);
  r.source().queue(half);
  r.source().queue(half);
  CHECK(r.source().queued() == popcorn::periph::stm32::rng_max_queued);
  std::vector<std::uint32_t> const one{1};
  CHECK_THROWS_AS(r.source().queue(one), popcorn::config_error);
  CHECK(r.source().queued() == popcorn::periph::stm32::rng_max_queued);
  auto const start = enabled_at_4mhz(r);
  r.run_until(start + init_4mhz);
  for(int read = 0; read < 4; ++read)
  {
    static_cast<void>(r.cpu_dr());
  }
  CHECK(r.source().queued() == popcorn::periph::stm32::rng_max_queued - 4);
  r.source().queue(std::vector<std::uint32_t>(4, 7));
  CHECK(r.source().queued() == popcorn::periph::stm32::rng_max_queued);
  CHECK(r.logged.empty());
}

TEST_CASE("1000 queue and drain cycles leave the queue storage bounded")
{
  rig const  r;
  auto const start = enabled_at_4mhz(r);
  r.run_until(start + init_4mhz);
  for(std::uint32_t cycle = 0; cycle < 1000; ++cycle)
  {
    std::vector<std::uint32_t> const words{cycle, cycle + 1, cycle + 2, cycle + 3};
    r.source().queue(words);
    for(int read = 0; read < 4; ++read)
    {
      static_cast<void>(r.cpu_dr());
    }
    r.run_for(round_4mhz);
    REQUIRE(r.source().queued() == 0);
  }
  std::vector<std::byte> saved;
  r.f.target->model("RNG")->save(saved);
  CHECK(saved.size() < 1024);
  CHECK(r.logged.empty());
}

TEST_CASE("a seed error while producing empties the pipeline and stops generation")
{
  rig const r;
  full_pipeline(r);
  r.write(cr, cr_reset | rngen | ie);
  REQUIRE(r.ready());
  r.source().set_seed_fault(true);
  CHECK(r.source().seed_fault());
  CHECK(r.read(sr) == (secs | seis));
  CHECK(r.read(dr) == 0);
  CHECK(r.cpu_dr() == 0);
  CHECK(r.line() == 1);
  auto const now = r.now();
  CHECK_FALSE(std::ranges::any_of(r.pending(), [&](std::uint64_t ps) { return ps > now; }));
  r.run_for(10 * init_4mhz);
  CHECK(r.read(sr) == (secs | seis));
  CHECK(r.logged.empty());
}

TEST_CASE("a seed fault set with RNGEN 0 is detected at the RNGEN write")
{
  rig const r;
  r.clocks();
  r.source().set_seed_fault(true);
  CHECK(r.read(sr) == 0);
  r.run_for(us);
  CHECK(r.read(sr) == 0);
  r.write(cr, cr_reset | rngen);
  CHECK(r.read(sr) == (secs | seis));
  CHECK(r.logged.empty());
}

TEST_CASE("ARDIS 1: a CONDRST cycle recovers a released seed error, a held one comes back")
{
  SUBCASE("released before the CONDRST cycle")
  {
    rig const r{"seed_recover_condrst"};
    r.source().set_seed_fault(true);
    r.until("checkpoint", 100'000);
    auto log = r.log();
    REQUIRE(log.size() == 3);
    CHECK(log[1] == 0);
    CHECK(log[2] == (secs | seis));
    r.source().set_seed_fault(false);
    CHECK(r.read(sr) == (secs | seis));
    REQUIRE(r.finish(200'000).reason == stop_reason::user);
    log = r.log();
    REQUIRE(log.size() == 16);
    CHECK(log[3] >= 1);
    CHECK(log[4] == 0);
    CHECK(log[5] == 0);
    CHECK(log[6] == 0);
    for(std::size_t word = 0; word < 4; ++word)
    {
      CHECK(log[7 + 2 * word] < 2000);
      CHECK(log[8 + 2 * word] == rng_word_at(0, word));
    }
    CHECK(r.logged.empty());
  }
  SUBCASE("held")
  {
    rig const r{"seed_recover_condrst"};
    r.source().set_seed_fault(true);
    REQUIRE(r.finish(400'000).reason == stop_reason::user);
    auto const log = r.log();
    REQUIRE(log.size() == 16);
    CHECK(log[4] == secs);
    CHECK(log[5] == 2000);
    CHECK(log[6] == secs);
    for(std::size_t word = 0; word < 4; ++word)
    {
      CHECK(log[7 + 2 * word] == 2000);
      CHECK(log[8 + 2 * word] == 0);
    }
    CHECK(r.logged.empty());
  }
}

TEST_CASE("ARDIS 0: SECS clears 2 AHB plus 2 RNG cycles after the fault is released")
{
  rig const r{"seed_recover_auto"};
  r.source().set_seed_fault(true);
  r.until("checkpoint", 100'000);
  auto log = r.log();
  REQUIRE(log.size() == 2);
  CHECK(log[1] == (secs | seis));
  r.source().set_seed_fault(false);
  auto const due = r.now() + clear_4mhz;
  CHECK(r.due(due));
  r.f.set("pc", sym("rng_ll", "idle"));
  r.run_until(due - cycle);
  CHECK(r.read(sr) == (secs | seis));
  r.run_until(due);
  CHECK(r.read(sr) == seis);
  CHECK(r.due(due + init_4mhz));
  CHECK(r.logged.empty());
}

TEST_CASE("ARDIS 0: the LL flow recovers once the fault is released")
{
  rig const r{"seed_recover_auto"};
  r.source().set_seed_fault(true);
  r.until("checkpoint", 100'000);
  r.source().set_seed_fault(false);
  REQUIRE(r.finish(200'000).reason == stop_reason::user);
  auto const log = r.log();
  REQUIRE(log.size() == 13);
  CHECK(log[3] == 0);
  for(std::size_t word = 0; word < 4; ++word)
  {
    CHECK(log[4 + 2 * word] < 2000);
    CHECK(log[5 + 2 * word] == rng_word_at(0, word));
  }
  CHECK(r.logged.empty());
}

TEST_CASE("CONDRST clears SEIS and CEIS and keeps SECS until the clear ends")
{
  rig const r;
  r.clocks();
  r.source().set_seed_fault(true);
  r.source().set_clock_fault(true);
  r.write(cr, cr_reset | rngen | ardis | condrst);
  r.write(cr, cr_reset | rngen | ardis);
  r.run_for(us);
  REQUIRE(r.read(sr) == (secs | seis | cecs | ceis));
  r.write(cr, cr_reset | rngen | ardis | condrst);
  CHECK(r.read(sr) == (secs | cecs));
  r.source().set_seed_fault(false);
  r.run_for(10 * us);
  CHECK(r.read(sr) == (secs | cecs));
  r.write(cr, cr_reset | rngen | ardis);
  auto const done = r.now() + clear_4mhz;
  r.run_until(done - cycle);
  CHECK(r.read(sr) == (secs | cecs));
  r.run_until(done);
  CHECK(r.read(sr) == cecs);
  CHECK(r.due(done + init_4mhz));
  CHECK(r.logged.empty());
}

TEST_CASE("HSI48 stopped with RNGEN 1 sets CECS and CEIS, CECS clears when it is back")
{
  rig const r{"clock_error"};
  REQUIRE(r.finish(200'000).reason == stop_reason::user);
  auto const log = r.log();
  REQUIRE(log.size() == 7);
  CHECK(log[1] == rng_word_at(0, 0));
  CHECK(log[2] == 0);
  CHECK((log[3] & (cecs | ceis)) == (cecs | ceis));
  CHECK((log[5] & (cecs | ceis)) == ceis);
  CHECK((log[6] & (cecs | ceis | secs | seis)) == 0);
  CHECK(r.logged.empty());
}

TEST_CASE("CED 1 keeps CECS clear with HSI48 stopped")
{
  rig const r{"clock_error_ced_off"};
  REQUIRE(r.finish(200'000).reason == stop_reason::user);
  auto const log = r.log();
  REQUIRE(log.size() == 5);
  CHECK(log[0] < 2000);
  CHECK(log[3] == 2000);
  CHECK((log[4] & (cecs | ceis)) == 0);
  CHECK(r.logged.empty());
}

TEST_CASE("the AHB enable cleared with RNGEN 1 freezes the RNG with no clock error")
{
  rig const r;
  r.clocks();
  r.write(cr, cr_reset | rngen);
  r.set_field("RCC", "RCC_AHB2ENR1", "RNGEN", 0);
  r.run_for(us);
  CHECK(r.read(sr) == 0);
  r.set_field("RCC", "RCC_AHB2ENR1", "RNGEN", 1);
  r.set_field("RCC", "RCC_CR", "HSI48ON", 0);
  r.run_for(us);
  CHECK(r.read(sr) == (cecs | ceis));
  CHECK(r.logged.empty());
}

TEST_CASE("CECS follows 32 x fRNG < fHCLK, equality being a correct clock")
{
  core::clock* clock = nullptr;
  rig const    r{"idle", 0, &clock};
  REQUIRE(clock != nullptr);
  r.clocks();
  r.write(cr, cr_reset | rngen);
  REQUIRE(r.read(sr) == 0);
  auto const domain = clock->domain("rng");
  auto const hclk   = clock->hz(clock->domain("hclk"));
  auto* listener = dynamic_cast<popcorn::periph::stm32::clock_listener*>(r.f.target->model("RNG"));
  REQUIRE(listener != nullptr);
  clock->set_hz(domain, hclk / 32 - 1);
  listener->on_clock_change();
  CHECK(r.read(sr) == (cecs | ceis));
  clock->set_hz(domain, hclk / 32);
  listener->on_clock_change();
  CHECK(r.read(sr) == ceis);
  CHECK(r.logged.empty());
}

TEST_CASE("a clock fault sets CECS and CEIS and generation goes on")
{
  rig const  r;
  auto const start = enabled_at_4mhz(r);
  r.source().set_clock_fault(true);
  CHECK(r.read(sr) == (cecs | ceis));
  r.run_until(start + init_4mhz);
  CHECK(r.read(sr) == (cecs | ceis | drdy));
  CHECK(r.cpu_dr() == rng_word_at(0, 0));
  r.run_until(start + init_4mhz + round_4mhz);
  for(std::uint64_t word = 1; word < 8; ++word)
  {
    CHECK(r.cpu_dr() == rng_word_at(0, word));
  }
  r.run_for(round_4mhz);
  CHECK(r.read(sr) == (cecs | ceis | drdy));
  r.source().set_clock_fault(false);
  CHECK(r.read(sr) == (ceis | drdy));
  CHECK(r.logged.empty());
}

TEST_CASE("SEIS and CEIS keep a write of 1 and clear on a write of 0")
{
  rig const r;
  r.clocks();
  r.source().set_clock_fault(true);
  r.source().set_seed_fault(true);
  r.write(cr, cr_reset | rngen);
  REQUIRE(r.read(sr) == (secs | seis | cecs | ceis));
  r.write(sr, 0xFFFF'FFFF);
  CHECK(r.read(sr) == (secs | seis | cecs | ceis));
  r.write(sr, ~seis);
  CHECK(r.read(sr) == (secs | cecs | ceis));
  r.write(sr, ~ceis);
  CHECK(r.read(sr) == (secs | cecs));
  r.write(sr, 0);
  CHECK(r.read(sr) == (secs | cecs));
  r.write(sr, seis | ceis);
  CHECK(r.read(sr) == (secs | cecs));
  CHECK(r.logged.empty());
}

TEST_CASE("a snapshot with SECS held and an auto-reset pending recovers at the same time")
{
  rig const r;
  r.clocks();
  r.source().set_seed_fault(true);
  r.write(cr, cr_reset | rngen);
  REQUIRE(r.read(sr) == (secs | seis));
  r.source().set_seed_fault(false);
  auto const due   = r.now() + clear_4mhz;
  auto const saved = r.f.target->snapshot();
  rig const  other;
  other.f.target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  for(auto const* on : {&r, &other})
  {
    CHECK(on->due(due));
    CHECK_FALSE(on->source().seed_fault());
    on->run_until(due);
    CHECK(on->read(sr) == seis);
    CHECK(on->due(due + init_4mhz));
    on->run_until(due + init_4mhz);
    CHECK(on->cpu_dr() == rng_word_at(0, 0));
  }
  CHECK(r.logged.empty());
  CHECK(other.logged.empty());
}

TEST_CASE("a system reset clears SECS and keeps the seed fault, detected again at RNGEN 1")
{
  rig const r;
  r.clocks();
  r.source().set_seed_fault(true);
  r.write(cr, cr_reset | rngen);
  REQUIRE(r.read(sr) == (secs | seis));
  r.f.target->reset(popcorn::reset_kind::software);
  r.f.set("pc", sym("rng_ll", "idle"));
  CHECK(r.read(sr) == 0);
  CHECK(r.source().seed_fault());
  r.clocks();
  r.write(cr, cr_reset | rngen);
  CHECK(r.read(sr) == (secs | seis));
  CHECK(r.logged.empty());
}

TEST_CASE("seed_fault_after sets the seed fault at the n-th DR read that returns a word")
{
  rig const r{"keygen"};
  r.source().set_seed_fault_after(8);
  CHECK(r.source().seed_fault_after() == 8u);
  r.run_for(5 * init_4mhz);
  auto const word_at = [&](std::uint32_t index) {
    auto const got = r.f.target->mem().read(sym("rng_ll", "keygen_key") + 4 * index, 4);
    REQUIRE(got.has_value());
    return static_cast<std::uint32_t>(*got);
  };
  auto const status = r.f.target->mem().read(sym("rng_ll", "keygen_status"), 4);
  REQUIRE(status.has_value());
  CHECK(*status == 1);
  for(std::uint32_t word = 0; word < 8; ++word)
  {
    CHECK(word_at(word) == rng_word_at(0, word));
  }
  CHECK(word_at(8) == 0);
  CHECK(r.source().seed_fault());
  CHECK_FALSE(r.source().seed_fault_after().has_value());
  CHECK_THROWS_AS(r.source().set_seed_fault_after(0), popcorn::config_error);
  CHECK_FALSE(r.source().seed_fault_after().has_value());
  CHECK(r.logged.empty());
}

TEST_CASE("clock_fault_after counts CPU reads that return a word, not empty or host reads")
{
  rig const r;
  r.source().set_clock_fault_after(6);
  auto const start = enabled_at_4mhz(r);
  CHECK(r.cpu_dr() == 0);
  r.run_until(start + init_4mhz + round_4mhz);
  REQUIRE(r.read(sr) == drdy);
  for(int read = 0; read < 5; ++read)
  {
    static_cast<void>(r.read(dr));
    static_cast<void>(r.cpu_dr());
  }
  CHECK(r.source().clock_fault_after() == 1u);
  CHECK(r.read(sr) == drdy);
  CHECK(r.cpu_dr() == rng_word_at(0, 5));
  CHECK(r.read(sr) == (drdy | cecs | ceis));
  CHECK(r.source().clock_fault());
  CHECK_FALSE(r.source().clock_fault_after().has_value());
  CHECK_THROWS_AS(r.source().set_clock_fault_after(0), popcorn::config_error);
  CHECK(r.logged.empty());
}

TEST_CASE("keygen reports a good key, a seed error, a clock error and a repeated word")
{
  auto const status_of = [](rig const& r) {
    r.run_for(5 * init_4mhz);
    auto const done = r.f.target->mem().read(sym("rng_ll", "keygen_done"), 4);
    REQUIRE(done.has_value());
    REQUIRE(*done == 1);
    auto const status = r.f.target->mem().read(sym("rng_ll", "keygen_status"), 4);
    REQUIRE(status.has_value());
    return *status;
  };
  {
    rig const r{"keygen"};
    CHECK(status_of(r) == 0);
    for(std::uint32_t word = 0; word < 16; ++word)
    {
      auto const got = r.f.target->mem().read(sym("rng_ll", "keygen_key") + 4 * word, 4);
      REQUIRE(got.has_value());
      CHECK(*got == rng_word_at(0, word));
    }
  }
  {
    rig const r{"keygen"};
    r.source().set_seed_fault(true);
    CHECK(status_of(r) == 1);
  }
  {
    rig const r{"keygen"};
    r.source().set_clock_fault(true);
    CHECK(status_of(r) == 2);
  }
  {
    rig const r{"keygen"};
    r.source().set_stuck(0x1234'5678);
    CHECK(status_of(r) == 3);
  }
}

TEST_CASE("ARDIS 0: a fault released while CONDRST is held clears SECS at the end of the clear")
{
  rig const r;
  r.clocks();
  r.source().set_seed_fault(true);
  r.write(cr, cr_reset | rngen);
  REQUIRE(r.read(sr) == (secs | seis));
  r.write(cr, cr_reset | rngen | condrst);
  r.source().set_seed_fault(false);
  r.run_for(10 * us);
  CHECK(r.read(sr) == secs);
  r.write(cr, cr_reset | rngen);
  auto const done = r.now() + clear_4mhz;
  r.run_until(done - cycle);
  CHECK(r.read(sr) == secs);
  r.run_until(done);
  CHECK(r.read(sr) == 0);
  CHECK(r.logged.empty());
}

TEST_CASE("a snapshot keeps armed fault counters and held fault levels")
{
  SUBCASE("armed counters and a held clock fault")
  {
    rig const  r;
    auto const start = enabled_at_4mhz(r);
    r.source().set_seed_fault_after(5);
    r.source().set_clock_fault_after(7);
    r.source().set_clock_fault(true);
    auto const saved = r.f.target->snapshot();
    rig const  other;
    other.f.target->restore(
        core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
    CHECK(other.source().seed_fault_after() == 5u);
    CHECK(other.source().clock_fault_after() == 7u);
    CHECK(other.source().clock_fault());
    CHECK(other.read(sr) == (cecs | ceis));
    other.run_until(start + init_4mhz + round_4mhz);
    for(int read = 0; read < 4; ++read)
    {
      static_cast<void>(other.cpu_dr());
    }
    CHECK((other.read(sr) & secs) == 0);
    CHECK(other.cpu_dr() == rng_word_at(0, 4));
    CHECK((other.read(sr) & (secs | seis)) == (secs | seis));
    CHECK(other.logged.empty());
  }
  SUBCASE("a held seed fault")
  {
    rig const r;
    r.clocks();
    r.source().set_seed_fault(true);
    r.write(cr, cr_reset | rngen);
    REQUIRE(r.read(sr) == (secs | seis));
    auto const saved = r.f.target->snapshot();
    rig const  other;
    other.f.target->restore(
        core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
    CHECK(other.source().seed_fault());
    CHECK(other.read(sr) == (secs | seis));
    other.write(cr, cr_reset | rngen | condrst);
    other.write(cr, cr_reset | rngen);
    other.run_for(10 * us);
    CHECK(other.read(sr) == (secs | seis));
    CHECK(other.logged.empty());
  }
}

TEST_CASE("an AHB enable set while HSI48 is off raises the clock error at once")
{
  rig const r;
  r.clocks();
  r.write(cr, cr_reset | rngen);
  r.set_field("RCC", "RCC_AHB2ENR1", "RNGEN", 0);
  r.set_field("RCC", "RCC_CR", "HSI48ON", 0);
  r.run_for(us);
  CHECK(r.read(sr) == 0);
  r.set_field("RCC", "RCC_AHB2ENR1", "RNGEN", 1);
  CHECK(r.read(sr) == (cecs | ceis));
  CHECK(r.logged.empty());
}

TEST_CASE("ARDIS 1: a released seed fault keeps SECS until a CONDRST cycle")
{
  rig const r;
  r.clocks();
  r.write(cr, cr_reset | ardis | condrst);
  r.write(cr, cr_reset | ardis);
  r.run_for(us);
  r.source().set_seed_fault(true);
  r.write(cr, cr_reset | ardis | rngen);
  REQUIRE(r.read(sr) == (secs | seis));
  r.source().set_seed_fault(false);
  CHECK_FALSE(r.due(r.now() + clear_4mhz));
  r.run_for(10 * us);
  CHECK(r.read(sr) == (secs | seis));
  CHECK(r.logged.empty());
}

TEST_CASE("ARDIS 0: a kernel clock stop holds the auto-reset until the clock is back")
{
  rig const r;
  r.clocks();
  r.source().set_seed_fault(true);
  r.write(cr, cr_reset | rngen);
  REQUIRE(r.read(sr) == (secs | seis));
  r.source().set_seed_fault(false);
  auto const due = r.now() + clear_4mhz;
  REQUIRE(r.due(due));
  r.set_field("RCC", "RCC_CR", "HSI48ON", 0);
  CHECK_FALSE(r.due(due));
  r.run_for(1000 * us);
  CHECK((r.read(sr) & secs) == secs);
  auto const ready = r.hsi48(true);
  CHECK(r.due(ready + clear_4mhz));
  r.run_until(ready + clear_4mhz);
  CHECK((r.read(sr) & secs) == 0);
  CHECK(r.logged.empty());
}

TEST_CASE("a seed fault set again before the auto-reset waits for its own release")
{
  rig const r;
  r.clocks();
  r.source().set_seed_fault(true);
  r.write(cr, cr_reset | rngen);
  r.source().set_seed_fault(false);
  auto const first = r.now() + clear_4mhz;
  REQUIRE(r.due(first));
  r.source().set_seed_fault(true);
  CHECK_FALSE(r.due(first));
  r.run_for(10 * us);
  CHECK(r.read(sr) == (secs | seis));
  r.source().set_seed_fault(false);
  auto const second = r.now() + clear_4mhz;
  CHECK(r.due(second));
  r.run_until(second);
  CHECK((r.read(sr) & secs) == 0);
  CHECK(r.logged.empty());
}
