#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/model.hpp>

#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace periph = popcorn::periph;
using popcorn::address;
using popcorn::icount;
using popcorn::reset_kind;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::m33;

class horizon_arch final : public popcorn::testing::test_arch
{
public:
  using test_arch::test_arch;

  bool                    asleep  = true;
  bool                    looping = false;
  bool                    pends   = true;
  std::set<std::uint32_t> waking{};

  [[nodiscard]] auto idle() const -> bool override
  {
    return asleep;
  }

  [[nodiscard]] auto self_branch(address) const -> bool override
  {
    return looping;
  }

  [[nodiscard]] auto may_wake(std::uint32_t source) const -> bool override
  {
    return waking.contains(source);
  }

  [[nodiscard]] auto wake_pending() const -> bool override
  {
    return false;
  }

  [[nodiscard]] auto event_pends(std::uint32_t, std::uint64_t) const -> bool override
  {
    return pends;
  }
};

class probe_model final : public periph::model
{
public:
  probe_model(periph::model_context& ctx, std::optional<periph::wake_role> role) noexcept
      : context_{ctx}, role_{role}
  {}

  std::optional<std::optional<vtime>>              quiet{};
  std::optional<std::pair<address, std::uint64_t>> ask{};
  mutable std::optional<bool>                      watched{};
  std::vector<address>                             units_at{};
  mutable std::vector<std::pair<bool, bool>>       units{};

  void watch_reads(address at, std::uint32_t size)
  {
    context_.services.watch_reads(at, size, true);
  }

  void arm(vtime at)
  {
    std::ignore = context_.events.schedule_at(at, 0, 0, context_.clock);
  }

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }

  void reset(periph::register_file&, reset_kind) override
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

  [[nodiscard]] auto stateless() const -> bool override
  {
    return true;
  }

  [[nodiscard]] auto quiet_until(periph::quiet_query const& query) const
      -> std::optional<vtime> override
  {
    if(ask)
    {
      watched = query.read_watched(ask->first, ask->second);
    }
    units.clear();
    for(auto const at : units_at)
    {
      units.emplace_back(query.peripheral_at(at), query.opaque_at(at));
    }
    return quiet ? *quiet : query.next_event(*this);
  }

  [[nodiscard]] auto role_in_horizon() const noexcept -> periph::wake_role override
  {
    return role_ ? *role_ : periph::model::role_in_horizon();
  }

private:
  periph::model_context&           context_;
  std::optional<periph::wake_role> role_;
};

constexpr std::uint32_t first_line = 4;
constexpr std::uint64_t step_ps    = 250'000;

auto desc() -> popcorn::part_description
{
  auto out = m33();
  for(std::uint32_t index = 0; index < 4; ++index)
  {
    popcorn::peripheral_desc unit;
    unit.name = std::format("P{}", index);
    unit.ip   = "PROBE";
    unit.base = 0x4003'0000 + 0x400 * index;
    unit.size = 0x400;
    unit.irqs = {static_cast<std::uint16_t>(first_line + index)};
    out.peripherals.push_back(std::move(unit));
  }
  return out;
}

struct rig
{
  std::vector<std::optional<periph::wake_role>>                    roles;
  std::vector<probe_model*>                                        probes;
  popcorn::testing::machine_fixture<horizon_arch>*                 fixture{};
  std::unique_ptr<popcorn::testing::machine_fixture<horizon_arch>> owned;

  explicit rig(
      std::vector<std::optional<periph::wake_role>> chosen,
      core::machine_options                         options     = {},
      popcorn::part_description                     description = desc())
      : roles{std::move(chosen)}, probes(roles.size(), nullptr)
  {
    for(std::size_t index = 0; index < roles.size(); ++index)
    {
      options.models.emplace(
          std::format("P{}", index),
          [this, index](periph::model_context& ctx, popcorn::peripheral_desc const&) {
            auto made     = std::make_shared<probe_model>(ctx, roles[index]);
            probes[index] = made.get();
            return std::shared_ptr<periph::model>{made};
          });
    }
    owned = std::make_unique<popcorn::testing::machine_fixture<horizon_arch>>(
        std::move(description), "idle", std::move(options));
    fixture = owned.get();
  }

  [[nodiscard]] auto arch() const -> horizon_arch&
  {
    return *fixture->module;
  }

