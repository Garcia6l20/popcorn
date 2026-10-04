#pragma once

#include <popcorn/core/types.hpp>
#include <popcorn/periph/usb_host.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <vector>

namespace popcorn::periph::usb
{
/** @brief Factory name of the CDC ACM class driver, saved with each bound driver. */
inline constexpr std::string_view cdc_acm_name = "cdc_acm";

/** @brief Bytes received from a CDC function kept before dropping the oldest, 16 MiB. */
inline constexpr std::uint64_t cdc_byte_limit = std::uint64_t{1} << 24;

/** @brief The line coding of SET_LINE_CODING, CDC PSTN 1.2 table 17. */
struct line_coding
{
  /** @brief dwDTERate, bits per second. */
  std::uint32_t baud{115'200};
  /** @brief bCharFormat: 0 one stop bit, 1 one and a half, 2 two. */
  std::uint8_t stop_bits{};
  /** @brief bParityType: 0 none, 1 odd, 2 even, 3 mark, 4 space. */
  std::uint8_t parity{};
  /** @brief bDataBits: 5, 6, 7, 8 or 16. */
  std::uint8_t data_bits{8};

  /** @brief Member-wise equality. */
  friend auto operator==(line_coding const&, line_coding const&) -> bool = default;
};

/** @brief What the CDC ACM driver sends when the device is configured, fixed for a host's life. */
struct cdc_options
{
  /** @brief DTR in the SET_CONTROL_LINE_STATE sent at configuration. */
  bool dtr{true};
  /** @brief RTS in the SET_CONTROL_LINE_STATE sent at configuration. */
  bool rts{true};
  /** @brief The SET_LINE_CODING sent at configuration, before the control line state. */
  line_coding coding{};
};

/** @brief One packet a CDC function sent on its bulk IN pipe. */
struct cdc_packet
{
  /** @brief Its bytes, never empty. */
  std::vector<std::uint8_t> data{};
  /** @brief Virtual time of the transaction that carried it. */
  vtime at{};
};

/**
 * @brief The queues of one CDC ACM function, outside the host state: they outlive the drivers a
 *        detach, a bus reset or a restore destroys. Touched by the driver in model events and by
 *        the binding between slices, never both at once, as a UART queue endpoint.
 */
struct cdc_channel
{
  /** @brief Packets received and not taken yet, oldest first. */
  std::deque<cdc_packet> in{};
  /** @brief Bytes of in.front() already taken. */
  std::size_t taken{};
  /** @brief Bytes in `in`, those taken excluded. */
  std::uint64_t in_bytes{};
  /** @brief Received bytes kept at most. */
  std::uint64_t limit{cdc_byte_limit};
  /** @brief Received bytes dropped because `in` held `limit`. */
  std::uint64_t dropped{};
  /** @brief Bytes for the device the driver has not submitted yet, oldest first. Saved in the
   * driver's state. */
  std::deque<std::uint8_t> out{};
  /** @brief Bytes of the OUT transfer in progress. */
  std::uint64_t in_flight{};
  /** @brief The last line coding the device acknowledged. */
  line_coding coding{};
  /** @brief The last control line state the device acknowledged: DTR bit 0, RTS bit 1. */
  std::uint8_t line_state{};
  /** @brief The UART state bitmap of the last SERIAL_STATE notification, PSTN 1.2 table 31. */
  std::uint16_t serial_state{};
  /** @brief Communication interface of the bound driver, empty while none is bound. */
  std::optional<std::uint8_t> comm_interface{};
  /** @brief Whether the bound driver's SET_CONTROL_LINE_STATE of configuration has ended,
   * whatever its status: OUT bytes go out from then on. False while no driver is bound. */
  bool ready{};
  /** @brief Control transfers submitted for the binding whose result is not in yet. */
  std::set<transfer_id> awaited{};
  /** @brief Results of awaited control transfers not collected yet. */
  std::map<transfer_id, transfer_status> results{};

  /**
   * @brief Keeps a received packet, dropping the oldest bytes past `limit`.
   * @param data The packet, ignored when empty.
   * @param at Its time.
   */
  void received(std::span<std::uint8_t const> data, vtime at);

