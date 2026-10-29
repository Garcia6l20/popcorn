#include "factories.hpp"
#include "gtzc.hpp"
#include "model_base.hpp"
#include "pin_range.hpp"
#include "trigger_signal.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/scheduler.hpp>
#include <popcorn/periph/stm32/gpio.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
gpio_port::~gpio_port()   = default;
exti_input::~exti_input() = default;

namespace
{
constexpr std::size_t pins = 16;

constexpr std::array<std::pair<std::string_view, std::string_view>, 12> pin_fields{
    {
     {"MODER", "MODE"},
     {"OTYPER", "OT"},
     {"OSPEEDR", "OSPEED"},
     {"PUPDR", "PUPD"},
     {"IDR", "ID"},
     {"ODR", "OD"},
     {"BSRR", "BS"},
     {"BSRR", "BR"},
     {"BRR", "BR"},
     {"LCKR", "LCK"},
     {"HSLVR", "HSLV"},
     {"SECCFGR", "SEC"},
     }
};
constexpr std::uint32_t edge_due = 0;
constexpr std::uint8_t  released = 2;

struct pending_edge
{
  std::uint64_t board_ps{};
  std::uint64_t id{};
  std::uint8_t  pin{};
  std::uint8_t  level{};

  friend auto operator==(pending_edge const&, pending_edge const&) -> bool = default;
};

struct gpio_state
{
  std::uint32_t             driven{};
  std::uint32_t             driven_high{};
  std::uint32_t             lock_step{};
  std::uint32_t             lock_value{};
  std::vector<pending_edge> edges;
  std::uint32_t             fired{};
  std::uint64_t             next_edge{};

