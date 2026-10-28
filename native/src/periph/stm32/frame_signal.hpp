#pragma once

#include <popcorn/periph/model.hpp>

#include <cstdint>
#include <vector>

namespace popcorn::periph::stm32
{
/**
 * @brief Model that reacts to the USB start-of-frame signal of a device controller, such as the
 *        CRS taking the SOF as a synchronization source. Found by `dynamic_cast` over the
 *        machine's peers.
 */
class frame_sink
{
public:
  /** @brief Creates the interface. */
  frame_sink() = default;
  /** @brief Not copyable. */
  frame_sink(frame_sink const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(frame_sink const&) -> frame_sink& = delete;
  /** @brief Not movable. */
  frame_sink(frame_sink&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(frame_sink&&) -> frame_sink& = delete;

  /**
   * @brief A device controller received a start of frame while its D+ pull-up is on.
   * @param at_ps Virtual time of the SOF packet.
   */
  virtual void frame_started(std::uint64_t at_ps) = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~frame_sink() = default;
};

/** @brief The SOF output of a device controller. Finds its sinks among the peers on first use. */
class frame_output
{
public:
  /**
   * @brief Keeps the peer directory, finds nothing yet.
   * @param peers Models of the machine, which outlive the controller.
   */
  explicit frame_output(peer_directory const& peers) : peers_{&peers}
  {}

  /**
   * @brief Gives one SOF to every sink.
   * @param at_ps Virtual time of the SOF packet.
   */
  void start(std::uint64_t at_ps)
  {
    if(not resolved_ and not peers_->all().empty())
    {
      resolved_ = true;
      for(auto* const peer : peers_->all())
      {
        if(auto* const sink = dynamic_cast<frame_sink*>(peer))
        {
          sinks_.push_back(sink);
        }
      }
    }
    for(auto* const sink : sinks_)
    {
      sink->frame_started(at_ps);
    }
  }

private:
  peer_directory const*    peers_;
  std::vector<frame_sink*> sinks_;
  bool                     resolved_{};
};
}
