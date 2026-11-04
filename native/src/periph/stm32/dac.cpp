#include "dma.hpp"
#include "dma_signal.hpp"
#include "factories.hpp"
#include "model_base.hpp"
#include "trigger_signal.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/periph/bus_reset.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace popcorn::periph::stm32
{
namespace
{
using wide = unsigned __int128;

constexpr std::size_t   channels  = 2;
constexpr std::size_t   depth     = 3;
constexpr std::uint32_t data_mask = 0xFFF;

enum class holding : std::uint8_t
{
  right12_1,
  left12_1,
  right8_1,
  right12_2,
  left12_2,
  right8_2,
  right12_dual,
  left12_dual,
  right8_dual,
};

struct dac_state
{
  std::uint64_t                               generation{};
  std::array<std::uint64_t, channels * depth> due_ps{};
  std::array<std::uint8_t, channels * depth>  triggered{};
  std::array<std::uint16_t, channels>         dhr{};
  std::array<std::uint8_t, channels>          requested{};

  friend auto operator==(dac_state const&, dac_state const&) -> bool = default;
};

enum class rule : std::uint8_t
{
  trigger_locked,
  amplitude_locked,
  calibration_locked,
  calibration_enable,
  wave_reserved,
  mode_locked,
  sample_hold,
  count,
};

struct unmodelled_use
{
  std::string_view field;
  std::string_view what;
};

constexpr std::array<unmodelled_use, 1> unmodelled_table{
    {
     {"WAVE", "WAVE, the noise and triangle waves"},
     }
};

using selection = std::array<std::optional<std::uint32_t>, channels>;

/**
 * @brief DAC1 of the STM32L4, RM0351 section 19, two 12-bit channels on the APB1 clock.
 *        A write to DAC_DHR12Rx, DAC_DHR12Lx, DAC_DHR8Rx or a dual register stores the data
 *        into the internal DHR1 and DHR2 by the shifts of RM0351 19.4.3, and every holding
 *        register then reads its view of DHR1 and DHR2, inferred. With TENx 0 DHRx loads into
 *        DAC_DORx one APB1 clock cycle after the write, RM0351 19.4.4, the cycle counted from
 *        the access time at the APB1 rate of the write, inferred. With TENx 1 and TSELx 111 a
 *        DHR write loads nothing, and each SWTRIGx write loads DORx one APB1 cycle later,
 *        RM0351 19.4.6. SWTRIGx follows the `w` access of RM0351 19.7.2 and reads 0, inferred
 *        against the 19.4.6 text of a bit reset by hardware at the load. Written with TENx 0
 *        or another TSELx it loads nothing, inferred. With TENx 1 and TSELx below 111 the
 *        channel listens, as a trigger_sink, to the signal of RM0351 Table 128 that TSELx
 *        selects: `tim6_trgo`, `tim8_trgo`, `tim7_trgo`, `tim5_trgo`, `tim2_trgo`, `tim4_trgo`
 *        or `exti9`. Each rising edge loads DORx three APB1 cycles after the edge, RM0351
 *        19.4.4 and 19.4.6, counted from the exact instant the source gives the edge, and a
 *        falling edge loads nothing. The load takes DHRx as it is at the load, inferred. The
 *        model keeps three pending loads per channel, in order, and a fourth before the first
 *        load moves the third to its own due. Three cover the APB1 timers, whose update needs
 *        ARR 1 or more, RM0351 33.4.8, so two kernel clocks, one APB1 cycle at the most. TIM8
 *        on APB2 and EXTI9 can give edges closer than that. Popcorn charges no APB wait states,
 *        so writes closer than one APB1 cycle happen only in the model. In that window a load
 *        takes DHRx as it is at the load, and the same three slots hold the writes' loads.
 *        DAC_DORx is read only. The digital interface works with ENx 0, RM0351 19.4.2 note, so
 *        loads and triggers do not depend on ENx.
 *        While ENx is 1 a DAC_CR write keeps the old TSELx, RM0351 19.4.6 note, the old MAMPx,
 *        RM0351 19.4.9, and the old CENx, RM0351 19.7.1, and DAC_MCR keeps the old MODEx while
 *        ENx or CENx is 1, RM0351 19.7.16. ENx set while CENx is 1, which RM0351 19.4.11 does
 *        not allow, and WAVEx 11, reserved by RM0351 19.7.1, keep the write. Each of these logs
 *        one model_warning per rule, and under strict_peripherals raises model_error and leaves
 *        the register as before the write.
 *        With CENx 1 CAL_FLAGx reads 1 when OTRIMx is at or above the variant's factory trim and
 *        MODEx enables the buffer, 000, 001, 100 or 101, RM0351 19.4.11 and 19.7.14, at once
 *        with no tTRIM delay. Otherwise it reads 0 with a buffer-off MODEx and keeps its value
 *        with CENx 0, inferred. The factory trim is the variant's value for each channel,
 *        inferred, as RM0351 19.7.15 gives 0x00XX 00XX. DAC_CCR resets to it. DMAUDRx clears
 *        on a write of 1. CAL_FLAGx and BWSTx are read only.
 *        With DMAENx 1 a load from a hardware trigger raises the `dac1_ch<x>_dma` request at the
 *        load instant, RM0351 19.4.7, the instant inferred. A load from a DHR write or SWTRIGx
 *        raises none, RM0351 19.4.7. The request drops when a DMA transfer serving it
 *        acknowledges it, or when DMAENx is cleared, inferred. A hardware load while the request
 *        is still high raises no new request and sets DMAUDRx, RM0351 19.4.7, decided at the
 *        load, inferred, and DORx still loads DHRx. DMAUDRx with DMAUDRIEx holds the DAC's
 *        contribution to TIM6_DAC_IRQn, position 54 of RM0351 Table 61, which TIM6 shares.
 *        WAVEx other than 00 is not modelled: a DAC_CR write that sets it logs one
 *        model_warning, and under strict_peripherals raises model_error and leaves DAC_CR as
 *        before the write. MODEx 1xx, the sample and hold mode, is not modelled the same way on
 *        DAC_MCR. The output is DAC_DORx only, no pin or ADC channel sees it, so MODEx selects
 *        nothing else.
 *        A Stop 2 entry with ENx 1, which RM0351 19.5 Table 131 forbids, reaches the model as
 *        nothing: the PWR model has no Stop entry for a peripheral to see.
 *        DAC1RST and a machine reset restore the claimed registers, drop the pending loads and
 *        the DMA requests, and lower the interrupt.
 */
class dac final : public dispatch_model<dac, dac_state>,
                  public bus_reset_listener,
                  public trigger_sink,
                  public dma_requester
{
public:
  dac(model_context& ctx, peripheral_desc const& desc, dac_variant const& variant);

  void reset(register_file& regs, reset_kind kind) override;
  void on_bus_reset() override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx access)
      -> std::expected<std::uint32_t, bus_error> override;
  auto on_write(
      register_file& regs,
      std::uint32_t  offset,
      std::uint8_t   size,
      std::uint32_t  old,
      std::uint32_t  value,
      access_ctx     access) -> std::expected<void, bus_error> override;
  [[nodiscard]] auto listens(std::string_view signal) const -> bool override;
  [[nodiscard]] auto can_listen(std::string_view signal) const -> bool override;
  void triggered(std::string_view signal, trigger_edge edge, trigger_time at) override;
  void transfer_completed(dma_controller const& by, std::uint32_t request) override;
  void acknowledged(dma_controller const& by, std::uint32_t request) override;

  void                          control_written(std::uint32_t old, std::uint32_t now);
  void                          mode_written(std::uint32_t old, std::uint32_t now);
  void                          trim_written(std::uint32_t old, std::uint32_t now);
  void                          status_written(std::uint32_t old, std::uint32_t now);
  void                          trigger_written(std::uint32_t old, std::uint32_t now);
  template <holding Which> void holding_written(std::uint32_t old, std::uint32_t now);

private:
  struct channel_fields
  {
    field_ref enable;
    field_ref trigger_enable;
    field_ref trigger_select;
    field_ref amplitude;
    field_ref wave;
    field_ref calibration;
    field_ref software;
    reg_ref   output;
    field_ref mode;
    field_ref trim;
    field_ref flag;
    field_ref underrun;
    field_ref dma;
    field_ref underrun_interrupt;
  };

  [[nodiscard]] auto now_ps() const -> std::uint64_t;
  [[nodiscard]] auto cycles_ps(std::uint64_t cycles) const -> std::uint64_t;
  [[nodiscard]] auto load_delay_ps(trigger_time const& at) const -> std::uint64_t;
  [[nodiscard]] auto software_selected(std::size_t channel) const -> bool;
  [[nodiscard]] auto listened(std::uint32_t control) const -> selection;
  void               notify(selection const& which);
  void               retune(selection const& was);
  void               store(std::size_t channel, std::uint32_t data);
  void               mirror();
  void               request(std::size_t channel, std::uint64_t due, bool hardware);
  void               catch_up(std::size_t channel, std::uint64_t ps);
  void               transfer(std::size_t channel, bool hardware);
  void               release(std::size_t channel);
  void               refresh_line();
  void               restore_reset_values();
  void               refresh_flags();
  void               restore_trim();
  void
      refuse(rule which, std::string_view name, reg_ref at, std::uint32_t old, std::string message);

  dac_variant                                       variant_;
  register_file&                                    regs_;
  reg_ref                                           control_;
  reg_ref                                           mode_;
  reg_ref                                           status_;
  std::array<reg_ref, 9>                            holding_;
  std::array<channel_fields, 2>                     channel_;
  std::array<field_ref, 2>                          unmodelled_fields_;
  core::clock_domain                                bus_;
  irq_line*                                         line_;
  std::array<dma_signal, channels>                  requests_;
  std::array<bool, unmodelled_table.size()>         unmodelled_logged_{};
  std::array<bool, std::to_underlying(rule::count)> rule_logged_{};
};

constexpr std::array<write_entry<dac>, 16> dac_writes{
    {
     {"CR", &dac::control_written},
     {"MCR", &dac::mode_written},
     {"CCR", &dac::trim_written},
     {"SR", &dac::status_written},
     {"SWTRGR", &dac::trigger_written},
     {"DHR12R1", &dac::holding_written<holding::right12_1>},
     {"DHR12L1", &dac::holding_written<holding::left12_1>},
     {"DHR8R1", &dac::holding_written<holding::right8_1>},
     {"DHR12R2", &dac::holding_written<holding::right12_2>},
     {"DHR12L2", &dac::holding_written<holding::left12_2>},
     {"DHR8R2", &dac::holding_written<holding::right8_2>},
     {"DHR12RD", &dac::holding_written<holding::right12_dual>},
     {"DHR12LD", &dac::holding_written<holding::left12_dual>},
     {"DHR8RD", &dac::holding_written<holding::right8_dual>},
     {"DOR1", nullptr},
     {"DOR2", nullptr},
     }
};

dac::dac(model_context& ctx, peripheral_desc const& desc, dac_variant const& variant)
    : dispatch_model{
          ctx, desc, "DAC", dac_writes
},
      variant_{variant}, regs_{ctx.regs}, control_{reg("CR")}, mode_{reg("MCR")},
      status_{reg("SR")},
      holding_{reg("DHR12R1"), reg("DHR12L1"), reg("DHR8R1"),  reg("DHR12R2"), reg("DHR12L2"),
               reg("DHR8R2"),  reg("DHR12RD"), reg("DHR12LD"), reg("DHR8RD")},
      unmodelled_fields_{field("CR", "WAVE1"), field("CR", "WAVE2")},
      bus_{ctx.clock.domain("pclk1")}, line_{&irq("TIM6_DAC_IRQn")},
      requests_{
          dma_signal{ctx.peers, lower(desc.name) + "_ch1_dma"},
          dma_signal{ctx.peers, lower(desc.name) + "_ch2_dma"}}
{
  for(std::size_t index = 0; index < channels; ++index)
  {
    auto const n    = index + 1;
    channel_[index] = {
        .enable             = field("CR", std::format("EN{}", n)),
        .trigger_enable     = field("CR", std::format("TEN{}", n)),
        .trigger_select     = field("CR", std::format("TSEL{}", n)),
        .amplitude          = field("CR", std::format("MAMP{}", n)),
        .wave               = field("CR", std::format("WAVE{}", n)),
        .calibration        = field("CR", std::format("CEN{}", n)),
        .software           = field("SWTRGR", std::format("SWTRIG{}", n)),
        .output             = reg(std::format("DOR{}", n)),
        .mode               = field("MCR", std::format("MODE{}", n)),
        .trim               = field("CCR", std::format("OTRIM{}", n)),
        .flag               = field("SR", std::format("CAL_FLAG{}", n)),
        .underrun           = field("SR", std::format("DMAUDR{}", n)),
        .dma                = field("CR", std::format("DMAEN{}", n)),
        .underrun_interrupt = field("CR", std::format("DMAUDRIE{}", n)),
    };
  }
  restore_trim();
}

auto dac::now_ps() const -> std::uint64_t
{
  return ctx_.clock.time().ps;
}

auto dac::cycles_ps(std::uint64_t cycles) const -> std::uint64_t
{
  auto const hz = ctx_.clock.hz(bus_);
  return hz == 0 ? 0 : (cycles * ps_per_second + hz - 1) / hz;
}

auto dac::load_delay_ps(trigger_time const& at) const -> std::uint64_t
{
  auto const hz = ctx_.clock.hz(bus_);
  if(hz == 0 or at.hz == 0 or at.lag == 0)
  {
    return cycles_ps(3);
  }
  auto const span = wide{3} * ps_per_second * at.hz - wide{at.lag} * hz;
  auto const unit = wide{at.hz} * hz;
  return static_cast<std::uint64_t>((span + unit - 1) / unit);
}

auto dac::listened(std::uint32_t control) const -> selection
{
  selection which;
  for(std::size_t channel = 0; channel < channels; ++channel)
  {
    auto const select = channel_[channel].trigger_select.of(control);
    if(channel_[channel].trigger_enable.of(control) != 0 and select < variant_.software_trigger)
    {
      which[channel] = select;
    }
  }
  return which;
}

void dac::notify(selection const& which)
{
  for(auto const& select : which)
  {
    if(select)
    {
      static_cast<void>(interest_changed(ctx_.peers, variant_.hardware_triggers[*select]));
    }
  }
}

void dac::retune(selection const& was)
{
  auto const now = listened(regs_.peek(control_.offset));
  if(now != was)
  {
    notify(was);
    notify(now);
  }
}

auto dac::listens(std::string_view signal) const -> bool
{
  return std::ranges::any_of(listened(regs_.peek(control_.offset)), [&](auto const& select) {
    return select and variant_.hardware_triggers[*select] == signal;
  });
}

auto dac::can_listen(std::string_view signal) const -> bool
{
  return std::ranges::contains(variant_.hardware_triggers, signal);
}

void dac::triggered(std::string_view signal, trigger_edge edge, trigger_time at)
{
  if(edge != trigger_edge::rising)
  {
    return;
  }
  auto const which = listened(regs_.peek(control_.offset));
  for(std::size_t channel = 0; channel < channels; ++channel)
  {
    if(which[channel] and variant_.hardware_triggers[*which[channel]] == signal)
    {
      catch_up(channel, at.ps);
      request(channel, at.ps + load_delay_ps(at), true);
    }
  }
}

auto dac::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     access) -> std::expected<void, bus_error>
{
  if((offset & ~3u) != control_.offset)
  {
    return dispatch_model::on_write(regs, offset, size, old, value, access);
  }
  auto const config = regs_.peek(control_.offset);
  auto const was    = listened(config);
  notify(was);
  notify(listened(merge_write(config, offset, size, value)));
  auto const result = dispatch_model::on_write(regs, offset, size, old, value, access);
  retune(was);
  return result;
}

