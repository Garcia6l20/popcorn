#include <popcorn/core/machine.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/periph/firewall.hpp>
#include <popcorn/periph/model.hpp>

#include "../fake_services.hpp"
#include "../support/machine_fixture.hpp"

#include <doctest/doctest.h>

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

class counting_firewall final : public periph::model, public periph::bus_firewall
{
public:
  counting_firewall(periph::model_context& ctx, bool filtering)
      : services{ctx.services}, filtering{filtering}
  {
    ctx.install_firewall(*this);
  }

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }
  void reset(periph::register_file&, popcorn::reset_kind) override
  {}
  auto on_read(periph::register_file&, std::uint32_t, std::uint8_t, periph::access_ctx)
      -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    return 0u;
  }
  auto on_write(
      periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t,
      periph::access_ctx) -> std::expected<void, popcorn::bus_error> override
  {
    return {};
  }
  void on_event(std::uint32_t, std::uint64_t) override
  {}
  void save(std::vector<std::byte>&) const override
  {}
  void load(std::span<std::byte const>) override
  {}

  auto check_peripheral(std::size_t, bus_attrs, memory_op) -> bus_verdict override
  {
    return bus_verdict::allow;
  }
  [[nodiscard]] auto filters_memory() const -> bool override
  {
    return filtering;
  }
  auto check_memory(address, std::uint8_t, bus_attrs, memory_op) -> bus_verdict override
  {
    ++checks;
    return verdict;
  }
  auto allowed_extent(address at, std::uint8_t, bus_attrs, memory_op)
      -> periph::memory_extent override
  {
    auto const first = at & ~(extent - 1u);
    return {.first = first, .last = first + extent - 1u};
  }

  core::machine_services& services;
  bool                    filtering{};
  std::size_t             checks{};
  bus_verdict             verdict = bus_verdict::allow;
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
