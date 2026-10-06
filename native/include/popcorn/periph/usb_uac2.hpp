#pragma once

#include <popcorn/core/link_guard.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/periph/usb_host.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
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
/** @brief Factory name of the USB Audio 2.0 class driver, saved with each bound driver. */
inline constexpr std::string_view uac2_name = "uac2";

/** @brief Frames received from a microphone stream kept before dropping the oldest, 65 s at one
 * frame per ms. */
inline constexpr std::uint64_t uac2_frame_limit = std::uint64_t{1} << 16;

/** @brief Direction of an audio streaming interface. */
enum class uac2_direction : std::uint8_t
{
  /** @brief Host to device, an isochronous OUT endpoint. */
  speaker,
  /** @brief Device to host, an isochronous IN endpoint. */
  mic,
};

/** @brief Feature unit controls the driver reads and writes, UAC2 table A-23. */
enum class uac2_control : std::uint8_t
{
  /** @brief FU_MUTE_CONTROL, a 1-byte boolean. */
  mute = 1,
  /** @brief FU_VOLUME_CONTROL, a signed 2-byte value in 1/256 dB. */
  volume = 2,
};

/** @brief The PCM format of an alternate setting: AS_GENERAL and its Type I format descriptor. */
struct uac2_format
{
  /** @brief bNrChannels. */
  std::uint8_t channels{};
  /** @brief bSubslotSize, bytes per sample on the bus, 1 to 4. */
  std::uint8_t subslot{};
  /** @brief bBitResolution. */
  std::uint8_t bits{};

  /** @brief Member-wise equality. */
  friend auto operator==(uac2_format const&, uac2_format const&) -> bool = default;
};

/** @brief A GET_RANGE answer of one subrange: minimum, maximum and resolution. */
struct uac2_range
{
  /** @brief wMIN. */
  std::int32_t min{};
  /** @brief wMAX. */
  std::int32_t max{};
  /** @brief wRES. */
  std::int32_t res{};

  /** @brief Member-wise equality. */
  friend auto operator==(uac2_range const&, uac2_range const&) -> bool = default;
};

/** @brief One isochronous IN packet a microphone stream received, decoded. */
struct uac2_frame
{
  /** @brief Samples, interleaved, each the signed subslot as the bus carried it. Empty for a
   * frame whose packet was empty or missing. */
  std::vector<std::int32_t> samples{};
  /** @brief Virtual time of its transaction, whether the device answered or not. */
  vtime at{};
};

/** @brief A feature unit and what the driver last read from it or wrote to it. */
struct uac2_feature_unit
{
  /** @brief bUnitID. */
  std::uint8_t id{};
  /** @brief Logical channels, the master channel 0 excluded. */
  std::uint8_t channels{};
  /** @brief bmaControls of the master channel then each logical channel. */
  std::vector<std::uint32_t> controls{};
  /** @brief Mute per channel, master first, empty where not read. */
  std::vector<std::optional<bool>> mute{};
  /** @brief Volume per channel in 1/256 dB, master first, empty where not read. */
  std::vector<std::optional<std::int16_t>> volume{};
  /** @brief Volume range per channel, master first, empty where not read. */
  std::vector<std::optional<uac2_range>> volume_range{};
};

/** @brief One streaming direction of an audio function, as the driver found and runs it. */
struct uac2_stream
{
  /** @brief Interface number of the audio streaming interface, empty before a driver bound or
   * when the function has none in this direction. It and the other fields the driver found keep
   * their last values after an unbind, until a driver binds again or a restore. */
  std::optional<std::uint8_t> interface{};
  /** @brief Alternate setting start() selects, the first one the driver can stream. */
  std::uint8_t alternate{};
  /** @brief bEndpointAddress of its isochronous data endpoint. */
  std::uint8_t endpoint{};
  /** @brief wMaxPacketSize of that endpoint. */
  std::uint16_t max_packet{};
  /** @brief Its PCM format. */
  uac2_format format{};
  /** @brief bClockID of the clock source the terminal links to. */
  std::uint8_t clock{};
  /** @brief Sample rates of the clock's GET_RANGE, in Hz, discrete. */
  std::vector<std::uint32_t> rates{};
  /** @brief The rate it runs at: the clock's GET_CUR, else the highest of `rates`. */
  std::uint32_t rate{};
  /** @brief Whether the alternate setting is selected and its pipe open: an isochronous
   * packet goes every frame. */
  bool running{};
  /** @brief Speaker: samples written and not yet put in a packet, interleaved. Saved in the
   * driver's state, or with the factory while no driver is bound. */
  std::deque<std::int32_t> out{};
  /** @brief Speaker: a packet carrying written samples is submitted and the host has not
   * sent it yet. */
  bool sending{};
  /** @brief Speaker: time of the last packet that carried written samples. Saved in the driver's
   * state. */
  std::optional<vtime> sent_at{};
  /** @brief Speaker: stops that dropped written samples, queued or in a packet not sent, and
   * unbinds that dropped such a packet, so a drain in progress can tell them from a drained
   * queue. */
  std::uint64_t flushes{};
  /** @brief Mic: frames received and not taken yet, oldest first. */
  std::deque<uac2_frame> in{};
  /** @brief Mic: frames kept at most. */
  std::uint64_t limit{uac2_frame_limit};
  /** @brief Mic: frames dropped because `in` held `limit`. */
  std::uint64_t dropped{};