auto dac::software_selected(std::size_t channel) const -> bool
{
  return get(regs_, channel_[channel].trigger_select) == variant_.software_trigger;
}

void dac::refuse(
    rule             which,
    std::string_view name,
    reg_ref          at,
    std::uint32_t    old,
    std::string      message)
{
  if(ctx_.strict_peripherals)
  {
    regs_.poke(at.offset, old);
  }
  warn_once_or_refuse(rule_logged_[std::to_underlying(which)], name, at.offset, std::move(message));
}

void dac::control_written(std::uint32_t old, std::uint32_t now)
{
  auto       word = now;
  auto const keep = [&](field_ref const& bits, rule which, std::string message) {
    if(bits.of(word) == bits.of(old))
    {
      return;
    }
    word = bits.with(word, bits.of(old));
    regs_.poke(control_.offset, word);
    refuse(which, "CR", control_, old, std::move(message));
  };
  for(std::size_t index = 0; index < channels; ++index)
  {
    auto const& unit = channel_[index];
    auto const  n    = index + 1;
    if(unit.enable.of(old) == 0)
    {
      continue;
    }
    keep(
        unit.trigger_select, rule::trigger_locked,
        std::format("TSEL{0} written while EN{0} is 1, RM0351 19.4.6: the old TSEL{0} kept", n));
    keep(
        unit.amplitude, rule::amplitude_locked,
        std::format("MAMP{0} written while EN{0} is 1, RM0351 19.4.9: the old MAMP{0} kept", n));
    keep(
        unit.calibration, rule::calibration_locked,
        std::format("CEN{0} written while EN{0} is 1, ignored by RM0351 19.7.1", n));
  }
  for(std::size_t index = 0; index < channels; ++index)
  {
    auto const& unit = channel_[index];
    auto const  n    = index + 1;
    if(unit.calibration.of(word) != 0 and unit.enable.of(word) != 0 and unit.enable.of(old) == 0)
    {
      refuse(
          rule::calibration_enable, "CR", control_, old,
          std::format("EN{0} set while CEN{0} is 1, not allowed by RM0351 19.4.11", n));
    }
    if(unit.wave.of(word) == 3)
    {
      refuse(
          rule::wave_reserved, "CR", control_, old,
          std::format("WAVE{} 11, reserved by RM0351 19.7.1", n));
    }
  }
  for(std::size_t use = 0; use < unmodelled_table.size(); ++use)
  {
    if(unmodelled_fields_[2 * use].of(word) != 0 or unmodelled_fields_[2 * use + 1].of(word) != 0)
    {
      if(ctx_.strict_peripherals)
      {
        regs_.poke(control_.offset, old);
      }
      warn_once_or_refuse(
          unmodelled_logged_[use], "CR", control_.offset,
          std::format("{} is not modelled", unmodelled_table[use].what));
    }
  }
  for(std::size_t channel = 0; channel < channels; ++channel)
  {
    if(get(regs_, channel_[channel].dma) == 0)
    {
      release(channel);
    }
  }
  refresh_flags();
  refresh_line();
}

