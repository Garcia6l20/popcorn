#pragma once

#include <popcorn/core/errors.hpp>

namespace popcorn::bind::detail
{
/** @brief A guest memory access the bus refused, raised by the views as EmulationError with the
 *        failing address. */
class bus_access_error : public emulation_error
{
public:
  /**
   * @brief Builds the error.
   * @param message Text of the error.
   * @param at First address of the refused access.
   */
  bus_access_error(std::string const& message, address at) : emulation_error{message}, at_{at}
  {}

  /** @brief First address of the refused access. */
  [[nodiscard]] auto at() const noexcept -> address
  {
    return at_;
  }

private:
  address at_;
};

/**
 * @brief Registers the nanobind translator turning each popcorn exception into its class of
 *        `popcorn._errors`, with the context fields as attributes. Other exceptions, a subclass
 *        of popcorn::error declared outside the core included, pass through to the translators
 *        registered before it: nanobind keeps one translator list for every extension.
 */
void register_error_translator();
}
