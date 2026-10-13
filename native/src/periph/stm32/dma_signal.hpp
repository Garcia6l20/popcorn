#pragma once

#include "dma.hpp"

#include <popcorn/periph/model.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
/**
 * @brief DMA request line of a requester model, routed through the machine's peers to every
 *        controller that knows the signal, as GPDMA1 and LPDMA1 are wired in parallel on the
 *        STM32U5, RM0456 17.3.3 Table 137 and 18.3.3 Table 147. The requester creates it in
 *        its constructor, where the peers are still empty, resolves it in its reset, and drives
 *        it with set(). A signal not resolved yet, as after a snapshot restored into a machine
 *        that was never reset, resolves on its first use once the peers exist, and stays
 *        resolved: the peers and their request tables are fixed for the machine's life. The
 *        controllers keep the request levels and save them, so the signal holds no state of its
 *        own beyond the routes. A machine reset clears the controllers' levels: a requester
 *        whose line is still high after its own reset sets it again. Never call set() from a
 *        model destructor.
 */
class dma_signal
{
public:
  /**
   * @brief Names the signal, finds nothing yet.
   * @param peers Models of the machine, which outlive the requester.
   * @param signal Signal name as the reference manual spells it, such as `usart1_tx_dma`.
   */
  dma_signal(peer_directory const& peers, std::string signal)
      : peers_{&peers}, signal_{std::move(signal)}
  {}

  /**
   * @brief Finds every peer that is a dma_controller knowing the signal, with its request
   *        number, in the peers' description order.
   * @return Whether at least one controller knows the signal. When none does, set() does
   *         nothing.
   */
  auto resolve() -> bool
  {
    routes_.clear();
    resolved_ = not peers_->all().empty();
    for(auto* const peer : peers_->all())
    {
      if(auto* const controller = dynamic_cast<dma_controller*>(peer); controller != nullptr)
      {
        if(auto const request = controller->request_of(signal_))
        {
          routes_.push_back({.controller = controller, .request = *request});
        }
      }
    }
    return not routes_.empty();
  }

  /**
   * @brief Drives the request line on every controller that knows the signal, in the order
   *        resolve() found them, resolving it first when needed. No controller transfers inside
   *        this call.
   * @param level New level.
   */
  void set(bool level)
  {
    if(routed())
    {
      for(auto const& route : routes_)
      {
        route.controller->on_request(route.request, level);
      }
    }
  }

  /** @brief Whether the signal reaches a controller, resolving it first when needed. */
  [[nodiscard]] auto routed() -> bool
  {
    if(not resolved_)
    {
      resolve();
    }
    return not routes_.empty();
  }

  /**
   * @brief Whether @p request on @p by is this signal: @p by knows the signal under that number.
   * @param by Controller reporting the request.
   * @param request Request number @p by reports.
   */
  [[nodiscard]] auto carries(dma_controller const& by, std::uint32_t request) -> bool
  {
    return routed() and std::ranges::any_of(routes_, [&](route const& known) {
             return known.controller == &by and known.request == request;
           });
  }

private:
  struct route
  {
    dma_controller* controller;
    std::uint32_t   request;
  };

  peer_directory const* peers_;
  std::string           signal_;
  std::vector<route>    routes_;
  bool                  resolved_{};
};
}