void dac::mode_written(std::uint32_t old, std::uint32_t now)
{
  auto word = now;
  for(std::size_t index = 0; index < channels; ++index)
  {
    auto const& unit = channel_[index];
    auto const  n    = index + 1;
    if(unit.mode.of(word) != unit.mode.of(old)
       and (get(regs_, unit.enable) != 0 or get(regs_, unit.calibration) != 0))
    {
      word = unit.mode.with(word, unit.mode.of(old));
      regs_.poke(mode_.offset, word);
      refuse(
          rule::mode_locked, "MCR", mode_, old,
          std::format("MODE{0} written while EN{0} or CEN{0} is 1, ignored by RM0351 19.7.16", n));
    }
  }
  for(auto const& unit : channel_)
  {
    if(unit.mode.of(word) >= 4)
    {
      refuse(
          rule::sample_hold, "MCR", mode_, old,
          "MODE 1xx, the sample and hold mode, is not modelled");
    }
  }
  refresh_flags();
}

void dac::trim_written(std::uint32_t, std::uint32_t)
{
  refresh_flags();
}

void dac::status_written(std::uint32_t old, std::uint32_t now)
{
  auto word = old;
  for(auto const& unit : channel_)
  {
    if(unit.underrun.of(now) != 0)
    {
      word = unit.underrun.with(word, 0);
    }
  }
  regs_.poke(status_.offset, word);
  refresh_line();
}

