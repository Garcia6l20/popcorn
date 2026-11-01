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
namespace l1
{
enum class source : std::uint8_t
{
  msi,
  hsi,
  hse,
  lsi,
  lse,
  pll1,
};

enum class signal : std::uint8_t
{
  none,
  msi,
  hsi,
  hse,
  lsi,
  lse,
  sysclk,
  hclk,
  pclk1,
  pclk2,
  pllclk,
  rtc,
};

using enum signal;

constexpr std::uint64_t hsi_hz = 16'000'000;
constexpr std::uint64_t lsi_hz = 37'000;

/** @brief MSIRANGE 7 is not allowed, RM0038 6.3.2, and runs at 0 Hz. */
constexpr std::uint32_t msi_ranges = 7;

/**
 * @brief MSI rate of RM0038 6.2.3 and 6.3.2: range n of MSIRANGE runs at 32768 x 2^(n+1) Hz,
 *        65 536 Hz for range 0 to 4 194 304 Hz for range 6, as the CMSIS
 *        `SystemCoreClockUpdate` computes it.
 */
auto msi_rate(rate_inputs const& in) -> std::uint64_t
{
  auto const range = in.read(rcc_role::msis_range);
  return range < msi_ranges ? std::uint64_t{65'536} << range : 0;
}

/** @brief RTCPRE of RCC_CR, RM0038 6.3.1: HSE divided by 2, 4, 8 or 16 for the RTC clock. */
auto rtc_prescaler(rate_inputs const& in) -> std::uint64_t
{
  return std::uint64_t{2} << in.read(rcc_role::rtc_prescaler);
}

/** @brief PLLMUL of RCC_CFGR, RM0038 6.3.3: 3, 4, 6, 8, 12, 16, 24, 32 and 48, 0 for a code
 *         that is not allowed. */
constexpr auto pll_multiplier(std::uint32_t code) -> std::uint64_t
{
  constexpr std::array<std::uint64_t, 9> factors{3, 4, 6, 8, 12, 16, 24, 32, 48};
  return code < factors.size() ? factors[code] : 0;
}

/** @brief PLLDIV of RCC_CFGR, RM0038 6.3.3: 01 to 11 divide the VCO by 2 to 4, 00 is not
 *         allowed and gives 0. */
constexpr auto pll_divider(std::uint32_t code) -> std::uint64_t
{
  return code == 0 ? 0 : std::uint64_t{code} + 1;
}

constexpr std::array<mux<signal>, 0> muxes{};

constexpr std::array<timer_names<signal>, 9> timers{
    {
     {"TIM2", "TIM2EN", pclk1},
     {"TIM3", "TIM3EN", pclk1},
     {"TIM4", "TIM4EN", pclk1},
     {"TIM5", "TIM5EN", pclk1},
     {"TIM6", "TIM6EN", pclk1},
     {"TIM7", "TIM7EN", pclk1},
     {"TIM9", "TIM9EN", pclk2},
     {"TIM10", "TIM10EN", pclk2},
     {"TIM11", "TIM11EN", pclk2},
     }
};

constexpr std::array<std::string_view, 3> enable_registers{"AHBENR", "APB2ENR", "APB1ENR"};

constexpr std::array<oscillator_names<signal>, 6> oscillators{
    {
     {"CR", "MSION", "MSIRDY", "MSIRDYF", "MSIRDYIE", msi, &msi_rate},
     {"CR", "HSION", "HSIRDY", "HSIRDYF", "HSIRDYIE", hsi, &fixed_rate<hsi_hz>},
     {"CR", "HSEON", "HSERDY", "HSERDYF", "HSERDYIE", hse, &hse_rate},
     {"CSR", "LSION", "LSIRDY", "LSIRDYF", "LSIRDYIE", lsi, &fixed_rate<lsi_hz>},
     {"CSR", "LSEON", "LSERDY", "LSERDYF", "LSERDYIE", lse, &lse_rate},
     {"CR", "PLLON", "PLLRDY", "PLLRDYF", "PLLRDYIE"},
     }
};

constexpr auto names = std::to_array<named_field<rcc_role>>({
    {rcc_role::css_on,             {"CR", "CSSON"}      },
    {rcc_role::msi_select,         {}                   },
    {rcc_role::msis_range,         {"ICSCR", "MSIRANGE"}},
    {rcc_role::msik_range,         {}                   },
    {rcc_role::msis_standby_range, {}                   },
    {rcc_role::msik_standby_range, {}                   },
    {rcc_role::sw,                 {"CFGR", "SW"}       },
    {rcc_role::sws,                {"CFGR", "SWS"}      },
    {rcc_role::hpre,               {"CFGR", "HPRE"}     },
    {rcc_role::ppre1,              {"CFGR", "PPRE1"}    },
    {rcc_role::ppre2,              {"CFGR", "PPRE2"}    },
    {rcc_role::ppre3,              {}                   },
    {rcc_role::rtc_select,         {"CSR", "RTCSEL"}    },
    {rcc_role::rtc_enable,         {"CSR", "RTCEN"}     },
    {rcc_role::lsi_prediv,         {}                   },
    {rcc_role::backup_reset,       {"CSR", "RTCRST"}    },
    {rcc_role::remove_flags,       {"CSR", "RMVF"}      },
    {rcc_role::hsi_divider,        {}                   },
    {rcc_role::hsi_divider_flag,   {}                   },
    {rcc_role::timer_prescaler,    {}                   },
    {rcc_role::rtc_prescaler,      {"CR", "RTCPRE"}     },
});

static_assert(names_every_role(names));

constexpr std::array<signal_mux<signal>, 0> signal_muxes{};

constexpr std::array<divided_signal<signal>, 0> divided{};

constexpr std::array<signal, 0> internal_signals{};

constexpr std::array<rtc_input<signal>, 4> rtc_sources{
    {{none, &divide_by<1>}, {lse, &divide_by<1>}, {lsi, &divide_by<1>}, {hse, &rtc_prescaler}}
};

constexpr std::array<source, 2>                pll_inputs{source::hsi, source::hse};
constexpr std::array<signal, 2>                pll_sources{hsi, hse};
constexpr std::array<std::optional<source>, 4> system_inputs{
    source::msi, source::hsi, source::hse, source::pll1};
constexpr std::array<signal, 4> system_sources{msi, hsi, hse, pllclk};

constexpr std::array<bus_node<signal>, 2> buses{
    {{pclk1, rcc_role::ppre1}, {pclk2, rcc_role::ppre2}}
};

constexpr std::array<pll_node<signal>, 1> pll_nodes{
    {
     {.config        = "CFGR",
         .dividers      = "CFGR",
         .fraction      = {},
         .src           = "PLLSRC",
         .range         = {},
         .m             = {},
         .fracen        = {},
         .n             = "PLLMUL",
         .frac          = {},
         .divider       = {"PLLDIV"},
         .output_enable = {},
         .out           = {pllclk},
         .outputs       = 1,
         .m_decode      = &direct,
         .n_decode      = &pll_multiplier,
         .output_decode = {&pll_divider}},
     }
};

constexpr std::array<std::string_view, 7> reset_flags{"LPWRRSTF", "WWDGRSTF", "IWDGRSTF", "SFTRSTF",
                                                      "PORRSTF",  "PINRSTF",  "OBLRSTF"};

constexpr std::array<secured_group, 0> rcc_secured{};

constexpr std::array<std::string_view, 0> rcc_unit_registers{};

constexpr std::array<std::string_view, 0> rcc_unit_suffixes{};

constexpr std::array<secured_group, 0> rcc_backup_secured{};

constexpr std::array<unit_alias, 0> rcc_unit_aliases{};

constexpr auto system_reset_kept = std::to_array<kept_fields>({
    {"CR", "RTCPRE HSEBYP"},
});

constexpr std::array<reset_kind, 2> power_resets{reset_kind::power_on, reset_kind::brown_out};

constexpr auto held_while_on = std::to_array<held_field<source>>({
    {"CR",  "RTCPRE HSEBYP", source::hse},
    {"CSR", "LSEBYP",        source::lse},
});
}

/**
 * @brief Clock tree of the STM32L1 RCC, `STM32L1_rcc_v1_0` on every category, RM0038 section
 *        6: MSI with MSIRANGE in RCC_ICSCR, HSI 16 MHz, HSE, LSI 37 kHz and LSE, one PLL in
 *        RCC_CFGR fed by HSI or HSE with the PLLMUL factor and the PLLDIV divider, SYSCLK from
 *        MSI, HSI, HSE or the PLL, the AHB and two APB prescalers, and the RTC clock of
 *        RCC_CSR with the HSE through RTCPRE of RCC_CR. The ready flags and enables of every
 *        oscillator share RCC_CIR, LSI, LSE, the RTC selection and the reset flags share
 *        RCC_CSR. No kernel clock mux and no TrustZone. The bits of RCC_CSR in the RTC domain
 *        and RTCPRE and HSEBYP of RCC_CR keep their value through a system reset, RM0038 6.1.3,
 *        6.3.1 and 6.3.14, and a BOR is a power reset, RM0038 6.1.2. RTCPRE and HSEBYP keep
 *        their value on a write while HSEON is 1, LSEBYP while LSEON is 1, RM0038 6.3.1 and
 *        6.3.14.
 */
struct l1_rcc
{
  using source = l1::source;
  using signal = l1::signal;

