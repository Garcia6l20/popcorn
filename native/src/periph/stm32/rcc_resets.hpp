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
/** @brief The RCC peripheral reset registers, RM0456 11.8.21 to 11.8.28. */
inline constexpr std::array<std::string_view, 8> reset_registers{
    "RCC_AHB1RSTR",  "RCC_AHB2RSTR1", "RCC_AHB2RSTR2", "RCC_AHB3RSTR",
    "RCC_APB1RSTR1", "RCC_APB1RSTR2", "RCC_APB2RSTR",  "RCC_APB3RSTR"};

/** @brief Instances whose reset bit is not named `<instance>RST`. */
inline constexpr std::array<std::pair<std::string_view, std::string_view>, 7> reset_bit_names{
    {
     {"ADC1", "ADC12RST"},
     {"ADC12", "ADC12RST"},
     {"OTG_FS", "OTGRST"},
     {"DCMI", "DCMI_PSSIRST"},
     {"PSSI", "DCMI_PSSIRST"},
     {"VREFBUF", "VREFRST"},
     {"FMC", "FSMCRST"},
     }
};

/** @brief Where an instance's reset bit lives in the RCC description. */
struct reset_bit
{
  /** @brief Index of the register in reset_registers. */
  std::size_t reg;
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
 * @brief The RCC reset bit of @p instance.
 * @param rcc Description of the RCC peripheral.
 * @param instance Instance name as the description spells it.
 * @return The register index and field, nullopt when no reset register of @p rcc has it.
 */
[[nodiscard]] inline auto reset_bit_of(peripheral_desc const& rcc, std::string_view instance)
    -> std::optional<reset_bit>
{
  auto const name = reset_bit_name(instance);
  for(std::size_t index = 0; index < reset_registers.size(); ++index)
  {
    auto const* const reg = find_register(rcc, reset_registers[index]);
    if(reg != nullptr and std::ranges::contains(reg->fields, name, &field_desc::name))
    {
      return reset_bit{.reg = index, .field = name};
    }
  }
  return std::nullopt;
}
}