  friend auto operator==(gpio_state const&, gpio_state const&) -> bool = default;
};

using listener_entry =
    std::pair<std::uint64_t, std::shared_ptr<std::move_only_function<void(std::uint8_t, bool)>>>;

struct listener_table
{
  std::uint64_t               next{};
  std::vector<listener_entry> entries;
};

/**
 * @brief GPIO port of the STM32U5 and STM32H5, RM0456 section 13 and RM0481 section 13. Pin levels
 *        follow the output register, external drives and pulls. BSRR and BRR set and reset outputs,
 *        a set wins over a reset of the same pin. A port the variant gives fewer than 16 pins, H5
 *        port I with 12, has the per-pin fields of its pins only, the description checked at
 *        attach, and its other pins read 0. The LCKR key sequence freezes the configuration of the
 *        locked pins until reset. Level changes reach board listeners, then the EXTI model. Edges
 *        scheduled with drive_at() apply through events on core::host_lane, kept in the state with
 *        their board time, rescheduled at every reset at the same instant. A pin in alternate
 *        function mode carries its level to the signal its AFSEL selects in the description's
 *        alternates, entries with a pin or AF above 15 ignored: every change of a signal's level,
 *        from a pin edge, a mode or AFSEL write or a pull, reaches the trigger_sink peers that take
 *        it from a pin, trigger_sink::takes_pin, at the time of the edge. A signal two pins of the
 *        port carry reads high when either does. Entries whose signal no peer takes are dropped
 *        once the peers exist, so a pin in USART, SPI or LPTIM alternate function mode costs a
 *        write nothing and reaches no ADC trigger of the same name.
 *
 *        TrustZone, RM0456 13.3.18 and Table 128: a Non-secure access reads the fields of a pin
 *        whose `GPIOx_SECCFGR` bit is set as 0 and cannot write them, in MODER, OTYPER,
 *        OSPEEDR, PUPDR, IDR, ODR, BSRR, LCKR, AFRL, AFRH, BRR and HSLVR, with no illegal access
 *        event. `GPIOx_SECCFGR` reads for every transaction and takes Secure writes only.
 *        Security filters bus access only: the pins drive and sense the same. Its reset is the
 *        description's, except `GPIOI_SECCFGR` 0x00FF on the STM32U575 and STM32U585, RM0456
 *        13.4.13, whose port I has 8 pins. Under TZEN=0 `GPIOx_SECCFGR` is RAZ/WI, its storage
 *        keeps the description's reset. A `GPIOx_SECCFGR` write with the port clock off lands,
 *        the model has no RCC clock gating.
 */
class gpio final : public dispatch_model<gpio, gpio_state>,
                   public gpio_port,
                   public trustzone_aware,
                   public bus_reset_listener
{
public:
  gpio(model_context& ctx, peripheral_desc const& desc, gpio_variant const& variant);

  void reset(register_file& regs, reset_kind kind) override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
  void on_bus_reset() override;

  void               drive(std::uint8_t pin, std::optional<bool> level) override;
  void               drive_at(std::uint8_t pin, std::optional<bool> level, vtime at) override;
  [[nodiscard]] auto pending_edges() const -> std::size_t override;
  [[nodiscard]] auto level(std::uint8_t pin) const -> bool override;
  [[nodiscard]] auto mode(std::uint8_t pin) const -> pin_mode override;
  [[nodiscard]] auto alternate(std::uint8_t pin) const -> std::uint8_t override;
  [[nodiscard]] auto on_change(std::move_only_function<void(std::uint8_t pin, bool level)> listener)
      -> subscription override;
  [[nodiscard]] auto signal_level(std::string_view signal) const -> std::optional<bool> override;
  void               load(std::span<std::byte const> in) override;
  [[nodiscard]] auto secure_function() const -> bool override;
  [[nodiscard]] auto secure_word() const -> std::uint32_t override;
  [[nodiscard]] auto guard(std::uint32_t word, access_ctx const& ctx, bool write) const
      -> bus_guard;

  template <std::size_t Which> void config_written(std::uint32_t old, std::uint32_t now);
  void                              afrh_written(std::uint32_t old, std::uint32_t now);
  void                              output_written(std::uint32_t old, std::uint32_t now);
  void                              input_written(std::uint32_t old, std::uint32_t now);
  void                              set_reset_written(std::uint32_t old, std::uint32_t now);
  void                              reset_written(std::uint32_t old, std::uint32_t now);
  void                              lock_written(std::uint32_t old, std::uint32_t now);

private:
  [[nodiscard]] auto computed() const -> std::uint32_t;
  [[nodiscard]] auto board_base() const -> std::uint64_t;
  [[nodiscard]] auto pending() const -> std::span<pending_edge const>;
  void               apply(std::uint8_t pin, std::optional<bool> level);
  void               settle(bool notify);
  void               restore_reset_values();
  [[nodiscard]] auto routed() const -> std::vector<std::uint8_t>;
  void               route_pins();
  void               check_pin(std::uint8_t pin) const;
  [[nodiscard]] auto pin_count() const noexcept -> std::uint8_t override
  {
    return port_pins_;
  }
  [[nodiscard]] auto bonded_pins() const noexcept -> std::uint16_t override
  {
    return bonded_;
  }
  void forward();
  auto sinks() -> std::span<trigger_sink* const>;

  register_file&                                                         regs_;
  std::uint8_t                                                           port_;
  std::array<field_ref, pins>                                            mode_{};
  std::array<field_ref, pins>                                            type_{};
  std::array<field_ref, pins>                                            speed_{};
  std::array<field_ref, pins>                                            pull_{};
  std::array<field_ref, pins>                                            alternate_{};
  std::uint8_t                                                           port_pins_{};
  std::uint16_t                                                          bonded_{};
  std::array<field_ref, pins>                                            input_{};
  std::array<field_ref, pins>                                            output_{};
  std::array<field_ref, pins>                                            set_{};
  std::array<field_ref, pins>                                            clear_{};
  std::array<field_ref, pins>                                            brr_{};
  std::array<field_ref, pins>                                            lock_{};
  field_ref                                                              lock_key_;
  reg_ref                                                                secure_pins_;
  std::uint32_t                                                          secure_reset_{};
  std::vector<std::pair<std::uint32_t, std::array<std::uint32_t, pins>>> pin_bits_;
  std::uint32_t                                                          reported_{};
  std::uint32_t                                                          notifying_{};
  std::shared_ptr<listener_table>           listeners_ = std::make_shared<listener_table>();
  std::vector<std::string>                  signals_;
  std::vector<std::array<std::uint16_t, 3>> alternates_;
  std::vector<std::uint8_t>                 forwarded_;
  std::array<std::uint16_t, pins>           af_entries_{};
  std::uint16_t                             routed_pins_{};
  bool                                      forwarding_{};
  std::optional<std::uint64_t>              edge_at_;
  std::vector<trigger_sink*>                sinks_;
  bool                                      sinks_resolved_{};
};

constexpr std::array<write_entry<gpio>, 12> gpio_writes{
    {
     {"MODER", &gpio::config_written<0>},
     {"OTYPER", &gpio::config_written<1>},
     {"OSPEEDR", &gpio::config_written<2>},
     {"PUPDR", &gpio::config_written<3>},
     {"IDR", &gpio::input_written},
     {"ODR", &gpio::output_written},
     {"BSRR", &gpio::set_reset_written},
     {"LCKR", &gpio::lock_written},
     {"AFRL", &gpio::config_written<4>},
     {"BRR", &gpio::reset_written},
     {"SECCFGR", nullptr},
     {"AFRH", &gpio::afrh_written},
     }
};

constexpr std::array<write_entry<gpio>, 1> gpio_trustzone_writes{
    {
     {"HSLVR", nullptr},
     }
};

gpio::gpio(model_context& ctx, peripheral_desc const& desc, gpio_variant const& variant)
    : dispatch_model{ctx, desc, "GPIO", gpio_writes, gpio_trustzone_writes}, regs_{ctx.regs},
      port_{static_cast<std::uint8_t>(desc.name.back() - 'A')}, lock_key_{field("LCKR", "LCKK")},
      secure_pins_{reg("SECCFGR")}, secure_reset_{describe("SECCFGR").reset}
{
  port_pins_          = desc.name.back() == variant.short_port ? variant.short_port_pins : pins;
  auto const all_pins = static_cast<std::uint16_t>((std::uint32_t{1} << port_pins_) - 1);
  bonded_             = desc.bonded.value_or(all_pins);
  if((bonded_ & ~all_pins) != 0)
  {
    throw config_error(
        desc.name, 0, "bonded", std::format("a mask of pins 0 to {}", port_pins_ - 1));
  }
  expect_register("AFRH", true);
  for(std::size_t pin = 0; pin < pins; ++pin)
  {
    auto const n       = std::to_string(pin);
    auto const present = pin < port_pins_;
    for(auto const& [reg_name, prefix] : pin_fields)
    {
      expect_field(reg_name, std::string{prefix} + n, present);
    }
    expect_field(pin < 8 ? "AFRL" : "AFRH", "AFSEL" + n, present);
  }
  for(std::size_t pin = 0; pin < port_pins_; ++pin)
  {
    auto const n    = std::to_string(pin);
    mode_[pin]      = field("MODER", "MODE" + n);
    type_[pin]      = field("OTYPER", "OT" + n);
    speed_[pin]     = field("OSPEEDR", "OSPEED" + n);
    pull_[pin]      = field("PUPDR", "PUPD" + n);
    alternate_[pin] = field(pin < 8 ? "AFRL" : "AFRH", "AFSEL" + n);
    input_[pin]     = field("IDR", "ID" + n);
    output_[pin]    = field("ODR", "OD" + n);
    set_[pin]       = field("BSRR", "BS" + n);
    clear_[pin]     = field("BSRR", "BR" + n);
    brr_[pin]       = field("BRR", "BR" + n);
    lock_[pin]      = field("LCKR", "LCK" + n);
  }
  auto const add = [this](field_ref f, std::size_t pin) {
    auto found = std::ranges::find(pin_bits_, f.offset, &decltype(pin_bits_)::value_type::first);
    if(found == pin_bits_.end())
    {
      pin_bits_.emplace_back(f.offset, std::array<std::uint32_t, pins>{});
      found = pin_bits_.end() - 1;
    }
    found->second[pin] |= f.mask();
  };
  for(std::size_t pin = 0; pin < port_pins_; ++pin)
  {
    for(auto const* fields :
        {&mode_, &type_, &speed_, &pull_, &alternate_, &input_, &output_, &set_, &clear_, &brr_,
         &lock_})
    {
      add((*fields)[pin], pin);
    }
    add(field("HSLVR", "HSLV" + std::to_string(pin)), pin);
  }
  for(auto const& entry : desc.alternates.value_or(std::vector<alternate_desc>{}))
  {
    if(entry.pin >= pins or entry.af >= 16)
    {
      continue;
    }
    auto found = std::ranges::find(signals_, entry.signal);
    if(found == signals_.end())
    {
      if(signals_.size() >= std::numeric_limits<std::uint16_t>::max())
      {
        throw config_error(
            ctx.part.part, 0, std::format("peripherals.{}.alternates", desc.name),
            "at most 65535 distinct signals on one port");
      }
      signals_.push_back(entry.signal);
      found = signals_.end() - 1;
    }
    alternates_.push_back(
        {entry.pin, entry.af, static_cast<std::uint16_t>(found - signals_.begin())});
  }
  forwarded_.assign(signals_.size(), 0);
}

void gpio::restore_reset_values()
{
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
  if(ctx_.trustzone)
  {
    regs_.poke(secure_pins_.offset, secure_reset_);
  }
  state_.lock_step  = 0;
  state_.lock_value = 0;
}

void gpio::on_bus_reset()
{
  auto const inputs = regs_.peek(input_[0].offset);
  restore_reset_values();
  regs_.poke(input_[0].offset, inputs);
  route_pins();
  settle(true);
}

void gpio::reset(register_file&, reset_kind)
{
  restore_reset_values();
  settle(false);
  auto const base = board_base();
  for(auto const& edge : pending())
  {
    auto const due = edge.board_ps > base ? edge.board_ps - base : 0;
    ctx_.events.schedule_at(vtime{due}, edge_due, edge.id, core::host_lane, ctx_.clock);
  }
}

auto gpio::board_base() const -> std::uint64_t
{
  return ctx_.clock.board_time().ps - ctx_.clock.time().ps;
}

auto gpio::pending() const -> std::span<pending_edge const>
{
  return std::span{state_.edges}.subspan(state_.fired);
}

void gpio::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind != edge_due)
  {
    return;
  }
  auto const live  = state_.edges.begin() + state_.fired;
  auto const found = std::ranges::find(live, state_.edges.end(), payload, &pending_edge::id);
  if(found == state_.edges.end())
  {
    return;
  }
  auto const edge = *found;
  if(found == live)
  {
    ++state_.fired;
    if(state_.fired * 2 >= state_.edges.size())
    {
      state_.edges.erase(state_.edges.begin(), state_.edges.begin() + state_.fired);
      state_.fired = 0;
    }
  }
  else
  {
    state_.edges.erase(found);
  }
  auto const base = board_base();
  edge_at_        = edge.board_ps > base ? edge.board_ps - base : ctx_.clock.time().ps;
  try
  {
    apply(edge.pin, edge.level == released ? std::nullopt : std::optional<bool>{edge.level != 0});
  }
  catch(...)
  {
    edge_at_.reset();
    throw;
  }
  edge_at_.reset();
}

