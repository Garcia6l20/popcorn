#pragma once

#include "link_queues.hpp"

#include <popcorn/core/machine.hpp>
#include <popcorn/periph/uart.hpp>
#include <popcorn/periph/usb_cdc_acm.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace popcorn::bind::detail
{
/** @brief Bytes a pty keeps for the host while no tool reads it. */
inline constexpr std::size_t pty_outgoing_limit = 64 * 1024;

/** @brief Milliseconds with no open or close of a pty before its watcher walks /proc to verify
 * the count. */
inline constexpr int pty_quiet_ms = 20;

/** @brief Walks a pump starts over when events interrupt them, before it leaves the verification
 * to a later pump or to the watcher. */
inline constexpr int pty_walk_retries = 3;

/** @brief What the host wrote to a pty, split at the close that left no tool holding it. */
struct host_input
{
  /** @brief The bytes read at that close, written by the tool that closed. */
  std::vector<std::uint8_t> before;
  /** @brief Every byte read after it. */
  std::vector<std::uint8_t> after;

  [[nodiscard]] auto empty() const noexcept -> bool
  {
    return before.empty() and after.empty();
  }
};

/** @brief A change of the host tools holding a pty. */
enum class session_edge : std::uint8_t
{
  /** @brief The first tool opened the pty. */
  opened,
  /** @brief The last tool closed the pty. */
  closed,
};

/** @brief The byte link a pty bridge moves bytes through. Python-free. */
class pty_link
{
public:
  /** @brief A link. */
  pty_link() = default;
  /** @brief Not copyable. */
  pty_link(pty_link const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(pty_link const&) -> pty_link& = delete;
  /** @brief Destroys the link. */
  virtual ~pty_link() = default;

  /**
   * @brief Queues host bytes towards the firmware. Called by the thread that owns the engine.
   * @param bytes Bytes the host wrote.
   */
  virtual void send(std::span<std::uint8_t const> bytes) = 0;

  /**
   * @brief Takes every byte the firmware sent and not taken yet.
   * @param out Receives them, appended oldest first.
   */
  virtual void drain(std::vector<std::uint8_t>& out) = 0;

  /**
   * @brief Whether the device behind the link detached or attached since the last call.
   * @return False for a link with no such event.
   */
  [[nodiscard]] virtual auto device_changed() -> bool
  {
    return false;
  }

  /**
   * @brief Whether the device behind the link is gone, so the pty is hung up: true while no
   *        device is bound, and once when a device unbound since the last call that returned true,
   *        a bind and an unbind between two calls included.
   * @return False for a link with no such state.
   */
  [[nodiscard]] virtual auto detached() -> bool
  {
    return false;
  }

  /** @brief Whether the link acts on host_opened() and host_closed(). The bridge wakes the host
   * at an open or a close of the pty only for a link that does. */
  [[nodiscard]] virtual auto wants_sessions() const -> bool
  {
    return false;
  }

  /** @brief Whether the link has a line state to settle at the next pump. Called by the thread
   * that owns the engine. */
  [[nodiscard]] virtual auto unsettled() const -> bool
  {
    return false;
  }

  /** @brief Settles the line state the host tools holding the pty call for. Called by the thread
   * that owns the engine after every pump of the bridge that found unsettled() true. */
  virtual void settle()
  {}

  /** @brief A host tool opened the pty and none had it open. Called by the thread that owns the
   * engine, before the bytes the tool wrote. */
  virtual void host_opened()
  {}

  /** @brief The last host tool closed the pty. Called by the thread that owns the engine, after
   * the bytes the tool wrote. */
  virtual void host_closed()
  {}

  /** @brief Forgets what the link remembers of the device and settles the line state again, at a
   * restore. */
  virtual void discard() noexcept
  {}
};

/** @brief A file descriptor owned alone, closed on destruction. */
class unique_fd
{
public:
  /** @brief Owns nothing. */
  unique_fd() noexcept = default;
  /** @brief Owns @p fd, -1 for nothing. */
  explicit unique_fd(int fd) noexcept : fd_{fd}
  {}
  /** @brief Takes over @p other's descriptor. */
  unique_fd(unique_fd&& other) noexcept;
  /** @brief Closes the descriptor owned, then takes over @p other's. */
  auto operator=(unique_fd&& other) noexcept -> unique_fd&;
  /** @brief Not copyable. */
  unique_fd(unique_fd const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(unique_fd const&) -> unique_fd& = delete;
  /** @brief Closes the descriptor owned. */
  ~unique_fd();

  /** @brief The descriptor, -1 for none. */
  [[nodiscard]] auto get() const noexcept -> int
  {
    return fd_;
  }

private:
  int fd_ = -1;
};

/**
 * @brief Starts the thread that runs @p body.
 * @throws std::system_error when the system refuses the thread.
 */
[[nodiscard]] auto start_thread(std::move_only_function<void()> body) -> std::jthread;

/** @brief Starts a pty_bridge's watcher thread, start_thread() or a stand-in for tests. */
using thread_starter = std::function_ref<std::jthread(std::move_only_function<void()>)>;

/** @brief Opens the slave at a path for a pty_bridge's hang-up, as open(2) does or a stand-in for
 * tests. Returns the descriptor, or -1 with errno set. */
using slave_opener = std::move_only_function<int(char const*)>;

/**
 * @brief A host pty bridged to a byte link: host tools open path() like a serial port. Owns both
 *        ends of the pty and keeps the slave open, so the master never reads EIO or polls
 *        POLLHUP while no tool has the pty open. Through that slave it clears TIOCEXCL when the
 *        last tool closes the pty, as the last close of a serial port frees it, and when the
 *        link's device detaches or attaches, as a re-enumerated device is a fresh port.
 *
 *        A watcher thread per bridge reads the opens and closes of path() through inotify.
 *        When its count of tools falls to 0 at a close it clears TIOCEXCL at once, so the next
 *        process opens the pty whether the machine runs, sleeps or is parked. The count is a
 *        hint, so after a close the next pump, or the watcher once no open or close came for
 *        pty_quiet_ms, walks /proc for the tools still holding the pty, while the pty is
 *        exclusive, was cleared or the link wants sessions. A forked child holding a tool's
 *        descriptor counts, one holding only the bridge's own slave does not. A holder found
 *        after a clear sets the flag again, unless a tool opened since: two opens made before
 *        the watcher read the first count once, so between such a clear and the walk another
 *        tool can open the pty. At the last close the bridge also drops the bytes the pty holds
 *        for tools that no tool read, as a serial port drops its tty buffer when its last user
 *        closes it. flush() drops the bytes the link sends while no tool holds the pty after such
 *        a close, ordered with that flush under the bridge's mutex. It applies the queued events
 *        first, and confirms the empty count with a holder scan before it drops, since the merge
 *        above can leave a stale 0. Bytes sent before the first open stay buffered. The merge can
 *        bring the count to 0 while a tool still holds the pty, and the last close's flush then
 *        drops the bytes that tool had not read. A process that closes the pty and opens it again
 *        with no gap can still find it exclusive: the clear follows the close by the
 *        watcher's wake-up. Tools running as root open an exclusive pty anyway. The watcher runs no
 *        model code, no Python and no logging, and takes only the bridge's own mutex.
 *
 *        A kernel refusing the watch leaves only the clear at a device change, with one warning.
 *        A system refusing the thread leaves the count to pump(), which clears TIOCEXCL at the
 *        next slice, with one warning. pump() and discard() run on the thread that owns the
 *        engine, the others on any thread.
 */
class pty_bridge
{
public:
  /**
   * @brief Opens a raw pty for @p name over @p link, its master non-blocking, and starts its
   *        watcher through @p start.
   * @throws std::system_error when no pty can be opened.
   */
  pty_bridge(
      std::string               name,
      std::unique_ptr<pty_link> link,
      thread_starter            start  = start_thread,
      slave_opener              opener = {});

  /** @brief Not copyable. */
  pty_bridge(pty_bridge const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(pty_bridge const&) -> pty_bridge& = delete;

  /**
   * @brief Stops and joins the watcher, then closes the pty. In a process forked from the one
   *        that built the bridge it closes the descriptors only, and leaves the parent's watcher
   *        running.
   */
  ~pty_bridge();

  /** @brief Name of the bridged UART or CDC function, as Board.ptys keys it. */
  [[nodiscard]] auto name() const noexcept -> std::string const&
  {
    return name_;
  }

  /** @brief Path of the slave, /dev/pts/N. */
  [[nodiscard]] auto path() const noexcept -> std::string const&
  {
    return path_;
  }

  /** @brief The master descriptor, the one a host polls. */
  [[nodiscard]] auto master() const noexcept -> int
  {
    return master_.get();
  }

  /** @brief How many times the bridge walked /proc for tools holding the pty. Any thread. */
  [[nodiscard]] auto scans() const noexcept -> std::size_t
  {
    return scans_.load(std::memory_order_relaxed);
  }

  /** @brief Whether the pty is hung up. Called by the thread that owns the engine. */
  [[nodiscard]] auto hung_up() const noexcept -> bool
  {
    return hung_up_;
  }

  /** @brief Whether the watcher thread runs. */
  [[nodiscard]] auto watching() const noexcept -> bool
  {
    return watcher_.joinable();
  }

  /** @brief The descriptor that turns readable when a tool opens or closes the pty, for a poller:
   * the watcher's eventfd for a link that wants sessions, the inotify descriptor when no watcher
   * runs, -1 when nothing needs the poller. */
  [[nodiscard]] auto clients_watch() const noexcept -> int
  {
    return watching() ? wake_.get() : clients_watch_.get();
  }

  /** @brief Whether bytes for the host wait for room in the pty. Lock-free, any thread. */
  [[nodiscard]] auto wants_write() const noexcept -> bool
  {
    return wants_write_->load(std::memory_order_relaxed);
  }

  /** @brief The flag wants_write() reads, for a poller that must not own the bridge. */
  [[nodiscard]] auto wants_write_flag() const noexcept -> std::shared_ptr<std::atomic<bool> const>
  {
    return wants_write_;
  }

  /**
   * @brief Hangs the pty up or releases it when the link's detached() differs from hung_up().
   *        pump() and pump_all() call it before they read what the tools wrote, so bytes written
   *        before an unbind never reach the device bound next, and the flush() that follows
   *        does not look again. Called by the thread that owns the engine.
   */
  void follow_link();

  /**
   * @brief Moves bytes both ways: take_sessions() and take_input() into the link, then flush().
   * @return The warning of flush().
   */
  auto pump() -> std::optional<std::string>;

  /**
   * @brief Reads what the host wrote, until the pty has no more. When the watcher read a close
   *        that left no tool holding a link that wants sessions, it read the master then: those
   *        bytes are before, the rest after. A tool that wrote after its open before the watcher
   *        read the previous tool's close puts its bytes in before, and bytes that reach the
   *        master after that read are in after.
   */
  [[nodiscard]] auto take_input() -> host_input;

  /**
   * @brief Counts the opens and closes of the pty the watcher has not read yet, then reports
   *        the changes since the last call. Every open made before the call is seen, and every
   *        close whose last reference to the file was dropped: a process reading
   *        /proc/<pid>/fd/<n> holds the file for that moment, so a close() can return before the
   *        close event and the change comes at a later call. A close whose verification a tool
   *        loop kept interrupting is reported at a later call, once a walk completed.
   *        A close that leaves a counted holder makes no change. A close that drops the last
   *        counted holder, followed by an open, makes a close and an open, so a link sees every
   *        last close. The count is a hint: inotify merges identical unread events, so two
   *        merged opens count once and two merged closes count once. When an open lands
   *        while the verification of a close runs, the close and the open come at the next
   *        call, after the bytes the new holder wrote.
   * @return The changes of the host tools holding the pty, in order, empty for a link that does
   *         not want sessions.
   */
  [[nodiscard]] auto take_sessions() -> std::vector<session_edge>;

  /**
   * @brief Delivers sessions and input to the link: the input read at a last close, every
   *        change but a trailing close, the rest of the input, then that close.
   * @param sessions What take_sessions() returned.
   * @param input What take_input() returned.
   */
  void deliver(std::vector<session_edge> const& sessions, host_input const& input) const;

  /** @brief Whether pump_all() has work for the bridge besides what it read. */
  [[nodiscard]] auto settling() const -> bool
  {
    return link_->unsettled();
  }

  /** @brief The link, shared so input can be sent without holding the bridge. */
  [[nodiscard]] auto link() const noexcept -> std::shared_ptr<pty_link>
  {
    return link_;
  }

  /**
   * @brief Moves what the firmware sent into the pty, then keeps the last pty_outgoing_limit
   *        bytes the pty had no room for.
   * @return The warning to log the first time bytes are dropped, empty otherwise.
   */
  auto flush() -> std::optional<std::string>;

  /** @brief Drops the bytes waiting for the host, at a restore. */
  void discard() noexcept;

private:
  struct clients
  {
    std::size_t               count      = 0;
    bool                      holds      = false;
    bool                      opened     = false;
    bool                      closed     = false;
    bool                      overflowed = false;
    bool                      unverified = false;
    bool                      inexact    = false;
    bool                      cleared    = false;
    bool                      joined     = false;
    bool                      flushed    = false;
    bool                      confirmed  = false;
    bool                      cycled     = false;
    bool                      emptied    = false;
    std::vector<std::uint8_t> farewell;
  };

  struct batch
  {
    bool any    = false;
    bool closed = false;
    bool cycle  = false;
  };

  enum class verify : std::uint8_t
  {
    later,
    when_quiet,
    now,
  };

  std::string                        name_;
  unique_fd                          master_;
  unique_fd                          slave_;
  unique_fd                          clients_watch_;
  unique_fd                          stop_;
  unique_fd                          wake_;
  std::string                        clients_warning_;
  std::atomic<std::size_t>           scans_{0};
  std::string                        path_;
  std::shared_ptr<pty_link>          link_;
  bool                               wants_sessions_;
  std::uint64_t                      forks_;
  std::vector<std::uint8_t>          outgoing_;
  std::shared_ptr<std::atomic<bool>> wants_write_ = std::make_shared<std::atomic<bool>>(false);
  bool                               overflowed_  = false;
  bool                               held_        = false;
  bool                               hung_up_     = false;
  bool                               followed_    = false;
  bool                               hang_warned_ = false;
  bool                               own_open_    = false;
  slave_opener                       opener_;
  std::mutex                         clients_mutex_;
  clients                            clients_;
  std::vector<std::string>           holders_;
  std::jthread                       watcher_;

  /** @brief Starts watching path() and its thread through @p start. */
  void start_watching(thread_starter start);

  /** @brief Appends what the host wrote to @p into, until the pty has no more. */
  void read_master(std::vector<std::uint8_t>& into) const;

  /**
   * @brief Hangs the pty up. The kernel refuses an open of a slave its master locked with
   *        TIOCSPTLCK, with EIO, and checks TIOCEXCL before that lock, so the flag is cleared
   *        first. A tool that sets it again between the clear and the bridge's own open makes
   *        that open fail with EBUSY, which is retried once after another clear. The refused open
   *        sets the tty's I/O error flag: read and write fail with EIO on every descriptor
   *        already open, the bridge's own slave included, while ioctls keep working. The next
   *        open that succeeds, after release(), clears the flag for all of them. A reader asleep
   *        in read or poll is not woken and sees EIO at its next call. A refused open emits no
   *        inotify event. An open that succeeds anyway is closed, its events are not counted and
   *        one warning is logged. Drops the bytes the tools wrote that the bridge has not read,
   *        by reading the master and flushing both queues of the pty.
   */
  void hang_up();

  /** @brief Hangs the pty up or releases it when the link's detached() differs from hung_up(). */
  void follow();

  /** @brief Unlocks the pty. */
  void release();

  /** @brief The watcher thread: polls the inotify descriptor and stop_, consumes under
   * clients_mutex_, writes wake_ after an open or a close, and verifies once no event came for
   * pty_quiet_ms, writing wake_ again when that leaves a close for the pump. A failed poll is
   * retried after 1 ms, the thread ends only at the stop write. */
  void watch() noexcept;

  /**
   * @brief Reads every pending inotify event into clients_. The kernel emits one open per open
   *        file description and one close when its last reference goes, which can be a /proc walk
   *        in another process after the tool's close() returned: dup, fork, O_PATH opens and
   *        refused opens emit nothing. It merges an event into an identical unread one before
   *        it, so two quick opens or two quick closes count once, and the count is a hint.
   * @param held The lock of clients_mutex_.
   * @return What it read.
   */
  auto read_events(std::scoped_lock<std::mutex> const& held) -> batch;

  /**
   * @brief read_events(), then clears TIOCEXCL at once when a close brought the count to 0, and
   *        verifies by /proc when @p when allows it. The flag stays on the slave across its last
   *        close while the master is open, and the kernel checks it first at an open, so only
   *        the bridge's own slave descriptor can clear it. A verification walks /proc while the
   *        pty is exclusive, was cleared or the link wants sessions: no holder found clears the
   *        flag and the count, a holder found after a clear with no open since sets the flag
   *        again. An event arriving during the walk ends it: verify::now reads it and walks
   *        again, up to pty_walk_retries walks, then leaves the verification to the next pump or
   *        the watcher's next quiet period. verify::when_quiet reads it and leaves the walk to
   *        the next quiet period. A walk with no holder ends the inexact state an overflow left,
   *        in which a close at count 0 does not clear the flag.
   * @param held The lock of clients_mutex_.
   * @param when Whether to verify a close or an overflow read since the last verification.
   * @return Whether an open, a close or an overflow was read.
   */
  auto consume(std::scoped_lock<std::mutex> const& held, verify when) -> bool;

  /**
   * @brief Drops the bytes the pty holds for tools and no tool read, once per run of closes with
   *        no open between. Under clients_mutex_.
   */
  void drop_unread();

  /** @brief Whether a tool walk_holders() found last still holds the pty. Under clients_mutex_. */
  [[nodiscard]] auto still_held() const -> bool;

  /**
   * @brief Walks /proc for the descriptors of other tools holding the pty. Under clients_mutex_.
   * @return Empty when an inotify event arrived during the walk, which consume() reads first.
   */
  [[nodiscard]] auto walk_holders() -> std::optional<std::vector<std::string>>;
};

/** @brief A UART as a pty link: host bytes enter its receiver, the low 8 bits of each frame out. */
class uart_pty_link final : public pty_link
{
public:
  /**
   * @brief Links @p port and the queue @p endpoint attached to it.
   * @param port The UART, reached only by the engine owner.
   * @param endpoint Its far end, drained under its link lock.
   */
  uart_pty_link(periph::uart_port& port, std::shared_ptr<queue_endpoint> endpoint) noexcept;

  void send(std::span<std::uint8_t const> bytes) override;
  void drain(std::vector<std::uint8_t>& out) override;

private:
  periph::uart_port*              port_;
  std::shared_ptr<queue_endpoint> endpoint_;
};

/** @brief A CDC ACM function of a virtual USB host as a pty link, both ways under the hub lock. */
class cdc_pty_link final : public pty_link
{
public:
  /**
   * @brief Links function @p function of @p hub.
   * @param hub The channels of the controller's host.
   * @param function Function index.
   * @param attachments The channel's attachment count now, the baseline of device_changed().
   * @param unbinds The channel's unbind count now, the baseline of detached().
   * @param post Posts a SET_CONTROL_LINE_STATE with DTR and RTS as the engine owner, false when
   *        no driver is bound. It returns false when the machine is gone and throws nothing.
   */
  cdc_pty_link(
      std::shared_ptr<periph::usb::cdc_hub>     hub,
      std::size_t                               function,
      std::uint64_t                             attachments,
      std::uint64_t                             unbinds,
      std::move_only_function<bool(bool, bool)> post) noexcept;

  void               send(std::span<std::uint8_t const> bytes) override;
  void               drain(std::vector<std::uint8_t>& out) override;
  [[nodiscard]] auto device_changed() -> bool override;
  /** @brief True while no driver is bound, and once for a driver that unbound since the last
   * true. A restore of the machine's state is no unbind: the first call after discard() takes the
   * channel's count as the baseline. */
  [[nodiscard]] auto detached() -> bool override;

  [[nodiscard]] auto wants_sessions() const -> bool override
  {
    return true;
  }

  [[nodiscard]] auto unsettled() const -> bool override
  {
    return unsettled_;
  }

  /** @brief Raises DTR and RTS once a driver is configured, unless the device has them set. */
  void settle() override;
  /** @brief Posts SET_CONTROL_LINE_STATE with DTR and RTS, unless the device has them set. A
   * device with no configured driver is settled at the next pump that finds one. */
  void host_opened() override;
  /** @brief Posts SET_CONTROL_LINE_STATE with DTR and RTS clear, unless the device has them so
   * or no driver is configured. */
  void host_closed() override;
  /** @brief Forgets the request in flight and raises again while a tool holds the pty. A restore
   * settles at the restore instant, whatever the restored device does about enumeration. */
  void discard() noexcept override;

private:
  struct line_view
  {
    bool         ready;
    std::uint8_t lines;
  };

  std::shared_ptr<periph::usb::cdc_hub>     hub_;
  std::size_t                               function_;
  std::uint64_t                             seen_{};
  std::uint64_t                             unbinds_seen_{};
  bool                                      rebase_ = false;
  std::move_only_function<bool(bool, bool)> post_;
  std::optional<std::uint8_t>               requested_;
  std::uint64_t                             requested_at_{};
  bool                                      tool_holds_ = false;
  bool                                      unsettled_  = false;

  auto view() -> line_view;
  void raise();
  void request(std::uint8_t lines);
};
/** @brief A warning of a bridge, for the machine's log. */
struct pty_warning
{
  /** @brief Name of the bridge. */
  std::string bridge;
  /** @brief The text. */
  std::string message;
};

/**
 * @brief The pty bridges of a machine, pumped by its host thread around every slice. The mutex
 *        covers fd and queue work only: pump_all() sends host input to the links, engine code,
 *        after releasing it. A thread holding it takes nothing but a link lock, so it never waits
 *        for the GIL, a host mutex or engine code.
 */
struct pty_bridges
{
  /** @brief Guards bridges. */
  std::mutex mutex{};
  /** @brief The open bridges. */
  std::vector<std::shared_ptr<pty_bridge>> bridges{};
  /** @brief Whether bridges may hold one, read without the mutex. */
  std::atomic<bool> any{false};
};

/**
 * @brief Pumps every bridge of @p ptys, as the engine owner: input read under the mutex, sent
 *        after it, output flushed under it again.
 * @return The warnings to log, after the mutex was released.
 */
auto pump_all(pty_bridges& ptys) -> std::vector<pty_warning>;

/**
 * @brief pump_all(), then sends each warning to the log sinks of @p engine as a model_warning
 *        of its bridge, as the engine owner between slices. What a sink throws propagates.
 */
void pump_and_log(pty_bridges& ptys, core::machine& engine);

/**
 * @brief Adds @p bridge to @p ptys.
 * @param held The lock of @p ptys.
 */
void add_bridge(
    pty_bridges&                        ptys,
    std::unique_lock<std::mutex> const& held,
    std::shared_ptr<pty_bridge> bridge) pre(held.mutex() == &ptys.mutex and held.owns_lock());

/**
 * @brief Removes @p bridge from @p ptys.
 * @param held The lock of @p ptys.
 * @return The bridge, for the caller to destroy after releasing the lock, empty when absent.
 */
auto remove_bridge(
    pty_bridges&                        ptys,
    std::unique_lock<std::mutex> const& held,
    pty_bridge const*                   bridge)
    -> std::shared_ptr<pty_bridge> pre(held.mutex() == &ptys.mutex and held.owns_lock());
}
