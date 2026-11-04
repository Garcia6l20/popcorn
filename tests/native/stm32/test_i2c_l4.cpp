#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/devices/i2c_eeprom.hpp>
#include <popcorn/periph/i2c.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace
{
namespace periph  = popcorn::periph;
namespace devices = popcorn::periph::devices;
namespace stm32   = popcorn::periph::stm32;
using popcorn::address;
using popcorn::icount;
using popcorn::part_description;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::l476rc;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr std::string_view ll            = "l476_l4_i2c_ll";
constexpr std::string_view hal           = "l476_l4_i2c_hal";
constexpr std::uint64_t    ps_per_second = 1'000'000'000'000;
constexpr address          i2c1          = 0x4000'5400;
constexpr address          cr1           = 0x00;
constexpr address          cr2           = 0x04;
constexpr address          oar1          = 0x08;
constexpr address          timingr       = 0x10;
constexpr address          isr           = 0x18;
constexpr address          icr           = 0x1C;
constexpr address          apb1enr1      = 0x4002'1058;
constexpr std::uint32_t    pe            = 1u << 0;
constexpr std::uint32_t    start_bit     = 1u << 13;
constexpr std::uint32_t    addr          = 1u << 3;
constexpr std::uint32_t    stopf         = 1u << 5;
constexpr std::uint8_t     own           = 0x42;
constexpr std::uint32_t    wire          = 1;

class tap final : public periph::i2c_target
{
public:
  explicit tap(std::shared_ptr<periph::i2c_target> inner) : inner_{std::move(inner)}
  {}

  std::vector<std::uint64_t> writes;

  [[nodiscard]] auto address() const -> std::uint8_t override
  {
    return inner_->address();
  }

  auto start(bool read, vtime at) -> periph::i2c_reply override
  {
    return inner_->start(read, at);
  }

  auto write(std::uint8_t byte, vtime at) -> periph::i2c_reply override
  {
    writes.push_back(at.ps);
    return inner_->write(byte, at);
  }

  auto read(vtime at) -> periph::i2c_read_reply override
  {
    return inner_->read(at);
  }

  void stop(vtime at) override
  {
    inner_->stop(at);
  }

  void save(std::vector<std::byte>& out) const override
  {
    inner_->save(out);
  }

  void load(std::span<std::byte const> in) override
  {
    inner_->load(in);
  }

  void reset(popcorn::reset_kind kind) override
  {
    inner_->reset(kind);
  }

private:
  std::shared_ptr<periph::i2c_target> inner_;
};

class quiet_sink final : public periph::i2c_controller_sink
{
public:
  void answered(std::uint64_t, periph::i2c_phase, periph::i2c_answer, vtime) override
  {}
  void dropped(std::uint64_t, periph::i2c_phase, vtime) override
  {}
};

struct rig
{
  std::shared_ptr<tap>        memory = std::make_shared<tap>(std::make_shared<devices::eeprom_24xx>(
      devices::eeprom_24xx_options{.size = 4096, .page_size = 16}));
  std::shared_ptr<quiet_sink> sink   = std::make_shared<quiet_sink>();
  std::unique_ptr<stm32_fixture> f;

  explicit rig(
      std::string_view        entry,
      std::string_view        elf      = ll,
      bool                    targeted = false,
      part_description const& desc     = l476rc())
  {
    stm32::board board{};
    if(targeted)
    {
      board.i2c_controllers["I2C1"] = sink;
    }
    else
    {
      board.i2c["I2C1"] = {memory};
    }
    auto options   = popcorn::testing::stm32_options();
    options.models = stm32::factories(board, desc);
    f              = std::make_unique<stm32_fixture>(desc, elf, std::move(options));
    if(entry != "main")
    {
      f->set("pc", sym(elf, entry));
    }
  }

  auto finish() const -> std::vector<std::uint32_t>
  {
    auto const stopped = f->run_to_stop({.max_instructions = icount{20'000'000}});
    CAPTURE(stopped.detail);
    REQUIRE(stopped.reason == stop_reason::user);
    return f->log();
  }

  [[nodiscard]] auto read(address at) const -> std::uint32_t
  {
    auto const value = f->target->mem().read(at, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void write(address at, std::uint32_t value) const
  {
    REQUIRE(f->target->mem().write(at, 4, value).has_value());
  }

  [[nodiscard]] auto now_ps() const -> std::uint64_t
  {
    return f->target->virtual_clock().time().ps;
  }

  void run_until(std::uint64_t ps) const
  {
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{1'000'000}, .until_time = vtime{ps}}).reason
        == stop_reason::time);
  }

  [[nodiscard]] auto port() const -> periph::i2c_controller_port&
  {
    auto* const found = dynamic_cast<periph::i2c_controller_port*>(f->target->model("I2C1"));
    REQUIRE(found != nullptr);
    return *found;
  }

  void listen() const
  {
    write(apb1enr1, read(apb1enr1) | (1u << 21));
    write(i2c1 + timingr, 0x0010'0305u);
    write(i2c1 + oar1, (std::uint32_t{own} << 1) | (1u << 15));
    write(i2c1 + cr1, pe | (3u << 30));
  }
};

auto bit_ps(std::uint32_t timing, std::uint64_t hz) -> std::uint64_t
{
  auto const low   = (timing & 0xFFu) + 1u;
  auto const high  = ((timing >> 8) & 0xFFu) + 1u;
  auto const presc = ((timing >> 28) & 0xFu) + 1u;
  return std::uint64_t{low + high} * presc * ps_per_second / hz;
}

void byte_spaced(std::vector<std::uint64_t> const& writes, std::uint64_t bit)
{
  REQUIRE(writes.size() >= 10);
  for(std::size_t index = 2; index < 10; ++index)
  {
    CAPTURE(index);
    CHECK(writes[index] - writes[index - 1] == 9 * bit);
  }
}

auto with_i2c1(std::function<void(popcorn::peripheral_desc&)> const& edit) -> part_description
{
  auto       desc  = l476rc();
  auto const found = std::ranges::find(
      desc.peripherals, std::string_view{"I2C1"}, &popcorn::peripheral_desc::name);
  REQUIRE(found != desc.peripherals.end());
  edit(*found);
  return desc;
}
}

