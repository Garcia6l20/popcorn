#pragma once

#include <popcorn/core/types.hpp>

#include <reflex/enum.hpp>
#include <reflex/serde/annotations.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace popcorn
{
/** @brief Schema version of the MCU description format, bumped on any breaking change. */
inline constexpr std::uint32_t description_schema_version = 1;

/** @brief Access permitted to a register or field, as SVD `access` spells it. */
enum class[[= reflex::derive{reflex::Format, reflex::Parse}]] access
{
  read_only,
  write_only,
  read_write,
  write_once,
  read_write_once,
};

/** @brief Side effect of a write on a field, as SVD `modifiedWriteValues` spells it. */
enum class[[= reflex::derive{reflex::Format, reflex::Parse}]] write_effect
{
  modify,
  one_to_clear,
  one_to_set,
  one_to_toggle,
  zero_to_clear,
  zero_to_set,
  zero_to_toggle,
  clear,
  set,
};

/** @brief Side effect of a read on a field, as SVD `readAction` spells it. */
enum class[[= reflex::derive{reflex::Format, reflex::Parse}]] read_effect
{
  none,
  clear,
  set,
  modify,
  modify_external,
};

/** @brief Effect of a cross-register rule on its target field. */
enum class[[= reflex::derive{reflex::Format, reflex::Parse}]] cross_effect
{
  clear,
  set,
};

/** @brief Kind of memory region. */
enum class[[= reflex::derive{reflex::Format, reflex::Parse}]] region_kind
{
  rom,
  ram,
};

/** @brief Bit field of a register. */
struct field_desc
{
  /** @brief Field name. */
  std::string name;
  /** @brief Position of the least significant bit. */
  std::uint8_t lsb{};
  /** @brief Width in bits. */
  std::uint8_t width{};
  /** @brief Access, the register's when the source gives none. */
  access acc{access::read_write};
  /** @brief Effect of a write. */
  write_effect on_write{};
  /** @brief Effect of a read. */
  read_effect on_read{};

  /** @brief Member-wise equality. */
  friend auto operator==(field_desc const&, field_desc const&) -> bool = default;
};

/** @brief Register of a peripheral. */
struct[[= reflex::serde::omit_if_empty{^^std::optional}]] register_desc
{
  /** @brief Register name. */
  std::string name;
  /** @brief Offset from the peripheral base. */
  std::uint32_t offset{};
  /** @brief Width in bits. */
  std::uint8_t width{};
  /** @brief Value after reset. */
  std::uint32_t reset{};
  /** @brief Bits of reset that are defined. */
  std::uint32_t reset_mask{};
  /** @brief Access, `read_write` when the source gives none. */
  access acc{access::read_write};
  /** @brief Bit fields. */
  std::vector<field_desc> fields;
  /** @brief Register this one shares its storage word with, when it is an alternate view. */
  std::optional<std::string> alternate_of;

  /** @brief Member-wise equality. */
  friend auto operator==(register_desc const&, register_desc const&) -> bool = default;
};

/** @brief Write of 1 to a field of one register acting on a field of another register of the
 * same peripheral, which SVD cannot express, such as `USART_ICR.TCCF` clearing `USART_ISR.TC`. */
struct cross_rule
{
  /** @brief Register written. */
  std::string reg;
  /** @brief Field written. */
  std::string field;
  /** @brief Register acted on. */
  std::string target_reg;
  /** @brief Field acted on. */
  std::string target_field;
  /** @brief What the write does to the target field. */
  cross_effect effect{};

  /** @brief Member-wise equality. */
  friend auto operator==(cross_rule const&, cross_rule const&) -> bool = default;
};

/** @brief Peripheral instance. */
struct[[= reflex::serde::omit_if_empty{^^std::optional}]] peripheral_desc
{
  /** @brief Instance name. */
  std::string name;
  /** @brief IP name, which keys behaviour models. */
  std::string ip;
  /** @brief IP version. */
  std::string ip_version;
  /** @brief Base address. */
  address base{};
  /** @brief Size of the address block in bytes. */
  std::uint32_t size{};
  /** @brief Registers. */
  std::vector<register_desc> registers;
  /** @brief Cross-register rules. */
  std::vector<cross_rule> cross_rules;
  /** @brief IRQ numbers raised by the instance. */
  std::vector<std::uint16_t> irqs;
  /** @brief Peripheral whose storage this one aliases, such as a `SEC_*` instance. */
  std::optional<std::string> alias_of;

  /** @brief Member-wise equality. */
  friend auto operator==(peripheral_desc const&, peripheral_desc const&) -> bool = default;
};

/** @brief Access permitted to a memory region. */
struct region_access
{
  /** @brief Readable. */
  bool read{};
  /** @brief Writable. */
  bool write{};
  /** @brief Executable. */
  bool execute{};

  /** @brief Member-wise equality. */
  friend auto operator==(region_access const&, region_access const&) -> bool = default;
};

/** @brief Memory region. */
struct[[= reflex::serde::omit_if_empty{^^std::optional}]] region_desc
{
  /** @brief Region name. */
  std::string name;
  /** @brief First address. */
  address start{};
  /** @brief Size in bytes. */
  std::uint32_t size{};
  /** @brief Kind of memory. */
  region_kind kind{};
  /** @brief Access permitted. */
  region_access acc{};
  /** @brief Region is in the secure address space. */
  bool secure{};
  /** @brief Start of the region this one aliases and shares storage with. */
  std::optional<address> physical;

  /** @brief Member-wise equality. */
  friend auto operator==(region_desc const&, region_desc const&) -> bool = default;
};

/** @brief Interrupt line. */
struct irq_desc
{
  /** @brief IRQ number, 0 for the first external interrupt. */
  std::uint16_t number{};
  /** @brief IRQ name. */
  std::string name;

  /** @brief Member-wise equality. */
  friend auto operator==(irq_desc const&, irq_desc const&) -> bool = default;
};

/** @brief Processor core. */
struct core_desc
{
  /** @brief Core name, as SVD `cpu/name` spells it. */
  std::string cpu;
  /** @brief Core revision in CPUID variant and revision form, such as `r0p1`. */
  std::string revision;
  /** @brief Floating-point unit present. */
  bool fpu{};
  /** @brief Floating-point unit handles double precision, false for a single-precision unit. */
  bool fpu_double{};
  /** @brief Memory protection unit present. */
  bool mpu{};
  /** @brief Memory protection regions, of the non-secure unit when there are two. */
  std::uint8_t mpu_regions{};
  /** @brief Memory protection regions of the secure unit, 0 without a security extension. */
  std::uint8_t mpu_regions_secure{};
  /** @brief Security attribution regions, 0 without a security extension. */
  std::uint8_t sau_regions{};
  /** @brief Security extension present, TrustZone on Arm. */
  bool trustzone{};
  /** @brief DSP extension present. */
  bool dsp{};
  /** @brief Implemented interrupt priority bits. */
  std::uint8_t nvic_prio_bits{};
  /** @brief Number of external interrupts. */
  std::uint16_t num_irqs{};
  /** @brief The vendor supplies its own system tick configuration instead of the core's. */
  bool vendor_systick_config{};
  /** @brief Core clock out of reset in Hz, before firmware configures clocks, not the maximum. */
  std::uint64_t reset_clock_hz{};

  /** @brief Member-wise equality. */
  friend auto operator==(core_desc const&, core_desc const&) -> bool = default;
};

/** @brief What an MCU is: core, memory map, peripherals and interrupts. */
struct mcu_description
{
  /** @brief Format version, 0 when absent from the file, which the loader refuses. */
  std::uint32_t schema_version{};
  /** @brief Part name, such as `STM32U575QGIx`. */
  std::string part;
  /** @brief Tool that generated the description, name and version. */
  std::string generator;
  /** @brief SHA-256 of each source the description was built from, keyed by role, such as
   * `svd`, `rzone`, `cmsis_header`, `patch` or `mcu_db`. */
  std::map<std::string, std::string> sources;
  /** @brief Processor core. */
  core_desc core;
  /** @brief Memory regions. */
  std::vector<region_desc> regions;
  /** @brief Peripheral instances. */
  std::vector<peripheral_desc> peripherals;
  /** @brief Interrupt lines. */
  std::vector<irq_desc> irqs;

  /** @brief Member-wise equality. */
  friend auto operator==(mcu_description const&, mcu_description const&) -> bool = default;
};

/**
 * @brief Parses a JSON MCU description.
 * @param json JSON text.
 * @param source Name of the text's origin, used in error messages.
 * @return The description.
 * @throws config_error when @p json is malformed, does not match the schema, carries no
 *         schema_version, or carries one other than description_schema_version, naming both.
 */
auto parse_description(std::string_view json, std::string_view source) -> mcu_description;

/**
 * @brief Loads a JSON MCU description file.
 * @param file Path of the file.
 * @return The description.
 * @throws config_error when @p file cannot be read, or as parse_description().
 */
auto load_description(std::filesystem::path const& file) -> mcu_description;

/**
 * @brief Serializes an MCU description to compact JSON, stamped with the current format.
 * @param description Description to serialize.
 * @return JSON text whose schema_version is description_schema_version whatever
 *         @p description carries, which parse_description() reads back to a value equal to
 *         @p description in every other member.
 */
auto dump_description(mcu_description const& description) -> std::string;

/**
 * @brief JSON Schema of the MCU description format, derived from the aggregates by reflection.
 * @return Compact JSON Schema (draft 2020-12) text.
 */
auto description_json_schema() -> std::string;
}
