#include "i2c_medium.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/host.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/member_link.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/core/system.hpp>
#include <popcorn/periph/devices/i2c_eeprom.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/uart_wire.hpp>

#include <reflex/serde/bson.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
namespace core    = popcorn::core;
namespace periph  = popcorn::periph;
namespace devices = popcorn::periph::devices;
namespace stm32   = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;
using popcorn::testing::u575;

constexpr std::int64_t us_ps      = 1'000'000;
constexpr std::int64_t period_ps  = 2'500'000;
constexpr std::int64_t address_ps = 10 * period_ps;
constexpr std::int64_t byte_ps    = 9 * period_ps;
constexpr std::int64_t latency    = 20 * us_ps;

constexpr popcorn::address area       = 0x2008'0000u;
constexpr std::uint32_t    magic      = 0x12C11Bu;
constexpr std::uint32_t    controller = 1;
constexpr std::uint32_t    target_fw  = 2;
constexpr std::uint32_t    boot       = 0xB000'0000u;
constexpr std::uint32_t    log_at     = 296;

constexpr std::uint32_t mode_target = 0;
constexpr std::uint32_t mode_off    = 1;
constexpr std::uint32_t mode_busy   = 2;
constexpr std::uint32_t mode_held   = 3;

constexpr auto op(std::uint32_t code, std::uint32_t arg = 0) -> std::uint32_t
{
  return code << 24 | arg;
}

constexpr auto write(std::uint32_t address, std::uint32_t count, std::uint32_t first)
    -> std::uint32_t
{
  return op(1, address << 16 | count << 8 | first);
}

constexpr auto read(std::uint32_t address, std::uint32_t count) -> std::uint32_t
{
  return op(2, address << 16 | count << 8);
}

constexpr auto write_sr(std::uint32_t address, std::uint32_t count, std::uint32_t first)
    -> std::uint32_t
{
  return op(3, address << 16 | count << 8 | first);
}

constexpr auto wait_us(std::uint32_t us) -> std::uint32_t
{
  return op(4, us);
}

constexpr auto reset_op() -> std::uint32_t
{
  return op(5);
}

constexpr auto rcc_op() -> std::uint32_t
{
  return op(6);
}

constexpr auto stamp_op() -> std::uint32_t
{
  return op(7);
}

constexpr auto repeat(std::uint32_t count, std::uint32_t to) -> std::uint32_t
{
  return op(8, count << 8 | to);
}

constexpr auto kick(std::uint32_t address, std::uint32_t byte) -> std::uint32_t
{
  return op(9, address << 16 | byte);
}

constexpr auto stall_us(std::uint32_t us) -> std::uint32_t
{
  return op(10, us);
}

constexpr auto own_op(std::uint32_t address) -> std::uint32_t
{
  return op(11, address);
}

constexpr auto enable_op() -> std::uint32_t
{
  return op(12);
}

constexpr auto disable_op() -> std::uint32_t
{
  return op(13);
}

constexpr auto written(std::uint32_t address, std::uint32_t sent, bool nack = false)
    -> std::uint32_t
{
  return 0x5700'0000u | (nack ? 1u : 0u) << 16 | address << 8 | sent;
}

constexpr auto got(std::uint32_t byte) -> std::uint32_t
{
  return 0x5D00'0000u | byte;
}

constexpr auto read_done(std::uint32_t address, bool nack = false) -> std::uint32_t
{
  return 0x5E00'0000u | (nack ? 1u : 0u) << 16 | address << 8;
}

constexpr auto addressed(std::uint32_t address, bool read = false) -> std::uint32_t
{
  return 0xADD0'0000u | address << 1 | (read ? 1u : 0u);
}

constexpr auto data(std::uint32_t byte) -> std::uint32_t
{
  return 0xDA7A'0000u | byte;
}

constexpr std::uint32_t stopped   = 0x5709u;
constexpr std::uint32_t nacked    = 0x4AC0u;
constexpr std::uint32_t timed_out = 0x7140u;

void rcc_pulse(core::machine& target)
{
  auto const& desc  = target.peripheral("RCC").description();
  auto const* found = popcorn::find_register(desc, "RCC_APB1RSTR1");
  REQUIRE(found != nullptr);
  auto const field =
      std::ranges::find(found->fields, std::string_view{"I2C1RST"}, &popcorn::field_desc::name);
  REQUIRE(field != found->fields.end());
  REQUIRE(target.mem().write(desc.base + found->offset, 4, 1u << field->lsb).has_value());
  REQUIRE(target.mem().write(desc.base + found->offset, 4, 0).has_value());
}

auto model_refused(core::machine& target) -> std::uint64_t
{
  return static_cast<std::uint64_t>(
      popcorn::testing::saved_integer(target, "I2C1", "refused").value_or(0));
}

class recording_link final : public core::member_link
{
public:
  explicit recording_link(std::shared_ptr<core::member_link> inner) : inner_{std::move(inner)}
  {}

  [[nodiscard]] auto sender() const noexcept -> std::size_t override
  {
    return inner_->sender();
  }

  [[nodiscard]] auto receiver() const noexcept -> std::size_t override
  {
    return inner_->receiver();
  }

  [[nodiscard]] auto latency() const noexcept -> core::sys_time override
  {
    return inner_->latency();
  }

  void enter_window(std::uint64_t window, core::time_map sender) override
  {
    inner_->enter_window(window, sender);
  }

  void sender_reset(core::sys_time at) override
  {
    sender_resets.push_back(at);
    inner_->sender_reset(at);
  }

  void receiver_reset(core::sys_time at) override
  {
    receiver_resets.push_back(at);
    inner_->receiver_reset(at);
  }

  auto drain(
      std::uint64_t         window,
      core::sys_time const& barrier,
      core::sys_time const& next,
      core::time_map        receiver) -> std::optional<core::link_fault> override
  {
    return inner_->drain(window, barrier, next, receiver);
  }

  [[nodiscard]] auto quiet() const -> bool override
  {
    return inner_->quiet();
  }

  void finish(core::sys_time at) override
  {
    inner_->finish(at);
  }

  void save(std::vector<std::byte>& out) const override
  {
    inner_->save(out);
  }

  void load(std::span<std::byte const> in) override
  {
    inner_->load(in);
  }

  [[nodiscard]] auto inner() const -> core::member_link&
  {
    return *inner_;
  }

  std::vector<core::sys_time> sender_resets;
  std::vector<core::sys_time> receiver_resets;

private:
  std::shared_ptr<core::member_link> inner_;
};

class quiet_link final : public core::member_link
{
public:
  quiet_link(std::size_t from, std::size_t to, std::int64_t ps) : from_{from}, to_{to}, ps_{ps}
  {}

  [[nodiscard]] auto sender() const noexcept -> std::size_t override
  {
    return from_;
  }

  [[nodiscard]] auto receiver() const noexcept -> std::size_t override
  {
    return to_;
  }

  [[nodiscard]] auto latency() const noexcept -> core::sys_time override
  {
    return {ps_};
  }

  void enter_window(std::uint64_t, core::time_map) override
  {}

  void sender_reset(core::sys_time) override
  {}

  void receiver_reset(core::sys_time) override
  {}

  auto drain(std::uint64_t, core::sys_time const&, core::sys_time const&, core::time_map)
      -> std::optional<core::link_fault> override
  {
    return std::nullopt;
  }

  [[nodiscard]] auto quiet() const -> bool override
  {
    return true;
  }

  void finish(core::sys_time) override
  {}

  void save(std::vector<std::byte>&) const override
  {}

  void load(std::span<std::byte const>) override
  {}

private:
  std::size_t  from_;
  std::size_t  to_;
  std::int64_t ps_;
};

struct target_spec
{
  std::uint32_t              own{0x42};
  std::uint32_t              mode{mode_target};
  std::uint32_t              timeout{};
  std::uint32_t              first_tx{};
  std::vector<std::uint32_t> script{};
};

struct node
{
  stm32::board                   board{};
  std::unique_ptr<stm32_fixture> f;
  core::machine*                 target{};
  stm32::i2c_link_port*          port{};
  std::int64_t                   origin{};

