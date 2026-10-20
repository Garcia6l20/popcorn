#include <popcorn/core/clock.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/reset_listener.hpp>

#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace arch   = popcorn::arch;
namespace periph = popcorn::periph;
using popcorn::address;
using popcorn::icount;
using popcorn::reset_kind;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::m33;
using popcorn::testing::m33_code_alias;
using fixture = popcorn::testing::machine_fixture<>;

constexpr address       unmapped_page    = 0x3000'0000;
constexpr address       stub             = 0x2004'0000;
constexpr address       stub_code_alias  = 0x0a04'0000;
constexpr address       tag              = 0x2004'0100;
constexpr address       tag_code_alias   = 0x0a04'0100;
constexpr address       flash_word       = 0x081f'0000;
constexpr address       flash_word_alias = 0x001f'0000;
constexpr address       arena            = 0x6f00'0000;
constexpr std::uint32_t arena_size       = 0x1'0000;
constexpr std::uint32_t returns_42       = 0x4770'202a;

struct supply_event
{
  std::string name;
  char        what{};
  reset_kind  kind{};
  vtime       at{};

  friend auto operator==(supply_event const&, supply_event const&) -> bool = default;
};

using journal = std::vector<supply_event>;

class supply_model : public periph::model
{
public:
  supply_model(
      periph::model_context& ctx,
      std::string            name,
      journal&               events,
      bool                   throws,
      bool const*            reset_throws)
      : ctx_{ctx}, name_{std::move(name)}, events_{events}, throws_{throws},
        reset_throws_{reset_throws}
  {}

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }

  void reset(periph::register_file&, reset_kind kind) override
  {
    events_.push_back({.name = name_, .what = 'r', .kind = kind, .at = {}});
    if(reset_throws_ != nullptr and *reset_throws_)
    {
      throw std::runtime_error{name_ + " reset"};
    }
  }

  void power_cut() override
  {
    events_.push_back({.name = name_, .what = 'c', .kind = {}, .at = ctx_.clock.since_power_on()});
    if(throws_)
    {
      throw popcorn::model_error(name_, "CUT", 0);
    }
  }

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

private:
  periph::model_context& ctx_;
  std::string            name_;
  journal&               events_;
  bool                   throws_;
  bool const*            reset_throws_;
};

auto options_for(
    journal&    events,
    bool        first_throws        = false,
    bool const* second_reset_throws = nullptr) -> core::machine_options
{
  core::machine_options options;
  for(auto const& [name, throws, reset_throws] : {
          std::tuple{std::string{"TESTDEV"},    first_throws, static_cast<bool const*>(nullptr)},
          std::tuple{std::string{"UNMODELLED"}, false,        second_reset_throws              }
  })
  {
    options.models.emplace(
        name,
        [&events, name, throws, reset_throws](
            periph::model_context& ctx,
            popcorn::peripheral_desc const&) -> std::shared_ptr<periph::model> {
          return std::make_shared<supply_model>(ctx, name, events, throws, reset_throws);
        });
  }
  return options;
}

class listening_model final : public supply_model, public periph::reset_listener
{
public:
  listening_model(
      periph::model_context& ctx,
      std::string            name,
      journal&               events,
      bool const*            before_throws)
      : supply_model{ctx, name, events, false, nullptr}, ctx_{ctx}, name_{std::move(name)},
        events_{events}, before_throws_{before_throws}
  {}

  void before_reset(reset_kind kind) override
  {
    events_.push_back(
        {.name = name_, .what = 'b', .kind = kind, .at = ctx_.clock.since_power_on()});
    if(before_throws_ != nullptr and *before_throws_)
    {
      throw popcorn::model_error(name_, "BEFORE", 0);
    }
  }

private:
  periph::model_context& ctx_;
  std::string            name_;
  journal&               events_;
  bool const*            before_throws_;
};

auto listening_options(journal& events, bool const* before_throws = nullptr)
    -> core::machine_options
{
  auto options = options_for(events);
  options.models.insert_or_assign(
      "TESTDEV",
      [&events, before_throws](periph::model_context& ctx, popcorn::peripheral_desc const&)
          -> std::shared_ptr<periph::model> {
        return std::make_shared<listening_model>(ctx, "TESTDEV", events, before_throws);
      });
  return options;
}

