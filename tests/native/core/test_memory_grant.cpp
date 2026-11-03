#include "../../../native/src/core/machine_impl.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/periph/firewall.hpp>
#include <popcorn/periph/model.hpp>

#include "../fake_services.hpp"
#include "../support/inert_model.hpp"
#include "../support/machine_fixture.hpp"

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace periph = popcorn::periph;
using periph::bus_verdict;
using popcorn::address;
using popcorn::bus_attrs;
using popcorn::initiator;
using popcorn::arch::memory_op;
using popcorn::testing::m33;
using fixture = popcorn::testing::machine_fixture<>;

constexpr address   sram       = 0x2000'0000;
constexpr address   extent     = 0x400;
constexpr bus_attrs dma        = {.by = initiator::dma};
constexpr address   sram_last  = 0x200b'ffff;
constexpr address   flash      = 0x0800'0000;
constexpr address   flash_last = 0x081f'ffff;

class counting_firewall final : public popcorn::testing::inert_model, public periph::bus_firewall
{
public:
  counting_firewall(periph::model_context& ctx, bool filtering)
      : services{ctx.services}, filtering{filtering}
  {
    ctx.install_firewall(*this);
  }

  auto check_peripheral(std::size_t, bus_attrs, memory_op) -> bus_verdict override
  {
    return bus_verdict::allow;
  }
  [[nodiscard]] auto filters_memory() const -> bool override
  {
    return filtering;
  }
  auto check_memory(address at, std::uint8_t, bus_attrs, memory_op) -> bus_verdict override
  {
    ++checks;
    if(at >= refused_first and at <= refused_last and throws)
    {
      throw popcorn::model_error{"FIREWALL", "", 0};
    }
    return at >= refused_first and at <= refused_last ? refused : verdict;
  }
  auto allowed_extent(address at, std::uint8_t size, bus_attrs, memory_op)
      -> periph::memory_extent override
  {
    if(access_only)
    {
      return {.first = at, .last = at + size - 1u};
    }
    auto const first = at & ~(extent - 1u);
    return {.first = first, .last = first + extent - 1u};
  }

  core::machine_services& services;
  bool                    filtering{};
  std::size_t             checks{};
  bus_verdict             verdict = bus_verdict::allow;
  bool                    access_only{};
  address                 refused_first = 1;
  address                 refused_last  = 0;
  bus_verdict             refused       = bus_verdict::allow;
  bool                    throws{};
};

struct rig
{
  std::shared_ptr<counting_firewall> firewall{};
  std::unique_ptr<fixture>           f{};
};

auto booted(bool filtering) -> rig
{
  rig                   out;
  core::machine_options options;
  options.models.emplace(
      "UNMODELLED",
      [&out, filtering](periph::model_context& ctx, popcorn::peripheral_desc const&)
          -> std::shared_ptr<periph::model> {
        out.firewall = std::make_shared<counting_firewall>(ctx, filtering);
        return out.firewall;
      });
  out.f = std::make_unique<fixture>(m33(), "intercepts", std::move(options));
  return out;
}

auto services(rig const& r) -> core::machine_services&
{
  return r.firewall->services;
}

void read_through(rig const& r, address at, core::memory_grant& grant)
{
  REQUIRE(services(r).read(at, 4, dma, grant).has_value());
}
}

