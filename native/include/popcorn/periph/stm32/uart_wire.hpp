#pragma once

#include <popcorn/core/link_guard.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/member_link.hpp>
#include <popcorn/periph/stm32/uart.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace popcorn::periph::stm32
{
/** @brief The line a UART link declares: baud and frame format. */
struct line_shape
{
  /** @brief Baud, above 0. */
  std::uint32_t baud{};
  /** @brief M bits: 7, 8 or 9. */
  std::uint8_t word_bits{8};
  /** @brief Stop length in half bits: 1, 2, 3 or 4. */
  std::uint8_t half_stops{2};
  /** @brief Parity. */
  line_parity parity{};

  /** @brief Frame time, start bit to stop-bit end, in picoseconds. */
  [[nodiscard]] auto frame_ps() const noexcept -> std::uint64_t;

  /**
   * @brief Whether a frame has another shape: another word or stop length, or a frame time more
   *        than 3 percent off. 3 percent passes the BRR rounding of common clock and baud pairs
   *        and stays inside the sampler's error-free band.
   */
  [[nodiscard]] auto differs(line_frame const& frame) const noexcept -> bool;
};

/** @brief Member indices of a wire's two ends. */
struct wire_ends
{
  /** @brief Member whose UART transmits on the wire. */
  std::size_t sender{};
  /** @brief Member whose UART receives from it. */
  std::size_t receiver{};
};

/** @brief Delivery lanes of a wire on its receiver, see core::scheduler. Both 1 and up. */
struct wire_lanes
{
  /** @brief Lane of the frames the sender transmits. */
  std::uint32_t peer{};
  /** @brief Lane of injected frames. */
  std::uint32_t injected{};
};

/** @brief The receiving side of a wire: the port and how to report on its machine. */
struct wire_receiver
{
  /** @brief The receiver's UART, which must outlive the wire's drains. */
  uart_port* port{};
  /** @brief Name of the receiver's UART in warnings. */
  std::string unit;
  /** @brief The receiver machine's log, called on its host thread between slices. Decision
   * 11: nothing reports on a host thread, so this must queue into the machine's own log, flushed
   * later as model warnings are, never call a Python sink. */
  std::function<void(core::log_record)> log;
  /** @brief The receiver machine's machine_options::strict_peripherals. */
  bool strict{};
};

/** @brief One frame of a wire, as the tap shows it. */
struct tap_frame
{
  /** @brief Sender member. */
  std::size_t from{};
  /** @brief Receiver member. */
  std::size_t to{};
  /** @brief Number of the frame among the sender's frames on this wire, from 0. */
  std::uint64_t index{};
  /** @brief Data bits, 0 for a break. */
  std::uint16_t data{};
  /** @brief Start-bit edge at the sender, system time. */
  core::sys_time start{};
  /** @brief Stop-bit end at the sender, system time. The abort time for a lost frame. */
  core::sys_time end{};
  /** @brief end plus the wire's latency, when the receiver sees the frame end. */
  core::sys_time arrival{};
  /** @brief Whether the frame is a break. */
  bool brk{};
  /** @brief Whether the sender aborted the frame, UE cleared while it was in flight. */
  bool lost{};
  /** @brief Whether an armed drop kept the frame off the line: the receiver saw it idle. */
  bool dropped{};

  /** @brief Member-wise equality. */
  friend auto operator==(tap_frame const&, tap_frame const&) -> bool = default;
};

/**
 * @brief Frames of a wire for observers, any thread. A link lock guards it. Holds at most a
 *        capacity of frames, the oldest dropped and counted.
 */
class tap_queue
{
public:
  /** @param capacity Frames kept, above 0. */
  explicit tap_queue(std::size_t capacity) pre(capacity > 0);

  /** @brief Appends @p frame, dropping the oldest when full. */
  void push(tap_frame const& frame);

  /** @brief Takes every frame held, oldest first. */
  auto take() -> std::vector<tap_frame>;

  /** @brief Frames dropped so far because the queue was full. */
  [[nodiscard]] auto dropped() const -> std::uint64_t;

private:
  mutable std::mutex    mutex_;
  std::deque<tap_frame> frames_;
  std::size_t           capacity_;
  std::uint64_t         dropped_{};
};

namespace detail
{
/** @brief A sender's post on a wire: a frame at its start bit, or an abort. */
struct wire_post
{
  bool           abort{};
  bool           dropped{};
  std::uint64_t  index{};
  line_frame     frame{};
  core::sys_time start{};
  core::sys_time end{};

  friend auto operator==(wire_post const&, wire_post const&) -> bool = default;
};

/** @brief An edit armed for frames not started yet: new data, or a drop when data is empty. */
struct armed_edit
{
  std::uint64_t              first{};
  std::uint64_t              count{};
  std::vector<std::uint16_t> data;

  friend auto operator==(armed_edit const&, armed_edit const&) -> bool = default;
};

/** @brief Everything a wire saves. */
struct wire_state
{
  std::vector<wire_post>  mail_even;
  std::vector<wire_post>  mail_odd;
  std::uint64_t           window{};
  core::sys_time          sender_base{};
  std::uint64_t           started{};
  std::vector<armed_edit> edits;
  std::vector<tap_frame>  untapped;
  std::uint64_t           tapped{};
  bool                    mismatch_logged{};

  friend auto operator==(wire_state const&, wire_state const&) -> bool = default;
};
}

/**
 * @brief One direction of a UART link between two members of a linked system. Attached as the
 *        sender port's far end, it numbers each frame at its start bit and posts it, in system
 *        time, to the mailbox of the sender's window. The receiver's drain after the barrier
 *        hands each frame to the receiver's port at its start plus L in the receiver's time, on
 *        the wire's peer lane, and each abort as a cancel at its time plus L. A frame whose shape
 *        differs from the declared line logs one warning per wire on the receiver, and under
 *        strict_peripherals is also a model_error at the frame's sender start. The check compares
 *        each sender with the declaration, not with the receiver: two ends just under 3 percent
 *        off in opposite directions are about 6 percent apart and get no warning, the
 *        receiver's FE and NE still show it. Frames the sender
 *        transmits still reach the endpoint the wire replaced. Edits armed from any thread
 *        replace or drop frames by number. The tap gets each frame once its
 *        end, or its abort for a lost frame, lies at or before a drained barrier or the final
 *        stop, so its content at a barrier does not depend on how the run was cut into windows.
 */
class uart_wire final : public uart_endpoint, public core::member_link
{
public:
  /**
   * @param ends Sender and receiver members.
   * @param lanes Lanes on the receiver.
   * @param latency L, above 0.
   * @param declared The line the link declares.
   * @param into The receiving side.
   * @param forward Endpoint the wire replaced on the sender's port, may be empty.
   * @param tap_capacity Frames the tap keeps.
   */
  uart_wire(
      wire_ends                      ends,
      wire_lanes                     lanes,
      core::sys_time                 latency,
      line_shape                     declared,
      wire_receiver                  into,
      std::shared_ptr<uart_endpoint> forward,
      std::size_t tap_capacity = std::size_t{1} << 20) pre(latency.ps > 0) pre(lanes.peer > 0)
      pre(lanes.injected > 0) pre(lanes.peer != lanes.injected) pre(into.port != nullptr)
          pre(declared.baud > 0);

  void started(line_frame const& frame) override;
  void aborted(vtime at) override;
  void transmitted(std::uint16_t data, vtime at, bool brk) override;

  [[nodiscard]] auto sender() const noexcept -> std::size_t override;
  [[nodiscard]] auto receiver() const noexcept -> std::size_t override;
  [[nodiscard]] auto latency() const noexcept -> core::sys_time override;
  void               enter_window(std::uint64_t window, core::time_map sender) override;
  auto               drain(std::uint64_t window, core::sys_time barrier, core::time_map receiver)
      -> std::optional<core::link_fault> override;
  void finish(core::sys_time at) override;
  void save(std::vector<std::byte>& out) const override;
  void load(std::span<std::byte const> in) override;

  /** @brief The wire's tap, any thread. */
  [[nodiscard]] auto tap() noexcept -> tap_queue&;

  /**
   * @brief Arms new data for the frames numbered @p index on, one per element, any thread. The
   *        sender reads armed edits under the same lock as it numbers a frame, so an edit armed
   *        before the frame starts applies and one armed after is refused. Which of the two
   *        happens depends on wall timing while the sender runs, the effect of an accepted edit
   *        only on the numbers.
   * @param index Number of the first frame, see tap_frame::index.
   * @param data Data bits for frames @p index and on, not empty.
   * @return Whether the edit was armed: false when a frame it covers has started, or when it
   *         overlaps an edit already armed.
   */
  auto replace(std::uint64_t index, std::span<std::uint16_t const> data)
      -> bool pre(not data.empty());

  /**
   * @brief Arms a drop of @p count frames from @p index on, any thread: the sender transmits
   *        them, the receiver sees an idle line, the tap shows them dropped. Armed as replace().
   * @param index Number of the first frame.
   * @param count Frames dropped, above 0.
   * @return Whether the drop was armed, as replace().
   */
  auto drop(std::uint64_t index, std::uint64_t count) -> bool pre(count > 0);

  /** @brief Frames the sender has started on this wire so far, any thread. */
  [[nodiscard]] auto posted() const -> std::uint64_t;

private:
  auto mailbox(std::uint64_t window) -> std::vector<detail::wire_post>&;
  void collect(std::vector<detail::wire_post> const& posts, std::vector<tap_frame>& into) const;
  void emit(std::vector<tap_frame>& from, core::sys_time through);
  auto check_shape(detail::wire_post const& post) -> std::optional<core::link_fault>;
  auto arm(detail::armed_edit edit) -> bool;

  wire_ends                      ends_;
  wire_lanes                     lanes_;
  core::sys_time                 latency_;
  line_shape                     declared_;
  wire_receiver                  into_;
  std::shared_ptr<uart_endpoint> forward_;
  tap_queue                      tap_;
  mutable std::mutex             edit_mutex_;
  detail::wire_state             state_;
};
}