void dac::refresh_flags()
{
  for(std::size_t index = 0; index < channels; ++index)
  {
    auto const& unit = channel_[index];
    if(get(regs_, unit.calibration) == 0)
    {
      continue;
    }
    auto const buffered = (get(regs_, unit.mode) & 2u) == 0;
    auto const reached  = get(regs_, unit.trim) >= variant_.factory_trim[index];
    update(regs_, unit.flag, buffered and reached ? 1u : 0u);
  }
}

void dac::restore_trim()
{
  for(std::size_t index = 0; index < channels; ++index)
  {
    update(regs_, channel_[index].trim, variant_.factory_trim[index]);
  }
}

void dac::trigger_written(std::uint32_t, std::uint32_t now)
{
  for(std::size_t channel = 0; channel < channels; ++channel)
  {
    if(channel_[channel].software.of(now) != 0
       and get(regs_, channel_[channel].trigger_enable) != 0
       and software_selected(channel))
    {
      request(channel, now_ps() + cycles_ps(1), false);
    }
  }
  regs_.poke(channel_[0].software.offset, 0);
}

template <holding Which> void dac::holding_written(std::uint32_t, std::uint32_t now)
{
  using enum holding;
  switch(Which)
  {
    case right12_1:
      store(0, now & data_mask);
      break;
    case left12_1:
      store(0, now >> 4 & data_mask);
      break;
    case right8_1:
      store(0, (now & 0xFF) << 4);
      break;
    case right12_2:
      store(1, now & data_mask);
      break;
    case left12_2:
      store(1, now >> 4 & data_mask);
      break;
    case right8_2:
      store(1, (now & 0xFF) << 4);
      break;
    case right12_dual:
      store(0, now & data_mask);
      store(1, now >> 16 & data_mask);
      break;
    case left12_dual:
      store(0, now >> 4 & data_mask);
      store(1, now >> 20 & data_mask);
      break;
    case right8_dual:
      store(0, (now & 0xFF) << 4);
      store(1, (now >> 8 & 0xFF) << 4);
      break;
  }
  mirror();
}

