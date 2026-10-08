#pragma once

#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <mutex>
#include <system_error>
#include <utility>
#include <vector>

namespace popcorn::core
{
/**
 * @brief Wakes one event loop from any thread without its interpreter: notify() queues a token
 *        and makes an eventfd readable, the loop watches the fd and drain()s the tokens. Knows
 *        nothing of Python. The queue mutex is a leaf: nothing is taken under it.
 */
class loop_waker
{
public:
  /**
   * @brief Opens the eventfd, non-blocking and closed on exec.
   * @throws std::system_error when the eventfd cannot be created.
   */
  loop_waker() : fd_{::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)}
  {
    if(fd_ < 0)
    {
      throw std::system_error(errno, std::generic_category(), "eventfd");
    }
  }

  /** @brief Not copyable. */
  loop_waker(loop_waker const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(loop_waker const&) -> loop_waker& = delete;

  /** @brief Closes the eventfd. */
  ~loop_waker()
  {
    ::close(fd_);
  }

  /** @brief The eventfd a loop watches for reading. */
  [[nodiscard]] auto fd() const noexcept -> int
  {
    return fd_;
  }

  /**
   * @brief Queues @p token, then makes the fd readable. Never blocks on the loop, never throws.
   * @param token What drain() returns for this wake.
   */
  void notify(std::uint64_t token) noexcept
  {
    try
    {
      std::scoped_lock const lock{mutex_};
      tokens_.push_back(token);
    }
    catch(...)
    {
      return;
    }
    std::uint64_t const one = 1;
    std::ignore             = ::write(fd_, &one, sizeof one);
  }

  /**
   * @brief Clears the fd's readiness, then takes every queued token in notify() order. A token
   *        queued after the read makes the fd readable again, so none is left behind unseen.
   * @return The tokens, empty after a spurious wake.
   */
  auto drain() -> std::vector<std::uint64_t>
  {
    std::uint64_t count = 0;
    std::ignore         = ::read(fd_, &count, sizeof count);
    std::scoped_lock const lock{mutex_};
    return std::exchange(tokens_, {});
  }

private:
  int                        fd_;
  std::mutex                 mutex_;
  std::vector<std::uint64_t> tokens_;
};
}
