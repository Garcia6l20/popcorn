#include "dma_signal.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/machine.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/periph/firewall.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/builtin.hpp>
#include <popcorn/periph/uart.hpp>
#include <popcorn/periph/usb_host.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace periph = popcorn::periph;
namespace stm32  = popcorn::periph::stm32;
using popcorn::address;
using popcorn::icount;
using popcorn::reset_kind;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;
using popcorn::testing::u575;

constexpr std::uint64_t instruction_ps  = 250'000;
constexpr std::uint64_t us              = 1'000'000;
constexpr std::uint64_t ms              = 1'000 * us;
constexpr address       stub            = 0x2006'0000;
constexpr address       source_words    = 0x2006'1000;
constexpr address       target_words    = 0x2006'2000;
constexpr address       gpdma           = 0x4002'0000;
constexpr address       rcc             = 0x4602'0c00;
constexpr address       usart1          = 0x4001'3800;
constexpr address       nvic_iser       = 0xe000'e100;
constexpr address       nvic_ispr       = 0xe000'e200;
constexpr address       nvic_icpr       = 0xe000'e280;
constexpr address       scb_scr         = 0xe000'ed10;
constexpr address       syst_csr        = 0xe000'e010;
constexpr address       syst_rvr        = 0xe000'e014;
constexpr std::uint32_t rng_irq         = 94;
constexpr std::uint32_t hash_irq        = 96;
constexpr std::uint32_t gpdma0_irq      = 29;
constexpr std::uint32_t gpdma1_irq      = 30;
constexpr std::uint32_t gpdma12_irq     = 84;
constexpr std::uint32_t gtzc_irq        = 8;
constexpr std::uint32_t sai1_irq        = 90;
constexpr address       sai1_a          = 0x4001'5404;
constexpr address       sai1_b          = 0x4001'5424;
constexpr std::uint32_t sai_enable      = 1u << 16;
constexpr std::uint64_t sai_main_bound  = 1'000'000;
constexpr std::uint64_t sai_setup_bound = 1'000'000;

constexpr std::array<std::uint16_t, 2> wfi_loop{0xbf30, 0xe7fd};
constexpr std::array<std::uint16_t, 2> wfe_loop{0xbf20, 0xe7fd};
constexpr std::array<std::uint16_t, 1> self_loop{0xe7fe};

class pulse_model final : public periph::model
{
public:
  pulse_model(periph::model_context& ctx, std::optional<periph::wake_role> role) noexcept
      : context_{ctx}, role_{role}
  {}

  bool stop_on_event = false;

  void arm(vtime at)
  {
    std::ignore = context_.events.schedule_at(at, 0, 0, context_.clock);
  }

  void watch_reads(address at, std::uint32_t size)
  {
    context_.services.watch_reads(at, size, true);
  }

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }

  void reset(periph::register_file&, reset_kind) override
  {}

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
  {
    if(stop_on_event)
    {
      context_.request_stop(stop_reason::user, "pulse");
      return;
    }
    context_.irqs[0].raise();
  }

  void save(std::vector<std::byte>&) const override
  {}

  void load(std::span<std::byte const>) override
  {}

  [[nodiscard]] auto stateless() const -> bool override
  {
    return true;
  }

  [[nodiscard]] auto role_in_horizon() const noexcept -> periph::wake_role override
  {
    return role_ ? *role_ : periph::model::role_in_horizon();
  }

private:
  periph::model_context&           context_;
  std::optional<periph::wake_role> role_;
};

class denying_firewall final : public periph::model, public periph::bus_firewall
{
public:
  denying_firewall(periph::model_context& ctx, address first, address last)
      : first_{first}, last_{last}
  {
    ctx.install_firewall(*this);
  }

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }

  void reset(periph::register_file&, reset_kind) override
  {}

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

  [[nodiscard]] auto stateless() const -> bool override
  {
    return true;
  }

  [[nodiscard]] auto role_in_horizon() const noexcept -> periph::wake_role override
  {
    return periph::wake_role::lines;
  }

  [[nodiscard]] auto check_peripheral(std::size_t, popcorn::bus_attrs, popcorn::arch::memory_op)
      -> periph::bus_verdict override
  {
    return periph::bus_verdict::allow;
  }

  [[nodiscard]] auto filters_memory() const -> bool override
  {
    return true;
  }

  [[nodiscard]] auto
      check_memory(address at, std::uint8_t, popcorn::bus_attrs attrs, popcorn::arch::memory_op op)
          -> periph::bus_verdict override
  {
    return attrs.by == popcorn::initiator::dma
               and op == popcorn::arch::memory_op::write
               and at >= first_
               and at <= last_
             ? periph::bus_verdict::bus_error
             : periph::bus_verdict::allow;
  }

private:
  address first_;
  address last_;
};

class ticking_requester final : public periph::model, public stm32::dma_requester
{
public:
  explicit ticking_requester(periph::model_context& ctx)
      : context_{ctx}, signal_{ctx.peers, "adc1_dma"}
  {}

  void arm(vtime first, std::uint64_t period_ps, std::uint32_t count)
  {
    for(std::uint32_t index = 0; index < count; ++index)
    {
      std::ignore =
          context_.events.schedule_at(vtime{first.ps + index * period_ps}, 0, 0, context_.clock);
    }
  }

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }

  void reset(periph::register_file&, reset_kind) override
  {}

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
  {
    signal_.set(true);
  }

  void acknowledged(stm32::dma_controller const& by, std::uint32_t request) override
  {
    if(signal_.carries(by, request))
    {
      signal_.set(false);
    }
  }

  void transfer_completed(stm32::dma_controller const&, std::uint32_t) override
  {}

  void save(std::vector<std::byte>&) const override
  {}

  void load(std::span<std::byte const>) override
  {}

  [[nodiscard]] auto stateless() const -> bool override
  {
    return true;
  }

  [[nodiscard]] auto role_in_horizon() const noexcept -> periph::wake_role override
  {
    return periph::wake_role::lines;
  }

private:
  periph::model_context& context_;
  stm32::dma_signal      signal_;
};

class frame_log final : public periph::uart_endpoint
{
public:
  std::vector<vtime> starts{};

  void transmitted(std::uint16_t, vtime, bool) override
  {}

  void started(periph::line_frame const& frame) override
  {
    starts.push_back(frame.start);
  }
};

template <typename Model> struct slot
{
  Model* model{};
};

void write32(stm32_fixture const& f, address at, std::uint32_t value)
{
  REQUIRE(f.target->mem().write(at, 4, value).has_value());
}

