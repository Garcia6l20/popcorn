#include <popcorn/core/host.hpp>
#include <popcorn/core/machine.hpp>

#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"
#include "pty.hpp"

#include <doctest/doctest.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace
{
namespace core = popcorn::core;
namespace pty  = popcorn::bind::detail;
using namespace std::chrono_literals;

struct counting_link final : pty::pty_link
{
  std::atomic<std::uint64_t>* received;

  explicit counting_link(std::atomic<std::uint64_t>& count) : received{&count}
  {}

  void send(std::span<std::uint8_t const> bytes) override
  {
    *received += bytes.size();
  }

  void drain(std::vector<std::uint8_t>& out) override
  {
    out.push_back('y');
  }
};

auto idle_forever() -> std::unique_ptr<core::machine>
{
  popcorn::testing::machine_fixture<> f{popcorn::testing::m33(), "idle", core::machine_options{}};
  REQUIRE(f.run_to_stop().reason == popcorn::stop_reason::user);
  f.set("pc", popcorn::testing::sym("idle", "idle_forever"));
  return std::move(f.target);
}

auto settles(std::function_ref<bool()> reached) -> bool
{
  auto const until = std::chrono::steady_clock::now() + 5s;
  while(not reached())
  {
    if(std::chrono::steady_clock::now() > until)
    {
      return false;
    }
    std::this_thread::sleep_for(100us);
  }
  return true;
}
}

TEST_CASE("bridges opened and closed while a free run pumps them deliver their input")
{
  auto                                        ptys = std::make_shared<pty::pty_bridges>();
  std::atomic<std::uint64_t>                  received{0};
  std::vector<std::unique_ptr<core::machine>> members;
  members.push_back(idle_forever());
  core::host on{std::move(members), {}, {.boundary = [weak = std::weak_ptr{ptys}](core::member_id) {
                  if(auto const alive = weak.lock())
                  {
                    std::ignore = pty::pump_all(*alive);
                  }
                }}};
  {
    auto held = on.lock();
    on.start(held, 0);
  }
  for(int round = 0; round < 50; ++round)
  {
    auto bridge =
        std::make_shared<pty::pty_bridge>("USART1", std::make_unique<counting_link>(received));
    {
      std::unique_lock lock{ptys->mutex};
      pty::add_bridge(*ptys, lock, bridge);
    }
    auto const watched = on.watch(0, bridge->master(), [wants = bridge->wants_write_flag()] {
      return wants->load(std::memory_order_relaxed);
    });
    auto const client  = ::open(bridge->path().c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    REQUIRE(client >= 0);
    auto const before = received.load();
    REQUIRE(::write(client, "x", 1) == 1);
    CHECK(settles([&] { return received.load() > before; }));
    ::close(client);
    on.unwatch(watched);
    std::shared_ptr<pty::pty_bridge> closing;
    {
      std::unique_lock lock{ptys->mutex};
      closing = pty::remove_bridge(*ptys, lock, bridge.get());
    }
    CHECK(closing == bridge);
  }
  CHECK_FALSE(on.halt(0).has_value());
}

namespace
{
struct flooding_link final : pty::pty_link
{
  int drains = 0;

  void send(std::span<std::uint8_t const>) override
  {}

  void drain(std::vector<std::uint8_t>& out) override
  {
    if(++drains == 2)
    {
      out.resize(out.size() + (128 * 1024), 'z');
    }
  }
};

struct refused : std::runtime_error
{
  using std::runtime_error::runtime_error;
};
}

TEST_CASE("a log sink throwing on the overflow warning ends the free run or fails the goal")
{
  for(auto const goal : {false, true})
  {
    CAPTURE(goal);
    auto ptys   = std::make_shared<pty::pty_bridges>();
    auto bridge = std::make_shared<pty::pty_bridge>("USART1", std::make_unique<flooding_link>());
    {
      std::unique_lock lock{ptys->mutex};
      pty::add_bridge(*ptys, lock, bridge);
    }
    auto                     engine = idle_forever();
    std::vector<std::string> logged;
    auto const               sink = engine->add_log_sink([&logged](core::log_record const& record) {
      logged.push_back(record.peripheral + "|" + record.message);
      throw refused{"sink refused"};
    });
    auto&                    target = *engine;
    std::vector<std::unique_ptr<core::machine>> members;
    members.push_back(std::move(engine));
    core::host on{std::move(members), {}, {.boundary = [&ptys, &target](core::member_id) {
                    pty::pump_and_log(*ptys, target);
                  }}};
    if(goal)
    {
      auto held    = on.lock();
      auto running = on.run(held, 0, {.until_time = popcorn::vtime{1'000'000'000}});
      held.unlock();
      CHECK_THROWS_AS(running.get(), refused);
    }
    else
    {
      {
        auto held = on.lock();
        on.start(held, 0);
      }
      REQUIRE(settles([&] { return on.state(0) == core::host_state::parked; }));
      auto const end = on.halt(0);
      REQUIRE(end.has_value());
      CHECK_THROWS_AS(std::rethrow_exception(std::get<std::exception_ptr>(*end)), refused);
    }
    REQUIRE(logged.size() == 1);
    CHECK(logged.front().starts_with("USART1|USART1: no host tool reads " + bridge->path()));
  }
}

TEST_CASE("a failed poll logs one warning per watched member and still wakes it")
{
  std::array<int, 2> first{};
  std::array<int, 2> second{};
  REQUIRE(::pipe2(first.data(), O_CLOEXEC) == 0);
  REQUIRE(::pipe2(second.data(), O_CLOEXEC) == 0);
  std::mutex                 logging;
  std::vector<std::string>   logged;
  std::atomic<std::uint64_t> boundaries{0};
  auto                       engine = idle_forever();
  auto const                 sink   = engine->add_log_sink([&](core::log_record const& record) {
    std::scoped_lock const lock{logging};
    logged.push_back(record.message);
  });
  std::vector<std::unique_ptr<core::machine>> members;
  members.push_back(std::move(engine));
  core::host on{
      std::move(members), {}, {.boundary = [&boundaries](core::member_id) { ++boundaries; }}};
  {
    auto held = on.lock();
    on.start(held, 0);
  }
  REQUIRE(settles([&] { return on.state(0) == core::host_state::idle_wait; }));
  rlimit saved{};
  REQUIRE(::getrlimit(RLIMIT_NOFILE, &saved) == 0);
  rlimit lowered   = saved;
  lowered.rlim_cur = 2;
  REQUIRE(::setrlimit(RLIMIT_NOFILE, &lowered) == 0);
  auto const one         = on.watch(0, first[0], [] { return false; });
  auto const two         = on.watch(0, second[0], [] { return false; });
  auto const logged_once = settles([&] {
    std::scoped_lock const lock{logging};
    return not logged.empty();
  });
  auto const before      = boundaries.load();
  REQUIRE(::write(first[1], "x", 1) == 1);
  auto const woken = settles([&] { return boundaries.load() > before + 2; });
  REQUIRE(::setrlimit(RLIMIT_NOFILE, &saved) == 0);
  CHECK(logged_once);
  CHECK(woken);
  {
    std::scoped_lock const lock{logging};
    REQUIRE(logged.size() == 1);
    CHECK(logged.front() == "poll failed with EINVAL, pty input may be late by up to 10 ms");
  }
  on.unwatch(one);
  on.unwatch(two);
  std::ignore = on.halt(0);
  for(auto const fd : {first[0], first[1], second[0], second[1]})
  {
    ::close(fd);
  }
}