void gpio::drive_at(std::uint8_t pin, std::optional<bool> level, vtime at)
{
  check_pin(pin);
  auto const now  = ctx_.clock.time().ps;
  auto const base = board_base();
  if(at.ps < now)
  {
    throw std::invalid_argument(
        std::format("a pin edge at {} ps lies before the machine time {} ps", at.ps, now));
  }
  if(at.ps > std::numeric_limits<std::uint64_t>::max() - base)
  {
    throw std::invalid_argument("a pin edge time leaves the 64-bit picosecond range");
  }
  if(pending_edges() >= max_pending_edges)
  {
    throw config_error(
        desc_.name, 0, "edges", std::format("at most {} pending pin edges", max_pending_edges));
  }
  auto const edge = pending_edge{
      .board_ps = base + at.ps,
      .id       = ++state_.next_edge,
      .pin      = pin,
      .level    = level ? static_cast<std::uint8_t>(*level ? 1 : 0) : released};
  state_.edges.insert(
      std::ranges::upper_bound(
          state_.edges.begin() + state_.fired, state_.edges.end(), edge.board_ps, {},
          &pending_edge::board_ps),
      edge);
  ctx_.events.schedule_at(at, edge_due, edge.id, core::host_lane, ctx_.clock);
}

auto gpio::pending_edges() const -> std::size_t
{
  return state_.edges.size() - state_.fired;
}

