#pragma once

#include <popcorn/periph/model.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
/** @brief Edge of a trigger signal. */
enum class trigger_edge : std::uint8_t
{
  rising,
  falling,
};

/**
 * @brief Model that reacts to trigger signals of other models, such as ADC1 to `tim15_trgo`.
 *        Found by `dynamic_cast` over the machine's peers.
 */
class trigger_sink
{
public:
  /** @brief Creates the interface. */
  trigger_sink() = default;
  /** @brief Not copyable. */
  trigger_sink(trigger_sink const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(trigger_sink const&) -> trigger_sink& = delete;
  /** @brief Not movable. */
  trigger_sink(trigger_sink&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(trigger_sink&&) -> trigger_sink& = delete;

  /**
   * @brief Whether the sink reacts to @p signal now. Derived from its registers only, so the
   *        answer survives a snapshot without state.
   * @param signal Signal name as the reference manual spells it, such as `tim15_trgo`.
   */
  [[nodiscard]] virtual auto listens(std::string_view signal) const -> bool = 0;

  /**
   * @brief @p signal changed level while the sink listens. The sink applies its own edge rule.
   *        Never calls back into the source.
   * @param signal Signal name.
   * @param edge Which edge.
   * @param at_ps Time of the edge. A lazy source learns of its own past events late, so it may
   *        lie before now. The falling edge of a pulse may lie up to its width after now.
   */
  virtual void triggered(std::string_view signal, trigger_edge edge, std::uint64_t at_ps) = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~trigger_sink() = default;
};

/**
 * @brief Model that drives trigger signals and computes them lazily. Found by its instance name,
 *        the signal's prefix in upper case.
 */
class trigger_source
{
public:
  /** @brief Creates the interface. */
  trigger_source() = default;
  /** @brief Not copyable. */
  trigger_source(trigger_source const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(trigger_source const&) -> trigger_source& = delete;
  /** @brief Not movable. */
  trigger_source(trigger_source&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(trigger_source&&) -> trigger_source& = delete;

  /**
   * @brief A sink's interest in one of this model's signals is about to change or has just
   *        changed. The source brings its signals up to now under the interest it sees, then
   *        schedules what that interest needs. A sink calls it on both sides of the change, so
   *        events from before the change are computed under the old interest.
   */
  virtual void interest_changed() = 0;

  /**
   * @brief Whether this model drives @p signal at all, whatever its registers hold.
   * @param signal Signal name.
   */
  [[nodiscard]] virtual auto produces(std::string_view signal) const -> bool = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~trigger_source() = default;
};

/**
 * @brief One output signal of a source. Finds its sinks among the peers on first use, once the
 *        machine filled them, and holds no state of its own.
 */
class trigger_output
{
public:
  /**
   * @brief Names the signal, finds nothing yet.
   * @param peers Models of the machine, which outlive the source.
   * @param signal Signal name as the reference manual spells it, such as `tim15_trgo`.
   */
  trigger_output(peer_directory const& peers, std::string signal)
      : peers_{&peers}, signal_{std::move(signal)}
  {}

  /** @brief Signal name. */
  [[nodiscard]] auto signal() const -> std::string_view
  {
    return signal_;
  }

  /** @brief Whether any sink listens to the signal now. */
  [[nodiscard]] auto wanted() -> bool
  {
    return std::ranges::any_of(
        sinks(), [this](trigger_sink* sink) { return sink->listens(signal_); });
  }

  /**
   * @brief Gives one edge of the signal to every sink listening now.
   * @param edge Which edge.
   * @param at_ps Time of the edge.
   */
  void edge(trigger_edge edge, std::uint64_t at_ps)
  {
    for(auto* const sink : sinks())
    {
      if(sink->listens(signal_))
      {
        sink->triggered(signal_, edge, at_ps);
      }
    }
  }

  /**
   * @brief Gives a positive pulse to every sink listening now: its rising edge, then its falling
   *        edge @p width_ps later.
   * @param at_ps Time of the rising edge.
   * @param width_ps Width of the pulse.
   */
  void pulse(std::uint64_t at_ps, std::uint64_t width_ps)
  {
    edge(trigger_edge::rising, at_ps);
    edge(trigger_edge::falling, at_ps + width_ps);
  }

private:
  auto sinks() -> std::span<trigger_sink* const>
  {
    if(not resolved_ and not peers_->all().empty())
    {
      resolved_ = true;
      for(auto* const peer : peers_->all())
      {
        if(auto* const sink = dynamic_cast<trigger_sink*>(peer))
        {
          sinks_.push_back(sink);
        }
      }
    }
    return sinks_;
  }

  peer_directory const*      peers_;
  std::string                signal_;
  std::vector<trigger_sink*> sinks_;
  bool                       resolved_{};
};

/**
 * @brief Source model of @p signal: the peer named by the signal's prefix up to its first
 *        underscore, in upper case, `TIM15` for `tim15_trgo`.
 * @param peers Models of the machine.
 * @param signal Signal name.
 * @return The source, or null when that peer is absent, is not a trigger_source, or does not
 *         produce @p signal.
 */
[[nodiscard]] inline auto trigger_source_of(peer_directory const& peers, std::string_view signal)
    -> trigger_source*
{
  std::string name{signal.substr(0, signal.find('_'))};
  std::ranges::transform(
      name, name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  auto* const source = dynamic_cast<trigger_source*>(peers.find(name));
  return source != nullptr and source->produces(signal) ? source : nullptr;
}

/**
 * @brief Tells the source of @p signal that a sink's interest in it is about to change or has
 *        just changed, see trigger_source::interest_changed().
 * @param peers Models of the machine.
 * @param signal Signal name.
 * @return Whether a model produces @p signal: its source exists, is a trigger_source and its
 *         produces() says so.
 */
inline auto interest_changed(peer_directory const& peers, std::string_view signal) -> bool
{
  auto* const source = trigger_source_of(peers, signal);
  if(source == nullptr)
  {
    return false;
  }
  source->interest_changed();
  return true;
}
}