  /**
   * @brief Takes received bytes, splitting a packet when @p bytes ends inside it.
   * @param bytes Most bytes taken.
   * @return The packets or parts taken, oldest first.
   */
  auto take(std::uint64_t bytes) -> std::vector<cdc_packet>;

  /**
   * @brief Takes whole received packets, the rest of a packet partly taken counting as one.
   * @param count Most packets taken.
   * @return The packets, oldest first.
   */
  auto take_packets(std::uint64_t count) -> std::vector<cdc_packet>;

  /** @brief Received packets not taken yet, a packet partly taken included. */
  [[nodiscard]] auto packets() const noexcept -> std::uint64_t;

  /**
   * @brief Sets `limit`, dropping the oldest bytes beyond it.
   * @param bytes Bytes, at least 1.
   */
  void set_limit(std::uint64_t bytes);

  /**
   * @brief Forgets the timeline a restore abandons: received bytes, the OUT bytes and transfer,
   *        the drop count and the control results. A driver load() refills the OUT side.
   */
  void discard();
};

/**
 * @brief The channels of every CDC ACM function of one host, by function index: the n-th ACM
 *        function in the configuration's interface order. Created empty with the host's
 *        factories, its channels created on first use, by the driver or by the binding, so a
 *        stream exists before the device is configured.
 */
class cdc_hub
{
public:
  /**
   * @brief The channel of a function, created empty on first use.
   * @param function Function index.
   */
  [[nodiscard]] auto channel(std::size_t function) -> cdc_channel&;

  /**
   * @brief Appends the OUT bytes of the channels no driver is bound to. A bound driver saves its
   *        own channel's.
   * @param out Bytes, appended to.
   */
  void save(std::vector<std::byte>& out) const;

  /**
   * @brief Empties the OUT bytes of every channel, then restores those save() wrote. The host
   *        calls it before its drivers load theirs.
   * @param in Bytes of one save() call, or empty for none.
   */
  void load(std::span<std::byte const> in);

  /** @brief Calls cdc_channel::discard() on every channel. Call it before the host loads a
   * snapshot. */
  void discard();

private:
  std::map<std::size_t, cdc_channel> channels_{};
};

/**
 * @brief Interfaces of a CDC ACM function, the communication interface first.
 * @param configuration The configuration.
 * @param interface An alternate setting 0.
 * @return Empty unless @p interface is a communication interface of subclass ACM, CDC 1.2
 *         table 4 and PSTN 1.2 table 2, whose function has a data interface: the interfaces of
 *         its association, or else the interfaces its union functional descriptor names.
 */
[[nodiscard]] auto
    cdc_acm_claim(configuration_info const& configuration, interface_info const& interface)
        -> std::vector<std::uint8_t>;

/**
 * @brief Communication interfaces of the CDC ACM functions of a configuration, in interface
 *        order: the index of one is its function index.
 * @param configuration The configuration.
 */
[[nodiscard]] auto cdc_acm_functions(configuration_info const& configuration)
    -> std::vector<std::uint8_t>;

/**
 * @brief The CDC ACM class driver. Per function: SET_LINE_CODING then SET_CONTROL_LINE_STATE at
 *        configuration, a bulk IN transfer of one packet always pending, OUT bytes taken from
 *        the channel once the control line state is answered and sent in transfers ending with
 *        a zero-length packet after a full one, the notification pipe polled for SERIAL_STATE.
 *        A bulk IN STALL is cleared as Linux cdc-acm does: CLEAR_FEATURE(ENDPOINT_HALT), the
 *        toggle back to DATA0, a new transfer. A bulk OUT STALL stops writing. It sends no other
 *        control request: DTR changes only on a request through the binding. The factory saves
 *        the OUT bytes of functions no driver is bound to with the host.
 * @param hub The channels, which the driver reaches by function index.
 * @param options What the driver sends at configuration.
 * @return The factory, named cdc_acm_name.
 */
[[nodiscard]] auto cdc_acm_factory(std::shared_ptr<cdc_hub> hub, cdc_options options)
    -> driver_factory;
}
