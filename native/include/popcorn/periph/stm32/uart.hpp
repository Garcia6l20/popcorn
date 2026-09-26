#pragma once

#include <popcorn/core/types.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace popcorn::periph::stm32
{
/**
 * @brief The far end of a UART line. Receives what the firmware transmits, one frame at a time,
 *        at the virtual time its stop bit ends. Called from the UART model's event: it must not
 *        run the machine, and it may feed the port's receiver.
 */
class uart_endpoint
{
public:
  /** @brief Creates the endpoint. */
  uart_endpoint() = default;
  /** @brief Not copyable. */
  uart_endpoint(uart_endpoint const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(uart_endpoint const&) -> uart_endpoint& = delete;
  /** @brief Not movable. */
  uart_endpoint(uart_endpoint&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(uart_endpoint&&) -> uart_endpoint& = delete;
  /** @brief Destroys the endpoint. */
  virtual ~uart_endpoint() = default;

  /**
   * @brief A frame left the transmitter.
   * @param data Data bits of the frame, 7 to 9 of them, right-aligned.
   * @param at Virtual time the stop bit ended.
   */
  virtual void transmitted(std::uint16_t data, vtime at) = 0;
};

/**
 * @brief The firmware side of a UART as a board sees it, implemented by the UART model. Reach it
 *        with `dynamic_cast<uart_port*>(machine.model("USART1"))`.
 */
class uart_port
{
public:
  /** @brief Creates the interface. */
  uart_port() = default;
  /** @brief Not copyable. */
  uart_port(uart_port const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(uart_port const&) -> uart_port& = delete;
  /** @brief Not movable. */
  uart_port(uart_port&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(uart_port&&) -> uart_port& = delete;

  /**
   * @brief Queues frames on the line towards the firmware. They arrive one frame time apart at the
   *        configured baud, the first one frame time from now. The queue is saved in snapshots.
   *        An MCU reset drops the frames still queued, as the line keeps no data on hardware.
   * @param data Frames, data bits right-aligned.
   */
  virtual void receive(std::span<std::uint16_t const> data) = 0;

  /** @brief Host frames still queued by receive(), not yet delivered to the receiver. */
  [[nodiscard]] virtual auto queued() const -> std::size_t = 0;

  /**
   * @brief Connects the far end, replacing the board's endpoint. Not saved in snapshots.
   * @param far_end Endpoint, empty to disconnect: transmitted frames are then dropped.
   */
  virtual void attach(std::shared_ptr<uart_endpoint> far_end) = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~uart_port() = default;
};

/** @brief One frame a capture_endpoint kept. */
struct captured_frame
{
  /** @brief Data bits. */
  std::uint16_t data{};
  /** @brief Virtual time its stop bit ended. */
  vtime at{};

  /** @brief Member-wise equality. */
  friend auto operator==(captured_frame const&, captured_frame const&) -> bool = default;
};

/** @brief Keeps every transmitted frame with its time. */
class capture_endpoint final : public uart_endpoint
{
public:
  /** @brief Frames received, in order. */
  std::vector<captured_frame> frames;

  void transmitted(std::uint16_t data, vtime at) override
  {
    frames.push_back({.data = data, .at = at});
  }
};

/** @brief Feeds every transmitted frame back into a port's receiver. */
class loopback_endpoint final : public uart_endpoint
{
public:
  /**
   * @brief Loops back into @p port.
   * @param port Port whose receiver gets the frames, which must outlive the endpoint's use.
   */
  explicit loopback_endpoint(uart_port& port) : port_{&port}
  {}

  void transmitted(std::uint16_t data, vtime) override
  {
    port_->receive(std::span{&data, 1});
  }

private:
  uart_port* port_;
};
}
