#pragma once

#include "family_names.hpp"
#include "model_base.hpp"

#include <popcorn/periph/stm32/rcc.hpp>

#include <algorithm>
#include <array>
#include <contracts>
#include <cstddef>
#include <cstdint>
#include <meta>
#include <string_view>
#include <utility>

namespace popcorn::periph::stm32
{
namespace
{
enum class rcc_role : std::uint8_t
{
  css_on,
  msi_select,
  msis_range,
  msik_range,
  msis_standby_range,
  msik_standby_range,
  sw,
  sws,
  hpre,
  ppre1,
  ppre2,
  ppre3,
  rtc_select,
  rtc_enable,
  lsi_prediv,
  backup_reset,
  remove_flags,
  hsi_divider,
  hsi_divider_flag,
  timer_prescaler,
  rtc_prescaler,
};

/** @brief Cycles of the running HSI rate from an `HSIDIV` write to the `HSIDIVF` clear and set. */
struct hsi_divider_timing
{
  /** @brief Cycles to the clear. */
  std::uint8_t clear;
  /** @brief Cycles to the set and the new rate. */
  std::uint8_t set;
};

struct rate_inputs
{
  register_file const&         regs;
  field_table<rcc_role> const& names;
  rcc_options const&           options;
  std::uint32_t                hsi_divider{};

