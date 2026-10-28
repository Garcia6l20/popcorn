#include "stm32_fixture.hpp"

#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/stm32/analog.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/supply.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core  = popcorn::core;
namespace stm32 = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::h573;
using popcorn::testing::rcc_reset_pulse;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr std::string_view firmware = "h573_rcc_ll";
constexpr std::uint64_t    cycle_ps = 31'250;
constexpr std::uint64_t    half_ps  = cycle_ps / 2;
constexpr std::uint64_t    us       = 1'000'000;
constexpr std::uint64_t    ms       = 1'000'000'000;

constexpr std::uint32_t aden     = 1u << 0;
constexpr std::uint32_t adstart  = 1u << 2;
constexpr std::uint32_t adstp    = 1u << 4;
constexpr std::uint32_t advregen = 1u << 28;
constexpr std::uint32_t deeppwd  = 1u << 29;
constexpr std::uint32_t eoc      = 1u << 2;
constexpr std::uint32_t ovr      = 1u << 4;
constexpr std::uint32_t dmaen    = 1u << 0;
constexpr std::uint32_t dmacfg   = 1u << 1;
constexpr std::uint32_t ovrmod   = 1u << 12;
constexpr std::uint32_t cont     = 1u << 13;
constexpr std::uint32_t align    = 1u << 15;
constexpr std::uint32_t awd1sgl  = 1u << 22;
constexpr std::uint32_t awd1en   = 1u << 23;
constexpr std::uint32_t rovse    = 1u << 0;
constexpr std::uint32_t jovse    = 1u << 1;
constexpr std::uint32_t smpplus  = 1u << 31;
constexpr std::uint32_t vrefen   = 1u << 22;
constexpr std::uint32_t tsen     = 1u << 23;

constexpr std::uint32_t vdda_mv     = 3300;
constexpr std::uint32_t ts_cal1     = 0x0301;
constexpr std::uint32_t ts_cal2     = 0x03FA;
constexpr std::uint32_t vrefint_cal = 0x05E5;

constexpr std::uint32_t    adc1_irq      = 37;
constexpr std::uint32_t    adc2_irq      = 69;
constexpr std::uint32_t    dma_word_copy = 0x400A'0002;
constexpr popcorn::address buffer        = 0x2001'1000;

[[nodiscard]] constexpr auto ovsr(std::uint32_t code) -> std::uint32_t
{
  return code << 2;
}

[[nodiscard]] constexpr auto res(std::uint32_t code) -> std::uint32_t
{
  return code << 3;
}

[[nodiscard]] constexpr auto ckmode(std::uint32_t code) -> std::uint32_t
{
  return code << 16;
}

[[nodiscard]] constexpr auto presc(std::uint32_t code) -> std::uint32_t
{
  return code << 18;
}

struct rig
{
  std::unique_ptr<stm32_fixture> f;
  std::vector<core::log_record>  logged;
  core::hook_handle              sink;

  explicit rig(bool strict = false)
  {
    stm32::board board{};
    board.supply.vdda_mv       = vdda_mv;
    auto options               = popcorn::testing::stm32_options();
    options.models             = stm32::factories(board, h573());
    options.strict_peripherals = strict;
    f = std::make_unique<stm32_fixture>(h573(), firmware, std::move(options));
    f->set("pc", sym(firmware, "idle"));
    sink = f->target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
    set("RCC", "RCC_AHB2ENR", "ADC12EN", 1);
  }

