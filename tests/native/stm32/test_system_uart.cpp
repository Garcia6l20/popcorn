#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/host.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/member_link.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/core/system.hpp>
#include <popcorn/core/window_barrier.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/uart.hpp>
#include <popcorn/periph/stm32/uart_wire.hpp>

#include <doctest/doctest.h>

#include <sched.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
namespace core  = popcorn::core;
namespace stm32 = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;
using popcorn::testing::u575;

constexpr std::int64_t  us_ps      = 1'000'000;
constexpr std::int64_t  ms_ps      = 1'000'000'000;
constexpr std::int64_t  cycle_ps   = 6'250;
constexpr std::int64_t  latency_20 = 20 * us_ps;
constexpr std::int64_t  script_end = 3 * ms_ps;
constexpr std::uint32_t link_byte  = 1;

struct node
{
  std::shared_ptr<stm32::capture_endpoint> own   = std::make_shared<stm32::capture_endpoint>();
  std::shared_ptr<stm32::capture_endpoint> spare = std::make_shared<stm32::capture_endpoint>();
  stm32::board                             board{};
  std::unique_ptr<stm32_fixture>           f;
  core::machine*                           target{};
  std::int64_t                             origin{};
  stm32::uart_port*                        port{};
  stm32::uart_port*                        spare_port{};
  std::vector<core::log_record>            warnings;
  core::hook_handle                        sink;