TEST_CASE("RM0351 39.4.9: LL writes a 24xx page and reads it back at 400 kHz from PCLK1")
{
  rig r{"main"};
  CHECK(r.finish() == std::vector<std::uint32_t>{8, 1, 0x0010'0305u});
  byte_spaced(r.memory->writes, bit_ps(0x0010'0305u, 4'000'000));
}

TEST_CASE("RM0351 6.4.28: I2C1 on HSI16 through CCIPR.I2C1SEL runs the same page at 100 kHz")
{
  rig r{"hsi16_100k"};
  CHECK(r.finish() == std::vector<std::uint32_t>{8, 1, 0x0030'3D5Bu});
  byte_spaced(r.memory->writes, bit_ps(0x0030'3D5Bu, 16'000'000));
}

TEST_CASE("HAL_I2C_IsDeviceReady, Mem_Write and Mem_Read reach the 24xx on I2C1")
{
  rig r{"main", hal};
  CHECK(r.finish() == std::vector<std::uint32_t>{0, 1, 0xC0, 0xC3});
}

TEST_CASE("RM0351 39.9.8: the LL target takes an address, a byte and a STOP, ICR clears each flag")
{
  rig r{"target", ll, true};
  r.run_until(r.now_ps() + 1'000'000'000);
  auto const phase = 9 * bit_ps(0x0010'0305u, 4'000'000);
  auto const base  = r.now_ps() + 1'000'000;
  r.port().start(own, false, phase, vtime{base}, wire, 1);
  r.port().write(0x5A, phase, vtime{base + phase}, wire, 2);
  r.port().stop(vtime{base + 3 * phase}, wire, 3);
  r.run_until(base + 4 * phase);
  CHECK(
      r.f->log()
      == std::vector<std::uint32_t>{
          0xADD0'0000u | (std::uint32_t{own} << 1), 0xDA7A'005Au, 0x5709u});
  CHECK((r.read(i2c1 + isr) & (addr | stopf)) == 0);
}

TEST_CASE(
    "ES0250 2.20.7: a target address match leaves a pending START, ADDRCF clears both, bits 31:30 "
    "of CR1 change nothing")
{
  rig r{"idle", ll, true};
  r.listen();
  auto const phase = 9 * bit_ps(0x0010'0305u, 4'000'000);
  auto const base  = r.now_ps() + 1'000'000;
  r.port().start(own, false, phase, vtime{base}, wire, 1);
  r.run_until(base + phase / 2);
  r.write(i2c1 + cr2, start_bit | (0x38u << 1));
  r.run_until(base + phase + 1'000'000);
  CHECK((r.read(i2c1 + isr) & addr) != 0);
  CHECK((r.read(i2c1 + cr2) & start_bit) != 0);
  r.write(i2c1 + icr, addr);
  CHECK((r.read(i2c1 + isr) & addr) == 0);
  CHECK((r.read(i2c1 + cr2) & start_bit) == 0);
}

TEST_CASE("an L4 I2C description with CR1.ADDRACLR throws config_error, the L4 has none")
{
  auto const with_aclr = with_i2c1([](popcorn::peripheral_desc& unit) {
    auto const found =
        std::ranges::find(unit.registers, std::string_view{"CR1"}, &popcorn::register_desc::name);
    REQUIRE(found != unit.registers.end());
    auto added = found->fields.front();
    added.name = "ADDRACLR";
    added.lsb  = 30;
    found->fields.push_back(added);
  });
  CHECK_THROWS_WITH_AS(
      (rig{"idle", ll, false, with_aclr}), doctest::Contains("ADDRACLR"), popcorn::config_error);
}
