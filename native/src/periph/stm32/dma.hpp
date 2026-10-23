#pragma once

#include <popcorn/core/types.hpp>
#include <popcorn/periph/model.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace popcorn::periph::stm32
{
/**
 * @brief A model that serves DMA requests, such as GPDMA1 or LPDMA1, found through the machine's
 *        peers. Each controller numbers its requests on its own.
 */
class dma_controller
{
public:
  /** @brief Creates the interface. */
  dma_controller() = default;
  /** @brief Not copyable. */
  dma_controller(dma_controller const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(dma_controller const&) -> dma_controller& = delete;
  /** @brief Not movable. */
  dma_controller(dma_controller&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(dma_controller&&) -> dma_controller& = delete;

  /**
   * @brief A request line changed level. Never transfers inside the call: the requester is
   *        inside its own register callback.
   * @param request Request number, the value of the channel's request selection field.
   * @param level New level.
   */
  virtual void on_request(std::uint32_t request, bool level) = 0;

  /**
   * @brief Request number of a signal on this controller.
   * @param signal Signal name as the reference manual spells it, such as `usart1_tx_dma`.
   * @return The number, empty when the controller serves no such signal.
   */
  [[nodiscard]] virtual auto request_of(std::string_view signal) const
      -> std::optional<std::uint32_t> = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~dma_controller() = default;
};

/**
 * @brief A model that issues DMA requests and must know when a channel serving it finishes a
 *        burst or ends, found by the controller through the machine's peers.
 */
class dma_requester
{
public:
  /** @brief Creates the interface. */
  dma_requester() = default;
  /** @brief Not copyable. */
  dma_requester(dma_requester const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(dma_requester const&) -> dma_requester& = delete;
  /** @brief Not movable. */
  dma_requester(dma_requester&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(dma_requester&&) -> dma_requester& = delete;

  /**
   * @brief A hardware-requested channel completed its last transfer, its TC with LLR = 0. Called
   *        from a controller event or the core hold, never inside a requester's register
   *        callback. A request number means something only on its controller: the requester
   *        matches the pair, see dma_signal::carries().
   * @param by Controller of the channel.
   * @param request Request number of the channel on @p by, its request selection field.
   */
  virtual void transfer_completed(dma_controller const& by, std::uint32_t request) = 0;

  /**
   * @brief A hardware-requested channel of @p by finished one burst for @p request. Called from
   *        a controller event or the core hold, never inside a requester's register callback.
   *        The requester drops that request if the burst served it. The default ignores it, for
   *        requesters whose request follows their own flags.
   * @param by Controller of the channel.
   * @param request Request number of the channel on @p by, its request selection field.
   */
  virtual void acknowledged(dma_controller const& by, std::uint32_t request)
  {
    static_cast<void>(by);
    static_cast<void>(request);
  }

protected:
  /** @brief Destroys the interface, only through its model. */
  ~dma_requester() = default;
};

/**
 * @brief Offset of @p at in a window of @p unit, its own or the window of a peripheral that
 *        aliases it.
 * @return The offset, empty outside those windows.
 */
[[nodiscard]] inline auto
    window_offset(part_description const& part, peripheral_desc const& unit, popcorn::address at)
        -> std::optional<std::uint32_t>
{
  for(auto const& window : part.peripherals)
  {
    if((window.name == unit.name or window.alias_of == unit.name)
       and at >= window.base
       and at - window.base < window.size)
    {
      return static_cast<std::uint32_t>(at - window.base);
    }
  }
  return std::nullopt;
}

/** @brief What a feeder tells a DMA channel about the requests it can still raise. */
struct feed_info
{
  /** @brief Requests the feeder can raise from its current state with no event of its own, such
   * as the free words of a transmit FIFO or the held words of a receive FIFO. */
  std::uint32_t room{};
  /** @brief Requests one event of the feeder adds at most. */
  std::uint32_t per_event{1};
  /** @brief Local time of the feeder's next event, empty when none is due. */
  std::optional<vtime> next{};
  /** @brief Shortest time between two events of the feeder, in picoseconds, at least 1. */
  std::uint64_t gap_ps{1};
};

/**
 * @brief A DMA requester that tells a controller when its requests can come, for
 *        periph::model::quiet_until(), found through the machine's peers. Its data registers can
 *        be declared quiet: a DMA access to them raises no flag of its own beyond what its
 *        dma_feed() counts.
 */
class dma_feeder
{
public:
  /** @brief Creates the interface. */
  dma_feeder() = default;
  /** @brief Not copyable. */
  dma_feeder(dma_feeder const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(dma_feeder const&) -> dma_feeder& = delete;
  /** @brief Not movable. */
  dma_feeder(dma_feeder&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(dma_feeder&&) -> dma_feeder& = delete;

  /**
   * @brief The requests @p request on @p by can still bring.
   * @param by Controller asking.
   * @param request Request number on @p by.
   * @param query What the feeder reads from its machine.
   * @return The feed, empty when @p request on @p by is not one of the feeder's signals.
   */
  [[nodiscard]] virtual auto
      dma_feed(dma_controller const& by, std::uint32_t request, quiet_query const& query) const
      -> std::optional<feed_info> = 0;

  /**
   * @brief Whether a DMA access at @p at is to a quiet data register of the feeder.
   * @param at Bus address.
   * @param write Whether the access is a write.
   */
  [[nodiscard]] virtual auto quiet_data(popcorn::address at, bool write) const -> bool = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~dma_feeder() = default;
};

/** @brief What the channels serving one request can still deliver, dma_supplier::dma_supply(). */
struct supply_info
{
  /** @brief Words the channels can still move, empty for no bound, as with a linked list. */
  std::optional<std::uint64_t> words{};
  /** @brief Longest time a channel takes to answer a request with a burst, in picoseconds,
   * empty when one does not answer, as with a stopped clock. */
  std::optional<std::uint64_t> delay_ps{};
};

/** @brief A DMA controller that tells a feeder how much its channels can still move. */
class dma_supplier
{
public:
  /** @brief Creates the interface. */
  dma_supplier() = default;
  /** @brief Not copyable. */
  dma_supplier(dma_supplier const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(dma_supplier const&) -> dma_supplier& = delete;
  /** @brief Not movable. */
  dma_supplier(dma_supplier&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(dma_supplier&&) -> dma_supplier& = delete;

  /**
   * @brief What the active channels serving @p request can still move.
   * @param request Request number on this controller.
   */
  [[nodiscard]] virtual auto dma_supply(std::uint32_t request) const -> supply_info = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~dma_supplier() = default;
};
}
