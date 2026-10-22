#pragma once

#include <utility>

namespace popcorn::core
{
/** @brief Marks the machine as inside a handler for the scope's lifetime, restoring the outer
 * state after: views work, state-changing calls throw machine_running. */
class handler_scope
{
public:
  /** @brief Enters a handler. @param in_handler The run state's flag. */
  explicit handler_scope(bool& in_handler) noexcept
      : in_handler_{in_handler}, outer_{std::exchange(in_handler, true)}
  {}
  /** @brief Not copyable. */
  handler_scope(handler_scope const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(handler_scope const&) -> handler_scope& = delete;
  /** @brief Leaves the handler, back to the outer state. */
  ~handler_scope()
  {
    in_handler_ = outer_;
  }

private:
  bool& in_handler_;
  bool  outer_;
};

/** @brief Sets a flag for the scope's lifetime and restores its outer value after, even when the
 * scope unwinds. */
class flag_scope
{
public:
  /** @brief Sets @p flag to @p value. @param flag The flag. @param value Its value in the scope. */
  flag_scope(bool& flag, bool value) noexcept : flag_{flag}, outer_{std::exchange(flag, value)}
  {}
  /** @brief Not copyable. */
  flag_scope(flag_scope const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(flag_scope const&) -> flag_scope& = delete;
  /** @brief Restores the outer value. */
  ~flag_scope()
  {
    flag_ = outer_;
  }

private:
  bool& flag_;
  bool  outer_;
};
}
