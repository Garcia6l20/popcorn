#pragma once

#include <popcorn/export.hpp>

#include <cstdint>

namespace popcorn::core
{
class machine;
}

namespace popcorn::periph
{
/** @brief Receiver of line level changes, implemented by the machine. */
class line_bus;

/** @brief Interrupt request line from a peripheral to the interrupt controller, level-driven. */
class POPCORN_API irq_line
{
public:
  /** @brief Creates a line connected to nothing, which only keeps its level. */
  irq_line() = default;
  /** @brief Drives the line active. */
  void raise();
  /** @brief Drives the line inactive. */
  void lower();
  /** @brief Whether the line is active. */
  [[nodiscard]] auto level() const -> bool;

private:
  friend class core::machine;
  irq_line(line_bus& bus, std::uint32_t source) noexcept;

  line_bus*     bus_{};
  std::uint32_t source_{};
  bool          level_{};
};

/** @brief DMA request line from a peripheral to a DMA controller, level-driven. */
class POPCORN_API dma_request
{
public:
  /** @brief Creates a request line connected to nothing, which only keeps its level. */
  dma_request() = default;
  /** @brief Drives the request active. */
  void request();
  /** @brief Drives the request inactive. */
  void release();
  /** @brief Whether the request is active. */
  [[nodiscard]] auto level() const -> bool;

private:
  friend class core::machine;
  dma_request(line_bus& bus, std::uint32_t source) noexcept;

  line_bus*     bus_{};
  std::uint32_t source_{};
  bool          level_{};
};
}
