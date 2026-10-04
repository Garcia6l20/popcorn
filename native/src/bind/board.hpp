#pragma once

#include <popcorn/periph/stm32/analog.hpp>
#include <popcorn/periph/stm32/audio.hpp>
#include <popcorn/periph/stm32/gpio.hpp>
#include <popcorn/periph/stm32/uart.hpp>

#include <reflex/py.hpp>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
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
/** @brief One frame an SAI audio block transmitted, built when Python reads it. */
struct[[= py::rename{"AudioRecord"}]] audio_record
{
  /** @brief One word per slot, data right-aligned. */
  [[= py::readonly]] std::vector<std::uint32_t> slots{};
  /** @brief Data size in bits. */
  [[= py::readonly]] std::uint8_t data_bits{};
  /** @brief Active slots, bit n for slot n. */
  [[= py::readonly]] std::uint16_t active_slots{};
  /** @brief Mono mode with two slots. */
  [[= py::readonly]] bool mono{};
  /** @brief Virtual time the frame started, in picoseconds. */
  [[= py::readonly]] std::uint64_t at_ps{};
};

namespace detail
{
/**
 * @brief Transmitted frames an audio link keeps before dropping the oldest: 5.4 s at 48 kHz,
 *        4 MiB of 16-bit stereo, 18 MiB of 16-slot TDM, the widest frame.
 */
inline constexpr std::uint64_t audio_frame_limit = std::uint64_t{1} << 18;

/** @brief Consecutive kept frames sharing one layout. */
struct format_run
{
  /** @brief Layout of the frames. */
  periph::stm32::audio_format format{};
  /** @brief Words per frame. */
  std::uint8_t slots{};
  /** @brief Frames in the run. */
  std::uint64_t frames{};
};

/**
 * @brief Far end of an SAI audio block for Python: keeps transmitted frames flat, slot words
 *        back to back with one start time per frame and one layout per run of frames, at most
 *        `limit` frames, a full queue dropping its oldest frame and counting it. Serves words
 *        Python wrote to a receiving block in slot order, then 0.
 */
class audio_queue_endpoint final : public periph::stm32::audio_endpoint
{
public:
  /** @brief Slot words of the kept frames, oldest first. */
  std::deque<std::uint32_t> words;
  /** @brief Start of each kept frame in picoseconds, oldest first. */
  std::deque<std::uint64_t> starts;
  /** @brief Layouts of the kept frames, oldest run first. */
  std::deque<format_run> runs;
  /** @brief Words written and not served to the receiver yet. */
  std::deque<std::uint32_t> pending;
  /** @brief Frames kept at most. */
  std::uint64_t limit = audio_frame_limit;
  /** @brief Frames dropped because the queue was full. */
  std::uint64_t dropped{};

  void transmitted(
      std::span<std::uint32_t const> slots,
      periph::stm32::audio_format    format,
      vtime                          at) override;
  void receive(std::span<std::uint32_t> slots, periph::stm32::audio_format format, vtime at)
      override;

  /** @brief Removes the oldest kept frame. */
  void pop_front();
  /**
   * @brief Takes the oldest kept frames.
   * @param count Most frames taken.
   */
  auto take(std::uint64_t count) -> std::vector<audio_record>;
};
}

/**
 * @brief An SAI audio block of the machine connected to Python: frames the block transmits are
 *        kept with their start time, words Python writes are what the block receives. Valid
 *        while the machine lives, every method raises ConfigError after.
 */
class[[= py::rename{"AudioLink"}]] audio_link
{
public:
  /**
   * @brief Connects a queue to a block, replacing the board's attachment.
   * @param port The SAI.
   * @param block Audio block.
   * @param decimation Frame time multiplier, 0 or 1 for the configured rate.
   * @param external_frame_hz Frame rate of an external clock master, 0 for none.
   * @param clock_wired A bus device drives the external clock at run time.
   * @param alive Cleared when the machine is destroyed.
   */
  [[= py::skip]] audio_link(
      periph::stm32::audio_port&  port,
      periph::stm32::audio_block  block,
      std::uint32_t               decimation,
      std::uint32_t               external_frame_hz,
      bool                        clock_wired,
      std::shared_ptr<bool const> alive);

  /** @brief Frames transmitted and not drained yet. */
  [[= py::doc{"Frames transmitted and not drained yet."}]] [[nodiscard]] auto available() const
      -> std::uint64_t;
  /** @brief Transmitted frames dropped because the queue held its limit. */
  [[= py::doc{"Transmitted frames dropped because the queue was full."}]] [[nodiscard]] auto
      dropped() const -> std::uint64_t;
  /** @brief Words written and not served to the receiver yet. */
  [[= py::doc{"Words written and not served to the receiver yet."}]] [[nodiscard]] auto
      queued() const -> std::uint64_t;
  /**
   * @brief Takes the oldest transmitted frames.
   * @param count Most frames taken.
   */
  [[= py::doc{"Take at most count of the oldest transmitted frames."}]] auto
      drain(std::uint64_t count) -> std::vector<audio_record>;
  /**
   * @brief Queues words for a receiving block, one per slot in slot order.
   * @param words Words, inactive slots included.
   */
  [[= py::doc{"Queue words for a receiving block, one per slot in slot order."}]] void
      write(std::vector<std::uint32_t> const& words);
  /** @brief Drops the kept frames and the words not served yet, and clears the drop count. */
  [[= py::doc{"Drop kept frames and unserved words, clear the drop count."}]] void discard();
  /**
   * @brief Sets the most transmitted frames the queue keeps, audio_frame_limit by default.
   * @param limit Frames, at least 1.
   * @throws config_error when @p limit is 0.
   */
  [[= py::doc{"Set the most transmitted frames the queue keeps, for tests."}]] void
      set_frame_limit(std::uint64_t limit);
  /**
   * @brief Starts, changes or stops the external frame clock of the block, as the board drives
   *        it, periph::stm32::audio_port::set_external_clock().
   * @param frame_hz Frames per second, 0 to stop.
   */
  [[= py::doc{"Start, change or stop the block's external frame clock, 0 to stop."}]] void
      set_external_frame_hz(std::uint32_t frame_hz);
  /** @brief The external frame rate set last, 0 when stopped. */
  [[= py::doc{"The block's external frame rate, 0 when stopped."}]] [[nodiscard]] auto
      external_frame_hz() const -> std::uint32_t;

private:
  [[nodiscard]] auto endpoint() const -> detail::audio_queue_endpoint&;
  [[nodiscard]] auto port() const -> periph::stm32::audio_port&;

  std::shared_ptr<detail::audio_queue_endpoint> endpoint_;
  periph::stm32::audio_port*                    port_;
  periph::stm32::audio_block                    block_;
  std::shared_ptr<bool const>                   alive_;
};
}
