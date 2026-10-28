#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/usb.hpp>

#include <doctest/doctest.h>
#include <reflex/serde/bson.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace core = popcorn::core;
namespace usb  = popcorn::periph::usb;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::stm32_fixture;

constexpr std::uint32_t cr          = 0x0;
constexpr std::uint32_t cfgr        = 0x4;
constexpr std::uint32_t isr         = 0x8;
constexpr std::uint32_t icr         = 0xC;
constexpr std::uint32_t syncokie    = 1u << 0;
constexpr std::uint32_t errie       = 1u << 2;
constexpr std::uint32_t esyncie     = 1u << 3;
constexpr std::uint32_t cen         = 1u << 5;
constexpr std::uint32_t autotrimen  = 1u << 6;
constexpr std::uint32_t swsync      = 1u << 7;
constexpr std::uint32_t syncokf     = 1u << 0;
constexpr std::uint32_t syncwarnf   = 1u << 1;
constexpr std::uint32_t errf        = 1u << 2;
constexpr std::uint32_t esyncf      = 1u << 3;
constexpr std::uint32_t syncokc     = 1u << 0;
constexpr std::uint32_t esyncc      = 1u << 3;
constexpr std::uint32_t errc        = 1u << 2;
constexpr std::uint32_t trimovf     = 1u << 10;
constexpr std::uint32_t syncmiss    = 1u << 9;
constexpr std::uint32_t syncerr     = 1u << 8;
constexpr std::uint32_t flag_mask   = 0x070F;
constexpr std::uint32_t source_usb  = 2u << 28;
constexpr std::uint32_t source_lse  = 1u << 28;
constexpr std::uint32_t matched     = 47'999;
constexpr std::uint32_t felim       = 34;
constexpr std::uint32_t outrange    = 128 * felim;
constexpr std::uint64_t ms          = 1'000'000'000;
constexpr std::uint64_t miss_after  = 90'666'667;
constexpr std::uint64_t microsecond = 1'000'000;

struct recording_host final : usb::host_side
{
  void pull_up_changed(bool, popcorn::vtime) override
  {}
  void on_time(popcorn::vtime) override
  {}
  void save(std::vector<std::byte>&) const override
  {}
  void load(std::span<std::byte const>) override
  {}
};

struct crs_state_mirror
{
  std::uint64_t sync_ps{};
  std::uint64_t generation{};
  std::uint64_t queued{};
  std::uint8_t  frames{};
  std::uint8_t  phase{};
};

struct u5_family
{
  static constexpr std::string_view firmware   = "rcc_ll";
  static constexpr std::string_view usb_name   = "OTG_FS";
  static constexpr std::uint32_t    trim_reset = 64u << 8;
  static constexpr std::string_view reset_reg  = "RCC_APB1RSTR1";

  static auto part() -> popcorn::part_description const&
  {
    return popcorn::testing::u575();
  }
};

struct h5_family
{
  static constexpr std::string_view firmware   = "h573_rcc_ll";
  static constexpr std::string_view usb_name   = "USB";
  static constexpr std::uint32_t    trim_reset = 32u << 8;
  static constexpr std::string_view reset_reg  = "RCC_APB1LRSTR";

  static auto part() -> popcorn::part_description const&
  {
    return popcorn::testing::h573();
  }
};