  [[nodiscard]] auto read(rcc_role role) const -> std::uint32_t
  {
    return get(regs, names[role]);
  }
};

using rate_rule = auto (*)(rate_inputs const&) -> std::uint64_t;

template <typename Signal> struct oscillator_names
{
  std::string_view reg;
  std::string_view on;
  std::string_view ready;
  std::string_view flag;
  std::string_view enable;
  Signal           out{};
  rate_rule        rate{};
};

/**
 * @brief A clock that follows an enable bit once its source oscillator is ready: the STM32U5
 *        `LSESYSEN` / `LSESYSRDY`, RM0456 11.4.7. The ready bit sets after @p cycles periods of the
 *        source and clears with the enable bit or the source.
 * @tparam Signal Signal enumeration of the family.
 * @tparam Source Oscillator enumeration of the family.
 */
template <typename Signal, typename Source> struct dependent_clock
{
  /** @brief Signal the clock publishes: the rate of @p in while ready, else 0. */
  Signal out;
  /** @brief Signal of the source oscillator. */
  Signal in;
  /** @brief Oscillator that has to be ready. */
  Source from;
  /** @brief Register holding the enable and ready bits. */
  std::string_view reg;
  /** @brief Enable bit name. */
  std::string_view enable;
  /** @brief Ready bit name. */
  std::string_view ready;
  /** @brief Source periods from the enable, or from the source becoming ready, to the ready. */
  std::uint8_t cycles;
};

struct consumer
{
  std::string_view instance;
  std::string_view enable;
};

template <typename Signal> struct mux
{
  std::string_view        reg;
  std::string_view        field;
  std::array<Signal, 8>   inputs;
  std::array<consumer, 4> consumers;
};

template <typename Signal> struct signal_mux
{
  std::string_view      reg;
  std::string_view      field;
  std::array<Signal, 4> inputs;
  Signal                out;
};

template <typename Signal> struct divided_signal
{
  Signal        out;
  Signal        in;
  std::uint64_t divider;
};

template <typename Signal> struct rtc_input
{
  Signal    from;
  rate_rule divider;
};

template <typename Signal> struct timer_names
{
  std::string_view instance;
  std::string_view enable;
  Signal           bus;
};

template <typename Signal> struct bus_node
{
  Signal   out;
  rcc_role prescaler;
};

using field_decode = auto (*)(std::uint32_t code) -> std::uint64_t;

constexpr std::size_t max_pll_outputs = 3;

template <typename Signal> struct pll_node
{
  std::string_view                              config;
  std::string_view                              dividers;
  std::string_view                              fraction;
  std::string_view                              src;
  std::string_view                              range;
  std::string_view                              m;
  std::string_view                              fracen;
  std::string_view                              n;
  std::string_view                              frac;
  std::array<std::string_view, max_pll_outputs> divider;
  std::array<std::string_view, max_pll_outputs> output_enable;
  std::array<Signal, max_pll_outputs>           out;
  std::size_t                                   outputs;
  field_decode                                  m_decode;
  field_decode                                  n_decode;
  std::array<field_decode, max_pll_outputs>     output_decode;
  /** @brief Whether the output enables keep their value while the PLL is on or ready. */
  bool outputs_held_while_on{};
  /** @brief Register of the output enables, `config` when empty. */
  std::string_view enables{};
};

constexpr auto plus_one(std::uint32_t code) -> std::uint64_t
{
  return std::uint64_t{code} + 1;
}

constexpr auto direct(std::uint32_t code) -> std::uint64_t
{
  return code;
}

struct secured_group
{
  std::string_view bit;
  std::string_view reg;
  std::string_view fields;
};

using unit_alias = std::pair<std::string_view, std::string_view>;

/** @brief Fields of one register that keep their value through a system reset. */
struct kept_fields
{
  /** @brief Register name. */
  std::string_view reg;
  /** @brief Field names, separated by spaces. */
  std::string_view fields;
};

/**
 * @brief Fields of one register that keep their value while an oscillator is on: a write
 *        changes them only when the oscillator's ON bit was 0 before the write.
 * @tparam Source Oscillator enumeration of the family.
 */
template <typename Source> struct held_field
{
  /** @brief Register name. */
  std::string_view reg;
  /** @brief Field names, separated by spaces. */
  std::string_view fields;
  /** @brief Oscillator whose ON bit holds them. */
  Source source;
};

template <std::uint64_t Hz> auto fixed_rate(rate_inputs const&) -> std::uint64_t
{
  return Hz;
}

template <std::uint64_t N> auto divide_by(rate_inputs const&) -> std::uint64_t
{
  return N;
}

auto hse_rate(rate_inputs const& in) -> std::uint64_t
{
  return in.options.hse_hz;
}

auto lse_rate(rate_inputs const& in) -> std::uint64_t
{
  return in.options.lse_hz;
}

constexpr std::uint64_t frac_one = 8192;

constexpr auto hpre_divider(std::uint32_t code) noexcept -> std::uint64_t
{
  constexpr std::array<std::uint64_t, 8> high{2, 4, 8, 16, 64, 128, 256, 512};
  return code < 8 ? 1 : high[code - 8];
}

constexpr auto ppre_divider(std::uint32_t code) noexcept -> std::uint64_t
{
  return code < 4 ? 1 : std::uint64_t{2} << (code - 4);
}

/** @brief Flags of the reset status register one kind of reset sets, the second empty when one
 * flag is enough. */
struct reset_flag_row
{
  reset_kind                      kind;
  std::array<std::string_view, 2> flags;
};

/**
 * @brief Whether @p rows holds exactly one row for every `reset_kind` enumerator.
 * @param rows Reset flag table of one family.
 */
template <std::size_t N>
consteval auto covers_every_reset_kind(std::array<reset_flag_row, N> const& rows) -> bool
{
  auto const kinds = std::meta::enumerators_of(^^reset_kind);
  if(rows.size() != kinds.size())
  {
    return false;
  }
  for(auto const e : kinds)
  {
    if(std::ranges::count(rows, std::meta::extract<reset_kind>(e), &reset_flag_row::kind) != 1)
    {
      return false;
    }
  }
  return true;
}

/**
 * @brief Flags a reset of @p kind sets.
 * @param rows Reset flag table of one family, checked with covers_every_reset_kind().
 * @param kind Kind of the reset.
 */
template <std::size_t N>
constexpr auto reset_flags_in(std::array<reset_flag_row, N> const& rows, reset_kind kind) noexcept
    -> std::array<std::string_view, 2> pre(std::ranges::contains(rows, kind, &reset_flag_row::kind))
{
  return std::ranges::find(rows, kind, &reset_flag_row::kind)->flags;
}
}
}
