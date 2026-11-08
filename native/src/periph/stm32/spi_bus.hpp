#pragma once

/**
 * @file
 * @brief Frame and controller-queue rules shared by the STM32 SPI models.
 */

#include "hook_failure.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/spi.hpp>
#include <popcorn/periph/stm32/gpio.hpp>
#include <popcorn/periph/stm32/spi.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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
 * @param before_queue Called after the start-time check and before the queued frames are counted.
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
    std::span<std::uint32_t const>  frames,
    std::function_ref<void()>       before_queue) -> vtime
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
  before_queue();
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

/** @brief queue_host_transfer() with no step before the queued frames are counted. */
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
  return queue_host_transfer(queue, part, instance, now_ps, start, frame_ps, bits, frames, [] {});
}

/**
 * @brief Records a mode refusal when @p declared differs from @p bus_mode.
 * @param mismatched Refusals the model raises at the next check.
 * @param bus_mode CPOL and CPHA the firmware set, `CPOL << 1 | CPHA`.
 * @param declared Mode a device or controller declares.
 * @param who Returns the name of the one that declares it.
 */
template <typename Who>
void spi_check_mode(
    std::vector<std::string>&   mismatched,
    std::uint8_t                bus_mode,
    std::optional<std::uint8_t> declared,
    Who&&                       who)
{
  if(declared and *declared != bus_mode)
  {
    mismatched.push_back(std::format("the SPI mode {} {} declares", *declared, who()));
  }
}

/**
 * @brief The devices attached to one SPI instance: their chip select subscriptions, mode checks
 *        and snapshot. The instance owns it as a direct member and keeps its address.
 * @tparam State The model's serialized state, holding `selected`, `mode_checked` and `devices`.
 */
template <typename State> class spi_attachments
{
public:
  /**
   * @param state The model's state, which outlives this object.
   * @param attached The devices, in board order.
   */
  spi_attachments(State& state, std::vector<spi_attachment> attached)
      : state_{&state}, attached_{std::move(attached)}
  {}

  /** @brief Not copyable, the chip select listeners hold its address. */
  spi_attachments(spi_attachments const&) = delete;
  /** @brief Not assignable. */
  auto operator=(spi_attachments const&) -> spi_attachments& = delete;

  /** @brief Number of devices. */
  [[nodiscard]] auto size() const -> std::size_t
  {
    return attached_.size();
  }

  /** @brief True when no device is attached. */
  [[nodiscard]] auto empty() const -> bool
  {
    return attached_.empty();
  }

  /** @brief The device at @p index. */
  [[nodiscard]] auto operator[](std::size_t index) const -> spi_attachment const&
  {
    return attached_[index];
  }

  /** @brief First device. */
  [[nodiscard]] auto begin() const
  {
    return attached_.begin();
  }

  /** @brief End of the devices. */
  [[nodiscard]] auto end() const
  {
    return attached_.end();
  }

  /**
   * @brief Listens to the chip select pin of every device that has one. Runs once.
   * @param ctx Context of the owning model, which outlives this object.
   * @param instance SPI instance name for configuration errors.
   * @throws config_error When a chip select names no GPIO port.
   */
  void subscribe(model_context& ctx, std::string_view instance)
  {
    if(std::exchange(subscribed_, true))
    {
      return;
    }
    for(std::size_t index = 0; index < attached_.size(); ++index)
    {
      auto const& attached = attached_[index];
      if(not attached.cs_pin)
      {
        continue;
      }
      auto* const port = dynamic_cast<gpio_port*>(ctx.peers.find(attached.cs_port));
      if(port == nullptr)
      {
        throw config_error(
            ctx.part.part, 0, std::format("board.spi.{}.cs_port", instance),
            std::format("a GPIO port the chip select of a {} device names", instance));
      }
      auto const last = std::none_of(
          attached_.begin() + static_cast<std::ptrdiff_t>(index) + 1, attached_.end(),
          [&](spi_attachment const& later) {
            return later.cs_pin == attached.cs_pin and later.cs_port == attached.cs_port;
          });
      subscriptions_.push_back(
          port->on_change([this, &ctx, index, last](std::uint8_t pin, bool level) {
            auto const& at = attached_[index];
            if(pin != *at.cs_pin or index >= state_->selected.size())
            {
              return;
            }
            auto const selected = level ? 0 : 1;
            if(state_->selected[index] != selected)
            {
              state_->selected[index]     = static_cast<std::uint8_t>(selected);
              state_->mode_checked[index] = 0;
              hooks_.run([&] { at.device->select(selected != 0, ctx.clock.since_power_on()); });
            }
            if(last)
            {
              hooks_.rethrow();
            }
          }));
    }
  }

  /**
   * @brief Checks the mode the device at @p index declares, once per selection.
   * @param index Device index.
   * @param bus_mode CPOL and CPHA the firmware set, `CPOL << 1 | CPHA`.
   * @param mismatched Receives the refusal.
   */
  void check_device(std::size_t index, std::uint8_t bus_mode, std::vector<std::string>& mismatched)
  {
    if(std::exchange(state_->mode_checked[index], 1) != 0)
    {
      return;
    }
    auto const& attached = attached_[index];
    spi_check_mode(mismatched, bus_mode, attached.device->mode(), [&] {
      return std::format("the device on {}", spi_pin_name(attached));
    });
  }

  /** @brief Replaces @p out with the saved state of every device. */
  void save_devices(std::vector<std::vector<std::byte>>& out) const
  {
    out.clear();
    for(auto const& attached : attached_)
    {
      attached.device->save(out.emplace_back());
    }
  }

  /**
   * @brief Restores the devices from the state's `devices`, sizes the selection vectors and
   *        subscribes.
   * @param ctx Context of the owning model.
   * @param instance SPI instance name for configuration errors.
   */
  void load_devices(model_context& ctx, std::string_view instance)
  {
    for(std::size_t index = 0; index < attached_.size() and index < state_->devices.size(); ++index)
    {
      hooks_.run([&] { attached_[index].device->load(state_->devices[index]); });
    }
    state_->devices.clear();
    state_->selected.resize(attached_.size());
    state_->mode_checked.resize(attached_.size());
    subscribe(ctx, instance);
    hooks_.rethrow();
  }

private:
  State*                      state_;
  hook_failure                hooks_;
  std::vector<spi_attachment> attached_;
  std::vector<subscription>   subscriptions_;
  bool                        subscribed_{};
};
}
