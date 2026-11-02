#pragma once

#include "rcc_graph.hpp"

#include <popcorn/core/types.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace popcorn::periph::stm32
{
namespace
{
namespace l4
{
enum class source : std::uint8_t
{
  msi,
  hsi16,
  hse,
  lsi,
  lse,
  pll1,
};

enum class signal : std::uint8_t
{
  none,
  msi,
  hsi16,
  hse,
  lsi,
  lse,
  sysclk,
  hclk,
  pclk1,
  pclk2,
  pll1_p,
  pll1_q,
  pll1_r,
  rtc,
};

using enum signal;

constexpr std::uint64_t hsi16_hz = 16'000'000;
constexpr std::uint64_t lsi_hz   = 32'000;

/** @brief MSI range frequencies of RM0351 6.4.1 MSIRANGE and DS10198 Table 60, range 0 to 11. */
constexpr std::array<std::uint64_t, 12> msi_hz{100'000,    200'000,    400'000,    800'000,
                                               1'000'000,  2'000'000,  4'000'000,  8'000'000,
                                               16'000'000, 24'000'000, 32'000'000, 48'000'000};

/**
 * @brief MSI rate of RM0351 6.4.1: the range of MSIRANGE in RCC_CR when MSIRGSEL is 1, else the
 *        range of MSISRANGE in RCC_CSR, 4 to 7, 6.4.30. A reserved MSISRANGE code runs at 0 Hz.
 */
auto msi_rate(rate_inputs const& in) -> std::uint64_t
{
  using enum rcc_role;
  if(in.read(msi_select) != 0)
  {
    auto const range = in.read(msis_range);
    return range < msi_hz.size() ? msi_hz[range] : 0;
  }
  auto const range = in.read(msis_standby_range);
  return range >= 4 and range <= 7 ? msi_hz[range] : 0;
}

/** @brief PLLN of RCC_PLLCFGR, RM0351 6.4.4: 8 to 86, 0 for a code outside that range. */
constexpr auto pll_multiplier(std::uint32_t code) -> std::uint64_t
{
  return code >= 8 and code <= 86 ? code : 0;
}

/** @brief PLLP of RCC_PLLCFGR, RM0351 6.4.4: 0 divides by 7, 1 by 17. */
constexpr auto pll_p_divider(std::uint32_t code) -> std::uint64_t
{
  return code == 0 ? 7 : 17;
}

/** @brief PLLQ and PLLR of RCC_PLLCFGR, RM0351 6.4.4: 00 to 11 divide by 2, 4, 6 and 8. */
constexpr auto pll_even_divider(std::uint32_t code) -> std::uint64_t
{
  return (std::uint64_t{code} + 1) * 2;
}

constexpr std::array<mux<signal>, 0> muxes{};

constexpr std::array<timer_names<signal>, 11> timers{
    {
     {"TIM1", "TIM1EN", pclk2},
     {"TIM2", "TIM2EN", pclk1},
     {"TIM3", "TIM3EN", pclk1},
     {"TIM4", "TIM4EN", pclk1},
     {"TIM5", "TIM5EN", pclk1},
     {"TIM6", "TIM6EN", pclk1},
     {"TIM7", "TIM7EN", pclk1},
     {"TIM8", "TIM8EN", pclk2},
     {"TIM15", "TIM15EN", pclk2},
     {"TIM16", "TIM16EN", pclk2},
     {"TIM17", "TIM17EN", pclk2},
     }
};

constexpr std::array<std::string_view, 6> enable_registers{"AHB1ENR",  "AHB2ENR",  "AHB3ENR",
                                                           "APB1ENR1", "APB1ENR2", "APB2ENR"};

constexpr std::array<oscillator_names<signal>, 6> oscillators{
    {
     {"CR", "MSION", "MSIRDY", "MSIRDYF", "MSIRDYIE", msi, &msi_rate},
     {"CR", "HSION", "HSIRDY", "HSIRDYF", "HSIRDYIE", hsi16, &fixed_rate<hsi16_hz>},
     {"CR", "HSEON", "HSERDY", "HSERDYF", "HSERDYIE", hse, &hse_rate},
     {"CSR", "LSION", "LSIRDY", "LSIRDYF", "LSIRDYIE", lsi, &fixed_rate<lsi_hz>},
     {"BDCR", "LSEON", "LSERDY", "LSERDYF", "LSERDYIE", lse, &lse_rate},
     {"CR", "PLLON", "PLLRDY", "PLLRDYF", "PLLRDYIE"},
     }
};

constexpr auto names = std::to_array<named_field<rcc_role>>({
    {rcc_role::css_on,             {"CR", "CSSON"}     },
    {rcc_role::msi_select,         {"CR", "MSIRGSEL"}  },
    {rcc_role::msis_range,         {"CR", "MSIRANGE"}  },
    {rcc_role::msik_range,         {}                  },
    {rcc_role::msis_standby_range, {"CSR", "MSISRANGE"}},
    {rcc_role::msik_standby_range, {}                  },
    {rcc_role::sw,                 {"CFGR", "SW"}      },
    {rcc_role::sws,                {"CFGR", "SWS"}     },
    {rcc_role::hpre,               {"CFGR", "HPRE"}    },
    {rcc_role::ppre1,              {"CFGR", "PPRE1"}   },
    {rcc_role::ppre2,              {"CFGR", "PPRE2"}   },
    {rcc_role::ppre3,              {}                  },
    {rcc_role::rtc_select,         {"BDCR", "RTCSEL"}  },
    {rcc_role::rtc_enable,         {"BDCR", "RTCEN"}   },
    {rcc_role::lsi_prediv,         {}                  },
    {rcc_role::backup_reset,       {"BDCR", "BDRST"}   },
    {rcc_role::remove_flags,       {"CSR", "RMVF"}     },
    {rcc_role::hsi_divider,        {}                  },
    {rcc_role::hsi_divider_flag,   {}                  },
    {rcc_role::timer_prescaler,    {}                  },
    {rcc_role::rtc_prescaler,      {}                  },
});

static_assert(names_every_role(names));

constexpr std::array<signal_mux<signal>, 0> signal_muxes{};

constexpr std::array<divided_signal<signal>, 0> divided{};

constexpr std::array<signal, 0> internal_signals{};

constexpr std::array<rtc_input<signal>, 4> rtc_sources{
    {{none, &divide_by<1>}, {lse, &divide_by<1>}, {lsi, &divide_by<1>}, {hse, &divide_by<32>}}
};

constexpr std::array<source, 4> pll_inputs{source::pll1, source::msi, source::hsi16, source::hse};
constexpr std::array<signal, 4> pll_sources{none, msi, hsi16, hse};
constexpr std::array<std::optional<source>, 4> system_inputs{
    source::msi, source::hsi16, source::hse, source::pll1};
constexpr std::array<signal, 4> system_sources{msi, hsi16, hse, pll1_r};

constexpr std::array<bus_node<signal>, 2> buses{
    {{pclk1, rcc_role::ppre1}, {pclk2, rcc_role::ppre2}}
};

constexpr std::array<pll_node<signal>, 1> pll_nodes{
    {
     {.config        = "PLLCFGR",
         .dividers      = "PLLCFGR",
         .fraction      = {},
         .src           = "PLLSRC",
         .range         = {},
         .m             = "PLLM",
         .fracen        = {},
         .n             = "PLLN",
         .frac          = {},
         .divider       = {"PLLP", "PLLQ", "PLLR"},
         .output_enable = {"PLLPEN", "PLLQEN", "PLLREN"},
         .out           = {pll1_p, pll1_q, pll1_r},
         .outputs       = 3,
         .m_decode      = &plus_one,
         .n_decode      = &pll_multiplier,
         .output_decode = {&pll_p_divider, &pll_even_divider, &pll_even_divider}},
     }
};

constexpr std::array<std::string_view, 8> reset_flags{
    "LPWRSTF", "WWDGRSTF", "IWDGRSTF", "SFTRSTF", "BORRSTF", "PINRSTF", "OBLRSTF", "FIREWALLRSTF"};

constexpr std::array<secured_group, 0> rcc_secured{};

constexpr std::array<std::string_view, 0> rcc_unit_registers{};

constexpr std::array<std::string_view, 0> rcc_unit_suffixes{};

constexpr std::array<secured_group, 0> rcc_backup_secured{};

constexpr std::array<unit_alias, 0> rcc_unit_aliases{};

constexpr auto system_reset_kept = std::to_array<kept_fields>({
    {"CR", "HSEBYP"},
});

constexpr std::array<reset_kind, 2> power_resets{reset_kind::power_on, reset_kind::brown_out};

constexpr auto held_while_on = std::to_array<held_field<source>>({
    {"CR", "HSEBYP", source::hse},
});
}

/**
 * @brief Clock tree of the STM32L47x/L48x RCC, `STM32L4_rcc_v1_0` on DIE415, RM0351 section 6:
 *        MSI with the twelve ranges of MSIRANGE in RCC_CR once MSIRGSEL is 1, else the range of
 *        MSISRANGE in RCC_CSR, HSI16, HSE, LSI 32 kHz in RCC_CSR and LSE in RCC_BDCR, the main
 *        PLL in RCC_PLLCFGR fed by MSI, HSI16 or HSE, SYSCLK from MSI, HSI16, HSE or the PLLCLK
 *        output, the AHB and two APB prescalers, and the RTC clock of RCC_BDCR with the HSE
 *        divided by 32. The ready flags and enables share RCC_CIFR and RCC_CIER. No TrustZone.
 *        MSIRGSEL is set only, a write of 0 keeps it, and an MSIRANGE code above 11 keeps the
 *        old range, RM0351 6.4.1. MSISRANGE takes a write only while MSIRGSEL is 1, 6.4.30.
 *        CSSON is set only. HSEBYP keeps its value on a write while HSEON is 1 and through every
 *        reset but a power reset, 6.4.1. A BOR is a power reset, 6.1.1. Every field of RCC_BDCR
 *        keeps its value through a system reset, 6.4.29. MSIPLLEN takes a write of 1 only while
 *        LSERDY is 1 and clears when the LSE stops, 6.4.1, and the MSI runs at its range rate
 *        with or without it.
 */
struct l4_rcc
{
  using source = l4::source;
  using signal = l4::signal;

  static constexpr bool msi_standby_ranges       = true;
  static constexpr bool hsi_divider              = false;
  static constexpr bool css_held                 = true;
  static constexpr bool pll1_input_held_while_on = false;

  /** @brief MSIRANGE codes below this take a write, RM0351 6.4.1 hardware write protection. */
  static constexpr std::uint32_t writable_msi_ranges = l4::msi_hz.size();

  /** @brief Fields of RCC_CR that need the LSE: a write keeps them 0 while LSERDY is 0 and they
   *         clear when the LSE stops, RM0351 6.4.1 MSIPLLEN. */
  static constexpr std::array<std::string_view, 1> lse_locked_controls{"MSIPLLEN"};

  static constexpr auto const& oscillators       = l4::oscillators;
  static constexpr auto const& names             = l4::names;
  static constexpr auto const& muxes             = l4::muxes;
  static constexpr auto const& signal_muxes      = l4::signal_muxes;
  static constexpr auto const& divided           = l4::divided;
  static constexpr auto const& internal_signals  = l4::internal_signals;
  static constexpr auto const& rtc_sources       = l4::rtc_sources;
  static constexpr auto const& timers            = l4::timers;
  static constexpr auto const& enable_registers  = l4::enable_registers;
  static constexpr auto const& pll_inputs        = l4::pll_inputs;
  static constexpr auto const& pll_sources       = l4::pll_sources;
  static constexpr auto const& system_inputs     = l4::system_inputs;
  static constexpr auto const& system_sources    = l4::system_sources;
  static constexpr auto const& pll_nodes         = l4::pll_nodes;
  static constexpr auto const& buses             = l4::buses;
  static constexpr auto const& reset_flags       = l4::reset_flags;
  static constexpr auto const& secured           = l4::rcc_secured;
  static constexpr auto const& unit_registers    = l4::rcc_unit_registers;
  static constexpr auto const& unit_aliases      = l4::rcc_unit_aliases;
  static constexpr auto const& unit_suffixes     = l4::rcc_unit_suffixes;
  static constexpr auto const& backup_secured    = l4::rcc_backup_secured;
  static constexpr auto const& system_reset_kept = l4::system_reset_kept;
  static constexpr auto const& power_resets      = l4::power_resets;
  static constexpr auto const& held_while_on     = l4::held_while_on;

  static constexpr std::string_view control_register     = "CR";
  static constexpr std::string_view flag_register        = "CIFR";
  static constexpr std::string_view enable_register      = "CIER";
  static constexpr std::string_view backup_register      = "BDCR";
  static constexpr std::string_view reset_flags_register = "CSR";
  /** @brief Every field of RCC_BDCR keeps its value through a system reset. */
  static constexpr std::string_view backup_excluded = "";
  static constexpr std::uint32_t    sw_mask         = 0b11;

  /**
   * @brief Timer kernel clock over its APB clock, RM0351 6.2.15: 1 when the APB prescaler
   *        divides by 1, else 2.
   * @param in Registers and names of the RCC.
   * @param prescaler Prescaler role of the timer's APB.
   */
  static auto timer_multiplier(rate_inputs const& in, rcc_role prescaler) -> std::uint64_t
  {
    return ppre_divider(in.read(prescaler)) == 1 ? 1 : 2;
  }

  /**
   * @brief Flags of `RCC_CSR` each kind of reset sets, one row per `reset_kind`, RM0351 6.4.30:
   *        BORRSTF and PINRSTF for a power reset, the reset value 0x0C00 0600, then PINRSTF,
   *        SFTRSTF, IWDGRSTF, WWDGRSTF, LPWRSTF and OBLRSTF.
   */
  static constexpr auto reset_flag_rows = std::to_array<reset_flag_row>({
      {reset_kind::power_on,        {"BORRSTF", "PINRSTF"}},
      {reset_kind::brown_out,       {"BORRSTF", "PINRSTF"}},
      {reset_kind::pin,             {"PINRSTF", {}}       },
      {reset_kind::software,        {"SFTRSTF", {}}       },
      {reset_kind::watchdog,        {"IWDGRSTF", {}}      },
      {reset_kind::window_watchdog, {"WWDGRSTF", {}}      },
      {reset_kind::low_power,       {"LPWRSTF", {}}       },
      {reset_kind::option_load,     {"OBLRSTF", {}}       },
      {reset_kind::peripheral,      {}                    },
  });

  static_assert(covers_every_reset_kind(reset_flag_rows));
};
}
}
