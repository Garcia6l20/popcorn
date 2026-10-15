#pragma once

#include <popcorn/core/link_guard.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>
#include <popcorn/periph/usb_host.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <vector>

namespace popcorn::periph::usb
{
/** @brief Factory name of the HID class driver, saved with each bound driver. */
inline constexpr std::string_view hid_name = "hid";

/** @brief Input reports received from a HID function kept before dropping the oldest. */
inline constexpr std::uint64_t hid_report_limit = std::uint64_t{1} << 16;

/** @brief Report types of GET_REPORT and SET_REPORT, the high byte of wValue, HID 1.11 7.2.1. */
enum class hid_report_type : std::uint8_t
{
  input = 1,
  output,
  feature,
};

/** @brief One input report a HID function sent on its interrupt IN pipe. */
struct hid_report
{
  /** @brief Its bytes as the transaction carried them, the report ID first when the device
   * numbers its reports. Never empty. */
  std::vector<std::uint8_t> data{};
  /** @brief Virtual time of the transaction that carried it. */
  vtime at{};
  /** @brief Virtual time the host queued it: the start of the frame of its transaction. */
  vtime ready{};
};

/** @brief An output report waiting for the device. */
struct hid_write
{
  /** @brief Handle its result carries, from 1, never reused within a channel. */
  std::uint64_t id{};
  /** @brief The report as hidraw takes it: the report ID first, 0 for a device that does not
   * number its reports, which is then not sent. */
  std::vector<std::uint8_t> data{};
};

/** @brief How a GET_REPORT or SET_REPORT queued for the binding ended. */
struct hid_reply
{
  /** @brief How it ended. */
  transfer_status status{};
  /** @brief Data stage received for GET_REPORT, empty for SET_REPORT. */
  std::vector<std::uint8_t> data{};
};

/**
 * @brief The queues of one HID function, outside the host state: they outlive the drivers a
 *        detach, a bus reset or a restore destroys. Touched by the driver in model events and by
 *        the binding, each under the hub's link lock.
 */
struct POPCORN_API hid_channel
{
  /** @brief Input reports received and not taken yet, oldest first. */
  std::deque<hid_report> in{};
  /** @brief Input reports kept at most. */
  std::uint64_t limit{hid_report_limit};
  /** @brief Input reports dropped because `in` held `limit`. */
  std::uint64_t dropped{};
  /** @brief Output reports the driver has not submitted yet, oldest first. Saved in the
   * driver's state, or with the factory while no driver is bound. */
  std::deque<hid_write> out{};
  /** @brief The output report being sent, 0 for none. */
  std::uint64_t in_flight{};
  /** @brief Handle of the next output report queued. */
  std::uint64_t next_write{1};
  /** @brief Output reports queued for the binding whose result is not in yet. */
  std::set<std::uint64_t> awaited_writes{};
  /** @brief Results of awaited output reports not collected yet. */
  std::map<std::uint64_t, transfer_status> written{};
  /** @brief The report descriptor the bound driver read, emptied when a driver binds, kept
   * after an unplug. Saved with the factory while no driver is bound. */
  std::vector<std::uint8_t> report_descriptor{};
  /** @brief Interface of the bound driver, empty while none is bound. */
  std::optional<std::uint8_t> interface{};
  /** @brief Whether the bound driver has read the report descriptor and submitted its first
   * interrupt IN transfer, after which output reports may go out. False while no driver is
   * bound. */
  bool ready{};
  /** @brief Control transfers submitted for the binding whose result is not in yet. */
  std::set<transfer_id> awaited{};
  /** @brief Results of awaited control transfers not collected yet. */
  std::map<transfer_id, hid_reply> results{};

  /**
   * @brief Keeps a received input report, dropping the oldest past `limit`.
   * @param data The report, ignored when empty.
   * @param at Its time.
   * @param ready The time the host queued it.
   */
  void received(std::span<std::uint8_t const> data, vtime at, vtime ready);

  /**
   * @brief Takes received input reports, up to the first queued after @p until.
   * @param count Most reports taken.
   * @param until Latest `ready` taken.
   * @return The reports, oldest first.
   */
  auto take(std::uint64_t count, vtime until = vtime{std::numeric_limits<std::uint64_t>::max()})
      -> std::vector<hid_report>;

