#include <popcorn/arch/armv8m/armv8m.hpp>
#include <popcorn/core/description.hpp>
#include <popcorn/core/host.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/member_link.hpp>
#include <popcorn/core/system.hpp>
#include <popcorn/core/window_barrier.hpp>
#include <popcorn/image/image.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/builtin.hpp>
#include <popcorn/periph/stm32/uart.hpp>
#include <popcorn/periph/stm32/uart_wire.hpp>
#include <popcorn/periph/uart.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <print>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace periph = popcorn::periph;
namespace stm32  = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::vtime;
using clock_type = std::chrono::steady_clock;

constexpr std::int64_t     us_ps         = 1'000'000;
constexpr std::int64_t     ms_ps         = 1'000'000'000;
constexpr std::size_t      tap_capacity  = std::size_t{1} << 17;
constexpr std::uint64_t    fnv_basis     = 0xcbf2'9ce4'8422'2325u;
constexpr std::uint64_t    fnv_prime     = 0x100'0000'01b3u;
constexpr std::size_t      audio_words   = 2 * 7;
constexpr std::uint32_t    link_baud     = 460'800;
constexpr std::string_view firmware_name = "usart_ll";

auto u575() -> popcorn::part_description const&
{
  static auto const desc = popcorn::load_description(POPCORN_U575_DESC);
  return desc;
}

auto firmware() -> popcorn::image::image const&
{
  static auto const loaded = popcorn::image::load_elf(POPCORN_FW_USART_LL);
  return loaded;
}

auto symbol(std::string_view name) -> popcorn::address
{
  return firmware().lookup(name).value;
}

struct fnv
{
  std::uint64_t value = fnv_basis;

  void mix(std::uint64_t word) noexcept
  {
    for(int shift = 0; shift < 64; shift += 8)
    {
      value = (value ^ ((word >> shift) & 0xffu)) * fnv_prime;
    }
  }
};

class hashing_endpoint final : public periph::uart_endpoint
{
public:
  std::uint64_t frames{};
  fnv           hash{};

  void transmitted(std::uint16_t data, vtime at, bool brk) override
  {
    ++frames;
    hash.mix(data);
    hash.mix(at.ps);
    hash.mix(brk ? 1 : 0);
  }
};

struct node
{
  std::array<std::shared_ptr<hashing_endpoint>, 2> ends{
      std::make_shared<hashing_endpoint>(), std::make_shared<hashing_endpoint>()};
  std::unique_ptr<core::machine>              target;
  std::int64_t                                origin{};
  std::array<stm32::uart_link_port*, 2>       ports{};
  std::shared_ptr<std::atomic<std::uint64_t>> warnings =
      std::make_shared<std::atomic<std::uint64_t>>(0);
  core::hook_handle sink;