auto read32(stm32_fixture const& f, address at) -> std::uint32_t
{
  auto const value = f.target->mem().read(at, 4);
  REQUIRE(value.has_value());
  return static_cast<std::uint32_t>(*value);
}

void set_bits(stm32_fixture const& f, address at, std::uint32_t bits)
{
  write32(f, at, read32(f, at) | bits);
}

void enable_irq(stm32_fixture const& f, std::uint32_t irq)
{
  write32(f, nvic_iser + 4 * (irq / 32), std::uint32_t{1} << (irq % 32));
}

auto now(stm32_fixture const& f) -> vtime
{
  return f.target->virtual_clock().time();
}

auto later(stm32_fixture const& f, std::uint64_t ps) -> vtime
{
  return vtime{now(f).ps + ps};
}

void park(
    stm32_fixture const&           f,
    std::span<std::uint16_t const> code,
    std::uint64_t                  steps = 1,
    address                        at    = stub)
{
  for(auto const [index, half] : std::views::enumerate(code))
  {
    REQUIRE(f.target->mem().write(at + 2 * static_cast<address>(index), 2, half).has_value());
  }
  f.set("pc", at | 1u);
  auto const result = f.run({.exact_instructions = icount{steps}});
  REQUIRE(
      (not result.stopped
       or result.stopped->reason == stop_reason::budget
       or result.stopped->reason == stop_reason::idle));
}

auto run_until(stm32_fixture const& f, vtime at) -> core::slice_result
{
  return f.run(
      {.max_instructions = icount{4'000'000'000},
       .wall_budget      = std::chrono::seconds{60},
       .until_time       = at,
       .internal_stop    = true});
}

void check_wakes_at(stm32_fixture const& f, vtime wake)
{
  auto const before = f.target->executed();
  std::ignore       = run_until(f, wake);
  CHECK(now(f) >= wake);
  CHECK(now(f).ps < wake.ps + instruction_ps);
  CHECK(f.target->executed() == before);
  std::ignore = run_until(f, vtime{wake.ps + 2 * instruction_ps});
  CHECK(f.target->executed() != before);
}

void check_asleep_until(stm32_fixture const& f, vtime until)
{
  auto const before = f.target->executed();
  std::ignore       = run_until(f, until);
  CHECK(f.target->executed() == before);
}

struct pulses
{
  slot<pulse_model>              rng;
  slot<pulse_model>              hash;
  std::unique_ptr<stm32_fixture> f;

  explicit pulses(std::optional<periph::wake_role> role = periph::wake_role::lines)
  {
    auto options           = popcorn::testing::stm32_options();
    options.spin_threshold = 0;
    options.models.emplace("RNG", [this, role](periph::model_context& ctx, auto const&) {
      auto made = std::make_shared<pulse_model>(ctx, role);
      rng.model = made.get();
      return std::shared_ptr<periph::model>{made};
    });
    options.models.emplace("HASH", [this, role](periph::model_context& ctx, auto const&) {
      auto made  = std::make_shared<pulse_model>(ctx, role);
      hash.model = made.get();
      return std::shared_ptr<periph::model>{made};
    });
    f = std::make_unique<stm32_fixture>(u575(), "boot_ll", std::move(options));
  }
};

struct sweep_result
{
  std::uint32_t sleeps{};
  std::uint32_t late_wakes{};
  std::uint32_t frames{};
  std::uint32_t late_frames{};
  std::uint32_t transmit_bounds{};
  std::uint32_t early_stops{};
};

auto sweep(stm32_fixture const& f, vtime end, std::span<frame_log const* const> wires)
    -> sweep_result
{
  sweep_result out;
  while(now(f) < end)
  {
    auto const h  = f.target->horizon();
    auto const t0 = now(f);
    auto       to = h.wake.value_or(end);
    for(auto const& bound : h.transmit)
    {
      if(bound)
      {
        ++out.transmit_bounds;
        to = std::min(to, *bound);
      }
    }
    if(h.stop)
    {
      to = std::min(to, *h.stop);
    }
    to = std::min(to, end);
    if(not h.asleep or to <= t0)
    {
      auto const result = f.run(
          {.max_instructions = icount{2'000},
           .wall_budget      = std::chrono::seconds{60},
           .until_time       = end,
           .internal_stop    = true});
      if(result.stopped and result.stopped->reason != stop_reason::time)
      {
        break;
      }
      continue;
    }
    ++out.sleeps;
    std::vector<std::size_t> seen;
    for(auto const* const wire : wires)
    {
      seen.push_back(wire->starts.size());
    }
    auto const before = f.target->executed();
    auto const result = run_until(f, to);
    if(f.target->executed() != before)
    {
      ++out.late_wakes;
    }
    for(std::size_t w = 0; w < wires.size(); ++w)
    {
      for(auto index = seen[w]; index < wires[w]->starts.size(); ++index)
      {
        ++out.frames;
        auto const bound = w < h.transmit.size() ? h.transmit[w] : std::nullopt;
        if(not bound or wires[w]->starts[index] < *bound)
        {
          ++out.late_frames;
        }
      }
    }
    if(result.stopped and result.stopped->reason != stop_reason::time)
    {
      if(now(f) < to)
      {
        ++out.early_stops;
      }
      break;
    }
  }
  return out;
}

void enter_main(stm32_fixture const& f)
{
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{sai_main_bound}, .until = sym("sai_ll", "main")})
          .reason
      == stop_reason::address);
}

void channel(
    stm32_fixture const& f,
    std::size_t          ch,
    std::uint32_t        tr1,
    std::uint32_t        tr2,
    std::uint32_t        bytes,
    address              from,
    address              to,
    std::uint32_t        cr)
{
  auto const base = gpdma + 0x50 + 0x80 * static_cast<address>(ch);
  set_bits(f, rcc + 0x88, 1u);
  write32(f, base + 0x40, tr1);
  write32(f, base + 0x44, tr2);
  write32(f, base + 0x48, bytes);
  write32(f, base + 0x4c, static_cast<std::uint32_t>(from));
  write32(f, base + 0x50, static_cast<std::uint32_t>(to));
  write32(f, base + 0x7c, 0);
  write32(f, base + 0x14, cr);
}

