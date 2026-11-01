#pragma once

/**
 * @file
 * @brief Frame and controller-queue rules shared by the STM32 SPI models.
 */

#include <popcorn/core/errors.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/periph/spi.hpp>
#include <popcorn/periph/stm32/spi.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace popcorn::periph::stm32
{
/** @brief One transfer an spi_controller_sink queued, saved with the SPI model. */
struct spi_host_transfer
{
  /** @brief Machine time since reset of its first frame. */
  std::uint64_t start_ps{};
  /** @brief Length of one frame. */
  std::uint64_t frame_ps{};
  /** @brief Frame size. */
  std::uint8_t bits{};
  /** @brief The frames in wire order. */
  std::vector<std::uint32_t> frames;

  friend auto operator==(spi_host_transfer const&, spi_host_transfer const&) -> bool = default;
};

/** @brief A frame the controller hears at its end. */
struct spi_heard_frame
{
  /** @brief Frame the controller sent. */
  std::uint32_t sent;
  /** @brief Frame the SPI drove. */
  std::uint32_t received;
  /** @brief Machine time since reset of the frame end. */
  std::uint64_t at_ps;
};

/** @brief @p name with ASCII capitals lowered, the SPI clock and DMA request names. */
[[nodiscard]] inline auto spi_lower(std::string_view name) -> std::string
{
  std::string out{name};
  std::ranges::transform(out, out.begin(), [](unsigned char c) {
    return static_cast<char>(c >= 'A' and c <= 'Z' ? c - 'A' + 'a' : c);
  });
  return out;
}

/** @brief The low @p bits bits set, every bit from 32. */
[[nodiscard]] constexpr auto spi_mask(std::uint32_t bits) noexcept -> std::uint32_t
{
  return bits >= 32 ? ~std::uint32_t{} : (std::uint32_t{1} << bits) - 1u;
}

/** @brief The low @p bits bits of @p value in the reverse order. */
[[nodiscard]] constexpr auto spi_reversed(std::uint32_t value, std::uint32_t bits) noexcept
    -> std::uint32_t
{
  auto out = std::uint32_t{};
  for(std::uint32_t bit = 0; bit < bits; ++bit)
  {
    out = out << 1 | (value >> bit & 1u);
  }
  return out;
}

static_assert(spi_reversed(0x01, 8) == 0x80);
static_assert(spi_reversed(0x001, 12) == 0x800);
static_assert(spi_reversed(0x1234'5678, 32) == 0x1E6A'2C48);

/** @brief The chip select a device is named by in a mode refusal: its GPIO pin, or `SS`. */
[[nodiscard]] inline auto spi_pin_name(spi_attachment const& attached) -> std::string
{
  if(not attached.cs_pin or attached.cs_port.empty())
  {
    return "SS";
  }
  return std::format("P{}{}", attached.cs_port.back(), *attached.cs_pin);
}

/**
 * @brief Checks one spi_controller_port::transfer() call and appends it to @p queue.
 * @param queue Transfers not finished, in order.
 * @param part Part name for configuration errors.
 * @param instance SPI instance name for configuration errors.
 * @param now_ps Machine time since reset.
 * @param start Earliest start the controller asked for.
 * @param frame_ps Length of one frame.
 * @param bits Frame size.
 * @param frames The frames in wire order.
 * @return Start of the transfer: the later of @p start and the end of the queued transfers.
 * @throws config_error When the queue would hold more than spi_controller_port::max_frames.
 * @throws std::invalid_argument As spi_controller_port::transfer() states.
 */
inline auto queue_host_transfer(
    std::vector<spi_host_transfer>& queue,
    std::string_view                part,
    std::string_view                instance,
    std::uint64_t                   now_ps,
    vtime                           start,
    std::uint64_t                   frame_ps,
    std::uint8_t                    bits,
    std::span<std::uint32_t const>  frames) -> vtime
{
  constexpr auto max_frames = spi_controller_port::max_frames;
  if(frame_ps == 0 or bits < 4 or bits > 32 or frames.empty() or frames.size() > max_frames)
  {
    throw std::invalid_argument(
        "an SPI transfer has 1 to 4096 frames of 4 to 32 bits, each at least 1 ps long");
  }
  if(std::ranges::any_of(
         frames, [&](std::uint32_t frame) { return (frame & ~spi_mask(bits)) != 0; }))
  {
    throw std::invalid_argument(std::format("an SPI frame of {} bits is below 2**{}", bits, bits));
  }
  if(start.ps < now_ps)
  {
    throw std::invalid_argument(
        std::format(
            "an SPI transfer at {} ps starts before the machine time {} ps", start.ps, now_ps));
  }
  auto queued = std::size_t{};
  auto from   = start.ps;
  for(auto const& queued_transfer : queue)
  {
    queued += queued_transfer.frames.size();
    from = std::max(
        from, queued_transfer.start_ps + queued_transfer.frames.size() * queued_transfer.frame_ps);
  }
  if(queued + frames.size() > max_frames)
  {
    throw config_error(
        std::string{part}, 0, std::format("devices.{}", instance),
        std::format(
            "at most {} SPI frames queued, {} are and the transfer adds {}", max_frames, queued,
            frames.size()));
  }
  auto constexpr last = std::numeric_limits<std::uint64_t>::max();
  if(frame_ps > (last - from) / frames.size())
  {
    throw std::invalid_argument("an SPI transfer ends within the 64-bit picosecond range");
  }
  queue.push_back({
      .start_ps = from,
      .frame_ps = frame_ps,
      .bits     = bits,
      .frames   = {frames.begin(), frames.end()}
  });
  return vtime{from};
}
}
