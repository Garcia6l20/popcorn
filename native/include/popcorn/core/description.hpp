#pragma once

#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>

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
/** @brief Schema version of the part description format, bumped on any breaking change. */
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
  /** @brief Other names the register answers to in lookups, such as the SVD name `TIM1_CR1`
   * of `CR1`. Unique within the peripheral, among register names and aliases. */
  std::optional<std::vector<std::string>> aliases;

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

/** @brief Alternate function of a GPIO pin: the signal the pin carries when its AFSEL selects
 * the function, from the CubeMX GPIO modes table. */
struct alternate_desc
{
  /** @brief Pin number in its port, 0 to 15. */
  std::uint8_t pin{};
  /** @brief Alternate function number, 0 to 15. */
  std::uint8_t af{};
  /** @brief Signal in lower case, such as `tim3_ch1`. */
  std::string signal;

  /** @brief Member-wise equality. */
  friend auto operator==(alternate_desc const&, alternate_desc const&) -> bool = default;
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
  /** @brief Alternate functions of the pins of a GPIO port, by pin, then function, then signal.
   * Absent on other peripherals and on parts without a GPIO modes table. */
  std::optional<std::vector<alternate_desc>> alternates;

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

/** @brief Bytes a region holds from power-on, over the fill of its kind. */
struct region_content
{
  /** @brief Offset of the first byte in the region. */
  std::uint32_t offset{};
  /** @brief The bytes. */
  std::vector<std::uint8_t> data;

  /** @brief Member-wise equality. */
  friend auto operator==(region_content const&, region_content const&) -> bool = default;
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
  /** @brief Peripheral whose model serves CPU and DMA stores to this read-only region, such as
   * the FLASH interface of a flash bank. */
  std::optional<std::string> controller;
  /** @brief Bytes the region holds from power-on, such as a factory-programmed device
   * signature, in offset order and not overlapping. Only on a rom region without physical. */
  std::optional<std::vector<region_content>> content;

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

/**
 * @brief One range of the implementation defined attribution unit, IDAU, of a part with a
 *        security extension, ARMv8-M ARM B10.4. Addresses from start up to end, end excluded,
 *        are Non-secure, or Secure and Non-secure callable when nsc is set, or exempt from
 *        attribution when exempt is set, B10.4 RMVCM. An address no range covers is Secure.
 *        Ranges are sorted by start and do not overlap.
 */
struct idau_region
{
  /** @brief First address. */
  address start{};
  /** @brief Address past the last one, up to 0x100000000 for a range ending at the top of the
   * address space. */
  std::uint64_t end{};
  /** @brief Secure and Non-secure callable, instead of Non-secure. */
  bool nsc{};
  /** @brief Exempt from attribution: an access there takes the security of its requester. */
  bool exempt{};

  /** @brief Member-wise equality. */
  friend auto operator==(idau_region const&, idau_region const&) -> bool = default;
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
struct part_description
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
  friend auto operator==(part_description const&, part_description const&) -> bool = default;
};

/**
 * @brief Parses a JSON part description.
 * @param json JSON text.
 * @param source Name of the text's origin, used in error messages.
 * @return The description.
 * @throws config_error when @p json is malformed, does not match the schema, carries no
 *         schema_version, or carries one other than description_schema_version, naming both.
 */
POPCORN_API auto parse_description(std::string_view json, std::string_view source)
    -> part_description;

/**
 * @brief Loads a JSON part description file.
 * @param file Path of the file.
 * @return The description.
 * @throws config_error when @p file cannot be read, or as parse_description().
 */
POPCORN_API auto load_description(std::filesystem::path const& file) -> part_description;

/**
 * @brief Serializes a part description to compact JSON, stamped with the current format.
 * @param description Description to serialize.
 * @return JSON text whose schema_version is description_schema_version whatever
 *         @p description carries, which parse_description() reads back to a value equal to
 *         @p description in every other member.
 */
POPCORN_API auto dump_description(part_description const& description) -> std::string;

/**
 * @brief JSON Schema of the part description format, derived from the aggregates by reflection.
 * @return Compact JSON Schema (draft 2020-12) text.
 */
POPCORN_API auto description_json_schema() -> std::string;

/**
 * @brief Digest identifying a description: SHA-256 of dump_description() output, lower-case hex.
 *        dump_description() is canonical, members in declaration order and compact, so equal
 *        descriptions have equal digests. A saved snapshot records it.
 * @param description Description to digest.
 * @return 64 hex digits.
 */
POPCORN_API auto description_digest(part_description const& description) -> std::string;

/**
 * @brief Position of a register in a description: index of its peripheral in
 *        `part_description::peripherals`, then of the register in `peripheral_desc::registers`.
 */
struct register_ref
{
  /** @brief Peripheral index. */
  std::uint32_t peripheral{};
  /** @brief Register index within the peripheral. */
  std::uint32_t reg{};

  /** @brief Member-wise equality. */
  friend auto operator==(register_ref, register_ref) -> bool = default;
};

/**
 * @brief Flat register id, the register string id of trace records: registers are numbered
 *        from 0 in description order, every register of the first peripheral in its order, then
 *        the next peripheral's.
 * @param description Description the reference points into.
 * @param ref Register position, within the description.
 */
POPCORN_API auto flat_register_id(part_description const& description, register_ref ref)
    -> std::uint32_t pre(ref.peripheral < description.peripherals.size())
        pre(ref.reg < description.peripherals[ref.peripheral].registers.size());

/**
 * @brief Register of a flat register id, the inverse of flat_register_id().
 * @param description Description the id numbers.
 * @param id Flat register id.
 * @return Its position, empty when the description has fewer registers.
 */
POPCORN_API auto register_at(part_description const& description, std::uint32_t id)
    -> std::optional<register_ref>;

/**
 * @brief Register of a peripheral by name, or by one of its aliases.
 * @param peripheral Peripheral to search.
 * @param name Register name or alias, such as `CR1` or `TIM1_CR1`.
 * @return The register, null when no register has that name or alias.
 */
[[nodiscard]] POPCORN_API auto
    find_register(peripheral_desc const& peripheral, std::string_view name) -> register_desc const*;

/**
 * @brief IRQ sources a peripheral's model drives: its own in description order, then under
 *        TrustZone those of the peripherals that alias it, in description order, such as
 *        `FLASH_S_IRQn` of `SEC_FLASH`. Without TrustZone a Secure alias raises nothing.
 * @param description Part the peripheral belongs to.
 * @param peripheral Peripheral that is no alias.
 * @param trustzone Whether the machine runs with TrustZone.
 * @return IRQ numbers, own first, in the order of periph::model_context::irqs.
 */
[[nodiscard]] POPCORN_API auto irq_sources(
    part_description const& description,
    peripheral_desc const&  peripheral,
    bool                    trustzone) -> std::vector<std::uint16_t>;
}
