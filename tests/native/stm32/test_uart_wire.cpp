#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/member_link.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/core/window_barrier.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/uart.hpp>
#include <popcorn/periph/stm32/uart_wire.hpp>

#include <reflex/serde/bson.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <span>
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

constexpr std::uint64_t us_ps         = 1'000'000;
constexpr std::uint64_t ms_ps         = 1'000'000'000;
constexpr std::uint64_t cycle_ps      = 6'250;
constexpr std::uint64_t coarse        = 1'000'000'000;
constexpr std::uint64_t fine          = 997;
constexpr std::uint32_t link_byte     = 1;
constexpr std::uint32_t link_baud     = 460'800;
constexpr std::uint64_t latency_20    = 20 * us_ps;
constexpr std::uint64_t script_end_ps = 3 * ms_ps;

auto declared() -> stm32::line_shape
{
  return {.baud = link_baud};
}

struct fault_seen
{
  std::int64_t at{};
  std::size_t  member{};

  friend auto operator==(fault_seen const&, fault_seen const&) -> bool = default;
};

struct node
{
  std::shared_ptr<stm32::capture_endpoint> own = std::make_shared<stm32::capture_endpoint>();
  stm32::board                             board{};
  std::unique_ptr<stm32_fixture>           f;
  std::uint64_t                            origin{};
  stm32::uart_port*                        port{};
  std::vector<core::log_record>            warnings;
  std::vector<fault_seen>                  faults;
  core::hook_handle                        sink;
  bool                                     done{};
  bool                                     failed{};

