#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/services.hpp>

#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"
#include "../support/test_model.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::icount;
using popcorn::initiator;
using popcorn::stop_reason;
using popcorn::testing::m33;
using popcorn::testing::sym;
using popcorn::testing::test_model;
using fixture = popcorn::testing::machine_fixture<>;

auto booted(std::string_view firmware = "intercepts") -> fixture
{
  core::machine_options options;
  options.models.emplace("TESTDEV", popcorn::testing::test_model_factory());
  fixture f{m33(), firmware, options};
  std::ignore = f.run_to_stop();
  return f;
}

auto fn(std::string_view name) -> address
{
  return sym("intercepts", name);
}

auto recorder(
    std::vector<core::watch_hit>& hits,
    core::watch_action            action = core::watch_action::resume) -> core::watch_handler
{
  return [&hits, action](core::watch_hit const& hit) {
    hits.push_back(hit);
    return action;
  };
}
}

TEST_CASE("a write watch stops at the next block boundary and names the writer")
{
  auto                         f       = booted();
  auto const                   counter = fn("counter");
  std::vector<core::watch_hit> hits;
  auto const                   watch = f.target->add_watch(
      counter, 4, core::watch_kind::write, recorder(hits, core::watch_action::stop));
  f.set("pc", fn("bump"));
  auto const stop = f.run_to_stop();
  CHECK(stop.reason == stop_reason::watchpoint);
  REQUIRE(hits.size() == 1);
  CHECK(hits[0].pc == fn("bump_store"));
  CHECK(hits[0].addr == counter);
  CHECK(hits[0].write);
  CHECK(hits[0].value == 1);
  CHECK(hits[0].by == initiator::cpu);
  CHECK(stop.detail.contains(std::format("{:#010x}", fn("bump_store"))));
  CHECK(stop.where.pc != fn("bump_store"));
}

TEST_CASE("a read watch sees the value in memory, and the debugger never hits a watch")
{
  auto                         f       = booted();
  auto const                   counter = fn("counter");
  std::vector<core::watch_hit> hits;
  auto const watch = f.target->add_watch(counter, 4, core::watch_kind::read, recorder(hits));
  REQUIRE(f.target->mem().write(counter, 4, 0x1234));
  CHECK(f.target->mem().read(counter, 4) == 0x1234u);
  CHECK(hits.empty());
  f.set("pc", fn("bump"));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  REQUIRE(hits.size() == 2);
  CHECK(hits[0].value == 0x1234);
  CHECK(hits[1].value == 0x1235);
  CHECK_FALSE(hits[0].write);
}

TEST_CASE("a DMA access through services hits a watch as dma")
{
  auto                         f       = booted();
  auto const                   counter = fn("counter");
  std::vector<core::watch_hit> hits;
  auto const watch    = f.target->add_watch(counter, 4, core::watch_kind::access, recorder(hits));
  auto&      services = f.module->attached_services();
  REQUIRE(services.write(counter, 4, 9, initiator::dma));
  std::array<std::byte, 8> block{};
  REQUIRE(services.read_block(counter - 2, block, initiator::dma));
  REQUIRE(hits.size() == 2);
  CHECK(hits[0].by == initiator::dma);
  CHECK(hits[0].write);
  CHECK(hits[0].value == 9);
  CHECK_FALSE(hits[1].write);
  CHECK(hits[1].addr == counter);
  CHECK(hits[1].size == 4);
  CHECK(hits[1].value == 9);
}

TEST_CASE("a watch on a peripheral register fires from firmware with an exact pc")
{
  auto                         f = booted();
  std::vector<core::watch_hit> hits;
  auto const watch = f.target->add_watch(0x4001'0008, 4, core::watch_kind::write, recorder(hits));
  f.set("pc", fn("poke_testdev"));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  REQUIRE(hits.size() == 1);
  CHECK(hits[0].addr == 0x4001'0008);
  CHECK(hits[0].value == 0x5a);
  CHECK(hits[0].pc == fn("poke_testdev") + 4);
}

TEST_CASE("a watch makes model access pcs exact, and removing it restores the block start")
{
  auto       f                = booted("periph");
  auto&      model            = dynamic_cast<test_model&>(*f.target->model("TESTDEV"));
  auto const run_model_access = [&] {
    model.accesses.clear();
    f.set("pc", sym("periph", "model_access"));
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
    REQUIRE(model.accesses.size() == 2);
    return model.accesses[0].ctx.pc;
  };
  CHECK(run_model_access() == sym("periph", "model_access"));
  std::vector<core::watch_hit> hits;
  auto watch = f.target->add_watch(0x2007'0000, 4, core::watch_kind::access, recorder(hits));
  CHECK(run_model_access() == sym("periph", "model_read"));
  CHECK(model.accesses[1].ctx.pc == sym("periph", "model_write"));
  watch.reset();
  CHECK(run_model_access() == sym("periph", "model_access"));
}

TEST_CASE("a watch handler cannot remove its watch during the slice")
{
  auto                             f = booted();
  std::optional<core::hook_handle> watch;
  watch =
      f.target->add_watch(fn("counter"), 4, core::watch_kind::write, [&](core::watch_hit const&) {
        watch->reset();
        return core::watch_action::resume;
      });
  f.set("pc", fn("bump"));
  CHECK_THROWS_AS(std::ignore = f.run(), popcorn::machine_running);
}

TEST_CASE("a watch covers every mapping of the watched bytes and reports the accessed address")
{
  auto                         f = booted();
  std::vector<core::watch_hit> hits;
  SUBCASE("FLASH read through FLASH_ALIAS")
  {
    auto const word  = fn("flash_word");
    auto const watch = f.target->add_watch(word, 4, core::watch_kind::read, recorder(hits));
    f.set("pc", fn("read_flash_alias"));
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].addr == word - 0x0800'0000);
    CHECK(hits[0].value == 0xC0DE'F00Du);
  }
  SUBCASE("TESTDEV written through SEC_TESTDEV")
  {
    auto const watch = f.target->add_watch(0x4001'0008, 4, core::watch_kind::write, recorder(hits));
    f.set("pc", fn("poke_sec_testdev"));
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].addr == 0x5001'0008);
    CHECK(hits[0].value == 0xa5);
  }
  SUBCASE("SEC_TESTDEV watched, TESTDEV written")
  {
    auto const watch = f.target->add_watch(0x5001'0008, 4, core::watch_kind::write, recorder(hits));
    f.set("pc", fn("poke_testdev"));
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].addr == 0x4001'0008);
  }
}

TEST_CASE("a watch over no memory and no peripheral window is a configuration error")
{
  auto f = booted();
  CHECK_THROWS_AS(
      std::ignore = f.target->add_watch(0x3000'0000, 4, core::watch_kind::write, {}),
      popcorn::config_error);
}

TEST_CASE("a store starting below a watched byte and overlapping it hits the watch")
{
  auto                         f       = booted();
  auto const                   counter = fn("counter");
  std::vector<core::watch_hit> hits;
  auto const watch = f.target->add_watch(counter + 2, 1, core::watch_kind::write, recorder(hits));
  f.set("pc", fn("bump"));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  REQUIRE(hits.size() == 1);
  CHECK(hits[0].addr == counter);
  CHECK(hits[0].size == 4);
}