void channel0(
    stm32_fixture const& f,
    std::uint32_t        tr1,
    std::uint32_t        tr2,
    std::uint32_t        bytes,
    address              from,
    address              to,
    std::uint32_t        cr)
{
  set_bits(f, rcc + 0x88, 1u);
  write32(f, gpdma + 0x90, tr1);
  write32(f, gpdma + 0x94, tr2);
  write32(f, gpdma + 0x98, bytes);
  write32(f, gpdma + 0x9c, static_cast<std::uint32_t>(from));
  write32(f, gpdma + 0xa0, static_cast<std::uint32_t>(to));
  write32(f, gpdma + 0xcc, 0);
  write32(f, gpdma + 0x64, cr);
}

constexpr std::uint32_t cr_enable = 1u << 0;
constexpr std::uint32_t cr_tcie   = 1u << 8;
constexpr std::uint32_t cr_dteie  = 1u << 10;
constexpr std::uint32_t tr2_swreq = 1u << 9;
constexpr std::uint32_t tr1_words = 2u | (2u << 16);
constexpr std::uint32_t tr1_sinc  = 1u << 3;
constexpr std::uint32_t tr1_dinc  = 1u << 19;
constexpr std::uint64_t burst_ps  = 4 * instruction_ps;
constexpr std::uint64_t frame_ps  = 100 * us;

auto field_at(std::string_view peripheral, std::string_view field)
    -> std::pair<address, std::uint32_t>
{
  auto const& unit =
      *std::ranges::find(u575().peripherals, peripheral, &popcorn::peripheral_desc::name);
  for(auto const& reg : unit.registers)
  {
    for(auto const& bits : reg.fields)
    {
      if(bits.name == field)
      {
        return {unit.base + reg.offset, std::uint32_t{1} << bits.lsb};
      }
    }
  }
  FAIL("no field ", field, " in ", peripheral);
  return {};
}

void enter_sai_setup(stm32_fixture const& f)
{
  enter_main(f);
  f.set("pc", sym("sai_ll", "dma"));
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{sai_setup_bound},
                     .until            = sym("sai_ll", "__NVIC_EnableIRQ.part.0")})
          .reason
      == stop_reason::address);
  write32(f, gpdma + 0x64, 0);
  write32(f, gpdma + 0xcc, 0);
}

void usart_rx_dma(stm32_fixture const& f, bool fifo, std::uint32_t bytes)
{
  set_bits(f, rcc + 0xa4, 1u << 14);
  write32(f, usart1 + 0x0c, 40);
  write32(f, usart1 + 0x08, 1u << 6);
  write32(f, usart1 + 0x00, 1u | (1u << 2) | (fifo ? 1u << 29 : 0u));
  auto* const controller = dynamic_cast<stm32::dma_controller*>(f.target->model("GPDMA1"));
  REQUIRE(controller != nullptr);
  auto const request = controller->request_of("usart1_rx_dma");
  REQUIRE(request.has_value());
  enable_irq(f, gpdma0_irq);
  channel0(f, tr1_dinc, *request, bytes, usart1 + 0x24, target_words, 0);
}

void receive_frames(stm32_fixture const& f, std::size_t count)
{
  std::vector<std::uint16_t> data(count, 0x5a);
  dynamic_cast<periph::uart_port*>(f.target->model("USART1"))->receive(data);
}

void run_one(stm32_fixture const& f, vtime end)
{
  std::ignore = f.run(
      {.wall_budget        = std::chrono::seconds{60},
       .exact_instructions = icount{1},
       .until_time         = end});
}

constexpr std::uint32_t cr_htie       = 1u << 9;
constexpr std::uint32_t cr_uleie      = 1u << 11;
constexpr std::uint32_t cr_useie      = 1u << 12;
constexpr std::uint32_t cr_errors     = cr_dteie | cr_uleie | cr_useie;
constexpr std::uint32_t tr1_planar    = 1u | tr1_sinc | (2u << 16);
constexpr std::uint32_t tcem_item     = 2u << 30;
constexpr std::uint32_t br1_brsdec    = 1u << 30;
constexpr std::uint32_t htf           = 1u << 9;
constexpr std::size_t   planar_ch     = 12;
constexpr address       planar_link   = 0x2006'3000;
constexpr std::uint64_t request_ps    = 20 * us;
constexpr std::uint32_t planar_blocks = 8;

struct planar_walk
{
  std::uint32_t tr1{tr1_planar};
  std::uint32_t tr2{tr2_swreq | tcem_item};
  std::uint32_t br1{4u | ((planar_blocks - 1) << 16) | br1_brsdec};
  address       from{source_words};
  address       to{target_words};
  std::uint32_t tr3{1022};
  std::uint32_t br2{2046};
  bool          linked{true};
  std::uint32_t cr{cr_tcie | cr_htie | cr_errors | cr_enable};
};

auto channel_base(std::size_t ch) -> address
{
  return gpdma + 0x50 + 0x80 * static_cast<address>(ch);
}

void channel_2d(stm32_fixture const& f, std::size_t ch, planar_walk const& w)
{
  auto const base = channel_base(ch);
  set_bits(f, rcc + 0x88, 1u);
  write32(f, planar_link, 0);
  write32(f, base + 0x00, 0x2006'0000);
  write32(f, base + 0x40, w.tr1);
  write32(f, base + 0x44, w.tr2);
  write32(f, base + 0x48, w.br1);
  write32(f, base + 0x4c, static_cast<std::uint32_t>(w.from));
  write32(f, base + 0x50, static_cast<std::uint32_t>(w.to));
  write32(f, base + 0x54, w.tr3);
  write32(f, base + 0x58, w.br2);
  write32(f, base + 0x7c, w.linked ? (planar_link & 0xfffcu) | (1u << 16) : 0u);
  write32(f, base + 0x14, w.cr);
}

auto adc_requester(slot<ticking_requester>& adc) -> core::machine_options
{
  auto options           = popcorn::testing::stm32_options();
  options.spin_threshold = 0;
  options.models.emplace("ADC1", [&adc](periph::model_context& ctx, auto const&) {
    auto made = std::make_shared<ticking_requester>(ctx);
    adc.model = made.get();
    return std::shared_ptr<periph::model>{made};
  });
  return options;
}

void rearm_line(stm32_fixture const& f, std::size_t ch, std::uint32_t irq)
{
  auto const base = channel_base(ch);
  REQUIRE((read32(f, base + 0x10) & htf) != 0);
  write32(f, base + 0x0c, htf);
  write32(f, nvic_icpr + 4 * (irq / 32), std::uint32_t{1} << (irq % 32));
  enable_irq(f, irq);
}

auto adc_request(stm32_fixture const& f) -> std::uint32_t
{
  auto* const controller = dynamic_cast<stm32::dma_controller*>(f.target->model("GPDMA1"));
  REQUIRE(controller != nullptr);
  auto const request = controller->request_of("adc1_dma");
  REQUIRE(request.has_value());
  return *request;
}
}

