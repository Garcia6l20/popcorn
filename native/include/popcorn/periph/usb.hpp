#pragma once

#include <popcorn/core/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace popcorn::periph::usb
{
/** @brief The 8 bytes of a SETUP transaction's data stage, USB 2.0 section 9.3. */
struct setup_packet
{
  /** @brief bmRequestType: direction, type and recipient. */
  std::uint8_t request_type{};
  /** @brief bRequest. */
  std::uint8_t request{};
  /** @brief wValue. */
  std::uint16_t value{};
  /** @brief wIndex. */
  std::uint16_t index{};
  /** @brief wLength: bytes of the data stage. */
  std::uint16_t length{};

  /** @brief The packet as it travels on the bus, little-endian fields. */
  [[nodiscard]] constexpr auto bytes() const noexcept -> std::array<std::uint8_t, 8>
  {
    return {
        request_type,
        request,
        static_cast<std::uint8_t>(value),
        static_cast<std::uint8_t>(value >> 8),
        static_cast<std::uint8_t>(index),
        static_cast<std::uint8_t>(index >> 8),
        static_cast<std::uint8_t>(length),
        static_cast<std::uint8_t>(length >> 8)};
  }

  /** @brief Member-wise equality. */
  friend auto operator==(setup_packet const&, setup_packet const&) -> bool = default;
};

/**
 * @brief How the device answered a token. `none` is no answer at all: a token to another
 *        address, to an endpoint that is not enabled, or for isochronous traffic in this frame,
 *        or a device that is not on the bus.
 *
 * Isochronous transactions have no handshake on the bus, and the calls report them this way:
 * an iso IN the device answers with a packet is `ack` with the data, a zero-length packet
 * included. An iso OUT the device takes is `ack`. `none` means the endpoint is not addressed
 * or not enabled for this frame, or the device could not take the iso OUT packet: above the
 * maximum packet size, no room in the receive FIFO, or a global OUT NAK. `nak` and `stall` never
 * occur for iso.
 */
enum class handshake : std::uint8_t
{
  ack,
  nak,
  stall,
  none,
};

/** @brief Data PID of a full-speed data packet, the data toggle of USB 2.0 section 8.6. */
enum class data_pid : std::uint8_t
{
  data0,
  data1,
};

/** @brief Answer of the device to an IN token. */
struct in_result
{
  /** @brief Handshake. With `ack`, @ref data and @ref pid are the packet sent, also for an
   * isochronous IN, which has no handshake on the bus. */
  handshake hs{handshake::none};
  /** @brief Data PID of the packet. */
  data_pid pid{data_pid::data0};
  /** @brief Payload, empty for a zero-length packet. */
  std::vector<std::uint8_t> data;
};

/**
 * @brief The host as the device sees it, told what the device changed on the bus. Called from
 *        the device model, during a register write or one of its events: it must not run the
 *        machine.
 */
class host_side
{
public:
  /** @brief Creates the interface. */
  host_side() = default;
  /** @brief Not copyable. */
  host_side(host_side const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(host_side const&) -> host_side& = delete;
  /** @brief Not movable. */
  host_side(host_side&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(host_side&&) -> host_side& = delete;
  /** @brief Destroys the host. */
  virtual ~host_side() = default;

  /**
   * @brief The D+ pull-up of a full-speed device was switched.
   * @param on Whether the device now pulls D+ up, a connect, or released it, a disconnect.
   * @param at Virtual time of the change.
   */
  virtual void pull_up_changed(bool on, vtime at) = 0;

  /**
   * @brief A wake-up the host asked for with device_port::wake_at() is due.
   * @param at The virtual time asked for.
   */
  virtual void on_time(vtime at) = 0;

  /**
   * @brief Appends the host state to a snapshot of the device model, which keeps it with its
   *        own state.
   * @param out Bytes, appended to.
   */
  virtual void save(std::vector<std::byte>& out) const = 0;

  /**
   * @brief Restores the host state from bytes save() wrote, when the device model is restored.
   *        The bytes are empty when the snapshot was taken with no host attached: the host then
   *        starts as if it had seen no device. After this call the device compares its pull-up,
   *        recomputed from the restored registers and this host, with the pull-up it had saved,
   *        which is the last one the host saw, and calls pull_up_changed() when they differ.
   *        The machine has restored its event queue by then, so the host may call
   *        device_port::wake_at() from that callback.
   * @param in Bytes of one save() call, possibly empty.
   */
  virtual void load(std::span<std::byte const> in) = 0;
};

/**
 * @brief A full-speed USB device as the bus sees it, implemented by a device controller model.
 *        Every call is one bus event at virtual time @p at, made from a model event or a test,
 *        never while the machine runs an instruction. Tokens carry the address and endpoint
 *        they target: the device answers `none` to another address, as real hardware ignores
 *        them. Reach it with `dynamic_cast<device_port*>(machine.model("OTG_FS"))`.
 */
class device_port
{
public:
  /** @brief Creates the interface. */
  device_port() = default;
  /** @brief Not copyable. */
  device_port(device_port const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(device_port const&) -> device_port& = delete;
  /** @brief Not movable. */
  device_port(device_port&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(device_port&&) -> device_port& = delete;

  /**
   * @brief Connects a host, which powers VBUS, or disconnects it. Not saved in snapshots.
   *        When the device already pulls D+ up, the host is told at once.
   * @param host Host, empty to unplug: VBUS drops and no callback is made any more.
   */
  virtual void attach(std::shared_ptr<host_side> host) = 0;

  /** @brief Whether the device pulls D+ up: a host is attached and the device connected. */
  [[nodiscard]] virtual auto pulled_up() const -> bool = 0;

  /**
   * @brief The host started a bus reset. The device goes back to address 0 and DATA0 on every
   *        endpoint at once. It reports the reset end, the OTG_FS ENUMDNE, 10 us after @p at:
   *        from then the firmware prepares EP0, so a host keeping SE0 for the 10 ms of
   *        USB 2.0 7.1.7.5 sends its first SETUP after the device is ready.
   * @param at Virtual time SE0 starts.
   */
  virtual void bus_reset(vtime at) = 0;

  /**
   * @brief Start of a full-speed frame, every 1 ms while the bus is not suspended.
   * @param frame Frame number, 11 bits.
   * @param at Virtual time of the SOF packet.
   */
  virtual void start_of_frame(std::uint16_t frame, vtime at) = 0;

  /**
   * @brief SETUP token and its DATA0 packet to endpoint 0. A device takes a SETUP even while it
   *        NAKs or STALLs the endpoint.
   * @param address Device address the token targets.
   * @param packet The 8 bytes.
   * @param at Virtual time of the transaction.
   * @return `ack`, or `none` when the token is not for this device.
   */
  virtual auto setup(std::uint8_t address, setup_packet packet, vtime at) -> handshake = 0;

  /**
   * @brief OUT token and its data packet. A packet with the wrong data toggle is acknowledged
   *        and dropped, USB 2.0 8.6.4.
   * @param address Device address the token targets.
   * @param endpoint Endpoint number, 0 to 15.
   * @param data Payload, at most the endpoint's maximum packet size.
   * @param pid Data PID the host sent.
   * @param at Virtual time of the transaction.
   * @return `ack` when taken, `nak` when the device cannot take it now, `stall`, or `none`.
   */
  virtual auto
      out(std::uint8_t                  address,
          std::uint8_t                  endpoint,
          std::span<std::uint8_t const> data,
          data_pid                      pid,
          vtime                         at) -> handshake = 0;

  /**
   * @brief IN token. The host acknowledges a packet it receives.
   * @param address Device address the token targets.
   * @param endpoint Endpoint number, 0 to 15.
   * @param at Virtual time of the transaction.
   * @return The packet with `ack`, or `nak`, `stall` or `none` and no data.
   */
  virtual auto in(std::uint8_t address, std::uint8_t endpoint, vtime at) -> in_result = 0;

  /**
   * @brief The bus has been idle for 3 ms: the device suspends.
   * @param at Virtual time the suspend is detected.
   */
  virtual void suspend(vtime at) = 0;

  /**
   * @brief The host resumed the bus.
   * @param at Virtual time of the resume.
   */
  virtual void resume(vtime at) = 0;

  /**
   * @brief Asks for host_side::on_time() at a virtual time, through an event of the device
   *        model, so the host runs on the device's clock and is saved with it. One wake-up is
   *        pending at a time: a new request replaces the previous one.
   * @param at Virtual time of the wake-up, now when it is in the past.
   */
  virtual void wake_at(vtime at) = 0;

protected:
  /** @brief Destroys the interface, only through its implementation. */
  ~device_port() = default;
};
}
