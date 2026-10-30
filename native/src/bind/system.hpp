#pragma once

#include "../periph/stm32/i2c_medium.hpp"
#include "../periph/stm32/spi_medium.hpp"
#include "board.hpp"
#include "host_future.hpp"
#include "machine.hpp"

#include <popcorn/core/system.hpp>
#include <popcorn/periph/stm32/uart_wire.hpp>

#include <reflex/py.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace popcorn::bind
{
namespace py = reflex::py;

/** @brief One direction of a UART link, as System declares it to SystemHandle. */
struct[[= py::rename{"WireSpec"}]] wire_spec
{
  /** @brief Index of the sending member. */
  std::size_t sender{};
  /** @brief Index of the receiving member. */
  std::size_t receiver{};
  /** @brief Name of the receiving UART, for warnings. */
  std::string receiver_unit{};
  /** @brief Latency L in picoseconds, above 0. */
  std::int64_t latency_ps{};
  /** @brief Declared baud, above 0. */
  std::uint32_t baud{};
  /** @brief Declared M bits: 7, 8 or 9, the parity bit included when there is one. */
  std::uint8_t word_bits{8};
  /** @brief Declared stop length in half bits: 1, 2, 3 or 4. */
  std::uint8_t half_stops{2};
  /** @brief Declared parity: 0 none, 1 even, 2 odd. */
  std::uint8_t parity{};
  /** @brief The receiver's strict_peripherals: a shape mismatch is then a model_error. */
  bool strict{};
};

/** @brief One frame a wire's tap kept, times in picoseconds of system time. */
struct[[= py::rename{"TapRecord"}]] tap_record
{
  /** @brief Sending member. */
  [[= py::readonly]] std::size_t from{};
  /** @brief Receiving member. */
  [[= py::readonly]] std::size_t to{};
  /** @brief Number among the sender's frames on the wire, or among the injected ones. */
  [[= py::readonly]] std::uint64_t index{};
  /** @brief Data bits, 0 for a break. */
  [[= py::readonly]] std::uint16_t data{};
  /** @brief Start-bit edge. */
  [[= py::readonly]] std::int64_t start_ps{};
  /** @brief Stop-bit end, the abort time for a lost frame. */
  [[= py::readonly]] std::int64_t end_ps{};
  /** @brief When the receiver sees the frame end. */
  [[= py::readonly]] std::int64_t arrival_ps{};
  /** @brief Whether the frame is a break. */
  [[= py::readonly]] bool brk{};
  /** @brief Whether the sender aborted it. */
  [[= py::readonly]] bool lost{};
  /** @brief Whether an armed drop kept it off the line. */
  [[= py::readonly]] bool dropped{};
  /** @brief Whether it was injected. */
  [[= py::readonly]] bool injected{};
  /** @brief Whether a line cut touched it. */
  [[= py::readonly]] bool cut{};
};

/** @brief One slave of an SPI bus, as System declares it to SystemHandle. */
struct[[= py::rename{"SpiSlaveSpec"}]] spi_slave_spec
{
  /** @brief Index of the slave's member. */
  std::size_t member{};
  /** @brief Name of the slave's SPI instance. */
  std::string unit{};
  /** @brief Chip select GPIO port on the master, such as `GPIOA`, empty for the SS output. */
  std::string cs_port{};
  /** @brief Chip select pin on the master, empty for the SS output. */
  std::optional<std::uint8_t> cs_pin{};
};

/** @brief One SPI bus, as System declares it to SystemHandle. */
struct[[= py::rename{"SpiBusSpec"}]] spi_bus_spec
{
  /** @brief Index of the master's member. */
  std::size_t master{};
  /** @brief Name of the master's SPI instance. */
  std::string master_unit{};
  /** @brief Latency L in picoseconds, above 0. */
  std::int64_t latency_ps{};
  /** @brief The slaves, at least one. */
  std::vector<spi_slave_spec> slaves{};
};

/** @brief One frame an SPI bus's tap kept, times in picoseconds of system time. */
struct[[= py::rename{"SpiTapRecord"}]] spi_tap_record
{
  /** @brief Index of the slave among the bus's slaves. */
  [[= py::readonly]] std::size_t slave{};
  /** @brief Unit number the master side gave the frame. */
  [[= py::readonly]] std::uint64_t unit{};
  /** @brief Chip select pin on the master, empty for the SS output. */
  [[= py::readonly]] std::optional<std::uint8_t> cs_pin{};
  /** @brief End of the master's frame. */
  [[= py::readonly]] std::int64_t at_ps{};
  /** @brief Stamp of the slave's reply, or the time the frame was dropped. */
  [[= py::readonly]] std::int64_t reply_ps{};
  /** @brief Frame the master sent. */
  [[= py::readonly]] std::uint32_t mosi{};
  /** @brief Frame the slave returned. */
  [[= py::readonly]] std::uint32_t miso{};
  /** @brief Frame size in bits. */
  [[= py::readonly]] std::uint8_t bits{};
  /** @brief Whether the slave never clocked the frame. */
  [[= py::readonly]] bool dropped{};
};

/** @brief One target of an I2C bus, as System declares it to SystemHandle. */
struct[[= py::rename{"I2CTargetSpec"}]] i2c_target_spec
{
  /** @brief Index of the target's member. */
  std::size_t member{};
  /** @brief Name of the target's I2C instance. */
  std::string unit{};
};

/** @brief One I2C bus, as System declares it to SystemHandle. */
struct[[= py::rename{"I2CBusSpec"}]] i2c_bus_spec
{
  /** @brief Index of the controller's member. */
  std::size_t controller{};
  /** @brief Name of the controller's I2C instance. */
  std::string controller_unit{};
  /** @brief Latency L in picoseconds, above 0. */
  std::int64_t latency_ps{};
  /** @brief The targets, at least one. */
  std::vector<i2c_target_spec> targets{};
};

/** @brief One phase an I2C bus's tap kept, times in picoseconds of system time. */
struct[[= py::rename{"I2CTapRecord"}]] i2c_tap_record
{
  /** @brief Index of the target among the bus's targets. */
  [[= py::readonly]] std::size_t target{};
  /** @brief Unit number the controller side gave the phase. */
  [[= py::readonly]] std::uint64_t unit{};
  /** @brief The phase, the value of periph::i2c_phase. */
  [[= py::readonly]] std::uint8_t phase{};
  /** @brief The address byte, the byte the controller wrote or the byte the target sent. */
  [[= py::readonly]] std::uint8_t byte{};
  /** @brief The target's acknowledge, the controller's for a read byte. */
  [[= py::readonly]] bool ack{};
  /** @brief End of the controller's phase. */
  [[= py::readonly]] std::int64_t at_ps{};
  /** @brief Stamp of the target's answer, or the time the phase was dropped. */
  [[= py::readonly]] std::int64_t answer_ps{};
  /** @brief Whether the target never answered and the controller got the undriven answer. */
  [[= py::readonly]] bool dropped{};
};

namespace detail
{
/**
 * @brief One medium a system_handle declares: the member_links it adds to the system, how it
 *        attaches its ends at start() and how it gives them back at close. Each medium class
 *        owns its own state.
 */
class medium
{
public:
  /** @brief Destroys the medium. */
  virtual ~medium() = default;

  /** @brief The member_links the medium adds to the system, in core order. */
  [[nodiscard]] virtual auto links() const -> std::vector<std::shared_ptr<core::member_link>> = 0;

  /** @brief Attaches the medium's ends to their members, at start(). */
  virtual void attach() = 0;

  /**
   * @brief Gives the ends back to the members that owned them before attach(). Called after a
   *        failed start() and at close, any number of times.
   */
  virtual void give_back() noexcept = 0;

protected:
  medium()                                     = default;
  medium(medium const&)                        = default;
  medium(medium&&) noexcept                    = default;
  auto operator=(medium const&) -> medium&     = default;
  auto operator=(medium&&) noexcept -> medium& = default;
};

/**
 * @brief One UART wire, one direction from the sender's UART to the receiver's. A
 *        `popcorn.UartLink` gives two uart_medium records, one per wire, in add_wire order, so
 *        the wire index is the index of the record among the UART media.
 */
class uart_medium final : public medium
{
public:
  /**
   * @brief Holds a wire and the sender's UART it attaches to.
   * @param wire The wire.
   * @param sender Member index of the sender.
   * @param slot Host slot of the sender.
   * @param port The sender's UART port.
   * @param own The endpoint the sender's UART had before the wire, given back at close.
   */
  uart_medium(
      std::shared_ptr<periph::stm32::uart_wire> wire,
      std::size_t                               sender,
      host_ref                                  slot,
      periph::uart_port&                        port,
      std::shared_ptr<periph::uart_endpoint>    own);

  [[nodiscard]] auto links() const -> std::vector<std::shared_ptr<core::member_link>> override;
  void               attach() override;
  void               give_back() noexcept override;

  /** @brief The wire. */
  [[nodiscard]] auto wire() const noexcept -> periph::stm32::uart_wire&;
  /** @brief Member index of the sender. */
  [[nodiscard]] auto sender() const noexcept -> std::size_t;
  /** @brief Whether add_link paired the wire already. */
  [[nodiscard]] auto linked() const noexcept -> bool;
  /** @brief Marks the wire paired by add_link. */
  void link() noexcept;

private:
  std::shared_ptr<periph::stm32::uart_wire> wire_;
  std::size_t                               sender_{};
  host_ref                                  slot_;
  periph::uart_port*                        port_;
  std::shared_ptr<periph::uart_endpoint>    own_;
  bool                                      linked_{};
};

/**
 * @brief One SPI bus: a spi_medium over a master member and its slave members, attached at
 *        start() and given back at close.
 */
class spi_bus_medium final : public medium
{
public:
  /**
   * @param bus The medium.
   * @param master Host slot of the master.
   * @param slaves Host slot of each slave, in the medium's slave order.
   */
  spi_bus_medium(
      std::shared_ptr<periph::stm32::spi_medium> bus,
      host_ref                                   master,
      std::vector<host_ref>                      slaves);

  [[nodiscard]] auto links() const -> std::vector<std::shared_ptr<core::member_link>> override;
  void               attach() override;
  void               give_back() noexcept override;

  /** @brief The medium. */
  [[nodiscard]] auto bus() const noexcept -> periph::stm32::spi_medium&;

private:
  std::shared_ptr<periph::stm32::spi_medium> bus_;
  host_ref                                   master_;
  std::vector<host_ref>                      slaves_;
};

/**
 * @brief One I2C bus: an i2c_medium over a controller member and its target members, attached
 *        at start() and given back at close.
 */
class i2c_bus_medium final : public medium
{
public:
  /**
   * @param bus The medium.
   * @param controller Host slot of the controller.
   * @param targets Host slot of each target, in the medium's target order.
   */
  i2c_bus_medium(
      std::shared_ptr<periph::stm32::i2c_medium> bus,
      host_ref                                   controller,
      std::vector<host_ref>                      targets);

  [[nodiscard]] auto links() const -> std::vector<std::shared_ptr<core::member_link>> override;
  void               attach() override;
  void               give_back() noexcept override;

  /** @brief The medium. */
  [[nodiscard]] auto bus() const noexcept -> periph::stm32::i2c_medium&;

private:
  std::shared_ptr<periph::stm32::i2c_medium> bus_;
  host_ref                                   controller_;
  std::vector<host_ref>                      targets_;
};
}

/**
 * @brief Owner of one core::system over the hosts of MachineHandles, the object
 *        `popcorn.System` wraps. Members and wires are added, then start() builds and
 *        starts the system. Every method that takes the system mutex tries it with the GIL held
 *        and on contention releases the GIL and blocks, then calls the system with the GIL
 *        released, since the system takes host mutexes. A method called from a callback of a
 *        machine, inside a parked section, or from a completion callback that runs while this
 *        thread holds the system mutex raises RuntimeError instead of deadlocking.
 */
class[[= py::rename{"SystemHandle"}]] system_handle
{
  using cut_outcome = std::tuple<
      std::optional<std::int64_t>,
      std::optional<std::tuple<std::string, std::int64_t, std::int64_t>>>;

public:
  /**
   * @param slice_instructions Instructions after which a member's slice ends, above 0.
   * @param slice_wall_ns Wall time after which a member's slice ends, in nanoseconds, above 0.
   * @throws config_error when either is 0.
   */
  system_handle(std::uint64_t slice_instructions, std::int64_t slice_wall_ns);

  /** @brief Not copyable. */
  system_handle(system_handle const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(system_handle const&) -> system_handle& = delete;
  /**
   * @brief Closes the system as close() does, without raising. On a host thread, inside a
   *        parked section or under the system mutex, a collection there, the system is closed
   *        and destroyed by a thread of its own, and each UART is given back at its machine's
   *        next boundary. While the interpreter finalizes the system is left alive.
   */
  ~system_handle();

  /**
   * @brief Adds a member before start().
   * @param member The machine, parked.
   * @param name Name of the member in errors.
   * @param stop_on_reset Whether a firmware or watchdog reset of a member that runs its core
   *        ends its goal, rather than resetting it and running on.
   * @return The member's index.
   * @throws std::logic_error after start().
   */
  [[= py::doc{"Add a member before start(), return its index."}]] auto
      add_member(machine_handle& member, std::string name, bool stop_on_reset) -> std::size_t;

  /**
   * @brief Adds a wire before start(): frames the sender's UART transmits reach the receiver's
   *        UART L later, and still reach the sender link's own queue.
   * @param spec The wire.
   * @param sender The sending UART's link of the sending member.
   * @param receiver The receiving UART's link of the receiving member.
   * @return The wire's index.
   * @throws std::logic_error after start().
   * @throws std::invalid_argument for a member index out of range, a latency or baud of 0, or a
   *         shape outside the UART's.
   * @throws config_error when the receiving UART's model takes no frames from a wire.
   */
  [[= py::doc{"Add a wire before start(), return its index."}]] auto
      add_wire(wire_spec const& spec, uart_queue const& sender, uart_queue const& receiver)
          -> std::size_t;

  /**
   * @brief Pairs the wires @p forth and @p back as one link before start(): they share one
   *        periph::stm32::link_registration, and the link's cut owner is @p forth's sender.
   * @return The link's index.
   * @throws std::logic_error after start().
   * @throws std::invalid_argument for a wire out of range, wires that are not the two
   *         directions between the same members, or a wire in a link already.
   */
  [[= py::doc{"Pair two wires as one link before start(), return its index."}]] auto
      add_link(std::size_t forth, std::size_t back) -> std::size_t;

  /**
   * @brief Adds an SPI bus before start(): the master's SPI frames reach the slaves' SPIs L
   *        later and their replies the master L after that. The bus holds two links per
   *        slave, added to the system in slave order.
   * @param spec The bus.
   * @return The bus's index among the SPI buses.
   * @throws std::logic_error after start().
   * @throws std::invalid_argument for a member index out of range, a slave on the master's
   *         member, no slave or a latency of 0.
   * @throws config_error with key `master` or `slaves[i]` and the offending unit as file when an
   *         end is no SPI instance of its member, the master's SPI has a board controller, a
   *         slave's SPI has a board controller or device, or a chip select names no GPIO port or
   *         pin of the master. With key `pin` when the chip select is an unbonded pad of the
   *         master's package.
   */
  [[= py::doc{"Add an SPI bus before start(), return its index."}]] auto
      add_spi_bus(spi_bus_spec const& spec) -> std::size_t;

  /** @brief Takes every frame the tap of SPI bus @p bus holds, oldest first. */
  [[= py::doc{"Take the frames an SPI bus's tap holds."}]] auto spi_tap_take(std::size_t bus)
      -> std::vector<spi_tap_record>;

  /** @brief Frames the tap of SPI bus @p bus dropped because it was full. */
  [[= py::doc{"Frames an SPI bus's tap dropped."}]] auto spi_tap_dropped(std::size_t bus)
      -> std::uint64_t;

  /**
   * @brief Adds an I2C bus before start(): the controller's phases reach the targets' I2Cs L
   *        later and their answers the controller L after that. The bus holds two links per
   *        target, added to the system in target order.
   * @param spec The bus.
   * @return The bus's index among the I2C buses.
   * @throws std::logic_error after start().
   * @throws std::invalid_argument for a member index out of range, a target on the controller's
   *         member, no target or a latency of 0.
   * @throws config_error with key `controller` or `targets[i]` and the offending unit as file
   *         when an end is no I2C instance of its member, the controller's I2C has a board
   *         controller, or a target's I2C has a board controller or a board target.
   */
  [[= py::doc{"Add an I2C bus before start(), return its index."}]] auto
      add_i2c_bus(i2c_bus_spec const& spec) -> std::size_t;

  /** @brief Takes every phase the tap of I2C bus @p bus holds, oldest first. */
  [[= py::doc{"Take the phases an I2C bus's tap holds."}]] auto i2c_tap_take(std::size_t bus)
      -> std::vector<i2c_tap_record>;

  /** @brief Phases the tap of I2C bus @p bus dropped because it was full. */
  [[= py::doc{"Phases an I2C bus's tap dropped."}]] auto i2c_tap_dropped(std::size_t bus)
      -> std::uint64_t;

  /**
   * @brief Attaches the wires, builds the system and fixes each member's base: member i's local
   *        time now is system time @p offsets_ps[i].
   * @throws machine_busy when a member is busy.
   * @throws std::logic_error when started already, with no member or no wire, or when
   *         @p offsets_ps does not have one entry per member.
   */
  [[= py::doc{"Build and start the system, offsets in picoseconds of system time."}]] void
      start(std::vector<std::int64_t> const& offsets_ps);

  /**
   * @brief core::system::resume().
   * @param until_ps System time every member runs its core to, None to halt them.
   * @return Resolves to a tuple (PauseReason, member or None, at_ps, StopInfo, error or None,
   *         windows, slices). Cancelling it requests a pause.
   */
  [[= py::doc{"Resume until the next pause, return a HostFuture of the pause tuple."}]] auto
      resume(std::optional<std::int64_t> until_ps) -> host_future_handle;

  /**
   * @brief core::system::post_goal().
   * @param member Member index.
   * @param until Stop before executing this address.
   * @param budget Instructions the goal may execute.
   * @param exact Stop after exactly this many instructions.
   * @param until_ps Stop at this system time.
   * @param stop_when Predicate asked on the member's host thread after each dispatched event,
   *        in a copy of the caller's context.
   * @param gate Native predicate asked first, with no GIL: @p stop_when is asked only while it
   *        holds, and without @p stop_when the goal stops when it holds.
   * @param open Whether a system IDLE leaves the goal open.
   * @param cap_ps Virtual time the goal may run from its start.
   * @param until_return Stop with reason returned at this (return address or EXC_RETURN, stack
   *        pointer), core::frame_return. None for none.
   * @return Resolves to a tuple (StopInfo, at_ps). Cancelling it requests a pause only: the
   *         caller withdraws the goal at that pause. From the post until the future completes,
   *         by its delivery, a withdraw(), finish() or an error, a parked access of the member
   *         raises MachineRunning: its position at a pause depends on the quantum.
   */
  [[= py::doc{"Post a member goal while paused, return a HostFuture of (StopInfo, at_ps)."}]] auto
      post_goal(
          std::size_t                                       member,
          std::optional<address>                            until,
          std::optional<std::uint64_t>                      budget,
          std::optional<std::uint64_t>                      exact,
          std::optional<std::int64_t>                       until_ps,
          std::optional<reflex::py::nb::object>             stop_when,
          std::optional<native_predicate>                   gate,
          bool                                              open,
          std::optional<std::int64_t>                       cap_ps,
          std::optional<std::tuple<address, std::uint64_t>> until_return) -> host_future_handle;

  /**
   * @brief core::system::post_goal() of an open goal that serves several waits at once.
   * @param member Member index.
   * @param untils Stop before executing any of these addresses.
   * @param arrive_at Addresses of @p untils the goal stops at when it starts there.
   * @param until_ps Stop at this system time.
   * @param gates Each wait's native predicate, None for none.
   * @param stop_whens Each wait's Python predicate, None for none, asked only while that wait's
   *        gate holds. A wait with neither never holds.
   * @param cap_ps Virtual time the goal may run from its start.
   * @return Resolves to a tuple (StopInfo, at_ps, held): bit i of held is set when wait i held
   *         at the check that stopped the goal, 0 for a stop no wait asked for. Otherwise as
   *         post_goal().
   * @throws ValueError for more than 64 waits or lists of different lengths.
   */
  [[= py::doc{"Post an open goal serving several waits, return a HostFuture of (StopInfo, at_ps, "
              "held)."}]] auto
      post_waits(
          std::size_t                                        member,
          std::vector<address>                               untils,
          std::vector<address>                               arrive_at,
          std::optional<std::int64_t>                        until_ps,
          std::vector<std::optional<native_predicate>>       gates,
          std::vector<std::optional<reflex::py::nb::object>> stop_whens,
          std::optional<std::int64_t>                        cap_ps) -> host_future_handle;

  /** @brief core::system::withdraw().
   * @throws IndexError for no such member. */
  [[= py::doc{"Withdraw a member's pending goal while paused."}]] void withdraw(std::size_t member);

  /** @brief core::system::release(). */
  [[= py::doc{
      "Release an undecided member while paused, halted from where it stands when halt."}]] void
      release(std::size_t member, bool halt);

  /** @brief core::system::undecided(). */
  [[= py::doc{"Members whose goal was delivered, neither re-issued nor released."}]] auto
      undecided() -> std::vector<std::size_t>;

  /** @brief core::system::finish(). */
  [[= py::doc{"Final stop at the last pause point, no resume after it."}]] void finish();

  /** @brief Refuses a caller that may not drive the system, as every other call does.
   * @throws RuntimeError from a callback of a machine. */
  [[= py::doc{"Raise as a call that drives the system would from this thread."}]] void
      check_caller() const;

  /** @brief core::system::request_pause(). */
  [[= py::doc{"Ask for a pause at the next barrier."}]] void request_pause();

  /** @brief core::system::wake(). */
  [[= py::doc{"Wake a system asleep on IDLE inside open goals."}]] void wake();

  /** @brief core::system::reset(). */
  [[= py::doc{"Reset a member while paused."}]] void reset(std::size_t member, reset_kind kind);

  /**
   * @brief Closes the system, waits for every member to park, unlinks the hosts and gives each
   *        sending UART its own endpoint back. Idempotent. A call in flight on another thread
   *        keeps the system alive until it returns, and the last holder destroys it with the
   *        GIL released.
   */
  [[= py::doc{"Close the system, unlink the members and give the UARTs back."}]] void close();

  /** @brief core::system::save(). */
  [[= py::doc{"The system's own state, members apart."}]] auto save() -> reflex::py::nb::bytes;

  /** @brief core::system::load(). */
  [[= py::doc{"Restore the system's own state after the members were restored."}]] void
      load(reflex::py::nb::bytes const& state);

  /** @brief core::system::check_state(). */
  [[= py::doc{"Check that load() would accept a state, changing nothing."}]] void
      check_state(reflex::py::nb::bytes const& state);

  /** @brief The quantum Q in picoseconds. */
  [[= py::doc{"The quantum, the smallest link latency, in picoseconds."}]] [[nodiscard]] auto
      quantum_ps() const -> std::int64_t;

  /** @brief core::system::pending_barrier() in picoseconds. */
  [[= py::doc{"The first barrier not exchanged yet, in picoseconds."}]] auto pending_barrier_ps()
      -> std::int64_t;

  /** @brief core::system::time_of() in picoseconds. */
  [[= py::doc{"System time of a parked member, in picoseconds."}]] auto
      time_of_ps(std::size_t member) -> std::int64_t;

  /** @brief core::system::base_of() in picoseconds, any thread, a machine's callbacks
   * included, no lock: it reads `system_` under the GIL, so it never takes or drops a
   * reference to the system.
   * @throws IndexError for no such member. */
  [[= py::doc{"A member's base in picoseconds: its system time minus its local time."}]] auto
      base_of_ps(std::size_t member) -> std::int64_t;

  /** @brief core::system::executed_of(). */
  [[= py::doc{"Instructions a parked member executed since the system started."}]] auto
      executed_of(std::size_t member) -> std::uint64_t;

  /** @brief Takes every frame the tap of wire @p wire holds, oldest first. */
  [[= py::doc{"Take the frames a wire's tap holds."}]] auto tap_take(std::size_t wire)
      -> std::vector<tap_record>;

  /** @brief Frames the tap of wire @p wire dropped because it was full. */
  [[= py::doc{"Frames a wire's tap dropped."}]] auto tap_dropped(std::size_t wire) -> std::uint64_t;

  /**
   * @brief periph::stm32::uart_wire::inject(): frames back to back on wire @p wire's receiving
   *        line from system time @p at_ps. With @p paused, the system paused or stopped, the
   *        receiver's position and base are read under the system mutex and its host mutex, and
   *        accepted frames are delivered at once. Else, while it runs, any thread, the
   *        acceptance depends on the drains so far, and accepted frames wake a system asleep on
   *        IDLE.
   * @return None when accepted, else a tuple (reason name, bound_ps).
   */
  [[= py::doc{"Inject frames on a wire's line, None or (reason, bound_ps) for a refusal."}]] auto
      inject(
          std::size_t                       wire,
          std::vector<std::uint16_t> const& data,
          std::int64_t                      at_ps,
          bool paused) -> std::optional<std::tuple<std::string, std::int64_t>>;

  /**
   * @brief periph::stm32::cut_link() over [@p start_ps, @p end_ps) on link @p link. The
   *        caller decides the bound: inside a parked section MachineRunning. On a machine's
   *        thread, from a callback of the link's cut owner, its system time, from any other
   *        callback RuntimeError. With @p stopped, the later of the two ends' system times,
   *        under the system mutex. Else, while it runs, the reach of the link's drains, and an
   *        accepted cut wakes a system asleep on IDLE.
   * @return (start_ps, None), or (None, (reason name, bound_ps, end_ps)) for a refusal.
   */
  [[= py::doc{"Cut a link over [start_ps, end_ps), the line low when low."}]] auto
      cut(std::size_t link, std::int64_t start_ps, std::int64_t end_ps, bool low, bool stopped)
          -> cut_outcome;

  /**
   * @brief periph::stm32::cut_link() of a cut of link @p link open from now. Now is the cut
   *        owner's system time in its callback, or with @p stopped the later of the two ends'
   *        system times. Callers as cut(), except that a caller off a machine's thread
   *        without @p stopped gets RuntimeError.
   * @return (start_ps, None), or (None, (reason name, bound_ps, end_ps)) for a refusal.
   */
  [[= py::doc{"Open a cut of a link from now."}]] auto
      break_line(std::size_t link, bool low, bool stopped) -> cut_outcome;

  /**
   * @brief periph::stm32::restore_link() of link @p link at now, as break_line() takes it.
   * @return (end_ps, None), or (None, (reason name, bound_ps, end_ps)) for a refusal.
   */
  [[= py::doc{"End a link's open cut now."}]] auto restore(std::size_t link, bool stopped)
      -> cut_outcome;

  /**
   * @brief The member whose callbacks may change link @p link's cuts while it runs.
   * @throws machine_closed once the system is closed.
   */
  [[= py::doc{"A link's cut owner, a member index."}]] [[nodiscard]] auto
      link_owner(std::size_t link) const -> std::size_t;

  /** @brief periph::stm32::max_waiting_cuts, the most cuts one link holds at once. */
  [[= py::doc{"The most cuts one link holds at once."}]] [[nodiscard]] auto max_waiting_cuts() const
      -> std::size_t;

  /** @brief Sets link @p link's cut owner, while the system is stopped.
   * @throws IndexError for no such link or member. */
  [[= py::doc{"Set a link's cut owner while stopped."}]] void
      set_link_owner(std::size_t link, std::size_t member);

  /** @brief periph::stm32::uart_wire::replace(), any thread. */
  [[= py::doc{"Arm new data for frames from index on, whether armed."}]] auto
      replace(std::size_t wire, std::uint64_t index, std::vector<std::uint16_t> const& data)
          -> bool;

  /** @brief periph::stm32::uart_wire::drop(), any thread. */
  [[= py::doc{"Arm a drop of count frames from index on, whether armed."}]] auto
      drop(std::size_t wire, std::uint64_t index, std::uint64_t count) -> bool;

  /** @brief periph::stm32::uart_wire::posted(), any thread. */
  [[= py::doc{"Frames the sender started on a wire so far."}]] auto posted(std::size_t wire)
      -> std::uint64_t;

  /** @brief How many times a member asked to sleep on IDLE inside open goals, the
   * core::system before_sleep hook. Any thread, no lock, for tests. */
  [[= py::doc{
      "Times a member went to sleep on IDLE inside open goals, for tests."}]] [[nodiscard]] auto
      sleeps() const noexcept -> std::uint64_t;

  /** @brief Sets the most frames the tap of each wire added later keeps, for tests. */
  [[= py::doc{"Frames the taps of wires added from now on keep, for tests."}]] void
      set_tap_capacity(std::size_t capacity);

  /** @brief Members, wires and whether start() ran. */
  [[= py::doc{"Members, wires and whether started, for repr."}]] [[nodiscard]] auto shape() const
      -> std::tuple<std::size_t, std::size_t, bool>;

  /** @brief The native system, empty before start(). */
  [[= py::skip]] [[nodiscard]] auto native() const noexcept -> core::system*;

  [[= py::traverse]] void report_python(reflex::py::gc_visitor& visit) const noexcept
  {
    for(auto const& one : handles_)
    {
      visit(one);
    }
  }

private:
  struct member_entry
  {
    machine_handle*             handle{};
    std::string                 name;
    bool                        stop_on_reset{};
    std::shared_ptr<core::host> host;
    detail::host_ref            slot;
  };

  struct link_entry
  {
    std::size_t              forth{};
    std::size_t              back{};
    std::atomic<std::size_t> owner{};
  };

  using cut_change = std::function_ref<std::expected<core::sys_time, periph::stm32::cut_refusal>(
      periph::stm32::uart_wire&,
      periph::stm32::uart_wire&,
      std::optional<core::sys_time>)>;

  [[nodiscard]] auto system() const -> std::shared_ptr<core::system>;
  [[nodiscard]] auto member_index(std::size_t member) const -> std::size_t;
  [[nodiscard]] auto wire(std::size_t index) const -> periph::stm32::uart_wire&;
  [[nodiscard]] auto link(std::size_t index) const -> link_entry const&;
  auto               change_cut(
      std::size_t      index,
      std::string_view what,
      bool             stopped,
      bool             from_body,
      cut_change       change) -> cut_outcome;
  void give_back() noexcept;

  core::slice_bounds                            bounds_;
  std::size_t                                   tap_capacity_ = std::size_t{1} << 20;
  std::vector<member_entry>                     members_;
  std::vector<std::polymorphic<detail::medium>> media_;
  std::vector<detail::uart_medium*>             wires_;
  std::vector<detail::spi_bus_medium*>          buses_;
  std::vector<detail::i2c_bus_medium*>          i2c_buses_;
  std::uint32_t                                 next_lane_ = 1;
  std::deque<link_entry>                        links_;
  std::vector<reflex::py::nb::object>           handles_;
  std::shared_ptr<core::system>                 system_;
  std::shared_ptr<std::atomic<std::uint64_t>>   sleeps_ =
      std::make_shared<std::atomic<std::uint64_t>>(0);
  bool closed_{};
};
}
