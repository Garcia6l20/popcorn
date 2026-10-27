#pragma once

#include <popcorn/core/description.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace popcorn::periph::stm32
{
/** @brief The STM32U5 RCC peripheral reset registers, RM0456 11.8.21 to 11.8.28, in that order.
 *         RCC finds its reset registers in its description, only the tests read this list. */
inline constexpr std::array<std::string_view, 8> reset_registers{
    "RCC_AHB1RSTR",  "RCC_AHB2RSTR1", "RCC_AHB2RSTR2", "RCC_AHB3RSTR",
    "RCC_APB1RSTR1", "RCC_APB1RSTR2", "RCC_APB2RSTR",  "RCC_APB3RSTR"};

/** @brief Instances whose reset bit is not named `<instance>RST` in at least one family, the
 *         STM32U5 RM0456 11.8.21 to 11.8.28 and the STM32H5 RM0481 11.8.19 to 11.8.25. A name
 *         found in no reset register of a part falls back to `<instance>RST`. */
inline constexpr std::array<std::pair<std::string_view, std::string_view>, 17> reset_bit_names{
    {
     {"ADC1", "ADC12RST"},
     {"ADC12", "ADC12RST"},
     {"ADC2", "ADC12RST"},
     {"ADCC", "ADC12RST"},
     {"OTG_FS", "OTGRST"},
     {"USB", "USBFSRST"},
     {"DCMI", "DCMI_PSSIRST"},
     {"PSSI", "DCMI_PSSIRST"},
     {"VREFBUF", "VREFRST"},
     {"FMC", "FSMCRST"},
     {"LPUART", "LPUART1RST"},
     {"DAC", "DAC12RST"},
     {"FDCAN1", "FDCAN12RST"},
     {"FDCAN2", "FDCAN12RST"},
     {"OCTOSPI", "OCTOSPI1RST"},
     {"UCPD1", "UCPDRST"},
     {"I3C", "I3C1RST"},
     }
};

/** @brief Where an instance's reset bit lives in the RCC description. */
struct reset_bit
{
  /** @brief Name of the register, as the description spells it. */
  std::string reg;
  /** @brief Field name of the bit. */
  std::string field;
};

/**
 * @brief Field name of the RCC reset bit of @p instance.
 * @param instance Instance name as the description spells it.
 * @return `<instance>RST`, or the name reset_bit_names gives.
 */
[[nodiscard]] inline auto reset_bit_name(std::string_view instance) -> std::string
{
  auto const renamed = std::ranges::find(
      reset_bit_names, instance, &std::pair<std::string_view, std::string_view>::first);
  return renamed != reset_bit_names.end() ? std::string{renamed->second}
                                          : std::string{instance} + "RST";
}

/**
 * @brief The RCC reset bit of @p instance, searched in every register of @p rcc whose name
 *        holds `RSTR`, so a family with other reset register names, such as the STM32H5
 *        RCC_APB1LRSTR, resolves too. The name of reset_bit_names is searched first, then
 *        `<instance>RST`, so an instance renamed in one family keeps its own bit in the other,
 *        such as the STM32U5 UCPD1RST against the STM32H5 UCPDRST.
 * @param rcc Description of the RCC peripheral.
 * @param instance Instance name as the description spells it.
 * @return The register name and field, nullopt when no reset register of @p rcc has it.
 */
[[nodiscard]] inline auto reset_bit_of(peripheral_desc const& rcc, std::string_view instance)
    -> std::optional<reset_bit>
{
  for(auto const& name : {reset_bit_name(instance), std::string{instance} + "RST"})
  {
    for(auto const& reg : rcc.registers)
    {
      if(not reg.alternate_of
         and reg.name.contains("RSTR")
         and std::ranges::contains(reg.fields, name, &field_desc::name))
      {
        return reset_bit{.reg = reg.name, .field = name};
      }
    }
  }
  return std::nullopt;
}
}