  static constexpr bool msi_standby_ranges       = false;
  static constexpr bool hsi_divider              = false;
  static constexpr bool css_held                 = false;
  static constexpr bool pll1_input_held_while_on = false;

  static constexpr auto const& oscillators       = l1::oscillators;
  static constexpr auto const& names             = l1::names;
  static constexpr auto const& muxes             = l1::muxes;
  static constexpr auto const& signal_muxes      = l1::signal_muxes;
  static constexpr auto const& divided           = l1::divided;
  static constexpr auto const& internal_signals  = l1::internal_signals;
  static constexpr auto const& rtc_sources       = l1::rtc_sources;
  static constexpr auto const& timers            = l1::timers;
  static constexpr auto const& enable_registers  = l1::enable_registers;
  static constexpr auto const& pll_inputs        = l1::pll_inputs;
  static constexpr auto const& pll_sources       = l1::pll_sources;
  static constexpr auto const& system_inputs     = l1::system_inputs;
  static constexpr auto const& system_sources    = l1::system_sources;
  static constexpr auto const& pll_nodes         = l1::pll_nodes;
  static constexpr auto const& buses             = l1::buses;
  static constexpr auto const& reset_flags       = l1::reset_flags;
  static constexpr auto const& secured           = l1::rcc_secured;
  static constexpr auto const& unit_registers    = l1::rcc_unit_registers;
  static constexpr auto const& unit_aliases      = l1::rcc_unit_aliases;
  static constexpr auto const& unit_suffixes     = l1::rcc_unit_suffixes;
  static constexpr auto const& backup_secured    = l1::rcc_backup_secured;
  static constexpr auto const& system_reset_kept = l1::system_reset_kept;
  static constexpr auto const& power_resets      = l1::power_resets;
  static constexpr auto const& held_while_on     = l1::held_while_on;

