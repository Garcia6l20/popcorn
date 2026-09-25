#pragma once

#include <popcorn/core/errors.hpp>

#include <cstdint>
#include <string>

namespace popcorn::obs
{
/** @brief A `.pcov` or `.pctr` file, or a coverage set, is malformed or does not match its
 * images. */
class format_error : public error
{
public:
  /**
   * @brief Constructs a format_error.
   * @param file File or source the fault is in, empty when there is none.
   * @param offset Byte offset of the fault in @p file, 0 when not applicable.
   * @param expected What was expected there.
   */
  format_error(std::string file, std::uint64_t offset, std::string expected);

  /** @brief Destroys the error. Declared out-of-line: the key function pins the vtable. */
  ~format_error() override;

  /** @brief File or source the fault is in. */
  [[nodiscard]] auto file() const noexcept -> std::string const&;
  /** @brief Byte offset of the fault. */
  [[nodiscard]] auto offset() const noexcept -> std::uint64_t;
  /** @brief What was expected there. */
  [[nodiscard]] auto expected() const noexcept -> std::string const&;

private:
  std::string   file_;
  std::uint64_t offset_;
  std::string   expected_;
};
}
