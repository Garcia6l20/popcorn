#pragma once

#include "../support/inert_model.hpp"
#include "trigger_signal.hpp"

#include <popcorn/periph/model.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace popcorn::testing
{
/**
 * @brief A model with no registers that listens to one trigger signal and records its edges.
 *        Installed in place of a peripheral the case does not use.
 */
class trigger_probe final : public inert_model, public periph::stm32::trigger_sink
{
public:
  /**
   * @brief Records nothing until listening is set.
   * @param ctx Context of the model it replaces.
   */
  explicit trigger_probe(periph::model_context& ctx) : context{ctx}
  {}

  /** @brief Context of the machine, for its peers. */
  periph::model_context& context;
  /** @brief Signal it listens to. */
  std::string watched{"tim15_trgo"};
  /** @brief Whether it listens now. */
  bool listening{};
  /** @brief Rising edge times. */
  std::vector<std::uint64_t> pulses;
  /** @brief Falling edge times. */
  std::vector<std::uint64_t> falls;
  /** @brief Rising edges with their exact instant. */
  std::vector<periph::stm32::trigger_time> rises;

  [[nodiscard]] auto listens(std::string_view signal) const -> bool override
  {
    return listening and signal == watched;
  }
  void triggered(std::string_view, periph::stm32::trigger_edge edge, periph::stm32::trigger_time at)
      override
  {
    (edge == periph::stm32::trigger_edge::rising ? pulses : falls).push_back(at.ps);
    if(edge == periph::stm32::trigger_edge::rising)
    {
      rises.push_back(at);
    }
  }

  /**
   * @brief Starts or stops listening, telling the source on both sides of the change.
   * @param on Whether to listen.
   */
  void listen(bool on)
  {
    static_cast<void>(periph::stm32::interest_changed(context.peers, watched));
    listening = on;
    CHECK(periph::stm32::interest_changed(context.peers, watched));
  }
};
}