  [[nodiscard]] auto machine() const -> core::machine&
  {
    return *fixture->target;
  }

  [[nodiscard]] auto at(std::uint64_t steps) const -> vtime
  {
    return vtime{steps * step_ps};
  }
};

using role = periph::wake_role;
}

TEST_CASE("a lines model bounds the wake only while one of its lines wakes the core")
{
  rig r{{role::lines}};
  r.probes[0]->arm(r.at(10));
  auto h = r.machine().horizon();
  CHECK(h.asleep);
  CHECK_FALSE(h.wake.has_value());
  r.arch().waking.insert(first_line);
  h = r.machine().horizon();
  REQUIRE(h.wake.has_value());
  CHECK(*h.wake == r.at(10));
  CHECK_FALSE(h.stop.has_value());
}

TEST_CASE("a trigger model bounds the wake, every transmit and the stop by its earliest event")
{
  rig r{
      {role::trigger, role::lines}
  };
  r.machine().set_transmit_sources({{"P1"}, {}});
  r.probes[0]->arm(r.at(30));
  r.probes[0]->arm(r.at(20));
  r.probes[0]->quiet = std::optional<vtime>{};
  auto const h       = r.machine().horizon();
  REQUIRE(h.wake.has_value());
  CHECK(*h.wake == r.at(20));
  REQUIRE(h.transmit.size() == 2);
  CHECK(h.transmit[0] == std::optional{r.at(20)});
  CHECK(h.transmit[1] == std::optional{r.at(20)});
  CHECK(h.stop == std::optional{r.at(20)});
}

TEST_CASE("a reset source bounds the wake and the stop, not the transmit")
{
  rig r{{role::reset_source}};
  r.machine().set_transmit_sources({{"P0"}});
  r.probes[0]->arm(r.at(40));
  auto const h = r.machine().horizon();
  CHECK(h.wake == std::optional{r.at(40)});
  CHECK(h.stop == std::optional{r.at(40)});
  REQUIRE(h.transmit.size() == 1);
  CHECK_FALSE(h.transmit[0].has_value());
}

TEST_CASE("a bus master's own answer bounds the wake and every transmit whatever its lines")
{
  rig r{{role::bus_master}};
  r.machine().set_transmit_sources({{}});
  r.probes[0]->arm(r.at(5));
  r.probes[0]->quiet = std::optional{r.at(50)};
  auto const h       = r.machine().horizon();
  CHECK(h.wake == std::optional{r.at(50)});
  REQUIRE(h.transmit.size() == 1);
  CHECK(h.transmit[0] == std::optional{r.at(50)});
}

TEST_CASE("an opaque model bounds the wake, every transmit and the stop by its earliest event")
{
  rig r{{role::opaque}};
  r.machine().set_transmit_sources({{}});
  r.probes[0]->arm(r.at(60));
  r.probes[0]->quiet = std::optional<vtime>{};
  auto const h       = r.machine().horizon();
  CHECK(h.wake == std::optional{r.at(60)});
  CHECK(h.transmit == std::vector{std::optional{r.at(60)}});
  CHECK(h.stop == std::optional{r.at(60)});
}

TEST_CASE("python_backed counts a lines model as opaque")
{
  core::machine_options options;
  options.python_backed = {"P0"};
  rig r{{role::lines}, options};
  r.probes[0]->arm(r.at(70));
  r.probes[0]->quiet = std::optional<vtime>{};
  auto const h       = r.machine().horizon();
  CHECK(h.wake == std::optional{r.at(70)});
  CHECK(h.stop == std::optional{r.at(70)});
}

TEST_CASE("a model that declares no role counts as opaque")
{
  rig r{{std::nullopt}};
  r.machine().set_transmit_sources({{}});
  r.probes[0]->arm(r.at(65));
  r.probes[0]->quiet = std::optional<vtime>{};
  auto const h       = r.machine().horizon();
  CHECK(h.wake == std::optional{r.at(65)});
  CHECK(h.transmit == std::vector{std::optional{r.at(65)}});
  CHECK(h.stop == std::optional{r.at(65)});
  r.arch().asleep = false;
  CHECK(r.machine().horizon().stop == std::optional{r.at(65)});
}

