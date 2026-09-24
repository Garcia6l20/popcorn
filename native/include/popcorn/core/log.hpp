#pragma once

#include <popcorn/core/types.hpp>

#include <reflex/enum.hpp>

#include <functional>
#include <string>

namespace popcorn::core
{
/** @brief What a machine log record reports. */
enum class[[= reflex::derive{reflex::Format}]] log_kind
{
  unmodelled_access,
  model_warning,
};

/**
 * @brief One machine log record. An unmodelled access is logged once per register, the first
 *        time firmware reads or writes a register of a peripheral with no behaviour model.
 */
struct log_record
{
  /** @brief What is reported. */
  log_kind kind{};
  /** @brief Address of the instruction that caused it. */
  address pc{};
  /** @brief Peripheral name, empty when not peripheral-specific. */
  std::string peripheral{};
  /** @brief Register name, empty when not register-specific. */
  std::string reg{};
  /** @brief Byte offset of the access within the peripheral. */
  address offset{};
  /** @brief Whether the access was a write. */
  bool write{};
  /** @brief Human-readable text. */
  std::string message{};

  /** @brief Member-wise equality. */
  friend auto operator==(log_record const&, log_record const&) -> bool = default;
};

/** @brief Receives machine log records, on the thread that runs the slice. */
using log_sink = std::function<void(log_record const& record)>;
}