  [[nodiscard]] auto reg(std::string_view unit, std::string_view name) const
      -> popcorn::register_desc const&
  {
    auto const* found = popcorn::find_register(f->target->peripheral(unit).description(), name);
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto at(std::string_view unit, std::string_view name) const -> popcorn::address
  {
    return f->target->peripheral(unit).description().base + reg(unit, name).offset;
  }

  [[nodiscard]] auto read(std::string_view unit, std::string_view name) const -> std::uint32_t
  {
    auto const value = f->target->mem().read(at(unit, name), 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void write(std::string_view unit, std::string_view name, std::uint32_t value) const
  {
    REQUIRE(f->target->mem().write(at(unit, name), 4, value).has_value());
  }

  [[nodiscard]] auto get(std::string_view unit, std::string_view name, std::string_view field) const
      -> std::uint32_t
  {
    auto const& fields = reg(unit, name).fields;
    auto const  found  = std::ranges::find(fields, field, &popcorn::field_desc::name);
    REQUIRE(found != fields.end());
    return (read(unit, name) >> found->lsb) & ((std::uint32_t{1} << found->width) - 1u);
  }

  void
      set(std::string_view unit,
          std::string_view name,
          std::string_view field,
          std::uint32_t    value) const
  {
    auto const& fields = reg(unit, name).fields;
    auto const  found  = std::ranges::find(fields, field, &popcorn::field_desc::name);
    REQUIRE(found != fields.end());
    auto const mask = ((std::uint32_t{1} << found->width) - 1u) << found->lsb;
    write(unit, name, (read(unit, name) & ~mask) | ((value << found->lsb) & mask));
  }

  [[nodiscard]] auto isr(std::string_view field, std::string_view unit = "ADC1") const
      -> std::uint32_t
  {
    return get(unit, "ADC_ISR", field);
  }

  [[nodiscard]] auto cpu_read(std::string_view unit, std::string_view name) const -> std::uint32_t
  {
    auto&      regs  = f->target->peripheral(unit);
    auto const value = f->target->model(unit)->on_read(
        regs, reg(unit, name).offset, 4,
        {.pc = 0, .now = icount{0}, .by = popcorn::initiator::cpu});
    REQUIRE(value.has_value());
    return *value;
  }

  [[nodiscard]] auto port(std::string_view unit = "ADC1") const -> stm32::analog_port&
  {
    auto* const found = f->model_of<stm32::analog_port>(unit);
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto now() const -> std::uint64_t
  {
    return f->target->virtual_clock().time().ps;
  }

  void run_until(std::uint64_t ps) const
  {
    auto const result = f->run(
        {.max_instructions = icount{10'000'000},
         .wall_budget      = std::chrono::seconds{60},
         .until_time       = vtime{ps}});
    REQUIRE(result.stopped);
    REQUIRE(result.stopped->reason == stop_reason::time);
  }

  void settle() const
  {
    run_until(now() + 200 * us);
  }

  void enable(std::string_view unit = "ADC1") const
  {
    write(unit, "ADC_CR", advregen);
    write(unit, "ADC_CR", advregen | aden);
  }

  [[nodiscard]] auto start(std::uint32_t channel, std::string_view unit = "ADC1") const
      -> std::uint64_t
  {
    write(unit, "ADC_SQR1", channel << 6);
    auto const at = now();
    write(unit, "ADC_CR", advregen | aden | adstart);
    return at;
  }

  [[nodiscard]] auto convert(std::uint32_t channel, std::string_view unit = "ADC1") const
      -> std::uint32_t
  {
    static_cast<void>(start(channel, unit));
    settle();
    return cpu_read(unit, "ADC_DR");
  }

  void ends_at(std::uint64_t end, std::string_view unit = "ADC1") const
  {
    run_until((end + cycle_ps - 1) / cycle_ps * cycle_ps - cycle_ps);
    CHECK(isr("EOC", unit) == 0);
    run_until(end);
    CHECK(isr("EOC", unit) == 1);
  }

  [[nodiscard]] auto warnings() const -> std::vector<core::log_record>
  {
    std::vector<core::log_record> out;
    std::ranges::copy_if(logged, std::back_inserter(out), [](core::log_record const& record) {
      return record.kind == core::log_kind::model_warning;
    });
    return out;
  }

  [[nodiscard]] auto pending(std::uint32_t irq) const -> bool
  {
    auto const word = f->target->mem().read(0xe000'e200u + 4 * (irq / 32), 4);
    REQUIRE(word.has_value());
    return (*word >> (irq % 32) & 1u) != 0;
  }

  [[nodiscard]] auto buffer_words(std::size_t count) const -> std::vector<std::uint32_t>
  {
    std::vector<std::uint32_t> out;
    for(std::size_t index = 0; index < count; ++index)
    {
      auto const word = f->target->mem().read(buffer + 4 * index, 4);
      REQUIRE(word.has_value());
      out.push_back(static_cast<std::uint32_t>(*word));
    }
    return out;
  }

  void dma(std::uint32_t bytes) const
  {
    write("GPDMA1", "GPDMA_C0TR1", dma_word_copy);
    write("GPDMA1", "GPDMA_C0TR2", 0);
    write("GPDMA1", "GPDMA_C0BR1", bytes);
    write("GPDMA1", "GPDMA_C0SAR", static_cast<std::uint32_t>(at("ADC1", "ADC_DR")));
    write("GPDMA1", "GPDMA_C0DAR", static_cast<std::uint32_t>(buffer));
    write("GPDMA1", "GPDMA_C0LLR", 0);
    write("GPDMA1", "GPDMA_C0FCR", (1u << 8) | (1u << 9));
    write("GPDMA1", "GPDMA_C0CR", 1u);
  }

  [[nodiscard]] auto dma_left() const -> std::uint32_t
  {
    return get("GPDMA1", "GPDMA_C0BR1", "BNDT");
  }

  [[nodiscard]] auto tim15_trgo() const -> std::uint64_t
  {
    set("RCC", "RCC_APB2ENR", "TIM15EN", 1);
    write("TIM15", "PSC", 31);
    write("TIM15", "ARR", 999);
    write("TIM15", "EGR", 1);
    write("TIM15", "SR", 0);
    write("TIM15", "CR2", 2u << 4);
    auto const at = now();
    write("TIM15", "CR1", 1u);
    return at;
  }
};

[[nodiscard]] constexpr auto code_at(std::uint64_t microvolts) -> std::uint32_t
{
  return static_cast<std::uint32_t>(
      std::min<std::uint64_t>(4095, microvolts * 4096 / (std::uint64_t{vdda_mv} * 1000)));
}
}

TEST_CASE("ADC1, ADC2 and ADCC of the H573 attach, ADC_CR resets to DEEPPWD")
{
  rig const r;
  REQUIRE(r.f->target->model("ADC1") != nullptr);
  REQUIRE(r.f->target->model("ADC2") != nullptr);
  REQUIRE(r.f->target->model("ADCC") != nullptr);
  CHECK(r.read("ADC1", "ADC_CR") == deeppwd);
  CHECK(r.read("ADC2", "ADC_CR") == deeppwd);
}

TEST_CASE("channel 3 at 1650 mV with VDDA 3300 mV reads 2048 at 12 bits, RES drops two bits a step")
{
  rig const r;
  r.enable();
  r.port().drive(3, 1'650'000);
  CHECK(r.convert(3) == 2048);
  r.write("ADC1", "ADC_CFGR", res(1));
  CHECK(r.convert(3) == 512);
  r.write("ADC1", "ADC_CFGR", res(3));
  CHECK(r.convert(3) == 32);
  r.port().drive(3, 3'400'000);
  r.write("ADC1", "ADC_CFGR", 0);
  CHECK(r.convert(3) == 4095);
}

TEST_CASE("a conversion takes (SMP + TSAR) ADC clock cycles, SMPPLUS makes SMP 000 3.5 cycles")
{
  rig const r;
  r.enable();
  r.port().drive(3, 1'000'000);
  SUBCASE("SMP 000 at 12 bits: 2.5 + 12.5 cycles")
  {
    r.ends_at(r.start(3) + 15 * cycle_ps);
  }
  SUBCASE("SMPPLUS: 3.5 + 12.5 cycles")
  {
    r.write("ADC1", "ADC_SMPR1", smpplus);
    r.ends_at(r.start(3) + 16 * cycle_ps);
  }
  SUBCASE("SMPPLUS leaves SMP 001 at 6.5 cycles")
  {
    r.write("ADC1", "ADC_SMPR1", smpplus | (1u << 9));
    r.ends_at(r.start(3) + 19 * cycle_ps);
  }
  SUBCASE("each sampling code, 12 bits")
  {
    constexpr std::array<std::uint64_t, 8> halves{5, 13, 25, 49, 95, 185, 495, 1281};
    for(std::uint32_t code = 0; code < halves.size(); ++code)
    {
      CAPTURE(code);
      r.write("ADC1", "ADC_SMPR1", code << 9);
      r.ends_at(r.start(3) + (halves[code] + 25) * half_ps);
      static_cast<void>(r.cpu_read("ADC1", "ADC_DR"));
    }
  }
  SUBCASE("each resolution, SMP 000")
  {
    constexpr std::array<std::uint64_t, 4> halves{25, 21, 17, 13};
    for(std::uint32_t code = 0; code < halves.size(); ++code)
    {
      CAPTURE(code);
      r.write("ADC1", "ADC_CFGR", res(code));
      r.ends_at(r.start(3) + (5 + halves[code]) * half_ps);
      static_cast<void>(r.cpu_read("ADC1", "ADC_DR"));
    }
  }
}

TEST_CASE("H573 board: ADRDY sets 35 cycles after ADEN with ADVREGEN, 45 without")
{
  for(auto const& [regulator, cycles] : {
          std::pair{advregen, 35u},
          std::pair{0u,       45u}
  })
  {
    CAPTURE(cycles);
    rig const r;
    r.write("ADC1", "ADC_CR", regulator);
    auto const start = r.now();
    r.write("ADC1", "ADC_CR", regulator | aden);
    CHECK(r.read("ADC1", "ADC_ISR") == 0);
    r.run_until(start + (cycles - 1) * cycle_ps);
    CHECK(r.isr("ADRDY") == 0);
    r.run_until(start + cycles * cycle_ps);
    CHECK(r.read("ADC1", "ADC_ISR") == 1);
  }
}

TEST_CASE("ADEN in deep power-down sets no ADRDY")
{
  rig const r;
  r.write("ADC1", "ADC_CR", deeppwd | aden);
  r.settle();
  CHECK(r.isr("ADRDY") == 0);
  r.write("ADC1", "ADC_CR", deeppwd | aden | (1u << 1));
  r.write("ADC1", "ADC_CR", deeppwd | advregen);
  CHECK(r.get("ADC1", "ADC_CR", "ADVREGEN") == 0);
}

TEST_CASE("a snapshot taken while ADRDY is pending sets it 35 cycles after ADEN once restored")
{
  rig const r;
  r.write("ADC1", "ADC_CR", advregen);
  auto const start = r.now();
  r.write("ADC1", "ADC_CR", advregen | aden);
  r.run_until(start + 20 * cycle_ps);
  auto const saved = r.f->target->snapshot();
  r.run_until(start + 40 * cycle_ps);
  for(int again = 0; again < 2; ++again)
  {
    r.write("ADC1", "ADC_CR", advregen | aden | (1u << 1));
    r.write("ADC1", "ADC_CR", advregen | aden);
  }
  r.f->target->restore(saved);
  CHECK(r.isr("ADRDY") == 0);
  r.run_until(start + 34 * cycle_ps);
  CHECK(r.isr("ADRDY") == 0);
  r.run_until(start + 35 * cycle_ps);
  CHECK(r.isr("ADRDY") == 1);

  auto const bytes = core::encode_snapshot(saved.data());
  rig const  fresh;
  fresh.f->target->restore(core::snapshot{core::decode_snapshot(bytes)});
  CHECK(fresh.isr("ADRDY") == 0);
  fresh.run_until(start + 34 * cycle_ps);
  CHECK(fresh.isr("ADRDY") == 0);
  fresh.run_until(start + 35 * cycle_ps);
  CHECK(fresh.isr("ADRDY") == 1);
}

TEST_CASE("ADDIS, ADC12RST or a second ADEN before ADRDY leave no stale ADRDY")
{
  {
    rig const r;
    r.write("ADC1", "ADC_CR", advregen | aden);
    r.write("ADC1", "ADC_CR", advregen | aden | (1u << 1));
    r.settle();
    CHECK(r.isr("ADRDY") == 0);
  }
  {
    rig const r;
    r.write("ADC1", "ADC_CR", advregen | aden);
    rcc_reset_pulse(*r.f, "RCC_AHB2RSTR", "ADC12RST");
    r.settle();
    CHECK(r.isr("ADRDY") == 0);
  }
  {
    rig const r;
    r.write("ADC1", "ADC_CR", advregen | aden);
    auto const first = r.now();
    r.run_until(first + 20 * cycle_ps);
    r.write("ADC1", "ADC_CR", advregen | aden | (1u << 1));
    auto const second = r.now();
    r.write("ADC1", "ADC_CR", advregen | aden);
    r.run_until(first + 35 * cycle_ps);
    CHECK(r.isr("ADRDY") == 0);
    r.run_until(second + 35 * cycle_ps);
    CHECK(r.isr("ADRDY") == 1);
  }
}

TEST_CASE("EOC, EOS and ADRDY clear by writing 1, writing 0 keeps them")
{
  rig const r;
  r.enable();
  r.port().drive(3, 1'000'000);
  static_cast<void>(r.start(3));
  r.settle();
  REQUIRE(r.isr("EOC") == 1);
  REQUIRE(r.isr("EOS") == 1);
  REQUIRE(r.isr("ADRDY") == 1);
  r.write("ADC1", "ADC_ISR", 0);
  CHECK(r.isr("EOC") == 1);
  r.write("ADC1", "ADC_ISR", eoc);
  CHECK(r.isr("EOC") == 0);
  CHECK(r.isr("EOS") == 1);
  r.write("ADC1", "ADC_ISR", 0x7FF);
  CHECK(r.read("ADC1", "ADC_ISR") == 0);
}

TEST_CASE("H573 board: OVRMOD 0 keeps three results, the fourth sets OVR and leaves the head only")
{
  rig const r;
  r.enable();
  r.port().drive(3, 1'000'000);
  r.port().drive(4, 2'000'000);
  r.write("ADC1", "ADC_CFGR", cont);
  r.write("ADC1", "ADC_SQR1", 1u | (3u << 6) | (4u << 12));
  auto const start = r.now();
  r.write("ADC1", "ADC_CR", advregen | aden | adstart);
  r.run_until(start + 3 * 15 * cycle_ps);
  CHECK(r.isr("OVR") == 0);
  CHECK(r.read("ADC1", "ADC_DR") == code_at(1'000'000));
  r.run_until(start + 4 * 15 * cycle_ps);
  CHECK(r.isr("OVR") == 1);
  CHECK(r.read("ADC1", "ADC_DR") == code_at(1'000'000));
  r.write("ADC1", "ADC_CR", advregen | aden | adstp);
  std::vector<std::uint32_t> drained;
  while(r.isr("EOC") != 0)
  {
    drained.push_back(r.cpu_read("ADC1", "ADC_DR"));
  }
  CHECK(drained == std::vector<std::uint32_t>{code_at(1'000'000)});
}

TEST_CASE("H573 board: while OVR stays set a result lands in an empty FIFO, the next is discarded")
{
  rig const r;
  r.enable();
  r.port().drive(3, 1'000'000);
  r.write("ADC1", "ADC_CFGR", cont);
  auto const start = r.start(3);
  r.run_until(start + 4 * 15 * cycle_ps);
  REQUIRE(r.isr("OVR") == 1);
  CHECK(r.cpu_read("ADC1", "ADC_DR") == code_at(1'000'000));
  CHECK(r.isr("EOC") == 0);
  r.port().drive(3, 2'000'000);
  r.run_until(start + 5 * 15 * cycle_ps);
  CHECK(r.isr("EOC") == 1);
  CHECK(r.read("ADC1", "ADC_DR") == code_at(2'000'000));
  r.port().drive(3, 3'000'000);
  r.run_until(start + 7 * 15 * cycle_ps);
  CHECK(r.isr("OVR") == 1);
  CHECK(r.cpu_read("ADC1", "ADC_DR") == code_at(2'000'000));
  CHECK(r.isr("EOC") == 0);
}

TEST_CASE("OVRMOD 1 overwrites ADC_DR with the latest result and sets OVR")
{
  rig const r;
  r.enable();
  r.port().drive(3, 1'000'000);
  r.port().drive(4, 2'000'000);
  r.write("ADC1", "ADC_CFGR", ovrmod);
  r.write("ADC1", "ADC_SQR1", 1u | (3u << 6) | (4u << 12));
  static_cast<void>(r.convert(3));
  r.write("ADC1", "ADC_SQR1", 1u | (3u << 6) | (4u << 12));
  r.write("ADC1", "ADC_CR", advregen | aden | adstart);
  r.settle();
  CHECK(r.isr("OVR") == 1);
  CHECK(r.cpu_read("ADC1", "ADC_DR") == code_at(2'000'000));
  CHECK(r.isr("EOC") == 0);
}

TEST_CASE("DMAEN 1 DMACFG 1 feeds 16 results, the request stays on after the channel completes")
{
  rig const r;
  r.enable();
  r.port().drive(3, 1'200'000);
  r.write("ADC1", "ADC_CFGR", dmaen | dmacfg | cont | ovrmod);
  r.dma(64);
  static_cast<void>(r.start(3));
  r.run_until(r.now() + 17 * 15 * cycle_ps + 2 * us);
  CHECK(r.dma_left() == 0);
  CHECK(r.buffer_words(16) == std::vector<std::uint32_t>(16, code_at(1'200'000)));
  CHECK(r.get("ADC1", "ADC_CR", "ADSTART") == 1);
  CHECK(r.isr("OVR") == 1);
  r.port().drive(3, 2'400'000);
  r.run_until(r.now() + 2 * 15 * cycle_ps);
  r.dma(16);
  r.write("ADC1", "ADC_ISR", ovr);
  r.run_until(r.now() + 5 * 15 * cycle_ps + 2 * us);
  CHECK(r.dma_left() == 0);
  CHECK(r.buffer_words(4) == std::vector<std::uint32_t>(4, code_at(2'400'000)));
}

TEST_CASE(
    "H573 board: DMAEN 1 DMACFG 0 stops the ADC once the channel completes, until ADSTART 0 to 1")
{
  rig const r;
  r.enable();
  r.port().drive(3, 1'200'000);
  r.write("ADC1", "ADC_CFGR", dmaen | cont | ovrmod);
  r.dma(16);
  static_cast<void>(r.start(3));
  r.run_until(r.now() + 5 * 15 * cycle_ps + 2 * us);
  CHECK(r.dma_left() == 0);
  CHECK(r.get("ADC1", "ADC_CR", "ADSTART") == 0);
  auto const frozen = r.read("ADC1", "ADC_DR");
  r.port().drive(3, 2'400'000);
  r.run_until(r.now() + 5 * 15 * cycle_ps);
  CHECK(r.read("ADC1", "ADC_DR") == frozen);
  r.dma(16);
  r.write("ADC1", "ADC_ISR", ovr);
  r.run_until(r.now() + 5 * 15 * cycle_ps + 2 * us);
  CHECK(r.dma_left() == 16);
  CHECK(r.isr("EOC") == 0);
  r.write("ADC1", "ADC_CR", advregen | aden | adstp);
  static_cast<void>(r.start(3));
  r.run_until(r.now() + 5 * 15 * cycle_ps + 2 * us);
  CHECK(r.dma_left() == 0);
}

TEST_CASE("ADCC.CKMODE clocks the conversion from HCLK / 1, 2 or 4 and ignores PRESC")
{
  for(std::uint32_t mode = 0; mode < 4; ++mode)
  {
    CAPTURE(mode);
    rig const r;
    r.write("ADCC", "ADC_CCR", ckmode(mode) | presc(4));
    r.enable();
    r.port().drive(3, 1'000'000);
    constexpr std::array<std::uint64_t, 4> dividers{8, 1, 2, 4};
    r.ends_at(r.start(3) + 15 * dividers[mode] * cycle_ps);
  }
}

TEST_CASE("CKMODE 10 and 11 divide HCLK at 16 MHz while the kernel clock is SYSCLK at 32 MHz")
{
  struct setting
  {
    std::uint32_t mode;
    std::uint64_t end_ps;
  };
  for(auto const [mode, end_ps] : {
          setting{0, 3'750'000},
          setting{2, 1'875'000},
          setting{3, 3'750'000},
  })
  {
    CAPTURE(mode);
    rig const r;
    r.set("RCC", "RCC_CCIPR5", "ADCDACSEL", 1);
    r.set("RCC", "RCC_CFGR2", "HPRE", 0b1000);
    r.write("ADCC", "ADC_CCR", ckmode(mode) | presc(4));
    r.enable();
    r.port().drive(3, 1'000'000);
    auto const end = r.start(3) + end_ps;
    r.run_until(end - 4 * cycle_ps);
    CHECK(r.isr("EOC") == 0);
    r.run_until(end);
    CHECK(r.isr("EOC") == 1);
  }
}

TEST_CASE(
    "H573 board: a TIM15 trigger at CKMODE 00 waits for the next ADC clock edge, then tLATR 4.5 "
    "cycles; 2.5 at CKMODE 10, 2.25 at 11")
{
  struct setting
  {
    std::uint32_t mode;
    std::uint32_t prescaler;
    std::uint64_t divider;
    std::uint64_t latency_quarters;
    std::uint64_t offset_ps;
  };
  for(auto const [mode, prescaler, divider, quarters, offset] : {
          setting{0, 0,  1,   18, 0      },
          setting{0, 1,  2,   18, half_ps},
          setting{2, 0,  2,   10, 0      },
          setting{3, 0,  4,   9,  0      },
          setting{0, 10, 128, 18, us     },
          setting{0, 11, 256, 18, 0      },
          setting{0, 11, 256, 18, 3 * us },
  })
  {
    CAPTURE(mode);
    CAPTURE(divider);
    CAPTURE(offset);
    rig const r;
    r.write("ADCC", "ADC_CCR", ckmode(mode) | presc(prescaler));
    r.enable();
    r.port().drive(3, 1'000'000);
    r.write("ADC1", "ADC_CFGR", (1u << 10) | (14u << 5));
    if(offset != 0)
    {
      r.run_until(r.now() + offset);
    }
    auto const start   = r.tim15_trgo();
    auto const trigger = start + ms;
    auto const period  = cycle_ps * divider;
    auto const edge    = mode == 0 ? (trigger + period - 1) / period * period - trigger : 0;
    static_cast<void>(r.start(3));
    auto const end = trigger + edge + (quarters + 4 * 15) * cycle_ps * divider / 4;
    r.ends_at(end);
    CHECK(r.get("ADC1", "ADC_CR", "ADSTART") == 1);
  }
}

TEST_CASE("EXTSEL 20 and 21 are reserved on the H5: one warning each, no conversion")
{
  rig const r;
  r.enable();
  r.write("ADC1", "ADC_CFGR", (1u << 10) | (20u << 5));
  static_cast<void>(r.start(3));
  r.write("ADC1", "ADC_CR", advregen | aden | adstp);
  r.write("ADC1", "ADC_CFGR", (1u << 10) | (21u << 5));
  static_cast<void>(r.start(3));
  r.settle();
  auto const logged = r.warnings();
  REQUIRE(logged.size() == 2);
  CHECK(logged[0].message.contains("EXTSEL 20: reserved trigger selection"));
  CHECK(logged[1].message.contains("EXTSEL 21: reserved trigger selection"));
  CHECK(r.isr("EOC") == 0);
}

TEST_CASE("AWD1 compares the 12-bit code with LT1 and HT1, AWD2 its 8 MSBs with LT2 and HT2")
{
  rig const r;
  r.enable();
  r.write("ADC1", "ADC_TR1", (3000u << 16) | 1000u);
  r.write("ADC1", "ADC_TR2", 128u << 16);
  r.write("ADC1", "ADC_AWD2CR", 1u << 3);
  r.write("ADC1", "ADC_CFGR", awd1en | awd1sgl | (3u << 26));
  struct row
  {
    std::uint32_t code;
    std::uint32_t awd1;
    std::uint32_t awd2;
  };
  for(auto const [code, awd1, awd2] : {
          row{3000, 0, 1},
          row{3001, 1, 1},
          row{2063, 0, 0},
          row{2064, 0, 1},
          row{999,  1, 0},
  })
  {
    CAPTURE(code);
    r.port().drive(3, (code * 3'300'000ull + 4095) / 4096);
    REQUIRE(r.convert(3) == code);
    CHECK(r.isr("AWD1") == awd1);
    CHECK(r.isr("AWD2") == awd2);
    r.write("ADC1", "ADC_ISR", (1u << 7) | (1u << 8));
  }
}

TEST_CASE("with ROVSE the watchdogs compare ADC_DR[15:4] for AWD1 and ADC_DR[15:8] for AWD2")
{
  rig const r;
  r.enable();
  r.port().drive(3, 1'650'000);
  r.write("ADC1", "ADC_CFGR2", rovse | ovsr(0));
  r.write("ADC1", "ADC_AWD2CR", 1u << 3);
  r.write("ADC1", "ADC_CFGR", awd1en);
  r.write("ADC1", "ADC_TR1", 256u << 16);
  r.write("ADC1", "ADC_TR2", 16u << 16);
  CHECK(r.convert(3) == 4096);
  CHECK(r.isr("AWD1") == 0);
  CHECK(r.isr("AWD2") == 0);
  r.write("ADC1", "ADC_TR1", 255u << 16);
  r.write("ADC1", "ADC_TR2", 15u << 16);
  CHECK(r.convert(3) == 4096);
  CHECK(r.isr("AWD1") == 1);
  CHECK(r.isr("AWD2") == 1);
}

TEST_CASE("AWDFILT n in ADC_TR1 flags from the n + 1-th consecutive out-of-window result")
{
  rig const r;
  r.enable();
  r.port().drive(3, 3'000'000);
  r.write("ADC1", "ADC_TR1", (1000u << 16) | (2u << 12));
  r.write("ADC1", "ADC_CFGR", awd1en);
  CHECK(r.convert(3) > 1000);
  CHECK(r.isr("AWD1") == 0);
  static_cast<void>(r.convert(3));
  CHECK(r.isr("AWD1") == 0);
  static_cast<void>(r.convert(3));
  CHECK(r.isr("AWD1") == 1);
}

TEST_CASE("OVSR n sums 2 to the n + 1 codes in as many conversions, truncated to 16 bits")
{
  rig const r;
  r.enable();
  SUBCASE("OVSR 1, 4x, no shift")
  {
    r.port().drive(3, 1'650'000);
    r.write("ADC1", "ADC_CFGR2", rovse | ovsr(1));
    auto const start = r.start(3);
    r.ends_at(start + 4 * 15 * cycle_ps);
    CHECK(r.cpu_read("ADC1", "ADC_DR") == 4 * 2048);
  }
  SUBCASE("OVSR 2, 8x, OVSS 3")
  {
    r.port().drive(3, 1'650'000);
    r.write("ADC1", "ADC_CFGR2", rovse | ovsr(2) | (3u << 5));
    CHECK(r.convert(3) == 2048);
  }
  SUBCASE("OVSR 7, 256x full scale: 0xFFF00 truncated to 0xFF00, AWD1 compares 0xFF0")
  {
    r.port().drive(3, 3'300'000);
    r.write("ADC1", "ADC_CFGR2", rovse | ovsr(7));
    r.write("ADC1", "ADC_CFGR", awd1en);
    r.write("ADC1", "ADC_TR1", 0xFF0u << 16);
    static_cast<void>(r.start(3));
    r.run_until(r.now() + 256 * 15 * cycle_ps);
    CHECK(r.read("ADC1", "ADC_DR") == 0xFF00);
    CHECK(r.isr("AWD1") == 0);
  }
}

TEST_CASE(
    "ALIGN 1 left-aligns ADC_DR, a 6-bit code on the byte, and is ignored with ROVSE or JOVSE")
{
  rig const r;
  r.enable();
  r.port().drive(3, 1'650'000);
  r.write("ADC1", "ADC_CFGR", align);
  CHECK(r.convert(3) == 0x8000);
  r.write("ADC1", "ADC_CFGR", align | res(1));
  CHECK(r.convert(3) == 0x8000);
  r.write("ADC1", "ADC_CFGR", align | res(3));
  CHECK(r.convert(3) == 32u << 2);
  r.write("ADC1", "ADC_CFGR", align);
  r.write("ADC1", "ADC_CFGR2", rovse | ovsr(0) | (1u << 5));
  CHECK(r.convert(3) == 2048);
  r.write("ADC1", "ADC_CFGR2", jovse);
  CHECK(r.convert(3) == 2048);
}

TEST_CASE("ADC1 reads TS_CAL1 on channel 16 with TSEN and VREFINT_CAL on 17 with VREFEN")
{
  rig const r;
  r.enable();
  CHECK(r.convert(16) == 0);
  CHECK(r.convert(17) == 0);
  r.write("ADCC", "ADC_CCR", vrefen | tsen);
  auto const sensor = r.convert(16);
  CHECK(sensor == ts_cal1);
  CHECK(r.convert(17) == vrefint_cal);
  auto const celsius = static_cast<std::int32_t>(sensor - ts_cal1)
                         * (130 - 30)
                         / static_cast<std::int32_t>(ts_cal2 - ts_cal1)
                     + 30;
  CHECK(celsius == 30);
}

TEST_CASE("ADC2 channels 16 and 17 read 0 with TSEN and VREFEN, its external channels convert")
{
  rig const r;
  r.enable("ADC2");
  r.write("ADCC", "ADC_CCR", vrefen | tsen);
  CHECK(r.convert(16, "ADC2") == 0);
  CHECK(r.convert(17, "ADC2") == 0);
  r.port("ADC2").drive(18, 1'650'000);
  CHECK(r.convert(18, "ADC2") == 2048);
  CHECK(r.port("ADC1").input(18) == 0);
}

TEST_CASE("the external channels are 0 to 15, 18 and 19")
{
  rig const r;
  r.enable();
  r.write("ADC1", "ADC_OR", 1);
  for(std::uint8_t channel : {0, 15, 18, 19})
  {
    CAPTURE(channel);
    r.port().drive(channel, 1'650'000);
    CHECK(r.convert(channel) == 2048);
  }
  for(std::uint8_t channel : {16, 17, 20})
  {
    CAPTURE(channel);
    CHECK_THROWS_AS(r.port().drive(channel, 1'000'000), popcorn::config_error);
  }
}

TEST_CASE("ADC1 and ADC2 convert at once, each sets its own flags and IRQ")
{
  rig const r;
  r.enable("ADC1");
  r.enable("ADC2");
  r.port("ADC1").drive(3, 1'000'000);
  r.port("ADC2").drive(3, 2'000'000);
  r.write("ADC1", "ADC_IER", eoc);
  r.write("ADC2", "ADC_IER", eoc);
  r.write("ADC2", "ADC_SMPR1", 1u << 9);
  auto const start = r.start(3, "ADC1");
  static_cast<void>(r.start(3, "ADC2"));
  r.run_until(start + 15 * cycle_ps);
  CHECK(r.isr("EOC", "ADC1") == 1);
  CHECK(r.isr("EOC", "ADC2") == 0);
  CHECK(r.pending(adc1_irq));
  CHECK_FALSE(r.pending(adc2_irq));
  r.run_until(start + 19 * cycle_ps);
  CHECK(r.isr("EOC", "ADC2") == 1);
  CHECK(r.pending(adc2_irq));
  CHECK(r.cpu_read("ADC1", "ADC_DR") == code_at(1'000'000));
  CHECK(r.cpu_read("ADC2", "ADC_DR") == code_at(2'000'000));
}

TEST_CASE("ADC12RST resets ADC1, ADC2 and ADCC, the stale end of a dropped conversion does nothing")
{
  rig const r;
  r.write("ADCC", "ADC_CCR", vrefen | presc(1));
  r.enable("ADC1");
  r.enable("ADC2");
  r.port("ADC1").drive(3, 1'000'000);
  r.write("ADC1", "ADC_SMPR1", 7u << 9);
  auto const first = r.start(3, "ADC1");
  static_cast<void>(r.start(3, "ADC2"));
  r.run_until(first + 10 * cycle_ps);
  rcc_reset_pulse(*r.f, "RCC_AHB2RSTR", "ADC12RST");
  CHECK(r.read("ADC1", "ADC_CR") == deeppwd);
  CHECK(r.read("ADC2", "ADC_CR") == deeppwd);
  CHECK(r.read("ADCC", "ADC_CCR") == 0);
  r.port("ADC1").drive(3, 2'000'000);
  r.enable("ADC1");
  auto const second = r.start(3, "ADC1");
  r.ends_at(second + 15 * cycle_ps);
  CHECK(r.cpu_read("ADC1", "ADC_DR") == code_at(2'000'000));
  r.run_until(first + 2 * 653 * cycle_ps + us);
  CHECK(r.isr("EOC") == 0);
  CHECK(r.isr("EOC", "ADC2") == 0);
}

TEST_CASE(
    "ADC12RST drops a conversion waiting for its kernel clock, the clock's return converts none")
{
  rig const r;
  r.set("RCC", "RCC_CCIPR5", "ADCDACSEL", 3);
  r.enable();
  r.port().drive(3, 1'000'000);
  static_cast<void>(r.start(3));
  r.settle();
  REQUIRE(r.isr("EOC") == 0);
  rcc_reset_pulse(*r.f, "RCC_AHB2RSTR", "ADC12RST");
  r.set("RCC", "RCC_CCIPR5", "ADCDACSEL", 0);
  r.settle();
  CHECK(r.isr("EOC") == 0);
  CHECK(r.read("ADC1", "ADC_DR") == 0);
}

TEST_CASE("ADC12RST during a DISCEN subgroup restarts the sequence at rank 1")
{
  rig const r;
  r.port().drive(3, 1'000'000);
  r.port().drive(4, 2'000'000);
  r.port().drive(5, 3'000'000);
  auto const scan = [&] {
    r.set("ADC1", "ADC_CFGR", "DISCEN", 1);
    r.write("ADC1", "ADC_SQR1", 2u | (3u << 6) | (4u << 12) | (5u << 18));
    r.write("ADC1", "ADC_SMPR1", 7u << 12);
    r.enable();
    r.write("ADC1", "ADC_CR", advregen | aden | adstart);
  };
  scan();
  r.settle();
  REQUIRE(r.cpu_read("ADC1", "ADC_DR") == code_at(1'000'000));
  r.write("ADC1", "ADC_CR", advregen | aden | adstart);
  r.run_until(r.now() + 5 * us);
  REQUIRE(r.isr("EOC") == 0);
  rcc_reset_pulse(*r.f, "RCC_AHB2RSTR", "ADC12RST");
  scan();
  r.settle();
  CHECK(r.cpu_read("ADC1", "ADC_DR") == code_at(1'000'000));
}

TEST_CASE("a DUAL write other than 0 logs once, a model_error when strict")
{
  {
    rig const r;
    r.write("ADCC", "ADC_CCR", 6u);
    r.write("ADCC", "ADC_CCR", 1u);
    auto const logged = r.warnings();
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].peripheral == "ADCC");
    CHECK(logged[0].message.contains("DUAL"));
    r.write("ADCC", "ADC_CCR", 0u);
  }
  rig const r{true};
  CHECK_NOTHROW(r.write("ADCC", "ADC_CCR", tsen));
  CHECK_THROWS_AS(
      static_cast<void>(r.f->target->mem().write(r.at("ADCC", "ADC_CCR"), 4, 6u)),
      popcorn::model_error);
}

TEST_CASE("an ADC clock outside 1.5 to 75 MHz logs a warning at the ADEN edge, 2 MHz none")
{
  {
    rig const r;
    r.write("ADCC", "ADC_CCR", presc(7));
    r.enable();
    CHECK(r.warnings().empty());
  }
  rig const r;
  r.write("ADCC", "ADC_CCR", presc(8));
  r.enable();
  auto const logged = r.warnings();
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].message.contains("outside 1.5 to 75 MHz"));
}

