#pragma once

#include <popcorn/core/description.hpp>
#include <popcorn/periph/stm32/flash.hpp>

#include "flash_unit.hpp"
#include "row_of.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <span>

namespace popcorn::periph::stm32
{
/** @brief Differences between the STM32L1 FLASH controllers the model rows serve, by the
 *         `STM32L1_flash_<category>` version the category patches set. */
struct flash_l1_variant
{
  /** @brief Highest HCLK in kHz at 0 and 1 wait state in voltage ranges 1, 2 and 3, RM0038
   *         Table 13. */
  std::array<std::array<std::uint32_t, 2>, 3> max_khz;
  /** @brief Whether the part has two banks, FLASH_PECR.PARALLELBANK, RM0038 3.9.2. */
  bool dual_bank = false;
  /** @brief FLASH_WRPRx registers the category has, RM0038 3.9.9. */
  std::uint8_t wrp_registers = 1;
  /** @brief A byte or halfword store of 0 to the data EEPROM programs, RM0038 Table 21 note 7.
   *         Cat.1 programs only a non-zero byte or halfword, Table 20 note 7. */
  bool null_partial_writes = true;
  /** @brief FLASH_SR.OPTVERRUSR flags a USER option byte with a wrong complement, RM0038
   *         3.9.7: Cat.3 to Cat.6. */
  bool user_option_flag = true;
  /** @brief FLASH_OBR.SPRMOD turns the WRPx bits at 0 into PCROP read and write protection, and
   *         FLASH_SR.RDERR flags a data read of a PCROP sector, RM0038 3.7.4: the L151, L152
   *         and L162 Cat.2 and Cat.3 on DIE427. */
  bool pcrop = false;
};

/** @brief Table 13 on Cat.2 to Cat.6: Range 3 up to 4.2 MHz at 0 WS and 8 MHz at 1 WS. */
inline constexpr std::array<std::array<std::uint32_t, 2>, 3> l1_flash_wait_khz{
    {{16'000, 32'000}, {8'000, 16'000}, {4'200, 8'000}}
};
/** @brief STM32L1 Cat.1, `STM32L1_flash_cat1`: Range 3 up to 2.1 MHz at 0 WS and 4.2 MHz at
 *         1 WS, RM0038 Table 13. */
inline constexpr flash_l1_variant l1_flash_cat1{
    .max_khz             = {{{16'000, 32'000}, {8'000, 16'000}, {2'100, 4'200}}},
    .null_partial_writes = false,
    .user_option_flag    = false};
/** @brief STM32L1 Cat.2, `STM32L1_flash_cat2`. */
inline constexpr flash_l1_variant l1_flash_cat2{
    .max_khz          = l1_flash_wait_khz,
    .user_option_flag = false};
/** @brief STM32L151, L152 and L162 Cat.2, `STM32L1_flash_cat2_pcrop`: PCROP. */
inline constexpr flash_l1_variant l1_flash_cat2_pcrop{
    .max_khz          = l1_flash_wait_khz,
    .user_option_flag = false,
    .pcrop            = true};
/** @brief STM32L1 Cat.3, `STM32L1_flash_cat3`: FLASH_WRPR2. */
inline constexpr flash_l1_variant l1_flash_cat3{.max_khz = l1_flash_wait_khz, .wrp_registers = 2};
/** @brief STM32L151, L152 and L162 Cat.3 on DIE427, `STM32L1_flash_cat3_pcrop`: FLASH_WRPR2 and
 *         PCROP. */
inline constexpr flash_l1_variant l1_flash_cat3_pcrop{
    .max_khz       = l1_flash_wait_khz,
    .wrp_registers = 2,
    .pcrop         = true};
/** @brief STM32L1 Cat.4 and Cat.6, `STM32L1_flash_cat46`: two banks, FLASH_WRPR2 and
 *         FLASH_WRPR3. */
inline constexpr flash_l1_variant l1_flash_cat46{
    .max_khz       = l1_flash_wait_khz,
    .dual_bank     = true,
    .wrp_registers = 3};
/** @brief STM32L1 Cat.5, `STM32L1_flash_cat5`: two banks, FLASH_WRPR2 to FLASH_WRPR4. */
inline constexpr flash_l1_variant l1_flash_cat5{
    .max_khz       = l1_flash_wait_khz,
    .dual_bank     = true,
    .wrp_registers = 4};

/** @brief Differences between the FLASH controllers the model rows serve. */
struct flash_variant
{
  /** @brief Width of the SECWM, HDP and WRP page fields, RM0456 7.9.17. */
  std::uint8_t watermark_bits = 7;
  /** @brief Checks and encodes host option bytes for the unit and its program memory, and
   *         returns the boot address they select. Null when the row encodes no option bytes. */
  auto (*encode_options)(
      flash_variant const& variant,
      flash_program const& program,
      option_bytes const&  wanted) -> std::expected<address, option_refusal> = nullptr;
};

/** @brief The STM32U5 FLASH encoder, RM0456 7.4.1: the boot address of TZEN and the boot
 *         address options, @p program sized by its first region. */
[[nodiscard]] auto u5_encode_options(
    flash_variant const& variant,
    flash_program const& program,
    option_bytes const&  wanted) -> std::expected<address, option_refusal>;

/** @brief The STM32H5 FLASH encoder, RM0481 7.4.1, as u5_encode_options(). */
[[nodiscard]] auto h5_encode_options(
    flash_variant const& variant,
    flash_program const& program,
    option_bytes const&  wanted) -> std::expected<address, option_refusal>;

/**
 * @brief The STM32L47x/L48x FLASH encoder, BFB2 and DUALBANK of RM0351 3.7.8: the flash base,
 *        or with `swap_bank` the bank 2 base, a popcorn rule: the banks keep their addresses,
 *        where the RM0351 2.6.1 boot loader swaps them. On a flash of 512 KB or less `dualbank`
 *        false gives one bank. Every other field refused when it is not its default, as
 *        `option_bytes.<field>`.
 * @param variant Row of the unit.
 * @param program FLASH unit and its program memory regions, sized by the first.
 * @param wanted Option bytes the machine boots with.
 * @return The boot address, or the refusal.
 */
[[nodiscard]] auto l4_encode_options(
    flash_variant const& variant,
    flash_program const& program,
    option_bytes const&  wanted) -> std::expected<address, option_refusal>;

/**
 * @brief The STM32L1 FLASH encoder, RM0038 3.3.2 and Table 19: the flash base, or with
 *        `swap_bank` the bank 2 base, as the nBFB2 option byte at 0 selects on a part with two
 *        banks. Every other field refused when it is not its default, as `option_bytes.<field>`.
 * @param variant Category of the row.
 * @param program FLASH unit and its program memory regions.
 * @param wanted Option bytes the machine boots with.
 * @return The boot address, or the refusal.
 */
[[nodiscard]] auto l1_encode_options(
    flash_l1_variant const& variant,
    flash_program const&    program,
    option_bytes const&     wanted) -> std::expected<address, option_refusal>;

/** @brief l1_encode_options() for the category @p Variant, as a flash_variant encoder. */
template <flash_l1_variant const& Variant>
[[nodiscard]] auto l1_encode_options_of(
    flash_variant const&,
    flash_program const& program,
    option_bytes const&  wanted) -> std::expected<address, option_refusal>
{
  return l1_encode_options(Variant, program, wanted);
}

/** @brief STM32U5 parts with 7 bits. */
inline constexpr flash_variant flash_u5{.encode_options = u5_encode_options};
/** @brief STM32U535 and STM32U545, 5 bits. */
inline constexpr flash_variant flash_5bit{.watermark_bits = 5, .encode_options = u5_encode_options};
/** @brief STM32U59x, U5Ax, U5Fx and U5Gx, 8 bits. */
inline constexpr flash_variant flash_8bit{.watermark_bits = 8, .encode_options = u5_encode_options};
/** @brief STM32H5. */
inline constexpr flash_variant flash_h5{.encode_options = h5_encode_options};

/** @brief The STM32L47x/L48x FLASH row. */
inline constexpr flash_variant flash_l4{.encode_options = l4_encode_options};
/** @brief The FLASH row variant of an STM32L1 category, derived from its FLASH model variant. */
template <flash_l1_variant const& Variant>
inline constexpr flash_variant flash_l1_of{.encode_options = l1_encode_options_of<Variant>};

/**
 * @brief Factory of the FLASH model, RM0456 section 7.
 * @param options Board configuration, copied into every model made.
 */
[[nodiscard]] auto flash_factory(flash_options options) -> model_factory;

/**
 * @brief Factory of the STM32H5 FLASH model, RM0481 section 7.
 * @param options Board configuration, copied into every model made.
 */
[[nodiscard]] auto flash_h5_factory(flash_options options) -> model_factory;

/**
 * @brief Factory of the STM32L47x/L48x FLASH model, RM0351 section 3.
 * @param options Board configuration, copied into every model made.
 */
[[nodiscard]] auto flash_l4_factory(flash_options options) -> model_factory;

/**
 * @brief Factory of the STM32L1 FLASH and data EEPROM model, RM0038 section 3.
 * @param options Board configuration: only `option_bytes` applies, the boot bank of a part with
 *        two banks.
 * @param variant What differs between the categories the row serves.
 */
[[nodiscard]] auto flash_l1_factory(flash_options const& options, flash_l1_variant variant)
    -> model_factory;

/** @brief A FLASH model row with the variant its units use. */
struct flash_entry
{
  builtin       row;
  flash_variant variant;
};

template <std::meta::info Factory, std::meta::info Variant>
consteval auto flash_entry_of(row_key key) -> flash_entry
{
  if constexpr(std::meta::remove_cvref(std::meta::type_of(Variant)) == ^^flash_l1_variant)
  {
    return {row_of<Factory, Variant, ^^board::flash>(key), flash_l1_of<([:Variant:])>};
  }
  else
  {
    auto row    = row_of<Factory, ^^void, ^^board::flash>(key);
    row.variant = std::meta::identifier_of(Variant);
    return {row, [:Variant:]};
  }
}

/**
 * @brief The FLASH variant of the model row that serves a unit of @p part.
 * @param part Description the machine is built from.
 * @param unit FLASH unit of @p part.
 * @return The row's variant, the default one when no row serves the unit.
 */
[[nodiscard]] auto flash_variant_of(part_description const& part, peripheral_desc const& unit)
    -> flash_variant;

/** @brief The FLASH model rows of the STM32U5, STM32H5 and STM32L4. */
[[nodiscard]] auto flash_rows() -> std::span<builtin const>;

/** @brief The FLASH model rows of the STM32L1. */
[[nodiscard]] auto flash_l1_rows() -> std::span<builtin const>;

/**
 * @brief The FLASH variant of the STM32L1 model row that serves a unit of @p part.
 * @param part Description the machine is built from.
 * @param unit FLASH unit of @p part.
 * @return The row's variant, the default one when no row serves the unit.
 */
[[nodiscard]] auto flash_l1_variant_of(part_description const& part, peripheral_desc const& unit)
    -> flash_variant;
}