template <typename Family> struct rig
{
  rig() : f{Family::part(), Family::firmware}
  {
    f.set("pc", popcorn::testing::sym(Family::firmware, "idle"));
    sink = f.target->add_log_sink([this](core::log_record const& r) { logged.push_back(r); });
  }

  [[nodiscard]] auto base(std::string_view name) const -> std::uint32_t
  {
    return f.target->peripheral(name).description().base;
  }

  [[nodiscard]] auto read(std::uint32_t offset) const -> std::uint32_t
  {
    auto const value = f.target->mem().read(base("CRS") + offset, 4);
    REQUIRE(value.has_value());
    return *value;
  }

  void write(std::uint32_t offset, std::uint32_t value) const
  {
    REQUIRE(f.target->mem().write(base("CRS") + offset, 4, value).has_value());
  }

  [[nodiscard]] auto flags() const -> std::uint32_t
  {
    return read(isr) & flag_mask;
  }

  [[nodiscard]] auto capture() const -> std::uint32_t
  {
    return read(isr) >> 16;
  }

  [[nodiscard]] auto direction() const -> std::uint32_t
  {
    return (read(isr) >> 15) & 1u;
  }

  [[nodiscard]] auto line() const -> std::uint8_t
  {
    auto const  saved  = f.target->snapshot();
    auto const& images = saved.data().peripherals;
    auto const  image =
        std::ranges::find(images, std::string{"CRS"}, &core::peripheral_image::peripheral);
    REQUIRE(image != images.end());
    REQUIRE(image->irq_levels.size() == 1);
    return image->irq_levels[0];
  }

  [[nodiscard]] auto state() const -> crs_state_mirror
  {
    std::vector<std::byte> saved;
    f.target->model("CRS")->save(saved);
    auto const state = reflex::serde::bson::deserializer{
        std::span<std::byte const>{
            saved}}.template load<crs_state_mirror>();
    std::vector<std::byte> redumped;
    reflex::serde::bson::serializer{redumped}.dump(state);
    REQUIRE(redumped == saved);
    return state;
  }

  [[nodiscard]] auto frames() const -> std::uint8_t
  {
    return state().frames;
  }

  [[nodiscard]] auto pending() const -> std::vector<std::uint64_t>
  {
    auto const& units = f.target->description().peripherals;
    auto const  found =
        std::ranges::find(units, std::string_view{"CRS"}, &popcorn::peripheral_desc::name);
    REQUIRE(found != units.end());
    auto const                 owner = static_cast<std::uint32_t>(found - units.begin());
    std::vector<std::uint64_t> due;
    for(auto const& event : f.target->snapshot().data().events.pending)
    {
      if(event.id.owner == owner)
      {
        due.push_back(event.due_ps);
      }
    }
    return due;
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
        {.max_instructions = icount{1'000'000},
         .wall_budget      = std::chrono::seconds{60},
         .until_time       = vtime{ps}});
    REQUIRE(stopped.stopped.has_value());
    REQUIRE(stopped.stopped->reason == stop_reason::time);
    REQUIRE(now() == ps);
  }

  void run_past(std::uint64_t ps) const
  {
    REQUIRE(ps > now());
    auto const stopped = f.run(
        {.max_instructions = icount{1'000'000},
         .wall_budget      = std::chrono::seconds{60},
         .until_time       = vtime{ps}});
    REQUIRE(stopped.stopped.has_value());
    REQUIRE(stopped.stopped->reason == stop_reason::time);
    REQUIRE(now() >= ps);
    REQUIRE(now() - ps < microsecond);
  }

  void run_for(std::uint64_t ps) const
  {
    run_until(now() + ps);
  }

  void set_field(
      std::string_view peripheral,
      std::string_view reg,
      std::string_view field,
      std::uint32_t    value) const
  {
    auto const& desc  = f.target->peripheral(peripheral).description();
    auto const* found = popcorn::find_register(desc, reg);
    REQUIRE(found != nullptr);
    auto const bits = std::ranges::find(found->fields, field, &popcorn::field_desc::name);
    REQUIRE(bits != found->fields.end());
    auto const mask = ((std::uint32_t{1} << bits->width) - 1u) << bits->lsb;
    auto const at   = desc.base + found->offset;
    auto const old  = f.target->mem().read(at, 4);
    REQUIRE(old.has_value());
    REQUIRE(f.target->mem().write(at, 4, (*old & ~mask) | ((value << bits->lsb) & mask)));
  }

  auto port() const -> usb::device_port&
  {
    auto* const found = f.template model_of<usb::device_port>(Family::usb_name);
    REQUIRE(found != nullptr);
    return *found;
  }

  void connect() const
  {
    if constexpr(Family::usb_name == std::string_view{"OTG_FS"})
    {
      set_field("OTG_FS", "GCCFG", "PWRDWN", 1);
      set_field("OTG_FS", "GOTGCTL", "BVALOEN", 1);
      set_field("OTG_FS", "GOTGCTL", "BVALOVAL", 1);
      port().attach(host);
      set_field("OTG_FS", "DCTL", "SDIS", 0);
    }
    else
    {
      set_field("USB", "CNTR", "USBRST", 0);
      set_field("USB", "CNTR", "PDWN", 0);
      set_field("USB", "DADDR", "EF", 1);
      port().attach(host);
      set_field("USB", "BCDR", "DPPU_DPD", 1);
    }
    REQUIRE(port().pulled_up());
  }

  void sof(std::uint16_t number = 1) const
  {
    port().start_of_frame(number, f.target->virtual_clock().time());
  }

  void configure(
      std::uint32_t reload,
      std::uint32_t divider = 0,
      std::uint32_t limit   = felim,
      std::uint32_t source  = source_usb) const
  {
    write(cr, read(cr) & ~cen);
    write(cfgr, reload | (limit << 16) | (divider << 24) | source);
    write(cr, read(cr) | cen);
  }

  void enable_usb_sync(std::uint32_t divider = 0) const
  {
    configure(matched, divider);
  }

  void software_sync() const
  {
    write(cr, read(cr) | cen | swsync);
  }

  void two_syncs(std::uint32_t reload, std::uint64_t interval = ms) const
  {
    configure(reload);
    software_sync();
    run_for(interval);
    software_sync();
  }

  stm32_fixture                   f;
  std::shared_ptr<recording_host> host = std::make_shared<recording_host>();
  std::vector<core::log_record>   logged;
  core::hook_handle               sink;
};
}