auto call_at(fixture const& f, address entry) -> core::call_result
{
  return f.target->call(
      {
          .entry           = entry | 1u,
          .args            = {},
          .returns         = arch::value_kind::i32,
          .abi             = arch::float_abi::soft,
          .return_sentinel = unmapped_page,
      },
      icount{1000});
}

void write_word(fixture const& f, address at, std::uint32_t value)
{
  REQUIRE(f.target->mem().write(at, 4, value).has_value());
}

auto cut(std::string name, vtime at) -> supply_event
{
  return {.name = std::move(name), .what = 'c', .kind = {}, .at = at};
}

auto powered(std::string name) -> supply_event
{
  return {.name = std::move(name), .what = 'r', .kind = reset_kind::power_on, .at = {}};
}

auto cuts_of(journal const& events) -> std::size_t
{
  return static_cast<std::size_t>(std::ranges::count(events, 'c', &supply_event::what));
}
}

TEST_CASE("a power cycle cuts every model in description order, then resets each at power-on")
{
  journal events;
  fixture f{m33(), "calls", options_for(events)};
  write_word(f, stub, returns_42);
  REQUIRE(call_at(f, stub).value == 42);
  f.target->reset(reset_kind::software);
  REQUIRE(call_at(f, stub).value == 42);
  auto const at = f.target->virtual_clock().since_power_on();
  REQUIRE(at > f.target->virtual_clock().time());
  events.clear();
  f.target->power_cycle();
  CHECK(
      events
      == journal{
          cut("TESTDEV", at), cut("UNMODELLED", at), powered("TESTDEV"), powered("UNMODELLED")});
  CHECK(f.target->virtual_clock().time() == vtime{});
  CHECK(f.target->virtual_clock().since_power_on() == vtime{});
  events.clear();
  f.target->reset();
  CHECK(cuts_of(events) == 0);
}

TEST_CASE("a power cycle tells a reset listener once, first, before any cut or RAM zeroing")
{
  journal events;
  fixture f{m33(), "calls", listening_options(events)};
  write_word(f, stub, returns_42);
  REQUIRE(call_at(f, stub).value == 42);
  auto const at = f.target->virtual_clock().since_power_on();
  REQUIRE(at > vtime{});
  events.clear();
  f.target->power_cycle();
  CHECK(
      events
      == journal{
          {.name = "TESTDEV", .what = 'b', .kind = reset_kind::power_on, .at = at},
          cut("TESTDEV", at),
          cut("UNMODELLED", at),
          powered("TESTDEV"),
          powered("UNMODELLED"),
  });
}