TEST_CASE("python_backed naming a peripheral the description lacks is refused")
{
  core::machine_options options;
  options.python_backed = {"NOPE"};
  CHECK_THROWS_AS(rig({role::lines}, options), popcorn::config_error);
}

TEST_CASE("each wire's transmit takes the quiet time of its own lines sources only")
{
  rig r{
      {role::lines, role::lines, role::lines}
  };
  r.machine().set_transmit_sources({
      {"P0"},
      {"P1", "SEC_TESTDEV"}
  });
  r.probes[0]->arm(r.at(11));
  r.probes[1]->arm(r.at(12));
  r.probes[2]->arm(r.at(3));
  auto const h = r.machine().horizon();
  REQUIRE(h.transmit.size() == 2);
  CHECK(h.transmit[0] == std::optional{r.at(11)});
  CHECK(h.transmit[1] == std::optional{r.at(12)});
  CHECK_FALSE(h.wake.has_value());
}

TEST_CASE("transmit sources naming a peripheral the description lacks are refused unchanged")
{
  rig r{{role::lines}};
  r.machine().set_transmit_sources({{"P0"}});
  CHECK_THROWS_AS(r.machine().set_transmit_sources({{"P0"}, {"NOPE"}}), popcorn::config_error);
  CHECK(r.machine().horizon().transmit.size() == 1);
}

TEST_CASE("an arch event bounds the wake only when it can pend an exception")
{
  rig r{{}};
  std::ignore    = r.arch().attached_events().schedule(icount{8}, 7, 0);
  r.arch().pends = false;
  CHECK_FALSE(r.machine().horizon().wake.has_value());
  r.arch().pends = true;
  CHECK(r.machine().horizon().wake == std::optional{r.at(8)});
}

TEST_CASE("an awake core reports no wake and no transmit, its stop sources still count")
{
  rig r{
      {role::lines, role::reset_source}
  };
  r.machine().set_transmit_sources({{"P0"}});
  r.arch().asleep = false;
  r.arch().waking.insert(first_line);
  r.probes[0]->arm(r.at(4));
  r.probes[1]->arm(r.at(9));
  auto const h = r.machine().horizon();
  CHECK_FALSE(h.asleep);
  CHECK_FALSE(h.wake.has_value());
  CHECK(h.transmit.empty());
  CHECK(h.stop == std::optional{r.at(9)});
}

TEST_CASE("a held core sleeps with no wake while its transmit sources still count")
{
  rig r{
      {role::lines, role::trigger}
  };
  r.machine().set_transmit_sources({{"P0"}});
  r.arch().asleep = false;
  r.arch().waking.insert(first_line);
  r.probes[0]->arm(r.at(6));
  r.probes[1]->arm(r.at(8));
  r.machine().hold_core(true);
  auto const h = r.machine().horizon();
  CHECK(h.asleep);
  CHECK_FALSE(h.wake.has_value());
  CHECK(h.transmit == std::vector{std::optional{r.at(6)}});
}

TEST_CASE("while a watch exists the earliest model event bounds the stop")
{
  rig r{
      {role::lines, role::lines}
  };
  r.probes[1]->arm(r.at(21));
  r.probes[0]->arm(r.at(25));
  CHECK_FALSE(r.machine().horizon().stop.has_value());
  auto const watch =
      r.machine().add_watch(0x2000'0000, 4, core::watch_kind::write, [](core::watch_hit const&) {
        return core::watch_action::resume;
      });
  CHECK(r.machine().horizon().stop == std::optional{r.at(21)});
}

TEST_CASE("in a b . loop the instructions left bound the stop")
{
  rig r{{}};
  r.arch().asleep  = false;
  r.arch().looping = true;
  auto const h     = r.machine().horizon(icount{40});
  CHECK(h.asleep);
  CHECK(h.stop == std::optional{r.at(40)});
  CHECK_FALSE(r.machine().horizon().stop.has_value());
  r.arch().looping = false;
  CHECK_FALSE(r.machine().horizon(icount{40}).asleep);
}