auto gpio::mode(std::uint8_t pin) const -> pin_mode
{
  return static_cast<pin_mode>(get(regs_, mode_[pin]));
}

auto gpio::alternate(std::uint8_t pin) const -> std::uint8_t
{
  return static_cast<std::uint8_t>(get(regs_, alternate_[pin]));
}

auto gpio::level(std::uint8_t pin) const -> bool
{
  return get(regs_, input_[pin]) != 0;
}

auto gpio::computed() const -> std::uint32_t
{
  std::uint32_t levels = 0;
  for(std::uint8_t pin = 0; pin < pins; ++pin)
  {
    auto const bit            = std::uint32_t{1} << pin;
    auto const driven         = (state_.driven & bit) != 0;
    auto const pulled         = get(regs_, pull_[pin]) == 1;
    auto const floating_level = driven ? (state_.driven_high & bit) != 0 : pulled;
    auto       high           = false;
    switch(mode(pin))
    {
      case pin_mode::output:
        if(get(regs_, output_[pin]) == 0)
        {
          high = false;
        }
        else
        {
          high = get(regs_, type_[pin]) == 0 or floating_level;
        }
        break;
      case pin_mode::input:
      case pin_mode::alternate:
        high = floating_level;
        break;
      case pin_mode::analog:
        high = false;
        break;
    }
    levels |= high ? bit : 0;
  }
  return levels;
}

