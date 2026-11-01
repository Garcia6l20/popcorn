#pragma once

#include <popcorn/periph/model.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

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

  /**
   * @brief A hardware-requested channel of @p by moved the last but one item of its count, the
   *        EOT_1 signal of RM0038 26.3.7. Called from a controller event, never inside a
   *        requester's register callback, by the STM32L1 classic DMA. The default ignores it.
   * @param by Controller of the channel.
   * @param request Request number of the channel on @p by.
   */
  virtual void last_but_one(dma_controller const& by, std::uint32_t request)
  {
    static_cast<void>(by);
    static_cast<void>(request);
  }

  /**
   * @brief A hardware-requested channel of @p by moved the last item of its count, at the end of
   *        a normal channel and at each reload of a circular one. Called from a controller event
   *        or the core hold, never inside a requester's register callback, by the STM32L1
   *        classic DMA. The default ignores it.
   * @param by Controller of the channel.
   * @param request Request number of the channel on @p by.
   */
  virtual void count_ended(dma_controller const& by, std::uint32_t request)
  {
    static_cast<void>(by);
    static_cast<void>(request);
  }

protected:
  /** @brief Destroys the interface, only through its model. */
  ~dma_requester() = default;
};

/** @brief The DMA requesters of a machine, found once among its peers in their order. */
struct dma_requester_list
{
  /** @brief Requesters found, empty until resolve() sees a peer. */
  std::vector<dma_requester*> found;
  /** @brief Whether found holds the peers' requesters. */
  bool resolved{};

  /**
   * @brief The requesters among @p peers. The first call that sees a peer collects them.
   * @param peers Peers of the machine.
   * @return The requesters, in peer order.
   */
  [[nodiscard]] auto resolve(peer_directory const& peers) -> std::span<dma_requester* const>
  {
    if(not resolved and not peers.all().empty())
    {
      resolved = true;
      for(auto* const peer : peers.all())
      {
        if(auto* const requester = dynamic_cast<dma_requester*>(peer); requester != nullptr)
        {
          found.push_back(requester);
        }
      }
    }
    return found;
  }
};
}