TEST_CASE(
    "OFFSET_EN on the converted channel logs once, an OFFSET without OFFSET_EN or with ROVSE or "
    "JOVSE none")
{
  rig const r;
  r.enable();
  r.write("ADC1", "ADC_OFR1", (3u << 26) | 100u);
  static_cast<void>(r.convert(3));
  CHECK(r.warnings().empty());
  r.write("ADC1", "ADC_OFR1", (1u << 31) | (3u << 26) | 100u);
  r.write("ADC1", "ADC_CFGR2", rovse);
  static_cast<void>(r.convert(3));
  CHECK(r.warnings().empty());
  r.write("ADC1", "ADC_CFGR2", jovse);
  static_cast<void>(r.convert(3));
  CHECK(r.warnings().empty());
  r.write("ADC1", "ADC_CFGR2", 0);
  static_cast<void>(r.convert(3));
  auto const logged = r.warnings();
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].message.contains("OFFSET"));
}

TEST_CASE(
    "channel 0 with ADC1 ADC_OR.OP0 clear logs once on ADC1 and ADC2, a model_error when strict")
{
  {
    rig const r;
    r.enable("ADC1");
    r.enable("ADC2");
    r.port("ADC1").drive(0, 1'650'000);
    r.write("ADC1", "ADC_OR", 1);
    CHECK(r.convert(0, "ADC1") == 2048);
    CHECK(r.convert(0, "ADC2") == 0);
    CHECK(r.warnings().empty());
    r.write("ADC1", "ADC_OR", 0);
    CHECK(r.convert(0, "ADC1") == 0);
    CHECK(r.convert(0, "ADC1") == 0);
    auto const logged = r.warnings();
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].peripheral == "ADC1");
    CHECK(logged[0].reg == "ADC_OR");
    CHECK(logged[0].message.contains("ADC1 ADC_OR.OP0 clear"));
  }
  {
    rig const r;
    r.enable("ADC2");
    r.write("ADC2", "ADC_OR", 1);
    static_cast<void>(r.convert(0, "ADC2"));
    auto const logged = r.warnings();
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].peripheral == "ADC2");
  }
  rig const r{true};
  r.enable("ADC1");
  r.enable("ADC2");
  r.write("ADC1", "ADC_OR", 1);
  CHECK_NOTHROW(static_cast<void>(r.convert(0, "ADC2")));
  r.write("ADC1", "ADC_OR", 0);
  r.write("ADC2", "ADC_SQR1", 0);
  CHECK_THROWS_AS(
      static_cast<void>(
          r.f->target->mem().write(r.at("ADC2", "ADC_CR"), 4, advregen | aden | adstart)),
      popcorn::model_error);
}

