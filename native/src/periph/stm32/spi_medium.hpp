#pragma once

#include "medium_tap_queue.hpp"
#include "spi_link_port.hpp"

#include <popcorn/core/member_link.hpp>
#include <popcorn/export.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace popcorn::periph::stm32
{
/** @brief One frame of an SPI medium as its tap shows it, stamps in system time. */
struct spi_tap_frame
{
  /** @brief Index of the slave among the medium's slaves. */
  std::size_t slave{};
  /** @brief Unit number the master proxy gave the frame. */
  std::uint64_t unit{};
  /** @brief Chip select pin of the slave on the master, empty for the SS output. */
  std::optional<std::uint8_t> cs_pin;
  /** @brief End of the master's frame, the post's stamp t. */
  core::sys_time at{};
  /** @brief Stamp of the slave's reply, t + L + frame time, or the time it was dropped. */
  core::sys_time reply{};
  /** @brief Frame the master sent, in wire order. */
  std::uint32_t mosi{};
  /** @brief Frame the slave returned, in wire order, all ones masked to @ref bits when dropped. */
  std::uint32_t miso{};
  /** @brief Frame size. */
  std::uint8_t bits{};
  /** @brief Whether the slave never clocked the frame and gave the undriven reply. */
  bool dropped{};

  /** @brief Member-wise equality. */
  friend auto operator==(spi_tap_frame const&, spi_tap_frame const&) -> bool = default;
};

/**
 * @brief Frames of an SPI medium for observers, any thread. Holds at most a capacity of frames,
 *        the oldest dropped and counted. A frame enters when the master's drain takes its reply,
 *        in drain order.
 */
using spi_tap_queue = medium_tap_queue<spi_tap_frame>;

/** @brief A slave of an SPI medium: its member, its SPI and how the master selects it. */
struct spi_slave_end
{
  /** @brief Member index of the slave. */
  std::size_t member{};
  /** @brief The slave's SPI, which must outlive the medium's drains. */
  spi_link_port* port{};
  /** @brief GPIO port of the chip select on the master, such as `GPIOA`. */
  std::string cs_port;
  /** @brief Chip select pin on the master, active low, or empty for the master's SS output. */
  std::optional<std::uint8_t> cs_pin;
  /** @brief Delivery lane of the master's posts on the slave, 1 and up. */
  std::uint32_t forth_lane{};
  /** @brief Delivery lane of the slave's replies on the master, 1 and up, one per slave. */
  std::uint32_t back_lane{};
};

namespace detail
{
struct spi_pair;
}

/**
 * @brief An SPI bus across members of a linked system: one master SPI, one slave SPI per slave
 *        on other members, each slave selected by a chip select pin of the master or by its SS
 *        output. Each slave gets two links, master to slave (M->S) and slave to master (S->M),
 *        each of latency L.
 *
 *        The master side is a link target per slave, selected and ANDed by the master's SPI
 *        model as a local device. Each chip select edge is posted with its stamp, each frame
 *        with the stamp t of its end and a unit number, and the frame is left pending. The
 *        slave's frame runs over [t + L, t + L + fp], fp the master's frame time, its reply is
 *        stamped t + L + fp and completes the master's frame at t + 2L + fp, on the slave's
 *        S->M lane. SCK pauses for 2L between frames. TXC, EOT, RXP and the SS release follow
 *        the completion. At equal stamps the master's own events complete first, then the
 *        replies by lane. Chip select edges reach the slave at their stamp plus L.
 *
 *        Each link hands a post at a drain once its far-side start, stamp plus L, lies before
 *        the drain's next barrier, and carries it to a later drain otherwise, so a port holds
 *        no entry due more than one window ahead. A post handed at a drain is late by up to one
 *        sender instruction period plus the receiver's own stop past the barrier, and keeps its
 *        own instruction count on the receiver.
 *
 *        Every frame posted gets exactly one reply. A frame the slave never clocks, dropped by a
 *        reset or cut by a deselect, gets all ones masked to its size. A slave reset at t_r drops
 *        the frames of stamp below t_r still in the M->S link, answered at the later of t_r and
 *        their far-side end, and applies the chip select edges still in the link at the later of
 *        t_r and their stamp plus L. A master member reset at t_m posts a deselect at t_m when the
 *        chip select was selected, forgets the master's pending frames and mutes the master side
 *        until the next window, which posts a select at t_m when the chip select reads low. Replies
 *        of stamp below t_m still in the S->M link are dropped, later replies find no frame and are
 *        counted as refused.
 *
 *        The links save the proxies' state in their own bytes, each starting with a kind tag:
 *        `spi_forth` for M->S, `spi_back` for S->M. A link refuses another kind's bytes with
 *        config_error.
 */
class POPCORN_API spi_medium
{
public:
  /**
   * @param master Member index of the master.
   * @param master_port The master's SPI, which must outlive the medium's drains.
   * @param latency L, above 0.
   * @param slaves The slaves, at least one, none on the master's member.
   * @param tap_capacity Frames the tap keeps.
   * @throws std::invalid_argument When @p slaves is empty, a slave is on the master's member or
   *         has no port, or a lane is 0 or a reply lane is shared by two slaves.
   */
  spi_medium(
      std::size_t                master,
      spi_link_port&             master_port,
      core::sys_time             latency,
      std::vector<spi_slave_end> slaves,
      std::size_t                tap_capacity = std::size_t{1} << 20) pre(latency.ps > 0);
  /** @brief Not copyable. */
  spi_medium(spi_medium const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(spi_medium const&) -> spi_medium& = delete;
  /** @brief Not movable. */
  spi_medium(spi_medium&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(spi_medium&&) -> spi_medium& = delete;
  /** @brief Destroys the medium. */
  ~spi_medium();

  /**
   * @brief The links, two per slave in slave order, M->S then S->M. Each link and each proxy
   *        attached to an SPI keeps the state of its slave alive, so they outlive the medium.
   */
  [[nodiscard]] auto links() const -> std::vector<std::shared_ptr<core::member_link>>;

  /**
   * @brief Attaches the master-side proxies to the master's SPI, muted until their first window,
   *        which posts the select of a chip select reading low. Called with the master's host
   *        access, before the system starts.
   * @throws config_error As spi_link_port::attach_target(). Nothing stays attached then.
   */
  void attach_master();

  /**
   * @brief Attaches the proxy of slave @p index to its SPI. Called with that slave's host access,
   *        before the system starts.
   * @throws config_error As spi_link_port::attach_controller().
   */
  void attach_slave(std::size_t index);

  /**
   * @brief Completes every frame the master still waits for with all ones at the master's
   *        current time, then detaches the master-side proxies. Called with the master's host
   *        access. Does nothing for a proxy not attached.
   */
  void give_back_master() noexcept;

  /**
   * @brief Detaches the proxy of slave @p index, then answers every frame the slave owes with
   *        all ones masked to its size, stamped at its far-side end, or at the later of that end
   *        and the slave reset that dropped it. Called with that slave's host access. A frame the
   *        link hands to that slave afterwards gets the same answer. Chip select edges handed
   *        afterwards are discarded.
   */
  void give_back_slave(std::size_t index) noexcept pre(index < slave_count());

  /** @brief The tap, any thread. */
  [[nodiscard]] auto tap() noexcept -> spi_tap_queue&;

  /** @brief Number of slaves. */
  [[nodiscard]] auto slave_count() const noexcept -> std::size_t;

  /**
   * @brief Replies the master side refused so far: no pending frame for their unit. Read with
   *        the system stopped.
   */
  [[nodiscard]] auto refused() const -> std::uint64_t;

  /** @brief Units slave @p index owes a reply for. Read with the system stopped. */
  [[nodiscard]] auto owed(std::size_t index) const -> std::size_t;

  /** @brief Most units slave @p index owed at once since the medium was built. */
  [[nodiscard]] auto owed_peak(std::size_t index) const -> std::size_t;

  /** @brief Frames the master waits for on slave @p index. Read with the system stopped. */
  [[nodiscard]] auto tickets(std::size_t index) const -> std::size_t;

  /** @brief Most frames the master waited for on slave @p index at once. */
  [[nodiscard]] auto tickets_peak(std::size_t index) const -> std::size_t;

private:
  spi_link_port&                                 master_port_;
  std::shared_ptr<spi_tap_queue>                 tap_;
  std::vector<std::shared_ptr<detail::spi_pair>> pairs_;
};
}