  explicit node(std::string_view entry, std::uint64_t start_ps = 0)
  {
    board.uarts.emplace("USART1", own);
    auto options   = popcorn::testing::stm32_options();
    options.models = stm32::factories(board, u575());
    f              = std::make_unique<stm32_fixture>(u575(), "usart_ll", std::move(options));
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("usart_ll", "main")})
            .reason
        == stop_reason::address);
    if(start_ps != 0)
    {
      f->set("pc", sym("usart_ll", "idle"));
      REQUIRE(
          f->run_to_stop({.max_instructions = icount{1'000'000'000}, .until_time = vtime{start_ps}})
              .reason
          == stop_reason::time);
    }
    f->set("pc", sym("usart_ll", entry));
    origin = f->target->virtual_clock().time().ps;
    port   = f->model_of<stm32::uart_port>("USART1");
    REQUIRE(port != nullptr);
    sink = f->target->add_log_sink([this](core::log_record const& record) {
      if(record.kind == core::log_kind::model_warning and record.peripheral == "USART1")
      {
        warnings.push_back(record);
      }
    });
  }

  [[nodiscard]] auto map() const -> core::time_map
  {
    return core::time_map{core::sys_time{-static_cast<std::int64_t>(origin)}};
  }

  [[nodiscard]] auto now() const -> std::uint64_t
  {
    return f->target->virtual_clock().time().ps - origin;
  }

  auto advance(std::uint64_t system_ps, std::uint64_t slice, bool hold) -> stop_reason
  {
    for(;;)
    {
      auto const result = f->run(
          {.max_instructions = icount{slice},
           .wall_budget      = std::chrono::seconds{60},
           .until_time       = vtime{origin + system_ps},
           .hold_from_until  = hold});
      if(result.stopped)
      {
        return result.stopped->reason;
      }
    }
  }
};

struct host_byte
{
  std::size_t   member{};
  std::uint64_t at{};
  std::uint16_t data{};
};

struct run_plan
{
  std::uint64_t                      window{latency_20};
  std::uint64_t                      first{};
  std::uint64_t                      end{script_end_ps};
  std::uint64_t                      slice{coarse};
  bool                               threaded{};
  std::optional<std::uint32_t>       seed{};
  std::optional<host_byte>           host{};
  std::function<void(std::uint64_t)> after_drain{};
};

struct rig
{
  std::vector<std::unique_ptr<node>>             nodes;
  std::vector<std::shared_ptr<stm32::uart_wire>> wires;
  std::vector<std::vector<stm32::tap_frame>>     taps;
  bool                                           strict{};

  auto add(std::string_view entry, std::uint64_t start_ps = 0) -> node&
  {
    nodes.push_back(std::make_unique<node>(entry, start_ps));
    return *nodes.back();
  }

  auto link(std::size_t from, std::size_t to, std::uint64_t latency) -> stm32::uart_wire&
  {
    auto&      sender   = *nodes[from];
    auto&      receiver = *nodes[to];
    auto const lane     = static_cast<std::uint32_t>(2 * wires.size() + 1);
    auto*      machine  = receiver.f->target.get();
    auto       wire     = std::make_shared<stm32::uart_wire>(
        stm32::wire_ends{.sender = from, .receiver = to},
        stm32::wire_lanes{.peer = lane, .injected = lane + 1},
        core::sys_time{static_cast<std::int64_t>(latency)}, declared(),
        stm32::wire_receiver{
            .port   = receiver.port,
            .unit   = "USART1",
            .log    = [machine](core::log_record record) { machine->log(std::move(record)); },
            .strict = strict},
        sender.own);
    sender.port->attach(wire);
    wires.push_back(wire);
    taps.emplace_back();
    return *wire;
  }

  void enter(std::size_t member, std::uint64_t window)
  {
    for(auto const& wire : wires)
    {
      if(wire->sender() == member)
      {
        wire->enter_window(window, nodes[member]->map());
      }
    }
  }

  void drain(std::size_t member, std::uint64_t window, std::uint64_t barrier)
  {
    for(auto const& wire : wires)
    {
      if(wire->receiver() == member)
      {
        auto const found = wire->drain(
            window, core::sys_time{static_cast<std::int64_t>(barrier)}, nodes[member]->map());
        if(found)
        {
          nodes[member]->faults.push_back({.at = found->at.ps, .member = found->member});
        }
      }
    }
  }

  void run_window(
      std::size_t                     member,
      std::uint64_t                   window,
      std::uint64_t                   barrier,
      run_plan const&                 plan,
      std::optional<host_byte> const& host)
  {
    auto& one = *nodes[member];
    enter(member, window);
    if(one.done or one.failed)
    {
      return;
    }
    if(host
       and host->member == member
       and host->at <= barrier
       and host->at > barrier - std::min(barrier, plan.window))
    {
      if(one.advance(host->at, plan.slice, false) != stop_reason::time)
      {
        one.failed = true;
        return;
      }
      std::vector<std::uint16_t> const data{host->data};
      one.port->receive(data);
    }
    auto const reason = one.advance(barrier, plan.slice, true);
    one.done          = reason == stop_reason::user;
    one.failed        = reason != stop_reason::user and reason != stop_reason::time;
  }

  void finish(std::uint64_t at)
  {
    for(std::size_t index = 0; index < wires.size(); ++index)
    {
      wires[index]->finish(core::sys_time{static_cast<std::int64_t>(at)});
      auto taken = wires[index]->tap().take();
      taps[index].insert(taps[index].end(), taken.begin(), taken.end());
    }
  }

  void run(run_plan const& plan)
  {
    auto const windows = (plan.end + plan.window - 1) / plan.window;
    auto const barrier = [&](std::uint64_t window) {
      return std::min((window + 1) * plan.window, plan.end);
    };
    if(not plan.threaded)
    {
      for(std::uint64_t window = plan.first; window < windows; ++window)
      {
        for(std::size_t member = 0; member < nodes.size(); ++member)
        {
          run_window(member, window, barrier(window), plan, plan.host);
        }
        if(window + 1 == windows)
        {
          break;
        }
        for(std::size_t member = 0; member < nodes.size(); ++member)
        {
          drain(member, window, barrier(window));
        }
        if(plan.after_drain)
        {
          plan.after_drain(window);
        }
      }
    }
    else
    {
      core::window_barrier       meet{nodes.size(), core::default_wait_policy(nodes.size())};
      std::vector<std::jthread>  threads;
      std::vector<std::uint64_t> slept(nodes.size());
      for(std::size_t member = 0; member < nodes.size(); ++member)
      {
        threads.emplace_back([&, member] {
          std::mt19937                                 rng{plan.seed.value_or(0) * 31 + member};
          std::uniform_int_distribution<std::uint64_t> pause{0, 50};
          for(std::uint64_t window = plan.first; window < windows; ++window)
          {
            run_window(member, window, barrier(window), plan, plan.host);
            if(plan.seed)
            {
              auto const us = pause(rng);
              slept[member] += us;
              std::this_thread::sleep_for(std::chrono::microseconds{us});
            }
            meet.arrive_and_wait([]() noexcept {});
            if(window + 1 < windows)
            {
              drain(member, window, barrier(window));
            }
          }
        });
      }
      threads.clear();
    }
    finish(plan.end);
    for(auto const& one : nodes)
    {
      REQUIRE_FALSE(one->failed);
    }
  }

  void oracle(std::uint64_t end, std::optional<host_byte> const& host = {})
  {
    REQUIRE(nodes.size() == 2);
    enter(0, 0);
    REQUIRE(
        nodes[0]->f->run_to_stop({.max_instructions = icount{1'000'000'000}}).reason
        == stop_reason::user);
    nodes[0]->done = true;
    drain(1, 0, end);
    if(host)
    {
      REQUIRE(nodes[1]->advance(host->at, coarse, false) == stop_reason::time);
      std::vector<std::uint16_t> const data{host->data};
      nodes[1]->port->receive(data);
    }
    REQUIRE(nodes[1]->advance(end, coarse, false) == stop_reason::time);
    finish(end);
  }

  [[nodiscard]] auto dump() const -> std::string
  {
    std::string out;
    auto const  put = [&out](std::uint64_t value) {
      out.append(reinterpret_cast<char const*>(&value), sizeof value);
    };
    for(auto const& one : nodes)
    {
      auto const log = one->f->log();
      put(log.size());
      for(auto const word : log)
      {
        put(word);
      }
      put(one->own->frames.size());
      for(auto const& frame : one->own->frames)
      {
        put(frame.data);
        put(frame.at.ps - one->origin);
        put(frame.brk ? 1 : 0);
      }
      put(one->warnings.size());
      put(one->faults.size());
      for(auto const& fault : one->faults)
      {
        put(static_cast<std::uint64_t>(fault.at));
        put(fault.member);
      }
    }
    for(auto const& tap : taps)
    {
      put(tap.size());
      for(auto const& frame : tap)
      {
        put(frame.from);
        put(frame.to);
        put(frame.index);
        put(frame.data);
        put(static_cast<std::uint64_t>(frame.start.ps));
        put(static_cast<std::uint64_t>(frame.end.ps));
        put(static_cast<std::uint64_t>(frame.arrival.ps));
        put(frame.brk ? 1 : 0);
        put(frame.lost ? 1 : 0);
      }
    }
    return out;
  }
};

void keep_dump(std::string_view name, std::string const& bytes)
{
  if(auto const* dir = std::getenv("POPCORN_WIRE_DUMP"); dir != nullptr)
  {
    std::ofstream{std::string{dir} + "/" + std::string{name} + ".bin", std::ios::binary} << bytes;
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

auto bytes_of(std::vector<std::uint32_t> const& log) -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(auto const& one : entries(log))
  {
    if(one.kind == link_byte)
    {
      out.push_back(one.data);
    }
  }
  return out;
}

auto one_way(std::string_view sender, std::string_view receiver, std::uint64_t latency) -> rig
{
  rig r;
  r.add(sender);
  r.add(receiver);
  r.link(0, 1, latency);
  return r;
}

auto oracle_dump(std::string_view sender, std::string_view receiver, std::uint64_t latency)
    -> std::string
{
  auto r = one_way(sender, receiver, latency);
  r.oracle(script_end_ps);
  return r.dump();
}

auto run_dump(
    std::string_view sender,
    std::string_view receiver,
    std::uint64_t    latency,
    run_plan const&  plan) -> std::string
{
  auto r = one_way(sender, receiver, latency);
  r.run(plan);
  return r.dump();
}
}

TEST_CASE("frames cross a wire at their stop-bit end plus L whatever the window, slicing or thread")
{
  auto reference = one_way("link_tx", "link_rx", latency_20);
  reference.oracle(script_end_ps);
  auto const expected = reference.dump();
  keep_dump("oracle", expected);
  auto const& b = *reference.nodes[1];
  REQUIRE(bytes_of(b.f->log()).size() == 64);
  CHECK(
      std::ranges::count_if(entries(b.f->log()), [](entry const& one) { return one.kind == 2; })
      == 3);
  auto const& tap = reference.taps[0];
  REQUIRE(tap.size() == 64);
  REQUIRE(reference.nodes[0]->own->frames.size() == 64);
  for(std::size_t index = 0; index < tap.size(); ++index)
  {
    CAPTURE(index);
    CHECK(tap[index].index == index);
    CHECK(tap[index].data == 0x20u + index);
    CHECK(tap[index].arrival.ps == tap[index].end.ps + static_cast<std::int64_t>(latency_20));
    CHECK(
        tap[index].end.ps
        == static_cast<std::int64_t>(
            reference.nodes[0]->own->frames[index].at.ps - reference.nodes[0]->origin));
  }
  struct variant
  {
    std::string_view name;
    run_plan         plan;
  };
  std::vector<variant> const variants{
      {"q20",              {.window = latency_20}                                 },
      {"q6",               {.window = latency_20 / 3}                             },
      {"q1",               {.window = us_ps}                                      },
      {"q20-fine",         {.window = latency_20, .slice = fine}                  },
      {"q6-fine",          {.window = latency_20 / 3, .slice = fine}              },
      {"threads-q20",      {.window = latency_20, .threaded = true}               },
      {"threads-q6",       {.window = latency_20 / 3, .threaded = true}           },
      {"threads-q20-fine", {.window = latency_20, .slice = fine, .threaded = true}},
  };
  for(auto const& [name, plan] : variants)
  {
    CAPTURE(name);
    auto const seen = run_dump("link_tx", "link_rx", latency_20, plan);
    keep_dump(name, seen);
    CHECK(seen == expected);
  }
}

TEST_CASE("seeded sleeps before each barrier arrival leave both members' results unchanged")
{
  auto const expected = oracle_dump("link_tx", "link_rx", latency_20);
  for(std::uint32_t seed = 1; seed <= 20; ++seed)
  {
    CAPTURE(seed);
    CHECK(
        run_dump(
            "link_tx", "link_rx", latency_20,
            {.window = latency_20, .threaded = true, .seed = seed})
        == expected);
  }
}

TEST_CASE("a wire 20 us longer shifts every received byte and tap arrival by 20 us exactly")
{
  auto near = one_way("link_tx", "link_rx", latency_20);
  near.run({.window = latency_20});
  auto far = one_way("link_tx", "link_rx", 2 * latency_20);
  far.run({.window = 2 * latency_20 / 3});
  auto const a = entries(near.nodes[1]->f->log());
  auto const b = entries(far.nodes[1]->f->log());
  REQUIRE(a.size() == b.size());
  REQUIRE(a.size() > 64);
  for(std::size_t index = 0; index < a.size(); ++index)
  {
    CAPTURE(index);
    CHECK(b[index].kind == a[index].kind);
    CHECK(b[index].data == a[index].data);
    CHECK(b[index].cycles - a[index].cycles == latency_20 / cycle_ps);
  }
  REQUIRE(near.taps[0].size() == far.taps[0].size());
  for(std::size_t index = 0; index < near.taps[0].size(); ++index)
  {
    CHECK(
        far.taps[0][index].arrival.ps - near.taps[0][index].arrival.ps
        == static_cast<std::int64_t>(latency_20));
  }
}

TEST_CASE("members started at local 627 ms and 300 ms convert through their bases")
{
  rig plain = one_way("link_tx", "link_rx", latency_20);
  plain.run({.window = latency_20});

  rig based;
  based.add("link_tx", 627 * ms_ps);
  based.add("link_rx", 300 * ms_ps);
  based.link(0, 1, latency_20);
  auto& a = *based.nodes[0];
  auto& b = *based.nodes[1];
  CHECK(a.origin >= 627 * ms_ps);
  CHECK(b.origin >= 300 * ms_ps);
  bool checked = false;
  based.run(
      {.window = latency_20, .after_drain = [&](std::uint64_t) {
         if(checked or a.own->frames.empty())
         {
           return;
         }
         auto const pending = b.f->target->snapshot().data().events.pending;
         auto const linked  = std::ranges::find(pending, std::uint32_t{1}, &core::event::lane);
         if(linked == pending.end())
         {
           return;
         }
         checked = true;
         CHECK(linked->due_ps == a.own->frames.front().at.ps - a.origin + b.origin + latency_20);
       }});
  CHECK(checked);
  CHECK(based.nodes[1]->f->log() == plain.nodes[1]->f->log());
  CHECK(based.taps == plain.taps);
}

TEST_CASE("an echo returns each byte two frames, two L and the echo's ISR time later")
{
  auto const build = [] {
    rig r;
    r.add("link_ping");
    r.add("link_echo");
    r.link(0, 1, latency_20);
    r.link(1, 0, latency_20);
    return r;
  };
  auto reference = build();
  reference.run({.window = latency_20});
  auto const& ping = reference.taps[0];
  auto const& echo = reference.taps[1];
  REQUIRE(ping.size() == 16);
  REQUIRE(echo.size() == 16);
  auto const& a = *reference.nodes[0];
  CHECK(bytes_of(a.f->log()).size() == 16);
  std::vector<std::uint32_t> stamps;
  for(auto const& one : entries(a.f->log()))
  {
    if(one.kind == link_byte)
    {
      stamps.push_back(one.cycles);
    }
  }
  auto const cyccnt = a.f->target->mem().read(0xe000'1004, 4);
  REQUIRE(cyccnt.has_value());
  auto const now = static_cast<std::int64_t>(a.now());
  for(std::size_t index = 0; index < 16; ++index)
  {
    CAPTURE(index);
    CHECK(echo[index].data == 0x40u + index);
    auto const isr = echo[index].start.ps - ping[index].arrival.ps;
    CHECK(isr >= 0);
    CHECK(isr < static_cast<std::int64_t>(2 * us_ps));
    auto const frame = ping[index].end.ps - ping[index].start.ps;
    CHECK(
        echo[index].arrival.ps - ping[index].start.ps
        == 2 * frame + 2 * static_cast<std::int64_t>(latency_20) + isr);
    auto const stamp   = stamps[index];
    auto const arrival = static_cast<std::uint32_t>(
        *cyccnt - static_cast<std::uint64_t>(now - echo[index].arrival.ps) / cycle_ps);
    CHECK(stamp + 1 >= arrival);
    CHECK(stamp < arrival + 320);
  }
  auto const expected = reference.dump();
  for(auto const& plan : {
          run_plan{.window = latency_20 / 3},
          run_plan{.window = latency_20, .slice = fine},
          run_plan{.window = latency_20, .threaded = true},
          run_plan{.window = latency_20 / 3, .slice = fine, .threaded = true}
  })
  {
    auto again = build();
    again.run(plan);
    CHECK(again.dump() == expected);
  }
}

TEST_CASE("a sender restored mid-frame after the wire is attached never delivers that frame")
{
  node          alone{"link_tx"};
  std::uint64_t at = 0;
  while(alone.own->frames.size() < 3)
  {
    at += 10 * us_ps;
    REQUIRE(at < script_end_ps);
    REQUIRE(alone.advance(at, coarse, false) == stop_reason::time);
  }
  REQUIRE(alone.advance(at + 10 * us_ps, coarse, false) == stop_reason::time);
  REQUIRE(alone.own->frames.size() == 3);
  auto const resumed   = alone.now();
  auto const in_flight = alone.own->frames.size();
  auto const saved     = core::encode_snapshot(alone.f->target->snapshot().data());

  rig   r;
  auto& a = r.add("link_tx");
  r.add("link_rx");
  a.f->target->restore(core::snapshot{core::decode_snapshot(saved)});
  a.origin = alone.origin;
  r.link(0, 1, latency_20);
  REQUIRE(r.nodes[1]->advance(resumed, coarse, false) == stop_reason::time);
  r.run({.window = latency_20, .first = resumed / latency_20 + 1});
  auto const received = bytes_of(r.nodes[1]->f->log());
  REQUIRE_FALSE(received.empty());
  CHECK(received.front() == 0x20u + in_flight + 1);
  CHECK(received.size() == 64 - in_flight - 1);
  CHECK(a.own->frames.front().data == 0x20u + in_flight);
  CHECK(r.taps[0].front().data == 0x20u + in_flight + 1);
  CHECK(r.taps[0].front().index == 0);
}

TEST_CASE("an aborted frame is lost on the wire and in the tap, the next one arrives")
{
  auto reference = one_way("link_abort", "link_rx", latency_20);
  reference.oracle(script_end_ps);
  CHECK(bytes_of(reference.nodes[1]->f->log()) == std::vector<std::uint32_t>{'a', 'c'});
  auto const& tap = reference.taps[0];
  REQUIRE(tap.size() == 3);
  CHECK_FALSE(tap[0].lost);
  CHECK(tap[1].lost);
  CHECK(tap[1].data == 'b');
  CHECK(tap[1].end < tap[2].start);
  CHECK(tap[2].start.ps - tap[1].end.ps < static_cast<std::int64_t>(latency_20 / 4));
  CHECK(tap[1].arrival.ps == tap[1].end.ps + static_cast<std::int64_t>(latency_20));
  CHECK_FALSE(tap[2].lost);
  auto const expected = reference.dump();
  for(auto const& plan : {
          run_plan{.window = latency_20},
          run_plan{.window = latency_20 / 4},
          run_plan{.window = latency_20, .threaded = true}
  })
  {
    CHECK(run_dump("link_abort", "link_rx", latency_20, plan) == expected);
  }
}

TEST_CASE("the tap at each barrier holds exactly the frames ended by it, at any window")
{
  auto full = one_way("link_tx", "link_rx", latency_20);
  full.oracle(script_end_ps);
  auto const& all = full.taps[0];
  for(auto const window : {latency_20, latency_20 / 3})
  {
    CAPTURE(window);
    auto                          r = one_way("link_tx", "link_rx", latency_20);
    std::vector<stm32::tap_frame> seen;
    std::size_t                   in_flight_checks = 0;
    r.run({.window = window, .after_drain = [&](std::uint64_t k) {
             auto taken = r.wires[0]->tap().take();
             seen.insert(seen.end(), taken.begin(), taken.end());
             auto const barrier = static_cast<std::int64_t>((k + 1) * window);
             auto const ended   = std::ranges::count_if(
                 all, [&](stm32::tap_frame const& frame) { return frame.end.ps <= barrier; });
             CHECK(seen.size() == static_cast<std::size_t>(ended));
             in_flight_checks += std::ranges::any_of(all, [&](stm32::tap_frame const& frame) {
               return frame.start.ps < barrier and barrier < frame.end.ps;
             });
           }});
    CHECK(in_flight_checks > 10);
    seen.insert(seen.end(), r.taps[0].begin(), r.taps[0].end());
    CHECK(seen == all);
  }
}

TEST_CASE("finish at a stop between barriers taps exactly the frames ended by then")
{
  constexpr auto latency = 2 * latency_20;
  auto           full    = one_way("link_tx", "link_rx", latency);
  full.oracle(script_end_ps);
  auto const& all  = full.taps[0];
  auto const  last = std::ranges::find_if(all, [&](stm32::tap_frame const& frame) {
    auto const q = static_cast<std::int64_t>(latency);
    return frame.start.ps / q == (frame.end.ps + static_cast<std::int64_t>(us_ps)) / q;
  });
  REQUIRE(last != all.end());
  auto const count = static_cast<std::size_t>(last - all.begin()) + 1;
  auto const stop  = static_cast<std::uint64_t>(last->end.ps) + us_ps;
  CAPTURE(count);
  std::vector<std::vector<stm32::tap_frame>> runs;
  for(auto const window : {latency, latency / 3})
  {
    CAPTURE(window);
    auto r = one_way("link_tx", "link_rx", latency);
    r.run({.window = window, .end = stop});
    CHECK(std::ranges::equal(r.taps[0], std::span{all}.first(count)));
    runs.push_back(r.taps[0]);
    r.run({.window = window, .first = stop / window});
    CHECK(r.taps[0] == all);
  }
  CHECK(runs[0] == runs[1]);
}

TEST_CASE("a full tap keeps its newest frames and counts the ones it dropped")
{
  rig   r;
  auto& a    = r.add("link_tx");
  auto& b    = r.add("link_rx");
  auto  wire = std::make_shared<stm32::uart_wire>(
      stm32::wire_ends{.sender = 0, .receiver = 1}, stm32::wire_lanes{.peer = 1, .injected = 2},
      core::sys_time{static_cast<std::int64_t>(latency_20)}, declared(),
      stm32::wire_receiver{.port = b.port, .unit = "USART1", .log = {}, .strict = false}, a.own, 8);
  a.port->attach(wire);
  r.wires.push_back(wire);
  r.taps.emplace_back();
  r.run({.window = latency_20});
  REQUIRE(r.taps[0].size() == 8);
  CHECK(r.taps[0].front().index == 56);
  CHECK(r.taps[0].back().index == 63);
  CHECK(wire->tap().dropped() == 56);
  CHECK(bytes_of(b.f->log()).size() == 64);
}

TEST_CASE("a peer frame at another baud warns once per wire, a fault at its start when strict")
{
  for(auto const strict : {false, true})
  {
    CAPTURE(strict);
    auto const build = [strict] {
      rig r;
      r.strict = strict;
      r.add("link_tx_switch");
      r.add("link_rx");
      r.link(0, 1, latency_20);
      return r;
    };
    auto reference = build();
    reference.oracle(script_end_ps);
    auto const& b = *reference.nodes[1];
    REQUIRE(b.warnings.size() == 1);
    CHECK(b.warnings[0].message.contains("on a link declared 460800 baud"));
    auto const& tap = reference.taps[0];
    REQUIRE(tap.size() == 8);
    if(strict)
    {
      CHECK(
          b.faults
          == std::vector<fault_seen>{
              {.at = tap[4].start.ps, .member = 1}
      });
    }
    else
    {
      CHECK(b.faults.empty());
    }
    auto again = build();
    again.run({.window = latency_20 / 3});
    CHECK(again.dump() == reference.dump());
  }
}

TEST_CASE("a wire saved with posts in both mailboxes loads into a fresh wire with the same result")
{
  std::vector<std::uint16_t> const edit{'E'};
  auto                             reference = one_way("link_tx", "link_rx", latency_20);
  REQUIRE(reference.wires[0]->replace(60, edit));
  reference.run({.window = latency_20});
  auto const expected = reference.dump();

  auto r = one_way("link_tx", "link_rx", latency_20);
  REQUIRE(r.wires[0]->replace(60, edit));
  run_plan const plan{.window = latency_20};
  auto const     barrier = [](std::uint64_t k) { return (k + 1) * latency_20; };
  std::uint64_t  k       = 0;
  bool           saved   = false;
  auto const     windows = script_end_ps / latency_20;
  for(; k + 1 < windows; ++k)
  {
    r.run_window(0, k, barrier(k), plan, {});
    r.run_window(1, k, barrier(k), plan, {});
    if(not saved and k >= 20)
    {
      r.run_window(0, k + 1, barrier(k + 1), plan, {});
      std::vector<std::byte> bytes;
      r.wires[0]->save(bytes);
      auto const state = reflex::serde::bson::deserializer{
          std::span<std::byte const>{
              bytes}}.load<stm32::detail::wire_state>();
      CHECK_FALSE(state.mail_even.empty());
      CHECK_FALSE(state.mail_odd.empty());
      CHECK(state.edits.size() == 1);
      auto& a     = *r.nodes[0];
      auto& b     = *r.nodes[1];
      auto  fresh = std::make_shared<stm32::uart_wire>(
          stm32::wire_ends{.sender = 0, .receiver = 1}, stm32::wire_lanes{.peer = 1, .injected = 2},
          core::sys_time{static_cast<std::int64_t>(latency_20)}, declared(),
          stm32::wire_receiver{.port = b.port, .unit = "USART1", .log = {}, .strict = false},
          a.own);
      fresh->load(bytes);
      auto kept = r.wires[0]->tap().take();
      r.taps[0].insert(r.taps[0].end(), kept.begin(), kept.end());
      a.port->attach(fresh);
      r.wires[0] = fresh;
      saved      = true;
      r.drain(1, k, barrier(k));
      ++k;
      r.run_window(1, k, barrier(k), plan, {});
    }
    r.drain(1, k, barrier(k));
  }
  r.run_window(0, k, barrier(k), plan, {});
  r.run_window(1, k, barrier(k), plan, {});
  r.finish(script_end_ps);
  CHECK(saved);
  CHECK(r.dump() == expected);

  std::vector<std::byte> const junk(7, std::byte{0x5a});
  CHECK_THROWS_AS(r.wires[0]->load(junk), popcorn::config_error);
}

TEST_CASE("a wire frame landing on the count of a host frame orders the same at any window")
{
  constexpr std::uint16_t host_data = 0x99;
  constexpr std::uint64_t ps_per_s  = 1'000'000'000'000;
  node                    sender{"link_tx"};
  REQUIRE(
      sender.f->run_to_stop({.max_instructions = icount{1'000'000'000}}).reason
      == stop_reason::user);
  auto const& frames = sender.own->frames;
  REQUIRE(frames.size() == 64);
  auto const frame_ps = frames[1].at.ps - frames[0].at.ps;
  auto const end      = frames[40].at.ps - sender.origin;
  auto const before   = end - frame_ps - us_ps;

  node probe{"link_rx_2stop"};
  REQUIRE(probe.advance(before, coarse, false) == stop_reason::time);
  auto const t0   = probe.f->target->virtual_clock().time().ps - probe.origin;
  auto const brr  = probe.f->field("USART1", "BRR", "BRR");
  auto const f_rx = 22 * ps_per_s / (2 * (160'000'000 / brr));
  REQUIRE(t0 + f_rx > end);
  auto const latency = t0 + f_rx - end;
  CAPTURE(latency);
  auto const host = host_byte{.member = 1, .at = before, .data = host_data};

  auto reference = one_way("link_tx", "link_rx_2stop", latency);
  reference.oracle(script_end_ps, host);
  auto const seen = entries(reference.nodes[1]->f->log());
  CHECK(std::ranges::any_of(seen, [](entry const& one) {
    return one.kind == link_byte and one.data == host_data;
  }));
  CHECK(std::ranges::none_of(seen, [](entry const& one) {
    return one.kind == link_byte and one.data == 0x20u + 40u;
  }));
  CHECK(std::ranges::count_if(seen, [](entry const& one) { return one.kind == 3; }) == 1);
  auto const expected = reference.dump();
  for(auto const& plan : {
          run_plan{.window = latency, .host = host},
          run_plan{.window = latency / 3, .host = host},
          run_plan{.window = latency, .threaded = true, .host = host}
  })
  {
    CHECK(run_dump("link_tx", "link_rx_2stop", latency, plan) == expected);
  }
}

namespace
{
auto edited_run(run_plan const& plan, std::function<void(stm32::uart_wire&)> const& arm) -> rig
{
  auto r = one_way("link_tx", "link_rx", latency_20);
  arm(*r.wires[0]);
  r.run(plan);
  return r;
}

auto saved_state(stm32::uart_wire const& wire) -> stm32::detail::wire_state
{
  std::vector<std::byte> bytes;
  wire.save(bytes);
  return reflex::serde::bson::deserializer{std::span<std::byte const>{bytes}}
      .load<stm32::detail::wire_state>();
}
}

TEST_CASE("an edit armed before its frame starts replaces frames by number at any window")
{
  auto plain = one_way("link_tx", "link_rx", latency_20);
  plain.run({.window = latency_20});
  std::vector<std::uint16_t> const xyz{'x', 'y', 'z'};
  auto const arm       = [&](stm32::uart_wire& wire) { REQUIRE(wire.replace(5, xyz)); };
  auto       reference = edited_run({.window = latency_20}, arm);
  auto const before    = entries(plain.nodes[1]->f->log());
  auto const after     = entries(reference.nodes[1]->f->log());
  REQUIRE(after.size() == before.size());
  std::vector<std::uint32_t> changed;
  for(std::size_t index = 0; index < after.size(); ++index)
  {
    CHECK(after[index].kind == before[index].kind);
    CHECK(after[index].cycles == before[index].cycles);
    if(after[index].data != before[index].data)
    {
      changed.push_back(after[index].data);
    }
  }
  CHECK(changed == std::vector<std::uint32_t>{'x', 'y', 'z'});
  for(std::size_t index = 5; index < 8; ++index)
  {
    CHECK(reference.taps[0][index].data == xyz[index - 5]);
  }
  CHECK(saved_state(*reference.wires[0]).edits.empty());
  auto const expected = reference.dump();
  for(auto const& plan : {
          run_plan{.window = latency_20 / 3},
          run_plan{.window = latency_20 / 3, .threaded = true}
  })
  {
    CHECK(edited_run(plan, arm).dump() == expected);
  }
}

TEST_CASE("an edit is refused once its first frame started or over an armed edit")
{
  auto                             r    = one_way("link_tx", "link_rx", latency_20);
  auto&                            wire = *r.wires[0];
  std::vector<std::uint16_t> const one{'q'};
  std::vector<std::uint16_t> const two{'e', 'f'};
  REQUIRE(wire.replace(40, two));
  CHECK_FALSE(wire.drop(41, 1));
  CHECK_FALSE(wire.replace(39, two));
  CHECK(wire.replace(42, one));
  CHECK_FALSE(wire.drop(std::numeric_limits<std::uint64_t>::max(), 2));
  bool refused = false;
  r.run({.window = latency_20, .after_drain = [&](std::uint64_t) {
           if(not refused and wire.posted() > 3)
           {
             CHECK_FALSE(wire.replace(3, one));
             CHECK_FALSE(wire.drop(wire.posted() - 1, 1));
             refused = true;
           }
         }});
  CHECK(refused);
  CHECK(wire.posted() == 64);
  auto const bytes = bytes_of(r.nodes[1]->f->log());
  REQUIRE(bytes.size() == 64);
  CHECK(bytes[3] == 0x23u);
  CHECK(bytes[40] == 'e');
  CHECK(bytes[41] == 'f');
  CHECK(bytes[42] == 'q');
}

TEST_CASE("dropped frames never reach the receiver, IDLE rises in their gap, at any window")
{
  auto const arm       = [](stm32::uart_wire& wire) { REQUIRE(wire.drop(5, 2)); };
  auto       reference = edited_run({.window = latency_20}, arm);
  auto const seen      = entries(reference.nodes[1]->f->log());
  auto const bytes     = bytes_of(reference.nodes[1]->f->log());
  REQUIRE(bytes.size() == 62);
  CHECK(bytes[4] == 0x24u);
  CHECK(bytes[5] == 0x27u);
  REQUIRE(seen.size() > 6);
  CHECK(seen[4].data == 0x24u);
  CHECK(seen[5].kind == 2);
  CHECK(seen[6].data == 0x27u);
  auto const& tap = reference.taps[0];
  REQUIRE(tap.size() == 64);
  for(std::size_t index = 0; index < tap.size(); ++index)
  {
    CHECK(tap[index].dropped == (index == 5 or index == 6));
  }
  auto const expected = reference.dump();
  for(auto const& plan : {
          run_plan{.window = latency_20 / 3},
          run_plan{.window = latency_20, .threaded = true}
  })
  {
    CHECK(edited_run(plan, arm).dump() == expected);
  }
}

TEST_CASE("an arm racing the sender's start either applies or is refused, never both")
{
#if defined(__SANITIZE_THREAD__)
  constexpr std::uint32_t frames = 1'000;
#else
  constexpr std::uint32_t frames = 10'000;
#endif
  constexpr std::uint64_t frame_ps = 21'700 * 1'000;
  rig                     r;
  auto&                   a = r.add("link_tx_count");
  r.add("link_rx_quiet");
  REQUIRE(a.f->target->mem().write(sym("usart_ll", "link_count"), 4, frames).has_value());
  auto&             wire = r.link(0, 1, latency_20);
  std::vector<char> armed(frames);
  std::atomic<bool> stop{false};
  std::jthread      arming{[&] {
    std::mt19937                                 rng{11};
    std::uniform_int_distribution<std::uint32_t> late{0, 1};
    for(std::uint32_t index = 0; index < frames and not stop.load(); ++index)
    {
      auto const target = index + late(rng);
      while(wire.posted() < target and not stop.load())
      {
        std::this_thread::yield();
      }
      std::vector<std::uint16_t> const data{static_cast<std::uint16_t>((index + 0x80u) & 0xffu)};
      armed[index] = wire.replace(index, data) ? 1 : 0;
    }
  }};
  r.run({.window = latency_20, .end = frames * frame_ps + ms_ps, .threaded = true});
  stop.store(true);
  arming          = {};
  auto const& tap = r.taps[0];
  REQUIRE(tap.size() == frames);
  std::size_t accepted = 0;
  for(std::uint32_t index = 0; index < frames; ++index)
  {
    CAPTURE(index);
    auto const original = index & 0xffu;
    CHECK(tap[index].data == (armed[index] != 0 ? (original + 0x80u) & 0xffu : original));
    accepted += armed[index] != 0 ? 1 : 0;
  }
  MESSAGE("armed ", accepted, " of ", frames);
  CHECK(accepted > frames / 4);
  CHECK(accepted < 3 * frames / 4);
  auto const idle = entries(r.nodes[1]->f->log());
  REQUIRE(idle.size() == 1);
  CHECK(idle[0].data == frames);
}