TEST_CASE("BULB, SMPTRIG, TROVS and the DIFSEL bit of a converted channel each log once on the H5")
{
  constexpr std::uint32_t trovs   = 1u << 9;
  constexpr std::uint32_t bulb    = 1u << 26;
  constexpr std::uint32_t smptrig = 1u << 27;
  {
    rig const r;
    r.enable();
    r.port().drive(3, 1'650'000);
    r.write("ADC1", "ADC_DIFSEL", 1u << 4);
    CHECK(r.convert(3) == 2048);
    CHECK(r.warnings().empty());
    r.write("ADC1", "ADC_CFGR2", bulb | smptrig | trovs);
    r.write("ADC1", "ADC_DIFSEL", 1u << 3);
    CHECK(r.convert(3) == 2048);
    CHECK(r.convert(3) == 2048);
    auto const logged = r.warnings();
    REQUIRE(logged.size() == 4);
    CHECK(logged[0].message.contains("BULB"));
    CHECK(logged[1].message.contains("SMPTRIG"));
    CHECK(logged[2].message.contains("TROVS"));
    CHECK(logged[3].message.contains("DIFSEL"));
  }
  struct setting
  {
    std::string_view reg;
    std::uint32_t    value;
  };
  for(auto const [name, value] : {
          setting{"ADC_CFGR2",  bulb   },
          setting{"ADC_CFGR2",  smptrig},
          setting{"ADC_CFGR2",  trovs  },
          setting{"ADC_DIFSEL", 1u << 3},
  })
  {
    CAPTURE(value);
    rig const r{true};
    r.enable();
    r.write("ADC1", name, value);
    r.write("ADC1", "ADC_SQR1", 3u << 6);
    CHECK_THROWS_AS(
        static_cast<void>(
            r.f->target->mem().write(r.at("ADC1", "ADC_CR"), 4, advregen | aden | adstart)),
        popcorn::model_error);
  }
}