TEST_CASE("WFI with an enabled IRQ due at T wakes at T")
{
  pulses      p;
  auto const& f = *p.f;
  park(f, wfi_loop);
  enable_irq(f, hash_irq);
  auto const at = later(f, 40 * us);
  p.hash.model->arm(at);
  auto const h = f.target->horizon();
  REQUIRE(h.asleep);
  REQUIRE(h.wake.has_value());
  CHECK(*h.wake == at);
  check_wakes_at(f, at);
}

TEST_CASE("WFI skips an IRQ disabled in the NVIC and wakes at the next enabled one")
{
  pulses      p;
  auto const& f = *p.f;
  park(f, wfi_loop);
  enable_irq(f, hash_irq);
  auto const first  = later(f, 20 * us);
  auto const second = later(f, 60 * us);
  p.rng.model->arm(first);
  p.hash.model->arm(second);
  auto const h = f.target->horizon();
  REQUIRE(h.wake.has_value());
  CHECK(*h.wake == second);
  check_wakes_at(f, second);
}

TEST_CASE("WFI with PRIMASK set wakes at an enabled IRQ")
{
  pulses      p;
  auto const& f = *p.f;
  park(f, wfi_loop);
  f.set("primask", 1);
  enable_irq(f, hash_irq);
  auto const at = later(f, 30 * us);
  p.hash.model->arm(at);
  auto const h = f.target->horizon();
  REQUIRE(h.wake.has_value());
  CHECK(*h.wake == at);
  check_wakes_at(f, at);
}

TEST_CASE("WFE under SEVONPEND wakes at a disabled IRQ, without it at the next enabled one")
{
  for(auto const sevonpend : {true, false})
  {
    CAPTURE(sevonpend);
    pulses      p;
    auto const& f = *p.f;
    if(sevonpend)
    {
      write32(f, scb_scr, 1u << 4);
    }
    park(f, wfe_loop, 3);
    REQUIRE(f.target->horizon().asleep);
    enable_irq(f, hash_irq);
    auto const first  = later(f, 20 * us);
    auto const second = later(f, 60 * us);
    p.rng.model->arm(first);
    p.hash.model->arm(second);
    auto const h = f.target->horizon();
    REQUIRE(h.wake.has_value());
    CHECK(*h.wake == (sevonpend ? first : second));
    check_wakes_at(f, *h.wake);
  }
}

TEST_CASE("WFE with the event register set runs at once")
{
  pulses      p;
  auto const& f = *p.f;
  write32(f, scb_scr, 1u << 4);
  park(f, wfe_loop, 3);
  REQUIRE(f.target->horizon().asleep);
  write32(f, nvic_ispr + 4 * (rng_irq / 32), std::uint32_t{1} << (rng_irq % 32));
  auto const h = f.target->horizon();
  CHECK_FALSE(h.asleep);
  CHECK_FALSE(h.wake.has_value());
}