  node(
      std::uint32_t                                    entry,
      std::vector<std::uint32_t> const&                script,
      target_spec const&                               spec  = {},
      std::vector<std::shared_ptr<periph::i2c_target>> local = {})
  {
    if(not local.empty())
    {
      board.i2c["I2C1"] = std::move(local);
    }
    auto options   = popcorn::testing::stm32_options();
    options.models = stm32::factories(board, u575());
    f              = std::make_unique<stm32_fixture>(u575(), "i2c_ll", std::move(options));
    target         = f->target.get();
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("i2c_ll", "main")})
            .reason
        == stop_reason::address);
    auto const put = [&](popcorn::address at, std::uint32_t value) {
      REQUIRE(target->mem().write(at, 4, value).has_value());
    };
    REQUIRE(script.size() < 64);
    put(area, magic);
    put(area + 4, entry);
    put(area + 8, 0);
    put(area + 12, 0);
    put(area + 16, 0);
    put(area + 20, spec.first_tx);
    put(area + 24, spec.own);
    put(area + 28, spec.mode);
    put(area + 32, spec.timeout);
    put(area + 36, 0);
    for(std::size_t index = 0; index < 64; ++index)
    {
      put(area + 40 + 4 * static_cast<popcorn::address>(index),
          index < script.size() ? script[index] : 0);
    }
    f->set("pc", sym("i2c_ll", "link_run"));
    origin = static_cast<std::int64_t>(target->virtual_clock().time().ps);
    port   = f->model_of<stm32::i2c_link_port>("I2C1");
    REQUIRE(port != nullptr);
  }

  [[nodiscard]] auto log() const -> std::vector<std::uint32_t>
  {
    auto const                 count = word(area + 12);
    std::vector<std::uint32_t> out;
    for(std::uint32_t index = 0; index < count and index < 256; ++index)
    {
      out.push_back(word(area + log_at + 4 * index));
    }
    return out;
  }

  [[nodiscard]] auto word(popcorn::address at) const -> std::uint32_t
  {
    auto const value = target->mem().read(at, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }
};

struct variant
{
  std::string_view             name;
  std::int64_t                 lowered{};
  core::slice_bounds           bounds{};
  std::optional<std::uint32_t> seed{};
};

auto variants() -> std::vector<variant>
{
  core::slice_bounds const fine{
      .max_instructions = icount{997}, .wall_budget = std::chrono::microseconds{10}};
  return {
      {.name = "q=l"},
      {.name = "q=l/2", .lowered = 2},
      {.name = "997 q=l/3", .lowered = 3, .bounds = fine},
      {.name = "sleeps q=l/2", .lowered = 2, .seed = 1},
  };
}

auto two_q() -> std::vector<variant>
{
  return {
      {.name = "q=l"},
      {.name = "q=l/2", .lowered = 2}
  };
}

auto eeprom() -> std::shared_ptr<devices::eeprom_24xx>
{
  return std::make_shared<devices::eeprom_24xx>(devices::eeprom_24xx_options{
      .address       = 0x50,
      .size          = 4096,
      .address_bytes = 2,
      .page_size     = 16,
      .write_cycle   = std::chrono::microseconds{50}});
}

struct world
{
  std::vector<std::unique_ptr<node>>              nodes;
  std::shared_ptr<stm32::i2c_medium>              medium;
  std::vector<std::shared_ptr<core::member_link>> links;
  std::vector<core::member_ref>                   members;
  std::unique_ptr<core::system>                   sys;
  std::vector<stm32::i2c_tap_phase>               taps;
  std::vector<std::shared_ptr<recording_link>>    recorded;
  std::int64_t                                    l{latency};
  std::int64_t                                    q{latency};

  world(
      std::vector<std::uint32_t> const&                master,
      std::vector<target_spec> const&                  targets,
      std::int64_t                                     link_latency = latency,
      std::vector<std::shared_ptr<periph::i2c_target>> local        = {})
      : l{link_latency}
  {
    nodes.push_back(std::make_unique<node>(controller, master, target_spec{}, std::move(local)));
    std::vector<stm32::i2c_target_end> ends;
    for(auto const& [index, spec] : std::views::enumerate(targets))
    {
      nodes.push_back(std::make_unique<node>(target_fw, spec.script, spec));
      ends.push_back(
          {.member     = static_cast<std::size_t>(index) + 1,
           .port       = nodes.back()->port,
           .forth_lane = 1,
           .back_lane  = static_cast<std::uint32_t>(index) + 1});
    }
    medium = std::make_shared<stm32::i2c_medium>(0, *nodes[0]->port, core::sys_time{l}, ends);
    medium->attach_controller();
    for(std::size_t index = 0; index < targets.size(); ++index)
    {
      medium->attach_target(index);
    }
    for(auto const& link : medium->links())
    {
      recorded.push_back(std::make_shared<recording_link>(link));
      links.push_back(recorded.back());
    }
  }

  void host(variant const& how)
  {
    q = l;
    if(how.lowered != 0)
    {
      q = l / how.lowered;
      links.push_back(std::make_shared<quiet_link>(0, 1, q));
    }
    for(auto& one : nodes)
    {
      std::vector<std::unique_ptr<core::machine>> machines;
      machines.push_back(std::move(one->f->target));
      members.push_back(
          {.on   = std::make_shared<core::host>(std::move(machines)),
           .name = std::format("m{}", members.size())});
    }
    std::move_only_function<void(std::size_t)> hook;
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
    sys = std::make_unique<core::system>(members, links, how.bounds, std::move(hook), nullptr);
    std::vector<core::sys_time> offsets(nodes.size());
    auto const                  held = sys->lock();
    sys->start(held, offsets);
  }

  auto resume(std::int64_t until) -> core::pause
  {
    auto const pending = [&] {
      auto const held = sys->lock();
      return sys->resume(held, core::sys_time{until});
    }();
    return pending.get();
  }

  auto run_to(std::int64_t until) -> core::pause
  {
    for(;;)
    {
      auto const stopped = resume(until);
      if(stopped.reason != core::pause_reason::delivered)
      {
        take_taps();
        return stopped;
      }
      auto const held = sys->lock();
      sys->release(held, *stopped.member, true);
    }
  }

  void take_taps()
  {
    if(medium)
    {
      auto taken = medium->tap().take();
      taps.insert(taps.end(), taken.begin(), taken.end());
    }
  }

