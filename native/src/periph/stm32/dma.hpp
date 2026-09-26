#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief A model that serves DMA requests: the GPDMA model, found through the machine's peers. */
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
}
