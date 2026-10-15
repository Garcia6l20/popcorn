#include <popcorn/core/host.hpp>
#include <popcorn/core/machine.hpp>

#include "../support/machine_fixture.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <memory>
#include <utility>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::icount;
using popcorn::testing::m33;

auto members() -> std::vector<std::unique_ptr<core::machine>>
{
  popcorn::testing::machine_fixture<> fixture{m33(), "alu", core::machine_options{}};
  fixture.set("r0", 10);
  std::vector<std::unique_ptr<core::machine>> made;
  made.push_back(std::move(fixture.target));
  return made;
}
}

TEST_CASE("a host lock counted inside libpopcorn_core.so is the count the executable reads")
{
  core::host shared{members()};
  REQUIRE(core::detail::host_locks_held == 0);
  {
    auto const held = shared.lock();
    REQUIRE(held.owns_lock());
    CHECK(core::detail::host_locks_held == 1);
  }
  CHECK(core::detail::host_locks_held == 0);
}

TEST_CASE("an engine owner set inside libpopcorn_core.so is the owner the executable reads")
{
  core::host shared{members()};
  REQUIRE(core::detail::engine_owner == nullptr);
  auto const held = shared.lock();
  {
    core::owner_scope const owning{shared, held};
    CHECK(core::detail::engine_owner != nullptr);
    CHECK(shared.owns_engine());
  }
  CHECK(core::detail::engine_owner == nullptr);
}

TEST_CASE("the host thread flag set inside libpopcorn_core.so is the flag the executable reads")
{
  std::atomic<bool> seen{false};
  core::host_hooks  hooks{.thread_started = [&seen] { seen.store(core::detail::on_host_thread); }};
  core::host        shared{members(), {}, std::move(hooks)};
  core::host_future<core::goal_result> done;
  {
    auto const held = shared.lock();
    done            = shared.run(held, 0, core::run_goal{.budget = icount{100}});
  }
  done.get();
  CHECK(seen.load());
  CHECK_FALSE(core::detail::on_host_thread);
}