  auto idle() -> core::pause
  {
    std::vector<core::host_future<core::goal_reached>> goals;
    auto const                                         pending = [&] {
      auto const held = sys->lock();
      for(std::size_t member = 0; member < members.size(); ++member)
      {
        goals.push_back(sys->post_goal(held, member, {.until = popcorn::address{0x0800'0001}}));
      }
      return sys->resume(held);
    }();
    auto const stopped = pending.get();
    take_taps();
    return stopped;
  }

  template <typename F> auto on(std::size_t member, F&& fn)
  {
    return core::access(*members[member].on, 0, std::forward<F>(fn));
  }

  void reset(std::size_t member, popcorn::reset_kind kind = popcorn::reset_kind::software)
  {
    auto const held = sys->lock();
    sys->reset(held, member, kind);
  }

  void rcc(std::size_t member)
  {
    on(member, [](core::machine& target) { rcc_pulse(target); });
  }

  [[nodiscard]] auto refused() -> std::uint64_t
  {
    auto total = medium->refused();
    for(std::size_t member = 0; member < nodes.size(); ++member)
    {
      total += on(member, [](core::machine& target) { return model_refused(target); });
    }
    return total;
  }

  [[nodiscard]] auto sorted_taps() const -> std::vector<stm32::i2c_tap_phase>
  {
    auto out = taps;
    std::ranges::sort(out, {}, [](stm32::i2c_tap_phase const& phase) {
      return std::pair{phase.target, phase.unit};
    });
    return out;
  }

  [[nodiscard]] auto phases_of(std::size_t target) const -> std::vector<stm32::i2c_tap_phase>
  {
    std::vector<stm32::i2c_tap_phase> out;
    for(auto const& phase : sorted_taps())
    {
      if(phase.target == target)
      {
        out.push_back(phase);
      }
    }
    return out;
  }

  [[nodiscard]] auto log(std::size_t member) -> std::vector<std::uint32_t>
  {
    return on(member, [&](core::machine&) { return nodes[member]->log(); });
  }

  [[nodiscard]] auto dump() -> std::string
  {
    std::string out;
    for(std::size_t member = 0; member < nodes.size(); ++member)
    {
      out += std::format("m{}:", member);
      for(auto const word : log(member))
      {
        out += std::format(" {:x}", word);
      }
      out += "\n";
    }
    for(auto const& phase : sorted_taps())
    {
      out += std::format(
          "t{} u{} {} {:x} ack {} at {} answer {} {}\n", phase.target, phase.unit, phase.phase,
          phase.byte, phase.ack, phase.at.ps, phase.answer.ps, phase.dropped ? "dropped" : "");
    }
    out += std::format("refused {}\n", refused());
    for(auto const& link : recorded)
    {
      for(auto const& at : link->sender_resets)
      {
        out += std::format("sender reset {}\n", at.ps);
      }
      for(auto const& at : link->receiver_resets)
      {
        out += std::format("receiver reset {}\n", at.ps);
      }
    }
    for(std::size_t index = 0; index < medium->target_count(); ++index)
    {
      out += std::format("owed {} tickets {}\n", medium->owed(index), medium->tickets(index));
    }
    return out;
  }
};

struct spi_kind
{
  std::string kind;
};

void keep_dump(std::string_view name, std::string const& bytes)
{
  if(auto const* dir = std::getenv("POPCORN_SYSTEM_DUMP"); dir != nullptr)
  {
    std::string file{name};
    std::ranges::replace(file, ' ', '_');
    std::ranges::replace(file, '/', '_');
    std::ofstream{std::string{dir} + "/" + file + ".txt", std::ios::binary} << bytes;
  }
}

using build_fn  = std::copyable_function<world() const>;
using build_ref = std::function_ref<world() const>;
using check_fn  = std::function_ref<void(world&)>;

void no_check(world&)
{}

struct step
{
  std::int64_t                               at{};
  std::copyable_function<void(world&) const> act;
};

auto same_everywhere(
    std::string_view         name,
    build_ref                build,
    std::int64_t             end,
    check_fn                 check = no_check,
    std::vector<step> const& steps = {},
    std::vector<variant>     ways  = variants()) -> std::string
{
  std::optional<std::string> reference;
  for(auto const& how : ways)
  {
    CAPTURE(how.name);
    auto w = build();
    w.host(how);
    for(auto const& one : steps)
    {
      REQUIRE(w.run_to(one.at).reason == core::pause_reason::system_time);
      one.act(w);
    }
    REQUIRE(w.run_to(end).reason == core::pause_reason::system_time);
    auto const seen = w.dump();
    keep_dump(std::format("i2c {} {}", name, how.name), seen);
    check(w);
    if(reference)
    {
      CHECK(seen == *reference);
    }
    else
    {
      reference = seen;
    }
  }
  return *reference;
}

constexpr std::int64_t row_end = 2'000 * us_ps;

auto ceil_after(std::int64_t at, std::int64_t q) -> std::int64_t
{
  return (at / q + 1) * q;
}

auto handed_by(std::int64_t stamp, std::int64_t at, std::int64_t l, std::int64_t q) -> bool
{
  return ceil_after(stamp + l - q, q) < at;
}

auto drained_by(std::int64_t stamp, std::int64_t at, std::int64_t q) -> bool
{
  return ceil_after(stamp, q) < at;
}

auto try_pick(std::int64_t from, std::int64_t to, std::function_ref<bool(std::int64_t)> fits)
    -> std::optional<std::int64_t>
{
  for(auto at = from + us_ps / 8; at < to; at += us_ps / 8)
  {
    if(fits(at))
    {
      return at;
    }
  }
  return std::nullopt;
}

auto pick(std::int64_t from, std::int64_t to, std::function_ref<bool(std::int64_t)> fits)
    -> std::int64_t
{
  auto const found = try_pick(from, to, fits);
  REQUIRE(found.has_value());
  return *found;
}

auto single(std::vector<std::uint32_t> master, target_spec spec = {}, std::int64_t l = latency)
    -> build_fn
{
  return [master, spec, l] { return world{master, {spec}, l}; };
}

auto reference(build_ref build, std::int64_t end = row_end) -> std::vector<stm32::i2c_tap_phase>
{
  auto w = build();
  w.host({.name = "reference"});
  REQUIRE(w.run_to(end).reason == core::pause_reason::system_time);
  return w.phases_of(0);
}

void settled(world& w)
{
  for(std::size_t index = 0; index < w.medium->target_count(); ++index)
  {
    CHECK(w.medium->owed(index) == 0);
    CHECK(w.medium->tickets(index) == 0);
  }
}

void one_answer_each(world& w, std::size_t target, std::size_t phases)
{
  auto const seen = w.phases_of(target);
  REQUIRE(seen.size() == phases);
  for(std::size_t index = 1; index < seen.size(); ++index)
  {
    CHECK(seen[index].unit > seen[index - 1].unit);
  }
  settled(w);
}

auto phase_length(stm32::i2c_tap_phase const& phase) -> std::int64_t
{
  return phase.phase == periph::i2c_phase::address ? address_ps : byte_ps;
}

auto stretches(stm32::i2c_tap_phase const& phase) -> bool
{
  return phase.phase == periph::i2c_phase::address and phase.ack;
}

void exact(world& w, std::size_t target)
{
  for(auto const& phase : w.phases_of(target))
  {
    CAPTURE(phase.unit);
    CHECK_FALSE(phase.dropped);
    auto const end = phase.at.ps + w.l + phase_length(phase);
    if(stretches(phase))
    {
      CHECK(phase.answer.ps > end);
      CHECK(phase.answer.ps < end + 20 * us_ps);
    }
    else
    {
      CHECK(phase.answer.ps == end);
    }
  }
}
}

TEST_CASE("two remote I2C targets and a local EEPROM each get their own writes and reads")
{
  auto const build = [] {
    return world{
        {wait_us(50), write(0x42, 2, 0x10), write(0x43, 2, 0x20), write(0x50, 4, 0x00),
         wait_us(100), write_sr(0x50, 2, 0x00), read(0x50, 2), read(0x42, 2), read(0x43, 2)},
        {target_spec{.own = 0x42}, target_spec{.own = 0x43, .first_tx = 0xC0}},
        latency,
        {eeprom()}
    };
  };
  same_everywhere("three targets", build, 4'000 * us_ps, [](world& w) {
    CHECK(
        w.log(0)
        == std::vector<std::uint32_t>{
            boot, written(0x42, 2), written(0x43, 2), written(0x50, 4), written(0x50, 2), got(0x02),
            got(0x03), read_done(0x50), got(0xB0), got(0xB1), read_done(0x42), got(0xC0), got(0xC1),
            read_done(0x43)});
    CHECK(
        w.log(1)
        == std::vector<std::uint32_t>{
            boot, addressed(0x42), data(0x10), data(0x11), stopped, addressed(0x42, true), nacked,
            stopped});
    CHECK(
        w.log(2)
        == std::vector<std::uint32_t>{
            boot, addressed(0x43), data(0x20), data(0x21), stopped, addressed(0x43, true), nacked,
            stopped});
    for(std::size_t target = 0; target < 2; ++target)
    {
      exact(w, target);
    }
    CHECK(w.refused() == 0);
    settled(w);
  });
}

namespace
{
auto basic(std::uint32_t wait = 50) -> std::vector<std::uint32_t>
{
  return {wait_us(wait), write(0x42, 2, 0x10), read(0x42, 2)};
}

auto stop_stamp(stm32::i2c_tap_phase const& last, std::int64_t l) -> std::int64_t
{
  return last.answer.ps + l + period_ps;
}

auto contains(std::vector<std::uint32_t> const& log, std::uint32_t word) -> bool
{
  return std::ranges::contains(log, word);
}

auto nacked_write(std::vector<std::uint32_t> const& log, std::uint32_t address) -> bool
{
  return std::ranges::any_of(log, [address](std::uint32_t word) {
    return (word & 0xFFFF'FF00u) == (written(address, 0, true) & 0xFFFF'FF00u);
  });
}

struct split
{
  build_fn     build;
  std::int64_t stamp{};
  std::int64_t at{};
};

auto split_by_q(
    std::function_ref<build_fn(std::uint32_t)>                                make,
    std::function_ref<std::int64_t(std::vector<stm32::i2c_tap_phase> const&)> stamp_of,
    std::int64_t end = row_end) -> split
{
  for(std::uint32_t wait = 50; wait < 90; ++wait)
  {
    auto const build = make(wait);
    auto const s     = stamp_of(reference(build, end));
    auto const at    = try_pick(s, s + latency, [&](std::int64_t x) {
      return handed_by(s, x, latency, latency) and not handed_by(s, x, latency, latency / 2);
    });
    if(at)
    {
      return {.build = build, .stamp = s, .at = *at};
    }
  }
  FAIL("no wait puts the reset between the drains of Q = L and Q = L/2");
  return {};
}

auto first_stamp(std::vector<stm32::i2c_tap_phase> const& seen) -> std::int64_t
{
  REQUIRE_FALSE(seen.empty());
  return seen[0].at.ps;
}

auto stop_after_write(std::vector<stm32::i2c_tap_phase> const& seen) -> std::int64_t
{
  REQUIRE(seen.size() >= 2);
  return stop_stamp(seen[1], latency);
}

auto two_writes(std::uint32_t pause) -> std::copyable_function<build_fn(std::uint32_t) const>
{
  return [pause](std::uint32_t wait) {
    return single({wait_us(wait), write(0x42, 1, 0x10), wait_us(pause), write(0x42, 1, 0x20)});
  };
}
}

TEST_CASE("I1: a target RCC reset under a phase answers it undriven at the phase end")
{
  auto const build = single(basic());
  auto const t     = reference(build)[1].at.ps;
  same_everywhere(
      "I1", build, row_end,
      [&](world& w) {
        one_answer_each(w, 0, 3);
        auto const seen = w.phases_of(0);
        CHECK(seen[0].ack);
        CHECK(seen[1].dropped);
        CHECK_FALSE(seen[1].ack);
        CHECK(seen[1].answer.ps == t + w.l + byte_ps);
        CHECK_FALSE(seen[2].ack);
        CHECK(
            w.log(0)
            == std::vector<std::uint32_t>{boot, written(0x42, 2, true), read_done(0x42, true)}
             );
        CHECK(w.refused() == 0);
  },
      {{.at = t + latency + byte_ps / 2, .act = [](world& w) { w.rcc(1); }}});
}

TEST_CASE(
    "I1b: a START or STOP handed before a target RCC reset and due after it runs in the fresh I2C")
{
  SUBCASE("a START")
  {
    auto const r = split_by_q([](std::uint32_t wait) { return single(basic(wait)); }, first_stamp);
    same_everywhere(
        "I1b start", r.build, row_end,
        [&](world& w) {
          auto const seen = w.phases_of(0);
          REQUIRE(seen.size() == 2);
          CHECK_FALSE(seen[0].dropped);
          CHECK_FALSE(seen[0].ack);
          CHECK(seen[0].answer.ps == r.stamp + w.l + address_ps);
          CHECK(w.log(1) == std::vector<std::uint32_t>{boot}
            );
          settled(w);
    },
        {{.at = r.at, .act = [](world& w) { w.rcc(1); }}});
  }
  SUBCASE("a STOP")
  {
    auto const r = split_by_q(two_writes(400), stop_after_write);
    same_everywhere(
        "I1b stop", r.build, row_end,
        [&](world& w) {
          CHECK(w.log(1) == std::vector<std::uint32_t>{boot, addressed(0x42), data(0x10)}
            );
          CHECK(w.phases_of(0).size() == 3);
          settled(w);
    },
        {{.at = r.at, .act = [](world& w) { w.rcc(1); }}});
  }
}

TEST_CASE("I2: a target member reset under a running phase answers it at the phase end")
{
  auto const build = single(basic());
  auto const t     = reference(build)[0].at.ps;
  same_everywhere(
      "I2", build, row_end,
      [&](world& w) {
        auto const seen = w.phases_of(0);
        REQUIRE(seen.size() == 2);
        CHECK(seen[0].dropped);
        CHECK(seen[0].answer.ps == t + w.l + address_ps);
        REQUIRE(w.recorded[0]->receiver_resets.size() == 1);
        CHECK(w.recorded[0]->receiver_resets[0].ps == t + latency + address_ps / 2);
        settled(w);
  },
      {{.at = t + latency + address_ps / 2, .act = [](world& w) { w.reset(1); }}});
}

TEST_CASE("I3: a target member reset before a queued phase starts answers it at the phase end")
{
  auto const build = single(basic());
  auto const t     = reference(build)[0].at.ps;
  auto const at    = pick(t, t + latency, [&](std::int64_t x) {
    return std::ranges::all_of(std::array{latency, latency / 2, latency / 3}, [&](auto q) {
      return handed_by(t, x, latency, q);
    });
  });
  same_everywhere(
      "I3", build, row_end,
      [&](world& w) {
        auto const seen = w.phases_of(0);
        REQUIRE_FALSE(seen.empty());
        CHECK(seen[0].dropped);
        CHECK(seen[0].answer.ps == t + w.l + address_ps);
        settled(w);
  },
      {{.at = at, .act = [](world& w) { w.reset(1); }}});
}

TEST_CASE("I4 and I5: a phase still in the link at a target reset is answered at its far-side end")
{
  auto const build = single(basic());
  auto const t     = reference(build)[0].at.ps;
  auto const check = [&](world& w) {
    auto const seen = w.phases_of(0);
    REQUIRE_FALSE(seen.empty());
    CHECK(seen[0].dropped);
    CHECK(seen[0].answer.ps == t + w.l + address_ps);
    settled(w);
  };
  SUBCASE("I4, in the mailbox at every Q")
  {
    auto const at = pick(t, t + us_ps, [&](std::int64_t x) {
      return std::ranges::none_of(std::array{latency, latency / 2, latency / 3}, [&](auto q) {
        return drained_by(t, x, q);
      });
    });
    same_everywhere(
        "I4", build, row_end, check,
        {
            {.at = at, .act = [](world& w) { w.reset(1); }}
    });
  }
  SUBCASE("I5, carried past a drain at L above Q")
  {
    auto const at = pick(t, t + latency, [&](std::int64_t x) {
      return drained_by(t, x, latency / 2) and not handed_by(t, x, latency, latency / 2);
    });
    same_everywhere(
        "I5", build, row_end, check,
        {
            {.at = at, .act = [](world& w) { w.reset(1); }}
    });
  }
}

TEST_CASE("I6: a data phase stamped after a target reset reaches the fresh I2C unaddressed")
{
  auto const build = single({wait_us(50), stall_us(800), write(0x42, 2, 0x10)});
  auto const seen  = reference(build);
  REQUIRE(seen.size() >= 2);
  auto const reset = seen[0].answer.ps + 10 * us_ps;
  auto const t     = seen[1].at.ps;
  REQUIRE(t > reset + 600 * us_ps);
  same_everywhere(
      "I6", build, row_end,
      [&](world& w) {
        auto const phases = w.phases_of(0);
        REQUIRE(phases.size() == 2);
        CHECK(phases[0].ack);
        CHECK_FALSE(phases[1].dropped);
        CHECK_FALSE(phases[1].ack);
        CHECK(phases[1].answer.ps == t + w.l + byte_ps);
        CHECK(nacked_write(w.log(0), 0x42));
        CHECK(w.log(1) == std::vector<std::uint32_t>{boot, addressed(0x42), boot}
          );
        settled(w);
  },
      {{.at = reset, .act = [](world& w) { w.reset(1); }}});
}

TEST_CASE("I7: a START stamped after a target reset reaches the fresh I2C, which acknowledges it")
{
  auto const build = single({wait_us(1'000), write(0x42, 1, 0x10)});
  same_everywhere(
      "I7", build, 3'000 * us_ps,
      [&](world& w) {
        auto const seen = w.phases_of(0);
        REQUIRE(seen.size() == 2);
        CHECK(seen[0].ack);
        CHECK(seen[1].ack);
        CHECK(
            w.log(1)
            == std::vector<std::uint32_t>{boot, boot, addressed(0x42), data(0x10), stopped}
             );
        exact(w, 0);
        settled(w);
  },
      {{.at = 100 * us_ps, .act = [](world& w) { w.reset(1); }}});
}

TEST_CASE("I8: a STOP stamped before a target reset, in the link or in the port, leaves it idle")
{
  auto const r = split_by_q(two_writes(800), stop_after_write, 3'000 * us_ps);
  same_everywhere(
      "I8", r.build, 3'000 * us_ps,
      [&](world& w) {
        CHECK(
            w.log(1)
            == std::vector<std::uint32_t>{
                boot, addressed(0x42), data(0x10), boot, addressed(0x42), data(0x20), stopped}
             );
        CHECK(w.phases_of(0).size() == 4);
        settled(w);
  },
      {{.at = r.at, .act = [](world& w) { w.reset(1); }}});
}

TEST_CASE("I9: a controller member reset mid-phase sends a STOP, refuses the late answer once")
{
  auto const build = single({wait_us(50), write(0x42, 1, 0x10), write(0x42, 1, 0x20)});
  auto const t     = reference(build)[0].at.ps;
  SUBCASE("the STOP cuts the target's phase, its answer stamped after t_m is refused")
  {
    auto const at = t + address_ps / 2;
    same_everywhere(
        "I9 cut", build, row_end,
        [&](world& w) {
          auto const seen = w.phases_of(0);
          REQUIRE(seen.size() == 3);
          CHECK(seen[0].dropped);
          CHECK(seen[0].answer.ps == at + w.l);
          CHECK(seen[1].ack);
          CHECK(w.refused() == 1);
          REQUIRE(w.recorded[0]->sender_resets.size() == 1);
          CHECK(w.recorded[0]->sender_resets[0].ps == at);
          CHECK(w.log(1) == std::vector<std::uint32_t>{boot, addressed(0x42), data(0x20), stopped}
            );
          settled(w);
    },
        {{.at = at, .act = [](world& w) { w.reset(0); }}});
  }
  SUBCASE("an answer stamped before t_m is dropped or wiped and not counted")
  {
    auto const answer = reference(build)[0].answer.ps;
    auto const at     = answer + latency / 2 + us_ps / 4;
    same_everywhere(
        "I9 late", build, row_end,
        [&](world& w) {
          auto const seen = w.phases_of(0);
          REQUIRE(seen.size() == 3);
          CHECK_FALSE(seen[0].dropped);
          CHECK(seen[0].ack);
          CHECK(w.refused() == 0);
          CHECK(contains(w.log(1), data(0x20)));
          settled(w);
    },
        {{.at = at, .act = [](world& w) { w.reset(0); }}});
  }
}

TEST_CASE("I9: a controller firmware reset mid-transfer reaches the target again")
{
  auto const build =
      single({wait_us(50), kick(0x42, 0x10), wait_us(30), reset_op(), write(0x42, 1, 0x20)});
  same_everywhere("I9 firmware", build, row_end, [&](world& w) {
    REQUIRE(w.recorded[0]->sender_resets.size() == 1);
    CHECK(w.refused() == 1);
    CHECK(contains(w.log(1), data(0x20)));
    CHECK(w.log(0) == std::vector<std::uint32_t>{boot, boot | 4, written(0x42, 1)});
    settled(w);
  });
}

TEST_CASE("I10: a controller RCC reset refuses the kept answer once at every Q")
{
  build_fn                    build;
  std::optional<std::int64_t> at;
  std::int64_t                answer = 0;
  for(std::uint32_t wait = 50; wait < 70 and not at; ++wait)
  {
    build  = single({wait_us(wait), write(0x42, 1, 0x10)});
    answer = reference(build)[0].answer.ps;
    at     = try_pick(answer, answer + latency, [&](std::int64_t x) {
      return handed_by(answer, x, latency, latency)
         and not handed_by(answer, x, latency, latency / 2);
    });
  }
  REQUIRE(at.has_value());
  same_everywhere(
      "I10", build, row_end,
      [&](world& w) {
        auto const seen = w.phases_of(0);
        REQUIRE(seen.size() == 1);
        CHECK_FALSE(seen[0].dropped);
        CHECK(seen[0].ack);
        CHECK(w.refused() == 1);
        CHECK(w.recorded[0]->sender_resets.empty());
        settled(w);
  },
      {{.at = *at, .act = [](world& w) { w.rcc(0); }}});
}

TEST_CASE("I11 and I12: controller and target member resets in one window give one answer per unit")
{
  auto const build = single(basic());
  auto const t     = reference(build)[0].at.ps;
  for(bool const controller_first : {true, false})
  {
    CAPTURE(controller_first);
    auto const first  = t + address_ps / 2;
    auto const second = first + 2 * us_ps;
    same_everywhere(
        controller_first ? "I11" : "I12", build, row_end,
        [&](world& w) {
          auto const seen = w.phases_of(0);
          REQUIRE_FALSE(seen.empty());
          CHECK(seen[0].dropped);
          CHECK_FALSE(seen[0].ack);
          CHECK(seen[0].answer.ps == t + w.l + address_ps);
          REQUIRE(w.recorded[0]->sender_resets.size() == 1);
          REQUIRE(w.recorded[0]->receiver_resets.size() == 1);
          CHECK(
              (w.recorded[0]->sender_resets[0] < w.recorded[0]->receiver_resets[0])
              == controller_first);
          CHECK(w.refused() == 1);
          settled(w);
    },
        {{.at = first, .act = [controller_first](world& w) { w.reset(controller_first ? 0 : 1); }},
         {.at  = second,
          .act = [controller_first](world& w) { w.reset(controller_first ? 1 : 0); }}});
  }
}

TEST_CASE("I11b: a controller STOP handed before a later target reset keeps the dropped answer")
{
  auto const base = latency / 3;
  auto const half = latency / 6;
  auto const gap  = [&](std::int64_t x) {
    return x + latency - half - ceil_after(x + latency - base, base);
  };
  build_fn     build;
  std::int64_t t  = 0;
  std::int64_t tm = 0;
  for(std::uint32_t wait = 50; wait < 60 and gap(tm) <= us_ps; ++wait)
  {
    build = single(basic(wait));
    t     = reference(build)[0].at.ps;
    tm    = t + us_ps / 8;
    for(auto x = tm; x < t + address_ps - 2 * us_ps; x += us_ps / 8)
    {
      tm = gap(x) > gap(tm) ? x : tm;
    }
  }
  REQUIRE(gap(tm) > us_ps);
  auto const ts = ceil_after(tm + latency - base, base) + gap(tm) / 2;
  same_everywhere(
      "I11b", build, row_end,
      [&](world& w) {
        auto const seen = w.phases_of(0);
        REQUIRE_FALSE(seen.empty());
        CHECK(seen[0].dropped);
        REQUIRE(w.recorded[0]->receiver_resets.size() == 1);
        REQUIRE(w.recorded[0]->sender_resets.size() == 1);
        CHECK(w.recorded[0]->sender_resets[0].ps + w.l < t + w.l + address_ps);
        CHECK(
            seen[0].answer.ps
            == std::max(w.recorded[0]->receiver_resets[0].ps, t + w.l + address_ps));
        settled(w);
  },
      {{.at = tm, .act = [](world& w) { w.reset(0); }},
       {.at = ts, .act = [](world& w) { w.reset(1); }}},
      {{.name = "q=l/3", .lowered = 3}, {.name = "q=l/6", .lowered = 6}});
}

TEST_CASE("I13: two target resets before the re-armed entry fires give one answer")
{
  auto const build = single(basic());
  auto const t     = reference(build)[0].at.ps;
  same_everywhere(
      "I13", build, row_end,
      [&](world& w) {
        auto const seen = w.phases_of(0);
        REQUIRE_FALSE(seen.empty());
        CHECK(seen[0].dropped);
        REQUIRE(w.recorded[0]->receiver_resets.size() == 2);
        CHECK(
            seen[0].answer.ps
            == std::max(w.recorded[0]->receiver_resets[1].ps, t + w.l + address_ps));
        settled(w);
  },
      {{.at = t + latency + us_ps, .act = [](world& w) { w.reset(1); }},
       {.at = t + latency + 3 * us_ps, .act = [](world& w) { w.reset(1); }}});
}

TEST_CASE(
    "unknown addresses get NACK, a general call reaches every target, OAR1 changes at run time")
{
  auto const build = [] {
    return world{
        {wait_us(50), write(0x2A, 1, 0x10), write(0x00, 1, 0x33), wait_us(300),
         write(0x44, 1, 0x55), write(0x42, 1, 0x66)},
        {target_spec{.own = 0x42, .script = {wait_us(400), own_op(0x44)}},
         target_spec{.own = 0x43}}
    };
  };
  same_everywhere("addresses", build, 3'000 * us_ps, [](world& w) {
    CHECK(
        w.log(0)
        == std::vector<std::uint32_t>{
            boot, written(0x2A, 0, true), written(0x00, 1), written(0x44, 1),
            written(0x42, 0, true)});
    CHECK(
        w.log(1)
        == std::vector<std::uint32_t>{
            boot, addressed(0x00), data(0x33), stopped, addressed(0x44), data(0x55), stopped});
    CHECK(w.log(2) == std::vector<std::uint32_t>{boot, addressed(0x00), data(0x33), stopped});
    exact(w, 0);
    exact(w, 1);
    settled(w);
  });
}

TEST_CASE("a target with PE clear, in controller mode or held in reset NACKs, decided at t + L")
{
  SUBCASE("PE clear, controller mode, held in reset")
  {
    auto const build = [] {
      return world{
          {wait_us(50), write(0x42, 1, 0x10), write(0x43, 1, 0x11), write(0x44, 1, 0x12)},
          {target_spec{.own = 0x42, .mode = mode_off}, target_spec{.own = 0x43, .mode = mode_busy},
           target_spec{.own = 0x44, .mode = mode_held}}
      };
    };
    same_everywhere("refusing targets", build, row_end, [](world& w) {
      CHECK(
          w.log(0)
          == std::vector<std::uint32_t>{
              boot, written(0x42, 0, true), written(0x43, 0, true), written(0x44, 0, true)});
      for(std::size_t target = 0; target < 3; ++target)
      {
        for(auto const& phase : w.phases_of(target))
        {
          CHECK_FALSE(phase.ack);
          CHECK_FALSE(phase.dropped);
        }
        exact(w, target);
      }
      settled(w);
    });
  }
  auto const late = [](std::uint32_t own, std::uint32_t mode, auto&& change) {
    auto const r = split_by_q(
        [&](std::uint32_t wait) {
          return single({wait_us(wait), write(own, 1, 0x10)}, {.own = 0x42, .mode = mode});
        },
        first_stamp);
    same_everywhere(
        "late change", r.build, row_end,
        [&](world& w) {
          auto const seen = w.phases_of(0);
          REQUIRE(seen.size() == 2);
          CHECK(seen[0].ack);
          CHECK(contains(w.log(0), written(own, 1)));
          settled(w);
    },
        {{.at = r.at, .act = [&](world& w) { w.on(1, change); }}});
  };
  SUBCASE("PE set between the drain and t + L acknowledges")
  {
    late(0x42, mode_off, [](core::machine& target) {
      auto const base = target.peripheral("I2C1").description().base;
      auto const cr1  = target.mem().read(base, 4);
      REQUIRE(cr1.has_value());
      REQUIRE(target.mem().write(base, 4, *cr1 | 1u).has_value());
    });
  }
  SUBCASE("OAR1 changed between the drain and t + L acknowledges the new address")
  {
    late(0x46, mode_target, [](core::machine& target) {
      auto const base = target.peripheral("I2C1").description().base;
      for(auto const word : {0x46u << 1, 0x46u << 1, 0x8000u | 0x46u << 1})
      {
        REQUIRE(target.mem().write(base + 8, 4, word).has_value());
      }
    });
  }
}

TEST_CASE("a remote and a local target on one address: ACK from either, read bytes ANDed")
{
  auto const build = [] {
    return world{
        {wait_us(50), write(0x50, 4, 0x00), wait_us(100), write_sr(0x50, 2, 0x00), read(0x50, 2)},
        {target_spec{.own = 0x50, .first_tx = 0x3C}},
        latency,
        {eeprom()}
    };
  };
  same_everywhere("same address", build, 3'000 * us_ps, [](world& w) {
    CHECK(
        w.log(0)
        == std::vector<std::uint32_t>{
            boot, written(0x50, 4), written(0x50, 2), got(0x02 & 0x3C), got(0x03 & 0x3D),
            read_done(0x50)});
    auto const seen = w.phases_of(0);
    REQUIRE(seen.size() == 11);
    CHECK(seen[9].byte == 0x3C);
    CHECK(seen[10].byte == 0x3D);
    exact(w, 0);
    settled(w);
  });
}

TEST_CASE("a local EEPROM next to a remote target that does not match waits only for its address")
{
  auto const build = [] {
    return world{
        {wait_us(50), write(0x50, 4, 0x00), wait_us(100), write_sr(0x50, 2, 0x00), read(0x50, 2)},
        {target_spec{.own = 0x42}},
        latency,
        {eeprom()}
    };
  };
  same_everywhere("local eeprom", build, 3'000 * us_ps, [](world& w) {
    CHECK(
        w.log(0)
        == std::vector<std::uint32_t>{
            boot, written(0x50, 4), written(0x50, 2), got(0x02), got(0x03), read_done(0x50)});
    auto const seen = w.phases_of(0);
    REQUIRE(seen.size() == 3);
    for(auto const& phase : seen)
    {
      CHECK(phase.phase == periph::i2c_phase::address);
      CHECK_FALSE(phase.ack);
    }
    CHECK(w.log(1) == std::vector<std::uint32_t>{boot});
    exact(w, 0);
    settled(w);
  });
}

TEST_CASE("1000 controller resets mid-phase keep at most two owed units and one ticket")
{
  auto const build = single({kick(0x42, 0x10), wait_us(40), reset_op(), repeat(1000, 0)});
  same_everywhere(
      "owed bound", build, 570'000 * us_ps,
      [](world& w) {
        CHECK(w.phases_of(0).size() == 1000);
        CHECK(w.medium->owed_peak(0) <= 2);
        CHECK(w.medium->tickets_peak(0) == 1);
        CHECK(w.refused() == 1000);
        REQUIRE(w.recorded[0]->sender_resets.size() == 1000);
        settled(w);
      },
      {}, two_q());
}

TEST_CASE("four controller resets in one window keep at most five owed units")
{
  constexpr std::int64_t long_l = 3'000 * us_ps;
  auto const             build  = single(
      {kick(0x42, 0x10), wait_us(30), reset_op(), repeat(4, 0), write(0x42, 1, 0x20)}, {}, long_l);
  same_everywhere(
      "owed packed", build, 14'000 * us_ps,
      [](world& w) {
        CHECK(w.medium->owed_peak(0) <= 5);
        CHECK(w.medium->owed_peak(0) == (w.q == w.l ? 5u : 3u));
        CHECK(contains(w.log(1), data(0x20)));
        settled(w);
      },
      {}, two_q());
}

TEST_CASE("1000 controller resets after the answer's stamp keep at most one ticket")
{
  auto const build = single({kick(0x42, 0x10), wait_us(80), reset_op(), repeat(1000, 0)});
  same_everywhere(
      "ticket bound", build, 610'000 * us_ps,
      [](world& w) {
        CHECK(w.phases_of(0).size() == 1000);
        CHECK(w.medium->tickets_peak(0) == 1);
        CHECK(w.refused() == 0);
        settled(w);
      },
      {}, two_q());
}

TEST_CASE("two controller RCC resets inside a round trip keep three tickets, none at rest")
{
  constexpr std::int64_t long_l = 100 * us_ps;
  auto const             build  = single(
      {wait_us(50), kick(0x42, 0x10), wait_us(30), rcc_op(), kick(0x42, 0x11), wait_us(30),
       rcc_op(), write(0x42, 1, 0x12)},
      {}, long_l);
  same_everywhere("tickets rcc", build, row_end, [](world& w) {
    CHECK(w.medium->tickets_peak(0) <= 3);
    if(w.q < w.l)
    {
      CHECK(w.medium->tickets_peak(0) == 3);
    }
    CHECK(w.refused() == 2);
    CHECK(contains(w.log(1), data(0x12)));
    settled(w);
  });
}

TEST_CASE("phases after a software reset and a power-on reset of the controller keep their stamps")
{
  auto const build = single(
      {wait_us(50), write(0x42, 1, 0x10), wait_us(800), stamp_op(), write(0x42, 1, 0x11),
       wait_us(800), write(0x42, 1, 0x12)});
  auto const t = reference(build, 3'500 * us_ps)[0].at.ps;
  same_everywhere(
      "time bases", build, 3'500 * us_ps,
      [&](world& w) {
        REQUIRE(w.recorded[0]->sender_resets.size() == 2);
        auto const software = w.recorded[0]->sender_resets[0].ps;
        auto const power_on = w.recorded[0]->sender_resets[1].ps;
        auto const log      = w.log(0);
        auto const stamp =
            std::ranges::find_if(log, [](std::uint32_t word) { return word >> 24 == 0x5Au;}
               );
        REQUIRE(stamp != log.end());
        auto const stamped = software + w.nodes[0]->origin + (*stamp & 0xffffffu) * 250'000;
        auto const seen    = w.phases_of(0);
        auto const second  = std::ranges::find_if(seen, [](auto const& phase) {
          return phase.phase == periph::i2c_phase::write and phase.byte == 0x11;
                                                              }
               );
        REQUIRE(second != seen.end());
        auto const address = *(second - 1);
        CHECK(address.at.ps >= stamped + address_ps);
        CHECK(address.at.ps <= stamped + address_ps + 40 * us_ps);
        CHECK(address.at.ps < power_on);
        auto const third = std::ranges::find_if(seen, [](auto const& phase) {
          return phase.phase == periph::i2c_phase::write and phase.byte == 0x12;
                                                              }
               );
        REQUIRE(third != seen.end());
        CHECK(third->at.ps > power_on);
        exact(w, 0);
        CHECK(contains(w.log(1), data(0x11)));
        CHECK(contains(w.log(1), data(0x12)));
  },
      {{.at = t + 600 * us_ps, .act = [](world& w) { w.reset(0); }},
       {.at  = t + 1'800 * us_ps,
        .act = [](world& w) { w.reset(0, popcorn::reset_kind::power_on); }}});
}

TEST_CASE("give_back completes the phase the controller waits for, the transfer ends")
{
  auto const build = single({wait_us(50), write(0x42, 1, 0x10), write(0x42, 1, 0x11)});
  auto const t     = reference(build)[0].at.ps;
  same_everywhere(
      "give back", build, row_end,
      [](world& w) {
        CHECK(
            w.log(0)
            == std::vector<std::uint32_t>{boot, written(0x42, 0, true), written(0x42, 0, true)}
             );
        CHECK(w.medium->tickets(0) == 0);
  },
      {{.at = t + latency, .act = [](world& w) {
          w.on(0, [&](core::machine&) { w.medium->give_back_controller(); });
        }}});
}

TEST_CASE("a phase owed by a target given back gets NACK at its far-side end")
{
  auto const build = single({wait_us(50), write(0x42, 1, 0x10)});
  auto const t     = reference(build)[0].at.ps;
  auto const check = [&](world& w) {
    CHECK(w.log(0) == std::vector<std::uint32_t>{boot, written(0x42, 0, true)});
    CHECK(w.log(1) == std::vector<std::uint32_t>{boot});
    auto const seen = w.phases_of(0);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].dropped);
    CHECK(seen[0].answer.ps == t + w.l + address_ps);
    CHECK(w.refused() == 0);
    settled(w);
  };
  auto const give_back = [](world& w) {
    w.on(1, [&](core::machine&) { w.medium->give_back_target(0); });
  };
  SUBCASE("still in the M->S link")
  {
    same_everywhere(
        "give back target link", build, row_end, check,
        {
            {.at = t + us_ps / 4, .act = give_back}
    });
  }
  SUBCASE("running in the target's port")
  {
    same_everywhere(
        "give back target port", build, row_end, check,
        {
            {.at = t + latency + address_ps / 2, .act = give_back}
    });
  }
}

TEST_CASE("an idle pause of sleeping members waits for the answers carried in the links")
{
  auto const                 build = single({wait_us(50), kick(0x42, 0x10)});
  std::optional<std::string> first;
  for(auto const& how : variants())
  {
    CAPTURE(how.name);
    auto w = build();
    w.host(how);
    auto const paused = w.idle();
    REQUIRE(paused.reason == core::pause_reason::idle);
    auto const seen = w.phases_of(0);
    REQUIRE(seen.size() == 2);
    CHECK(seen[1].byte == 0x10);
    CHECK(paused.at.ps >= seen[1].answer.ps + w.l);
    CHECK(w.log(1) == std::vector<std::uint32_t>{boot, addressed(0x42), data(0x10), stopped});
    settled(w);
    auto const dump = std::format("{}idle at {}\n", w.dump(), paused.at.ps);
    keep_dump(std::format("i2c idle {}", how.name), dump);
    if(first)
    {
      CHECK(dump == *first);
    }
    else
    {
      first = dump;
    }
  }
}

TEST_CASE("the links and proxies keep their target's state alive after the medium is destroyed")
{
  auto w = single(basic())();
  w.host({.name = "q=l"});
  w.medium.reset();
  REQUIRE(w.run_to(row_end).reason == core::pause_reason::system_time);
  CHECK(
      w.log(0)
      == std::vector<std::uint32_t>{boot, written(0x42, 2), got(0xB0), got(0xB1), read_done(0x42)});
}

TEST_CASE("the medium refuses a target on the controller's member and a target with no port")
{
  auto       w   = world{basic(), {target_spec{}}};
  auto const end = [&](std::size_t member, stm32::i2c_link_port* port) {
    return std::vector<stm32::i2c_target_end>{
        {.member = member, .port = port, .forth_lane = 1, .back_lane = 1}
    };
  };
  CHECK_THROWS_WITH_AS(
      stm32::i2c_medium(0, *w.nodes[0]->port, core::sys_time{latency}, end(0, w.nodes[1]->port)),
      "I2C target 0 is an I2C on another member than the controller's", std::invalid_argument);
  CHECK_THROWS_WITH_AS(
      stm32::i2c_medium(0, *w.nodes[0]->port, core::sys_time{latency}, end(1, nullptr)),
      "I2C target 0 has an I2C link port", std::invalid_argument);
}

TEST_CASE("an I2C link refuses another link kind's bytes, SPI and UART bytes included")
{
  auto                   w     = world{basic(), {target_spec{}}};
  auto&                  forth = w.recorded[0]->inner();
  auto&                  back  = w.recorded[1]->inner();
  std::vector<std::byte> forth_bytes;
  std::vector<std::byte> back_bytes;
  forth.save(forth_bytes);
  back.save(back_bytes);
  CHECK_THROWS_AS(back.load(forth_bytes), popcorn::config_error);
  CHECK_THROWS_AS(forth.load(back_bytes), popcorn::config_error);
  auto* const usart = w.nodes[1]->f->model_of<stm32::uart_link_port>("USART1");
  REQUIRE(usart != nullptr);
  stm32::uart_wire wire{
      {.sender = 0, .receiver = 1},
      {.peer = 7, .injected = 8},
      core::sys_time{latency},
      {.baud = 115'200},
      {.port = usart, .unit = "USART1", .log = [](core::log_record) {}, .strict = false},
      nullptr
  };
  std::vector<std::byte> uart_bytes;
  wire.save(uart_bytes);
  CHECK_THROWS_AS(forth.load(uart_bytes), popcorn::config_error);
  CHECK_THROWS_AS(back.load(uart_bytes), popcorn::config_error);
  CHECK_THROWS_AS(wire.load(forth_bytes), popcorn::config_error);
  CHECK_THROWS_AS(wire.load(back_bytes), popcorn::config_error);
  for(auto const* kind : {"spi_forth", "spi_back"})
  {
    CAPTURE(kind);
    std::vector<std::byte> spi_bytes;
    reflex::serde::bson::serializer{spi_bytes}.dump(spi_kind{.kind = kind});
    CHECK_THROWS_AS(forth.load(spi_bytes), popcorn::config_error);
    CHECK_THROWS_AS(back.load(spi_bytes), popcorn::config_error);
  }
  std::vector<std::byte> after;
  forth.save(after);
  CHECK(after == forth_bytes);
  forth.load(forth_bytes);
  back.load(back_bytes);
}

namespace
{
struct captured
{
  std::vector<core::snapshot> members;
  std::vector<std::byte>      system;
};

auto capture(world& w) -> captured
{
  captured out;
  for(std::size_t member = 0; member < w.nodes.size(); ++member)
  {
    out.members.push_back(w.on(member, [](core::machine& target) { return target.snapshot(); }));
  }
  auto const held = w.sys->lock();
  w.sys->save(held, out.system);
  return out;
}

void put_back(world& w, captured const& saved)
{
  for(std::size_t member = 0; member < w.nodes.size(); ++member)
  {
    w.on(member, [&](core::machine& target) { target.restore(saved.members[member]); });
  }
  for(std::size_t member = 0; member < w.nodes.size(); ++member)
  {
    w.on(member, [&](core::machine& target) {
      auto        again = target.snapshot().data();
      auto const& was   = saved.members[member].data();
      CHECK(again.generation > was.generation);
      again.generation = was.generation;
      CHECK(core::encode_snapshot(again) == core::encode_snapshot(was));
    });
  }
  auto const held = w.sys->lock();
  w.sys->load(held, saved.system);
  std::vector<std::byte> again;
  w.sys->save(held, again);
  CHECK(again == saved.system);
}

void restored_run(
    build_ref                       build,
    std::int64_t                    at,
    std::function_ref<void(world&)> act,
    std::int64_t                    end   = row_end,
    check_fn                        after = no_check)
{
  for(auto const& how : variants())
  {
    CAPTURE(how.name);
    auto w = build();
    w.host(how);
    REQUIRE(w.run_to(at).reason == core::pause_reason::system_time);
    act(w);
    auto const saved = capture(w);
    w.taps.clear();
    REQUIRE(w.run_to(end).reason == core::pause_reason::system_time);
    auto const first = w.dump();
    put_back(w, saved);
    w.taps.clear();
    REQUIRE(w.run_to(end).reason == core::pause_reason::system_time);
    CHECK(w.dump() == first);
    CHECK_FALSE(w.taps.empty());
    after(w);
  }
}
}

TEST_CASE("a system snapshot with a phase in flight restores the same remaining phases")
{
  auto const        build = single(basic());
  static auto const seen  = reference(build);
  auto const        t     = seen[0].at.ps;
  SUBCASE("the controller holding SCL, the target's phase running")
  {
    restored_run(build, t + latency + us_ps, [](world&) {});
  }
  SUBCASE("between the address answer and the first data phase")
  {
    restored_run(build, seen[0].answer.ps + latency + us_ps, [](world&) {});
  }
  SUBCASE("between a target reset and the re-armed answer")
  {
    restored_run(build, t + latency + us_ps, [](world& w) { w.reset(1); });
  }
  SUBCASE("a phase in the M->S link at a target reset")
  {
    restored_run(build, t + us_ps / 4, [](world& w) { w.reset(1); });
  }
  SUBCASE("the controller muted by a reset at a pause")
  {
    restored_run(build, t + 2 * us_ps, [](world& w) { w.reset(0); });
  }
}

TEST_CASE("a system snapshot with a bus release in the link or in the target restores it")
{
  auto const build =
      single({wait_us(50), kick(0x42, 0x10), wait_us(40), rcc_op()}, {.own = 0x42, .timeout = 1});
  static auto const seen = reference(build, 3'000 * us_ps);
  auto const        t    = seen[0].at.ps;
  auto const        end  = 3'000 * us_ps;
  auto const        none = [](world& w) {
    CHECK(w.log(1) == std::vector<std::uint32_t>{boot, addressed(0x42)});
  };
  SUBCASE("the release in the M->S link")
  {
    restored_run(build, t + 20 * us_ps, [](world&) {}, end, none);
  }
  SUBCASE("the release stored in the target's timer")
  {
    restored_run(build, t + 40 * us_ps, [](world&) {}, end, none);
  }
}

namespace
{
auto timeout_due_at(std::int64_t due) -> step
{
  return {
      .at = due - 10 * us_ps, .act = [due](world& w) {
        w.on(1, [&](core::machine& target) {
          auto const& units = target.description().peripherals;
          auto const  found =
              std::ranges::find(units, std::string_view{"I2C1"}, &popcorn::peripheral_desc::name);
          REQUIRE(found != units.end());
          auto const owner = static_cast<std::uint32_t>(found - units.begin());
          auto       dues  = std::vector<std::uint64_t>{};
          for(auto const& event : target.snapshot().data().events.pending)
          {
            if(event.id.owner == owner and event.id.kind == 4)
            {
              dues.push_back(event.due_ps);
            }
          }
          CHECK(
              dues
              == std::vector<std::uint64_t>{static_cast<std::uint64_t>(due + w.nodes[1]->origin)});
        });
      }};
}
}

TEST_CASE("a target times out when the remote controller stalls past tTIMEOUT between phases")
{
  constexpr std::int64_t limit = 512 * us_ps;
  auto const             spec  = target_spec{.own = 0x42, .timeout = 1};
  SUBCASE("a stall past the limit sets TIMEOUT at the gap start plus tTIMEOUT, the target busy")
  {
    auto busy        = spec;
    busy.script      = {wait_us(3'000)};
    auto const build = single({wait_us(50), stall_us(900), write(0x42, 2, 0x10)}, busy);
    auto const t     = reference(build)[0].at.ps;
    auto const due   = t + latency + address_ps + limit;
    same_everywhere(
        "scl low", build, 3'000 * us_ps,
        [&](world& w) {
          auto const log = w.log(1);
          CHECK(log == std::vector<std::uint32_t>{boot, addressed(0x42), timed_out, stopped});
          auto const seen = w.phases_of(0);
          REQUIRE(seen.size() == 2);
          CHECK(seen[0].ack);
          CHECK_FALSE(seen[1].ack);
          CHECK(nacked_write(w.log(0), 0x42));
          settled(w);
        },
        {timeout_due_at(due)});
  }
  SUBCASE("a next phase starting just below the due sets nothing at every Q")
  {
    auto const left = [&](build_fn const& build) {
      auto const seen = reference(build);
      REQUIRE(seen.size() >= 2);
      auto const due   = seen[0].at.ps + latency + address_ps + limit;
      auto const start = seen[1].at.ps + latency;
      return due - start;
    };
    auto const stalled = [&](std::uint32_t stall) {
      return single({wait_us(50), stall_us(stall), write(0x42, 2, 0x10)}, spec);
    };
    auto const skip = std::max<std::int64_t>(0, (left(stalled(300)) - latency / 4) / us_ps);
    std::optional<build_fn> chosen;
    for(auto stall = 300 + static_cast<std::uint32_t>(skip); stall < 520 and not chosen; ++stall)
    {
      auto const build = stalled(stall);
      auto const gap   = left(build);
      if(gap > 0 and gap < latency / 4)
      {
        chosen = build;
      }
    }
    REQUIRE(chosen.has_value());
    same_everywhere("scl low below", *chosen, 3'000 * us_ps, [&](world& w) {
      CHECK(
          w.log(1)
          == std::vector<std::uint32_t>{boot, addressed(0x42), data(0x10), data(0x11), stopped});
      exact(w, 0);
      settled(w);
    });
  }
}

TEST_CASE("a target the controller does not address counts no SCL low during another's transfer")
{
  auto const build = [] {
    return world{
        {wait_us(50), write(0x50, 10, 0x10)},
        {target_spec{.own = 0x42, .timeout = 1}, target_spec{.own = 0x50}}
    };
  };
  same_everywhere("uninvolved", build, 3'000 * us_ps, [](world& w) {
    CHECK(w.log(0) == std::vector<std::uint32_t>{boot, written(0x50, 10)});
    CHECK(w.log(1) == std::vector<std::uint32_t>{boot});
    std::vector<std::uint32_t> wanted{boot, addressed(0x50)};
    for(std::uint32_t byte = 0x10; byte < 0x1A; ++byte)
    {
      wanted.push_back(data(byte));
    }
    wanted.push_back(stopped);
    CHECK(w.log(2) == wanted);
    auto const seen = w.phases_of(1);
    REQUIRE(seen.size() == 11);
    CHECK(seen.back().at.ps - seen.front().at.ps > 512 * us_ps);
    exact(w, 0);
    exact(w, 1);
    settled(w);
  });
}

TEST_CASE("an RCC reset or a PE clear of the remote controller ends the target's SCL low count")
{
  for(auto const release : {rcc_op(), disable_op()})
  {
    CAPTURE(release);
    auto const build =
        single({wait_us(50), kick(0x42, 0x10), wait_us(40), release}, {.own = 0x42, .timeout = 1});
    same_everywhere(
        release == rcc_op() ? "released rcc" : "released pe", build, 3'000 * us_ps, [](world& w) {
          CHECK(w.log(1) == std::vector<std::uint32_t>{boot, addressed(0x42)});
          auto const seen = w.phases_of(0);
          REQUIRE(seen.size() == 1);
          CHECK(seen[0].ack);
        });
  }
}

TEST_CASE("a target counts the SCL low of a new transfer after a controller RCC reset")
{
  constexpr std::int64_t limit = 512 * us_ps;
  auto const             build = single(
      {wait_us(50), kick(0x42, 0x10), wait_us(40), rcc_op(), stall_us(900), write(0x42, 2, 0x10)},
      {.own = 0x42, .timeout = 1});
  auto const seen = reference(build);
  REQUIRE(seen.size() == 3);
  auto const due = seen[1].at.ps + latency + address_ps + limit;
  same_everywhere(
      "released then stalled", build, 3'000 * us_ps,
      [](world& w) {
        CHECK(
            w.log(1)
            == std::vector<std::uint32_t>{
                boot, addressed(0x42), addressed(0x42), timed_out, stopped});
        auto const phases = w.phases_of(0);
        REQUIRE(phases.size() == 3);
        CHECK(phases[0].ack);
        CHECK(phases[1].ack);
        CHECK_FALSE(phases[2].ack);
        CHECK(nacked_write(w.log(0), 0x42));
      },
      {timeout_due_at(due)});
}

TEST_CASE("a target left by a repeated START to another counts no SCL low during that transfer")
{
  auto const build = [] {
    return world{
        {wait_us(50), write_sr(0x42, 2, 0x20), write(0x50, 10, 0x10)},
        {target_spec{.own = 0x42, .timeout = 1}, target_spec{.own = 0x50}}
    };
  };
  same_everywhere("left by sr", build, 3'000 * us_ps, [](world& w) {
    CHECK(w.log(0) == std::vector<std::uint32_t>{boot, written(0x42, 2), written(0x50, 10)});
    CHECK(
        w.log(1)
        == std::vector<std::uint32_t>{boot, addressed(0x42), data(0x20), data(0x21), stopped});
    std::vector<std::uint32_t> wanted{boot, addressed(0x50)};
    for(std::uint32_t byte = 0x10; byte < 0x1A; ++byte)
    {
      wanted.push_back(data(byte));
    }
    wanted.push_back(stopped);
    CHECK(w.log(2) == wanted);
    auto const seen = w.phases_of(1);
    REQUIRE(seen.size() == 12);
    CHECK(seen.back().at.ps - seen.front().at.ps > 512 * us_ps);
    exact(w, 1);
    settled(w);
  });
}

TEST_CASE("a phase drained to a target given back keeps the M->S link loud until the S->M window")
{
  world w{
      {wait_us(50), kick(0x42, 0x10)},
      {target_spec{}}
  };
  auto const links = w.medium->links();
  REQUIRE(links.size() == 2);
  auto& forth = *links[0];
  auto& back  = *links[1];
  forth.enter_window(0, core::time_map{});
  back.enter_window(0, core::time_map{});
  auto const& master = *w.nodes[0]->f;
  auto const  end    = vtime{static_cast<std::uint64_t>(w.nodes[0]->origin + 200 * us_ps)};
  REQUIRE(
      master.run_to_stop({.max_instructions = icount{1'000'000}, .until_time = end}).reason
      == stop_reason::time);
  REQUIRE_FALSE(forth.quiet());
  w.medium->give_back_target(0);
  auto const barrier = core::sys_time{static_cast<std::int64_t>(end.ps)};
  auto const next    = core::sys_time{barrier.ps + row_end};
  CHECK_FALSE(forth.drain(0, barrier, next, core::time_map{}).has_value());
  CHECK_FALSE(forth.quiet());
  CHECK(back.quiet());
  back.enter_window(1, core::time_map{});
  CHECK(forth.quiet());
  CHECK_FALSE(back.quiet());
  CHECK_FALSE(back.drain(1, next, core::sys_time{next.ps + row_end}, core::time_map{}).has_value());
  CHECK(back.quiet());
  CHECK(w.medium->owed(0) == 0);
  CHECK(w.medium->tickets(0) == 0);
}

TEST_CASE("the proxies detach and attach again under host access between two transfers")
{
  auto const build =
      single({wait_us(50), write(0x42, 1, 0x10), wait_us(600), write(0x42, 1, 0x20)});
  same_everywhere(
      "reattach", build, row_end,
      [](world& w) {
        CHECK(w.log(0) == std::vector<std::uint32_t>{boot, written(0x42, 1), written(0x42, 1)}
          );
        CHECK(
            w.log(1)
            == std::vector<std::uint32_t>{
                boot, addressed(0x42), data(0x10), stopped, addressed(0x42), data(0x20), stopped}
          );
        exact(w, 0);
        CHECK(w.refused() == 0);
        settled(w);
  },
      {{.at = 450 * us_ps, .act = [](world& w) {
          w.on(0, [&](core::machine&) { w.medium->give_back_controller(); });
          w.on(1, [&](core::machine&) { w.medium->give_back_target(0); });
          w.on(0, [&](core::machine&) { w.medium->attach_controller(); });
          w.on(1, [&](core::machine&) { w.medium->attach_target(0); });
        }}});
}
