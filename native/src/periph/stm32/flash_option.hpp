#pragma once

#include "family_names.hpp"
#include "model_base.hpp"

#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief Address of a FLASH option word and the mask of one field in it. */
struct option_bits
{
  address       at{};
  std::uint32_t mask{};
};

/**
 * @brief Where an option field sits in the FLASH peripheral of a part.
 * @param ctx Context of the model.
 * @param option FLASH register and field names.
 * @param role What the model reads the field for, for the error text.
 * @throws config_error when the part has no such FLASH register or field.
 */
[[nodiscard]] inline auto
    option_bits_of(model_context const& ctx, field_name option, std::string_view role)
        -> option_bits
{
  auto const flash =
      std::ranges::find(ctx.part.peripherals, std::string_view{"FLASH"}, &peripheral_desc::name);
  register_desc const* found = nullptr;
  if(flash != ctx.part.peripherals.end())
  {
    found = find_register(*flash, option.reg);
  }
  auto const bits = found == nullptr
                      ? std::vector<field_desc>::const_iterator{}
                      : std::ranges::find(found->fields, option.field, &field_desc::name);
  if(found == nullptr or bits == found->fields.end())
  {
    throw config_error(
        ctx.part.part, 0,
        std::format("peripherals.FLASH.registers.{}.{}", option.reg, option.field),
        std::string{role});
  }
  return {
      .at   = flash->base + found->offset,
      .mask = static_cast<std::uint32_t>(((std::uint64_t{1} << bits->width) - 1) << bits->lsb)};
}
}