void gpio::route_pins()
{
  if(not sinks_resolved_ and not ctx_.peers.all().empty())
  {
    std::erase_if(alternates_, [this](auto const& entry) {
      return std::ranges::none_of(
          sinks(), [&](trigger_sink* sink) { return sink->takes_pin(signals_[entry[2]]); });
    });
    for(auto const& [pin, af, signal] : alternates_)
    {
      af_entries_[pin] |= static_cast<std::uint16_t>(1u << af);
    }
  }
  routed_pins_ = 0;
  for(std::uint8_t pin = 0; pin < pins; ++pin)
  {
    if(mode(pin) == pin_mode::alternate and ((af_entries_[pin] >> alternate(pin)) & 1u) != 0)
    {
      routed_pins_ |= static_cast<std::uint16_t>(1u << pin);
    }
  }
}

auto gpio::routed() const -> std::vector<std::uint8_t>
{
  std::vector<std::uint8_t> levels(signals_.size());
  if(routed_pins_ == 0)
  {
    return levels;
  }
  for(auto const& [pin, af, signal] : alternates_)
  {
    if(((routed_pins_ >> pin) & 1u) != 0 and alternate(pin) == af and level(pin))
    {
      levels[signal] = 1;
    }
  }
  return levels;
}

auto gpio::sinks() -> std::span<trigger_sink* const>
{
  if(not sinks_resolved_ and not ctx_.peers.all().empty())
  {
    sinks_resolved_ = true;
    for(auto* const peer : ctx_.peers.all())
    {
      if(auto* const sink = dynamic_cast<trigger_sink*>(peer))
      {
        sinks_.push_back(sink);
      }
    }
  }
  return sinks_;
}

void gpio::forward()
{
  if(routed_pins_ == 0 and not forwarding_)
  {
    return;
  }
  auto const before = std::exchange(forwarded_, routed());
  forwarding_       = routed_pins_ != 0;
  auto const after  = forwarded_;
  if(before == after)
  {
    return;
  }
  auto const at = edge_at_.value_or(ctx_.clock.time().ps);
  for(std::size_t signal = 0; signal < signals_.size(); ++signal)
  {
    if(before[signal] == after[signal])
    {
      continue;
    }
    auto const edge = after[signal] != 0 ? trigger_edge::rising : trigger_edge::falling;
    for(auto* const sink : sinks())
    {
      if(sink->takes_pin(signals_[signal]))
      {
        sink->triggered(signals_[signal], edge, at);
      }
    }
  }
}

