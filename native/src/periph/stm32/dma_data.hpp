#pragma once

#include <cstdint>

namespace popcorn::periph::stm32
{
/**
 * @brief Bits of a data item of @p width bytes.
 * @param width 1, 2 or 4.
 */
[[nodiscard]] constexpr auto data_mask(std::uint8_t width) noexcept -> std::uint32_t
{
  return width >= 4 ? ~std::uint32_t{} : (std::uint32_t{1} << (8u * width)) - 1u;
}

/**
 * @brief Source byte exchange of GPDMA_CxTR1.SBX, RM0456 17.4.10 "GPDMA data handling": the two
 *        bytes of the unaligned middle half-word of a source word are exchanged.
 * @param value Source data, right-aligned.
 * @param width Source data width in bytes, the exchange applies to words only.
 * @param exchange SBX.
 */
[[nodiscard]] constexpr auto
    exchange_source(std::uint32_t value, std::uint8_t width, bool exchange) noexcept
    -> std::uint32_t
{
  if(not exchange or width != 4)
  {
    return value;
  }
  return (value & 0xFF00'00FFu) | ((value & 0x0000'FF00u) << 8) | ((value >> 8) & 0x0000'FF00u);
}

/**
 * @brief One source data item converted to one destination data item, GPDMA_CxTR1.PAM with
 *        PAM[1] = 0 or equal widths, RM0456 Table 141 "Programmed data handling".
 * @param value Source data after the source byte exchange, right-aligned.
 * @param from Source data width in bytes.
 * @param to Destination data width in bytes.
 * @param pam PAM. With a wider destination, 0 pads with 0s and 1 sign-extends. With a narrower
 *        destination, 0 keeps the low bytes and 1 the high bytes.
 */
[[nodiscard]] constexpr auto
    resize(std::uint32_t value, std::uint8_t from, std::uint8_t to, std::uint32_t pam) noexcept
    -> std::uint32_t
{
  auto const source  = value & data_mask(from);
  auto const pam_low = (pam & 1u) != 0;
  if(to > from)
  {
    auto const sign     = std::uint32_t{1} << (8u * from - 1u);
    auto const negative = pam_low and (source & sign) != 0;
    return (negative ? source | ~data_mask(from) : source) & data_mask(to);
  }
  if(to < from and pam_low)
  {
    return (source >> (8u * (from - to))) & data_mask(to);
  }
  return source & data_mask(to);
}

/**
 * @brief Destination byte and half-word exchanges of GPDMA_CxTR1.DBX and DHX, applied after
 *        the width conversion.
 * @param value Destination data, right-aligned.
 * @param width Destination data width in bytes.
 * @param bytes DBX: the two bytes of each half-word are exchanged, for half-words and words.
 * @param halves DHX: the two half-words of a word are exchanged, for words only.
 */
[[nodiscard]] constexpr auto
    exchange_destination(std::uint32_t value, std::uint8_t width, bool bytes, bool halves) noexcept
    -> std::uint32_t
{
  auto out = value & data_mask(width);
  if(bytes and width >= 2)
  {
    out = ((out & 0x00FF'00FFu) << 8) | ((out >> 8) & 0x00FF'00FFu);
  }
  if(halves and width == 4)
  {
    out = (out << 16) | (out >> 16);
  }
  return out & data_mask(width);
}
}
