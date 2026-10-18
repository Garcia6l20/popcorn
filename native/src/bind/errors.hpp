#pragma once

#include <popcorn/core/errors.hpp>

#include <exception>

namespace popcorn::bind::detail
{
/** @brief A guest memory access the bus refused, raised by the views as BusAccessError with the
 *        failing address and whether the bus denied it. */
class bus_access_error : public emulation_error
{
public:
  /**
   * @brief Builds the error.
   * @param message Text of the error.
   * @param at First address of the refused access.
   * @param denied Whether the bus denied the access, false when nothing is mapped there.
   */
  bus_access_error(std::string const& message, address at, bool denied)
      : emulation_error{message}, at_{at}, denied_{denied}
  {}

  /** @brief First address of the refused access. */
  [[nodiscard]] auto at() const noexcept -> address
  {
    return at_;
  }

  /** @brief Whether the bus denied the access, false when nothing is mapped there. */
  [[nodiscard]] auto denied() const noexcept -> bool
  {
    return denied_;
  }

private:
  address at_;
  bool    denied_;
};

/**
 * @brief Registers the nanobind translator turning each popcorn exception into its class of
 *        `popcorn._errors`, with the context fields as attributes. Other exceptions, a subclass
 *        of popcorn::error declared outside the core included, pass through to the translators
 *        registered before it: nanobind keeps one translator list for every extension.
 */
void register_error_translator();

/**
 * @brief Sets the Python error indicator from @p thrown, as the translator would raise it. A
 *        Python error is raised again as a copy, any other exception as RuntimeError.
 * @param thrown The exception, the GIL held.
 */
void set_python_error(std::exception_ptr const& thrown) noexcept;
}
