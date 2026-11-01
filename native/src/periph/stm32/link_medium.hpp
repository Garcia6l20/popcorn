#pragma once

#include <popcorn/core/errors.hpp>
#include <popcorn/core/member_link.hpp>

#include <reflex/serde/bson.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32::detail
{
/** @brief @p at moved by @p ps picoseconds. */
[[nodiscard]] inline auto plus(core::sys_time at, std::int64_t ps) -> core::sys_time
{
  return {at.ps + ps};
}

/** @brief The latest of @p resets after @p stamp, if any. */
[[nodiscard]] inline auto
    latest_after(std::vector<core::sys_time> const& resets, core::sys_time stamp)
        -> std::optional<core::sys_time>
{
  std::optional<core::sys_time> found;
  for(auto const& reset : resets)
  {
    if(reset > stamp and (not found or reset > *found))
    {
      found = reset;
    }
  }
  return found;
}

/** @brief The earlier of two faults, @p kept on a tie. */
[[nodiscard]] inline auto
    earliest(std::optional<core::link_fault> kept, std::optional<core::link_fault> found)
        -> std::optional<core::link_fault>
{
  if(not kept or (found and found->at < kept->at))
  {
    return found;
  }
  return kept;
}

/**
 * @brief Loads a link state of @p State from @p in.
 * @param lower_name The medium's name in the error's file, `i2c` or `spi`.
 * @param upper_name The medium's name in the error's text, `I2C` or `SPI`.
 * @throws config_error When @p in does not decode, or holds another kind of state.
 */
template <typename State>
auto decoded(
    std::span<std::byte const> in,
    std::string_view           lower_name,
    std::string_view           upper_name) -> State
{
  State loaded;
  try
  {
    loaded = reflex::serde::bson::deserializer{in}.load<State>();
  }
  catch(std::runtime_error const& failure)
  {
    throw config_error(
        std::format("{} link state", lower_name), 0, "bytes",
        std::format(
            "a state an {} link of kind {} saved: {}", upper_name, State{}.kind, failure.what()));
  }
  if(loaded.kind != State{}.kind)
  {
    throw config_error(
        std::format("{} link state", lower_name), 0, "kind",
        std::format(
            "an {} link state of kind {}, the bytes hold {}", upper_name, State{}.kind,
            loaded.kind));
  }
  return loaded;
}

/** @brief The default of drain_mail()'s `dropped`: every post goes on to the carry rule. */
struct no_post_dropped
{
  auto operator()(auto const&) const noexcept -> bool
  {
    return false;
  }
};

/** @brief The default of drain_mail()'s `taken`: nothing before the hand. */
struct no_post_taken
{
  void operator()(auto const&) const noexcept
  {}
};

/**
 * @brief The drain of one link of a medium. Forgets the resets whose far side lies at or before
 *        the last barrier, then walks the carried posts and the mailbox of @p window in order. A
 *        post for which @p dropped returns true leaves the link. A post whose far-side start,
 *        stamp plus @p latency, lies at or past @p next, or that follows a carried post, is
 *        carried. Every other post goes to @p taken, then to @p hand. A throw from @p hand is a
 *        fault on @p member at the later of the post's stamp and the last barrier, the earliest
 *        one kept. Stores @p barrier as the last barrier.
 * @tparam State A link state with `resets`, `carried`, `mail_even`, `mail_odd` and `last_barrier`.
 * @return The earliest fault, if any.
 */
template <
    typename State,
    typename Hand,
    typename Dropped = no_post_dropped,
    typename Taken   = no_post_taken>
auto drain_mail(
    State&                state,
    std::uint64_t         window,
    core::sys_time const& barrier,
    core::sys_time const& next,
    core::sys_time        latency,
    std::size_t           member,
    Hand                  hand,
    Dropped               dropped = {},
    Taken                 taken   = {}) -> std::optional<core::link_fault>
{
  auto const before = state.last_barrier;
  std::erase_if(
      state.resets, [&](core::sys_time const& reset) { return plus(reset, latency.ps) <= before; });
  auto  posts = std::exchange(state.carried, {});
  auto& mail  = window % 2 == 0 ? state.mail_even : state.mail_odd;
  posts.insert(posts.end(), mail.begin(), mail.end());
  mail.clear();
  std::optional<core::link_fault> fault;
  for(auto const& post : posts)
  {
    if(dropped(post))
    {
      continue;
    }
    if(not state.carried.empty() or plus(post.stamp, latency.ps) >= next)
    {
      state.carried.push_back(post);
      continue;
    }
    taken(post);
    try
    {
      hand(post);
    }
    catch(...)
    {
      fault = earliest(
          fault, core::link_fault{
                     .at     = std::max(post.stamp, before),
                     .member = member,
                     .error  = std::current_exception()});
    }
  }
  state.last_barrier = barrier;
  return fault;
}
}
