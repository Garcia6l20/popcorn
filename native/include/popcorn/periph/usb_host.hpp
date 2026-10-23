#pragma once

#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>
#include <popcorn/periph/usb.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace popcorn::periph::usb
{
/** @brief Transfer type of an endpoint, bits 1:0 of its bmAttributes, USB 2.0 table 9-13. */
enum class transfer_type : std::uint8_t
{
  control,
  isochronous,
  bulk,
  interrupt,
};

/**
 * @brief Device states of USB 2.0 9.1.1 as the host sees them. `detached` is no pull-up seen,
 *        `powered` a pull-up seen before the first bus reset ends, `default_` after it, at
 *        address 0.
 */
enum class device_state : std::uint8_t
{
  detached,
  powered,
  default_,
  address,
  configured,
};

/** @brief How a transfer ended. */
enum class transfer_status : std::uint8_t
{
  /** @brief Done: the requested length, a short packet, or the status stage acknowledged. */
  completed,
  /** @brief The device answered STALL. */
  stalled,
  /** @brief A control transfer not done within host_options::request_timeout. */
  timed_out,
  /**
   * @brief No handshake three times in a row, or an isochronous packet not answered or not sent
   *        because it would have ended past the frame's EOPF point.
   */
  error,
  /** @brief An IN packet longer than the space left in the transfer, the data clipped. */
  overflow,
  /** @brief Dropped by a detach before it ended. */
  cancelled,
};

/** @brief A device descriptor, USB 2.0 table 9-8. */
struct device_descriptor
{
  /** @brief bcdUSB. */
  std::uint16_t usb_version{};
  /** @brief bDeviceClass. */
  std::uint8_t class_code{};
  /** @brief bDeviceSubClass. */
  std::uint8_t subclass{};
  /** @brief bDeviceProtocol. */
  std::uint8_t protocol{};
  /** @brief bMaxPacketSize0. */
  std::uint8_t max_packet0{};
  /** @brief idVendor. */
  std::uint16_t vendor{};
  /** @brief idProduct. */
  std::uint16_t product{};
  /** @brief bcdDevice. */
  std::uint16_t device_version{};
  /** @brief iManufacturer. */
  std::uint8_t manufacturer_string{};
  /** @brief iProduct. */
  std::uint8_t product_string{};
  /** @brief iSerialNumber. */
  std::uint8_t serial_string{};
  /** @brief bNumConfigurations. */
  std::uint8_t configurations{};
};

/** @brief An endpoint descriptor, USB 2.0 table 9-13, with the descriptors that follow it. */
struct endpoint_info
{
  /** @brief bEndpointAddress: number in bits 3:0, IN when bit 7 is set. */
  std::uint8_t address{};
  /** @brief Transfer type from bmAttributes. */
  transfer_type type{};
  /** @brief bmAttributes, synchronization and usage bits included. */
  std::uint8_t attributes{};
  /** @brief wMaxPacketSize, bits 10:0. */
  std::uint16_t max_packet{};
  /** @brief bInterval. */
  std::uint8_t interval{};
  /** @brief Raw class-specific descriptors after this one, before the next endpoint or
   * interface. */
  std::vector<std::uint8_t> extra{};

  /** @brief Whether the endpoint is IN, device to host. */
  [[nodiscard]] constexpr auto in() const noexcept -> bool
  {
    return (address & 0x80u) != 0;
  }

  /** @brief Endpoint number, 0 to 15. */
  [[nodiscard]] constexpr auto number() const noexcept -> std::uint8_t
  {
    return address & 0x0fu;
  }
};

/** @brief One alternate setting of an interface, USB 2.0 table 9-12. */
struct interface_info
{
  /** @brief bInterfaceNumber. */
  std::uint8_t number{};
  /** @brief bAlternateSetting. */
  std::uint8_t alternate{};
  /** @brief bInterfaceClass. */
  std::uint8_t class_code{};
  /** @brief bInterfaceSubClass. */
  std::uint8_t subclass{};
  /** @brief bInterfaceProtocol. */
  std::uint8_t protocol{};
  /** @brief iInterface. */
  std::uint8_t string_index{};
  /** @brief Endpoints of this setting, in descriptor order. */
  std::vector<endpoint_info> endpoints{};
  /** @brief Raw class-specific descriptors between this interface and its first endpoint. */
  std::vector<std::uint8_t> extra{};
};

/** @brief An interface association descriptor, USB 2.0 ECN IAD. */
struct association_info
{
  /** @brief bFirstInterface. */
  std::uint8_t first_interface{};
  /** @brief bInterfaceCount. */
  std::uint8_t count{};
  /** @brief bFunctionClass. */
  std::uint8_t class_code{};
  /** @brief bFunctionSubClass. */
  std::uint8_t subclass{};
  /** @brief bFunctionProtocol. */
  std::uint8_t protocol{};
  /** @brief iFunction. */
  std::uint8_t string_index{};
};

/** @brief A configuration descriptor and everything wTotalLength covers, parsed. */
struct POPCORN_API configuration_info
{
  /** @brief bConfigurationValue. */
  std::uint8_t value{};
  /** @brief bmAttributes. */
  std::uint8_t attributes{};
  /** @brief bMaxPower, in 2 mA units. */
  std::uint8_t max_power{};
  /** @brief iConfiguration. */
  std::uint8_t string_index{};
  /** @brief bNumInterfaces. */
  std::uint8_t interface_count{};
  /** @brief Every interface and alternate setting, in descriptor order. */
  std::vector<interface_info> interfaces{};
  /** @brief Interface associations, in descriptor order. */
  std::vector<association_info> associations{};
  /** @brief Raw descriptors before the first interface that are not associations. */
  std::vector<std::uint8_t> extra{};

  /**
   * @brief Finds an alternate setting.
   * @param number bInterfaceNumber.
   * @param alternate bAlternateSetting.
   * @return The setting, or null when the configuration has none.
   */
  [[nodiscard]] auto find(std::uint8_t number, std::uint8_t alternate = 0) const
      -> interface_info const*;

  /**
   * @brief Interfaces of the function an interface belongs to.
   * @param number bInterfaceNumber.
   * @return The interfaces an association names when one covers @p number, else @p number
   *         alone.
   */
  [[nodiscard]] auto function_of(std::uint8_t number) const -> std::vector<std::uint8_t>;
};

/**
 * @brief Parses a device descriptor.
 * @param bytes The descriptor, 18 bytes or more.
 * @return The descriptor, or nothing when @p bytes is shorter or not a device descriptor.
 */
[[nodiscard]] POPCORN_API auto parse_device_descriptor(std::span<std::uint8_t const> bytes)
    -> std::optional<device_descriptor>;

/**
 * @brief Parses a configuration descriptor with its interfaces, associations and endpoints.
 *        Descriptors the parser does not know are kept raw in the `extra` of the entity they
 *        follow.
 * @param bytes The wTotalLength bytes.
 * @return The configuration, or why it is malformed.
 */
[[nodiscard]] POPCORN_API auto parse_configuration(std::span<std::uint8_t const> bytes)
    -> std::expected<configuration_info, std::string>;

/** @brief Pipe handle, unique for the life of a host, never reused. */
using pipe_id = std::uint32_t;

/** @brief Transfer handle, unique for the life of a host, never reused. */
using transfer_id = std::uint32_t;

/** @brief What a class driver submits on a pipe. */
struct transfer_request
{
  /** @brief OUT data. Empty for an IN transfer or a zero-length OUT. */
  std::vector<std::uint8_t> data{};
  /** @brief Bytes to receive for an IN transfer, ignored for OUT. */
  std::uint32_t length{};
  /** @brief OUT only: end a transfer whose length is a non-zero multiple of the maximum packet
   * size with a zero-length packet, the URB_ZERO_PACKET rule of Linux. */
  bool zero_packet{};
};

/** @brief A finished transfer. */
struct transfer_result
{
  /** @brief Handle returned at submission. */
  transfer_id id{};
  /** @brief How it ended. */
  transfer_status status{};
  /** @brief The SETUP packet, for a control transfer. */
  setup_packet setup{};
  /** @brief Data received for IN, data sent for OUT. */
  std::vector<std::uint8_t> data{};
  /** @brief Virtual time of its last transaction, or of the event that ended it. */
  vtime at{};
  /** @brief Virtual time of the host event that ended it: the start of its frame for a
   * periodic transfer, which runs every transaction of the frame at that event, else `at`. */
  vtime ready{};
  /** @brief Frame number it ended in. */
  std::uint16_t frame{};
};

/** @brief Kind of a request log entry. */
enum class log_kind : std::uint8_t
{
  /** @brief The host started a bus reset. */
  bus_reset,
  /** @brief The host sent the SETUP of a control transfer. */
  request,
};

/** @brief One entry of the host's request log. */
struct log_entry
{
  /** @brief Kind. */
  log_kind kind{};
  /** @brief Virtual time of the reset or of the SETUP. */
  vtime at{};
  /** @brief Device address the SETUP went to. */
  std::uint8_t address{};
  /** @brief The SETUP packet. */
  setup_packet setup{};
  /** @brief Transfer handle of the request. */
  transfer_id id{};
  /** @brief How the request ended, nothing while it runs. */
  std::optional<transfer_status> status{};
};

/** @brief A string descriptor read during enumeration, raw. */
struct string_descriptor
{
  /** @brief String index. */
  std::uint8_t index{};
  /** @brief The descriptor bytes, bLength and bDescriptorType included. */
  std::vector<std::byte> bytes{};

  /** @brief Member-wise equality. */
  friend auto operator==(string_descriptor const&, string_descriptor const&) -> bool = default;
};

class host;
class host_context;

/**
 * @brief A class driver bound to the interfaces of one function after SET_CONFIGURATION. It
 *        runs inside host callbacks, on virtual time, and never runs the machine. Its state
 *        that affects the bus, an OUT byte queue for instance, goes through save() and load().
 *        Results for Python, IN bytes for instance, stay outside them.
 */
class POPCORN_API class_driver
{
public:
  /** @brief Creates the driver. */
  class_driver() = default;
  /** @brief Not copyable. */
  class_driver(class_driver const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(class_driver const&) -> class_driver& = delete;
  /** @brief Not movable. */
  class_driver(class_driver&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(class_driver&&) -> class_driver& = delete;
  /**
   * @brief Destroys the driver. This is the unbind signal, there is no other callback: the host
   *        destroys a driver when the device detaches, at a bus reset, when SET_CONFIGURATION
   *        binds the drivers again, at a load() whose state does not name it with the same
   *        interfaces, and with the host. The host is changing its state then: the destructor
   *        must not reach it, through a host_context or otherwise. Its pipes and transfers are
   *        gone with it, without callbacks, its control transfers queued at a bus reset
   *        included. A driver is always destroyed before the driver that replaces it is built:
   *        at a load(), every driver not kept dies before the first factory make().
   *
   * State that must survive a re-bind or a restore, the queues Python reads and fills for
   * instance, lives outside the driver, in an object that outlives the host, such as one its
   * factory captures.
   */
  virtual ~class_driver();

  /**
   * @brief The device is configured: open pipes, send class requests.
   * @param ctx The driver's access to the host.
   */
  virtual void configured(host_context& ctx) = 0;

  /**
   * @brief A control transfer the driver submitted ended, a STALL included.
   * @param ctx The driver's access to the host.
   * @param result The transfer.
   */
  virtual void control_done(host_context& ctx, transfer_result const& result);

  /**
   * @brief A transfer on one of the driver's pipes ended.
   * @param ctx The driver's access to the host.
   * @param pipe The pipe.
   * @param result The transfer.
   */
  virtual void transfer_done(host_context& ctx, pipe_id pipe, transfer_result const& result);

  /**
   * @brief Start of a frame, after the SOF and before its periodic transactions: the place to
   *        submit this frame's isochronous packets.
   * @param ctx The driver's access to the host.
   * @param frame Frame number, 11 bits.
   */
  virtual void frame(host_context& ctx, std::uint16_t frame);

  /**
   * @brief Appends the driver state to the host state.
   * @param out Bytes, appended to.
   */
  virtual void save(std::vector<std::byte>& out) const;

  /**
   * @brief Restores the driver state from bytes save() wrote. The host calls it on a driver
   *        that may have run since, or on a new one.
   * @param in Bytes of one save() call.
   */
  virtual void load(std::span<std::byte const> in);
};

/** @brief How a host recognizes and builds one kind of class driver. */
struct driver_factory
{
  /** @brief Name, saved with the driver to rebuild it at load. Unique among the factories. */
  std::string name{};
  /**
   * @brief Interfaces the driver takes, given the configuration and the alternate setting 0 of
   *        an interface not taken yet. Empty when it does not take that interface. A claim that
   *        names an interface the configuration lacks, or one already taken, binds nothing: the
   *        host warns and tries the next factory.
   */
  std::copyable_function<
      std::vector<std::uint8_t>(configuration_info const&, interface_info const&)>
      claim{};
  /** @brief Builds a driver. */
  std::copyable_function<std::unique_ptr<class_driver>()> make{};
  /**
   * @brief Appends the state of this driver kind that lives outside its drivers and drives the
   *        bus once a driver binds, bytes queued for a function not bound yet for instance. Saved
   *        with the host, whatever the device state. Optional.
   */
  std::copyable_function<void(std::vector<std::byte>&) const> save{};
  /**
   * @brief Restores what save() wrote, or a state with nothing pending from empty bytes. A
   *        host load() calls it before it keeps, rebuilds or loads any driver. Optional.
   */
  std::copyable_function<void(std::span<std::byte const>)> load{};
};

/** @brief One step of the enumeration sequence. */
enum class enumeration_action : std::uint8_t
{
  /** @brief GET_DESCRIPTOR device with wLength 64, learns bMaxPacketSize0. */
  get_device_head,
  /** @brief A bus reset of host_options::reset_time. */
  bus_reset,
  /** @brief SET_ADDRESS host_options::address. */
  set_address,
  /** @brief Waits enumeration_step::wait, frames running. */
  wait,
  /** @brief GET_DESCRIPTOR device with wLength 18. */
  get_device,
  /** @brief GET_DESCRIPTOR configuration 0 with wLength 9. */
  get_configuration_head,
  /** @brief GET_DESCRIPTOR configuration 0 with its wTotalLength. */
  get_configuration,
  /** @brief GET_DESCRIPTOR string 0, the language IDs. A STALL skips every string. */
  get_languages,
  /** @brief GET_DESCRIPTOR string for each index the descriptors name, in the first language. A
   *  STALL skips that string. */
  get_strings,
  /** @brief SET_CONFIGURATION with bConfigurationValue of configuration 0, then binds the class
   *  drivers. */
  set_configuration,
};

/** @brief One step of the enumeration sequence. */
struct enumeration_step
{
  /** @brief What to do. */
  enumeration_action action{};
  /** @brief How long a `wait` step waits. */
  std::chrono::nanoseconds wait{};
};

/**
 * @brief The Linux sequence: device descriptor head, second reset, SET_ADDRESS, 10 ms of
 *        SET_ADDRESS recovery as `hub_port_init` waits, device descriptor, configuration head
 *        then whole, languages, strings, SET_CONFIGURATION.
 */
[[nodiscard]] POPCORN_API auto default_enumeration() -> std::vector<enumeration_step>;

/** @brief Behaviour of a host, fixed for its life and not saved. */
struct host_options
{
  /** @brief The enumeration sequence, run after the attach reset. */
  std::vector<enumeration_step> enumeration = default_enumeration();
  /** @brief Address SET_ADDRESS gives. */
  std::uint8_t address{1};
  /** @brief Connect debounce before the attach reset, USB 2.0 7.1.7.3. */
  std::chrono::nanoseconds debounce{std::chrono::milliseconds{100}};
  /** @brief SE0 time of a bus reset, USB 2.0 7.1.7.5. */
  std::chrono::nanoseconds reset_time{std::chrono::milliseconds{10}};
  /** @brief A control transfer not done this long after its SETUP ends `timed_out`. */
  std::chrono::nanoseconds request_timeout{std::chrono::seconds{5}};
  /** @brief Share of the frame, in percent, periodic transactions must end within: the device's
   *  EOPF point, DCFG.PFIVL on a DWC2. */
  std::uint32_t periodic_percent{80};
  /** @brief Class driver kinds, tried in order on each interface. */
  std::vector<driver_factory> drivers{};
  /** @brief Receives the host's warnings: a request timed out, a malformed descriptor. */
  std::copyable_function<void(std::string const&) const> warn{};
};

/** @brief A class driver bound by the host. */
struct bound_driver
{
  /** @brief Name of its factory. */
  std::string name{};
  /** @brief Interfaces it took, as its factory's claim returned them. */
  std::vector<std::uint8_t> interfaces{};
  /** @brief The driver. */
  std::unique_ptr<class_driver> driver{};
};

/**
 * @brief A class driver's access to the host. Valid while the host lives and the driver stays
 *        bound, also outside host callbacks, for instance from a binding with the machine
 *        stopped: what it submits then goes on the bus from the next frame.
 */
class POPCORN_API host_context
{
public:
  /**
   * @brief Access for one bound driver.
   * @param owner The host.
   * @param driver Index of the driver in host::drivers().
   */
  host_context(host& owner, std::size_t driver) noexcept;

  /** @brief Interfaces the driver took. */
  [[nodiscard]] auto interfaces() const -> std::span<std::uint8_t const>;

  /** @brief The configuration the device runs. */
  [[nodiscard]] auto configuration() const -> configuration_info const&;

  /**
   * @brief Opens a pipe to an endpoint, DATA0, after the pipes already open. An interrupt pipe
   *        is first served in the next frame. An isochronous pipe is first served two frames
   *        after the current one, the earliest start xHCI 1.2 4.11.2.5 allows and the lower
   *        bound of Linux's xHCI start frame for an isochronous URB.
   * @param endpoint The endpoint, from configuration().
   * @return The pipe.
   */
  auto open_pipe(endpoint_info const& endpoint) -> pipe_id;

  /**
   * @brief Closes a pipe at once: no transaction on it from now, its queued transfers dropped
   *        without callbacks. Unknown pipes are ignored. A pipe of another driver stays open,
   *        with a warning.
   * @param pipe The pipe.
   */
  void close_pipe(pipe_id pipe);

  /**
   * @brief Queues a transfer on a pipe. Bulk and interrupt transfers end on a short packet or
   *        the requested length. An isochronous transfer is one packet in one frame, no retry.
   * @param pipe The pipe.
   * @param request The transfer.
   * @return The transfer, reported through class_driver::transfer_done(), or 0 when the pipe
   *         is not open or belongs to another driver: the transfer is then dropped with a
   *         warning.
   */
  auto submit(pipe_id pipe, transfer_request request) -> transfer_id;

  /**
   * @brief Queues a control transfer on the default pipe, after those already queued.
   * @param setup The SETUP packet.
   * @param data OUT data stage, wLength bytes. Empty for IN or no data.
   * @return The transfer, reported through class_driver::control_done().
   */
  auto control(setup_packet setup, std::vector<std::uint8_t> data = {}) -> transfer_id;

  /**
   * @brief Sets the data toggle of a pipe back to DATA0, as the device does when it takes a
   *        CLEAR_FEATURE(ENDPOINT_HALT), USB 2.0 9.4.5. Call it when that request completes. The
   *        default pipe, whose toggles follow its stages, an unknown pipe and a pipe of another
   *        driver are left as they are, with a warning.
   * @param pipe The pipe.
   */
  void reset_toggle(pipe_id pipe);

  /** @brief Transfers queued on a pipe, the one in progress included. */
  [[nodiscard]] auto queued(pipe_id pipe) const -> std::size_t;

  /** @brief Virtual time of the transaction or event being handled, or of the last one. */
  [[nodiscard]] auto now() const -> vtime;

  /** @brief Number of the current frame. */
  [[nodiscard]] auto frame() const -> std::uint16_t;

  /** @brief Passes a warning to host_options::warn. */
  void warn(std::string const& message) const;

private:
  host*       host_;
  std::size_t driver_;
};

/**
 * @brief Holds back the warnings every USB host raises on the current thread while it lives,
 *        and passes them to host_options::warn in order when it dies. Declared before a hub's
 *        core::link_guard, it moves the warnings of a driver event out of the hub lock: a
 *        warning sink may take the GIL, which no thread takes under a link lock. A nested one
 *        hands its warnings to the one it nests in.
 */
class POPCORN_API deferred_warnings
{
public:
  /** @brief Starts holding the warnings of the current thread. */
  deferred_warnings() noexcept;
  /** @brief Not copyable. */
  deferred_warnings(deferred_warnings const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(deferred_warnings const&) -> deferred_warnings& = delete;
  /** @brief Not movable. */
  deferred_warnings(deferred_warnings&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(deferred_warnings&&) -> deferred_warnings& = delete;
  /**
   * @brief Passes the held warnings on, to the enclosing deferred_warnings if any.
   * @throws Whatever a sink throws, unless an exception is already unwinding the stack: the
   *         warnings after it are dropped then, as a sink throwing in place would drop them.
   */
  ~deferred_warnings() noexcept(false);

  /**
   * @brief Holds a warning when a deferred_warnings lives on the current thread.
   * @param sink Where it goes, host_options::warn. Must outlive this deferred_warnings: it is
   *        referred to, not copied, so no callable state is copied under a link lock.
   * @param message The warning.
   * @return Whether it was held. When not, the caller passes it on itself.
   */
  static auto hold(
      std::copyable_function<void(std::string const&) const> const& sink,
      std::string const&                                            message) -> bool;

private:
  struct held_warning
  {
    std::copyable_function<void(std::string const&) const> const* sink;
    std::string                                                   message;
  };

  std::vector<held_warning> held_{};
  deferred_warnings*        outer_;
  int                       exceptions_;
};

/**
 * @brief A virtual full-speed USB host with one port, in virtual time. It sees the device's
 *        pull-up, debounces, resets the bus, enumerates, binds class drivers, then runs frames:
 *        a SOF every 1 ms, the periodic transactions of the frame, isochronous then interrupt,
 *        before the EOPF point, then control and bulk transactions one per slot, round robin,
 *        each slot after the bus time of the previous transaction. When every control and bulk
 *        pipe with work NAKed in a row, the next pass starts 10 us after that NAK ends, as long
 *        as its transaction fits before the next SOF. The pacing is borrowed from EHCI's
 *        AsyncSchedSleepTime. Real hosts at RL 0 retry faster, the gap bounds the wake-ups.
 *
 * The host acts only in host_side callbacks and through device_port::wake_at(). Its state,
 * drivers' included, is BSON in save(). The user control results and the request log are not.
 */
class POPCORN_API host final : public host_side
{
public:
  /** @brief What the host has scheduled or queued, host::pending(). */
  struct pending_work
  {
    /** @brief A frame start is scheduled. */
    bool frame{};
    /** @brief A control or bulk slot is scheduled. */
    bool slot{};
    /** @brief A port step is scheduled: a debounce, a bus reset or an enumeration wait ends. */
    bool port{};
    /** @brief A control pipe has a transfer queued. */
    bool control{};
    /** @brief A non-control OUT pipe has a transfer queued. */
    bool out{};
  };

  /**
   * @brief Creates a host that sees no device yet.
   * @param port The device, which outlives the host.
   * @param options Behaviour.
   */
  host(device_port& port, host_options options);
  /** @brief Destroys the host. */
  ~host() override;

  void pull_up_changed(bool on, vtime at) override;
  void on_time(vtime at) override;
  void save(std::vector<std::byte>& out) const override;
  void load(std::span<std::byte const> in) override;

  /** @brief What the host has scheduled or queued now. */
  [[nodiscard]] auto pending() const -> pending_work;

  /** @brief Device state. */
  [[nodiscard]] auto state() const -> device_state;

  /** @brief Address tokens go to, 0 before SET_ADDRESS. */
  [[nodiscard]] auto address() const -> std::uint8_t;

  /** @brief bConfigurationValue set, 0 when not configured. */
  [[nodiscard]] auto configuration_value() const -> std::uint8_t;

  /** @brief Whether the enumeration ran its last step. */
  [[nodiscard]] auto enumerated() const -> bool;

  /** @brief Whether the enumeration stopped on a failed step, which it then names. */
  [[nodiscard]] auto failed_step() const -> std::optional<enumeration_action>;

  /** @brief Device descriptor bytes read so far, empty before the first. */
  [[nodiscard]] auto device_bytes() const -> std::span<std::uint8_t const>;

  /** @brief Configuration descriptor bytes read so far, wTotalLength once complete. */
  [[nodiscard]] auto configuration_bytes() const -> std::span<std::uint8_t const>;

  /** @brief The configuration parsed from configuration_bytes(), when complete and valid. */
  [[nodiscard]] auto configuration() const -> configuration_info const*;

  /** @brief String descriptors read, string 0 included, in the order read. */
  [[nodiscard]] auto strings() const -> std::span<string_descriptor const>;

  /** @brief Language ID the strings were read in, 0 when the device has none. */
  [[nodiscard]] auto language() const -> std::uint16_t;

  /**
   * @brief Queues a control transfer on the default pipe for the user. It goes on the bus from
   *        the next frame. The result arrives in take_results(). With no device, the result is
   *        `cancelled` at once.
   * @param setup The SETUP packet.
   * @param data OUT data stage, wLength bytes. Empty for IN or no data.
   * @return The transfer.
   */
  auto control(setup_packet setup, std::vector<std::uint8_t> data = {}) -> transfer_id;

  /** @brief Takes the results of the user's control transfers, oldest first. Not saved. */
  [[nodiscard]] auto take_results() -> std::vector<transfer_result>;

  /**
   * @brief Whether the result of @p id waits in take_results().
   * @param id A handle control() returned.
   */
  [[nodiscard]] auto has_result(transfer_id id) const -> bool;

  /**
   * @brief Takes the result of @p id alone, leaving the others for their callers.
   * @param id A handle control() returned.
   * @return The result, empty while the transfer runs.
   */
  [[nodiscard]] auto take_result(transfer_id id) -> std::optional<transfer_result>;

  /**
   * @brief Drops the result of @p id: at once when it waits, else when the transfer ends. A load
   *        clears the transfers marked this way. Not saved.
   * @param id A handle control() returned.
   */
  void forget(transfer_id id);

  /** @brief Transfers forget() marked whose result has not landed yet. */
  [[nodiscard]] auto forgotten() const noexcept -> std::size_t;

  /** @brief Bumped by every load: a transfer id is unique within one epoch only. Not saved. */
  [[nodiscard]] auto epoch() const noexcept -> std::uint64_t;

  /** @brief Bus resets and SETUPs since creation or clear_log(). Not saved. */
  [[nodiscard]] auto log() const -> std::span<log_entry const>;

  /** @brief Empties the request log. */
  void clear_log();

  /** @brief Bound class drivers, in binding order. Rebuilt by a load(): do not keep pointers. */
  [[nodiscard]] auto drivers() const -> std::span<bound_driver const>;

  /**
   * @brief Access for a bound driver, to call it outside host callbacks.
   * @param driver Index in drivers().
   */
  [[nodiscard]] auto context(std::size_t driver) -> host_context;

private:
  friend class host_context;
  struct POPCORN_HIDDEN impl;
  std::indirect<impl>   impl_;
};
}
