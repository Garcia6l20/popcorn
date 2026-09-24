#pragma once

namespace popcorn::periph
{
/** @brief Interrupt request line from a peripheral to the interrupt controller, level-driven. */
class irq_line
{
public:
  /** @brief Drives the line active. */
  void raise();
  /** @brief Drives the line inactive. */
  void lower();
  /** @brief Whether the line is active. */
  [[nodiscard]] auto level() const -> bool;
};

/** @brief DMA request line from a peripheral to a DMA controller, level-driven. */
class dma_request
{
public:
  /** @brief Drives the request active. */
  void request();
  /** @brief Drives the request inactive. */
  void release();
  /** @brief Whether the request is active. */
  [[nodiscard]] auto level() const -> bool;
};
}