  /**
   * @brief Queues an output report for the device.
   * @param data The report, its report ID first. A report whose bytes on the bus, the ID 0
   *        dropped, exceed 0xffff ends `error` unsent, with a warning.
   * @return Its handle.
   */
  auto write(std::vector<std::uint8_t> data) -> std::uint64_t;

  /** @brief Output reports not yet taken by the device, the one in progress included. */
  [[nodiscard]] auto queued() const noexcept -> std::uint64_t;

  /**
   * @brief Sets `limit`, dropping the oldest reports beyond it.
   * @param count Reports, at least 1.
   */
  void set_limit(std::uint64_t count);

  /**
   * @brief Forgets the timeline a restore abandons: received reports, the output reports, the
   *        drop count and every result. A driver load() refills the output side.
   */
  void discard();
};

/**
 * @brief The channels of every HID function of one host, by function index: the n-th HID
 *        interface in the configuration's interface order. Created empty with the host's
 *        factories, its channels created on first use, by the driver or by the binding.
 */
class POPCORN_API hid_hub
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
      -> hid_channel& pre(held.guards(mutex_));

  /**
   * @brief Appends the output reports and report descriptor of the channels no driver is
   *        bound to. A bound driver saves its own channel's.
   * @param out Bytes, appended to.
   */
  void save(std::vector<std::byte>& out) const;

  /**
   * @brief Empties the output reports and report descriptor of every channel, then restores
   *        those save() wrote. The host calls it before its drivers load theirs.
   * @param in Bytes of one save() call, or empty for none.
   */
  void load(std::span<std::byte const> in);

  /** @brief Calls hid_channel::discard() on every channel under the hub's lock. Call it before
   * the host loads a snapshot. */
  void discard();

private:
  mutable std::mutex                 mutex_;
  std::map<std::size_t, hid_channel> channels_{};
};

/**
 * @brief wDescriptorLength of the report descriptor an interface's HID descriptor announces,
 *        found as Linux usbhid looks for it: among the descriptors after the interface, else
 *        after its first endpoint.
 * @param interface An alternate setting.
 * @return Empty when the HID descriptor is missing or malformed, its first class descriptor is
 *         not a report descriptor, or the length is 0 or above 4096.
 */
[[nodiscard]] POPCORN_API auto hid_report_descriptor_length(interface_info const& interface)
    -> std::optional<std::uint16_t>;

/**
 * @brief The interface a HID function takes.
 * @param configuration The configuration.
 * @param interface An alternate setting 0.
 * @return @p interface alone when it is of class 0x03 with an interrupt IN endpoint and a
 *         report descriptor length, else empty.
 */
[[nodiscard]] POPCORN_API auto
    hid_claim(configuration_info const& configuration, interface_info const& interface)
        -> std::vector<std::uint8_t>;

/**
 * @brief Interfaces of the HID functions of a configuration, in interface order: the index of
 *        one is its function index.
 * @param configuration The configuration.
 */
[[nodiscard]] POPCORN_API auto hid_functions(configuration_info const& configuration)
    -> std::vector<std::uint8_t>;

/**
 * @brief The HID class driver, as Linux usbhid probes and runs a device. Per interface: SET_IDLE
 *        with duration 0 for every report, then GET_DESCRIPTOR of the report descriptor with
 *        the length its HID descriptor gives, tried up to 4 times while it ends short or fails.
 *        Once the descriptor is read, an interrupt IN transfer of one packet is always pending
 *        and output reports leave the channel one at a time: on the interrupt OUT pipe when the
 *        interface has one, else as SET_REPORT(Output). An interrupt IN STALL is cleared as
 *        usbhid does: CLEAR_FEATURE(ENDPOINT_HALT), the toggle back to DATA0, a new transfer.
 *        The factory saves the output reports of functions no driver is bound to with the host.
 * @param hub The channels, which the driver reaches by function index.
 * @return The factory, named hid_name.
 */
[[nodiscard]] POPCORN_API auto hid_factory(std::shared_ptr<hid_hub> hub) -> driver_factory;
}
