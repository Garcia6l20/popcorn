#pragma once

#include "model_base.hpp"

#include <popcorn/periph/stm32/usb.hpp>
#include <popcorn/periph/usb.hpp>
#include <popcorn/periph/usb_host.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
/** @brief Bus idle time after which a device that pulls D+ up with no host suspends, 3 ms. */
inline constexpr std::uint64_t usb_idle_ps = 3'000'000'000;

/** @brief The cable and host state a USB device controller model keeps in its snapshots. */
struct usb_link_state
{
  /** @brief Bytes of the host's save(), empty outside a snapshot. */
  std::vector<std::byte> host;
  /** @brief Time of the pending host wake-up. */
  std::uint64_t wake_ps{};
  /** @brief The pull-up the host was last told of. */
  bool pulled_up{};
  /** @brief Whether the cable to the board's host is plugged. */
  bool cable{};

  /** @brief Member-wise equality. */
  friend auto operator==(usb_link_state const&, usb_link_state const&) -> bool = default;
};

/**
 * @brief The host side every USB device controller model shares: the board's host, the cable,
 *        the host given to attach(), wake-ups and the host's bytes in snapshots.
 *
 * A board host, built from the board's usb_hosts, is owned by the model and reached through
 * usb_host_port: its cable is model state, plugged at power-on as the board says, kept across
 * other resets. Its bytes are saved with the cable unplugged too, a detached host then, so state
 * its drivers keep for a later plug survives a restore. A host given to attach() replaces it until
 * the next plug(), and is not saved. Unplugging a device that pulls D+ up leaves the bus idle:
 * 3 ms later the model's `enter_suspend()` runs. Plugging back cancels that suspend.
 *
 * @tparam Model The controller model. It gives `link_state()`, `link(bool)` that recomputes its
 *         pull-up and calls tell_pull_up(), `refresh()`, `schedule(kind, at_ps)`, `cancel(kind)`,
 *         `enter_suspend()`, `now()`, `warn(reg, message)`, and the event kinds
 *         `host_wake_event` and `idle_suspend_event`.
 */
template <typename Model> class usb_device_link : public usb::device_port, public usb_host_port
{
public:
  void attach(std::shared_ptr<usb::host_side> host) override
  {
    host_ = std::move(host);
    model().link(true);
    model().refresh();
  }

  [[nodiscard]] auto board_host() -> usb::host* override
  {
    return board_host_.get();
  }

  [[nodiscard]] auto plugged() const -> bool override
  {
    return board_host_ != nullptr and model().link_state().cable;
  }

  void plug(bool attached) override
  {
    if(not board_host_)
    {
      return;
    }
    auto&      link   = model().link_state();
    auto const was_up = link.pulled_up and host_ == board_host_;
    link.cable        = attached;
    select_host();
    model().link(true);
    if(attached)
    {
      model().cancel(Model::idle_suspend_event);
    }
    else if(was_up)
    {
      board_host_->pull_up_changed(false, model().now());
      model().schedule(Model::idle_suspend_event, model().now().ps + usb_idle_ps);
    }
    model().refresh();
  }

  void wake_at(vtime at) override
  {
    auto& link   = model().link_state();
    link.wake_ps = std::max(at.ps, model().now().ps);
    model().schedule(Model::host_wake_event, link.wake_ps);
  }

protected:
  usb_device_link()           = default;
  ~usb_device_link() override = default;

  /**
   * @brief Builds the board's host when the board wires one, with the cable as it says.
   * @param wired The board's host options for this controller, if any.
   */
  void wire(std::optional<usb_host_options> wired)
  {
    if(not wired)
    {
      return;
    }
    if(not wired->host.warn)
    {
      wired->host.warn = [this](std::string const& message) { model().warn("host", message); };
    }
    board_host_                = std::make_shared<usb::host>(*this, std::move(wired->host));
    plugged_at_power_on_       = wired->attached;
    model().link_state().cable = wired->attached;
    select_host();
  }

  /** @brief The host the device talks to, null with none. */
  [[nodiscard]] auto host() const -> usb::host_side*
  {
    return host_.get();
  }

  /** @brief Takes the board's host when the cable is plugged, none otherwise. */
  void select_host()
  {
    host_ = model().link_state().cable ? board_host_ : nullptr;
  }

  /**
   * @brief Tells the host of a pull-up that differs from the one it last heard of.
   * @param up The pull-up now.
   */
  void tell_pull_up(bool up)
  {
    auto& link = model().link_state();
    if(up != link.pulled_up)
    {
      link.pulled_up = up;
      if(host_)
      {
        host_->pull_up_changed(up, model().now());
      }
    }
  }

  /**
   * @brief Puts back the pull-up and cable a reset keeps, after the model cleared its state, and
   *        plugs the cable back as the board says on a power-on reset.
   * @param before The link state before the reset.
   * @param kind The machine reset kind, empty for an RCC peripheral reset.
   */
  void keep_link(usb_link_state const& before, std::optional<reset_kind> kind)
  {
    auto& link     = model().link_state();
    link.pulled_up = before.pulled_up;
    link.cable     = before.cable;
    if(board_host_ and kind == reset_kind::power_on and before.cable != plugged_at_power_on_)
    {
      plug(plugged_at_power_on_);
    }
  }

  /**
   * @brief The host's bytes for a snapshot: the board host's while it is plugged or no other
   *        host is attached, else the attached host's when the board wires none.
   * @param out Cleared, then filled.
   */
  void save_host(std::vector<std::byte>& out) const
  {
    out.clear();
    if(board_host_ and (host_ == board_host_ or host_ == nullptr))
    {
      board_host_->save(out);
    }
    else if(not board_host_ and host_ != nullptr)
    {
      host_->save(out);
    }
  }

  /** @brief Gives the host its bytes back after a load, then empties them in the state. */
  void load_host()
  {
    auto const blob = std::exchange(model().link_state().host, {});
    if(board_host_)
    {
      select_host();
      board_host_->load(blob);
    }
    else if(host_)
    {
      host_->load(blob);
    }
  }

  /** @brief A host wake-up event fired. */
  void host_woken()
  {
    if(host_)
    {
      host_->on_time(vtime{model().link_state().wake_ps});
    }
  }

  /** @brief The idle time after an unplug ended: the device suspends unless plugged back. */
  void idle_ended()
  {
    if(not model().link_state().cable)
    {
      model().enter_suspend();
    }
  }

private:
  [[nodiscard]] auto model() -> Model&
  {
    return static_cast<Model&>(*this);
  }

  [[nodiscard]] auto model() const -> Model const&
  {
    return static_cast<Model const&>(*this);
  }

  std::shared_ptr<usb::host_side> host_;
  std::shared_ptr<usb::host>      board_host_;
  bool                            plugged_at_power_on_{};
};
}
