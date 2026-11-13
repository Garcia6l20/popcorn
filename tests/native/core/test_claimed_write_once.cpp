#include <popcorn/core/machine.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/layer_keeper.hpp>
#include <popcorn/periph/model.hpp>

#include "../../../native/src/core/machine_impl.hpp"
#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::initiator;
using popcorn::reset_kind;
using popcorn::testing::m33;
using fixture = popcorn::testing::machine_fixture<>;

constexpr address testdev = 0x4001'0000;
constexpr address wonce   = testdev + 12;
constexpr address rwonce  = testdev + 16;

class once_model final : public popcorn::periph::model, public popcorn::periph::layer_keeper
{
public:
  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {"WONCE", "RWONCE"};
  }

  void reset(popcorn::periph::register_file& regs, reset_kind) override
  {
    regs.poke(12, 0);
    regs.poke(16, 0);
  }

  auto on_read(
      popcorn::periph::register_file& regs,
      std::uint32_t                   offset,
      std::uint8_t                    size,
      popcorn::periph::access_ctx) -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    auto const shifted = regs.peek(offset & ~3u) >> (8 * (offset % 4));
    return size == 4 ? shifted : shifted & ((std::uint32_t{1} << (8 * size)) - 1);
  }

  auto on_write(
      popcorn::periph::register_file& regs,
      std::uint32_t                   offset,
      std::uint8_t                    size,
      std::uint32_t,
      std::uint32_t value,
      popcorn::periph::access_ctx) -> std::expected<void, popcorn::bus_error> override
  {
    auto const shift = 8 * (offset % 4);
    auto const mask  = (size == 4 ? ~std::uint32_t{} : (std::uint32_t{1} << (8 * size)) - 1)
                    << shift;
    auto const word  = offset & ~3u;
    regs.poke(word, (regs.peek(word) & ~mask) | ((value << shift) & mask));
    return {};
  }

  void on_event(std::uint32_t, std::uint64_t) override
  {}

  void save(std::vector<std::byte>&) const override
  {}

  void load(std::span<std::byte const>) override
  {}

  [[nodiscard]] auto keeps_over_reset(std::uint32_t offset, reset_kind kind) const -> bool override
  {
    return offset == 16 and kind == reset_kind::software;
  }
};

auto part() -> popcorn::part_description
{
  auto  made = m33();
  auto& desc =
      *std::ranges::find(made.peripherals, std::string{"TESTDEV"}, &popcorn::peripheral_desc::name);
  auto& mixed =
      *std::ranges::find(desc.registers, std::string{"RWONCE"}, &popcorn::register_desc::name);
  mixed.acc    = popcorn::access::read_write;
  mixed.fields = {
      {.name = "LOW",  .lsb = 0,  .width = 8, .acc = popcorn::access::read_write_once},
      {.name = "HIGH", .lsb = 24, .width = 8, .acc = popcorn::access::read_write     },
  };
  return made;
}

auto make() -> fixture
{
  core::machine_options options;
  options.models.emplace(
      "TESTDEV",
      [](popcorn::periph::model_context&, popcorn::peripheral_desc const&)
          -> std::shared_ptr<popcorn::periph::model> { return std::make_shared<once_model>(); });
  return fixture{part(), "mmio", std::move(options)};
}

auto bus(fixture const& f) -> core::machine_services&
{
  return core::machine_internals::services(*f.target);
}

auto read(fixture const& f, address at) -> std::uint32_t
{
  return static_cast<std::uint32_t>(*f.target->mem().read(at, 4));
}
}

TEST_CASE("a claimed write-once register keeps the value of its first write")
{
  auto f = make();
  REQUIRE(bus(f).write(wonce, 4, 0x11, initiator::cpu).has_value());
  REQUIRE(bus(f).write(wonce, 4, 0x22, initiator::cpu).has_value());
  CHECK(f.target->peripheral("TESTDEV").peek(12) == 0x11);
}

TEST_CASE("a claimed read-write-once register reads back and keeps its first value")
{
  auto f = make();
  REQUIRE(bus(f).write(rwonce, 1, 0x5, initiator::cpu).has_value());
  REQUIRE(bus(f).write(rwonce, 1, 0x6, initiator::cpu).has_value());
  CHECK(bus(f).read(rwonce, 1, initiator::cpu) == 0x5u);
  REQUIRE(bus(f).write(rwonce + 3, 1, 0x7, initiator::cpu).has_value());
  CHECK(read(f, rwonce) == 0x0700'0005u);
}

TEST_CASE("a write that misses the write-once lanes leaves the lock open")
{
  auto f = make();
  REQUIRE(bus(f).write(rwonce + 3, 1, 0x7, initiator::cpu).has_value());
  REQUIRE(bus(f).write(rwonce + 3, 1, 0x8, initiator::cpu).has_value());
  CHECK(read(f, rwonce) == 0x0800'0000u);
  REQUIRE(bus(f).write(rwonce, 1, 0x1, initiator::cpu).has_value());
  REQUIRE(bus(f).write(rwonce, 1, 0x2, initiator::cpu).has_value());
  CHECK(read(f, rwonce) == 0x0800'0001u);
}

TEST_CASE("a debugger write of a claimed write-once word is held and locks like a CPU write")
{
  auto f = make();
  REQUIRE(bus(f).write(wonce, 4, 0x11, initiator::debugger).has_value());
  REQUIRE(bus(f).write(wonce, 4, 0x22, initiator::cpu).has_value());
  CHECK(f.target->peripheral("TESTDEV").peek(12) == 0x11);
  f.target->reset(reset_kind::power_on);
  REQUIRE(bus(f).write(wonce, 4, 0x33, initiator::cpu).has_value());
  REQUIRE(bus(f).write(wonce, 4, 0x44, initiator::debugger).has_value());
  CHECK(f.target->peripheral("TESTDEV").peek(12) == 0x33);
}

TEST_CASE("the lock of a claimed word is saved and restored")
{
  auto f = make();
  REQUIRE(bus(f).write(wonce, 4, 0x11, initiator::cpu).has_value());
  auto const saved = f.target->snapshot();
  f.target->reset(reset_kind::power_on);
  REQUIRE(bus(f).write(wonce, 4, 0x33, initiator::cpu).has_value());
  f.target->restore(saved);
  REQUIRE(bus(f).write(wonce, 4, 0x44, initiator::cpu).has_value());
  CHECK(f.target->peripheral("TESTDEV").peek(12) == 0x11);
}

TEST_CASE("a reset opens the lock of a claimed word unless the model keeps it")
{
  auto f = make();
  REQUIRE(bus(f).write(wonce, 4, 0x11, initiator::cpu).has_value());
  REQUIRE(bus(f).write(rwonce, 1, 0x5, initiator::cpu).has_value());
  f.target->reset(reset_kind::software);
  REQUIRE(bus(f).write(wonce, 4, 0x22, initiator::cpu).has_value());
  CHECK(f.target->peripheral("TESTDEV").peek(12) == 0x22);
  REQUIRE(bus(f).write(rwonce, 1, 0x6, initiator::cpu).has_value());
  CHECK(bus(f).read(rwonce, 1, initiator::cpu) == 0u);
  f.target->reset(reset_kind::power_on);
  REQUIRE(bus(f).write(rwonce, 1, 0x7, initiator::cpu).has_value());
  REQUIRE(bus(f).write(rwonce, 1, 0x8, initiator::cpu).has_value());
  CHECK(bus(f).read(rwonce, 1, initiator::cpu) == 0x7u);
}