auto gpio::signal_level(std::string_view signal) const -> std::optional<bool>
{
  auto const found = std::ranges::find(signals_, signal);
  if(found == signals_.end())
  {
    return std::nullopt;
  }
  return routed()[static_cast<std::size_t>(found - signals_.begin())] != 0;
}

void gpio::load(std::span<std::byte const> in)
{
  dispatch_model::load(in);
  route_pins();
  forwarded_  = routed();
  forwarding_ = routed_pins_ != 0;
}

void gpio::settle(bool notify)
{
  std::uint32_t before = 0;
  for(std::uint8_t pin = 0; pin < pins; ++pin)
  {
    before |= level(pin) ? std::uint32_t{1} << pin : 0;
  }
  auto const after = computed();
  for(std::uint8_t pin = 0; pin < pins; ++pin)
  {
    update(regs_, input_[pin], after >> pin & 1u);
  }
  if(not notify)
  {
    route_pins();
    forwarded_  = routed();
    forwarding_ = routed_pins_ != 0;
    return;
  }
  if(before == after)
  {
    forward();
    return;
  }
  if(notifying_ == 0)
  {
    reported_ = before;
  }
  ++notifying_;
  auto* const                exti = dynamic_cast<exti_input*>(ctx_.peers.find("EXTI"));
  std::vector<std::uint64_t> ids;
  for(auto const& [id, listener] : listeners_->entries)
  {
    ids.push_back(id);
  }
  for(std::uint8_t pin = 0; pin < pins; ++pin)
  {
    auto const high = level(pin);
    if(((reported_ >> pin) & 1u) == (high ? 1u : 0u))
    {
      continue;
    }
    reported_ ^= std::uint32_t{1} << pin;
    for(auto const id : ids)
    {
      auto const found = std::ranges::find(listeners_->entries, id, &listener_entry::first);
      if(found != listeners_->entries.end())
      {
        auto const listener = found->second;
        (*listener)(pin, high);
      }
    }
    if(exti != nullptr)
    {
      exti->pin_changed(port_, pin, high);
    }
  }
  --notifying_;
  forward();
}

template <std::size_t Which> void gpio::config_written(std::uint32_t old, std::uint32_t now)
{
  constexpr std::array<std::array<std::size_t, 2>, 6> spans{
      {
       {0, pins},
       {0, pins},
       {0, pins},
       {0, pins},
       {0, pins / 2},
       {pins / 2, pins},
       }
  };
  auto const* const fields =
      std::array{&mode_, &type_, &speed_, &pull_, &alternate_, &alternate_}[Which];
  auto const [first, last] = spans[Which];
  if(get(regs_, lock_key_) != 0)
  {
    for(auto pin = first; pin < last; ++pin)
    {
      if(get(regs_, lock_[pin]) != 0)
      {
        now = (*fields)[pin].with(now, (*fields)[pin].of(old));
      }
    }
    regs_.poke((*fields)[first].offset, now);
  }
  route_pins();
  settle(true);
}

void gpio::afrh_written(std::uint32_t old, std::uint32_t now)
{
  if(port_pins_ <= pins / 2)
  {
    regs_.poke(reg("AFRH").offset, 0);
    return;
  }
  config_written<5>(old, now);
}

void gpio::output_written(std::uint32_t, std::uint32_t)
{
  settle(true);
}

void gpio::input_written(std::uint32_t old, std::uint32_t)
{
  regs_.poke(input_[0].offset, old);
}

void gpio::set_reset_written(std::uint32_t, std::uint32_t now)
{
  auto odr = regs_.peek(output_[0].offset);
  for(std::size_t pin = 0; pin < pins; ++pin)
  {
    if(clear_[pin].of(now) != 0)
    {
      odr = output_[pin].with(odr, 0);
    }
    if(set_[pin].of(now) != 0)
    {
      odr = output_[pin].with(odr, 1);
    }
  }
  regs_.poke(set_[0].offset, 0);
  regs_.poke(output_[0].offset, odr);
  settle(true);
}

