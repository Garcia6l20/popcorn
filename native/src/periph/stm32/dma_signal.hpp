#pragma once

#include "dma.hpp"

#include <popcorn/periph/model.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace popcorn::periph::stm32
{
/**
 * @brief DMA request line of a requester model, routed to its controller through the machine's
 *        peers. The requester creates it in its constructor, where the peers are still empty,
 *        resolves it in its reset, and drives it with set(). A signal not resolved yet, as after
 *        a snapshot restored into a machine that was never reset, resolves on its first use. The
 *        controller keeps the request levels and saves them, so the signal holds no state of its
 *        own. A machine reset clears the controller's levels: a requester whose line is still
 *        high after its own reset sets it again. Never call set() from a model destructor.
 */
class dma_signal
{
public:
  /**
   * @brief Names the signal, finds nothing yet.
   * @param peers Models of the machine, which outlive the requester.
   * @param signal Signal name as the reference manual spells it, such as `usart1_tx_dma`.
   * @param controller Controller instance name.
   */
  dma_signal(peer_directory const& peers, std::string signal, std::string controller = "GPDMA1")
      : peers_{&peers}, signal_{std::move(signal)}, controller_name_{std::move(controller)}
  {}

  /**
   * @brief Finds the controller and the request number of the signal.
   * @return Whether the controller exists, serves DMA requests and knows the signal. When not,
   *         set() does nothing.
   */
  auto resolve() -> bool
  {
    controller_        = dynamic_cast<dma_controller*>(peers_->find(controller_name_));
    auto const request = controller_ != nullptr ? controller_->request_of(signal_) : std::nullopt;
    if(not request)
    {
      controller_ = nullptr;
      return false;
    }
    request_ = *request;
    return true;
  }

  /**
   * @brief Drives the request line, resolving it first when needed. The controller transfers
   *        later, never inside this call.
   * @param level New level.
   */
  void set(bool level)
  {
    if(controller_ != nullptr or resolve())
    {
      controller_->on_request(request_, level);
    }
  }

  /** @brief Whether the signal reaches its controller, resolving it first when needed. */
  [[nodiscard]] auto routed() -> bool
  {
    return controller_ != nullptr or resolve();
  }

  /**
   * @brief Whether @p request is this signal's request number on its controller.
   * @param request Request number a controller reports.
   */
  [[nodiscard]] auto carries(std::uint32_t request) -> bool
  {
    return routed() and request_ == request;
  }

private:
  peer_directory const* peers_;
  std::string           signal_;
  std::string           controller_name_;
  dma_controller*       controller_{};
  std::uint32_t         request_{};
};
}