void dac::store(std::size_t channel, std::uint32_t data)
{
  state_.dhr[channel] = static_cast<std::uint16_t>(data);
  if(get(regs_, channel_[channel].trigger_enable) == 0)
  {
    request(channel, now_ps() + cycles_ps(1), false);
  }
}

void dac::mirror()
{
  auto const                         one = std::uint32_t{state_.dhr[0]};
  auto const                         two = std::uint32_t{state_.dhr[1]};
  std::array<std::uint32_t, 9> const views{
      one,
      one << 4,
      one >> 4,
      two,
      two << 4,
      two >> 4,
      one | two << 16,
      one << 4 | two << 20,
      one >> 4 | (two >> 4) << 8};
  for(std::size_t index = 0; index < views.size(); ++index)
  {
    regs_.poke(holding_[index].offset, views[index]);
  }
}

void dac::request(std::size_t channel, std::uint64_t due, bool hardware)
{
  auto const first = channel * depth;
  auto       slot  = first;
  while(slot + 1 < first + depth and state_.due_ps[slot] != 0)
  {
    ++slot;
  }
  state_.due_ps[slot]    = due;
  state_.triggered[slot] = hardware ? 1 : 0;
  if(slot == first)
  {
    ctx_.events.schedule_at(
        vtime{due}, static_cast<std::uint32_t>(channel), state_.generation, ctx_.clock);
  }
}