TEST_CASE("b . with PRIMASK set and only maskable sources wakes at the watchdog alone")
{
  pulses      p;
  auto const& f = *p.f;
  park(f, self_loop);
  f.set("primask", 1);
  enable_irq(f, hash_irq);
  auto const masked = later(f, 30 * us);
  p.hash.model->arm(masked);
  auto h = f.target->horizon();
  CHECK(h.asleep);
  CHECK_FALSE(h.wake.has_value());
  CHECK_FALSE(h.stop.has_value());
  write32(f, 0x4000'3000, 0xcccc);
  for(h = f.target->horizon(); h.wake and *h.wake < later(f, 10 * ms); h = f.target->horizon())
  {
    std::ignore = run_until(f, vtime{h.wake->ps + instruction_ps});
    CHECK(f.target->regs().read(*f.target->regs().find("pc")) == stub);
  }
  REQUIRE(h.wake.has_value());
  CHECK(h.stop == h.wake);
  CHECK(*h.wake > later(f, 100 * ms));
  std::ignore = run_until(f, vtime{h.wake->ps - instruction_ps});
  CHECK(f.target->regs().read(*f.target->regs().find("pc")) == stub);
}

TEST_CASE("SysTick wakes WFI only while TICKINT is set")
{
  pulses      p;
  auto const& f = *p.f;
  park(f, wfi_loop);
  write32(f, syst_rvr, 399);
  write32(f, syst_csr, 7);
  auto const h = f.target->horizon();
  REQUIRE(h.wake.has_value());
  CHECK(*h.wake > now(f));
  write32(f, syst_csr, 5);
  CHECK_FALSE(f.target->horizon().wake.has_value());
  check_asleep_until(f, vtime{h.wake->ps + 10 * instruction_ps});
}

TEST_CASE("SysTick with TICKINT set wakes WFI at its expiry")
{
  pulses      p;
  auto const& f = *p.f;
  park(f, wfi_loop);
  write32(f, syst_rvr, 399);
  write32(f, syst_csr, 7);
  auto const h = f.target->horizon();
  REQUIRE(h.wake.has_value());
  check_wakes_at(f, *h.wake);
}

TEST_CASE("b . bounds the stop where the instructions left run out")
{
  pulses      p;
  auto const& f = *p.f;
  park(f, self_loop);
  auto const h = f.target->horizon(icount{1'000});
  REQUIRE(h.stop.has_value());
  CHECK(*h.stop == later(f, 1'000 * instruction_ps));
  auto const stopped = f.run_to_stop({.budget_left = icount{1'000}});
  CHECK(stopped.reason == stop_reason::budget);
  CHECK(now(f) == *h.stop);
}

TEST_CASE("an opaque model's event bounds the stop and the stop happens there")
{
  pulses      p{periph::wake_role::opaque};
  auto const& f = *p.f;
  park(f, wfi_loop);
  p.rng.model->stop_on_event = true;
  auto const at              = later(f, 25 * us);
  p.rng.model->arm(at);
  auto const h = f.target->horizon();
  CHECK(h.stop == std::optional{at});
  CHECK(h.wake == std::optional{at});
  auto const before  = f.target->executed();
  auto const stopped = f.run_to_stop();
  CHECK(stopped.reason == stop_reason::user);
  CHECK(now(f) == at);
  CHECK(f.target->executed() == before);
}

TEST_CASE("a GPDMA channel counting to TC wakes at the closed form of its fifth burst")
{
  slot<ticking_requester> adc;
  auto                    options = popcorn::testing::stm32_options();
  options.models.emplace("ADC1", [&adc](periph::model_context& ctx, auto const&) {
    auto made = std::make_shared<ticking_requester>(ctx);
    adc.model = made.get();
    return std::shared_ptr<periph::model>{made};
  });
  stm32_fixture f{u575(), "boot_ll", std::move(options)};
  park(f, wfi_loop);
  auto const  parked     = f.target->executed();
  auto* const controller = dynamic_cast<stm32::dma_controller*>(f.target->model("GPDMA1"));
  REQUIRE(controller != nullptr);
  auto const request = controller->request_of("adc1_dma");
  REQUIRE(request.has_value());
  enable_irq(f, gpdma0_irq);
  channel0(f, tr1_words | tr1_dinc, *request, 20, source_words, target_words, cr_tcie | cr_enable);
  auto const first = later(f, 10 * us);
  adc.model->arm(first, 20 * us, 5);
  auto const h = f.target->horizon();
  REQUIRE(h.wake.has_value());
  CHECK(*h.wake == vtime{first.ps + 4 * burst_ps});
  CHECK(*h.wake != vtime{first.ps + burst_ps});
  check_asleep_until(f, *h.wake);
  check_asleep_until(f, vtime{first.ps + 4 * 20 * us});
  std::ignore = run_until(f, vtime{first.ps + 4 * 20 * us + 2 * burst_ps});
  CHECK(f.target->executed() != parked);
}

TEST_CASE("a GPDMA channel writing a wired USART's TDR bounds the transmit at its burst")
{
  pulses      p;
  auto const& f = *p.f;
  set_bits(f, rcc + 0xa4, 1u << 14);
  write32(f, usart1 + 0x0c, 35);
  write32(f, usart1 + 0x00, 1u | (1u << 3));
  auto log = std::make_shared<frame_log>();
  dynamic_cast<periph::uart_port*>(f.target->model("USART1"))->attach(log);
  park(f, wfi_loop);
  std::ignore = run_until(f, later(f, 1 * ms));
  REQUIRE(f.target->horizon().asleep);
  log->starts.clear();
  f.target->set_transmit_sources({{"USART1"}});
  REQUIRE(f.target->mem().write(source_words, 1, 0x55).has_value());
  channel0(f, tr1_sinc, tr2_swreq, 1, source_words, usart1 + 0x28, cr_enable);
  auto const h = f.target->horizon();
  REQUIRE(h.transmit.size() == 1);
  REQUIRE(h.transmit[0].has_value());
  CHECK(*h.transmit[0] == later(f, burst_ps));
  std::ignore = run_until(f, later(f, 1 * ms));
  REQUIRE_FALSE(log->starts.empty());
  CHECK(log->starts.front() >= *h.transmit[0]);
}

TEST_CASE("a GPDMA linked list bounds the transmit at the first burst after its link")
{
  pulses      p;
  auto const& f = *p.f;
  set_bits(f, rcc + 0xa4, 1u << 14);
  write32(f, usart1 + 0x0c, 35);
  write32(f, usart1 + 0x00, 1u | (1u << 3));
  auto log = std::make_shared<frame_log>();
  dynamic_cast<periph::uart_port*>(f.target->model("USART1"))->attach(log);
  park(f, wfi_loop);
  std::ignore = run_until(f, later(f, 1 * ms));
  log->starts.clear();
  f.target->set_transmit_sources({{"USART1"}});
  constexpr address item = 0x2006'3000;
  write32(f, item, 1);
  write32(f, item + 4, static_cast<std::uint32_t>(usart1 + 0x28));
  write32(f, item + 8, 0);
  channel0(f, tr1_sinc | tr1_dinc, tr2_swreq, 4, source_words, target_words, 0);
  write32(f, gpdma + 0x50, 0x2006'0000);
  write32(f, gpdma + 0xcc, (item & 0xfffcu) | (1u << 29) | (1u << 27) | (1u << 16));
  write32(f, gpdma + 0x64, cr_enable);
  auto const h = f.target->horizon();
  REQUIRE(h.transmit.size() == 1);
  REQUIRE(h.transmit[0].has_value());
  CHECK(*h.transmit[0] == later(f, 5 * burst_ps));
  std::ignore = run_until(f, later(f, 1 * ms));
  REQUIRE_FALSE(log->starts.empty());
  CHECK(log->starts.front() >= *h.transmit[0]);
}

TEST_CASE("a GPDMA channel with DTEIE and an invalid destination wakes at its next burst")
{
  pulses      p;
  auto const& f = *p.f;
  park(f, wfi_loop);
  enable_irq(f, gpdma0_irq);
  channel0(
      f, tr1_words | tr1_sinc | tr1_dinc, tr2_swreq, 64, source_words, 0x6000'0000,
      cr_dteie | cr_enable);
  auto const h = f.target->horizon();
  REQUIRE(h.wake.has_value());
  CHECK(*h.wake == later(f, burst_ps));
  check_wakes_at(f, *h.wake);
}

TEST_CASE("a DMA write to a watched address bounds the stop and stops there with no instruction")
{
  pulses      p;
  auto const& f = *p.f;
  park(f, wfi_loop);
  auto const watch =
      f.target->add_watch(target_words, 4, core::watch_kind::write, [](core::watch_hit const&) {
        return core::watch_action::stop;
      });
  channel0(f, tr1_words | tr1_sinc | tr1_dinc, tr2_swreq, 4, source_words, target_words, cr_enable);
  auto const h = f.target->horizon();
  REQUIRE(h.stop.has_value());
  CHECK(*h.stop == later(f, burst_ps));
  auto const before  = f.target->executed();
  auto const stopped = f.run_to_stop();
  CHECK(stopped.reason == stop_reason::watchpoint);
  CHECK(now(f) == *h.stop);
  CHECK(f.target->executed() == before);
}

TEST_CASE("a running timer with its IRQ disabled bounds the wake as a trigger source")
{
  pulses      p;
  auto const& f = *p.f;
  park(f, wfi_loop);
  CHECK_FALSE(f.target->horizon().wake.has_value());
  set_bits(f, rcc + 0x9c, 1u << 4);
  write32(f, 0x4000'102c, 999);
  write32(f, 0x4000'1000, 1);
  auto const h = f.target->horizon();
  REQUIRE(h.wake.has_value());
  CHECK(*h.wake <= later(f, 1'000 * instruction_ps));
  check_asleep_until(f, *h.wake);
}

TEST_CASE("an SAI underrun with FIFO words left wakes at the closed-form slot")
{
  stm32_fixture f{u575(), "sai_ll"};
  enter_main(f);
  f.set("pc", sym("sai_ll", "underrun"));
  while(not f.target->horizon().asleep)
  {
    REQUIRE_FALSE(f.run({.max_instructions = icount{50}}).stopped.has_value());
  }
  auto const h = f.target->horizon();
  REQUIRE(h.wake.has_value());
  check_wakes_at(f, *h.wake);
}

TEST_CASE("an SAI fed by a bounded DMA block wakes at the underrun after its supply")
{
  stm32_fixture f{u575(), "sai_ll"};
  enter_main(f);
  f.set("pc", sym("sai_ll", "dma16_zero"));
  while(not f.target->horizon().asleep)
  {
    REQUIRE_FALSE(f.run({.max_instructions = icount{50}}).stopped.has_value());
  }
  write32(f, gpdma + 0x64, read32(f, gpdma + 0x64) & ~cr_tcie);
  auto const sai_a_imr = 0x4001'5400 + 0x04 + 0x10;
  write32(f, sai_a_imr, 1u);
  enable_irq(f, 90);
  auto const h = f.target->horizon();
  REQUIRE(h.wake.has_value());
  check_wakes_at(f, *h.wake);
}

TEST_CASE("an SAI whose DMA channel does not answer bounds at its next word point")
{
  stm32_fixture f{u575(), "sai_ll"};
  enter_main(f);
  f.set("pc", sym("sai_ll", "dma16_zero"));
  while(not f.target->horizon().asleep)
  {
    REQUIRE_FALSE(f.run({.max_instructions = icount{50}}).stopped.has_value());
  }
  write32(f, gpdma + 0x64, read32(f, gpdma + 0x64) & ~cr_tcie);
  write32(f, 0x4001'5400 + 0x14, 1u);
  enable_irq(f, 90);
  f.target->set_transmit_sources({{"SAI1"}});
  auto const answered = f.target->horizon();
  REQUIRE(answered.wake.has_value());
  REQUIRE(answered.transmit.size() == 1);
  CHECK(answered.transmit[0] == answered.wake);
  popcorn::testing::set_debug_freeze(f, "DBG_GPDMA0_STOP", true);
  f.target->hold_core(true);
  auto const frozen = f.target->horizon();
  REQUIRE(frozen.transmit.size() == 1);
  REQUIRE(frozen.transmit[0].has_value());
  CHECK(*frozen.transmit[0] < *answered.wake);
  CHECK(*frozen.transmit[0] >= now(f));
}

TEST_CASE("every sleep of the SAI ring with its DMA receiver wakes at or after its horizon")
{
  stm32_fixture f{u575(), "sai_ll"};
  enter_main(f);
  f.set("pc", sym("sai_ll", "dma"));
  auto const result = sweep(f, later(f, 20 * ms), {});
  CHECK(result.sleeps > 20);
  CHECK(result.late_wakes == 0);
  CHECK(result.early_stops == 0);
}

TEST_CASE("every sleep of a USB device enumerating wakes at or after its horizon")
{
  auto         options = popcorn::testing::stm32_options();
  stm32::board board;
  board.usb_hosts.emplace("OTG_FS", stm32::usb_host_options{.attached = true});
  auto factories = stm32::factories(board, u575());
  options.models.emplace("OTG_FS", std::move(factories.at("OTG_FS")));
  options.spin_threshold = 0;
  stm32_fixture f{u575(), "usb_cdc", std::move(options)};
  auto const    result = sweep(f, later(f, 300 * ms), {});
  CHECK(result.sleeps > 100);
  CHECK(result.late_wakes == 0);
  CHECK(result.early_stops == 0);
}

TEST_CASE("every sleep of the linked controller wakes and transmits at or after its horizon")
{
  stm32_fixture                             f{u575(), "linked_controller"};
  std::array<std::shared_ptr<frame_log>, 2> logs{
      std::make_shared<frame_log>(), std::make_shared<frame_log>()};
  dynamic_cast<periph::uart_port*>(f.target->model("UART4"))->attach(logs[0]);
  dynamic_cast<periph::uart_port*>(f.target->model("USART3"))->attach(logs[1]);
  f.target->set_transmit_sources({{"UART4"}, {"USART3"}});
  std::array<frame_log const*, 2> wires{logs[0].get(), logs[1].get()};
  auto const                      result = sweep(f, later(f, 400 * ms), wires);
  CHECK(result.sleeps > 100);
  CHECK(result.late_wakes == 0);
  CHECK(result.early_stops == 0);
  CHECK(result.frames > 0);
  CHECK(result.late_frames == 0);
}

TEST_CASE("a native model with no declared role bounds the stop at its event and stops there")
{
  pulses      p{std::nullopt};
  auto const& f = *p.f;
  park(f, wfi_loop);
  p.rng.model->stop_on_event = true;
  auto const at              = later(f, 25 * us);
  p.rng.model->arm(at);
  auto const h = f.target->horizon();
  CHECK(h.stop == std::optional{at});
  CHECK(h.wake == std::optional{at});
  auto const before  = f.target->executed();
  auto const stopped = f.run_to_stop();
  CHECK(stopped.reason == stop_reason::user);
  CHECK(now(f) == at);
  CHECK(f.target->executed() == before);
}

TEST_CASE("b . under a code hook is awake and the hook stops it at its next arrival")
{
  pulses      p;
  auto const& f = *p.f;
  park(f, self_loop);
  f.set("primask", 1);
  REQUIRE(f.target->horizon(icount{1'000}).asleep);
  auto const hook = f.target->add_intercept(stub, [](core::intercept_call const&) {
    return core::intercept_result::stop(stop_reason::user);
  });
  auto const h    = f.target->horizon(icount{1'000});
  CHECK_FALSE(h.asleep);
  CHECK_FALSE(h.wake.has_value());
  auto const before  = f.target->executed();
  auto const t0      = now(f);
  auto const stopped = f.run_to_stop({.budget_left = icount{1'000}});
  CHECK(stopped.reason == stop_reason::user);
  CHECK(f.target->executed().value <= before.value + 1);
  CHECK(now(f).ps <= t0.ps + instruction_ps);
}

TEST_CASE("a synchronous SAI receiver of a waiting master wakes at or before its DMA TC")
{
  stm32_fixture f{u575(), "sai_ll"};
  enter_sai_setup(f);
  write32(f, gpdma + 0x98, 64);
  enable_irq(f, gpdma1_irq);
  channel(f, 1, tr1_words | tr1_dinc, 37, 16, sai1_b + 0x1c, target_words, cr_tcie | cr_enable);
  set_bits(f, sai1_b, sai_enable);
  set_bits(f, sai1_a, sai_enable);
  write32(f, gpdma + 0x64, cr_enable);
  park(f, wfi_loop);
  auto const h = f.target->horizon();
  REQUIRE(h.asleep);
  REQUIRE(h.wake.has_value());
  check_asleep_until(f, *h.wake);
  run_one(f, vtime{h.wake->ps + 10 * ms});
  CHECK(now(f) > *h.wake);
  CHECK((read32(f, gpdma + 0xe0) & (1u << 8)) != 0);
}

TEST_CASE("a waiting SAI master with only OVRUDRIE wakes at or before its underrun")
{
  stm32_fixture f{u575(), "sai_ll"};
  enter_sai_setup(f);
  write32(f, gpdma + 0x98, 4);
  write32(f, sai1_a + 0x10, 1u);
  enable_irq(f, sai1_irq);
  set_bits(f, sai1_a, sai_enable);
  write32(f, gpdma + 0x64, cr_enable);
  park(f, wfi_loop);
  auto const h = f.target->horizon();
  REQUIRE(h.asleep);
  REQUIRE(h.wake.has_value());
  check_asleep_until(f, *h.wake);
  run_one(f, vtime{h.wake->ps + 10 * ms});
  CHECK(now(f) > *h.wake);
  CHECK((read32(f, sai1_a + 0x14) & 1u) != 0);
}

TEST_CASE("under TrustZone a waking TZIC makes an active channel wake at its next burst")
{
  auto options           = popcorn::testing::stm32_options();
  options.spin_threshold = 0;
  REQUIRE(stm32::configure_option_bytes(u575(), {.tzen = true}, options).has_value());
  stm32_fixture     f{u575(), "boot_ll", std::move(options)};
  constexpr address secure_stub = 0x3006'0000;
  constexpr address handler     = 0x3006'0100;
  constexpr address table       = 0x3006'4000;
  for(std::uint32_t entry = 0; entry < 16 + 128; ++entry)
  {
    write32(f, table + 4 * entry, static_cast<std::uint32_t>(handler | 1u));
  }
  REQUIRE(f.target->mem().write(handler, 2, self_loop[0]).has_value());
  write32(f, 0xe000'ed08, static_cast<std::uint32_t>(table));
  park(f, wfi_loop, 1, secure_stub);
  enable_irq(f, gtzc_irq);
  auto const [ier, ie]  = field_at("GTZC1_TZIC", "SRAM3IE");
  auto const [sr, flag] = field_at("GTZC1_TZIC", "SRAM3F");
  set_bits(f, ier, ie);
  channel0(f, tr1_words | tr1_sinc | tr1_dinc, tr2_swreq, 4, source_words, target_words, cr_enable);
  auto const h = f.target->horizon();
  REQUIRE(h.wake.has_value());
  CHECK(*h.wake == later(f, burst_ps));
  check_wakes_at(f, *h.wake);
  CHECK((read32(f, sr) & flag) != 0);
}

TEST_CASE("a GPDMA channel fed by USART frames wakes at the closed form of its feed gap")
{
  pulses      p;
  auto const& f = *p.f;
  park(f, wfi_loop);
  usart_rx_dma(f, false, 5);
  write32(f, gpdma + 0x64, cr_tcie | cr_enable);
  auto const t0 = now(f);
  receive_frames(f, 8);
  auto const h = f.target->horizon();
  REQUIRE(h.wake.has_value());
  CHECK(*h.wake == vtime{t0.ps + 3 * frame_ps + burst_ps});
  check_asleep_until(f, *h.wake);
  run_one(f, vtime{h.wake->ps + 10 * ms});
  CHECK(now(f).ps >= t0.ps + 5 * frame_ps);
  CHECK((read32(f, gpdma + 0x60) & (1u << 8)) != 0);
}

TEST_CASE("a GPDMA channel draining held USART frames wakes at its fifth burst")
{
  pulses      p;
  auto const& f = *p.f;
  park(f, wfi_loop);
  usart_rx_dma(f, true, 5);
  receive_frames(f, 8);
  std::ignore = run_until(f, later(f, 10 * frame_ps));
  write32(f, gpdma + 0x64, cr_tcie | cr_enable);
  auto const h = f.target->horizon();
  REQUIRE(h.wake.has_value());
  CHECK(*h.wake == later(f, 5 * burst_ps));
  check_wakes_at(f, *h.wake);
}

TEST_CASE("a GPDMA channel reading a range a model watches wakes at its next burst")
{
  pulses            p;
  auto const&       f     = *p.f;
  constexpr address flash = 0x0800'8000;
  park(f, wfi_loop);
  channel0(f, tr1_words | tr1_sinc | tr1_dinc, tr2_swreq, 16, flash, target_words, cr_enable);
  CHECK_FALSE(f.target->horizon().wake.has_value());
  p.rng.model->watch_reads(flash + 32, 16);
  CHECK_FALSE(f.target->horizon().wake.has_value());
  p.rng.model->watch_reads(flash + 8, 16);
  CHECK(f.target->horizon().wake == std::optional{later(f, burst_ps)});
}

TEST_CASE("a GPDMA channel through a firewall that faults transfers wakes at its first burst")
{
  auto options           = popcorn::testing::stm32_options();
  options.spin_threshold = 0;
  options.models.emplace("GTZC1_TZSC", [](periph::model_context& ctx, auto const&) {
    return std::shared_ptr<periph::model>{
        std::make_shared<denying_firewall>(ctx, target_words, target_words + 0xff)};
  });
  stm32_fixture f{u575(), "boot_ll", std::move(options)};
  park(f, wfi_loop);
  enable_irq(f, gpdma0_irq);
  channel0(
      f, tr1_words | tr1_sinc | tr1_dinc, tr2_swreq, 64, source_words, target_words,
      cr_dteie | cr_tcie | cr_enable);
  auto const h = f.target->horizon();
  REQUIRE(h.wake.has_value());
  CHECK(*h.wake == later(f, burst_ps));
  check_wakes_at(f, *h.wake);
  CHECK((read32(f, gpdma + 0x60) & (1u << 10)) != 0);
  CHECK((read32(f, gpdma + 0x60) & (1u << 8)) == 0);
}

TEST_CASE("a GPDMA channel fed by USART frames keeps its feed gap while a model watches reads")
{
  pulses      p;
  auto const& f = *p.f;
  park(f, wfi_loop);
  p.rng.model->watch_reads(0x0800'8000, 16);
  usart_rx_dma(f, false, 5);
  write32(f, gpdma + 0x64, cr_tcie | cr_enable);
  auto const t0 = now(f);
  receive_frames(f, 8);
  auto const h = f.target->horizon();
  REQUIRE(h.wake.has_value());
  CHECK(*h.wake == vtime{t0.ps + 3 * frame_ps + burst_ps});
}

TEST_CASE("under TrustZone a channel the GTZC filters with its TZIC quiet wakes at its TC")
{
  auto options           = popcorn::testing::stm32_options();
  options.spin_threshold = 0;
  REQUIRE(stm32::configure_option_bytes(u575(), {.tzen = true}, options).has_value());
  stm32_fixture     f{u575(), "boot_ll", std::move(options)};
  constexpr address secure_stub = 0x3006'0000;
  park(f, wfi_loop, 1, secure_stub);
  enable_irq(f, gpdma0_irq);
  channel0(
      f, tr1_words | tr1_sinc | tr1_dinc, tr2_swreq, 16, source_words, target_words,
      cr_tcie | cr_enable);
  auto const h = f.target->horizon();
  REQUIRE(h.wake.has_value());
  CHECK(*h.wake == later(f, 4 * burst_ps));
  check_wakes_at(f, *h.wake);
  CHECK((read32(f, gpdma + 0x60) & (1u << 8)) != 0);
}

TEST_CASE("a 2D channel walking planar buffers with DTEIE wakes at its half transfer")
{
  pulses      p;
  auto const& f = *p.f;
  park(f, wfi_loop);
  enable_irq(f, gpdma12_irq);
  channel_2d(f, planar_ch, {});
  auto const h = f.target->horizon();
  REQUIRE(h.wake.has_value());
  CHECK(*h.wake == later(f, 8 * burst_ps));
  check_wakes_at(f, *h.wake);
  CHECK((read32(f, channel_base(planar_ch) + 0x10) & htf) != 0);
}

TEST_CASE("a 2D channel whose later blocks leave SRAM wakes at its next burst")
{
  pulses            p;
  auto const&       f     = *p.f;
  constexpr address start = 0x200b'e800;
  constexpr address out   = start + 2 * 4094;
  REQUIRE(f.target->mem().read(start + 4094 + 1024, 2).has_value());
  REQUIRE_FALSE(f.target->mem().read(out, 2).has_value());
  park(f, wfi_loop);
  enable_irq(f, gpdma12_irq);
  auto const t0 = now(f);
  channel_2d(f, planar_ch, {.br1 = 4u | ((planar_blocks - 1) << 16), .from = start});
  auto const h = f.target->horizon();
  REQUIRE(h.wake.has_value());
  CHECK(*h.wake == later(f, burst_ps));
  check_asleep_until(f, vtime{t0.ps + 4 * burst_ps});
  std::ignore = run_until(f, vtime{t0.ps + 5 * burst_ps + 2 * instruction_ps});
  CHECK((read32(f, channel_base(planar_ch) + 0x10) & (1u << 10)) != 0);
}

TEST_CASE("a 2D channel whose later destination blocks leave SRAM wakes at its next burst")
{
  pulses            p;
  auto const&       f     = *p.f;
  constexpr address start = 0x200b'e800;
  REQUIRE_FALSE(f.target->mem().read(start + 2 * 4094, 2).has_value());
  park(f, wfi_loop);
  enable_irq(f, gpdma12_irq);
  auto const t0 = now(f);
  channel_2d(
      f, planar_ch,
      {.tr1  = 2u | (1u << 16) | tr1_dinc,
       .br1  = 8u | ((planar_blocks - 1) << 16),
       .from = source_words,
       .to   = start,
       .tr3  = 1022u << 16,
       .br2  = 2046u << 16});
  auto const h = f.target->horizon();
  REQUIRE(h.wake.has_value());
  CHECK(*h.wake == later(f, burst_ps));
  check_asleep_until(f, vtime{t0.ps + 4 * burst_ps});
  std::ignore = run_until(f, vtime{t0.ps + 5 * burst_ps + 2 * instruction_ps});
  CHECK((read32(f, channel_base(planar_ch) + 0x10) & (1u << 10)) != 0);
}

TEST_CASE("a 2D channel past its half transfer wakes at the closed form of its item's TC")
{
  slot<ticking_requester> adc;
  stm32_fixture           f{u575(), "boot_ll", adc_requester(adc)};
  park(f, wfi_loop);
  auto const first = later(f, 10 * us);
  adc.model->arm(first, request_ps, 2 * planar_blocks);
  channel_2d(f, planar_ch, {.tr2 = adc_request(f) | tcem_item});
  std::ignore = run_until(f, vtime{first.ps + 8 * request_ps + request_ps / 2});
  rearm_line(f, planar_ch, gpdma12_irq);
  auto const h = f.target->horizon();
  REQUIRE(h.wake.has_value());
  CHECK(*h.wake == vtime{first.ps + 9 * request_ps + 6 * burst_ps});
  check_asleep_until(f, vtime{first.ps + 15 * request_ps});
  std::ignore = run_until(f, vtime{first.ps + 15 * request_ps + burst_ps + 2 * instruction_ps});
  CHECK((read32(f, channel_base(planar_ch) + 0x10) & (1u << 8)) != 0);
}

TEST_CASE("a 2D channel in block mode past its half transfer wakes at the next block's half")
{
  slot<ticking_requester> adc;
  stm32_fixture           f{u575(), "boot_ll", adc_requester(adc)};
  park(f, wfi_loop);
  auto const first = later(f, 10 * us);
  adc.model->arm(first, request_ps, 16);
  channel_2d(
      f, planar_ch,
      {.tr1    = 1u | tr1_sinc | (1u << 16) | tr1_dinc,
       .tr2    = adc_request(f),
       .br1    = 8u | (3u << 16),
       .tr3    = 0,
       .br2    = 0,
       .linked = false,
       .cr     = cr_htie | cr_enable});
  std::ignore = run_until(f, vtime{first.ps + 2 * request_ps + request_ps / 2});
  rearm_line(f, planar_ch, gpdma12_irq);
  auto const h = f.target->horizon();
  REQUIRE(h.wake.has_value());
  CHECK(*h.wake == vtime{first.ps + 3 * request_ps + 2 * burst_ps});
  check_asleep_until(f, vtime{first.ps + 5 * request_ps});
  std::ignore = run_until(f, vtime{first.ps + 5 * request_ps + burst_ps + 2 * instruction_ps});
  CHECK((read32(f, channel_base(planar_ch) + 0x10) & htf) != 0);
}