  node(std::string_view entry, std::int64_t start_ps, bool strict)
  {
    board.uarts.emplace("USART1", own);
    board.uarts.emplace("USART2", spare);
    auto options               = popcorn::testing::stm32_options();
    options.models             = stm32::factories(board, u575());
    options.strict_peripherals = strict;
    f      = std::make_unique<stm32_fixture>(u575(), "usart_ll", std::move(options));
    target = f->target.get();
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("usart_ll", "main")})
            .reason
        == stop_reason::address);
    if(start_ps != 0)
    {
      f->set("pc", sym("usart_ll", "idle"));
      REQUIRE(
          f->run_to_stop({.max_instructions = icount{1'000'000'000},
                          .until_time       = vtime{static_cast<std::uint64_t>(start_ps)}})
              .reason
          == stop_reason::time);
    }
    f->set("pc", sym("usart_ll", entry));
    origin     = static_cast<std::int64_t>(target->virtual_clock().time().ps);
    port       = f->model_of<stm32::uart_port>("USART1");
    spare_port = f->model_of<stm32::uart_port>("USART2");
    REQUIRE(port != nullptr);
    REQUIRE(spare_port != nullptr);
    sink = target->add_log_sink([this](core::log_record const& record) {
      if(record.kind == core::log_kind::model_warning)
      {
        warnings.push_back(record);
      }
    });
  }

  [[nodiscard]] auto log() const -> std::vector<std::uint32_t>
  {
    auto const                 count = word(sym("usart_ll", "log_len"));
    std::vector<std::uint32_t> out;
    for(std::uint32_t index = 0; index < count and index < 256; ++index)
    {
      out.push_back(word(sym("usart_ll", "log_buf") + 4 * index));
    }
    return out;
  }

  [[nodiscard]] auto word(popcorn::address at) const -> std::uint32_t
  {
    auto const value = target->mem().read(at, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  [[nodiscard]] auto map() const -> core::time_map
  {
    return core::time_map{core::sys_time{-origin}};
  }

  auto advance(std::int64_t system_ps) -> stop_reason
  {
    for(;;)
    {
      auto const result = f->run(
          {.max_instructions = icount{1'000'000'000},
           .wall_budget      = std::chrono::seconds{60},
           .until_time       = vtime{static_cast<std::uint64_t>(system_ps + origin)}});
      if(result.stopped)
      {
        return result.stopped->reason;
      }
    }
  }
};

struct variant
{
  std::string_view             name;
  std::int64_t                 lowered{};
  core::slice_bounds           bounds{};
  std::optional<std::uint32_t> seed{};
  bool                         one_cpu{};
};

auto variants() -> std::vector<variant>
{
  core::slice_bounds const fine{
      .max_instructions = icount{997}, .wall_budget = std::chrono::microseconds{10}};
  return {
      {.name = "q=l"},
      {.name = "q=l/3", .lowered = 3},
      {.name = "997", .bounds = fine},
      {.name = "997 q=l/3", .lowered = 3, .bounds = fine},
      {.name = "sleeps 1", .seed = 1},
      {.name = "sleeps 2 q=l/3", .lowered = 3, .seed = 2},
      {.name = "sleeps 3 997", .bounds = fine, .seed = 3},
      {.name = "one cpu", .one_cpu = true},
      {.name = "one cpu q=l/3 997", .lowered = 3, .bounds = fine, .one_cpu = true},
  };
}

class pinned
{
public:
  explicit pinned(bool on) : on_{on}
  {
    if(not on_)
    {
      return;
    }
    REQUIRE(sched_getaffinity(0, sizeof saved_, &saved_) == 0);
    cpu_set_t one;
    CPU_ZERO(&one);
    for(int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    {
      if(CPU_ISSET(cpu, &saved_))
      {
        CPU_SET(cpu, &one);
        break;
      }
    }
    REQUIRE(sched_setaffinity(0, sizeof one, &one) == 0);
  }

  pinned(pinned const&)                    = delete;
  auto operator=(pinned const&) -> pinned& = delete;

  ~pinned()
  {
    if(on_)
    {
      sched_setaffinity(0, sizeof saved_, &saved_);
    }
  }

private:
  bool      on_;
  cpu_set_t saved_{};
};

struct world
{
  std::vector<std::unique_ptr<node>>             nodes;
  std::vector<std::shared_ptr<stm32::uart_wire>> wires;
  std::vector<std::vector<stm32::tap_frame>>     taps;
  std::vector<core::member_ref>                  members;
  std::unique_ptr<core::system>                  sys;
  std::vector<core::pause>                       pauses;
  bool                                           strict{};
  std::size_t                                    real{};

  auto add(std::string_view entry, std::int64_t start_ps = 0) -> node&
  {
    nodes.push_back(std::make_unique<node>(entry, start_ps, strict));
    return *nodes.back();
  }

  auto link(std::size_t from, std::size_t to, std::int64_t latency, bool spare = false)
      -> stm32::uart_wire&
  {
    auto&      sender   = *nodes[from];
    auto&      receiver = *nodes[to];
    auto const lane     = static_cast<std::uint32_t>(2 * wires.size() + 1);
    auto*      machine  = receiver.target;
    auto*      into     = spare ? receiver.spare_port : receiver.port;
    auto       wire     = std::make_shared<stm32::uart_wire>(
        stm32::wire_ends{.sender = from, .receiver = to},
        stm32::wire_lanes{.peer = lane, .injected = lane + 1}, core::sys_time{latency},
        stm32::line_shape{.baud = 460'800},
        stm32::wire_receiver{
            .port   = into,
            .unit   = spare ? "USART2" : "USART1",
            .log    = [machine](core::log_record record) { machine->log(std::move(record)); },
            .strict = strict},
        spare ? sender.spare : sender.own);
    (spare ? sender.spare_port : sender.port)->attach(wire);
    wires.push_back(wire);
    taps.emplace_back();
    return *wire;
  }

  void host(variant const& how, std::vector<core::sys_time> offsets = {})
  {
    real = wires.size();
    if(how.lowered != 0)
    {
      link(0, 1, latency_20 / how.lowered, true);
    }
    for(std::size_t index = 0; index < nodes.size(); ++index)
    {
      std::vector<std::unique_ptr<core::machine>> machines;
      machines.push_back(std::move(nodes[index]->f->target));
      members.push_back(
          {.on   = std::make_shared<core::host>(std::move(machines)),
           .name = std::format("m{}", index)});
    }
    std::function<void(std::size_t)> hook;
    if(how.seed)
    {
      auto engines = std::make_shared<std::vector<std::mt19937>>();
      for(std::size_t index = 0; index < nodes.size(); ++index)
      {
        engines->emplace_back(*how.seed * 31 + index);
      }
      hook = [engines](std::size_t index) {
        std::uniform_int_distribution<int> pause{0, 50};
        std::this_thread::sleep_for(std::chrono::microseconds{pause((*engines)[index])});
      };
    }
    std::vector<std::shared_ptr<core::member_link>> links{wires.begin(), wires.end()};
    auto const wait = how.one_cpu ? core::wait_policy{.spin = std::chrono::nanoseconds{0}}
                                  : core::default_wait_policy(nodes.size());
    sys = std::make_unique<core::system>(members, std::move(links), wait, how.bounds, hook);
    if(offsets.empty())
    {
      offsets.resize(nodes.size());
    }
    auto const held = sys->lock();
    sys->start(held, offsets);
  }

  auto resume(std::int64_t until) -> core::pause
  {
    core::host_future<core::pause> pending;
    {
      auto const held = sys->lock();
      pending         = sys->resume(held, core::sys_time{until});
    }
    auto stopped = pending.get();
    pauses.push_back(stopped);
    return stopped;
  }

  auto run_to(std::int64_t until) -> core::pause
  {
    for(;;)
    {
      auto const stopped = resume(until);
      if(stopped.reason != core::pause_reason::delivered)
      {
        collect();
        return stopped;
      }
      auto const held = sys->lock();
      sys->release(held, *stopped.member);
    }
  }

  void collect()
  {
    for(std::size_t index = 0; index < wires.size(); ++index)
    {
      auto taken = wires[index]->tap().take();
      taps[index].insert(taps[index].end(), taken.begin(), taken.end());
    }
  }

  void oracle(std::int64_t end)
  {
    REQUIRE(nodes.size() == 2);
    wires[0]->enter_window(0, nodes[0]->map());
    REQUIRE(
        nodes[0]->f->run_to_stop({.max_instructions = icount{1'000'000'000}}).reason
        == stop_reason::user);
    std::ignore =
        wires[0]->drain(0, core::sys_time{end}, core::sys_time{end + ms_ps}, nodes[1]->map());
    REQUIRE(nodes[1]->advance(end) == stop_reason::time);
    for(auto const& wire : wires)
    {
      wire->finish(core::sys_time{end});
    }
    collect();
  }

  [[nodiscard]] auto dump() const -> std::string
  {
    std::string out;
    auto const  put = [&out](std::uint64_t value) {
      out.append(reinterpret_cast<char const*>(&value), sizeof value);
    };
    for(auto const& one : nodes)
    {
      auto const log = one->log();
      put(log.size());
      for(auto const word : log)
      {
        put(word);
      }
      put(one->own->frames.size());
      for(auto const& frame : one->own->frames)
      {
        put(frame.data);
        put(frame.at.ps - static_cast<std::uint64_t>(one->origin));
        put(frame.brk ? 1 : 0);
      }
      put(one->warnings.size());
    }
    for(std::size_t index = 0; index < (real == 0 ? taps.size() : real); ++index)
    {
      put(taps[index].size());
      for(auto const& frame : taps[index])
      {
        put(frame.index);
        put(frame.data);
        put(static_cast<std::uint64_t>(frame.start.ps));
        put(static_cast<std::uint64_t>(frame.end.ps));
        put(static_cast<std::uint64_t>(frame.arrival.ps));
        put(frame.lost ? 1 : 0);
      }
    }
    return out;
  }

  [[nodiscard]] auto pause_dump() const -> std::string
  {
    std::string out;
    for(auto const& one : pauses)
    {
      out += std::format(
          "{} {} {}\n", static_cast<int>(one.reason), one.member.value_or(99), one.at.ps);
    }
    return out;
  }
};

void keep_dump(std::string_view name, std::string const& bytes)
{
  if(auto const* dir = std::getenv("POPCORN_SYSTEM_DUMP"); dir != nullptr)
  {
    std::string file{name};
    std::ranges::replace(file, '/', '_');
    std::ofstream{std::string{dir} + "/" + file + ".bin", std::ios::binary} << bytes;
  }
}

struct entry
{
  std::uint32_t kind{};
  std::uint32_t data{};
  std::uint32_t cycles{};
};

auto entries(std::vector<std::uint32_t> const& log) -> std::vector<entry>
{
  std::vector<entry> out;
  for(std::size_t index = 0; index + 1 < log.size(); index += 2)
  {
    out.push_back(
        {.kind = log[index] >> 16, .data = log[index] & 0xffffu, .cycles = log[index + 1]});
  }
  return out;
}

auto system_dump(
    std::string_view sender,
    std::string_view receiver,
    std::int64_t     latency,
    variant const&   how,
    std::int64_t     end = script_end) -> std::pair<std::string, std::string>
{
  pinned const pin{how.one_cpu};
  world        w;
  w.add(sender);
  w.add(receiver);
  w.link(0, 1, latency);
  w.host(how);
  auto const stopped = w.run_to(end);
  CHECK(stopped.reason == core::pause_reason::system_time);
  {
    auto const held = w.sys->lock();
    CHECK(w.sys->time_of(held, 0).ps == end);
    CHECK(w.sys->time_of(held, 1).ps == end);
  }
  return {w.dump(), w.pause_dump()};
}
}

TEST_CASE("frames cross a system at their stop-bit end plus L in every variant, as the oracle")
{
  world reference;
  reference.add("link_tx");
  reference.add("link_rx");
  reference.link(0, 1, latency_20);
  reference.oracle(script_end + 7 * us_ps);
  auto const expected = reference.dump();
  keep_dump("oracle", expected);
  REQUIRE(reference.taps[0].size() == 64);
  std::optional<std::string> pauses;
  for(auto const& how : variants())
  {
    CAPTURE(how.name);
    auto const [seen, order] =
        system_dump("link_tx", "link_rx", latency_20, how, script_end + 7 * us_ps);
    keep_dump(how.name, seen);
    CHECK(seen == expected);
    if(pauses)
    {
      CHECK(order == *pauses);
    }
    pauses = order;
  }
}

TEST_CASE("an echo over two wires gives the same result in every variant")
{
  std::optional<std::pair<std::string, std::string>> reference;
  for(auto const& how : variants())
  {
    CAPTURE(how.name);
    pinned const pin{how.one_cpu};
    world        w;
    w.add("link_ping");
    w.add("link_echo");
    w.link(0, 1, latency_20);
    w.link(1, 0, latency_20);
    w.host(how);
    CHECK(w.run_to(script_end).reason == core::pause_reason::system_time);
    REQUIRE(w.taps[0].size() == 16);
    REQUIRE(w.taps[1].size() == 16);
    for(std::size_t index = 0; index < 16; ++index)
    {
      CHECK(w.taps[1][index].start.ps >= w.taps[0][index].arrival.ps);
      CHECK(w.taps[1][index].start.ps <= w.taps[0][index].arrival.ps + 2 * us_ps);
    }
    std::pair seen{w.dump(), w.pause_dump()};
    if(reference)
    {
      CHECK(seen == *reference);
    }
    reference = seen;
  }
}

TEST_CASE("two links of 20 and 50 us each deliver at their own latency, Q the smaller")
{
  std::optional<std::string> reference;
  for(auto const& how : {variants()[0], variants()[3], variants()[5]})
  {
    CAPTURE(how.name);
    world w;
    w.add("link_ping");
    w.add("link_echo");
    w.link(0, 1, latency_20);
    w.link(1, 0, 50 * us_ps);
    w.host(how);
    CHECK(w.sys->quantum().ps == (how.lowered != 0 ? latency_20 / how.lowered : latency_20));
    CHECK(w.run_to(script_end).reason == core::pause_reason::system_time);
    REQUIRE(w.taps[0].size() == 16);
    REQUIRE(w.taps[1].size() == 16);
    for(std::size_t index = 0; index < 16; ++index)
    {
      CHECK(w.taps[0][index].arrival.ps == w.taps[0][index].end.ps + latency_20);
      CHECK(w.taps[1][index].arrival.ps == w.taps[1][index].end.ps + 50 * us_ps);
    }
    auto const seen = w.dump();
    if(reference)
    {
      CHECK(seen == *reference);
    }
    reference = seen;
  }
}

TEST_CASE("a 1 us link, shorter than a block, still gives the oracle's result")
{
  world reference;
  reference.add("link_tx");
  reference.add("link_rx");
  reference.link(0, 1, us_ps);
  reference.oracle(script_end);
  auto const expected = reference.dump();
  for(auto const& how : {variants()[0], variants()[2], variants()[4]})
  {
    CAPTURE(how.name);
    CHECK(system_dump("link_tx", "link_rx", us_ps, how).first == expected);
  }
}

TEST_CASE("a wire 20 us longer shifts every received byte by 20 us exactly")
{
  auto const near_ = system_dump("link_tx", "link_rx", latency_20, variants()[0]);
  world      far_w;
  far_w.add("link_tx");
  far_w.add("link_rx");
  far_w.link(0, 1, 2 * latency_20);
  far_w.host(variants()[1]);
  REQUIRE(far_w.run_to(script_end).reason == core::pause_reason::system_time);
  world near_w;
  near_w.add("link_tx");
  near_w.add("link_rx");
  near_w.link(0, 1, latency_20);
  near_w.host(variants()[0]);
  REQUIRE(near_w.run_to(script_end).reason == core::pause_reason::system_time);
  CHECK(near_w.dump() == near_.first);
  auto const a = entries(near_w.nodes[1]->log());
  auto const b = entries(far_w.nodes[1]->log());
  REQUIRE(a.size() == b.size());
  REQUIRE(a.size() > 64);
  for(std::size_t index = 0; index < a.size(); ++index)
  {
    CAPTURE(index);
    CHECK(b[index].kind == a[index].kind);
    CHECK(b[index].data == a[index].data);
    CHECK(b[index].cycles - a[index].cycles == latency_20 / cycle_ps);
  }
}

TEST_CASE("members booted to 627 ms and 300 ms convert through their bases, offsets shift")
{
  auto const plain = system_dump("link_tx", "link_rx", latency_20, variants()[0]).first;
  for(auto const& how : {variants()[0], variants()[3]})
  {
    CAPTURE(how.name);
    world w;
    w.add("link_tx", 627 * ms_ps);
    w.add("link_rx", 300 * ms_ps);
    w.link(0, 1, latency_20);
    w.host(how);
    REQUIRE(w.run_to(script_end).reason == core::pause_reason::system_time);
    CHECK(w.dump() == plain);
  }
  world plain_w;
  plain_w.add("link_tx");
  plain_w.add("link_rx");
  plain_w.link(0, 1, latency_20);
  plain_w.host(variants()[0]);
  REQUIRE(plain_w.run_to(script_end).reason == core::pause_reason::system_time);
  world shifted;
  shifted.add("link_tx");
  shifted.add("link_rx");
  shifted.link(0, 1, latency_20);
  shifted.host(variants()[1], {core::sys_time{0}, core::sys_time{37 * us_ps}});
  REQUIRE(shifted.run_to(script_end + 37 * us_ps).reason == core::pause_reason::system_time);
  auto const a = entries(plain_w.nodes[1]->log());
  auto const b = entries(shifted.nodes[1]->log());
  REQUIRE(a.size() == b.size());
  for(std::size_t index = 0; index < a.size(); ++index)
  {
    CAPTURE(index);
    CHECK(b[index].data == a[index].data);
    CHECK(a[index].cycles - b[index].cycles == 37 * us_ps / cycle_ps);
  }
}

TEST_CASE("a sender reset mid-frame loses that frame and its base moves by its local time")
{
  std::optional<std::string> reference;
  for(auto const& how : variants())
  {
    CAPTURE(how.name);
    pinned const pin{how.one_cpu};
    world        w;
    w.add("link_tx_reset");
    w.add("link_rx");
    w.link(0, 1, latency_20);
    w.host(how);
    REQUIRE(w.run_to(script_end).reason == core::pause_reason::system_time);
    auto const& tap = w.taps[0];
    REQUIRE(tap.size() >= 6);
    for(std::size_t index = 0; index < 5; ++index)
    {
      CHECK_FALSE(tap[index].lost);
      CHECK(tap[index].arrival.ps == tap[index].end.ps + latency_20);
    }
    CHECK(tap[5].lost);
    auto const bytes = entries(w.nodes[1]->log());
    CHECK(std::ranges::none_of(bytes, [](entry const& one) {
      return one.kind == link_byte and one.data == 0x25;
    }));
    {
      auto const held = w.sys->lock();
      CHECK(w.sys->time_of(held, 0).ps >= script_end);
    }
    auto const seen = w.dump() + w.pause_dump();
    if(reference)
    {
      CHECK(seen == *reference);
    }
    reference = seen;
  }
}

TEST_CASE("a peer frame at another baud is a strict system error at its start at any quantum")
{
  std::optional<core::sys_time> reference;
  for(auto const& how : {variants()[0], variants()[1], variants()[3], variants()[5]})
  {
    CAPTURE(how.name);
    world w;
    w.strict = true;
    w.add("link_tx_switch");
    w.add("link_rx");
    w.link(0, 1, latency_20);
    w.host(how);
    auto const stopped = w.run_to(script_end);
    REQUIRE(stopped.reason == core::pause_reason::error);
    CHECK(stopped.member == 1);
    CHECK_THROWS_AS(std::rethrow_exception(stopped.error), popcorn::model_error);
    if(reference)
    {
      CHECK(stopped.at == *reference);
    }
    reference = stopped.at;
  }
  REQUIRE(reference);
  for(auto const& how : {variants()[0], variants()[1]})
  {
    CAPTURE(how.name);
    world w;
    w.strict = true;
    w.add("link_tx_switch");
    w.add("link_rx");
    w.link(0, 1, latency_20);
    w.host(how);
    auto const stopped = w.run_to(reference->ps + 2 * us_ps);
    REQUIRE(stopped.reason == core::pause_reason::error);
    CHECK(stopped.at == *reference);
  }
}

TEST_CASE("a mismatched frame warns once through the receiver machine's own log")
{
  for(auto const& how : {variants()[0], variants()[1]})
  {
    CAPTURE(how.name);
    world w;
    w.add("link_tx_switch");
    w.add("link_rx");
    w.link(0, 1, latency_20);
    w.host(how);
    REQUIRE(w.run_to(script_end).reason == core::pause_reason::system_time);
    CHECK(w.nodes[1]->warnings.size() == 1);
    CHECK(w.nodes[0]->warnings.empty());
  }
}

TEST_CASE("a pause's drain leaves an injection at the pending barrier accepted")
{
  for(auto const& how : {variants()[0], variants()[1]})
  {
    CAPTURE(how.name);
    world w;
    w.add("link_tx");
    w.add("link_rx");
    w.link(0, 1, latency_20);
    w.host(how);
    REQUIRE(w.resume(1 * ms_ps + 3 * us_ps).reason == core::pause_reason::system_time);
    auto const                       held    = w.sys->lock();
    auto const                       barrier = w.sys->pending_barrier(held);
    std::vector<std::uint16_t> const data{0x7e};
    CHECK(w.wires[0]->inject(data, barrier, std::nullopt).has_value());
  }
}

TEST_CASE("halted members follow system time until nothing is due, then pause as IDLE")
{
  std::optional<std::pair<std::string, core::pause>> reference;
  for(auto const& how : {variants()[0], variants()[1], variants()[2]})
  {
    CAPTURE(how.name);
    world w;
    w.add("link_tx");
    w.add("link_rx");
    w.link(0, 1, latency_20);
    w.host(how);
    REQUIRE(w.resume(500 * us_ps).reason == core::pause_reason::system_time);
    auto const frames = w.nodes[0]->own->frames.size();
    REQUIRE(frames > 0);
    core::host_future<core::pause> pending;
    {
      auto const held = w.sys->lock();
      pending         = w.sys->resume(held);
    }
    auto const stopped = pending.get();
    CHECK(stopped.reason == core::pause_reason::idle);
    auto const held = w.sys->lock();
    CHECK(w.sys->time_of(held, 0) == stopped.at);
    CHECK(w.sys->time_of(held, 1) == stopped.at);
    CHECK(w.nodes[0]->own->frames.size() > frames);
    CHECK(w.nodes[0]->own->frames.size() <= frames + 2);
    if(reference)
    {
      CHECK(w.dump() == reference->first);
      CHECK(stopped.at == reference->second.at);
      CHECK(stopped.member == reference->second.member);
    }
    else
    {
      reference = std::pair{w.dump(), stopped};
    }
  }
}

namespace
{
auto halved_at() -> std::int64_t
{
  node        solo{"link_tx_mid_switch", 0, false};
  auto const& clock = solo.target->virtual_clock();
  auto const  step  = [&solo] {
    std::ignore =
        solo.f->run({.max_instructions = icount{1000}, .wall_budget = std::chrono::seconds{60}});
  };
  for(int slices = 0; clock.core_hz() != 160'000'000; ++slices)
  {
    REQUIRE(slices < 100'000);
    step();
  }
  return static_cast<std::int64_t>(clock.epoch_time().ps) - solo.origin - 41 * 12'500;
}
}

TEST_CASE("an HCLK change charged ahead in a block held at a barrier gives one result at any Q")
{
  auto const halved = halved_at();
  for(auto const before : {std::int64_t{6'250}, std::int64_t{100'000}, std::int64_t{256'250}})
  {
    CAPTURE(before);
    {
      node       probe{"link_tx_mid_switch", 0, false};
      auto const until = vtime{static_cast<std::uint64_t>(probe.origin + halved - before)};
      REQUIRE(probe.f
                  ->run(
                      {.max_instructions = icount{1'000'000},
                       .wall_budget      = std::chrono::seconds{60},
                       .until_time       = until,
                       .hold_from_until  = true})
                  .stopped);
      REQUIRE(probe.target->virtual_clock().epoch_time() > until);
    }
    std::optional<std::string> reference;
    for(auto const& how : {variants()[0], variants()[1], variants()[2]})
    {
      CAPTURE(how.name);
      world w;
      w.add("link_tx_mid_switch");
      w.add("link_rx");
      w.link(0, 1, latency_20);
      w.host(how, {core::sys_time{latency_20 - halved + before}, core::sys_time{0}});
      REQUIRE(w.run_to(script_end).reason == core::pause_reason::system_time);
      auto const seen = w.dump();
      CHECK(entries(w.nodes[1]->log()).size() > 32);
      if(reference)
      {
        CHECK(seen == *reference);
      }
      reference = seen;
    }
  }
}

TEST_CASE("frames a halted sender's FIFO still shifts arrive at their end plus L, as the oracle")
{
  for(auto const latency : {latency_20, 100 * us_ps})
  {
    CAPTURE(latency);
    world reference;
    reference.add("link_tx_fifo_stop");
    reference.add("link_rx");
    reference.link(0, 1, latency);
    reference.wires[0]->enter_window(0, reference.nodes[0]->map());
    REQUIRE(
        reference.nodes[0]->f->run_to_stop({.max_instructions = icount{1'000'000'000}}).reason
        == stop_reason::user);
    reference.nodes[0]->target->hold_core(true);
    REQUIRE(reference.nodes[0]->advance(script_end) == stop_reason::time);
    std::ignore = reference.wires[0]->drain(
        0, core::sys_time{script_end}, core::sys_time{script_end + ms_ps},
        reference.nodes[1]->map());
    REQUIRE(reference.nodes[1]->advance(script_end) == stop_reason::time);
    reference.wires[0]->finish(core::sys_time{script_end});
    reference.collect();
    REQUIRE(reference.taps[0].size() == 9);
    auto const received = entries(reference.nodes[1]->log());
    REQUIRE(std::ranges::count(received, link_byte, &entry::kind) == 9);
    auto const expected = reference.dump();
    for(auto const& how : variants())
    {
      CAPTURE(how.name);
      CHECK(system_dump("link_tx_fifo_stop", "link_rx", latency, how).first == expected);
    }
  }
}

TEST_CASE(
    "a stamped write on a halted sender transmits at its time, the frame arrives at end plus L")
{
  constexpr std::int64_t     stamp_at = 700 * us_ps;
  std::optional<std::string> reference;
  for(auto const& how : {variants()[0], variants()[1], variants()[2], variants()[4]})
  {
    CAPTURE(how.name);
    world w;
    w.add("link_rx");
    w.add("link_rx");
    w.link(0, 1, latency_20);
    w.host(how);
    REQUIRE(w.resume(600 * us_ps).reason == core::pause_reason::system_time);
    core::host_future<core::goal_reached> step;
    {
      auto const held = w.sys->lock();
      step            = w.sys->post_goal(held, 0, {.until_time = core::sys_time{610 * us_ps}});
    }
    auto const stepped = w.resume(650 * us_ps);
    REQUIRE(stepped.reason == core::pause_reason::delivered);
    std::int64_t local = 0;
    {
      auto const held = w.sys->lock();
      w.sys->release(held, 0);
      local = stamp_at
            - w.sys->time_of(held, 0).ps
            + static_cast<std::int64_t>(core::access(
                *w.members[0].on, 0, [](core::machine& m) { return m.virtual_clock().time().ps; }));
    }
    core::host_future<vtime> written;
    {
      auto const held_host = w.members[0].on->lock();
      written              = w.members[0].on->post(
          held_host, 0,
          [](core::machine& m) {
            auto&      usart = m.peripheral("USART1");
            auto const found = std::ranges::find(
                usart.description().registers, std::string_view{"TDR"},
                &popcorn::register_desc::name);
            REQUIRE(found != usart.description().registers.end());
            REQUIRE(m.mem().write(usart.description().base + found->offset, 4, 0x5a));
          },
          vtime{static_cast<std::uint64_t>(local)});
    }
    REQUIRE(w.run_to(900 * us_ps).reason == core::pause_reason::system_time);
    CHECK(written.get().ps == static_cast<std::uint64_t>(local));
    auto const got = entries(w.nodes[1]->log());
    CHECK(
        std::ranges::count_if(
            got, [](entry const& one) { return one.kind == link_byte and one.data == 0x5a; })
        == 1);
    auto const seen = w.dump();
    if(reference)
    {
      CHECK(seen == *reference);
    }
    reference = seen;
  }
}

TEST_CASE("a halted receiver's USART overruns while its core is held, the same in every variant")
{
  std::optional<std::string> reference;
  for(auto const& how : variants())
  {
    CAPTURE(how.name);
    pinned const pin{how.one_cpu};
    world        w;
    w.add("link_tx");
    w.add("link_rx");
    w.link(0, 1, latency_20);
    w.host(how);
    core::host_future<core::goal_reached> first;
    {
      auto const held = w.sys->lock();
      first           = w.sys->post_goal(held, 1, {.until_time = core::sys_time{700 * us_ps}});
    }
    REQUIRE(w.resume(script_end).reason == core::pause_reason::delivered);
    auto const before = entries(w.nodes[1]->log()).size();
    {
      auto const held = w.sys->lock();
      w.sys->release(held, 1);
    }
    REQUIRE(w.resume(1200 * us_ps).reason == core::pause_reason::system_time);
    CHECK(entries(w.nodes[1]->log()).size() == before);
    core::host_future<core::goal_reached> again;
    {
      auto const held = w.sys->lock();
      again           = w.sys->post_goal(held, 1, {.until_time = core::sys_time{1300 * us_ps}});
    }
    REQUIRE(w.run_to(script_end).reason == core::pause_reason::system_time);
    auto const got = entries(w.nodes[1]->log());
    CHECK(std::ranges::count(got, 3u, &entry::kind) >= 1);
    auto const seen = w.dump();
    if(reference)
    {
      CHECK(seen == *reference);
    }
    reference = seen;
  }
}

TEST_CASE("an injection queued while every member sleeps is received, then the system pauses IDLE")
{
  std::optional<std::string> reference;
  for(auto const& how : {variants()[0], variants()[1], variants()[2]})
  {
    CAPTURE(how.name);
    world w;
    w.add("link_rx");
    w.add("link_rx");
    w.link(0, 1, latency_20);
    w.host(how);
    REQUIRE(w.resume(600 * us_ps).reason == core::pause_reason::system_time);
    core::host_future<core::goal_reached> listening;
    {
      auto const held = w.sys->lock();
      listening       = w.sys->post_goal(held, 1, {.until = popcorn::address{0x0800'0001}});
      auto const at   = core::sys_time{w.sys->pending_barrier(held).ps + 20 * us_ps};
      std::vector<std::uint16_t> const data{0x7e};
      REQUIRE(w.wires[0]->inject(data, at, std::nullopt).has_value());
    }
    core::host_future<core::pause> pending;
    {
      auto const held = w.sys->lock();
      pending         = w.sys->resume(held);
    }
    auto const stopped = pending.get();
    CHECK(stopped.reason == core::pause_reason::idle);
    CHECK(listening.get().stopped.reason == stop_reason::idle);
    auto const got = entries(w.nodes[1]->log());
    CHECK(
        std::ranges::count_if(
            got, [](entry const& one) { return one.kind == link_byte and one.data == 0x7e; })
        == 1);
    auto const seen = w.dump();
    if(reference)
    {
      CHECK(seen == *reference);
    }
    reference = seen;
  }
}

TEST_CASE("a system saved with its members mid-burst and loaded again runs on the same")
{
  for(auto const& [sender, at] : {
          std::pair{std::string_view{"link_tx"},       1500 * us_ps},
          std::pair{std::string_view{"link_tx_reset"}, 400 * us_ps }
  })
  {
    std::optional<std::string> runs;
    for(auto const& how : {variants()[0], variants()[1], variants()[2]})
    {
      CAPTURE(how.name);
      CAPTURE(sender);
      world w;
      w.add(sender);
      w.add("link_rx");
      w.link(0, 1, latency_20);
      w.host(how);
      REQUIRE(w.resume(at).reason == core::pause_reason::system_time);
      w.collect();
      for(auto& tap : w.taps)
      {
        tap.clear();
      }
      std::vector<core::snapshot> saved;
      std::vector<std::byte>      state;
      for(auto const& one : w.members)
      {
        saved.push_back(core::access(*one.on, 0, [](core::machine& m) { return m.snapshot(); }));
      }
      {
        auto const held = w.sys->lock();
        w.sys->save(held, state);
      }
      auto const frames   = w.nodes[0]->own->frames.size();
      auto const warnings = std::pair{w.nodes[0]->warnings.size(), w.nodes[1]->warnings.size()};
      REQUIRE(w.run_to(script_end).reason == core::pause_reason::system_time);
      auto const first = w.dump();
      if(runs)
      {
        CHECK(first == *runs);
      }
      runs = first;
      for(std::size_t index = 0; index < w.members.size(); ++index)
      {
        core::access(*w.members[index].on, 0, [&](core::machine& m) { m.restore(saved[index]); });
      }
      {
        auto const held = w.sys->lock();
        w.sys->load(held, state);
      }
      w.nodes[0]->own->frames.resize(frames);
      w.nodes[0]->warnings.resize(warnings.first);
      w.nodes[1]->warnings.resize(warnings.second);
      for(auto& tap : w.taps)
      {
        tap.clear();
      }
      REQUIRE(w.run_to(script_end).reason == core::pause_reason::system_time);
      CHECK(w.dump() == first);
      {
        auto const                   held = w.sys->lock();
        std::vector<std::byte> const junk{std::byte{1}, std::byte{2}};
        CHECK_THROWS_AS(w.sys->load(held, junk), popcorn::config_error);
      }
    }
  }
}

namespace
{
struct after_point
{
  std::vector<std::uint32_t>    rx;
  std::vector<std::uint32_t>    tx;
  std::vector<stm32::tap_frame> tap;
  std::vector<std::uint64_t>    own{};

  friend auto operator==(after_point const&, after_point const&) -> bool = default;
};

auto observed_after(world& w, std::size_t own_from) -> after_point
{
  after_point out{.rx = w.nodes[1]->log(), .tx = w.nodes[0]->log(), .tap = w.taps[0]};
  for(std::size_t index = own_from; index < w.nodes[0]->own->frames.size(); ++index)
  {
    auto const& frame = w.nodes[0]->own->frames[index];
    out.own.push_back(frame.at.ps - static_cast<std::uint64_t>(w.nodes[0]->origin));
    out.own.push_back(frame.data);
  }
  return out;
}

auto restored_run(variant const& saving, variant const& loading, std::int64_t at) -> after_point
{
  world first;
  first.add("link_tx");
  first.add("link_rx");
  first.link(0, 1, latency_20);
  first.host(saving);
  REQUIRE(first.resume(at).reason == core::pause_reason::system_time);
  std::vector<core::snapshot_data> saved;
  for(auto const& one : first.members)
  {
    saved.push_back(core::access(*one.on, 0, [](core::machine& m) { return m.snapshot().data(); }));
  }
  std::vector<std::byte> state;
  {
    auto const held = first.sys->lock();
    first.sys->save(held, state);
  }
  world second;
  second.add("link_tx");
  second.add("link_rx");
  second.link(0, 1, latency_20);
  second.host(loading);
  for(std::size_t index = 0; index < second.members.size(); ++index)
  {
    core::access(*second.members[index].on, 0, [&](core::machine& m) {
      m.restore(core::snapshot{saved[index]});
    });
  }
  {
    auto const held = second.sys->lock();
    second.sys->load(held, state);
  }
  REQUIRE(second.run_to(script_end).reason == core::pause_reason::system_time);
  return observed_after(second, 0);
}
}

TEST_CASE("a state saved at one quantum continues under another as the run never saved")
{
  variant const          third{.name = "q=l/3", .lowered = 3};
  variant const          whole{.name = "q=l with a spare", .lowered = 1};
  constexpr std::int64_t at = 1500 * us_ps;
  world                  plain;
  plain.add("link_tx");
  plain.add("link_rx");
  plain.link(0, 1, latency_20);
  plain.host(whole);
  REQUIRE(plain.resume(at).reason == core::pause_reason::system_time);
  plain.collect();
  plain.taps[0].clear();
  auto const own_from = plain.nodes[0]->own->frames.size();
  REQUIRE(plain.run_to(script_end).reason == core::pause_reason::system_time);
  auto const expected = observed_after(plain, own_from);
  REQUIRE_FALSE(expected.tap.empty());
  CHECK(restored_run(third, whole, at) == expected);
  CHECK(restored_run(whole, third, at) == expected);
}

namespace
{
void freeze_field(core::machine& m, std::string_view name)
{
  auto const& desc = m.peripheral("DBGMCU").description();
  for(auto const& reg : desc.registers)
  {
    auto const field = std::ranges::find(reg.fields, name, &popcorn::field_desc::name);
    if(field != reg.fields.end())
    {
      auto const at  = desc.base + reg.offset;
      auto const old = m.mem().read(at, 4);
      REQUIRE(old.has_value());
      REQUIRE(m.mem().write(at, 4, *old | (std::uint32_t{1} << field->lsb)).has_value());
      return;
    }
  }
  FAIL("no DBGMCU field " << name);
}

auto ring_of(core::machine& m) -> std::vector<std::uint8_t>
{
  std::vector<std::uint8_t> out;
  auto const                base = sym("usart_ll", "big_ring");
  for(std::uint32_t index = 0; index < 64; ++index)
  {
    auto const value = m.mem().read(base + index, 1);
    REQUIRE(value.has_value());
    out.push_back(static_cast<std::uint8_t>(*value));
  }
  return out;
}
}

TEST_CASE("a halted receiver's RX DMA ring fills, or stops with its GPDMA channel frozen")
{
  std::optional<std::vector<std::uint8_t>> running_ring;
  for(auto const frozen : {false, true})
  {
    std::optional<std::pair<std::vector<std::uint8_t>, std::string>> reference;
    for(auto const& how : {variants()[0], variants()[1], variants()[2], variants()[4]})
    {
      CAPTURE(frozen);
      CAPTURE(how.name);
      world w;
      w.add("link_tx");
      w.add("link_rx_dma");
      w.link(0, 1, latency_20);
      w.host(how);
      if(frozen)
      {
        core::access(
            *w.members[1].on, 0, [](core::machine& m) { freeze_field(m, "DBG_GPDMA3_STOP"); });
      }
      core::host_future<core::goal_reached> listening;
      {
        auto const held = w.sys->lock();
        listening       = w.sys->post_goal(held, 1, {.until_time = core::sys_time{400 * us_ps}});
      }
      REQUIRE(w.resume(script_end).reason == core::pause_reason::delivered);
      {
        auto const held = w.sys->lock();
        w.sys->release(held, 1);
      }
      REQUIRE(w.run_to(script_end).reason == core::pause_reason::system_time);
      auto const ring =
          core::access(*w.members[1].on, 0, [](core::machine& m) { return ring_of(m); });
      std::pair seen{ring, w.dump()};
      if(reference)
      {
        CHECK(seen == *reference);
      }
      reference = seen;
    }
    if(not frozen)
    {
      running_ring = reference->first;
      for(std::size_t index = 0; index < 64; ++index)
      {
        CHECK(reference->first[index] == 0x20 + index);
      }
    }
    else
    {
      CHECK(reference->first != *running_ring);
    }
  }
}

TEST_CASE("a sender bitten by its watchdog mid-burst loses the frame in flight, its base moves")
{
  std::optional<std::string> reference;
  for(auto const& how : variants())
  {
    CAPTURE(how.name);
    pinned const pin{how.one_cpu};
    world        w;
    w.add("link_iwdg");
    w.add("link_rx");
    w.link(0, 1, latency_20);
    w.host(how);
    REQUIRE(w.run_to(8 * ms_ps).reason == core::pause_reason::system_time);
    auto const lost = std::ranges::find_if(w.taps[0], &stm32::tap_frame::lost);
    REQUIRE(lost != w.taps[0].end());
    CHECK(std::ranges::count_if(w.taps[0], &stm32::tap_frame::lost) == 1);
    {
      auto const held = w.sys->lock();
      CHECK(w.sys->time_of(held, 0).ps >= 8 * ms_ps);
    }
    auto const seen = w.dump();
    if(reference)
    {
      CHECK(seen == *reference);
    }
    reference = seen;
  }
}

TEST_CASE("a halted sender's watchdog bites unless DBG_IWDG_STOP freezes it")
{
  for(auto const frozen : {false, true})
  {
    std::optional<std::string> reference;
    for(auto const& how : {variants()[0], variants()[1], variants()[2]})
    {
      CAPTURE(frozen);
      CAPTURE(how.name);
      world w;
      w.add("link_iwdg");
      w.add("link_rx");
      w.link(0, 1, latency_20);
      w.host(how);
      if(frozen)
      {
        core::access(
            *w.members[0].on, 0, [](core::machine& m) { freeze_field(m, "DBG_IWDG_STOP"); });
      }
      core::host_future<core::goal_reached> sending;
      {
        auto const held = w.sys->lock();
        sending         = w.sys->post_goal(held, 0, {.until_time = core::sys_time{ms_ps}});
      }
      REQUIRE(w.resume(script_end).reason == core::pause_reason::delivered);
      auto const resets =
          core::access(*w.members[0].on, 0, [](core::machine& m) { return m.resets(); });
      {
        auto const held = w.sys->lock();
        w.sys->release(held, 0);
      }
      REQUIRE(w.run_to(10 * ms_ps).reason == core::pause_reason::system_time);
      auto const after =
          core::access(*w.members[0].on, 0, [](core::machine& m) { return m.resets(); });
      CHECK(after == resets + (frozen ? 0 : 1));
      CHECK(
          std::ranges::count_if(
              w.nodes[0]->warnings,
              [](core::log_record const& one) { return one.message.contains("while halted"); })
          == (frozen ? 0 : 1));
      auto const seen = w.dump();
      if(reference)
      {
        CHECK(seen == *reference);
      }
      reference = seen;
    }
  }
}
