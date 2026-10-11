#include "../../../native/src/core/machine_impl.hpp"

#include <popcorn/arch/armv8m/armv8m.hpp>
#include <popcorn/core/description.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/image/image.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/builtin.hpp>
#include <popcorn/periph/stm32/uart.hpp>

#include <sched.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <meta>
#include <optional>
#include <print>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
namespace core  = popcorn::core;
namespace stm32 = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::vtime;
using clock_type = std::chrono::steady_clock;

constexpr std::int64_t  ms_ps          = 1'000'000'000;
constexpr std::uint64_t fnv_basis      = 0xcbf2'9ce4'8422'2325u;
constexpr std::uint64_t fnv_prime      = 0x100'0000'01b3u;
constexpr std::uint32_t core_hz        = 160'000'000;
constexpr std::size_t   irq_array_size = 32;
constexpr std::size_t   uart_words     = 2 * 3;
constexpr std::uint32_t tim_cnt        = 0x24;

auto u575() -> popcorn::mcu_description const&
{
  static auto const desc = popcorn::load_description(POPCORN_U575_DESC);
  return desc;
}

auto firmware() -> popcorn::image::image const&
{
  static auto const loaded = popcorn::image::load_elf(POPCORN_FW_IRQ_LL);
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

class hashing_endpoint final : public stm32::uart_endpoint
{
public:
  std::uint64_t frames{};
  fnv           hash{};

  void transmitted(std::uint16_t data, vtime at) override
  {
    ++frames;
    hash.mix(data);
    hash.mix(at.ps);
  }
};

struct row
{
  std::string   name;
  std::string   entry;
  std::uint32_t rate{};
  std::uint32_t stores{};
  std::int64_t  divisor = 1;
};

struct options
{
  std::int64_t               runs       = 5;
  std::int64_t               virtual_ms = 1000;
  std::int64_t               warmup_ms  = 100;
  std::optional<std::string> only;
  std::optional<std::string> dump_dir;
};

struct sample
{
  double                 wall{};
  double                 virtual_s{};
  core::run_counters     counters{};
  std::string            dump;
  std::vector<std::byte> snapshot;
};

auto rows() -> std::vector<row>
{
  std::vector<row> out;
  out.push_back({.name = "spin", .entry = "irq_rate"});
  out.push_back({.name = "spin_store", .entry = "spin_store", .divisor = 50});
  for(auto const* entry : {"irq_rate", "irq_wfi"})
  {
    for(std::uint32_t const stores : {0u, 8u})
    {
      for(std::uint32_t const rate : {10'000u, 25'000u, 50'000u, 100'000u, 200'000u})
      {
        out.push_back(
            {.name   = std::format("{}/s{}/{}k", entry, stores, rate / 1000),
             .entry  = entry,
             .rate   = rate,
             .stores = stores});
      }
    }
  }
  out.push_back({.name = "uart_tx", .entry = "uart_tx"});
  return out;
}

struct lone
{
  std::array<std::shared_ptr<hashing_endpoint>, 2> ends{
      std::make_shared<hashing_endpoint>(), std::make_shared<hashing_endpoint>()};
  std::unique_ptr<core::machine> target;
  std::uint64_t                  origin{};

  explicit lone(row const& r)
  {
    stm32::register_builtin_models();
    stm32::board board{};
    board.uarts.emplace("USART1", ends[0]);
    board.uarts.emplace("USART2", ends[1]);
    core::machine_options options;
    options.faults         = popcorn::arch::fault_policy::stop;
    options.spin_threshold = 0;
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
      throw std::runtime_error{"the machine did not reach main"};
    }
    auto const period = r.rate == 0 ? 0u : core_hz / r.rate;
    if(not target->mem().write(symbol("irq_period_cycles"), 4, period)
       or not target->mem().write(symbol("irq_stores"), 4, r.stores))
    {
      throw std::runtime_error{"the bench variables are not writable"};
    }
    target->regs().write(*target->regs().find("pc"), symbol(r.entry));
    origin = target->virtual_clock().time().ps;
  }

  void run_to(std::int64_t until) const
  {
    for(;;)
    {
      auto const result = target->run_slice(
          {.max_instructions = icount{1'000'000'000},
           .wall_budget      = std::chrono::seconds{60},
           .until_time       = vtime{origin + static_cast<std::uint64_t>(until)}});
      if(not result.stopped)
      {
        continue;
      }
      if(result.stopped->reason != popcorn::stop_reason::time)
      {
        throw std::runtime_error{std::format(
            "the machine stopped as {} at {:#010x}", static_cast<int>(result.stopped->reason),
            result.stopped->where.pc)};
      }
      return;
    }
  }

  [[nodiscard]] auto word(popcorn::address at) const -> std::uint64_t
  {
    return target->mem().read(at, 4).value_or(0xdead'beefu);
  }

  [[nodiscard]] auto dump() const -> std::string
  {
    std::string out;
    auto const  put = [&out](std::uint64_t value) {
      out.append(reinterpret_cast<char const*>(&value), sizeof value);
    };
    put(target->virtual_clock().time().ps);
    put(target->virtual_clock().now().value);
    put(word(symbol("irq_count")));
    put(word(symbol("spins")));
    put(target->peripheral("TIM6").peek(tim_cnt));
    fnv registers;
    for(auto const& reg : target->regs().list())
    {
      registers.mix(target->regs().read(reg));
    }
    put(registers.value);
    fnv array;
    for(std::size_t index = 0; index < irq_array_size; ++index)
    {
      array.mix(word(symbol("irq_array") + 4 * index));
    }
    put(array.value);
    for(std::size_t index = 0; index < uart_words; ++index)
    {
      put(word(symbol("uart_ports") + 4 * index));
    }
    for(auto const& end : ends)
    {
      put(end->frames);
      put(end->hash.value);
    }
    return out;
  }
};

auto difference(core::run_counters const& to, core::run_counters const& from) -> core::run_counters
{
  core::run_counters out;
  template for(constexpr auto member : std::define_static_array(
                   std::meta::nonstatic_data_members_of(
                       ^^core::run_counters, std::meta::access_context::unchecked())))
  {
    if constexpr(std::is_integral_v<typename[:std::meta::type_of(member):]>)
    {
      out.[:member:] = to.[:member:] - from.[:member:];
    }
    else
    {
      for(std::size_t index = 0; index < out.[:member:].size(); ++index)
      {
        out.[:member:][index] = to.[:member:][index] - from.[:member:][index];
      }
    }
  }
  return out;
}

auto measure(options const& how, row const& r) -> sample
{
  lone       m{r};
  auto const warmup = how.warmup_ms * ms_ps;
  auto const span   = how.virtual_ms * ms_ps / r.divisor;
  m.run_to(warmup);
  auto const before = core::machine_internals::counters(*m.target);
  auto const from   = clock_type::now();
  m.run_to(warmup + span);
  auto const wall  = std::chrono::duration<double>(clock_type::now() - from).count();
  auto const after = core::machine_internals::counters(*m.target);
  return {
      .wall      = wall,
      .virtual_s = static_cast<double>(span) / 1e12,
      .counters  = difference(after, before),
      .dump      = m.dump(),
      .snapshot  = how.dump_dir ? core::encode_snapshot(m.target->snapshot().data())
                                : std::vector<std::byte>{}};
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
    if(flag == "--runs")
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
    else if(flag == "--rows")
    {
      out.only = std::string{value};
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
  return out;
}

auto median(std::vector<double> values) -> double
{
  std::ranges::sort(values);
  auto const n = values.size();
  return n % 2 != 0 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2;
}

auto first_line(std::string const& path) -> std::string
{
  std::ifstream file{path};
  std::string   line;
  std::getline(file, line);
  return line;
}

auto usable_cpus() -> int
{
  cpu_set_t set;
  CPU_ZERO(&set);
  return sched_getaffinity(0, sizeof set, &set) == 0 ? CPU_COUNT(&set) : 0;
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
      usable_cpus());
  auto const group = first_line("/proc/self/cgroup");
  auto const path  = group.substr(group.rfind(':') + 1);
  auto const quota = first_line(std::format("/sys/fs/cgroup{}/cpu.max", path));
  std::println("# cgroup {} cpu.max={}", path, quota.empty() ? "none" : quota);
  std::fflush(stdout);
}

auto end_names() -> std::string
{
  std::string out;
  template for(constexpr auto e :
               std::define_static_array(std::meta::enumerators_of(^^core::engine_end)))
  {
    out += std::format(" end_{}/vs", std::meta::identifier_of(e));
  }
  return out;
}

struct collected
{
  std::vector<double>             per_vs;
  std::vector<core::run_counters> counters;
  double                          virtual_s{};
};

auto per_vs(collected const& c, auto field) -> double
{
  std::vector<double> values;
  for(auto const& one : c.counters)
  {
    values.push_back(static_cast<double>(field(one)) / c.virtual_s);
  }
  return median(values);
}

struct fit
{
  double slope{};
  double intercept{};
  double slope_error{};
  double max_residual{};
};

auto least_squares(std::vector<std::pair<double, double>> const& points) -> fit
{
  auto const n = static_cast<double>(points.size());
  double     sx{}, sy{}, sxx{}, sxy{};
  for(auto const& [x, y] : points)
  {
    sx += x;
    sy += y;
    sxx += x * x;
    sxy += x * y;
  }
  fit        out;
  auto const denominator = n * sxx - sx * sx;
  out.slope              = (n * sxy - sx * sy) / denominator;
  out.intercept          = (sy - out.slope * sx) / n;
  double squares{};
  for(auto const& [x, y] : points)
  {
    auto const residual = y - (out.intercept + out.slope * x);
    squares += residual * residual;
    out.max_residual = std::max(out.max_residual, std::abs(residual));
  }
  auto const variance = points.size() > 2 ? squares / (n - 2) : 0.0;
  out.slope_error     = std::sqrt(variance * n / denominator);
  return out;
}

void summary(std::vector<row> const& chosen, std::map<std::string, collected> const& results)
{
  std::println(
      "\n| row | median wall s/vs | min | max | spread % | entries/vs | runs/vs | runs/entry | "
      "hooked stores/vs | stores/entry | ctl calls/vs | ctl/entry | ctl/store |{}",
      end_names());
  std::println("|---|---|---|---|---|---|---|---|---|---|---|---|---|{}", [] {
    std::string bars;
    for(std::size_t index = 0; index < core::engine_end_count; ++index)
    {
      bars += "---|";
    }
    return bars;
  }());
  for(auto const& r : chosen)
  {
    auto const& c       = results.at(r.name);
    auto const  md      = median(c.per_vs);
    auto const  lo      = *std::ranges::min_element(c.per_vs);
    auto const  hi      = *std::ranges::max_element(c.per_vs);
    auto const  entries = per_vs(c, [](auto const& k) { return k.entries; });
    auto const  runs    = per_vs(c, [](auto const& k) { return k.engine_runs; });
    auto const  stores  = per_vs(c, [](auto const& k) { return k.guest_stores_hooked; });
    auto const  calls =
        per_vs(c, [](auto const& k) { return k.remove_calls_in_engine + k.remove_calls_direct; });
    auto const  ratio = [](double a, double b) { return b == 0 ? 0.0 : a / b; };
    std::string ends;
    for(std::size_t index = 0; index < core::engine_end_count; ++index)
    {
      ends += std::format(" {:.0f} |", per_vs(c, [index](auto const& k) { return k.ends[index]; }));
    }
    std::println(
        "| {} | {:.4f} | {:.4f} | {:.4f} | {:.1f} | {:.0f} | {:.0f} | {:.2f} | {:.0f} | {:.2f} | "
        "{:.0f} | {:.2f} | {:.2f} |{}",
        r.name, md, lo, hi, (hi - lo) / md * 100, entries, runs, ratio(runs, entries), stores,
        ratio(stores, entries), calls, ratio(calls, entries), ratio(calls, stores), ends);
  }
  std::println(
      "\n| slope over | us/entry | intercept s/vs | slope error us/entry | error % | "
      "max residual s/vs |");
  std::println("|---|---|---|---|---|---|");
  for(auto const* entry : {"irq_rate", "irq_wfi"})
  {
    for(std::uint32_t const stores : {0u, 8u})
    {
      std::vector<std::pair<double, double>> points;
      for(auto const& r : chosen)
      {
        if(r.entry == entry and r.stores == stores and r.rate != 0)
        {
          auto const& c = results.at(r.name);
          points.emplace_back(per_vs(c, [](auto const& k) { return k.entries; }), median(c.per_vs));
        }
      }
      if(points.size() < 2)
      {
        continue;
      }
      auto const f = least_squares(points);
      std::println(
          "| {}/s{} | {:.3f} | {:.4f} | {:.3f} | {:.1f} | {:.4f} |", entry, stores, f.slope * 1e6,
          f.intercept, f.slope_error * 1e6, f.slope_error / f.slope * 100, f.max_residual);
    }
  }
  std::fflush(stdout);
}

auto bench(options const& how) -> int
{
  std::println(
      "# bench-irq runs={} virtual_ms={} warmup_ms={} spin_threshold=0", how.runs, how.virtual_ms,
      how.warmup_ms);
  std::println("# build {} compiler=gcc {}", POPCORN_BENCH_BUILD, __VERSION__);
  env_line();
  if(how.dump_dir)
  {
    std::filesystem::create_directories(*how.dump_dir);
  }
  std::vector<row> chosen;
  for(auto const& r : rows())
  {
    if(not how.only
       or std::ranges::contains(
           std::views::split(*how.only, ',') | std::views::transform([](auto part) {
             return std::string_view{part.begin(), part.end()};
           }),
           std::string_view{r.name}))
    {
      chosen.push_back(r);
    }
  }
  std::map<std::string, collected>   results;
  std::map<std::string, std::string> references;
  bool                               mismatch = false;
  for(std::int64_t run = 0; run < how.runs; ++run)
  {
    for(auto const& r : chosen)
    {
      auto const  out       = measure(how, r);
      auto const& reference = references.try_emplace(r.name, out.dump).first->second;
      auto const  same      = out.dump == reference;
      mismatch              = mismatch or not same;
      if(how.dump_dir)
      {
        auto file = r.name;
        std::ranges::replace(file, '/', '-');
        std::ofstream{std::format("{}/{}-{}.bin", *how.dump_dir, file, run), std::ios::binary}
            << out.dump;
        std::ofstream{std::format("{}/{}-{}.snap", *how.dump_dir, file, run), std::ios::binary}
            .write(
                reinterpret_cast<char const*>(out.snapshot.data()),
                static_cast<std::streamsize>(out.snapshot.size()));
      }
      fnv hash;
      for(auto const c : out.dump)
      {
        hash.mix(static_cast<unsigned char>(c));
      }
      auto& c     = results[r.name];
      c.virtual_s = out.virtual_s;
      c.per_vs.push_back(out.wall / out.virtual_s);
      c.counters.push_back(out.counters);
      std::println(
          "run={} row={} wall_s={:.4f} wall_per_vs={:.4f} entries={} engine_runs={} "
          "hooked_stores={} dump={:016x} same={}",
          run, r.name, out.wall, out.wall / out.virtual_s, out.counters.entries,
          out.counters.engine_runs, out.counters.guest_stores_hooked, hash.value,
          same ? "yes" : "NO");
      std::fflush(stdout);
    }
  }
  summary(chosen, results);
  env_line();
  if(mismatch)
  {
    std::println("MISMATCH: a run's dump differs from the first run's of its row");
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