TEST_CASE("a reset listener that throws in a power cycle refuses it before any change")
{
  journal events;
  bool    before_throws = false;
  fixture f{m33(), "calls", listening_options(events, &before_throws)};
  write_word(f, stub, returns_42);
  REQUIRE(call_at(f, stub).value == 42);
  write_word(f, tag, 0xa5a5'5a5a);
  auto const time = f.target->virtual_clock().time();
  REQUIRE(time > vtime{});
  events.clear();
  before_throws = true;
  CHECK_THROWS_AS(f.target->power_cycle(), popcorn::model_error);
  before_throws = false;
  REQUIRE(events.size() == 1);
  CHECK(events[0].what == 'b');
  CHECK(f.word(tag) == 0xa5a5'5a5a);
  CHECK(f.target->virtual_clock().time() == time);
  CHECK(call_at(f, stub).value == 42);
}

TEST_CASE("a power cycle zeroes RAM through every alias and keeps flash and the scratch region")
{
  journal events;
  auto    options = options_for(events);
  options.scratch = popcorn::address_range{.name = "", .start = arena, .size = arena_size};
  fixture f{m33_code_alias(), "calls", std::move(options)};
  write_word(f, tag, 0xa5a5'5a5a);
  write_word(f, flash_word, 0x1234'5678);
  write_word(f, arena, 0xcafe'f00d);
  REQUIRE(f.word(tag_code_alias) == 0xa5a5'5a5a);
  f.target->power_cycle();
  CHECK(f.word(tag) == 0);
  CHECK(f.word(tag_code_alias) == 0);
  CHECK(f.word(flash_word) == 0x1234'5678);
  CHECK(f.word(flash_word_alias) == 0x1234'5678);
  CHECK(f.word(arena) == 0xcafe'f00d);
}

TEST_CASE("code run from RAM before a power cycle does not run stale after it")
{
  journal events;
  fixture f{m33(), "calls", options_for(events)};
  write_word(f, stub, returns_42);
  auto const before = call_at(f, stub);
  REQUIRE(before.stopped.reason == stop_reason::returned);
  REQUIRE(before.value == 42);
  f.target->power_cycle();
  REQUIRE(f.word(stub) == 0);
  CHECK(call_at(f, stub).stopped.reason != stop_reason::returned);
}

TEST_CASE("a throwing cut still cuts every later model, zeroes RAM and resets, then rethrows")
{
  journal events;
  fixture f{m33(), "calls", options_for(events, true)};
  write_word(f, stub, returns_42);
  REQUIRE(call_at(f, stub).value == 42);
  write_word(f, tag, 0xa5a5'5a5a);
  auto const at = f.target->virtual_clock().since_power_on();
  REQUIRE(at > vtime{});
  events.clear();
  CHECK_THROWS_AS(f.target->power_cycle(), popcorn::model_error);
  CHECK(
      events
      == journal{
          cut("TESTDEV", at), cut("UNMODELLED", at), powered("TESTDEV"), powered("UNMODELLED")});
  CHECK(f.word(tag) == 0);
  CHECK(f.target->virtual_clock().time() == vtime{});
  write_word(f, stub, returns_42);
  auto const after = call_at(f, stub);
  CHECK(after.stopped.reason == stop_reason::returned);
  CHECK(after.value == 42);
}

TEST_CASE("a model reset that throws in a power cycle keeps the first cut exception")
{
  journal events;
  bool    reset_throws = false;
  fixture f{m33(), "calls", options_for(events, true, &reset_throws)};
  write_word(f, tag, 0xa5a5'5a5a);
  reset_throws = true;
  events.clear();
  CHECK_THROWS_AS(f.target->power_cycle(), popcorn::model_error);
  CHECK(cuts_of(events) == 2);
  CHECK(f.word(tag) == 0);
  CHECK(f.target->virtual_clock().time() == vtime{});
}

TEST_CASE("a model reset that throws alone in a power cycle is the exception rethrown")
{
  journal events;
  bool    reset_throws = false;
  fixture f{m33(), "calls", options_for(events, false, &reset_throws)};
  write_word(f, tag, 0xa5a5'5a5a);
  reset_throws = true;
  CHECK_THROWS_AS(f.target->power_cycle(), std::runtime_error);
  CHECK(f.word(tag) == 0);
}

TEST_CASE("a power cycle is refused as a reset is, before any model is cut")
{
  journal events;
  fixture f{m33(), "calls", options_for(events)};
  write_word(f, stub, returns_42);
  write_word(f, tag, 0xa5a5'5a5a);
  events.clear();
  SUBCASE("during a call begun by begin_call")
  {
    f.target->begin_call({
        .entry           = stub | 1u,
        .args            = {},
        .returns         = arch::value_kind::i32,
        .abi             = arch::float_abi::soft,
        .return_sentinel = unmapped_page,
    });
    CHECK_THROWS_AS(f.target->power_cycle(), popcorn::machine_busy);
    CHECK_THROWS_AS(f.target->reset(), popcorn::machine_busy);
    std::ignore = f.target->end_call();
  }
  SUBCASE("from a handler")
  {
    int refused = 0;
    std::ignore = f.run({
        .max_instructions = icount{100},
        .stop_when =
            [&] {
              CHECK_THROWS_AS(f.target->power_cycle(), popcorn::machine_running);
              CHECK_THROWS_AS(f.target->reset(), popcorn::machine_running);
              ++refused;
              return true;
            },
    });
    CHECK(refused > 0);
  }
  CHECK(events.empty());
  CHECK(f.word(tag) == 0xa5a5'5a5a);
}
