#pragma once

#include <popcorn/core/description.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/register_file.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <meta>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace popcorn::periph
{
/** @brief Number of enumerators of an enumeration, by reflection. */
template <typename E>
inline constexpr std::size_t effect_count = std::meta::enumerators_of(^^E).size();

/** @brief Position of an enumerator among its enumeration's enumerators, by reflection. */
template <typename E> constexpr auto effect_index(E value) noexcept -> std::size_t
{
  std::size_t index = 0;
  template for(constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^E)))
  {
    if(value == [:e:])
    {
      return index;
    }
    ++index;
  }
  return index;
}

/** @brief Semantics of one storage word, compiled from the registers mapped to it. */
struct word_rules
{
  /** @brief Value after reset, reset_mask applied. */
  std::uint32_t reset = 0;
  /** @brief Bits a read returns. */
  std::uint32_t readable = 0;
  /** @brief Bits a write may change. */
  std::uint32_t writable = 0;
  /** @brief Writable bits that lock after the first write since reset. */
  std::uint32_t write_once = 0;
  /** @brief Bits per write effect, indexed by effect_index(). */
  std::array<std::uint32_t, effect_count<write_effect>> on_write{};
  /** @brief Bits a read clears. */
  std::uint32_t read_clear = 0;
  /** @brief Bits a read sets. */
  std::uint32_t read_set = 0;
};

/** @brief A cross rule resolved to storage words and masks. */
struct cross_target
{
  /** @brief Word the rule watches. */
  std::uint32_t source_word{};
  /** @brief Bits of the source field. */
  std::uint32_t source_mask{};
  /** @brief Word the rule changes. */
  std::uint32_t target_word{};
  /** @brief Bits of the target field. */
  std::uint32_t target_mask{};
  /** @brief What a write of 1 to a source bit does to the target field. */
  cross_effect effect{};
};

/**
 * @brief Register semantics of one peripheral compiled to per-word masks, the storage words
 *        without a register sharing no rules.
 */
class register_layout
{
public:
  /**
   * @brief Compiles a peripheral description.
   * @param desc Peripheral, which must outlive the layout.
   * @throws config_error naming the peripheral and register or rule the flat storage cannot hold:
   *         a register outside one word, two registers of one name or at one offset without
   *         `alternate_of`, alternates with different write effects on shared bits, more than
   *         65535 words with registers, an unknown cross rule register or field.
   */
  explicit register_layout(peripheral_desc const& desc);

  /** @brief Words of storage. */
  [[nodiscard]] auto words() const noexcept -> std::uint32_t;

  /** @brief Rules of a word, all zero for a word without a register. */
  [[nodiscard]] auto rules(std::uint32_t word) const noexcept
      -> word_rules const& pre(word < words());

  /** @brief Cross rules watching a word. */
  [[nodiscard]] auto crosses(std::uint32_t word) const -> std::span<cross_target const>;

  /**
   * @brief Storage word of a register.
   * @param name Register name, as the description spells it.
   * @return The word, empty when the peripheral has no such register.
   */
  [[nodiscard]] auto word_of(std::string_view name) const -> std::optional<std::uint32_t>;

  /** @brief Whether any word has write-once bits. */
  [[nodiscard]] auto has_write_once() const noexcept -> bool;

  /** @brief Bytes the compiled rules take, for memory accounting. */
  [[nodiscard]] auto footprint() const noexcept -> std::size_t;

private:
  peripheral_desc const*     desc_;
  std::vector<std::uint16_t> rule_of_word_;
  std::vector<word_rules>    rules_;
  std::vector<cross_target>  crosses_;
  bool                       write_once_{};
};

/**
 * @brief Run-time state of the register layer beside the storage: write-once locks. A lock
 *        covers a whole word, as W7's `peripheral_image::write_locked` defines it: the first
 *        write since reset that reaches a write-once bit of the word locks every write-once bit
 *        of that word.
 */
struct layer_state
{
  /** @brief Lock bit per word, bit `i % 32` of element `i / 32`, empty for a peripheral without
   * write-once bits. */
  std::vector<std::uint32_t> write_locked{};
};

/**
 * @brief Reads a register the description serves.
 * @param layout Compiled semantics.
 * @param regs Storage.
 * @param offset Byte offset of the access, aligned to @p size.
 * @param size Access size in bytes, 1, 2 or 4.
 * @return Value right-aligned to @p size. Read effects applied to the storage after the read.
 */
auto layer_read(
    register_layout const& layout,
    register_file&         regs,
    std::uint32_t          offset,
    std::uint8_t           size) -> std::uint32_t pre(size == 1 or size == 2 or size == 4)
    pre(offset % size == 0) pre(offset / 4 < layout.words());

/**
 * @brief Writes a register the description serves: access, write effects, write-once lock,
 *        cross rules.
 * @param layout Compiled semantics.
 * @param regs Storage.
 * @param state Write-once locks.
 * @param offset Byte offset of the access, aligned to @p size.
 * @param size Access size in bytes, 1, 2 or 4.
 * @param value Value right-aligned to @p size.
 */
void layer_write(
    register_layout const& layout,
    register_file&         regs,
    layer_state&           state,
    std::uint32_t          offset,
    std::uint8_t           size,
    std::uint32_t          value) pre(size == 1 or size == 2 or size == 4) pre(offset % size == 0)
    pre(offset / 4 < layout.words());

/**
 * @brief Brings the storage of every word no model claims to its reset value and unlocks every
 *        write-once word.
 * @param layout Compiled semantics.
 * @param regs Storage.
 * @param state Write-once locks.
 * @param claimed Non-zero for each word a model serves, left to the model's reset.
 */
void layer_reset(
    register_layout const&        layout,
    register_file&                regs,
    layer_state&                  state,
    std::span<std::uint8_t const> claimed) pre(claimed.size() == layout.words());

/**
 * @brief Matches a glob with `*` and `?` against a text, for the model registry.
 * @param pattern Glob.
 * @param text Text.
 */
auto glob_match(std::string_view pattern, std::string_view text) noexcept -> bool;

/**
 * @brief Factory the model registry holds for a peripheral: the latest registration whose IP
 *        equals the peripheral's and whose glob matches its IP version.
 * @param desc Peripheral.
 * @return The factory, empty when none matches.
 */
auto find_model(peripheral_desc const& desc) -> model_factory;
}
