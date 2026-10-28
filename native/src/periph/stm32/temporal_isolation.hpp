#pragma once

#include <cstdint>

namespace popcorn::periph::stm32
{
/**
 * @brief The SBS model as the FLASH model sees it, found by `dynamic_cast` over the machine's
 *        peers: the temporal isolation level HDPL of RM0481 14.3.5, which closes the HDP areas.
 */
class temporal_isolation
{
public:
  /** @brief Creates the interface. */
  temporal_isolation() = default;
  /** @brief Not copyable. */
  temporal_isolation(temporal_isolation const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(temporal_isolation const&) -> temporal_isolation& = delete;
  /** @brief Not movable. */
  temporal_isolation(temporal_isolation&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(temporal_isolation&&) -> temporal_isolation& = delete;

  /** @brief Current HDPL, 0 to 3. */
  [[nodiscard]] virtual auto isolation_level() const -> std::uint8_t = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~temporal_isolation() = default;
};
}
