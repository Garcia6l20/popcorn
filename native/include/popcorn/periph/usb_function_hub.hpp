#pragma once

#include <popcorn/core/link_guard.hpp>

#include <cstddef>
#include <map>
#include <mutex>
#include <ranges>

namespace popcorn::periph::usb
{
/**
 * @brief The channels of one USB function kind, one per function index, created on first use,
 *        all under one link lock. The hubs of each function kind derive from it and add their
 *        own save() and load().
 * @tparam Channel The channel of the function kind, default constructible, with a discard()
 *         that forgets the timeline a restore abandons.
 */
template <typename Channel> class function_hub
{
public:
  /** @brief Takes the hub's link lock, blocking. It covers the channel map and every channel. */
  [[nodiscard]] auto lock() const -> core::link_guard
  {
    return core::link_guard{mutex_};
  }

  /** @brief Takes the hub's link lock if it is free. */
  [[nodiscard]] auto try_lock() const -> core::link_guard
  {
    return core::link_guard{mutex_, std::try_to_lock};
  }

  /**
   * @brief The channel of a function, created empty on first use. The reference is valid while
   *        the hub lives, touched only under the hub's lock.
   * @param held The hub's lock.
   * @param function Function index.
   */
  [[nodiscard]] auto channel(core::link_guard const& held, std::size_t function)
      -> Channel& pre(held.guards(mutex_));

  /**
   * @brief Calls Channel::discard() on every channel under the hub's lock. Call it before the
   *        host loads a snapshot.
   */
  void discard()
  {
    auto const held = lock();
    for(auto& entry : channels_ | std::views::values)
    {
      entry.discard();
    }
  }

protected:
  /** @brief An empty hub, built only as the base of a function kind's hub. */
  function_hub() = default;

  /** @brief Destroys the channels. Only the derived hub destroys its base. */
  ~function_hub() = default;

  /**
   * @brief Every channel created so far, by function index.
   * @param held The hub's lock.
   */
  [[nodiscard]] auto channels(core::link_guard const& held)
      -> std::map<std::size_t, Channel>& pre(held.guards(mutex_));

  /**
   * @brief Every channel created so far, by function index.
   * @param held The hub's lock.
   */
  [[nodiscard]] auto channels(core::link_guard const& held) const
      -> std::map<std::size_t, Channel> const& pre(held.guards(mutex_));

private:
  mutable std::mutex             mutex_;
  std::map<std::size_t, Channel> channels_{};
};

template <typename Channel>
auto function_hub<Channel>::channel(core::link_guard const&, std::size_t function) -> Channel&
{
  return channels_[function];
}

template <typename Channel>
auto function_hub<Channel>::channels(core::link_guard const&) -> std::map<std::size_t, Channel>&
{
  return channels_;
}

template <typename Channel>
auto function_hub<Channel>::channels(core::link_guard const&) const
    -> std::map<std::size_t, Channel> const&
{
  return channels_;
}
}