void dac::catch_up(std::size_t channel, std::uint64_t ps)
{
  auto const first = channel * depth;
  while(state_.due_ps[first] != 0 and state_.due_ps[first] <= ps)
  {
    auto const hardware = state_.triggered[first] != 0;
    for(auto slot = first; slot + 1 < first + depth; ++slot)
    {
      state_.due_ps[slot]    = state_.due_ps[slot + 1];
      state_.triggered[slot] = state_.triggered[slot + 1];
    }
    state_.due_ps[first + depth - 1]    = 0;
    state_.triggered[first + depth - 1] = 0;
    transfer(channel, hardware);
    if(state_.due_ps[first] != 0)
    {
      ctx_.events.schedule_at(
          vtime{state_.due_ps[first]}, static_cast<std::uint32_t>(channel), state_.generation,
          ctx_.clock);
    }
  }
}

void dac::transfer(std::size_t channel, bool hardware)
{
  auto const& unit = channel_[channel];
  regs_.poke(unit.output.offset, state_.dhr[channel]);
  if(not hardware or get(regs_, unit.dma) == 0)
  {
    return;
  }
  if(state_.requested[channel] != 0)
  {
    update(regs_, unit.underrun, 1);
    refresh_line();
    return;
  }
  state_.requested[channel] = 1;
  requests_[channel].set(true);
}

