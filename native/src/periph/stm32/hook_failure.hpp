#pragma once

#include <exception>
#include <type_traits>
#include <utility>

namespace popcorn::periph::stm32
{
/**
 * @brief Keeps the first exception a bus device's hook throws while a model event runs, so the
 *        event finishes its state change with a neutral answer for that device, and rethrows the
 *        exception once the event is done. A device whose hook raises answers as an absent one:
 *        no match, no acknowledge, all ones on the data line. A second exception in the same
 *        event is dropped.
 */
class hook_failure
{
public:
  /**
   * @brief Calls @p call, or answers @p neutral when it throws.
   * @param call The hook call.
   * @param neutral The answer of an absent device.
   * @return What @p call returns, or @p neutral.
   */
  template <typename Call, typename Neutral>
  auto guard(Call&& call, Neutral&& neutral) -> std::invoke_result_t<Call>
  {
    try
    {
      return std::forward<Call>(call)();
    }
    catch(...)
    {
      keep();
      return std::forward<Neutral>(neutral);
    }
  }

  /** @brief Calls @p call and keeps the exception it throws. */
  template <typename Call> void run(Call&& call)
  {
    try
    {
      std::forward<Call>(call)();
    }
    catch(...)
    {
      keep();
    }
  }

  /** @brief Throws the exception kept, once. */
  void rethrow()
  {
    if(auto const failure = std::exchange(failure_, nullptr))
    {
      std::rethrow_exception(failure);
    }
  }

private:
  void keep()
  {
    if(not failure_)
    {
      failure_ = std::current_exception();
    }
  }

  std::exception_ptr failure_;
};
}