TEST_CASE("an access inside a grant skips the firewall, one outside it asks again")
{
  auto const         r = booted(true);
  core::memory_grant grant;
  read_through(r, sram + 0x10, grant);
  CHECK(r.firewall->checks == 1);
  CHECK(grant.first == sram);
  CHECK(grant.last == sram + extent - 1u);
  read_through(r, sram + extent - 4u, grant);
  REQUIRE(services(r).write(sram + 0x20, 4, 0x1234'5678, dma, grant).has_value());
  CHECK(r.firewall->checks == 1);
  read_through(r, sram + extent, grant);
  CHECK(r.firewall->checks == 2);
  CHECK(grant.first == sram + extent);
}

TEST_CASE("a grant shorter than the access does not hold it")
{
  auto const         r = booted(true);
  core::memory_grant grant;
  r.firewall->access_only = true;
  REQUIRE(services(r).read(sram + 0x10, 1, dma, grant).has_value());
  REQUIRE(grant.first == grant.last);
  read_through(r, sram + 0x10, grant);
  CHECK(r.firewall->checks == 2);
  CHECK(grant.last == sram + 0x13);
}

TEST_CASE("memory_policy_changed, a reset and a restore each make the next access ask again")
{
  auto const         r     = booted(true);
  auto const         saved = r.f->target->snapshot();
  core::memory_grant grant;
  read_through(r, sram, grant);
  REQUIRE(r.firewall->checks == 1);

  SUBCASE("memory_policy_changed")
  {
    services(r).memory_policy_changed();
  }
  SUBCASE("a reset")
  {
    r.f->target->reset();
  }
  SUBCASE("a restore")
  {
    r.f->target->restore(saved);
  }
  r.firewall->verdict = bus_verdict::read_zero_write_ignore;
  REQUIRE(r.f->target->mem().write(sram, 4, 0x5ec0'0001));
  auto const value = services(r).read(sram, 4, dma, grant);
  REQUIRE(value.has_value());
  CHECK(*value == 0u);
  CHECK(r.firewall->checks == 2);
  CHECK(grant.generation == 0);
}

TEST_CASE("without a filtering firewall a grant spans the region of the access")
{
  auto const         r = booted(false);
  core::memory_grant grant;
  auto&              bus = services(r);
  REQUIRE(bus.read(sram + 0x100, 4, dma, grant).has_value());
  CHECK(grant.generation != 0);
  CHECK(grant.first == sram);
  CHECK(grant.last == sram_last);
  REQUIRE(bus.read(flash + 0x100, 4, dma, grant).has_value());
  CHECK(grant.first == flash);
  CHECK(grant.last == flash_last);
  CHECK(r.firewall->checks == 0);
}

TEST_CASE("the base granted overloads clear the grant and take the plain access")
{
  popcorn::testing::fake_services services;
  core::memory_grant              grant{.generation = 7, .first = sram, .last = sram_last};
  REQUIRE(services.write(sram, 4, 0x1234'5678, dma, grant).has_value());
  CHECK(grant.generation == 0);
  grant = {.generation = 7, .first = sram, .last = sram_last};
  CHECK(services.read(sram, 4, dma, grant) == 0x1234'5678u);
  CHECK(grant.generation == 0);
  CHECK(services.last_by == initiator::dma);
}

TEST_CASE(
    "a filtered block write drops translated code once per run the firewall allowed and keeps "
    "the bytes it refused")
{
  auto const r       = booted(true);
  auto&      bus     = services(r);
  auto const counted = [&r] { return core::machine_internals::counters(*r.f->target); };
  std::vector<std::byte> const old(32, std::byte{0xee});
  std::vector<std::byte>       in(32);
  for(std::size_t i = 0; i < in.size(); ++i)
  {
    in[i] = static_cast<std::byte>(i + 1);
  }
  auto const bytes = [&bus] {
    std::vector<std::byte> out(32);
    REQUIRE(bus.read_block(sram + 0x200, out, dma).has_value());
    return out;
  };
  auto const span_of = [](std::vector<std::byte> const& from, std::size_t first, std::size_t last) {
    return std::vector<std::byte>(from.begin() + first, from.begin() + last);
  };

  SUBCASE("every word allowed")
  {
    auto const before = counted();
    REQUIRE(bus.write_block(sram + 0x200, in, dma).has_value());
    CHECK(counted().removes_direct - before.removes_direct == 1);
    CHECK(counted().remove_calls_direct - before.remove_calls_direct == 1);
    CHECK(bytes() == in);
  }
  SUBCASE("two words read as zero and ignore writes")
  {
    REQUIRE(bus.write_block(sram + 0x200, old, dma).has_value());
    r.firewall->refused_first = sram + 0x20c;
    r.firewall->refused_last  = sram + 0x213;
    r.firewall->refused       = bus_verdict::read_zero_write_ignore;
    auto const before         = counted();
    REQUIRE(bus.write_block(sram + 0x200, in, dma).has_value());
    CHECK(counted().removes_direct - before.removes_direct == 2);
    CHECK(counted().remove_calls_direct - before.remove_calls_direct == 2);
    r.firewall->refused = bus_verdict::allow;
    auto const now      = bytes();
    CHECK(span_of(now, 0, 12) == span_of(in, 0, 12));
    CHECK(span_of(now, 12, 20) == span_of(old, 12, 20));
    CHECK(span_of(now, 20, 32) == span_of(in, 20, 32));
  }
  SUBCASE("a bus error stops the block after the words before it")
  {
    REQUIRE(bus.write_block(sram + 0x200, old, dma).has_value());
    r.firewall->refused_first = sram + 0x214;
    r.firewall->refused_last  = sram + 0x217;
    r.firewall->refused       = bus_verdict::bus_error;
    auto const before         = counted();
    auto const done           = bus.write_block(sram + 0x200, in, dma);
    REQUIRE_FALSE(done.has_value());
    CHECK(done.error().at == sram + 0x214);
    CHECK(done.error().error == popcorn::bus_error::denied);
    CHECK(counted().removes_direct - before.removes_direct == 1);
    CHECK(counted().remove_calls_direct - before.remove_calls_direct == 1);
    r.firewall->refused = bus_verdict::allow;
    auto const now      = bytes();
    CHECK(span_of(now, 0, 20) == span_of(in, 0, 20));
    CHECK(span_of(now, 20, 32) == span_of(old, 20, 32));
  }
  SUBCASE("a firewall that throws drops the code of the words written before it")
  {
    r.firewall->refused_first = sram + 0x208;
    r.firewall->refused_last  = sram + 0x20b;
    r.firewall->throws        = true;
    auto const before         = counted();
    CHECK_THROWS_AS(
        static_cast<void>(bus.write_block(sram + 0x200, in, dma)), popcorn::model_error);
    CHECK(counted().removes_direct - before.removes_direct == 1);
    r.firewall->throws = false;
  }
  SUBCASE("a bus error on the first word drops no code")
  {
    r.firewall->refused_first = sram + 0x200;
    r.firewall->refused_last  = sram + 0x203;
    r.firewall->refused       = bus_verdict::bus_error;
    auto const before         = counted();
    CHECK_FALSE(bus.write_block(sram + 0x200, in, dma).has_value());
    CHECK(counted().removes_direct == before.removes_direct);
  }
}

TEST_CASE("code a filtered block write replaces after a refused word runs as the new code")
{
  auto const r   = booted(true);
  auto&      bus = services(r);
  auto const run = [&r] {
    auto const registers = r.f->target->regs();
    registers.write(*registers.find("pc"), sram + 0x214);
    auto const slice = r.f->target->run_slice(
        {.max_instructions = popcorn::icount{16}, .wall_budget = std::chrono::seconds{10}});
    REQUIRE(slice.stopped.has_value());
    REQUIRE(slice.stopped->reason == popcorn::stop_reason::user);
    return registers.read(*registers.find("r0"));
  };
  std::vector<std::byte> block(32, std::byte{0});
  auto const             code = [&block](std::byte value) {
    block[20] = value;
    block[21] = std::byte{0x20};
    block[22] = std::byte{0x00};
    block[23] = std::byte{0xbe};
  };
  code(std::byte{0x01});
  REQUIRE(bus.write_block(sram + 0x200, block, dma).has_value());
  CHECK(run() == 1);
  code(std::byte{0x02});
  r.firewall->refused_first = sram + 0x20c;
  r.firewall->refused_last  = sram + 0x20f;
  r.firewall->refused       = bus_verdict::read_zero_write_ignore;
  REQUIRE(bus.write_block(sram + 0x200, block, dma).has_value());
  r.firewall->refused = bus_verdict::allow;
  CHECK(run() == 2);
}

TEST_CASE("a filtered block write over a watched range reports each written word")
{
  auto const                   r   = booted(true);
  auto&                        bus = services(r);
  std::vector<core::watch_hit> hits;
  auto const                   watch = r.f->target->add_watch(
      sram + 0x304, 8, core::watch_kind::write, [&hits](core::watch_hit const& hit) {
        hits.push_back(hit);
        return core::watch_action::resume;
      });
  std::vector<std::byte> in(16);
  for(std::size_t i = 0; i < in.size(); ++i)
  {
    in[i] = static_cast<std::byte>(0x10 + i);
  }
  REQUIRE(bus.write_block(sram + 0x300, in, dma).has_value());
  REQUIRE(hits.size() == 2);
  CHECK(hits[0].addr == sram + 0x304);
  CHECK(hits[0].value == 0x1716'1514u);
  CHECK(hits[1].addr == sram + 0x308);
  CHECK(hits[1].value == 0x1b1a'1918u);
  CHECK(hits[0].write);
  CHECK(hits[0].by == initiator::dma);
}

TEST_CASE("a filtered DMA block write into ROM is refused or ignored and leaves its bytes")
{
  auto const                     r   = booted(true);
  auto&                          bus = services(r);
  auto const                     at  = flash + 0x100;
  auto const                     old = r.f->target->mem().read(at, 4).value();
  std::array<std::byte, 8> const in{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                                    std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
  auto const                     done = bus.write_block(at, in, dma);
  REQUIRE_FALSE(done.has_value());
  CHECK(done.error() == core::block_error{popcorn::bus_error::denied, at});
  CHECK(r.f->target->mem().read(at, 4) == old);
  r.firewall->refused_first = at;
  r.firewall->refused_last  = at + 7;
  r.firewall->refused       = bus_verdict::read_zero_write_ignore;
  CHECK(bus.write_block(at, in, dma).has_value());
  r.firewall->refused = bus_verdict::allow;
  CHECK(r.f->target->mem().read(at, 4) == old);
}
