#pragma once

#include <optional>
#include <string_view>

namespace popcorn::periph::stm32
{
/**
 * @brief The DBGMCU model as a frozen peripheral sees it, found by `dynamic_cast` over the
 *        machine's peers: whether a debug freeze bit is set, looked up by its SVD field name.
 */
class debug_freeze
{
public:
  /** @brief Creates the interface. */
  debug_freeze() = default;
  /** @brief Not copyable. */
  debug_freeze(debug_freeze const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(debug_freeze const&) -> debug_freeze& = delete;
  /** @brief Not movable. */
  debug_freeze(debug_freeze&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(debug_freeze&&) -> debug_freeze& = delete;

  /**
   * @brief Whether a freeze field is set.
   * @param field SVD field name, such as `DBG_IWDG_STOP`.
   * @return Whether it is set, empty when no register of the part has it.
   */
  [[nodiscard]] virtual auto frozen(std::string_view field) const -> std::optional<bool> = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~debug_freeze() = default;
};
}
