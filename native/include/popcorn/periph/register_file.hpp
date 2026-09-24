#pragma once

#include <popcorn/core/description.hpp>

#include <cstdint>
#include <vector>

namespace popcorn::periph
{
/**
 * @brief Storage of one peripheral instance. Serves every register no model claims with the
 *        description's semantics: access, reset value, write and read effects, cross rules.
 *
 * For a register a model claims, the dispatch never writes the storage: the model pokes whatever
 * it wants visible there. peek() is the side-effect-free view that Python register views, trace
 * and snapshots read, so a model keeps the storage of its claimed registers current.
 * Neither copyable nor movable: models and bindings hold references to it.
 */
class register_file
{
public:
  /**
   * @brief Creates the storage of a peripheral, every word zero.
   * @param desc Description of the peripheral, must outlive the register file.
   */
  explicit register_file(peripheral_desc const& desc);

  /** @brief Not copyable. */
  register_file(register_file const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(register_file const&) -> register_file& = delete;
  /** @brief Not movable. */
  register_file(register_file&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(register_file&&) -> register_file& = delete;
  /** @brief Destroys the storage. */
  ~register_file();

  /** @brief Description of the peripheral. */
  [[nodiscard]] auto description() const noexcept -> peripheral_desc const&;

  /**
   * @brief Stored value of a register, without read effects.
   * @param offset Word-aligned offset of the register from the peripheral base, below the
   *        peripheral size.
   */
  [[nodiscard]] auto peek(std::uint32_t offset) const -> std::uint32_t
      pre(offset % 4 == 0 and offset < description().size);

  /**
   * @brief Stores a register value, without write effects or access checks.
   * @param offset Word-aligned offset of the register from the peripheral base, below the
   *        peripheral size.
   * @param value Value stored.
   */
  void poke(std::uint32_t offset, std::uint32_t value)
      pre(offset % 4 == 0 and offset < description().size);

private:
  peripheral_desc const*     desc_;
  std::vector<std::uint32_t> words_;
};
}