  static constexpr std::string_view control_register     = "CR";
  static constexpr std::string_view flag_register        = "CIR";
  static constexpr std::string_view enable_register      = "CIR";
  static constexpr std::string_view backup_register      = "CSR";
  static constexpr std::string_view reset_flags_register = "CSR";
  /** @brief Fields of RCC_CSR outside the RTC domain, which a system reset resets: LSION and
   *         LSIRDY, RM0038 6.3.14, and RMVF. The reset flags follow reset_flag_rows. */
  static constexpr std::string_view backup_excluded = "LSION LSIRDY RMVF";
  static constexpr std::uint32_t    sw_mask         = 0b11;

  /**
   * @brief Timer kernel clock over its APB clock, RM0038 6.2: 1 when the APB prescaler divides
   *        by 1, else 2.
   * @param in Registers and names of the RCC.
   * @param prescaler Prescaler role of the timer's APB.
   */
  static auto timer_multiplier(rate_inputs const& in, rcc_role prescaler) -> std::uint64_t
  {
    return ppre_divider(in.read(prescaler)) == 1 ? 1 : 2;
  }

  /**
   * @brief Flags of `RCC_CSR` each kind of reset sets, one row per `reset_kind`, RM0038 6.3.14:
   * PORRSTF and PINRSTF for a power reset, the power-on reset value 0x0C00 0000, then PINRSTF,
   * SFTRSTF, IWDGRSTF, WWDGRSTF, LPWRRSTF and OBLRSTF.
   */
  static constexpr auto reset_flag_rows = std::to_array<reset_flag_row>({
      {reset_kind::power_on,        {"PORRSTF", "PINRSTF"}},
      {reset_kind::brown_out,       {"PORRSTF", "PINRSTF"}},
      {reset_kind::pin,             {"PINRSTF", {}}       },
      {reset_kind::software,        {"SFTRSTF", {}}       },
      {reset_kind::watchdog,        {"IWDGRSTF", {}}      },
      {reset_kind::window_watchdog, {"WWDGRSTF", {}}      },
      {reset_kind::low_power,       {"LPWRRSTF", {}}      },
      {reset_kind::option_load,     {"OBLRSTF", {}}       },
      {reset_kind::peripheral,      {}                    },
  });

  static_assert(covers_every_reset_kind(reset_flag_rows));
};
}
}