  node(std::string_view entry, std::uint32_t spin_threshold)
  {
    stm32::board board{};
    board.uarts.emplace("USART1", ends[0]);
    board.uarts.emplace("USART2", ends[1]);
    core::machine_options options;
    options.faults         = popcorn::arch::fault_policy::stop;
    options.spin_threshold = spin_threshold;
    options.models         = stm32::factories(board, u575());
    target                 = std::make_unique<core::machine>(
        u575(), popcorn::arch::armv8m::factory(), std::move(options));
    target->load(firmware());
    target->reset();
    auto const booted = target->run_slice(
        {.max_instructions = icount{100'000'000},
         .wall_budget      = std::chrono::seconds{60},
         .until            = symbol("main")});
    if(not booted.stopped or booted.stopped->reason != popcorn::stop_reason::address)
    {
      throw std::runtime_error{"member did not reach main"};
    }
    target->regs().write(*target->regs().find("pc"), symbol(entry));
    origin   = static_cast<std::int64_t>(target->virtual_clock().time().ps);
    ports[0] = dynamic_cast<stm32::uart_link_port*>(target->model("USART1"));
    ports[1] = dynamic_cast<stm32::uart_link_port*>(target->model("USART2"));
    if(ports[0] == nullptr or ports[1] == nullptr)
    {
      throw std::runtime_error{"member has no USART1 or USART2 port"};
    }
    sink = target->add_log_sink([counter = warnings](core::log_record const& record) {
      if(record.kind == core::log_kind::model_warning)
      {
        counter->fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
};

struct measured
{
  double      wall{};
  double      warmup_wall{};
  core::pause stopped{};
  std::string dump;
};

class world
{
public:
  world(std::int64_t latency, std::uint32_t spin_threshold)
  {
    stm32::register_builtin_models();
    nodes_.push_back(std::make_unique<node>("link_audio", spin_threshold));
    nodes_.push_back(std::make_unique<node>("link_audio_peer", spin_threshold));
    nodes_.push_back(std::make_unique<node>("link_audio_peer", spin_threshold));
    link(0, 0, 1, 0, latency);
    link(1, 0, 0, 0, latency);
    link(0, 1, 2, 0, latency);
    link(2, 0, 0, 1, latency);
    std::vector<core::member_ref> members;
    for(std::size_t index = 0; index < nodes_.size(); ++index)
    {
      std::vector<std::unique_ptr<core::machine>> owned;
      owned.push_back(std::move(nodes_[index]->target));
      hosts_.push_back(std::make_shared<core::host>(std::move(owned)));
      members.push_back({.on = hosts_.back(), .name = std::format("m{}", index)});
    }
    std::vector<std::shared_ptr<core::member_link>> links{wires_.begin(), wires_.end()};
    sys_            = std::make_unique<core::system>(members, std::move(links));
    auto const held = sys_->lock();
    sys_->start(held, std::vector<core::sys_time>(nodes_.size()));
  }

  void measure(bool on)
  {
    auto const held = sys_->lock();
    sys_->measure_work(held, on);
  }

  auto run_to(std::int64_t until) -> core::pause
  {
    core::host_future<core::pause> pending;
    {
      auto const held = sys_->lock();
      pending         = sys_->resume(held, core::sys_time{until});
    }
    auto stopped = pending.get();
    if(stopped.reason != core::pause_reason::system_time)
    {
      throw std::runtime_error{std::format(
          "the system paused as {} at {} ps", static_cast<int>(stopped.reason), stopped.at.ps)};
    }
    reasons_.mix(static_cast<std::uint64_t>(stopped.reason));
    reasons_.mix(static_cast<std::uint64_t>(stopped.at.ps));
    return stopped;
  }

  [[nodiscard]] auto dump() -> std::string
  {
    std::string out;
    auto const  put = [&out](std::uint64_t value) {
      out.append(reinterpret_cast<char const*>(&value), sizeof value);
    };
    auto const held = sys_->lock();
    for(std::size_t index = 0; index < nodes_.size(); ++index)
    {
      auto const& one = *nodes_[index];
      put(static_cast<std::uint64_t>(sys_->time_of(held, index).ps));
      core::access(*sys_member(index), 0, [&](core::machine& m) {
        auto const base = symbol("audio_ports");
        for(std::size_t word = 0; word < audio_words; ++word)
        {
          put(m.mem().read(base + 4 * word, 4).value_or(0xdead'beefu));
        }
      });
      for(auto const& end : one.ends)
      {
        put(end->frames);
        put(end->hash.value);
      }
      put(one.warnings->load());
    }
    for(auto const& wire : wires_)
    {
      auto const frames = wire->tap().take();
      fnv        hash;
      for(auto const& frame : frames)
      {
        hash.mix(frame.index);
        hash.mix(frame.data);
        hash.mix(static_cast<std::uint64_t>(frame.start.ps));
        hash.mix(static_cast<std::uint64_t>(frame.end.ps));
        hash.mix(static_cast<std::uint64_t>(frame.arrival.ps));
        hash.mix((frame.lost ? 1u : 0u) | (frame.brk ? 2u : 0u) | (frame.dropped ? 4u : 0u));
      }
      put(frames.size());
      put(wire->tap().dropped());
      put(hash.value);
    }
    put(reasons_.value);
    return out;
  }

private:
  void link(
      std::size_t  from,
      std::size_t  from_port,
      std::size_t  to,
      std::size_t  to_port,
      std::int64_t latency)
  {
    auto&      sender   = *nodes_[from];
    auto&      receiver = *nodes_[to];
    auto const lane     = static_cast<std::uint32_t>(2 * wires_.size() + 1);
    auto*      machine  = receiver.target.get();
    auto       wire     = std::make_shared<stm32::uart_wire>(
        stm32::wire_ends{.sender = from, .receiver = to},
        stm32::wire_lanes{.peer = lane, .injected = lane + 1}, core::sys_time{latency},
        stm32::line_shape{.baud = link_baud},
        stm32::wire_receiver{
            .port   = receiver.ports[to_port],
            .unit   = to_port == 0 ? "USART1" : "USART2",
            .log    = [machine](core::log_record record) { machine->log(std::move(record)); },
            .strict = false},
        sender.ends[from_port], tap_capacity);
    sender.ports[from_port]->attach(wire);
    wires_.push_back(std::move(wire));
  }

  [[nodiscard]] auto sys_member(std::size_t index) const -> core::host*
  {
    return hosts_[index].get();
  }

  std::vector<std::shared_ptr<core::host>>       hosts_;
  std::vector<std::unique_ptr<node>>             nodes_;
  std::vector<std::shared_ptr<stm32::uart_wire>> wires_;
  std::unique_ptr<core::system>                  sys_;
  fnv                                            reasons_{};
};

struct variant
{
  std::string name;
};

auto variant_named(std::string_view name) -> variant
{
  if(name == "B" or name == "solo" or name == "solo-plain" or name == "solo-held")
  {
    return {.name = std::string{name}};
  }
  throw std::invalid_argument{
      std::format("bad variant {}, want B, solo, solo-plain or solo-held", name)};
}

auto parse_number(std::string_view text) -> std::int64_t
{
  std::int64_t value{};
  auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if(error != std::errc{} or end != text.data() + text.size() or value <= 0)
  {
    throw std::invalid_argument{std::format("bad number {}", text)};
  }
  return value;
}

auto median(std::vector<double> values) -> double
{
  std::ranges::sort(values);
  auto const n = values.size();
  return n % 2 != 0 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2;
}

auto first_line(char const* path) -> std::string
{
  std::ifstream file{path};
  std::string   line;
  std::getline(file, line);
  return line;
}

void env_line()
{
  auto const now  = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
  auto       load = first_line("/proc/loadavg");
  load            = load.substr(0, load.find(' ', load.find(' ', load.find(' ') + 1) + 1));
  std::println(
      "# {:%FT%T} load={} governor={} epp={} cpus={}", now, load,
      first_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"),
      first_line("/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference"),
      core::usable_cpus());
  auto const group = first_line("/proc/self/cgroup");
  auto const path  = group.substr(group.rfind(':') + 1);
  auto const quota = first_line(std::format("/sys/fs/cgroup{}/cpu.max", path).c_str());
  std::println("# cgroup {} cpu.max={}", path, quota.empty() ? "none" : quota);
  std::fflush(stdout);
}

struct options
{
  std::int64_t               latency_us = 20;
  std::int64_t               runs       = 5;
  std::int64_t               virtual_ms = 2000;
  std::int64_t               warmup_ms  = 100;
  std::uint32_t              spin_threshold{};
  bool                       measuring = true;
  std::vector<variant>       variants;
  std::optional<std::string> dump_dir;
};

auto parse(std::vector<std::string_view> const& args) -> options
{
  options out;
  for(std::size_t index = 0; index < args.size(); ++index)
  {
    auto const flag = args[index];
    if(index + 1 >= args.size())
    {
      throw std::invalid_argument{std::format("{} needs a value", flag)};
    }
    auto const value = args[++index];
    if(flag == "--latency-us")
    {
      out.latency_us = parse_number(value);
    }
    else if(flag == "--runs")
    {
      out.runs = parse_number(value);
    }
    else if(flag == "--virtual-ms")
    {
      out.virtual_ms = parse_number(value);
    }
    else if(flag == "--warmup-ms")
    {
      out.warmup_ms = parse_number(value);
    }
    else if(flag == "--variants")
    {
      for(auto const part : std::views::split(value, ','))
      {
        out.variants.push_back(variant_named(std::string_view{part.begin(), part.end()}));
      }
    }
    else if(flag == "--spin-threshold")
    {
      out.spin_threshold = value == "0" ? 0 : static_cast<std::uint32_t>(parse_number(value));
    }
    else if(flag == "--measure")
    {
      if(value != "on" and value != "off")
      {
        throw std::invalid_argument{"--measure wants on or off"};
      }
      out.measuring = value == "on";
    }
    else if(flag == "--dump")
    {
      out.dump_dir = std::string{value};
    }
    else
    {
      throw std::invalid_argument{std::format("unknown flag {}", flag)};
    }
  }
  if(out.variants.empty())
  {
    for(auto const* name : {"B"})
    {
      out.variants.push_back(variant_named(name));
    }
  }
  return out;
}

auto seconds(std::chrono::nanoseconds value) -> double
{
  return std::chrono::duration<double>(value).count();
}

enum class slicing : std::uint8_t
{
  whole,
  plain,
  held,
};

auto run_alone(node& lone, std::int64_t until, slicing how, std::int64_t slice) -> void
{
  auto const now = [&lone] {
    return static_cast<std::int64_t>(lone.target->virtual_clock().time().ps) - lone.origin;
  };
  for(;;)
  {
    auto bound = until;
    if(how != slicing::whole)
    {
      bound = std::min(until, (now() / slice + 1) * slice);
    }
    auto const result = lone.target->run_slice(
        {.max_instructions      = icount{1'000'000'000},
         .wall_budget           = std::chrono::seconds{60},
         .until_time            = vtime{static_cast<std::uint64_t>(bound + lone.origin)},
         .hold_from_until       = how == slicing::held,
         .internal_stop         = how == slicing::held,
         .stop_when_events_only = how == slicing::held});
    if(not result.stopped)
    {
      continue;
    }
    if(result.stopped->reason != popcorn::stop_reason::time)
    {
      throw std::runtime_error{
          std::format("the lone machine stopped as {}", static_cast<int>(result.stopped->reason))};
    }
    if(bound == until)
    {
      return;
    }
  }
}

auto measure_alone(options const& how, variant const& v) -> measured
{
  stm32::register_builtin_models();
  auto const mode  = v.name == "solo-plain" ? slicing::plain
                   : v.name == "solo-held"  ? slicing::held
                                            : slicing::whole;
  auto const slice = how.latency_us * us_ps;
  node       lone{"link_audio", how.spin_threshold};
  auto const warm_from = clock_type::now();
  run_alone(lone, how.warmup_ms * ms_ps, mode, slice);
  auto const warm_wall = std::chrono::duration<double>(clock_type::now() - warm_from).count();
  auto const from      = clock_type::now();
  run_alone(lone, (how.warmup_ms + how.virtual_ms) * ms_ps, mode, slice);
  auto const  wall = std::chrono::duration<double>(clock_type::now() - from).count();
  std::string out;
  auto const  put = [&out](std::uint64_t value) {
    out.append(reinterpret_cast<char const*>(&value), sizeof value);
  };
  put(lone.target->virtual_clock().time().ps);
  for(auto const& end : lone.ends)
  {
    put(end->frames);
    put(end->hash.value);
  }
  return {.wall = wall, .warmup_wall = warm_wall, .stopped = {}, .dump = std::move(out)};
}

auto measure(options const& how, variant const& v) -> measured
{
  if(v.name.starts_with("solo"))
  {
    return measure_alone(how, v);
  }
  world      w{how.latency_us * us_ps, how.spin_threshold};
  auto const warm_from = clock_type::now();
  std::ignore          = w.run_to(how.warmup_ms * ms_ps);
  auto const warm_wall = std::chrono::duration<double>(clock_type::now() - warm_from).count();
  w.measure(how.measuring);
  auto const from    = clock_type::now();
  auto const stopped = w.run_to((how.warmup_ms + how.virtual_ms) * ms_ps);
  auto const wall    = std::chrono::duration<double>(clock_type::now() - from).count();
  return {.wall = wall, .warmup_wall = warm_wall, .stopped = stopped, .dump = w.dump()};
}

struct row
{
  std::vector<double> per_vs;
  std::vector<double> serial_per_vs;
  std::vector<double> ideal_per_vs;
  std::vector<double> overhead_per_window_us;
  std::vector<double> windows_per_vs;
  std::vector<double> slices_per_vs;
  std::vector<double> phase_b_per_vs;
  std::vector<double> drain_share;
};

void summary(options const& how, std::map<std::string, row> const& rows)
{
  std::println(
      "\n| variant | median wall s/vs | min | max | spread % | S s/vs | ideal s/vs | "
      "overhead us/window | windows/vs | slices/vs | phase B/vs | drain share % |");
  std::println("|---|---|---|---|---|---|---|---|---|---|---|---|");
  for(auto const& v : how.variants)
  {
    auto const& r  = rows.at(v.name);
    auto const  md = median(r.per_vs);
    auto const  lo = *std::ranges::min_element(r.per_vs);
    auto const  hi = *std::ranges::max_element(r.per_vs);
    std::println(
        "| {} | {:.4f} | {:.4f} | {:.4f} | {:.1f} | {:.4f} | {:.4f} | {:.3f} | {:.0f} | {:.0f} | "
        "{:.0f} | {:.2f} |",
        v.name, md, lo, hi, (hi - lo) / md * 100, median(r.serial_per_vs), median(r.ideal_per_vs),
        median(r.overhead_per_window_us), median(r.windows_per_vs), median(r.slices_per_vs),
        median(r.phase_b_per_vs), median(r.drain_share) * 100);
  }
  std::fflush(stdout);
}

auto bench(options const& how) -> int
{
  std::println(
      "# bench-system L={}us runs={} virtual_ms={} warmup_ms={} members=3 baud={} block=512B/16ms "
      "spin_threshold={} measure={}",
      how.latency_us, how.runs, how.virtual_ms, how.warmup_ms, link_baud, how.spin_threshold,
      how.measuring ? "on" : "off");
  std::println("# build {} compiler=gcc {}", POPCORN_BENCH_BUILD, __VERSION__);
  env_line();
  if(how.dump_dir)
  {
    std::filesystem::create_directories(*how.dump_dir);
  }
  auto const                  vs = static_cast<double>(how.virtual_ms) / 1000;
  std::map<std::string, row>  rows;
  std::map<bool, std::string> references;
  bool                        mismatch = false;
  for(std::int64_t run = 0; run < how.runs; ++run)
  {
    for(auto const& v : how.variants)
    {
      auto const  out     = measure(how, v);
      auto const& work    = out.stopped.work;
      auto const  windows = static_cast<double>(out.stopped.windows);
      auto const& reference =
          references.try_emplace(v.name.starts_with("solo"), out.dump).first->second;
      auto const same = out.dump == reference;
      mismatch        = mismatch or not same;
      if(how.dump_dir)
      {
        std::ofstream{
            std::format("{}/L{}-{}-{}.bin", *how.dump_dir, how.latency_us, v.name, run),
            std::ios::binary}
            << out.dump;
      }
      fnv hash;
      for(auto const c : out.dump)
      {
        hash.mix(static_cast<unsigned char>(c));
      }
      auto& r = rows[v.name];
      r.per_vs.push_back(out.wall / vs);
      r.serial_per_vs.push_back(seconds(work.serial) / vs);
      r.ideal_per_vs.push_back(seconds(work.ideal) / vs);
      r.overhead_per_window_us.push_back(
          (out.wall - seconds(work.ideal)) * 1e6 / std::max(1.0, windows));
      r.windows_per_vs.push_back(windows / vs);
      r.slices_per_vs.push_back(static_cast<double>(out.stopped.slices) / vs);
      r.phase_b_per_vs.push_back(static_cast<double>(out.stopped.phase_b) / vs);
      r.drain_share.push_back(
          work.serial.count() == 0 ? 0.0 : seconds(work.drains) / seconds(work.serial));
      std::println(
          "run={} variant={} wall_s={:.4f} warmup_wall_s={:.4f} wall_per_vs={:.4f} windows={} "
          "rounds={} slices={} phase_b={} serial_s={:.4f} ideal_s={:.4f} drains_s={:.4f} "
          "dump={:016x} same={}",
          run, v.name, out.wall, out.warmup_wall, out.wall / vs, out.stopped.windows, work.rounds,
          out.stopped.slices, out.stopped.phase_b, seconds(work.serial), seconds(work.ideal),
          seconds(work.drains), hash.value, same ? "yes" : "NO");
      std::fflush(stdout);
    }
  }
  summary(how, rows);
  env_line();
  if(mismatch)
  {
    std::println("MISMATCH: a run's dump differs from the first run's");
    return 2;
  }
  return 0;
}
}

auto main(int argc, char** argv) -> int
{
  try
  {
    return bench(parse(std::vector<std::string_view>(argv + 1, argv + argc)));
  }
  catch(std::exception const& e)
  {
    std::println(stderr, "error: {}", e.what());
    return 1;
  }
}
