#pragma once

#include "board.hpp"
#include "host_future.hpp"
#include "machine.hpp"

#include <popcorn/core/system.hpp>
#include <popcorn/periph/stm32/uart_wire.hpp>

#include <reflex/py.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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
};

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

  struct wire_entry
  {
    std::shared_ptr<periph::stm32::uart_wire> wire;
    periph::uart_port*                        sender_port{};
    std::shared_ptr<periph::uart_endpoint>    own;
    std::size_t                               sender{};
  };

  [[nodiscard]] auto system() const -> std::shared_ptr<core::system>;
  [[nodiscard]] auto member_index(std::size_t member) const -> std::size_t;
  [[nodiscard]] auto wire(std::size_t index) const -> periph::stm32::uart_wire&;
  void               give_back() noexcept;

  core::slice_bounds                          bounds_;
  std::size_t                                 tap_capacity_ = std::size_t{1} << 20;
  std::vector<member_entry>                   members_;
  std::vector<wire_entry>                     wires_;
  std::vector<reflex::py::nb::object>         handles_;
  std::shared_ptr<core::system>               system_;
  std::shared_ptr<std::atomic<std::uint64_t>> sleeps_ =
      std::make_shared<std::atomic<std::uint64_t>>(0);
  bool closed_{};
};
}
