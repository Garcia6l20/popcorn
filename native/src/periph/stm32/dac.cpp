#include "factories.hpp"
#include "model_base.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/periph/bus_reset.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace popcorn::periph::stm32
{
namespace
{
constexpr std::size_t   channels  = 2;
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
  std::uint64_t                       generation{};
  std::array<std::uint64_t, channels> due_ps{};
  std::array<std::uint64_t, channels> next_ps{};
  std::array<std::uint16_t, channels> dhr{};

  friend auto operator==(dac_state const&, dac_state const&) -> bool = default;
};

struct unmodelled_use
{
  std::string_view field;
  std::string_view what;
};

constexpr std::array<unmodelled_use, 5> unmodelled_table{
    {
     {"WAVE", "WAVE, the noise and triangle waves"},
     {"DMAEN", "DMAEN, the DMA requests"},
     {"DMAUDRIE", "DMAUDRIE, the DMA underrun interrupt"},
     {"CEN", "CEN, the buffer calibration"},
     {"TSEL", "TEN with a hardware trigger in TSEL"},
     }
};

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
 *        or another TSELx it loads nothing, inferred. Popcorn charges no APB wait states, so
 *        writes closer than one APB1 cycle happen only in the model. In that window a load
 *        takes DHRx as it is at the load, and a write while a load is pending keeps that load
 *        and moves the next one to the last write's due, so of three such writes the second
 *        loses its own load. DAC_DORx is read only. The digital interface works with ENx 0,
 *        RM0351 19.4.2 note, so loads do not depend on ENx.
 *        WAVEx other than 00, DMAENx, DMAUDRIEx, CENx and TENx 1 with a TSELx below 111 are
 *        not modelled: a DAC_CR write that sets one logs one model_warning per function, and
 *        under strict_peripherals raises model_error and leaves DAC_CR as before the write.
 *        DAC1RST and a machine reset restore the claimed registers and drop the pending loads.
 */
class dac final : public dispatch_model<dac, dac_state>, public bus_reset_listener
{
public:
  dac(model_context& ctx, peripheral_desc const& desc, dac_variant const& variant);

  void reset(register_file& regs, reset_kind kind) override;
  void on_bus_reset() override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx access)
      -> std::expected<std::uint32_t, bus_error> override;

  void                          control_written(std::uint32_t old, std::uint32_t now);
  void                          trigger_written(std::uint32_t old, std::uint32_t now);
  template <holding Which> void holding_written(std::uint32_t old, std::uint32_t now);

private:
  struct channel_fields
  {
    field_ref trigger_enable;
    field_ref trigger_select;
    field_ref software;
    reg_ref   output;
  };

  [[nodiscard]] auto now_ps() const -> std::uint64_t;
  [[nodiscard]] auto cycle_ps() const -> std::uint64_t;
  [[nodiscard]] auto software_selected(std::size_t channel) const -> bool;
  void               store(std::size_t channel, std::uint32_t data);
  void               mirror();
  void               request(std::size_t channel);
  void               catch_up(std::size_t channel, std::uint64_t ps);
  void               restore_reset_values();

  dac_variant                               variant_;
  register_file&                            regs_;
  reg_ref                                   control_;
  std::array<reg_ref, 9>                    holding_;
  std::array<channel_fields, 2>             channel_;
  std::array<field_ref, 10>                 unmodelled_fields_;
  core::clock_domain                        bus_;
  std::array<bool, unmodelled_table.size()> unmodelled_logged_{};
};

constexpr std::array<write_entry<dac>, 13> dac_writes{
    {
     {"CR", &dac::control_written},
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
      variant_{variant}, regs_{ctx.regs}, control_{reg("CR")},
      holding_{reg("DHR12R1"), reg("DHR12L1"), reg("DHR8R1"),  reg("DHR12R2"), reg("DHR12L2"),
               reg("DHR8R2"),  reg("DHR12RD"), reg("DHR12LD"), reg("DHR8RD")},
      channel_{
          {{field("CR", "TEN1"), field("CR", "TSEL1"), field("SWTRGR", "SWTRIG1"), reg("DOR1")},
           {field("CR", "TEN2"), field("CR", "TSEL2"), field("SWTRGR", "SWTRIG2"), reg("DOR2")}}},
      unmodelled_fields_{field("CR", "WAVE1"),  field("CR", "WAVE2"),     field("CR", "DMAEN1"),
                         field("CR", "DMAEN2"), field("CR", "DMAUDRIE1"), field("CR", "DMAUDRIE2"),
                         field("CR", "CEN1"),   field("CR", "CEN2"),      field("CR", "TEN1"),
                         field("CR", "TEN2")},
      bus_{ctx.clock.domain("pclk1")}
{}

auto dac::now_ps() const -> std::uint64_t
{
  return ctx_.clock.time().ps;
}

auto dac::cycle_ps() const -> std::uint64_t
{
  auto const hz = ctx_.clock.hz(bus_);
  return hz == 0 ? 0 : (ps_per_second + hz - 1) / hz;
}

auto dac::software_selected(std::size_t channel) const -> bool
{
  return get(regs_, channel_[channel].trigger_select) == variant_.software_trigger;
}

void dac::control_written(std::uint32_t old, std::uint32_t now)
{
  for(std::size_t use = 0; use < unmodelled_table.size(); ++use)
  {
    auto used = false;
    for(std::size_t channel = 0; channel < channels; ++channel)
    {
      auto const& bits = unmodelled_fields_[2 * use + channel];
      if(bits.of(now) == 0)
      {
        continue;
      }
      used = used or unmodelled_table[use].field != "TSEL" or not software_selected(channel);
    }
    if(used)
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
}

void dac::trigger_written(std::uint32_t, std::uint32_t now)
{
  for(std::size_t channel = 0; channel < channels; ++channel)
  {
    if(channel_[channel].software.of(now) != 0
       and get(regs_, channel_[channel].trigger_enable) != 0
       and software_selected(channel))
    {
      request(channel);
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
    request(channel);
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

void dac::request(std::size_t channel)
{
  auto const due = now_ps() + cycle_ps();
  if(state_.due_ps[channel] == 0)
  {
    state_.due_ps[channel] = due;
    ctx_.events.schedule_at(
        vtime{due}, static_cast<std::uint32_t>(channel), state_.generation, ctx_.clock);
    return;
  }
  state_.next_ps[channel] = due;
}

void dac::catch_up(std::size_t channel, std::uint64_t ps)
{
  while(state_.due_ps[channel] != 0 and state_.due_ps[channel] <= ps)
  {
    regs_.poke(channel_[channel].output.offset, state_.dhr[channel]);
    state_.due_ps[channel] = std::exchange(state_.next_ps[channel], 0);
    if(state_.due_ps[channel] != 0)
    {
      ctx_.events.schedule_at(
          vtime{state_.due_ps[channel]}, static_cast<std::uint32_t>(channel), state_.generation,
          ctx_.clock);
    }
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
  restore_reset_values();
}

void dac::restore_reset_values()
{
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
  auto const generation = state_.generation + 1;
  state_                = {};
  state_.generation     = generation;
}
}

auto dac_factory(dac_variant variant) -> model_factory
{
  return [variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<dac>(ctx, desc, variant);
  };
}
}
