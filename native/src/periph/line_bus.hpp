#pragma once

#include <popcorn/periph/lines.hpp>

#include <cstdint>

namespace popcorn::periph
{
/** @brief Receiver of line level changes, implemented by the machine. */
class line_bus
{
public:
  /** @brief Creates the bus. */
  line_bus() = default;
  /** @brief Not copyable, lines hold its address. */
  line_bus(line_bus const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(line_bus const&) -> line_bus& = delete;
  /** @brief Not movable. */
  line_bus(line_bus&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(line_bus&&) -> line_bus& = delete;

  /**
   * @brief An IRQ line changed level.
   * @param source Interrupt source, `irq_desc::number`.
   * @param level New level.
   */
  virtual void set_irq(std::uint32_t source, bool level) = 0;

  /**
   * @brief A DMA request line changed level.
   * @param source Request source.
   * @param level New level.
   */
  virtual void set_dma(std::uint32_t source, bool level) = 0;

protected:
  /** @brief Destroys the bus, only through its implementation. */
  ~line_bus() = default;
};
}