TEST_CASE("a b . loop the run executes, under spin detection or a code site, is awake")
{
  core::machine_options spinning;
  spinning.spin_threshold = 1'000;
  rig s{{}, spinning};
  s.arch().asleep  = false;
  s.arch().looping = true;
  CHECK_FALSE(s.machine().horizon(icount{40}).asleep);
  rig r{{}};
  r.arch().asleep  = false;
  r.arch().looping = true;
  REQUIRE(r.machine().horizon().asleep);
  auto const probe = r.machine().add_probe(r.arch().pc());
  CHECK_FALSE(r.machine().horizon(icount{40}).asleep);
}

TEST_CASE("an event already due bounds at the machine's time")
{
  rig r{{role::trigger}};
  r.probes[0]->arm(vtime{0});
  CHECK(r.machine().horizon().wake == std::optional{r.machine().virtual_clock().time()});
}

namespace
{
auto watching_rig() -> std::unique_ptr<rig>
{
  auto description  = desc();
  auto region       = description.regions.front();
  region.name       = "PMA";
  region.start      = 0x4004'0000;
  region.size       = 0x400;
  region.physical   = std::nullopt;
  region.controller = "P0";
  description.regions.push_back(std::move(region));
  auto made = std::make_unique<rig>(
      std::vector<std::optional<periph::wake_role>>{role::bus_master}, core::machine_options{},
      std::move(description));
  made->probes[0]->watch_reads(0x4004'0000, 16);
  return made;
}

auto read_watched(rig& r, address at, std::uint64_t size) -> bool
{
  r.probes[0]->ask     = std::pair{at, size};
  r.probes[0]->watched = std::nullopt;
  std::ignore          = r.machine().horizon();
  REQUIRE(r.probes[0]->watched.has_value());
  return *r.probes[0]->watched;
}
}

TEST_CASE("a read inside one peripheral window is not watched, one past its end is")
{
  auto const r = watching_rig();
  CHECK_FALSE(read_watched(*r, 0x4003'03f0, 16));
  CHECK(read_watched(*r, 0x4003'03f8, 16));
}

TEST_CASE("a read of a watched region counts as watched")
{
  auto const r = watching_rig();
  CHECK(read_watched(*r, 0x4004'0000, 16));
  CHECK(read_watched(*r, 0x4003'fff8, 16));
}

namespace
{
auto units_seen(
    std::vector<std::optional<periph::wake_role>> roles,
    popcorn::part_description                     d,
    std::vector<address>                          extra = {}) -> std::vector<std::pair<bool, bool>>
{
  rig r{std::move(roles), core::machine_options{}, std::move(d)};
  r.probes[0]->units_at = {0x4002'fffc, 0x4003'0000, 0x4003'03fc, 0x4003'0400,
                           0x4003'07fc, 0x4003'0800, 0x4003'0c00, 0x4003'1000};
  r.probes[0]->units_at.insert(r.probes[0]->units_at.end(), extra.begin(), extra.end());
  std::ignore = r.machine().horizon();
  return r.probes[0]->units;
}
}

TEST_CASE("the horizon finds the unit serving an address at each window's edges")
{
  using seen = std::pair<bool, bool>;
  auto const units =
      units_seen({role::bus_master, role::opaque, role::lines, role::opaque}, desc());
  CHECK(
      units
      == std::vector<seen>{
          {false, false},
          {true,  false},
          {true,  false},
          {true,  true },
          {true,  true },
          {true,  false},
          {true,  true },
          {false, false}
  });
}

TEST_CASE("the horizon finds the unit serving an address whatever the description's order")
{
  using seen        = std::pair<bool, bool>;
  auto  description = desc();
  auto& units       = description.peripherals;
  std::ranges::reverse(units.end() - 4, units.end());
  popcorn::peripheral_desc alias;
  alias.name     = "P1_ALIAS";
  alias.ip       = "PROBE";
  alias.base     = 0x5003'0400;
  alias.size     = 0x400;
  alias.alias_of = "P1";
  units.insert(units.begin(), std::move(alias));
  auto const seen_units = units_seen(
      {role::bus_master, role::opaque, role::lines, role::opaque}, std::move(description),
      {0x5003'03fc, 0x5003'0400, 0x5003'07fc, 0x5003'0800});
  CHECK(
      seen_units
      == std::vector<seen>{
          {false, false},
          {true,  false},
          {true,  false},
          {true,  true },
          {true,  true },
          {true,  false},
          {true,  true },
          {false, false},
          {false, false},
          {true,  true },
          {true,  true },
          {false, false}
  });
}
