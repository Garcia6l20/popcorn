#pragma once

#include <popcorn/periph/stm32/gpio.hpp>
#include <popcorn/periph/stm32/uart.hpp>

#include <reflex/py.hpp>

#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace popcorn::bind
{
namespace py = reflex::py;

/** @brief One frame the firmware transmitted on a UART. */
struct[[= py::rename{"UartFrame"}]] uart_frame
{
  /** @brief Data bits. */
  [[= py::readonly]] std::uint16_t data{};
  /** @brief Virtual time its stop bit ended, in picoseconds. */
  [[= py::readonly]] std::uint64_t at_ps{};
};

namespace detail
{
/** @brief Transmitted frames a UART link keeps before dropping the oldest, about 16 MiB. */
inline constexpr std::uint64_t uart_frame_limit = std::uint64_t{1} << 20;

/**
 * @brief Far end of a UART that queues transmitted frames for Python to drain, at most `limit`
 *        of them: a full queue drops its oldest frame and counts it.
 */
class queue_endpoint final : public periph::stm32::uart_endpoint
{
public:
  /** @brief Frames not drained yet, oldest first. */
  std::deque<uart_frame> frames;
  /** @brief Frames kept at most. */
  std::uint64_t limit = uart_frame_limit;
  /** @brief Frames dropped because the queue was full. */
  std::uint64_t dropped{};

  void transmitted(std::uint16_t data, vtime at) override
  {
    while(frames.size() >= limit)
    {
      frames.pop_front();
      ++dropped;
    }
    frames.push_back({.data = data, .at_ps = at.ps});
  }
};
}

/**
 * @brief A UART of the machine connected to Python: frames the firmware transmits are queued
 *        with their time, frames Python sends enter the receiver at the configured baud. Valid
 *        while the machine lives, every method raises ConfigError after.
 */
class[[= py::rename{"UartLink"}]] uart_link
{
public:
  /**
   * @brief Connects a queue to a port, replacing its far end.
   * @param port The UART.
   * @param alive Cleared when the machine is destroyed.
   */
  [[= py::skip]] uart_link(periph::stm32::uart_port& port, std::shared_ptr<bool const> alive);

  /**
   * @brief Queues frames towards the firmware, one frame time apart.
   * @param data Frames, one per byte.
   */
  [[= py::doc{"Queue bytes towards the firmware, one frame each."}]] void
      send(reflex::py::nb::bytes const& data);

  /** @brief Frames sent and not yet delivered to the receiver. */
  [[= py::doc{"Frames sent and not yet delivered to the receiver."}]] [[nodiscard]] auto
      queued() const -> std::uint64_t;

  /** @brief Frames transmitted by the firmware and not drained yet. */
  [[= py::doc{"Frames transmitted by the firmware and not drained yet."}]] [[nodiscard]] auto
      available() const noexcept -> std::uint64_t;

  /**
   * @brief Takes the oldest transmitted frames.
   * @param count Most frames taken.
   */
  [[= py::doc{"Take at most count of the oldest transmitted frames."}]] auto
      drain(std::uint64_t count) -> std::vector<uart_frame>;

  /** @brief Transmitted frames dropped because the queue held its limit. */
  [[= py::doc{"Transmitted frames dropped because the queue was full."}]] [[nodiscard]] auto
      dropped() const noexcept -> std::uint64_t;

  /**
   * @brief Sets the most transmitted frames the queue keeps, uart_frame_limit by default.
   * @param limit Frames, at least 1.
   * @throws config_error when @p limit is 0.
   */
  [[= py::doc{"Set the most transmitted frames the queue keeps, for tests."}]] void
      set_frame_limit(std::uint64_t limit);

private:
  [[nodiscard]] auto port() const -> periph::stm32::uart_port&;

  periph::stm32::uart_port*               port_;
  std::shared_ptr<detail::queue_endpoint> endpoint_;
  std::shared_ptr<bool const>             alive_;
};
}