TEST_CASE("CKMODE 01 with HCLK not SYSCLK logs once at the ADEN edge, a model_error when strict")
{
  {
    rig const r;
    r.write("ADCC", "ADC_CCR", ckmode(1));
    r.enable();
    CHECK(r.warnings().empty());
  }
  {
    rig const r;
    r.set("RCC", "RCC_CFGR2", "HPRE", 0b1000);
    r.write("ADCC", "ADC_CCR", ckmode(1));
    r.enable();
    r.write("ADC1", "ADC_CR", advregen | (1u << 1));
    r.enable();
    auto const logged = r.warnings();
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].message.contains("CKMODE 01"));
    CHECK(logged[0].message.contains("RM0481 28.7.2"));
  }
  rig const r{true};
  r.set("RCC", "RCC_CFGR2", "HPRE", 0b1000);
  r.write("ADCC", "ADC_CCR", ckmode(1));
  r.write("ADC1", "ADC_CR", advregen);
  CHECK_THROWS_AS(
      static_cast<void>(r.f->target->mem().write(r.at("ADC1", "ADC_CR"), 4, advregen | aden)),
      popcorn::model_error);
}

TEST_CASE("ADC_CSR reads ADC1 ADC_ISR bits 10:0 at 10:0 and ADC2's at 26:16")
{
  rig const r;
  CHECK(r.read("ADCC", "ADC_CSR") == 0);
  r.enable("ADC1");
  r.run_until(r.now() + 35 * cycle_ps);
  CHECK(r.read("ADCC", "ADC_CSR") == 1u);
  r.enable("ADC2");
  r.run_until(r.now() + 35 * cycle_ps);
  CHECK(r.read("ADCC", "ADC_CSR") == (1u | 1u << 16));
  r.port("ADC2").drive(3, 1'000'000);
  static_cast<void>(r.start(3, "ADC2"));
  r.settle();
  auto const flags = r.read("ADC2", "ADC_ISR");
  CHECK((flags & eoc) != 0);
  CHECK(r.read("ADCC", "ADC_CSR") == (1u | (flags & 0x7FFu) << 16));
  r.write("ADC2", "ADC_ISR", 0x7FE);
  CHECK(r.read("ADCC", "ADC_CSR") == (1u | 1u << 16));
}