  /**
   * @brief Keeps a received frame, dropping the oldest past `limit`.
   * @param frame The frame.
   */
  void received(uac2_frame frame);

  /**
   * @brief Takes received frames.
   * @param count Most frames taken.
   * @return The frames, oldest first.
   */
  auto take(std::uint64_t count) -> std::vector<uac2_frame>;

  /**
   * @brief Sets `limit`, dropping the oldest frames beyond it.
   * @param count Frames, at least 1.
   */
  void set_limit(std::uint64_t count);
};

/**
 * @brief The queues and the state seen by Python of one audio function, outside the host
 *        state: they outlive the drivers a detach, a bus reset or a restore destroys. Touched by
 *        the driver in model events and by the binding, each under the hub's link lock.
 */
struct uac2_channel
{
  /** @brief Audio control interface of the bound driver, empty while none is bound. */
  std::optional<std::uint8_t> interface{};
  /** @brief Whether the bound driver has finished its probe requests. */
  bool ready{};
  /** @brief The speaker stream. */
  uac2_stream speaker{};
  /** @brief The microphone stream. */
  uac2_stream mic{};
  /** @brief Feature units, in the order the probe reaches them. */
  std::vector<uac2_feature_unit> units{};
  /** @brief Handle of the next operation queued for the binding. */
  std::uint64_t next_operation{1};
  /** @brief Operations queued for the binding whose result is not in yet. */
  std::set<std::uint64_t> awaited{};
  /** @brief Results of awaited operations not collected yet. */
  std::map<std::uint64_t, transfer_status> results{};

  /**
   * @brief One of the two streams.
   * @param direction Which.
   */
  [[nodiscard]] auto stream(uac2_direction direction) -> uac2_stream&;

  /**
   * @brief Settles an awaited operation, if it is awaited.
   * @param operation Its handle.
   * @param status How it ended.
   */
  void settle(std::uint64_t operation, transfer_status status);

  /**
   * @brief Forgets the timeline a restore abandons: received frames, written samples, drop
   *        counts, results and what the driver found. A driver load() refills it.
   */
  void discard();
};

/**
 * @brief The channels of every audio function of one host, by function index: the n-th audio
 *        control interface in the configuration's interface order. Created empty with the
 *        host's factories, its channels created on first use, by the driver or by the binding.
 */
class uac2_hub
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
      -> uac2_channel& pre(held.guards(mutex_));

  /**
   * @brief Appends the speaker samples of the channels no driver is bound to. A bound driver
   *        saves its own channel's.
   * @param out Bytes, appended to.
   */
  void save(std::vector<std::byte>& out) const;

  /**
   * @brief Empties the speaker samples of every channel, then restores those save() wrote. The
   *        host calls it before its drivers load theirs.
   * @param in Bytes of one save() call, or empty for none.
   */
  void load(std::span<std::byte const> in);

  /** @brief Calls uac2_channel::discard() on every channel under the hub's lock. Call it before
   * the host loads a snapshot. */
  void discard();

private:
  mutable std::mutex                  mutex_;
  std::map<std::size_t, uac2_channel> channels_{};
};