TEST_CASE_TEMPLATE("CRS registers read their reset values and log nothing", F, u5_family, h5_family)
{
  rig<F> const r;
  CHECK(r.read(cr) == F::trim_reset);
  CHECK(r.read(cfgr) == 0x2022'BB7F);
  CHECK(r.read(isr) == 0);
  CHECK(r.read(icr) == 0);
  CHECK(r.logged.empty());
  REQUIRE(r.f.target->model("CRS") != nullptr);
}

TEST_CASE_TEMPLATE("CFGR ignores writes while CEN is set", F, u5_family, h5_family)
{
  rig<F> const r;
  r.write(cr, cen);
  r.write(cfgr, 0x1234);
  CHECK(r.read(cfgr) == 0x2022'BB7F);
  r.write(cr, 0);
  r.write(cfgr, 0x1234);
  CHECK(r.read(cfgr) == 0x1234);
}

TEST_CASE_TEMPLATE("TRIM is read-only while AUTOTRIMEN is set", F, u5_family, h5_family)
{
  rig<F> const r;
  r.write(cr, autotrimen | (5u << 8));
  CHECK(r.read(cr) == (autotrimen | (5u << 8)));
  r.write(cr, autotrimen | (9u << 8));
  CHECK(r.read(cr) == (autotrimen | (5u << 8)));
  r.write(cr, 0);
  r.write(cr, 9u << 8);
  CHECK(r.read(cr) == (9u << 8));
}

TEST_CASE_TEMPLATE(
    "a lone SWSYNC sets ESYNCF after RELOAD + 1 cycles, then SYNCMISS and ERRF",
    F,
    u5_family,
    h5_family)
{
  rig<F> const r;
  r.write(cr, cen | errie | swsync);
  auto const t0 = r.now();
  CHECK((r.read(cr) & swsync) == 0);
  CHECK(r.read(isr) == 0);
  CHECK(r.pending() == std::vector<std::uint64_t>{t0 + ms});
  r.run_until(t0 + ms - microsecond);
  CHECK(r.flags() == 0);
  r.run_until(t0 + ms);
  CHECK(r.flags() == esyncf);
  CHECK(r.line() == 0);
  CHECK(r.pending() == std::vector<std::uint64_t>{t0 + ms + miss_after});
  r.run_past(t0 + ms + miss_after - microsecond);
  CHECK(r.flags() == esyncf);
  CHECK(r.line() == 0);
  r.run_past(t0 + ms + miss_after);
  CHECK(r.line() == 1);
  CHECK(r.flags() == (esyncf | syncmiss | errf));
  CHECK(r.pending().empty());
  CHECK(r.capture() == 0);
}

TEST_CASE_TEMPLATE("SWSYNC without CEN starts no counter", F, u5_family, h5_family)
{
  rig<F> const r;
  r.write(cr, swsync);
  CHECK((r.read(cr) & swsync) == 0);
  CHECK(r.pending().empty());
  r.run_for(ms);
  r.write(cr, swsync);
  CHECK(r.read(isr) == 0);
  CHECK(r.pending().empty());
}

TEST_CASE_TEMPLATE(
    "a matched RELOAD gives SYNCOKF and ESYNCF with FECAP 0 at each SYNC",
    F,
    u5_family,
    h5_family)
{
  rig<F> const r;
  r.two_syncs(matched);
  CHECK(r.flags() == (syncokf | esyncf));
  CHECK(r.capture() == 0);
  CHECK(r.direction() == 0);
  CHECK(r.pending() == std::vector<std::uint64_t>{r.now() + ms});
}

TEST_CASE_TEMPLATE(
    "an error below 3 x FELIM gives SYNCOKF, at 3 x FELIM SYNCWARNF",
    F,
    u5_family,
    h5_family)
{
  SUBCASE("down by 3 x FELIM - 1")
  {
    rig<F> const r;
    r.two_syncs(matched + 3 * felim - 1);
    CHECK(r.flags() == syncokf);
    CHECK(r.capture() == 3 * felim - 1);
    CHECK(r.direction() == 1);
  }
  SUBCASE("down by 3 x FELIM")
  {
    rig<F> const r;
    r.two_syncs(matched + 3 * felim);
    CHECK(r.flags() == syncwarnf);
    CHECK(r.capture() == 3 * felim);
    CHECK(r.direction() == 1);
  }
  SUBCASE("up by 3 x FELIM")
  {
    rig<F> const r;
    r.two_syncs(matched - 3 * felim);
    CHECK(r.flags() == (syncwarnf | esyncf));
    CHECK(r.capture() == 3 * felim);
    CHECK(r.direction() == 0);
  }
}

TEST_CASE_TEMPLATE(
    "a SYNC before ESYNC with an error of 128 x FELIM gives SYNCERR and ERRF",
    F,
    u5_family,
    h5_family)
{
  SUBCASE("128 x FELIM - 1 is a warning")
  {
    rig<F> const r;
    r.two_syncs(matched + outrange - 1);
    CHECK(r.flags() == syncwarnf);
    CHECK(r.capture() == outrange - 1);
  }
  SUBCASE("128 x FELIM is an error")
  {
    rig<F> const r;
    r.write(cr, errie);
    r.two_syncs(matched + outrange);
    CHECK(r.flags() == (syncerr | errf));
    CHECK(r.capture() == outrange);
    CHECK(r.direction() == 1);
    CHECK(r.line() == 1);
  }
}

TEST_CASE_TEMPLATE(
    "the counter stops at 128 x FELIM after ESYNC with SYNCMISS and ERRF",
    F,
    u5_family,
    h5_family)
{
  SUBCASE("128 x FELIM - 1 up is a warning")
  {
    rig<F> const r;
    r.two_syncs(matched - outrange + 1);
    CHECK(r.flags() == (syncwarnf | esyncf));
    CHECK(r.capture() == outrange - 1);
  }
  SUBCASE("128 x FELIM up is a miss")
  {
    rig<F> const r;
    r.two_syncs(matched - outrange);
    CHECK(r.flags() == (syncmiss | errf | esyncf));
    CHECK(r.capture() == outrange);
    CHECK(r.direction() == 0);
  }
  SUBCASE("a SYNC after the stop latches 128 x FELIM")
  {
    rig<F> const r;
    r.two_syncs(matched, 2 * ms);
    CHECK(r.flags() == (syncmiss | errf | esyncf));
    CHECK(r.capture() == outrange);
    CHECK(r.direction() == 0);
    CHECK(r.pending() == std::vector<std::uint64_t>{r.now() + ms});
  }
}

TEST_CASE_TEMPLATE("an ICR bit clears its flag alone and ICR reads 0", F, u5_family, h5_family)
{
  rig<F> const r;
  r.two_syncs(matched);
  r.write(icr, syncokc);
  CHECK(r.flags() == esyncf);
  CHECK(r.read(icr) == 0);
  r.write(icr, esyncc);
  CHECK(r.flags() == 0);
}

TEST_CASE_TEMPLATE("ERRC clears ERRF, TRIMOVF, SYNCMISS and SYNCERR", F, u5_family, h5_family)
{
  rig<F> const r;
  auto&        regs = r.f.target->peripheral("CRS");
  regs.poke(isr, errf | trimovf | syncmiss | syncerr | syncokf);
  r.write(icr, errc);
  CHECK(r.read(isr) == syncokf);
}

TEST_CASE_TEMPLATE("the IRQ line is the OR of each flag and its enable", F, u5_family, h5_family)
{
  rig<F> const r;
  r.two_syncs(matched);
  CHECK(r.line() == 0);
  r.write(cr, cen | syncokie);
  CHECK(r.line() == 1);
  r.write(cr, cen);
  CHECK(r.line() == 0);
  r.write(cr, cen | esyncie);
  CHECK(r.line() == 1);
  r.write(icr, syncokc);
  CHECK(r.line() == 1);
  r.write(icr, esyncc);
  CHECK(r.line() == 0);
}

TEST_CASE_TEMPLATE(
    "the bootloader setup gives SYNCOKF at each SOF after the first",
    F,
    u5_family,
    h5_family)
{
  rig<F> const r;
  r.connect();
  r.write(cr, F::trim_reset);
  r.configure(matched);
  r.write(cr, r.read(cr) | autotrimen);
  r.sof(1);
  CHECK(r.flags() == 0);
  for(std::uint16_t frame = 2; frame < 7; ++frame)
  {
    r.run_for(ms);
    r.sof(frame);
    CHECK(r.flags() == (syncokf | esyncf));
    CHECK(r.capture() == 0);
    CHECK(r.direction() == 0);
    r.write(icr, ~std::uint32_t{});
  }
  CHECK((r.read(cr) & (0x7Fu << 8)) == F::trim_reset);
}

TEST_CASE_TEMPLATE(
    "a USB SOF is a SYNC only with CEN, SYNCSRC 2 and a pull-up",
    F,
    u5_family,
    h5_family)
{
  rig<F> const r;
  r.write(cr, cen);
  r.sof();
  CHECK(r.pending().empty());
  r.connect();
  r.sof();
  r.run_for(ms);
  r.sof();
  CHECK(r.flags() == (syncokf | esyncf));
  r.write(icr, ~std::uint32_t{});
  r.write(cr, 0);
  r.write(cfgr, (r.read(cfgr) & ~(3u << 28)) | source_lse);
  r.write(cr, cen);
  r.sof();
  CHECK(r.pending().empty());
  r.write(cr, 0);
  r.write(cfgr, (r.read(cfgr) & ~(3u << 28)) | source_usb);
  r.sof();
  CHECK(r.pending().empty());
  CHECK(r.read(isr) == 0);
}

TEST_CASE_TEMPLATE(
    "SYNCDIV 2 counts four SOFs per SYNC, so RELOAD 47999 ends in SYNCMISS and ERRF",
    F,
    u5_family,
    h5_family)
{
  rig<F> const r;
  r.connect();
  r.enable_usb_sync(2);
  for(auto n = 0; n < 3; ++n)
  {
    r.sof();
    r.run_for(ms);
  }
  CHECK(r.frames() == 3);
  CHECK(r.pending().empty());
  r.sof();
  CHECK(r.frames() == 0);
  CHECK(r.flags() == 0);
  auto const first = r.now();
  for(auto n = 0; n < 4; ++n)
  {
    r.run_for(ms);
    r.sof();
  }
  CHECK(r.now() == first + 4 * ms);
  CHECK(r.flags() == (esyncf | syncmiss | errf));
  CHECK(r.capture() == outrange);
  CHECK(r.direction() == 0);
  r.sof();
  r.sof();
  CHECK(r.frames() == 2);
  r.write(cr, 0);
  CHECK(r.frames() == 0);
  CHECK(r.pending().empty());
}

TEST_CASE_TEMPLATE(
    "a SOF dated before the ESYNC event that already ran classes by elapsed count",
    F,
    u5_family,
    h5_family)
{
  rig<F> const r;
  r.connect();
  r.configure(matched + 1);
  r.software_sync();
  auto const t0 = r.now();
  r.run_past(t0 + ms + 20'834);
  CHECK(r.flags() == esyncf);
  r.port().start_of_frame(2, vtime{t0 + ms});
  CHECK(r.flags() == (esyncf | syncokf));
  CHECK(r.capture() == 1);
  CHECK(r.direction() == 1);
}

TEST_CASE_TEMPLATE(
    "FELIM 10 and RELOAD 23999 set the miss time and the 3 x FELIM class",
    F,
    u5_family,
    h5_family)
{
  constexpr std::uint32_t small   = 10;
  constexpr std::uint32_t half_ms = 500'000'000;
  SUBCASE("due times")
  {
    rig<F> const r;
    r.configure(23'999, 0, small);
    r.software_sync();
    auto const t0 = r.now();
    CHECK(r.pending() == std::vector<std::uint64_t>{t0 + half_ms});
    r.run_until(t0 + half_ms);
    CHECK(r.flags() == esyncf);
    CHECK(r.pending() == std::vector<std::uint64_t>{t0 + 526'666'667});
    r.run_past(t0 + 526'666'667);
    CHECK(r.flags() == (esyncf | syncmiss | errf));
  }
  SUBCASE("down by 3 x FELIM - 1")
  {
    rig<F> const r;
    r.configure(23'999 + 3 * small - 1, 0, small);
    r.software_sync();
    r.run_for(half_ms);
    r.software_sync();
    CHECK(r.flags() == syncokf);
    CHECK(r.capture() == 3 * small - 1);
  }
  SUBCASE("down by 3 x FELIM")
  {
    rig<F> const r;
    r.configure(23'999 + 3 * small, 0, small);
    r.software_sync();
    r.run_for(half_ms);
    r.software_sync();
    CHECK(r.flags() == syncwarnf);
    CHECK(r.capture() == 3 * small);
    CHECK(r.direction() == 1);
  }
}

TEST_CASE_TEMPLATE("two SYNCs in one HSI48 cycle latch RELOAD in FECAP", F, u5_family, h5_family)
{
  rig<F> const r;
  r.configure(0xFFFF);
  r.software_sync();
  r.software_sync();
  CHECK(r.capture() == 0xFFFF);
  CHECK(r.direction() == 1);
  CHECK(r.flags() == (syncerr | errf));
}

TEST_CASE_TEMPLATE(
    "CEN clear withdraws the counter event and a CEN toggle waits for a new first SYNC",
    F,
    u5_family,
    h5_family)
{
  rig<F> const r;
  r.software_sync();
  auto const t0 = r.now();
  r.run_for(ms / 2);
  r.write(cr, 0);
  CHECK(r.pending().empty());
  r.run_until(t0 + 2 * ms);
  CHECK(r.read(isr) == 0);
  r.write(cr, cen);
  r.run_for(2 * ms);
  CHECK(r.read(isr) == 0);
  CHECK(r.state().phase == 0);
}

TEST_CASE_TEMPLATE(
    "the RCC CRS reset restores the registers, the flags and the divider",
    F,
    u5_family,
    h5_family)
{
  rig<F> const r;
  r.connect();
  r.configure(matched, 3);
  r.write(cr, r.read(cr) | syncokie | autotrimen);
  r.software_sync();
  r.run_for(ms);
  r.software_sync();
  r.sof();
  REQUIRE(r.flags() == (syncokf | esyncf));
  REQUIRE(r.frames() == 1);
  REQUIRE(r.line() == 1);
  REQUIRE(r.pending().size() == 1);
  popcorn::testing::rcc_reset_pulse(r.f, F::reset_reg, "CRSRST");
  CHECK(r.read(cr) == F::trim_reset);
  CHECK(r.read(cfgr) == 0x2022'BB7F);
  CHECK(r.read(isr) == 0);
  CHECK(r.frames() == 0);
  CHECK(r.line() == 0);
  CHECK(r.pending().empty());
}

TEST_CASE_TEMPLATE(
    "a counter event pending across the RCC CRS reset does not act on the new counter",
    F,
    u5_family,
    h5_family)
{
  rig<F> const r;
  r.software_sync();
  auto const t0 = r.now();
  r.run_until(t0 + ms / 2);
  popcorn::testing::rcc_reset_pulse(r.f, F::reset_reg, "CRSRST");
  r.run_until(t0 + 6 * ms / 10);
  r.software_sync();
  r.run_until(t0 + 11 * ms / 10);
  CHECK(r.flags() == 0);
  r.run_until(t0 + 16 * ms / 10);
  CHECK(r.flags() == esyncf);
}

TEST_CASE_TEMPLATE(
    "the saved state with flags and an event pending round-trips through a snapshot",
    F,
    u5_family,
    h5_family)
{
  rig<F> const r;
  r.connect();
  r.configure(matched, 2);
  r.write(cr, r.read(cr) | syncokie);
  r.software_sync();
  r.run_for(ms);
  r.software_sync();
  r.sof();
  r.sof();
  auto const sync_at = r.now();
  auto const saved   = r.f.target->snapshot();
  REQUIRE(r.line() == 1);
  r.write(icr, ~std::uint32_t{});
  r.run_until(sync_at + 3 * ms);
  r.sof();
  r.sof();
  CHECK(r.frames() == 0);
  CHECK(r.line() == 0);
  r.f.target->restore(saved);
  CHECK(r.flags() == (syncokf | esyncf));
  CHECK(r.line() == 1);
  CHECK(r.frames() == 2);
  CHECK(r.pending() == std::vector<std::uint64_t>{sync_at + ms});
  r.run_past(sync_at + ms + miss_after);
  CHECK(r.flags() == (syncokf | esyncf | syncmiss | errf));
}

TEST_CASE_TEMPLATE(
    "CEN with a GPIO or LSE sync source logs one warning per model life",
    F,
    u5_family,
    h5_family)
{
  rig<F> r;
  r.configure(matched, 0, felim, source_lse);
  REQUIRE(r.logged.size() == 1);
  CHECK(r.logged[0].kind == core::log_kind::model_warning);
  CHECK(r.logged[0].peripheral == "CRS");
  CHECK(r.logged[0].reg == "CFGR");
  CHECK(r.logged[0].message.find("SYNCSRC 1") != std::string::npos);
  r.write(cr, 0);
  r.write(cr, cen);
  CHECK(r.logged.size() == 1);
}