void gpio::reset_written(std::uint32_t, std::uint32_t now)
{
  auto odr = regs_.peek(output_[0].offset);
  for(std::size_t pin = 0; pin < pins; ++pin)
  {
    if(brr_[pin].of(now) != 0)
    {
      odr = output_[pin].with(odr, 0);
    }
  }
  regs_.poke(brr_[0].offset, 0);
  regs_.poke(output_[0].offset, odr);
  settle(true);
}

void gpio::lock_written(std::uint32_t old, std::uint32_t now)
{
  if(lock_key_.of(old) != 0)
  {
    regs_.poke(lock_key_.offset, old);
    return;
  }
  std::uint32_t value = 0;
  for(std::size_t pin = 0; pin < pins; ++pin)
  {
    value |= lock_[pin].of(now) << pin;
  }
  auto const key = lock_key_.of(now) != 0;
  if(state_.lock_step == 1 and not key and value == state_.lock_value)
  {
    state_.lock_step = 2;
  }
  else if(state_.lock_step == 2 and key and value == state_.lock_value)
  {
    state_.lock_step = 0;
    regs_.poke(lock_key_.offset, lock_key_.with(now, 1));
    return;
  }
  else
  {
    state_.lock_step  = key ? 1 : 0;
    state_.lock_value = value;
  }
  regs_.poke(lock_key_.offset, lock_key_.with(now, 0));
}

auto gpio::secure_word() const -> std::uint32_t
{
  return ctx_.trustzone ? regs_.peek(secure_pins_.offset) & ((std::uint32_t{1} << pins) - 1u) : 0;
}

auto gpio::secure_function() const -> bool
{
  return secure_word() != 0;
}

auto gpio::guard(std::uint32_t word, access_ctx const& ctx, bool) const -> bus_guard
{
  if(word == secure_pins_.offset)
  {
    if(not ctx_.trustzone)
    {
      return bus_guard::both(~std::uint32_t{});
    }
    return {.wi = ctx.secure ? 0 : ~std::uint32_t{}};
  }
  if(not ctx_.trustzone or ctx.secure)
  {
    return {};
  }
  auto const secure = secure_word();
  auto const found  = std::ranges::find(pin_bits_, word, &decltype(pin_bits_)::value_type::first);
  if(secure == 0 or found == pin_bits_.end())
  {
    return {};
  }
  std::uint32_t hidden = 0;
  for(std::size_t pin = 0; pin < pins; ++pin)
  {
    if((secure >> pin & 1u) != 0)
    {
      hidden |= found->second[pin];
    }
  }
  return bus_guard::both(hidden);
}

void gpio::drive(std::uint8_t pin, std::optional<bool> level)
{
  check_pin(pin);
  apply(pin, level);
}

void gpio::check_pin(std::uint8_t pin) const
{
  if(pin >= port_pins_)
  {
    throw config_error(desc_.name, 0, "pin", pin_range_text(port_pins_));
  }
}

void gpio::apply(std::uint8_t pin, std::optional<bool> level)
{
  auto const bit     = std::uint32_t{1} << pin;
  state_.driven      = level ? state_.driven | bit : state_.driven & ~bit;
  state_.driven_high = level and *level ? state_.driven_high | bit : state_.driven_high & ~bit;
  settle(true);
}

auto gpio::on_change(std::move_only_function<void(std::uint8_t pin, bool level)> listener)
    -> subscription
{
  auto const id = listeners_->next++;
  listeners_->entries.emplace_back(
      id, std::make_shared<std::move_only_function<void(std::uint8_t, bool)>>(std::move(listener)));
  return subscription{[table = std::weak_ptr<listener_table>{listeners_}, id] {
    if(auto const live = table.lock())
    {
      std::erase_if(live->entries, [id](auto const& entry) { return entry.first == id; });
    }
  }};
}
}

auto gpio_factory(gpio_variant variant) -> model_factory
{
  return [variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    if(not desc.name.starts_with("GPIO"))
    {
      return nullptr;
    }
    return std::make_shared<gpio>(ctx, desc, variant);
  };
}
}
