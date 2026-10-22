#pragma once

#include "link_queues.hpp"

#include <popcorn/core/machine.hpp>
#include <popcorn/periph/uart.hpp>
#include <popcorn/periph/usb_cdc_acm.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace popcorn::bind::detail
{
/** @brief Bytes a pty keeps for the host while no tool reads it. */
inline constexpr std::size_t pty_outgoing_limit = 64 * 1024;

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
 * @brief A host pty bridged to a byte link: host tools open path() like a serial port. Owns both
 *        ends of the pty and keeps the slave open, so the master never reads EIO or polls
 *        POLLHUP while no tool has the pty open. Through that slave it clears TIOCEXCL when the
 *        link's device detaches or attaches, as a re-enumerated device is a fresh port. pump() and
 * discard() run on the thread that owns the engine, the others on any thread.
 */
class pty_bridge
{
public:
  /**
   * @brief Opens a raw pty for @p name over @p link, its master non-blocking.
   * @throws std::system_error when no pty can be opened.
   */
  pty_bridge(std::string name, std::unique_ptr<pty_link> link);

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
   * @brief Moves bytes both ways: take_input() into the link, then flush().
   * @return The warning of flush().
   */
  auto pump() -> std::optional<std::string>;

  /** @brief Reads what the host wrote, until the pty has no more. */
  [[nodiscard]] auto take_input() -> std::vector<std::uint8_t>;

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
  std::string                        name_;
  unique_fd                          master_;
  unique_fd                          slave_;
  std::string                        path_;
  std::shared_ptr<pty_link>          link_;
  std::vector<std::uint8_t>          outgoing_;
  std::shared_ptr<std::atomic<bool>> wants_write_ = std::make_shared<std::atomic<bool>>(false);
  bool                               overflowed_  = false;
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
   */
  cdc_pty_link(std::shared_ptr<periph::usb::cdc_hub> hub, std::size_t function);

  void               send(std::span<std::uint8_t const> bytes) override;
  void               drain(std::vector<std::uint8_t>& out) override;
  [[nodiscard]] auto device_changed() -> bool override;

private:
  std::shared_ptr<periph::usb::cdc_hub> hub_;
  std::size_t                           function_;
  std::uint64_t                         seen_{};
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