void dac::release(std::size_t channel)
{
  if(state_.requested[channel] == 0)
  {
    return;
  }
  state_.requested[channel] = 0;
  requests_[channel].set(false);
}

void dac::acknowledged(dma_controller const& by, std::uint32_t request)
{
  for(std::size_t channel = 0; channel < channels; ++channel)
  {
    if(requests_[channel].carries(by, request))
    {
      release(channel);
    }
  }
}

void dac::transfer_completed(dma_controller const&, std::uint32_t)
{}

void dac::refresh_line()
{
  auto const raised = std::ranges::any_of(channel_, [&](channel_fields const& unit) {
    return get(regs_, unit.underrun) != 0 and get(regs_, unit.underrun_interrupt) != 0;
  });
  if(raised)
  {
    line_->raise();
  }
  else
  {
    line_->lower();
  }
}

void dac::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind >= channels or payload != state_.generation)
  {
    return;
  }
  catch_up(kind, now_ps());
}

auto dac::on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx access)
    -> std::expected<std::uint32_t, bus_error>
{
  for(std::size_t channel = 0; channel < channels; ++channel)
  {
    catch_up(channel, now_ps());
  }
  return dispatch_model::on_read(regs, offset, size, access);
}

void dac::reset(register_file&, reset_kind)
{
  restore_reset_values();
}

void dac::on_bus_reset()
{
  auto const was = listened(regs_.peek(control_.offset));
  notify(was);
  restore_reset_values();
  retune(was);
}

void dac::restore_reset_values()
{
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
  for(std::size_t channel = 0; channel < channels; ++channel)
  {
    release(channel);
  }
  auto const generation = state_.generation + 1;
  state_                = {};
  state_.generation     = generation;
  restore_trim();
  refresh_line();
}
}

auto dac_factory(dac_variant variant) -> model_factory
{
  return [variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<dac>(ctx, desc, variant);
  };
}
}