/**
 * @brief The interfaces an audio function takes.
 * @param configuration The configuration.
 * @param interface An alternate setting 0.
 * @return For an audio control interface of protocol 0x20 covered by an association, the
 *         interfaces of that association, the control interface first, else empty, as Linux
 *         refuses a UAC2 function without one.
 */
[[nodiscard]] auto
    uac2_claim(configuration_info const& configuration, interface_info const& interface)
        -> std::vector<std::uint8_t>;

/**
 * @brief Audio control interfaces of the audio functions of a configuration, in interface order:
 *        the index of one is its function index.
 * @param configuration The configuration.
 */
[[nodiscard]] auto uac2_functions(configuration_info const& configuration)
    -> std::vector<std::uint8_t>;

/**
 * @brief Starts a stream through the driver bound to a function, from outside host callbacks:
 *        the clock requests Linux sends when it prepares a stream, then SET_INTERFACE to the
 *        stream's alternate setting, then an isochronous packet every frame.
 * @param owner The host.
 * @param hub The channels of the host's audio functions.
 * @param function Function index.
 * @param direction The stream.
 * @return The operation's handle, settled in the channel's results once SET_INTERFACE ended, or
 *         0 when no driver is bound, the function has no such stream, or the stream runs.
 */
[[nodiscard]] auto
    uac2_start(host& owner, uac2_hub& hub, std::size_t function, uac2_direction direction)
        -> std::uint64_t;

/**
 * @brief Stops a stream at once: its pipe closes, no isochronous packet goes from now, the
 *        speaker's samples are dropped, and SET_INTERFACE to alternate setting 0 is queued in
 *        the host state, after any control request already queued. A start in progress is
 *        settled `cancelled`. A stream neither running nor starting sends nothing, but the
 *        speaker's samples are dropped all the same, bound or not.
 * @param owner The host.
 * @param hub The channels of the host's audio functions.
 * @param function Function index.
 * @param direction The stream.
 */
void uac2_stop(host& owner, uac2_hub& hub, std::size_t function, uac2_direction direction);

/**
 * @brief Queues SET_CUR of a feature unit control through the driver bound to a function.
 * @param owner The host.
 * @param hub The channels of the host's audio functions.
 * @param function Function index.
 * @param unit bUnitID.
 * @param channel 0 for the master channel, else the logical channel.
 * @param control The control.
 * @param value The value: 0 or 1 for mute, 1/256 dB for volume.
 * @return The operation's handle, or 0 when no driver is bound or the unit has no such
 *         channel.
 */
[[nodiscard]] auto uac2_set_feature(
    host&        owner,
    uac2_hub&    hub,
    std::size_t  function,
    std::uint8_t unit,
    std::uint8_t channel,
    uac2_control control,
    std::int32_t value) -> std::uint64_t;

/**
 * @brief The USB Audio 2.0 class driver, as Linux snd-usb-audio probes and streams a device.
 *        At configuration, for each audio streaming interface and each of its alternate
 *        settings with an isochronous endpoint and a Type I PCM format: GET_RANGE of the
 *        sampling frequency, head then whole, SET_INTERFACE 0, the clock's validity, GET_CUR of
 *        the frequency, SET_CUR to the highest rate when the control is writable and differs,
 *        SET_INTERFACE to the setting, and SET_INTERFACE 0 after the last setting. Then for
 *        each feature unit reached from an output terminal, sources first: GET_CUR of mute per
 *        channel then master, GET_RANGE of volume three times, the resolution check, GET_CUR of
 *        volume. A stream runs once started: each frame one isochronous OUT packet of the
 *        rate's samples, silence past the written ones, or one IN transfer of wMaxPacketSize.
 *        The factory saves the speaker samples of functions no driver is bound to with the host.
 *
 * Not modelled from Linux: an alternate setting whose endpoint has a bInterval above 1 is not
 * streamed, a clock selector is followed statically through its first pin, a multiplier
 * through its source, with no GET_CUR or SET_CUR on them. Only mute and volume of feature units
 * are probed: other feature unit controls, mixer, selector, processing and extension units and
 * terminal connector controls send nothing, and an effect unit ends the walk as in Linux. The
 * early exit of Linux's mixer on an unrecoverable dB range is not mirrored.
 * @param hub The channels, which the driver reaches by function index.
 * @return The factory, named uac2_name.
 */
[[nodiscard]] auto uac2_factory(std::shared_ptr<uac2_hub> hub) -> driver_factory;
}
