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
#include <popcorn/periph/uart.hpp>

#include <reflex/serde/bson.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace periph = popcorn::periph;
namespace stm32  = popcorn::periph::stm32;
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
  std::shared_ptr<periph::capture_endpoint> own = std::make_shared<periph::capture_endpoint>();
  stm32::board                              board{};
  std::unique_ptr<stm32_fixture>            f;
  std::uint64_t                             origin{};
  std::int64_t                              base{};
  stm32::uart_link_port*                    port{};
  std::vector<core::log_record>             warnings;
  std::vector<fault_seen>                   faults;
  core::hook_handle                         sink;
  bool                                      done{};
  std::int64_t                              reset_at{-1};
  bool                                      failed{};

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
    base   = -static_cast<std::int64_t>(origin);
    port   = f->model_of<stm32::uart_link_port>("USART1");
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
    return core::time_map{core::sys_time{base}};
  }

  [[nodiscard]] auto now() const -> std::uint64_t
  {
    return static_cast<std::uint64_t>(
        static_cast<std::int64_t>(f->target->virtual_clock().time().ps) + base);
  }

  auto advance(std::uint64_t system_ps, std::uint64_t slice, bool hold) -> stop_reason
  {
    for(;;)
    {
      auto const result = f->run(
          {.max_instructions = icount{slice},
           .wall_budget      = std::chrono::seconds{60},
           .until_time =
               vtime{static_cast<std::uint64_t>(static_cast<std::int64_t>(system_ps) - base)},
           .hold_from_until = hold});
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

struct member_reset
{
  std::size_t      member{};
  std::uint64_t    at{};
  std::string_view entry{};
};

struct member_call
{
  std::size_t                                       member{};
  std::uint64_t                                     at{};
  std::copyable_function<void(std::uint64_t) const> call{};
};

struct run_plan
{
  std::uint64_t                                     window{latency_20};
  std::uint64_t                                     first{};
  std::uint64_t                                     end{script_end_ps};
  std::uint64_t                                     slice{coarse};
  bool                                              threaded{};
  std::optional<std::uint32_t>                      seed{};
  std::optional<host_byte>                          host{};
  std::optional<member_reset>                       reset{};
  std::copyable_function<void(std::uint64_t) const> after_drain{};
  std::vector<member_call>                          calls{};
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

  auto link(
      std::size_t       from,
      std::size_t       to,
      std::uint64_t     latency,
      stm32::line_shape shape = declared()) -> stm32::uart_wire&
  {
    auto&      sender   = *nodes[from];
    auto&      receiver = *nodes[to];
    auto const lane     = static_cast<std::uint32_t>(2 * wires.size() + 1);
    auto*      machine  = receiver.f->target.get();
    auto       wire     = std::make_shared<stm32::uart_wire>(
        stm32::wire_ends{.sender = from, .receiver = to},
        stm32::wire_lanes{.peer = lane, .injected = lane + 1},
        core::sys_time{static_cast<std::int64_t>(latency)}, shape,
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

  void drain(std::size_t member, std::uint64_t window, std::uint64_t barrier, std::uint64_t next)
  {
    for(auto const& wire : wires)
    {
      if(wire->receiver() == member)
      {
        auto const found = wire->drain(
            window, core::sys_time{static_cast<std::int64_t>(barrier)},
            core::sys_time{static_cast<std::int64_t>(next)}, nodes[member]->map());
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
    for(auto const& one_call : plan.calls)
    {
      if(one_call.member == member
         and one_call.at < barrier
         and one_call.at >= barrier - std::min(barrier, plan.window))
      {
        if(one.advance(one_call.at, plan.slice, false) != stop_reason::time)
        {
          one.failed = true;
          return;
        }
        one_call.call(one.now());
      }
    }
    if(plan.reset
       and plan.reset->member == member
       and plan.reset->at <= barrier
       and plan.reset->at > barrier - std::min(barrier, plan.window))
    {
      reset(member, *plan.reset, plan.slice);
      if(one.failed)
      {
        return;
      }
    }
    auto reason = one.advance(barrier, plan.slice, true);
    while(reason == stop_reason::reset_request and not one.failed)
    {
      reset_now(member, {});
      reason = one.advance(barrier, plan.slice, true);
    }
    one.done   = reason == stop_reason::user;
    one.failed = reason != stop_reason::user and reason != stop_reason::time;
  }

  void reset(std::size_t member, member_reset const& at, std::uint64_t slice)
  {
    auto& one = *nodes[member];
    if(one.advance(at.at, slice, false) != stop_reason::time)
    {
      one.failed = true;
      return;
    }
    reset_now(member, at.entry);
  }

  void reset_now(std::size_t member, std::string_view entry)
  {
    auto&      one    = *nodes[member];
    auto const system = core::sys_time{static_cast<std::int64_t>(one.now())};
    for(auto const& wire : wires)
    {
      if(wire->sender() == member)
      {
        wire->sender_reset(system);
      }
      if(wire->receiver() == member)
      {
        wire->receiver_reset(system);
      }
    }
    auto const local = static_cast<std::int64_t>(one.f->target->virtual_clock().time().ps);
    one.f->target->reset();
    one.base += local;
    one.reset_at = system.ps;
    if(entry.empty())
    {
      return;
    }
    auto const booted = one.f->run(
        {.max_instructions = icount{100'000'000},
         .wall_budget      = std::chrono::seconds{60},
         .until            = sym("usart_ll", "main")});
    if(not booted.stopped or booted.stopped->reason != stop_reason::address)
    {
      one.failed = true;
      return;
    }
    one.f->set("pc", sym("usart_ll", entry));
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
          drain(member, window, barrier(window), (window + 2) * plan.window);
        }
        if(plan.after_drain)
        {
          plan.after_drain(window);
        }
      }
    }
    else
    {
      core::window_barrier       meet{nodes.size()};
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
              drain(member, window, barrier(window), (window + 2) * plan.window);
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
    drain(1, 0, end, end + ms_ps);
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
  a.base   = alone.base;
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
  std::vector<std::uint16_t> const late{0x66};
  REQUIRE(
      reference.wires[0]->inject(late, core::sys_time{2'500'000'000}, std::nullopt).has_value());
  reference.run({.window = latency_20});
  auto const expected = reference.dump();

  auto r = one_way("link_tx", "link_rx", latency_20);
  REQUIRE(r.wires[0]->replace(60, edit));
  REQUIRE(r.wires[0]->inject(late, core::sys_time{2'500'000'000}, std::nullopt).has_value());
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
      CHECK(state.injections.size() == 1);
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
      r.drain(1, k, barrier(k), barrier(k + 1));
      ++k;
      r.run_window(1, k, barrier(k), plan, {});
    }
    r.drain(1, k, barrier(k), barrier(k + 1));
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
auto edited_run(run_plan const& plan, std::function_ref<void(stm32::uart_wire&)> arm) -> rig
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
  constexpr std::uint32_t frames = 500;
  constexpr std::size_t   wanted = 50;
#else
  constexpr std::uint32_t frames = 2'000;
  constexpr std::size_t   wanted = 200;
#endif
  constexpr std::size_t   round_cap = 50;
  constexpr std::uint64_t frame_ps  = 21'700 * 1'000;
  enum class outcome : char
  {
    untried,
    refused,
    applied
  };
  std::size_t applied = 0;
  std::size_t refused = 0;
  std::size_t raced   = 0;
  std::size_t round   = 0;
  for(; round < round_cap and (applied < wanted or refused < wanted); ++round)
  {
    rig   r;
    auto& a = r.add("link_tx_count");
    r.add("link_rx_quiet");
    REQUIRE(a.f->target->mem().write(sym("usart_ll", "link_count"), 4, frames).has_value());
    auto&                wire = r.link(0, 1, latency_20);
    std::vector<outcome> tried(frames, outcome::untried);
    std::vector<char>    at_edge(frames);
    std::jthread         arming{[&](std::stop_token const& token) {
      std::mt19937                rng{static_cast<std::uint32_t>(11 + round)};
      std::bernoulli_distribution late{0.5};
      auto const                  past = [&](std::uint64_t index) {
        while(wire.posted() <= index and not token.stop_requested())
        {
          std::this_thread::yield();
        }
      };
      while(not token.stop_requested())
      {
        auto const index = wire.posted();
        if(index >= frames)
        {
          break;
        }
        auto const after_start = late(rng);
        if(after_start)
        {
          past(index);
        }
        std::vector<std::uint16_t> const data{static_cast<std::uint16_t>((index + 0x80u) & 0xffu)};
        tried[index]   = wire.replace(index, data) ? outcome::applied : outcome::refused;
        at_edge[index] = after_start ? 0 : 1;
        past(index);
      }
    }};
    r.run({.window = latency_20, .end = frames * frame_ps + ms_ps, .threaded = true});
    arming          = {};
    auto const& tap = r.taps[0];
    REQUIRE(tap.size() == frames);
    for(std::uint32_t index = 0; index < frames; ++index)
    {
      CAPTURE(round);
      CAPTURE(index);
      auto const original = index & 0xffu;
      auto const edited   = tried[index] == outcome::applied;
      CHECK(tap[index].data == (edited ? (original + 0x80u) & 0xffu : original));
      applied += edited ? 1 : 0;
      refused += tried[index] == outcome::refused ? 1 : 0;
      raced += tried[index] == outcome::refused and at_edge[index] != 0 ? 1 : 0;
    }
    auto const idle = entries(r.nodes[1]->f->log());
    REQUIRE(idle.size() == 1);
    CHECK(idle[0].data == frames);
  }
  MESSAGE(
      round, " rounds, applied ", applied, ", refused ", refused, ", refused at the edge ", raced);
  CHECK(applied >= wanted);
  CHECK(refused >= wanted);
}

namespace
{
auto receiver_now(rig const& r) -> stm32::stopped_receiver
{
  auto const& b = *r.nodes[1];
  return {.now = core::sys_time{static_cast<std::int64_t>(b.now())}, .map = b.map()};
}

auto injected_byte(
    stm32::uart_wire&                      wire,
    std::uint16_t                          data,
    std::int64_t                           at,
    std::optional<stm32::stopped_receiver> stopped) -> std::expected<void, stm32::inject_refusal>
{
  std::vector<std::uint16_t> const one{data};
  return wire.inject(one, core::sys_time{at}, stopped);
}

auto earliest_fault(rig const& r) -> std::optional<fault_seen>
{
  std::optional<fault_seen> out;
  for(auto const& one : r.nodes)
  {
    for(auto const& fault : one->faults)
    {
      if(not out or fault.at < out->at)
      {
        out = fault;
      }
    }
  }
  return out;
}
}

TEST_CASE("an injection while stopped arrives at its time and cancels the IDLE due after it")
{
  auto full = one_way("link_tx", "link_rx", latency_20);
  full.oracle(script_end_ps);
  auto const& all  = full.taps[0];
  auto const  gap  = all[39].arrival.ps;
  auto const  stop = static_cast<std::uint64_t>(gap) + 5 * us_ps;

  auto stopped = one_way("link_tx", "link_rx", latency_20);
  stopped.run({.window = latency_20, .end = stop});
  auto const now = receiver_now(stopped);
  CHECK(now.now.ps >= static_cast<std::int64_t>(stop));
  auto const early = injected_byte(*stopped.wires[0], 0x77, now.now.ps - 1, now);
  REQUIRE_FALSE(early.has_value());
  CHECK(early.error().reason == stm32::inject_refusal_reason::before_receiver);
  CHECK(early.error().bound == now.now);
  REQUIRE(injected_byte(*stopped.wires[0], 0x77, now.now.ps, now).has_value());
  stopped.run({.window = latency_20, .first = stop / latency_20});

  auto const bytes = bytes_of(stopped.nodes[1]->f->log());
  REQUIRE(bytes.size() == 65);
  CHECK(bytes[40] == 0x77u);
  auto const seen   = entries(stopped.nodes[1]->f->log());
  auto const byte39 = std::ranges::find_if(
      seen, [](entry const& one) { return one.kind == link_byte and one.data == 0x20u + 39u; });
  REQUIRE(byte39 != seen.end());
  REQUIRE(byte39 + 1 != seen.end());
  CHECK(byte39[1].kind == link_byte);
  CHECK(byte39[1].data == 0x77u);
  auto const frame_ps = static_cast<std::int64_t>(declared().frame_ps());
  auto const injected = std::ranges::find_if(
      stopped.taps[0], [](stm32::tap_frame const& frame) { return frame.injected; });
  REQUIRE(injected != stopped.taps[0].end());
  CHECK(injected->start == now.now);
  CHECK(injected->end.ps == now.now.ps + frame_ps);
  CHECK(injected->arrival == injected->end);
  CHECK(injected->index == 0);
  CHECK(std::ranges::is_sorted(stopped.taps[0], {}, [](stm32::tap_frame const& frame) {
    return frame.end.ps;
  }));

  auto queued = one_way("link_tx", "link_rx", latency_20);
  REQUIRE(injected_byte(*queued.wires[0], 0x77, now.now.ps, std::nullopt).has_value());
  queued.run({.window = latency_20});
  CHECK(queued.nodes[1]->f->log() == stopped.nodes[1]->f->log());
  CHECK(queued.taps[0] == stopped.taps[0]);
}

TEST_CASE("an injection while stopped is refused over a frame delivered or still in a mailbox")
{
  auto full = one_way("link_tx", "link_rx", latency_20);
  full.oracle(script_end_ps);
  auto const&    all    = full.taps[0];
  constexpr auto window = static_cast<std::int64_t>(latency_20);
  auto const     next   = *std::ranges::find_if(std::views::iota(5, 19), [&](int index) {
    auto const start = all[static_cast<std::size_t>(index)].start.ps;
    return start / window == (start + static_cast<std::int64_t>(us_ps)) / window;
  });
  auto const&    frame  = all[static_cast<std::size_t>(next)];
  auto const&    before = all[static_cast<std::size_t>(next) - 1];
  auto const     stop   = static_cast<std::uint64_t>(frame.start.ps) + us_ps;
  auto           r      = one_way("link_tx", "link_rx", latency_20);
  r.run({.window = latency_20, .end = stop});
  auto const now   = receiver_now(r);
  auto const state = saved_state(*r.wires[0]);
  auto const holds = [&](std::vector<stm32::detail::wire_post> const& posts) {
    return std::ranges::any_of(posts, [&](stm32::detail::wire_post const& post) {
      return not post.abort and post.index == frame.index;
    });
  };
  REQUIRE((holds(state.mail_even) or holds(state.mail_odd)));
  auto const arrival = frame.start.ps + window;
  auto const posted  = injected_byte(*r.wires[0], 0x55, arrival, now);
  REQUIRE_FALSE(posted.has_value());
  CHECK(posted.error().reason == stm32::inject_refusal_reason::overlaps);
  CHECK(posted.error().bound.ps == arrival);
  auto const on_line = injected_byte(*r.wires[0], 0x55, now.now.ps, now);
  REQUIRE_FALSE(on_line.has_value());
  CHECK(on_line.error().reason == stm32::inject_refusal_reason::overlaps);
  CHECK(on_line.error().bound.ps == before.start.ps + window);
}

TEST_CASE("an injection while running is refused before the drains' reach, else delivered")
{
  constexpr std::int64_t late      = 2'500 * static_cast<std::int64_t>(us_ps);
  auto                   reference = one_way("link_tx", "link_rx", latency_20);
  REQUIRE(injected_byte(*reference.wires[0], 0x66, late, std::nullopt).has_value());
  reference.run({.window = latency_20});
  auto const bytes = bytes_of(reference.nodes[1]->f->log());
  REQUIRE(bytes.size() == 65);
  CHECK(bytes.back() == 0x66u);

  auto                                 r    = one_way("link_tx", "link_rx", latency_20);
  auto&                                wire = *r.wires[0];
  std::optional<stm32::inject_refusal> refused;
  bool                                 accepted = false;
  std::jthread                         injecting{[&] {
    for(;;)
    {
      auto const early = injected_byte(wire, 0x11, -1, std::nullopt);
      if(early.has_value())
      {
        return;
      }
      if(early.error().bound.ps > 0)
      {
        refused  = early.error();
        accepted = injected_byte(wire, 0x66, late, std::nullopt).has_value();
        return;
      }
      std::this_thread::yield();
    }
  }};
  r.run({.window = latency_20, .threaded = true});
  injecting = {};
  REQUIRE(refused.has_value());
  CHECK(refused->reason == stm32::inject_refusal_reason::before_drained);
  CHECK(refused->bound.ps > 0);
  CHECK(refused->bound.ps % static_cast<std::int64_t>(latency_20) == 0);
  CHECK(accepted);
  auto const seen = bytes_of(r.nodes[1]->f->log());
  CHECK(seen == bytes);
}

TEST_CASE("an injection overlapping later peer frames is a system error at the earliest start")
{
  auto full = one_way("link_tx", "link_rx", latency_20);
  full.oracle(script_end_ps);
  auto const& all      = full.taps[0];
  auto const  frame_ps = all[10].end.ps - all[10].start.ps;
  auto const  at       = all[10].start.ps + static_cast<std::int64_t>(latency_20) - frame_ps / 2;
  auto const  expected = fault_seen{.at = all[10].start.ps, .member = 0};
  auto const  arm      = [&](rig& r) {
    REQUIRE(injected_byte(*r.wires[0], 0x5a, at, std::nullopt).has_value());
  };
  auto reference = one_way("link_tx", "link_rx", latency_20);
  arm(reference);
  reference.oracle(script_end_ps);
  CHECK(earliest_fault(reference) == expected);
  for(auto const& plan : {
          run_plan{.window = latency_20},
          run_plan{.window = latency_20 / 3},
          run_plan{.window = latency_20, .threaded = true}
  })
  {
    auto r = one_way("link_tx", "link_rx", latency_20);
    arm(r);
    r.run(plan);
    CHECK(earliest_fault(r) == expected);
  }
}

TEST_CASE("an injection after an aborted frame's cut does not overlap its untransmitted rest")
{
  auto full = one_way("link_abort_gap", "link_rx", latency_20);
  full.oracle(script_end_ps);
  auto const& all = full.taps[0];
  REQUIRE(all.size() == 3);
  REQUIRE(all[1].lost);
  auto const at = all[1].arrival.ps + static_cast<std::int64_t>(us_ps);
  REQUIRE(
      at + static_cast<std::int64_t>(declared().frame_ps())
      < all[2].start.ps + static_cast<std::int64_t>(latency_20));
  for(auto const window : {latency_20, latency_20 / 4})
  {
    auto r = one_way("link_abort_gap", "link_rx", latency_20);
    REQUIRE(injected_byte(*r.wires[0], 0x42, at, std::nullopt).has_value());
    r.run({.window = window});
    CHECK_FALSE(earliest_fault(r).has_value());
    CHECK(bytes_of(r.nodes[1]->f->log()) == std::vector<std::uint32_t>{'a', 0x42, 'c'});
  }
}

TEST_CASE("the tap orders peer and injected frames by their settle time at any window")
{
  auto full = one_way("link_tx", "link_rx", latency_20);
  full.oracle(script_end_ps);
  auto const at   = full.taps[0][39].arrival.ps + 30 * static_cast<std::int64_t>(us_ps);
  auto const with = [at](rig& r) {
    REQUIRE(injected_byte(*r.wires[0], 0x3c, at, std::nullopt).has_value());
  };
  auto reference = one_way("link_tx", "link_rx", latency_20);
  with(reference);
  reference.oracle(script_end_ps);
  auto const& tap = reference.taps[0];
  REQUIRE(tap.size() == 65);
  CHECK(
      std::ranges::is_sorted(tap, {}, [](stm32::tap_frame const& frame) { return frame.end.ps; }));
  CHECK(
      std::ranges::count_if(tap, [](stm32::tap_frame const& frame) { return frame.injected; })
      == 1);
  for(auto const window : {latency_20, latency_20 / 4})
  {
    auto r = one_way("link_tx", "link_rx", latency_20);
    with(r);
    r.run({.window = window});
    CHECK(r.taps[0] == tap);
    CHECK(r.nodes[1]->f->log() == reference.nodes[1]->f->log());
  }
}

TEST_CASE("a sender reset mid-frame loses that frame, frames ended before keep their old base")
{
  std::vector<std::string> dumps;
  for(auto const& plan : {
          run_plan{.window = latency_20},
          run_plan{.window = latency_20 / 4},
          run_plan{.window = latency_20, .slice = fine}
  })
  {
    auto r = one_way("link_tx_reset", "link_rx", latency_20);
    r.run(plan);
    auto const& a   = *r.nodes[0];
    auto const& tap = r.taps[0];
    REQUIRE(a.reset_at > 0);
    REQUIRE(tap.size() > 6);
    REQUIRE(a.own->frames.size() > 5);
    for(std::size_t index = 0; index < 5; ++index)
    {
      CAPTURE(index);
      CHECK(tap[index].data == 0x20u + index);
      CHECK_FALSE(tap[index].lost);
      CHECK(tap[index].end.ps == static_cast<std::int64_t>(a.own->frames[index].at.ps - a.origin));
      CHECK(tap[index].arrival.ps == tap[index].end.ps + static_cast<std::int64_t>(latency_20));
    }
    CHECK(tap[5].data == 0x25u);
    CHECK(tap[5].lost);
    CHECK(tap[5].end.ps == a.reset_at);
    CHECK(tap[5].start.ps < a.reset_at);
    CHECK(tap[6].start.ps > a.reset_at);
    auto const bytes = bytes_of(r.nodes[1]->f->log());
    REQUIRE(bytes.size() >= 5);
    CHECK(
        std::ranges::equal(
            std::span{bytes}.first(5), std::vector<std::uint32_t>{0x20, 0x21, 0x22, 0x23, 0x24}));
    CHECK(std::ranges::find(bytes, 0x25u) == bytes.end());
    dumps.push_back(r.dump());
  }
  CHECK(dumps[1] == dumps[0]);
  CHECK(dumps[2] == dumps[0]);
}

TEST_CASE("a receiver reset drops the frames arriving within L after it, drained ahead or not")
{
  constexpr auto latency = ms_ps;
  constexpr auto end     = 14 * ms_ps;
  auto           full    = one_way("link_tx_long", "link_rx_quiet", latency);
  full.oracle(end);
  auto const& all = full.taps[0];
  REQUIRE(all.size() == 512);
  auto const                 at = static_cast<std::uint64_t>(all[100].arrival.ps) + 7 * us_ps;
  std::vector<std::string>   dumps;
  std::vector<std::uint32_t> counts;
  for(auto const& plan : {
          run_plan{
                   .window = latency,
                   .end    = end,
                   .reset  = member_reset{.member = 1, .at = at, .entry = "link_rx_quiet"}},
          run_plan{
                   .window = latency / 4,
                   .end    = end,
                   .reset  = member_reset{.member = 1, .at = at, .entry = "link_rx_quiet"}},
          run_plan{
                   .window = latency,
                   .end    = end,
                   .slice  = fine,
                   .reset  = member_reset{.member = 1, .at = at, .entry = "link_rx_quiet"}}
  })
  {
    auto r = one_way("link_tx_long", "link_rx_quiet", latency);
    r.run(plan);
    auto const reset_at = r.nodes[1]->reset_at;
    CHECK(reset_at >= static_cast<std::int64_t>(at));
    CHECK(r.taps[0] == all);
    auto const kept = std::ranges::count_if(all, [&](stm32::tap_frame const& frame) {
      return frame.arrival.ps - (frame.end.ps - frame.start.ps)
          >= reset_at + static_cast<std::int64_t>(latency);
    });
    auto const seen = entries(r.nodes[1]->f->log());
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].data == kept);
    counts.push_back(seen[0].data);
    dumps.push_back(r.dump());
  }
  CHECK(dumps[1] == dumps[0]);
  CHECK(dumps[2] == dumps[0]);
}

TEST_CASE("a receiver reset drops an injection queued within L after it at any window")
{
  constexpr auto latency = ms_ps;
  constexpr auto end     = 4 * ms_ps;
  auto           full    = one_way("link_tx", "link_rx_quiet", latency);
  full.oracle(end);
  auto const& all = full.taps[0];
  REQUIRE(all.size() == 64);
  REQUIRE(all[0].start.ps > static_cast<std::int64_t>(300 * us_ps));
  auto const at       = static_cast<std::uint64_t>(all[0].start.ps) - 300 * us_ps;
  auto const injected = static_cast<std::int64_t>(at + 9 * latency / 10);
  REQUIRE(
      injected + static_cast<std::int64_t>(declared().frame_ps())
      < all[0].start.ps + static_cast<std::int64_t>(latency));
  std::vector<std::string> dumps;
  for(auto const window : {latency, latency / 4, latency / 7})
  {
    CAPTURE(window);
    auto r = one_way("link_tx", "link_rx_quiet", latency);
    REQUIRE(injected_byte(*r.wires[0], 0x99, injected, std::nullopt).has_value());
    r.run({
        .window = window,
        .end    = end,
        .reset  = member_reset{.member = 1, .at = at, .entry = "link_rx_quiet"}
    });
    auto const seen = entries(r.nodes[1]->f->log());
    REQUIRE_FALSE(seen.empty());
    auto const reset_at = r.nodes[1]->reset_at;
    auto const kept     = std::ranges::count_if(
        all, [&](stm32::tap_frame const& frame) { return frame.start.ps >= reset_at; });
    CHECK(kept == 64);
    CHECK(seen.back().data == kept);
    auto const tapped = std::ranges::find_if(
        r.taps[0], [](stm32::tap_frame const& frame) { return frame.injected; });
    CHECK(tapped != r.taps[0].end());
    dumps.push_back(r.dump());
  }
  CHECK(dumps[1] == dumps[0]);
  CHECK(dumps[2] == dumps[0]);
}

namespace
{
void poke(node const& one, std::string_view peripheral, std::string_view reg, std::uint32_t value)
{
  auto const& desc  = one.f->target->peripheral(peripheral).description();
  auto const  found = popcorn::find_register(desc, reg);
  REQUIRE(found != nullptr);
  REQUIRE(one.f->target->mem().write(desc.base + found->offset, 4, value).has_value());
}

auto peek(node const& one, std::string_view peripheral, std::string_view reg) -> std::uint32_t
{
  auto const& desc  = one.f->target->peripheral(peripheral).description();
  auto const  found = popcorn::find_register(desc, reg);
  REQUIRE(found != nullptr);
  auto const value = one.f->target->mem().read(desc.base + found->offset, 4);
  REQUIRE(value.has_value());
  return static_cast<std::uint32_t>(*value);
}

void line_7n1(node const& one)
{
  constexpr std::uint32_t cr1 = 1u | (1u << 2) | (1u << 3) | (1u << 28) | (1u << 29);
  poke(one, "RCC", "RCC_APB2ENR", peek(one, "RCC", "RCC_APB2ENR") | (1u << 14));
  poke(one, "USART1", "BRR", 35);
  poke(one, "USART1", "CR1_enabled", cr1);
}

void even_7e1(node const& one)
{
  constexpr std::uint32_t cr1 = 1u | (1u << 2) | (1u << 3) | (1u << 10) | (1u << 29);
  poke(one, "RCC", "RCC_APB2ENR", peek(one, "RCC", "RCC_APB2ENR") | (1u << 14));
  poke(one, "USART1", "BRR", 35);
  poke(one, "USART1", "CR1_enabled", cr1);
}
}

TEST_CASE("a 7E1 link taps data bits and gives edited and injected frames their parity bit")
{
  rig r;
  r.add("idle");
  r.add("idle");
  auto& wire = r.link(
      0, 1, latency_20,
      {.baud = 114'286, .word_bits = 8, .half_stops = 2, .parity = periph::line_parity::even});
  for(auto const& one : r.nodes)
  {
    even_7e1(*one);
  }
  std::vector<std::uint16_t> const edit{0x01};
  REQUIRE(wire.replace(1, edit));
  REQUIRE(injected_byte(wire, 0xc1, static_cast<std::int64_t>(ms_ps), std::nullopt).has_value());
  r.enter(0, 0);
  for(auto const value : {0x41u, 0x40u, 0x7fu})
  {
    poke(*r.nodes[0], "USART1", "TDR", value);
  }
  r.run({.window = latency_20});
  std::vector<std::uint32_t> tapped;
  for(auto const& frame : r.taps[0])
  {
    tapped.push_back(frame.data);
  }
  CHECK(tapped == std::vector<std::uint32_t>{0x41, 0x01, 0x7f, 0x41});
  auto const isr = peek(*r.nodes[1], "USART1", "ISR_enabled");
  CHECK((isr & 1u) == 0);
  CHECK((isr & (1u << 5)) != 0);
  CHECK(peek(*r.nodes[1], "USART1", "RDR") == 0x41);
  CHECK(r.nodes[1]->warnings.empty());
}

TEST_CASE("a 7N1 link masks edited and injected frames to 7 bits")
{
  rig r;
  r.add("idle");
  r.add("idle");
  auto& wire = r.link(
      0, 1, latency_20,
      {.baud = 114'286, .word_bits = 7, .half_stops = 2, .parity = periph::line_parity::none});
  for(auto const& one : r.nodes)
  {
    line_7n1(*one);
  }
  std::vector<std::uint16_t> const edit{0xff};
  REQUIRE(wire.replace(1, edit));
  REQUIRE(injected_byte(wire, 0xff, static_cast<std::int64_t>(ms_ps), std::nullopt).has_value());
  r.enter(0, 0);
  for(auto const value : {0x41u, 0x40u})
  {
    poke(*r.nodes[0], "USART1", "TDR", value);
  }
  r.run({.window = latency_20});
  std::vector<std::uint32_t> tapped;
  for(auto const& frame : r.taps[0])
  {
    tapped.push_back(frame.data);
  }
  CHECK(tapped == std::vector<std::uint32_t>{0x41, 0x7f, 0x7f});
  CHECK(r.nodes[1]->warnings.empty());

  rig lone;
  lone.add("idle");
  lone.add("idle");
  auto& only = lone.link(
      0, 1, latency_20,
      {.baud = 114'286, .word_bits = 7, .half_stops = 2, .parity = periph::line_parity::none});
  for(auto const& one : lone.nodes)
  {
    line_7n1(*one);
  }
  REQUIRE(injected_byte(only, 0xff, static_cast<std::int64_t>(ms_ps), std::nullopt).has_value());
  lone.enter(0, 0);
  lone.run({.window = latency_20});
  CHECK(peek(*lone.nodes[1], "USART1", "RDR") == 0x7f);
}

namespace
{
constexpr std::int64_t open_end = std::numeric_limits<std::int64_t>::max();

using cut_result = std::expected<core::sys_time, stm32::cut_refusal>;

auto paired(std::string_view a, std::string_view b, std::uint64_t latency) -> rig
{
  rig r;
  r.add(a);
  r.add(b);
  r.link(0, 1, latency);
  r.link(1, 0, latency);
  auto const shared = std::make_shared<stm32::link_registration>();
  r.wires[0]->share_registration(shared);
  r.wires[1]->share_registration(shared);
  return r;
}

auto cut_rig(
    rig&                        r,
    std::int64_t                start,
    std::optional<std::int64_t> end,
    std::int64_t                now,
    bool                        low = false) -> cut_result
{
  return stm32::cut_link(
      *r.wires[0], *r.wires[1], core::sys_time{start},
      end ? std::optional{core::sys_time{*end}} : std::nullopt, low, core::sys_time{now});
}

auto stopped_now(rig const& r) -> std::int64_t
{
  std::int64_t out = 0;
  for(auto const& one : r.nodes)
  {
    out = std::max(out, static_cast<std::int64_t>(one->now()));
  }
  return out;
}

auto oracle_tap(std::string_view sender, std::string_view receiver, std::uint64_t latency)
    -> std::vector<stm32::tap_frame>
{
  auto full = one_way(sender, receiver, latency);
  full.oracle(script_end_ps);
  return full.taps[0];
}

auto idle_cycles(std::vector<std::uint32_t> const& log) -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(auto const& one : entries(log))
  {
    if(one.kind == 2)
    {
      out.push_back(one.cycles);
    }
  }
  return out;
}

auto without(std::vector<stm32::tap_frame> const& all, std::size_t first, std::size_t last)
    -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(std::size_t index = 0; index < all.size(); ++index)
  {
    if(index < first or index > last)
    {
      out.push_back(all[index].data);
    }
  }
  return out;
}

auto variants_of(std::uint64_t latency) -> std::vector<run_plan>
{
  return {
      run_plan{.window = latency},
      run_plan{.window = latency / 3},
      run_plan{.window = latency, .slice = fine},
      run_plan{.window = latency, .threaded = true},
      run_plan{.window = latency / 3, .threaded = true, .seed = 7},
  };
}

struct stub_port final : stm32::uart_link_port
{
  std::vector<periph::line_frame>                          received;
  std::vector<std::int64_t>                                cancels;
  std::move_only_function<void(periph::line_frame const&)> on_receive;
  std::vector<std::pair<char, std::int64_t>>               calls;

  void receive_at(periph::line_frame const& frame, std::uint32_t) override
  {
    received.push_back(frame);
    calls.emplace_back('r', static_cast<std::int64_t>(frame.start.ps));
    if(on_receive)
    {
      on_receive(frame);
    }
  }

  void cancel_in_flight(vtime at, std::uint32_t) override
  {
    cancels.push_back(static_cast<std::int64_t>(at.ps));
    calls.emplace_back('c', static_cast<std::int64_t>(at.ps));
  }

  void hold_line(vtime from, std::uint32_t lane) override
  {
    CHECK(lane == 1);
    calls.emplace_back('h', static_cast<std::int64_t>(from.ps));
  }

  void release_line(vtime at, std::uint32_t lane) override
  {
    CHECK(lane == 1);
    calls.emplace_back('l', static_cast<std::int64_t>(at.ps));
  }

  void receive_frames(std::span<periph::host_frame const>) override
  {}

  [[nodiscard]] auto queued() const -> std::size_t override
  {
    return 0;
  }

  void set_cts(bool, vtime) override
  {}

  [[nodiscard]] auto pending_cts() const -> std::size_t override
  {
    return 0;
  }

  [[nodiscard]] auto cts_ready() const -> bool override
  {
    return true;
  }

  [[nodiscard]] auto ready_to_receive() const -> bool override
  {
    return true;
  }

  void attach(std::shared_ptr<periph::uart_endpoint>) override
  {}
};

struct stub_link
{
  std::int64_t                              latency;
  stm32::line_shape                         shape;
  stub_port                                 forth_port;
  stub_port                                 back_port;
  std::shared_ptr<stm32::link_registration> shared = std::make_shared<stm32::link_registration>();
  std::shared_ptr<stm32::uart_wire>         forth;
  std::shared_ptr<stm32::uart_wire>         back;
  std::vector<stm32::tap_frame>             tap;
  std::move_only_function<void(std::uint64_t, std::int64_t)> after;

  stub_link(std::int64_t latency_ps, stm32::line_shape line) : latency{latency_ps}, shape{line}
  {
    forth = std::make_shared<stm32::uart_wire>(
        stm32::wire_ends{.sender = 0, .receiver = 1}, stm32::wire_lanes{.peer = 1, .injected = 2},
        core::sys_time{latency}, shape,
        stm32::wire_receiver{.port = &forth_port, .unit = "USART1", .log = {}, .strict = false},
        nullptr);
    back = std::make_shared<stm32::uart_wire>(
        stm32::wire_ends{.sender = 1, .receiver = 0}, stm32::wire_lanes{.peer = 1, .injected = 2},
        core::sys_time{latency}, shape,
        stm32::wire_receiver{.port = &back_port, .unit = "USART1", .log = {}, .strict = false},
        nullptr);
    forth->share_registration(shared);
    back->share_registration(shared);
  }

  [[nodiscard]] auto frame(std::int64_t start) const -> periph::line_frame
  {
    auto const ps = shape.frame_ps();
    return {
        .data     = 0x55,
        .frame_ps = ps,
        .start    = vtime{static_cast<std::uint64_t>(start)},
        .end      = vtime{static_cast<std::uint64_t>(start) + ps}};
  }

  void window(std::uint64_t k, std::int64_t q, std::span<std::int64_t const> starts)
  {
    auto const begin = static_cast<std::int64_t>(k) * q;
    forth->enter_window(k, {});
    back->enter_window(k, {});
    for(auto const start : starts)
    {
      if(start >= begin and start < begin + q)
      {
        forth->started(frame(start));
      }
    }
  }

  void drain(std::uint64_t k, std::int64_t q)
  {
    auto const barrier = core::sys_time{(static_cast<std::int64_t>(k) + 1) * q};
    auto const next    = core::sys_time{(static_cast<std::int64_t>(k) + 2) * q};
    forth->drain(k, barrier, next, {});
    back->drain(k, barrier, next, {});
    auto taken = forth->tap().take();
    tap.insert(tap.end(), taken.begin(), taken.end());
    if(after)
    {
      after(k, barrier.ps);
    }
  }

  void run(std::span<std::int64_t const> starts, std::int64_t q, std::int64_t until)
  {
    for(std::uint64_t k = 0; static_cast<std::int64_t>(k) * q < until; ++k)
    {
      window(k, q, starts);
      drain(k, q);
    }
  }

  auto
      cut(std::int64_t                start,
          std::optional<std::int64_t> end,
          std::optional<std::int64_t> now,
          bool                        low = false) -> cut_result
  {
    return stm32::cut_link(
        *forth, *back, core::sys_time{start},
        end ? std::optional{core::sys_time{*end}} : std::nullopt, low,
        now ? std::optional{core::sys_time{*now}} : std::nullopt);
  }
};

auto received_at(stub_link const& link, std::int64_t start) -> bool
{
  return std::ranges::any_of(link.forth_port.received, [&](periph::line_frame const& frame) {
    return static_cast<std::int64_t>(frame.start.ps) == start + link.latency;
  });
}
}

TEST_CASE("a cut keeps frames starting inside it off the line and truncates the one crossing t0")
{
  auto const all = oracle_tap("link_tx", "link_rx", latency_20);
  REQUIRE(all.size() == 64);
  auto const t0  = all[10].start.ps + static_cast<std::int64_t>(5 * us_ps);
  auto const t1  = all[14].start.ps + static_cast<std::int64_t>(5 * us_ps);
  auto const run = [&](run_plan const& plan, std::int64_t start, std::int64_t end) {
    auto r = paired("link_tx", "link_rx", latency_20);
    REQUIRE(cut_rig(r, start, end, 0).has_value());
    r.run(plan);
    return r;
  };
  auto        reference = run({.window = latency_20}, t0, t1);
  auto const& tap       = reference.taps[0];
  REQUIRE(tap.size() == 64);
  for(std::size_t index = 0; index < tap.size(); ++index)
  {
    CAPTURE(index);
    CHECK(tap[index].cut == (index >= 10 and index <= 14));
    CHECK(tap[index].lost == (index == 10 or index == 14));
    CHECK(tap[index].start == all[index].start);
    CHECK(tap[index].end == all[index].end);
    CHECK(tap[index].arrival == all[index].arrival);
  }
  CHECK(reference.taps[1].empty());
  CHECK(bytes_of(reference.nodes[1]->f->log()) == without(all, 10, 14));
  auto const expected = reference.dump();
  for(auto const& plan : variants_of(latency_20))
  {
    CAPTURE(plan.window);
    CAPTURE(plan.threaded);
    CHECK(run(plan, t0, t1).dump() == expected);
  }

  auto const later = run({.window = latency_20}, t0 + static_cast<std::int64_t>(us_ps), t1);
  auto const idle  = idle_cycles(reference.nodes[1]->f->log());
  auto const moved = idle_cycles(later.nodes[1]->f->log());
  REQUIRE(idle.size() == 4);
  REQUIRE(moved.size() == 4);
  CHECK(moved[0] - idle[0] == us_ps / cycle_ps);
  CHECK(std::ranges::equal(std::span{moved}.subspan(1), std::span{idle}.subspan(1)));

  auto touching = run({.window = latency_20 / 3}, all[20].end.ps, all[24].start.ps);
  for(std::size_t index = 19; index <= 25; ++index)
  {
    CAPTURE(index);
    CHECK(touching.taps[0][index].cut == (index > 20 and index < 24));
    CHECK_FALSE(touching.taps[0][index].lost);
  }
  CHECK(bytes_of(touching.nodes[1]->f->log()) == without(all, 21, 23));
}

TEST_CASE("a cut acts on the link's other wire the same way")
{
  auto const               all = oracle_tap("link_tx", "link_rx", latency_20);
  auto const               t0  = all[10].start.ps + static_cast<std::int64_t>(5 * us_ps);
  auto const               t1  = all[14].start.ps + static_cast<std::int64_t>(5 * us_ps);
  std::vector<std::string> dumps;
  for(auto const& plan : {
          run_plan{.window = latency_20},
          run_plan{.window = latency_20 / 3, .threaded = true}
  })
  {
    auto r = paired("link_rx", "link_tx", latency_20);
    REQUIRE(cut_rig(r, t0, t1, 0).has_value());
    r.run(plan);
    auto const& tap = r.taps[1];
    REQUIRE(tap.size() == 64);
    for(std::size_t index = 0; index < tap.size(); ++index)
    {
      CAPTURE(index);
      CHECK(tap[index].cut == (index >= 10 and index <= 14));
      CHECK(tap[index].lost == (index == 10 or index == 14));
    }
    CHECK(bytes_of(r.nodes[0]->f->log()) == without(all, 10, 14));
    dumps.push_back(r.dump());
  }
  CHECK(dumps[1] == dumps[0]);
}

TEST_CASE("a frame aborted and cut keeps the abort's end, an abort before the cut leaves it")
{
  auto const all = oracle_tap("link_abort", "link_rx", latency_20);
  REQUIRE(all.size() == 3);
  REQUIRE(all[1].lost);
  for(auto const offset : {5 * us_ps, 15 * us_ps})
  {
    CAPTURE(offset);
    auto const               t0 = all[1].start.ps + static_cast<std::int64_t>(offset);
    std::vector<std::string> dumps;
    for(auto const window : {latency_20, latency_20 / 4})
    {
      auto r = paired("link_abort", "link_rx", latency_20);
      REQUIRE(cut_rig(r, t0, t0 + static_cast<std::int64_t>(us_ps), 0).has_value());
      r.run({.window = window});
      auto const& tap = r.taps[0];
      REQUIRE(tap.size() == 3);
      CHECK(tap[1].lost);
      CHECK(tap[1].end == all[1].end);
      CHECK(tap[1].cut == (t0 < all[1].end.ps));
      CHECK_FALSE(tap[0].cut);
      dumps.push_back(r.dump());
    }
    CHECK(dumps[1] == dumps[0]);
  }
}

TEST_CASE("restoring an open cut inside a frame drained while open marks it cut and lost")
{
  auto const     all    = oracle_tap("link_tx", "link_rx", latency_20);
  constexpr auto window = static_cast<std::int64_t>(latency_20);
  auto const     inside = [&](std::size_t index) {
    auto const barrier = (all[index].start.ps / window + 1) * window;
    REQUIRE(barrier + static_cast<std::int64_t>(us_ps) < all[index].end.ps);
    return barrier;
  };
  auto const   open_at    = inside(3);
  auto const   restore_at = inside(8);
  std::int64_t t0         = 0;
  std::int64_t t1         = 0;

  auto r = paired("link_tx", "link_rx", latency_20);
  r.run({.window = latency_20, .after_drain = [&](std::uint64_t k) {
           auto const barrier = static_cast<std::int64_t>(k + 1) * window;
           if(barrier == open_at)
           {
             t0 = stopped_now(r);
             CHECK(cut_rig(r, t0, std::nullopt, t0) == core::sys_time{t0});
           }
           if(barrier == restore_at)
           {
             t1 = stopped_now(r);
             CHECK(
                 stm32::restore_link(*r.wires[0], *r.wires[1], core::sys_time{t1})
                 == core::sys_time{t1});
           }
         }});
  REQUIRE(t0 >= open_at);
  REQUIRE(t1 >= restore_at);
  REQUIRE(t1 < all[8].end.ps);
  auto const& tap = r.taps[0];
  REQUIRE(tap.size() == 64);
  CHECK(tap[8].cut);
  CHECK(tap[8].lost);
  CHECK(tap[8].end == all[8].end);
  for(std::size_t index = 0; index < tap.size(); ++index)
  {
    CAPTURE(index);
    CHECK(tap[index].cut == (all[index].end.ps > t0 and all[index].start.ps < t1));
    CHECK(tap[index].lost == (index == 3 or index == 8));
  }

  auto scheduled = paired("link_tx", "link_rx", latency_20);
  REQUIRE(cut_rig(scheduled, t0, t1, 0).has_value());
  scheduled.run({.window = latency_20});
  CHECK(scheduled.dump() == r.dump());
}

TEST_CASE("a frame crossing t0 drained before the cut is registered is truncated at t0 plus L")
{
  auto const all     = oracle_tap("link_tx", "link_rx", latency_20);
  auto const q       = static_cast<std::int64_t>(latency_20);
  auto const barrier = [&](std::size_t index) { return (all[index].start.ps / q + 1) * q; };
  auto const run     = std::views::iota(std::size_t{1}, std::size_t{19});
  auto const found   = std::ranges::find_if(run, [&](std::size_t one) {
    return barrier(one) < all[one].end.ps
       and all[one].end.ps - barrier(one) < static_cast<std::int64_t>(8 * us_ps);
  });
  REQUIRE(found != run.end());
  auto const index = *found;
  auto const at    = barrier(index);

  std::vector<std::string>  dumps;
  std::vector<std::int64_t> starts;
  for(auto const window : {latency_20, latency_20 / 4})
  {
    CAPTURE(window);
    auto r          = paired("link_tx", "link_rx", latency_20);
    bool registered = false;
    r.run({.window = window, .after_drain = [&](std::uint64_t k) {
             if(static_cast<std::int64_t>((k + 1) * window) == at)
             {
               auto const& posted = saved_state(*r.wires[0]).delivered;
               CHECK(std::ranges::any_of(posted, [&](stm32::detail::line_span const& span) {
                 return span.post == all[index].start;
               }));
               auto const now = stopped_now(r);
               REQUIRE(cut_rig(r, now, now + 3 * q, now).has_value());
               starts.push_back(now);
               registered = true;
             }
           }});
    CHECK(registered);
    CHECK(r.taps[0][index].cut);
    CHECK(r.taps[0][index].lost);
    auto const bytes = bytes_of(r.nodes[1]->f->log());
    CHECK(std::ranges::find(bytes, all[index].data) == bytes.end());
    dumps.push_back(r.dump());
  }
  REQUIRE(starts.size() == 2);
  CHECK(starts[0] == starts[1]);
  CHECK(dumps[0] == dumps[1]);
  auto scheduled = paired("link_tx", "link_rx", latency_20);
  REQUIRE(cut_rig(scheduled, starts[0], starts[0] + 3 * q, 0).has_value());
  scheduled.run({.window = latency_20});
  CHECK(scheduled.dump() == dumps[0]);
}

TEST_CASE("cut refusals leave both wires unchanged, a valid cut then lands on both")
{
  stub_link  link{static_cast<std::int64_t>(latency_20), declared()};
  auto const us = static_cast<std::int64_t>(us_ps);

  auto const early = link.cut(5 * us, 10 * us, 6 * us);
  REQUIRE_FALSE(early.has_value());
  CHECK(early.error().reason == stm32::cut_refusal_reason::before_bound);
  CHECK(early.error().bound.ps == 6 * us);

  auto state = saved_state(*link.back);
  state.cuts.push_back({.start = {100 * us}, .end = {200 * us}});
  std::vector<std::byte> bytes;
  reflex::serde::bson::serializer{bytes}.dump(state);
  link.back->load(bytes);
  auto const clash = link.cut(150 * us, 250 * us, 0);
  REQUIRE_FALSE(clash.has_value());
  CHECK(clash.error().reason == stm32::cut_refusal_reason::overlaps);
  CHECK(clash.error().bound.ps == 100 * us);
  CHECK(clash.error().end.ps == 200 * us);
  CHECK(saved_state(*link.forth).cuts.empty());

  auto const none = stm32::restore_link(*link.forth, *link.back, core::sys_time{0});
  REQUIRE_FALSE(none.has_value());
  CHECK(none.error().reason == stm32::cut_refusal_reason::none_open);

  REQUIRE(link.cut(200 * us, 300 * us, 0) == core::sys_time{200 * us});
  CHECK(saved_state(*link.forth).cuts.size() == 1);
  CHECK(saved_state(*link.back).cuts.size() == 2);
  REQUIRE(link.cut(400 * us, std::nullopt, 400 * us) == core::sys_time{400 * us});
  auto const twice = link.cut(500 * us, std::nullopt, 500 * us);
  REQUIRE_FALSE(twice.has_value());
  CHECK(twice.error().reason == stm32::cut_refusal_reason::already_open);
  CHECK(twice.error().bound.ps == 400 * us);
  CHECK(twice.error().end.ps == open_end);
  auto const over_open = link.cut(600 * us, 700 * us, 500 * us);
  REQUIRE_FALSE(over_open.has_value());
  CHECK(over_open.error().reason == stm32::cut_refusal_reason::overlaps);
  REQUIRE(
      stm32::restore_link(*link.forth, *link.back, core::sys_time{450 * us})
      == core::sys_time{450 * us});
  CHECK(saved_state(*link.forth).cuts.back().end.ps == 450 * us);
  CHECK_FALSE(saved_state(*link.back).cuts.back().open);
  REQUIRE(link.cut(450 * us, 460 * us, 450 * us).has_value());

  REQUIRE(link.cut(900 * us, std::nullopt, 800 * us).has_value());
  REQUIRE(
      stm32::restore_link(*link.forth, *link.back, core::sys_time{850 * us})
      == core::sys_time{850 * us});
  CHECK(saved_state(*link.forth).cuts.size() == 3);
  CHECK(saved_state(*link.back).cuts.size() == 4);

  auto full = saved_state(*link.back);
  for(std::int64_t index = 0; static_cast<std::size_t>(full.cuts.size()) < stm32::max_waiting_cuts;
      ++index)
  {
    full.cuts.push_back({.start = {(10'000 + index) * us}, .end = {(10'000 + index) * us + 1}});
  }
  bytes.clear();
  reflex::serde::bson::serializer{bytes}.dump(full);
  link.back->load(bytes);
  auto const refused = link.cut(2'000 * us, 2'001 * us, 0);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().reason == stm32::cut_refusal_reason::full);
  CHECK(saved_state(*link.forth).cuts.size() == 3);
}

TEST_CASE("cuts registered and passed one after another are erased, at most two kept")
{
  constexpr std::int64_t q      = static_cast<std::int64_t>(latency_20);
  constexpr std::int64_t us     = static_cast<std::int64_t>(us_ps);
  constexpr std::int64_t length = 30 * us;
  constexpr std::int64_t period = 80 * us;
  constexpr std::int64_t count  = 1'000;
  stub_link              link{q, declared()};
  REQUIRE(static_cast<std::int64_t>(declared().frame_ps()) < length);
  REQUIRE(period - length > q + static_cast<std::int64_t>(declared().frame_ps()));
  std::vector<std::int64_t> starts;
  for(std::int64_t index = 0; index < count; ++index)
  {
    starts.push_back(index * period + 2 * us);
  }
  std::int64_t registered = 0;
  std::size_t  largest    = 0;
  link.after              = [&](std::uint64_t, std::int64_t barrier) {
    while(registered < count and registered * period < barrier + q)
    {
      auto const start = std::max(registered * period, barrier);
      REQUIRE(link.cut(start, registered * period + length, barrier).has_value());
      ++registered;
    }
    largest = std::max(largest, saved_state(*link.forth).cuts.size());
  };
  link.run(starts, q, count * period + 10 * q);
  CHECK(registered == count);
  CHECK(largest <= 2);
  CHECK(saved_state(*link.forth).cuts.empty());
  CHECK(link.forth_port.received.size() <= 1);
  CHECK(link.tap.size() == static_cast<std::size_t>(count));
  CHECK(std::ranges::all_of(link.tap, [](stm32::tap_frame const& frame) { return frame.cut; }));
}

TEST_CASE("injected frames are cut on their receiver times, stopped and running alike")
{
  constexpr std::int64_t us  = static_cast<std::int64_t>(us_ps);
  constexpr std::int64_t t0  = 2'600 * us;
  constexpr std::int64_t t1  = 2'700 * us;
  auto const             all = oracle_tap("link_tx", "link_rx", latency_20);
  auto const             l   = static_cast<std::int64_t>(latency_20);
  REQUIRE(all.back().arrival.ps < 2'500 * us);
  auto const inside   = t0 + l + 30 * us;
  auto const crossing = t0 + l - 10 * us;
  auto const tail     = t1 + l - 15 * us;
  auto const late     = t1 + l + 10 * us;

  auto running = paired("link_tx", "link_rx", latency_20);
  REQUIRE(cut_rig(running, t0, t1, 0).has_value());
  for(auto const at : {crossing, inside, tail, late})
  {
    REQUIRE(injected_byte(*running.wires[0], 0x77, at, std::nullopt).has_value());
  }
  running.run({.window = latency_20});
  std::vector<stm32::tap_frame> injected;
  std::ranges::copy_if(running.taps[0], std::back_inserter(injected), &stm32::tap_frame::injected);
  REQUIRE(injected.size() == 4);
  CHECK(injected[0].cut);
  CHECK(injected[0].lost);
  CHECK(injected[1].cut);
  CHECK_FALSE(injected[1].lost);
  CHECK(injected[2].cut);
  CHECK(injected[2].lost);
  CHECK_FALSE(injected[3].cut);
  auto const bytes = bytes_of(running.nodes[1]->f->log());
  CHECK(std::ranges::count(bytes, 0x77u) == 1);

  auto stopped = paired("link_tx", "link_rx", latency_20);
  stopped.run({.window = latency_20, .end = 2'550 * us_ps});
  REQUIRE(cut_rig(stopped, t0, t1, stopped_now(stopped)).has_value());
  auto const now = receiver_now(stopped);
  for(auto const at : {crossing, inside, tail, late})
  {
    REQUIRE(injected_byte(*stopped.wires[0], 0x77, at, now).has_value());
  }
  stopped.run({.window = latency_20, .first = 2'550 * us_ps / latency_20});
  CHECK(stopped.nodes[1]->f->log() == running.nodes[1]->f->log());
  CHECK(stopped.taps[0] == running.taps[0]);
}

TEST_CASE("a stopped cut over a stopped injection acts as when cut first, at two windows")
{
  constexpr std::int64_t          us     = static_cast<std::int64_t>(us_ps);
  constexpr std::int64_t          t0     = 2'600 * us;
  constexpr std::int64_t          t1     = 2'700 * us;
  constexpr std::uint64_t         stop   = 2'550 * us_ps;
  auto const                      l      = static_cast<std::int64_t>(latency_20);
  std::vector<std::int64_t> const starts = {
      t0 + l - 10 * us, t0 + l + 30 * us, t1 + l - 15 * us, t1 + l + 10 * us};

  auto running = paired("link_tx", "link_rx", latency_20);
  REQUIRE(cut_rig(running, t0, t1, 0).has_value());
  for(auto const at : starts)
  {
    REQUIRE(injected_byte(*running.wires[0], 0x77, at, std::nullopt).has_value());
  }
  running.run({.window = latency_20});
  CHECK(std::ranges::count(bytes_of(running.nodes[1]->f->log()), 0x77u) == 1);
  auto const inside =
      std::ranges::find(running.taps[0], core::sys_time{starts[1]}, &stm32::tap_frame::start);
  REQUIRE(inside != running.taps[0].end());
  CHECK(inside->injected);
  CHECK(inside->cut);
  CHECK_FALSE(inside->lost);

  for(auto const window : {latency_20, latency_20 / 4})
  {
    for(auto const cut_first : {false, true})
    {
      CAPTURE(window);
      CAPTURE(cut_first);
      auto stopped = paired("link_tx", "link_rx", latency_20);
      stopped.run({.window = window, .end = stop});
      auto const now = receiver_now(stopped);
      if(cut_first)
      {
        REQUIRE(cut_rig(stopped, t0, t1, stopped_now(stopped)).has_value());
      }
      for(auto const at : starts)
      {
        REQUIRE(injected_byte(*stopped.wires[0], 0x77, at, now).has_value());
      }
      if(not cut_first)
      {
        REQUIRE(cut_rig(stopped, t0, t1, stopped_now(stopped)).has_value());
      }
      CHECK(saved_state(*stopped.wires[0]).injections.size() == starts.size());
      std::vector<std::byte> bytes;
      stopped.wires[0]->save(bytes);
      stopped.wires[0]->load(bytes);
      stopped.run({.window = window, .first = (stop - 1) / window});
      CHECK(stopped.nodes[1]->f->log() == running.nodes[1]->f->log());
      CHECK(stopped.taps[0] == running.taps[0]);
    }
  }
}

TEST_CASE("a stopped injection reaches the port by its start, queued or delivered at once")
{
  constexpr std::int64_t us       = static_cast<std::int64_t>(us_ps);
  constexpr std::int64_t l        = 20 * us;
  auto const             frame_ps = static_cast<std::int64_t>(declared().frame_ps());
  for(auto const stop : {std::int64_t{0}, std::int64_t{3}})
  {
    CAPTURE(stop);
    stub_link                                          link{l, declared()};
    std::int64_t                                       receiver = stop * l;
    std::vector<std::pair<std::int64_t, std::int64_t>> seen;
    link.forth_port.on_receive = [&](periph::line_frame const& frame) {
      seen.emplace_back(receiver, static_cast<std::int64_t>(frame.start.ps));
    };
    link.run({}, l, stop * l);
    std::vector<std::uint16_t> const data{0x11, 0x22};
    REQUIRE(link.forth
                ->inject(
                    data, core::sys_time{receiver + 5 * us},
                    stm32::stopped_receiver{.now = core::sys_time{receiver}, .map = {}})
                .has_value());
    for(auto k = static_cast<std::uint64_t>(stop); k < static_cast<std::uint64_t>(stop) + 3; ++k)
    {
      receiver = (static_cast<std::int64_t>(k) + 1) * l;
      link.window(k, l, {});
      link.drain(k, l);
    }
    REQUIRE(seen.size() == 2);
    CHECK(seen[0].second == stop * l + 5 * us);
    CHECK(seen[1].second == stop * l + 5 * us + frame_ps);
    for(auto const& [at, start] : seen)
    {
      CHECK(at <= start);
    }
  }
}

TEST_CASE("a stopped injection crossing t0 plus L of a passed cut is truncated at its delivery")
{
  constexpr std::int64_t  us      = static_cast<std::int64_t>(us_ps);
  constexpr std::uint64_t latency = 60 * us_ps;
  constexpr std::int64_t  t0      = 2'530 * us;
  constexpr std::int64_t  t1      = t0 + 100 * us;
  constexpr std::uint64_t S       = 2'582 * us_ps;
  auto const              ends    = static_cast<std::int64_t>(declared().frame_ps());
  for(auto const window : {latency, latency / 2})
  {
    CAPTURE(window);
    auto stopped = paired("link_tx", "link_rx", latency);
    REQUIRE(cut_rig(stopped, t0, t1, 0).has_value());
    stopped.run({.window = window, .end = S});
    auto const now  = receiver_now(stopped);
    auto const next = static_cast<std::int64_t>(S - S % window + window);
    REQUIRE(now.now.ps < t0 + static_cast<std::int64_t>(latency));
    REQUIRE(now.now.ps + ends > t0 + static_cast<std::int64_t>(latency));
    REQUIRE(now.now.ps + ends < next);
    REQUIRE(injected_byte(*stopped.wires[0], 0x77, now.now.ps, now).has_value());
    stopped.run({.window = window, .first = (S - 1) / window});

    auto running = paired("link_tx", "link_rx", latency);
    REQUIRE(cut_rig(running, t0, t1, 0).has_value());
    REQUIRE(injected_byte(*running.wires[0], 0x77, now.now.ps, std::nullopt).has_value());
    running.run({.window = window});

    CHECK(std::ranges::count(bytes_of(running.nodes[1]->f->log()), 0x77u) == 0);
    auto const injected = std::ranges::find_if(running.taps[0], &stm32::tap_frame::injected);
    REQUIRE(injected != running.taps[0].end());
    CHECK(injected->cut);
    CHECK(injected->lost);
    CHECK(stopped.nodes[1]->f->log() == running.nodes[1]->f->log());
    CHECK(stopped.taps[0] == running.taps[0]);
  }
}

TEST_CASE("an open cut restored at its own start is withdrawn, the frame across it arrives")
{
  auto const all = oracle_tap("link_tx", "link_rx", latency_20);
  auto const T   = static_cast<std::uint64_t>(all[8].start.ps) + 5 * us_ps;
  REQUIRE(static_cast<std::int64_t>(T) + static_cast<std::int64_t>(us_ps) < all[8].end.ps);
  for(auto const window : {latency_20, latency_20 / 4})
  {
    CAPTURE(window);
    auto plain = paired("link_tx", "link_rx", latency_20);
    plain.run({.window = window, .end = T});
    plain.run({.window = window, .first = (T - 1) / window});

    auto r = paired("link_tx", "link_rx", latency_20);
    r.run({.window = window, .end = T});
    auto const t = stopped_now(r);
    REQUIRE(t < all[8].end.ps);
    REQUIRE(cut_rig(r, t, std::nullopt, t) == core::sys_time{t});
    REQUIRE(stm32::restore_link(*r.wires[0], *r.wires[1], core::sys_time{t}) == core::sys_time{t});
    CHECK(saved_state(*r.wires[0]).cuts.empty());
    CHECK(saved_state(*r.wires[1]).cuts.empty());
    r.run({.window = window, .first = (T - 1) / window});

    CHECK(bytes_of(r.nodes[1]->f->log()) == bytes_of(plain.nodes[1]->f->log()));
    CHECK(r.dump() == plain.dump());
    CHECK(std::ranges::none_of(r.taps[0], &stm32::tap_frame::cut));
    CHECK(std::ranges::none_of(r.taps[1], &stm32::tap_frame::cut));
  }
}

TEST_CASE("a frame never handed to the port is never truncated: an overlap clash")
{
  constexpr std::int64_t us = static_cast<std::int64_t>(us_ps);
  constexpr std::int64_t l  = 50 * us;
  constexpr std::int64_t t0 = 33 * us;
  stub_link              link{l, declared()};
  auto const             frame_ps = static_cast<std::int64_t>(declared().frame_ps());
  REQUIRE(10 * us + frame_ps < t0);
  REQUIRE(t0 < 15 * us + frame_ps);
  REQUIRE(link.cut(t0, t0 + 100 * us, 0).has_value());
  std::vector<std::int64_t> const starts{10 * us, 15 * us};
  link.window(0, l, starts);
  auto const fault = link.forth->drain(0, core::sys_time{l}, core::sys_time{2 * l}, {});
  REQUIRE(fault.has_value());
  CHECK(fault->at.ps == 15 * us);
  CHECK(link.forth_port.received.size() == 1);
  CHECK(link.forth_port.cancels.empty());
}

TEST_CASE("drains stay cheap with the most cuts waiting ahead")
{
  constexpr std::int64_t us = static_cast<std::int64_t>(us_ps);
  constexpr std::int64_t l  = 20 * us;
  stub_link              link{l, declared()};
  auto const             first = 1'000'000 * us;
  bool                   all   = true;
  for(std::size_t index = 0; index < stm32::max_waiting_cuts; ++index)
  {
    auto const start = first + static_cast<std::int64_t>(index) * 10 * us;
    all              = all and link.cut(start, start + 5 * us, 0).has_value();
  }
  REQUIRE(all);
  auto const last    = first + static_cast<std::int64_t>(stm32::max_waiting_cuts) * 10 * us;
  auto const refused = link.cut(last, last + 5 * us, 0);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().reason == stm32::cut_refusal_reason::full);
  auto const    begin = std::chrono::steady_clock::now();
  std::uint64_t k     = 0;
  for(; k < 100 and std::chrono::steady_clock::now() - begin < std::chrono::seconds{1}; ++k)
  {
    link.window(k, l, {});
    link.drain(k, l);
  }
  CHECK(k == 100);
  CHECK(saved_state(*link.forth).cuts.size() == stm32::max_waiting_cuts);
}

TEST_CASE("a dropped frame inside a cut shows dropped only")
{
  auto const all = oracle_tap("link_tx", "link_rx", latency_20);
  auto const t0  = all[10].start.ps + static_cast<std::int64_t>(5 * us_ps);
  auto const t1  = all[14].start.ps + static_cast<std::int64_t>(5 * us_ps);
  auto       r   = paired("link_tx", "link_rx", latency_20);
  REQUIRE(r.wires[0]->drop(12, 1));
  REQUIRE(cut_rig(r, t0, t1, 0).has_value());
  r.run({.window = latency_20});
  CHECK(r.taps[0][12].dropped);
  CHECK_FALSE(r.taps[0][12].cut);
  CHECK_FALSE(r.taps[0][12].lost);
  CHECK(r.taps[0][11].cut);
  CHECK(r.taps[0][13].cut);
}

TEST_CASE("a cut from a member's hook mid-window acts as the same cut registered ahead")
{
  auto const all = oracle_tap("link_tx", "link_rx", latency_20);
  auto const q   = static_cast<std::int64_t>(latency_20);
  auto const us  = static_cast<std::int64_t>(us_ps);
  auto const at0 = (all[5].start.ps / q + 1) * q;
  REQUIRE(at0 < all[5].end.ps);
  auto const at1 = all[9].start.ps + 7 * us;
  auto const at2 = all[15].start.ps + 3 * us;

  for(std::size_t member = 0; member < 2; ++member)
  {
    std::optional<std::vector<std::int64_t>> first;
    std::optional<std::string>               expected;
    for(auto plan : variants_of(latency_20))
    {
      CAPTURE(member);
      CAPTURE(plan.window);
      CAPTURE(plan.threaded);
      auto                      r = paired("link_rx", "link_tx", latency_20);
      std::vector<cut_result>   results(4);
      std::vector<std::int64_t> times(3);
      plan.calls = {
          {.member = member,
           .at     = static_cast<std::uint64_t>(at0),
           .call =
               [&](std::uint64_t now) {
                 times[0]   = static_cast<std::int64_t>(now);
                 results[0] = cut_rig(r, times[0], std::nullopt, times[0]);
               }},
          {.member = member,
           .at     = static_cast<std::uint64_t>(at1),
           .call =
               [&](std::uint64_t now) {
                 times[1] = static_cast<std::int64_t>(now);
                 results[1] =
                     stm32::restore_link(*r.wires[0], *r.wires[1], core::sys_time{times[1]});
               }},
          {.member = member,
           .at     = static_cast<std::uint64_t>(at2),
           .call =
               [&](std::uint64_t now) {
                 times[2]   = static_cast<std::int64_t>(now);
                 results[2] = cut_rig(r, times[2] - 1, times[2] + 30 * us, times[2]);
                 results[3] = cut_rig(r, times[2], times[2] + 30 * us, times[2]);
               }},
      };
      r.run(plan);
      CHECK(results[0] == core::sys_time{times[0]});
      CHECK(results[1] == core::sys_time{times[1]});
      REQUIRE_FALSE(results[2].has_value());
      CHECK(results[2].error().reason == stm32::cut_refusal_reason::before_bound);
      CHECK(results[2].error().bound.ps == times[2]);
      CHECK(results[3] == core::sys_time{times[2]});
      CHECK(r.taps[1][5].cut);
      CHECK(r.taps[1][5].lost);
      CHECK(r.taps[1][9].lost);
      CHECK(r.taps[1][15].lost);
      if(not first)
      {
        first          = times;
        auto scheduled = paired("link_rx", "link_tx", latency_20);
        REQUIRE(cut_rig(scheduled, times[0], times[1], 0).has_value());
        REQUIRE(cut_rig(scheduled, times[2], times[2] + 30 * us, 0).has_value());
        scheduled.run({.window = latency_20});
        expected = scheduled.dump();
      }
      CHECK(times == *first);
      CHECK(r.dump() == *expected);
    }
  }
}

TEST_CASE("drains between a cut's checks and its commit wait, so the cut acts whole")
{
  constexpr std::int64_t          q  = static_cast<std::int64_t>(latency_20);
  constexpr std::int64_t          us = static_cast<std::int64_t>(us_ps);
  stub_link                       link{q, declared()};
  auto const                      frame = 2 * q + 1;
  std::vector<std::int64_t> const starts{frame};
  link.window(0, q, starts);
  link.drain(0, q);
  link.window(1, q, starts);
  link.window(2, q, starts);
  std::atomic<bool> done{false};
  std::jthread      drains;
  link.shared->between_checks_and_commit = [&] {
    drains = std::jthread{[&] {
      link.forth->drain(1, core::sys_time{2 * q}, core::sys_time{3 * q}, {});
      link.forth->drain(2, core::sys_time{3 * q}, core::sys_time{4 * q}, {});
      done.store(true);
    }};
    while(not done.load() and link.shared->contenders.load() == 0)
    {
      std::this_thread::yield();
    }
  };
  auto const result                      = link.cut(2 * q, 2 * q + 60 * us, std::nullopt);
  link.shared->between_checks_and_commit = {};
  drains                                 = {};
  REQUIRE(result == core::sys_time{2 * q});
  CHECK_FALSE(received_at(link, frame));
  CHECK(saved_state(*link.forth).delivered.front().suppressed);
}

TEST_CASE("a closed cut stays until the frames crossing it are tapped")
{
  constexpr std::int64_t us = static_cast<std::int64_t>(us_ps);
  constexpr std::int64_t l  = 10 * us;
  constexpr std::int64_t t0 = 100 * us;
  for(auto const start : {t0 - 10 * us, t0 + 15 * us})
  {
    for(auto const q : {l, l / 2})
    {
      CAPTURE(start);
      CAPTURE(q);
      stub_link link{l, {.baud = 115'200}};
      REQUIRE(link.shape.frame_ps() > 80 * us_ps);
      REQUIRE(link.cut(t0, t0 + 20 * us, 0).has_value());
      std::vector<std::int64_t> const starts{start};
      link.run(starts, q, 400 * us);
      REQUIRE(link.tap.size() == 1);
      CHECK(link.tap[0].cut);
      CHECK(link.tap[0].lost);
      CHECK(saved_state(*link.forth).cuts.empty());
      if(start < t0)
      {
        CHECK(link.forth_port.cancels == std::vector<std::int64_t>{t0 + l});
        CHECK(received_at(link, start));
      }
      else
      {
        CHECK(link.forth_port.received.empty());
      }
    }
  }
}

TEST_CASE("a cut registered from a port call during a drain is kept and acts at the next one")
{
  constexpr std::int64_t    q  = static_cast<std::int64_t>(latency_20);
  constexpr std::int64_t    us = static_cast<std::int64_t>(us_ps);
  stub_link                 link{q, declared()};
  std::optional<cut_result> registered;
  link.forth_port.on_receive = [&](periph::line_frame const&) {
    if(not registered)
    {
      registered = link.cut(2 * q, 5 * q, 2 * q);
    }
  };
  std::vector<std::int64_t> const starts{us, 2 * q + 10 * us};
  link.window(0, q, starts);
  link.drain(0, q);
  REQUIRE(registered.has_value());
  link.window(1, q, starts);
  link.drain(1, q);
  CHECK(*registered == core::sys_time{2 * q});
  CHECK(saved_state(*link.forth).cuts.size() == 1);
  CHECK(saved_state(*link.back).cuts.size() == 1);
  link.window(2, q, starts);
  link.drain(2, q);
  link.run({}, q, 10 * q);
  CHECK(received_at(link, starts[0]));
  CHECK_FALSE(received_at(link, starts[1]));
  REQUIRE(link.tap.size() == 2);
  CHECK_FALSE(link.tap[0].cut);
  CHECK(link.tap[1].cut);
}

TEST_CASE("cuts registered from one thread while both wires drain on two others")
{
  constexpr std::int64_t      q      = static_cast<std::int64_t>(latency_20);
  constexpr std::uint64_t     rounds = 1'000;
  stub_link                   link{q, declared()};
  auto const                  far = static_cast<std::int64_t>(rounds + 10) * q;
  std::jthread                registrant{[&] {
    for(std::uint64_t round = 0; round < rounds; ++round)
    {
      auto const now = far + 2 * static_cast<std::int64_t>(round);
      CHECK(link.cut(now, std::nullopt, now).has_value());
      CHECK(
          stm32::restore_link(*link.forth, *link.back, core::sys_time{now + 1})
          == core::sys_time{now + 1});
      std::this_thread::sleep_for(std::chrono::microseconds{10});
    }
  }};
  std::array<std::jthread, 2> drains{
      std::jthread{[&] {
        for(std::uint64_t k = 0; k < rounds; ++k)
        {
          auto const barrier = static_cast<std::int64_t>(k + 1) * q;
          link.forth->drain(k, core::sys_time{barrier}, core::sys_time{barrier + q}, {});
          std::this_thread::sleep_for(std::chrono::microseconds{10});
        }
      }},
      std::jthread{[&] {
        for(std::uint64_t k = 0; k < rounds; ++k)
        {
          auto const barrier = static_cast<std::int64_t>(k + 1) * q;
          link.back->drain(k, core::sys_time{barrier}, core::sys_time{barrier + q}, {});
          std::this_thread::sleep_for(std::chrono::microseconds{10});
        }
      }},
  };
  registrant = {};
  drains     = {};
  CHECK(saved_state(*link.forth).cuts.size() == rounds);
  CHECK(saved_state(*link.forth).cuts == saved_state(*link.back).cuts);
}

TEST_CASE("cuts never change errors: overlaps and shape faults are those of the run without")
{
  auto const all      = oracle_tap("link_tx", "link_rx", latency_20);
  auto const l        = static_cast<std::int64_t>(latency_20);
  auto const us       = static_cast<std::int64_t>(us_ps);
  auto const frame_ps = all[10].end.ps - all[10].start.ps;

  SUBCASE("an injection over peer frames, all inside the cut")
  {
    auto const at  = all[10].start.ps + l + frame_ps / 2;
    auto const run = [&](run_plan const& plan, bool with_cut) {
      auto r = paired("link_tx", "link_rx", latency_20);
      if(with_cut)
      {
        REQUIRE(cut_rig(r, all[10].start.ps, all[12].start.ps, 0).has_value());
      }
      REQUIRE(injected_byte(*r.wires[0], 0x5a, at, std::nullopt).has_value());
      r.run(plan);
      return earliest_fault(r);
    };
    auto const expected = run({.window = latency_20}, false);
    REQUIRE(expected.has_value());
    for(auto const& plan : variants_of(latency_20))
    {
      CAPTURE(plan.window);
      CHECK(run(plan, true) == expected);
    }
  }

  SUBCASE("a peer frame of another shape inside the cut under strict")
  {
    auto const shaped = oracle_tap("link_tx_switch", "link_rx", latency_20);
    REQUIRE(shaped.size() == 8);
    for(auto const window : {latency_20, latency_20 / 3})
    {
      rig r;
      r.strict = true;
      r.add("link_tx_switch");
      r.add("link_rx");
      r.link(0, 1, latency_20);
      r.link(1, 0, latency_20);
      auto const shared = std::make_shared<stm32::link_registration>();
      r.wires[0]->share_registration(shared);
      r.wires[1]->share_registration(shared);
      REQUIRE(cut_rig(r, shaped[4].start.ps, shaped[4].end.ps, 0).has_value());
      r.run({.window = window});
      CHECK(r.taps[0][4].cut);
      CHECK(r.nodes[1]->warnings.size() == 1);
      CHECK(
          r.nodes[1]->faults
          == std::vector<fault_seen>{
              {.at = shaped[4].start.ps, .member = 1}
      });
    }
  }

  auto const t0   = all[19].start.ps + 5 * us;
  auto const t1   = t0 + 30 * us;
  auto const tail = t0 + l + us;
  REQUIRE(tail + frame_ps < all[20].start.ps + l);

  SUBCASE("an injection over the truncated tail of a frame crossing t0, running")
  {
    auto const run = [&](run_plan const& plan, bool with_cut) {
      auto r = paired("link_tx", "link_rx", latency_20);
      if(with_cut)
      {
        REQUIRE(cut_rig(r, t0, t1, 0).has_value());
      }
      REQUIRE(injected_byte(*r.wires[0], 0x5a, tail, std::nullopt).has_value());
      r.run(plan);
      return earliest_fault(r);
    };
    auto const expected = run({.window = latency_20}, false);
    CHECK(expected == fault_seen{.at = tail, .member = 1});
    for(auto const& plan : variants_of(latency_20))
    {
      CAPTURE(plan.window);
      CHECK(run(plan, true) == expected);
    }
  }

  SUBCASE("an injection over the truncated tail of a frame crossing t0, stopped")
  {
    for(auto const window : {latency_20, latency_20 / 3})
    {
      for(auto const with_cut : {false, true})
      {
        CAPTURE(window);
        CAPTURE(with_cut);
        auto r = paired("link_tx", "link_rx", latency_20);
        if(with_cut)
        {
          REQUIRE(cut_rig(r, t0, t1, 0).has_value());
        }
        r.run({.window = window, .end = static_cast<std::uint64_t>(t0 + l)});
        auto const now = receiver_now(r);
        REQUIRE(now.now.ps <= tail);
        auto const state = saved_state(*r.wires[0]);
        CHECK(std::ranges::any_of(state.delivered, [&](stm32::detail::line_span const& span) {
          return span.post == all[19].start and span.truncated == with_cut;
        }));
        auto const refused = injected_byte(*r.wires[0], 0x5a, tail, now);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().reason == stm32::inject_refusal_reason::overlaps);
        CHECK(refused.error().bound.ps == all[19].start.ps + l);
      }
    }
  }

  SUBCASE("a stopped injection over a suppressed peer frame")
  {
    for(auto const window : {latency_20, latency_20 / 3})
    {
      CAPTURE(window);
      auto r = paired("link_tx", "link_rx", latency_20);
      REQUIRE(cut_rig(r, all[19].start.ps, all[19].start.ps + 30 * us, 0).has_value());
      r.run({.window = window, .end = static_cast<std::uint64_t>(all[19].start.ps + l)});
      auto const now   = receiver_now(r);
      auto const state = saved_state(*r.wires[0]);
      REQUIRE(std::ranges::any_of(state.delivered, [&](stm32::detail::line_span const& span) {
        return span.post == all[19].start and span.suppressed;
      }));
      auto const refused = injected_byte(*r.wires[0], 0x5a, all[19].start.ps + l + us, now);
      REQUIRE_FALSE(refused.has_value());
      CHECK(refused.error().reason == stm32::inject_refusal_reason::overlaps);
      CHECK(refused.error().bound.ps == all[19].start.ps + l);
    }
  }
}

TEST_CASE("the receiver gets the earlier of an abort and a cut of the same frame")
{
  constexpr std::int64_t us    = static_cast<std::int64_t>(us_ps);
  constexpr std::int64_t l     = 50 * us;
  constexpr std::int64_t start = 10 * us;
  constexpr std::int64_t abort = 25 * us;
  for(auto const t0 : {15 * us, 28 * us})
  {
    CAPTURE(t0);
    stub_link link{l, declared()};
    REQUIRE(start + static_cast<std::int64_t>(declared().frame_ps()) > t0);
    REQUIRE(link.cut(t0, t0 + 100 * us, 0).has_value());
    link.forth->enter_window(0, {});
    link.back->enter_window(0, {});
    link.forth->started(link.frame(start));
    link.forth->aborted(vtime{static_cast<std::uint64_t>(abort)});
    link.drain(0, l);
    link.run({}, l, 10 * l);
    REQUIRE_FALSE(link.forth_port.cancels.empty());
    CHECK(link.forth_port.cancels.front() == std::min(t0, abort) + l);
    REQUIRE(link.tap.size() == 1);
    CHECK(link.tap[0].lost);
    CHECK(link.tap[0].end.ps == abort);
    CHECK(link.tap[0].cut == (t0 < abort));
  }
}

namespace
{
auto received_starts(stub_port const& port) -> std::vector<std::int64_t>
{
  std::vector<std::int64_t> out;
  for(auto const& frame : port.received)
  {
    out.push_back(static_cast<std::int64_t>(frame.start.ps));
  }
  return out;
}

void run_windows(stub_link& link, std::uint64_t from, std::uint64_t to, std::int64_t q)
{
  for(auto k = from; k < to; ++k)
  {
    link.window(k, q, {});
    link.drain(k, q);
  }
}

auto pause_drain(stub_link& link, std::uint64_t k, std::int64_t at)
    -> std::optional<core::link_fault>
{
  auto fault  = link.forth->drain(k, core::sys_time{at}, core::sys_time{at}, {});
  std::ignore = link.back->drain(k, core::sys_time{at}, core::sys_time{at}, {});
  return fault;
}
}

TEST_CASE("an overshoot post past the barrier is carried, so a cut from the barrier keeps it off")
{
  constexpr std::int64_t us = static_cast<std::int64_t>(us_ps);
  constexpr std::int64_t l  = 50 * us;
  for(auto const q : {l, l / 2})
  {
    CAPTURE(q);
    stub_link link{l, declared()};
    link.window(0, q, {});
    link.forth->started(link.frame(q + 1));
    link.drain(0, q);
    CHECK(link.forth_port.received.empty());
    CHECK(saved_state(*link.forth).carried.size() == 1);
    REQUIRE(link.cut(q, std::nullopt, q) == core::sys_time{q});
    run_windows(link, 1, 10, q);
    CHECK(link.forth_port.received.empty());
    CHECK(saved_state(*link.forth).carried.empty());
    REQUIRE(link.tap.size() == 1);
    CHECK(link.tap[0].cut);
    CHECK_FALSE(link.tap[0].lost);
  }
}

TEST_CASE("an overshoot post carried inside an open cut restored before its start arrives")
{
  constexpr std::int64_t us = static_cast<std::int64_t>(us_ps);
  constexpr std::int64_t l  = 50 * us;
  for(auto const q : {l, l / 2})
  {
    CAPTURE(q);
    auto const start = q + 5 * us;
    auto const run   = [&](bool with_cut) {
      auto link = std::make_unique<stub_link>(l, declared());
      if(with_cut)
      {
        REQUIRE(link->cut(q / 2, std::nullopt, 0).has_value());
      }
      link->window(0, q, {});
      link->forth->started(link->frame(start));
      link->drain(0, q);
      CHECK(link->forth_port.received.empty());
      if(with_cut)
      {
        CHECK(saved_state(*link->forth).carried.size() == 1);
        REQUIRE(
            stm32::restore_link(*link->forth, *link->back, core::sys_time{q + us})
            == core::sys_time{q + us});
      }
      run_windows(*link, 1, 10, q);
      return link;
    };
    auto const plain = run(false);
    auto const cut   = run(true);
    CHECK(received_starts(cut->forth_port) == std::vector<std::int64_t>{start + l});
    CHECK(received_starts(cut->forth_port) == received_starts(plain->forth_port));
    CHECK(cut->forth_port.cancels == plain->forth_port.cancels);
    REQUIRE(cut->tap.size() == 1);
    CHECK_FALSE(cut->tap[0].cut);
    CHECK_FALSE(cut->tap[0].lost);
  }
}

TEST_CASE("a pause drain carries posts past its point, so a running cut after the resume holds")
{
  constexpr std::int64_t us = static_cast<std::int64_t>(us_ps);
  constexpr std::int64_t l  = 50 * us;
  for(auto const q : {l, l / 2})
  {
    CAPTURE(q);
    stub_link  link{l, declared()};
    auto const p     = 2 * q;
    auto const start = p + 3 * us;
    run_windows(link, 0, 1, q);
    link.window(1, q, {});
    link.forth->started(link.frame(start));
    CHECK_FALSE(pause_drain(link, 1, p - 10 * us).has_value());
    CHECK(link.forth_port.received.empty());
    CHECK(saved_state(*link.forth).carried.size() == 1);
    REQUIRE(link.cut(p, p + 50 * us, std::nullopt) == core::sys_time{p});
    link.drain(1, q);
    run_windows(link, 2, 12, q);
    CHECK(link.forth_port.received.empty());
    REQUIRE(link.tap.size() == 1);
    CHECK(link.tap[0].cut);
  }
}

TEST_CASE("a pause drain reports an overlap of a post it carries, dated at the post's start")
{
  constexpr std::int64_t us = static_cast<std::int64_t>(us_ps);
  constexpr std::int64_t l  = 10 * us;
  constexpr std::int64_t q  = 20 * us;
  constexpr std::int64_t c  = 41 * us;
  stub_link              link{l, declared()};
  REQUIRE(c + l < 35 * us + static_cast<std::int64_t>(declared().frame_ps()));
  REQUIRE(injected_byte(*link.forth, 0x5a, 35 * us, std::nullopt).has_value());
  link.window(0, q, {});
  CHECK_FALSE(link.forth->drain(0, core::sys_time{q}, core::sys_time{2 * q}, {}).has_value());
  REQUIRE(link.forth_port.received.size() == 1);
  link.window(1, q, {});
  link.forth->started(link.frame(c));
  auto const fault = pause_drain(link, 1, c);
  REQUIRE(fault.has_value());
  CHECK(fault->at.ps == c);
  CHECK(fault->member == 0);
  CHECK(link.forth_port.received.size() == 1);
}

TEST_CASE("an abort after the barrier edits the record at once and cancels at its delivery")
{
  constexpr std::int64_t us       = static_cast<std::int64_t>(us_ps);
  constexpr std::int64_t l        = 50 * us;
  auto const             frame_ps = static_cast<std::int64_t>(declared().frame_ps());
  for(auto const q : {l, l / 2})
  {
    CAPTURE(q);
    SUBCASE("running: the frame crosses the barrier, the abort and the next frame overshoot")
    {
      stub_link  link{l, declared()};
      auto const f = q - 10 * us;
      auto const a = q + 2 * us;
      auto const g = q + 3 * us;
      REQUIRE(g < f + frame_ps);
      link.window(0, q, {});
      link.forth->started(link.frame(f));
      link.forth->aborted(vtime{static_cast<std::uint64_t>(a)});
      link.forth->started(link.frame(g));
      auto fault = link.forth->drain(0, core::sys_time{q}, core::sys_time{2 * q}, {});
      CHECK_FALSE(fault.has_value());
      CHECK(received_starts(link.forth_port) == std::vector<std::int64_t>{f + l});
      CHECK(link.forth_port.cancels.empty());
      CHECK(saved_state(*link.forth).carried.size() == 2);
      link.window(1, q, {});
      fault = link.forth->drain(1, core::sys_time{2 * q}, core::sys_time{3 * q}, {});
      CHECK_FALSE(fault.has_value());
      CHECK(received_starts(link.forth_port) == std::vector<std::int64_t>{f + l, g + l});
      CHECK(link.forth_port.cancels == std::vector<std::int64_t>{a + l});
    }
    SUBCASE("pause: the frame crosses the pause point, the abort and the next frame after it")
    {
      stub_link  link{l, declared()};
      auto const c = q - 5 * us;
      auto const f = c - 10 * us;
      auto const a = c + 2 * us;
      auto const g = c + 3 * us;
      link.window(0, q, {});
      link.forth->started(link.frame(f));
      link.forth->aborted(vtime{static_cast<std::uint64_t>(a)});
      link.forth->started(link.frame(g));
      CHECK_FALSE(pause_drain(link, 0, c).has_value());
      CHECK(received_starts(link.forth_port) == std::vector<std::int64_t>{f + l});
      CHECK(saved_state(*link.forth).carried.size() == 2);
      CHECK_FALSE(link.forth->drain(0, core::sys_time{q}, core::sys_time{2 * q}, {}).has_value());
      CHECK(received_starts(link.forth_port) == std::vector<std::int64_t>{f + l, g + l});
      CHECK(link.forth_port.cancels == std::vector<std::int64_t>{a + l});
    }
    SUBCASE("pause: a stopped injection after the abort fits in the aborted frame's rest")
    {
      stub_link  link{l, declared()};
      auto const c = q - 5 * us;
      auto const f = c - 10 * us;
      auto const a = c + 2 * us;
      REQUIRE(a + l + us < f + l + frame_ps);
      link.window(0, q, {});
      link.forth->started(link.frame(f));
      link.forth->aborted(vtime{static_cast<std::uint64_t>(a)});
      CHECK_FALSE(pause_drain(link, 0, c).has_value());
      CHECK(saved_state(*link.forth).carried.size() == 1);
      CHECK(injected_byte(
                *link.forth, 0x5a, a + l + us,
                stm32::stopped_receiver{.now = core::sys_time{c}, .map = {}})
                .has_value());
    }
  }
}

TEST_CASE("a wire saved with an open cut and posts carried by a pause loads with the same result")
{
  constexpr std::int64_t us  = static_cast<std::int64_t>(us_ps);
  constexpr std::int64_t l   = 50 * us;
  constexpr std::int64_t q   = l;
  constexpr std::int64_t c   = q + us;
  auto const             run = [&](bool reload) {
    auto link = std::make_unique<stub_link>(l, declared());
    run_windows(*link, 0, 1, q);
    link->window(1, q, {});
    link->forth->started(link->frame(q + 5 * us));
    link->forth->started(link->frame(q + 30 * us));
    CHECK_FALSE(pause_drain(*link, 1, c).has_value());
    REQUIRE(link->cut(q + 20 * us, std::nullopt, c) == core::sys_time{q + 20 * us});
    if(reload)
    {
      auto const state = saved_state(*link->forth);
      CHECK(state.carried.size() == 2);
      CHECK(state.cuts.size() == 1);
      for(auto* wire : {&link->forth, &link->back})
      {
        std::vector<std::byte> bytes;
        (*wire)->save(bytes);
        auto const forth = wire == &link->forth;
        auto       fresh = std::make_shared<stm32::uart_wire>(
            stm32::wire_ends{.sender = forth ? 0U : 1U, .receiver = forth ? 1U : 0U},
            stm32::wire_lanes{.peer = 1, .injected = 2}, core::sys_time{l}, declared(),
            stm32::wire_receiver{
                .port   = forth ? &link->forth_port : &link->back_port,
                .unit   = "USART1",
                .log    = {},
                .strict = false},
            nullptr);
        fresh->share_registration(link->shared);
        fresh->load(bytes);
        *wire = fresh;
      }
    }
    link->drain(1, q);
    run_windows(*link, 2, 4, q);
    REQUIRE(stm32::restore_link(*link->forth, *link->back, core::sys_time{5 * q}).has_value());
    run_windows(*link, 4, 10, q);
    return link;
  };
  auto const reference = run(false);
  auto const loaded    = run(true);
  CHECK(received_starts(reference->forth_port) == std::vector<std::int64_t>{q + 5 * us + l});
  CHECK(reference->forth_port.cancels == std::vector<std::int64_t>{q + 20 * us + l});
  CHECK(received_starts(loaded->forth_port) == received_starts(reference->forth_port));
  CHECK(loaded->forth_port.cancels == reference->forth_port.cancels);
  CHECK(loaded->tap == reference->tap);
}

TEST_CASE("a frame a recorded receiver reset drops is final at its drain, never carried")
{
  constexpr std::int64_t us = static_cast<std::int64_t>(us_ps);
  constexpr std::int64_t l  = 50 * us;
  for(auto const q : {l, l / 2})
  {
    CAPTURE(q);
    auto const quiet_after = [&](std::int64_t reset_from_start, bool aborted) {
      stub_link  link{l, declared()};
      auto const s = q + 1;
      link.window(0, q, {});
      link.forth->started(link.frame(s));
      if(aborted)
      {
        link.forth->aborted(vtime{static_cast<std::uint64_t>(s + 5 * us)});
      }
      link.forth->receiver_reset(core::sys_time{s + reset_from_start});
      std::ignore      = link.forth->drain(0, core::sys_time{q}, core::sys_time{2 * q}, {});
      auto const quiet = link.forth->quiet();
      CHECK(saved_state(*link.forth).carried.size() == (quiet ? 0U : (aborted ? 2U : 1U)));
      run_windows(link, 1, 10, q);
      CHECK(link.forth_port.received.size() == (quiet ? 0U : 1U));
      CHECK(link.forth_port.cancels.size() == (quiet or not aborted ? 0U : 1U));
      return quiet;
    };
    CHECK(quiet_after(2 * us, false));
    CHECK(quiet_after(2 * us, true));
    CHECK_FALSE(quiet_after(-us, false));
    CHECK_FALSE(quiet_after(-us, true));
  }
}

TEST_CASE("a carried frame a receiver reset recorded later drops is final at the next pause")
{
  constexpr std::int64_t us = static_cast<std::int64_t>(us_ps);
  constexpr std::int64_t l  = 50 * us;
  for(auto const q : {l, l / 2})
  {
    CAPTURE(q);
    for(auto const aborted : {false, true})
    {
      CAPTURE(aborted);
      stub_link  link{l, declared()};
      auto const c = q - 5 * us;
      auto const s = c + 2 * us;
      link.window(0, q, {});
      link.forth->started(link.frame(s));
      if(aborted)
      {
        link.forth->aborted(vtime{static_cast<std::uint64_t>(s + 5 * us)});
      }
      CHECK_FALSE(pause_drain(link, 0, c).has_value());
      CHECK_FALSE(link.forth->quiet());
      link.forth->receiver_reset(core::sys_time{s + us});
      CHECK_FALSE(pause_drain(link, 0, c).has_value());
      CHECK(link.forth->quiet());
      CHECK(saved_state(*link.forth).carried.empty());
      run_windows(link, 0, 10, q);
      CHECK(link.forth_port.received.empty());
      CHECK(link.forth_port.cancels.empty());
    }
  }
}

TEST_CASE("posts starting exactly at a drain's barrier are carried, from the mailbox or held over")
{
  constexpr std::int64_t us = static_cast<std::int64_t>(us_ps);
  constexpr std::int64_t l  = 50 * us;
  for(auto const q : {l, l / 2})
  {
    CAPTURE(q);
    for(auto const held_over : {false, true})
    {
      CAPTURE(held_over);
      {
        stub_link link{l, declared()};
        link.window(0, q, {});
        link.forth->started(link.frame(q));
        if(held_over)
        {
          CHECK_FALSE(pause_drain(link, 0, q - 5 * us).has_value());
        }
        link.drain(0, q);
        CHECK(link.forth_port.received.empty());
        CHECK(saved_state(*link.forth).carried.size() == 1);
        REQUIRE(link.cut(q, std::nullopt, q) == core::sys_time{q});
        run_windows(link, 1, 10, q);
        CHECK(link.forth_port.received.empty());
      }
      {
        stub_link  link{l, declared()};
        auto const f = q - 10 * us;
        link.window(0, q, {});
        link.forth->started(link.frame(f));
        link.forth->aborted(vtime{static_cast<std::uint64_t>(q)});
        if(held_over)
        {
          CHECK_FALSE(pause_drain(link, 0, f - us).has_value());
          CHECK(link.forth_port.received.empty());
        }
        link.drain(0, q);
        CHECK(received_starts(link.forth_port) == std::vector<std::int64_t>{f + l});
        CHECK(link.forth_port.cancels.empty());
        CHECK(saved_state(*link.forth).carried.size() == 1);
        link.drain(1, q);
        CHECK(link.forth_port.cancels == std::vector<std::int64_t>{q + l});
      }
    }
  }
}

TEST_CASE("a carried frame reaches the port before the abort of it from the next mailbox")
{
  constexpr std::int64_t us = static_cast<std::int64_t>(us_ps);
  constexpr std::int64_t l  = 50 * us;
  for(auto const q : {l, l / 2})
  {
    CAPTURE(q);
    stub_link  link{l, declared()};
    auto const f = q + 1;
    auto const a = q + 5 * us;
    REQUIRE(a < 2 * q);
    REQUIRE(a < f + static_cast<std::int64_t>(declared().frame_ps()));
    link.window(0, q, {});
    link.forth->started(link.frame(f));
    link.drain(0, q);
    CHECK(saved_state(*link.forth).carried.size() == 1);
    link.window(1, q, {});
    link.forth->aborted(vtime{static_cast<std::uint64_t>(a)});
    link.drain(1, q);
    CHECK(
        link.forth_port.calls
        == std::vector<std::pair<char, std::int64_t>>{
            {'r', f + l},
            {'c', a + l}
    });
  }
}

TEST_CASE("a carried frame meets a receiver reset recorded after its carry at its handing")
{
  constexpr std::int64_t us       = static_cast<std::int64_t>(us_ps);
  constexpr std::int64_t l        = 50 * us;
  auto const             frame_ps = static_cast<std::int64_t>(declared().frame_ps());
  for(auto const q : {l, l / 2})
  {
    CAPTURE(q);
    auto const f   = q + 1;
    auto const a   = q + 5 * us;
    auto const run = [&](std::int64_t reset, bool aborted) {
      auto link = std::make_unique<stub_link>(l, declared());
      link->window(0, q, {});
      link->forth->started(link->frame(f));
      if(aborted)
      {
        link->forth->aborted(vtime{static_cast<std::uint64_t>(a)});
      }
      link->drain(0, q);
      CHECK(saved_state(*link->forth).carried.size() == (aborted ? 2U : 1U));
      link->forth->receiver_reset(core::sys_time{reset});
      run_windows(*link, 1, 10, q);
      return link;
    };
    CHECK(run(q + 2 * us, false)->forth_port.received.empty());
    CHECK(received_starts(run(q - us, false)->forth_port) == std::vector<std::int64_t>{f + l});
    REQUIRE(a + l < f + l + frame_ps);
    auto const shortened = run(a + l, true);
    CHECK(shortened->forth_port.received.empty());
    CHECK(shortened->forth_port.cancels == std::vector<std::int64_t>{a + l});
  }
}

namespace
{
struct span_before_carry
{
  core::sys_time start{};
  core::sys_time end{};
  bool           injected{};
  core::sys_time post{};
  std::uint64_t  index{};
};

struct state_before_carry
{
  std::uint64_t                  window{};
  std::uint64_t                  started{};
  core::sys_time                 drained_through{};
  std::vector<span_before_carry> delivered;
  core::sys_time                 last_barrier{};
};
}

TEST_CASE("a wire state saved before the carry loads with nothing carried and carries by the rule")
{
  constexpr std::int64_t us       = static_cast<std::int64_t>(us_ps);
  constexpr std::int64_t l        = 50 * us;
  constexpr std::int64_t q        = l;
  auto const             frame_ps = static_cast<std::int64_t>(declared().frame_ps());
  auto const             s        = q - 10 * us;
  auto const             g        = q + 5 * us;
  REQUIRE(g + l < s + l + frame_ps);
  state_before_carry const old{
      .window          = 1,
      .started         = 1,
      .drained_through = core::sys_time{2 * q},
      .delivered =
          {{.start = core::sys_time{s + l},
            .end   = core::sys_time{s + l + frame_ps},
            .post  = core::sys_time{s}}},
      .last_barrier = core::sys_time{q}};
  std::vector<std::byte> bytes;
  reflex::serde::bson::serializer{bytes}.dump(old);
  stub_link link{l, declared()};
  link.forth->load(bytes);
  auto const loaded = saved_state(*link.forth);
  CHECK(loaded.carried.empty());
  CHECK(loaded.cuts.empty());
  REQUIRE(loaded.delivered.size() == 1);
  CHECK_FALSE(loaded.delivered[0].carried);
  CHECK_FALSE(loaded.delivered[0].suppressed);
  CHECK_FALSE(loaded.delivered[0].truncated);
  link.window(1, q, {});
  link.forth->started(link.frame(g));
  link.forth->started(link.frame(2 * q));
  auto const fault = link.forth->drain(1, core::sys_time{2 * q}, core::sys_time{3 * q}, {});
  REQUIRE(fault.has_value());
  CHECK(fault->at.ps == g);
  CHECK(link.forth_port.received.empty());
  CHECK(saved_state(*link.forth).carried.size() == 1);
  link.drain(2, q);
  CHECK(received_starts(link.forth_port) == std::vector<std::int64_t>{2 * q + l});
}

TEST_CASE("a frame past the barrier inside a closed cut is final at its drain, never carried")
{
  constexpr std::int64_t us = static_cast<std::int64_t>(us_ps);
  constexpr std::int64_t l  = 50 * us;
  for(auto const q : {l, l / 2})
  {
    CAPTURE(q);
    auto const s = q + 1;
    for(auto const closed : {true, false})
    {
      CAPTURE(closed);
      std::optional<std::int64_t> const end =
          closed ? std::optional{q + 20 * us} : std::optional<std::int64_t>{};
      {
        stub_link link{l, declared()};
        REQUIRE(link.cut(q - 5 * us, end, 0).has_value());
        link.window(0, q, {});
        link.forth->started(link.frame(s));
        link.drain(0, q);
        CHECK(link.forth->quiet() == closed);
        CHECK(saved_state(*link.forth).carried.size() == (closed ? 0U : 1U));
        run_windows(link, 1, 10, q);
        CHECK(link.forth_port.received.empty());
      }
      {
        stub_link  link{l, declared()};
        auto const c = q - 2 * us;
        link.window(0, q, {});
        link.forth->started(link.frame(s));
        CHECK_FALSE(pause_drain(link, 0, c).has_value());
        CHECK(saved_state(*link.forth).carried.size() == 1);
        REQUIRE(link.cut(c, end, c).has_value());
        link.drain(0, q);
        CHECK(link.forth->quiet() == closed);
        CHECK(saved_state(*link.forth).carried.size() == (closed ? 0U : 1U));
        run_windows(link, 1, 10, q);
        CHECK(link.forth_port.received.empty());
      }
    }
  }
}

TEST_CASE("an abort past the barrier of a frame a cut truncates is final at its drain")
{
  constexpr std::int64_t  us   = static_cast<std::int64_t>(us_ps);
  constexpr std::int64_t  l    = 50 * us;
  stm32::line_shape const slow = {.baud = 9600};
  for(auto const q : {l, l / 2})
  {
    CAPTURE(q);
    auto const f = q - 10 * us;
    auto const a = 2 * q + 1;
    REQUIRE(a < f + static_cast<std::int64_t>(slow.frame_ps()));
    for(auto const when : {0, 1, 2})
    {
      CAPTURE(when);
      stub_link link{l, slow};
      if(when == 0)
      {
        REQUIRE(link.cut(q - 5 * us, q + 20 * us, 0).has_value());
      }
      link.window(0, q, {});
      link.forth->started(link.frame(f));
      link.drain(0, q);
      if(when == 1)
      {
        REQUIRE(link.cut(q, q + 20 * us, q).has_value());
      }
      link.window(1, q, {});
      link.forth->aborted(vtime{static_cast<std::uint64_t>(a)});
      link.drain(1, q);
      auto const cut_at = when == 0 ? q - 5 * us + l : q + l;
      CHECK(link.forth->quiet() == (when != 2));
      CHECK(saved_state(*link.forth).carried.size() == (when == 2 ? 1U : 0U));
      run_windows(link, 2, 10, q);
      CHECK(
          link.forth_port.cancels
          == (when == 2 ? std::vector<std::int64_t>{a + l} : std::vector<std::int64_t>{cut_at}));
    }
  }
}

TEST_CASE("a carried abort of a handed frame a later recorded reset drops is final at a pause")
{
  constexpr std::int64_t  us   = static_cast<std::int64_t>(us_ps);
  constexpr std::int64_t  l    = 50 * us;
  stm32::line_shape const slow = {.baud = 9600};
  for(auto const q : {l, l / 2})
  {
    CAPTURE(q);
    for(auto const reset : {true, false})
    {
      CAPTURE(reset);
      stub_link  link{l, slow};
      auto const s = q - 10 * us;
      auto const a = q + 5 * us;
      REQUIRE(a < s + static_cast<std::int64_t>(slow.frame_ps()));
      link.window(0, q, {});
      link.forth->started(link.frame(s));
      link.forth->aborted(vtime{static_cast<std::uint64_t>(a)});
      link.drain(0, q);
      CHECK(received_starts(link.forth_port) == std::vector<std::int64_t>{s + l});
      CHECK(saved_state(*link.forth).carried.size() == 1);
      if(reset)
      {
        link.forth->receiver_reset(core::sys_time{q + us});
      }
      link.window(1, q, {});
      CHECK_FALSE(pause_drain(link, 1, q + 2 * us).has_value());
      CHECK(link.forth->quiet() == reset);
      CHECK(saved_state(*link.forth).carried.size() == (reset ? 0U : 1U));
    }
  }
}

namespace
{
constexpr std::uint32_t link_error = 4;

auto stop_offset(rig const& r) -> std::int64_t
{
  auto const brr   = r.nodes[1]->f->field("USART1", "BRR", "BRR");
  auto const baud  = std::uint64_t{160'000'000} / brr;
  auto const frame = 20 * std::uint64_t{1'000'000'000'000} / (2 * baud);
  return static_cast<std::int64_t>(2 * frame * (16 * 9 + 9) / (16 * 20));
}

auto received_with_errors(std::vector<std::uint32_t> const& log) -> std::vector<entry>
{
  std::vector<entry> out;
  for(auto const& one : entries(log))
  {
    if(one.kind == link_byte or one.kind == link_error)
    {
      out.push_back(one);
    }
  }
  return out;
}
}

TEST_CASE(
    "a break cut turns the frame crossing t0 plus L into one break character at its stop sample")
{
  auto const all = oracle_tap("link_tx", "link_rx", latency_20);
  REQUIRE(all.size() == 64);
  auto const t0  = all[10].start.ps + static_cast<std::int64_t>(5 * us_ps);
  auto const t1  = all[14].start.ps + static_cast<std::int64_t>(5 * us_ps);
  auto const run = [&](run_plan const& plan) {
    auto r = paired("link_tx", "link_rx", latency_20);
    REQUIRE(cut_rig(r, t0, t1, 0, true).has_value());
    r.run(plan);
    return r;
  };
  auto       reference = run({.window = latency_20});
  auto const got       = received_with_errors(reference.nodes[1]->f->log());
  auto       expected  = without(all, 10, 14);
  expected.insert(expected.begin() + 10, 0);
  std::vector<std::uint32_t> bytes;
  std::vector<std::size_t>   errors;
  for(std::size_t index = 0; index < got.size(); ++index)
  {
    if(got[index].kind == link_byte)
    {
      bytes.push_back(got[index].data);
    }
    else
    {
      errors.push_back(index);
      CHECK(got[index].data == 1);
    }
  }
  CHECK(bytes == expected);
  REQUIRE(errors.size() == 1);
  REQUIRE(errors[0] == 10);
  REQUIRE(got[9].kind == link_byte);
  auto const gap      = all[10].start.ps + stop_offset(reference) - all[9].end.ps;
  auto const measured = static_cast<std::int64_t>(got[10].cycles - got[9].cycles);
  CAPTURE(gap);
  CHECK(measured >= gap / static_cast<std::int64_t>(cycle_ps) - 20);
  CHECK(measured <= gap / static_cast<std::int64_t>(cycle_ps) + 20);
  auto const& tap = reference.taps[0];
  for(std::size_t index = 0; index < tap.size(); ++index)
  {
    CAPTURE(index);
    CHECK(tap[index].cut == (index >= 10 and index <= 14));
    CHECK(tap[index].lost == (index == 10 or index == 14));
  }
  auto const dump = reference.dump();
  for(auto const& plan : variants_of(latency_20))
  {
    CAPTURE(plan.window);
    CAPTURE(plan.threaded);
    CHECK(run(plan).dump() == dump);
  }
}

TEST_CASE("a break cut from a frame's end gives its byte, then the break character, at two windows")
{
  auto const all = oracle_tap("link_tx", "link_rx", latency_20);
  REQUIRE(all.size() == 64);
  auto const                 t0 = all[10].end.ps;
  auto const                 t1 = all[14].start.ps + static_cast<std::int64_t>(5 * us_ps);
  std::optional<std::string> reference;
  for(auto const window : {latency_20, latency_20 / 3})
  {
    CAPTURE(window);
    auto r = paired("link_tx", "link_rx", latency_20);
    REQUIRE(cut_rig(r, t0, t1, 0, true).has_value());
    r.run({.window = window});
    auto const got = received_with_errors(r.nodes[1]->f->log());
    REQUIRE(got.size() > 12);
    CHECK(got[10].kind == link_byte);
    CHECK(got[10].data == all[10].data);
    CHECK(got[11].kind == link_error);
    CHECK(got[12].kind == link_byte);
    CHECK(got[12].data == 0);
    auto const gap      = stop_offset(r);
    auto const measured = static_cast<std::int64_t>(got[11].cycles - got[10].cycles);
    CHECK(measured >= gap / static_cast<std::int64_t>(cycle_ps) - 20);
    CHECK(measured <= gap / static_cast<std::int64_t>(cycle_ps) + 20);
    if(reference)
    {
      CHECK(r.dump() == *reference);
    }
    reference = r.dump();
  }
}

TEST_CASE("a receiver reset during a break cut or before its hold is handed leaves nothing of it")
{
  for(auto const during : {true, false})
  {
    CAPTURE(during);
    auto const latency = during ? latency_20 : 2 * ms_ps;
    auto const all     = oracle_tap("link_tx", "link_rx", latency);
    REQUIRE(all.size() == 64);
    auto const t0    = all[10].start.ps + static_cast<std::int64_t>(5 * us_ps);
    auto const t1    = all[14].start.ps + static_cast<std::int64_t>(5 * us_ps);
    auto const reset = during ? t0 + static_cast<std::int64_t>(latency + 2 * us_ps) : t0 + 1;
    auto const run   = [&](bool low, std::uint64_t window) {
      auto r = paired("link_tx", "link_rx", latency);
      REQUIRE(cut_rig(r, t0, t1, 0, low).has_value());
      r.run({
          .window = window,
          .end    = script_end_ps + latency,
          .reset  = member_reset{
                                 .member = 1, .at = static_cast<std::uint64_t>(reset), .entry = "link_rx"}
      });
      return r;
    };
    for(auto const window : {latency, latency / 4})
    {
      CAPTURE(window);
      auto const q = static_cast<std::int64_t>(window);
      if(not during)
      {
        REQUIRE(reset < (t0 / q + 1) * q);
      }
      auto const held = run(true, window);
      auto const idle = run(false, window);
      CHECK(held.nodes[1]->f->log() == idle.nodes[1]->f->log());
      CHECK(std::ranges::none_of(entries(held.nodes[1]->f->log()), [](entry const& one) {
        return one.kind == link_error;
      }));
    }
  }
}

TEST_CASE("a break cut hands its hold and release once, quiet only once each is handed")
{
  constexpr std::int64_t us = static_cast<std::int64_t>(us_ps);
  constexpr std::int64_t l  = 50 * us;
  for(auto const q : {l, l / 2})
  {
    CAPTURE(q);
    for(auto const closed : {true, false})
    {
      CAPTURE(closed);
      stub_link  link{l, declared()};
      auto const t0 = q / 2;
      auto const t1 = 3 * q + 5 * us;
      REQUIRE(link.cut(t0, closed ? std::optional{t1} : std::nullopt, 0, true).has_value());
      CHECK_FALSE(link.forth->quiet());
      CHECK_FALSE(link.back->quiet());
      link.window(0, q, {});
      link.drain(0, q);
      using call = std::pair<char, std::int64_t>;
      CHECK(
          link.forth_port.calls
          == std::vector<call>{
              {'h', t0 + l}
      });
      CHECK(
          link.back_port.calls
          == std::vector<call>{
              {'h', t0 + l}
      });
      CHECK(link.forth->quiet() == not closed);
      run_windows(link, 1, 3, q);
      CHECK(link.forth->quiet() == not closed);
      run_windows(link, 3, 10, q);
      CHECK(link.forth->quiet());
      if(closed)
      {
        CHECK(
            link.forth_port.calls
            == std::vector<call>{
                {'h', t0 + l},
                {'l', t1 + l}
        });
        CHECK(saved_state(*link.forth).cuts.empty());
      }
      else
      {
        CHECK(
            link.forth_port.calls
            == std::vector<call>{
                {'h', t0 + l}
        });
        REQUIRE(
            stm32::restore_link(*link.forth, *link.back, core::sys_time{10 * q})
            == core::sys_time{10 * q});
        CHECK_FALSE(link.forth->quiet());
        run_windows(link, 10, 14, q);
        CHECK(link.forth->quiet());
        CHECK(
            link.forth_port.calls
            == std::vector<call>{
                {'h', t0 + l    },
                {'l', 10 * q + l}
        });
      }
    }
  }
}
