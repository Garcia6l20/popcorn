#pragma once

#include "access_op.hpp"
#include "alive.hpp"
#include "gil_locks.hpp"
#include "pdm_queues.hpp"

#include <popcorn/periph/stm32/pdm.hpp>

#include <reflex/py.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace popcorn::bind
{
namespace py = reflex::py;

/** @brief One sample a digital filter wrote to its RXFIFO, or tried to. */
struct[[= py::rename{"PdmSampleRecord"}]] pdm_sample_record
{
  /** @brief Signed 24-bit sample. */
  [[= py::readonly]] std::int32_t pcm{};
  /** @brief Virtual time of the write, in picoseconds since reset. */
  [[= py::readonly]] std::uint64_t at_ps{};
  /** @brief The RXFIFO was full: DOVRF set and the sample lost. */
  [[= py::readonly]] bool overrun{};
  /** @brief False when the ADF's sound activity detector kept it out of the RXFIFO. */
  [[= py::readonly]] bool captured{};
};

/**
 * @brief The sensor on one clock edge of one MDF or ADF serial interface, fed from Python.
 *        Valid while the machine lives, every method raises ConfigError after.
 */
class[[= py::rename{"PdmLink"}]] pdm_link
{
public:
  /**
   * @brief Wires a queue source to an interface edge, replacing the board's attachment.
   * @param port The MDF or ADF.
   * @param sitf Serial interface index, below port.sitfs().
   * @param edge Clock edge.
   * @param alive Cleared when the machine is destroyed.
   */
  [[= py::skip]] pdm_link(
      periph::stm32::pdm_port& port,
      std::size_t              sitf,
      periph::stm32::pdm_edge  edge,
      detail::alive_flag       alive);

  /** @brief PCM samples queued and not reached yet, the one being sent included. */
  [[= py::doc{"PCM samples queued and not reached yet."}]] [[nodiscard]] auto queued_samples() const
      -> std::uint64_t;
  /** @brief Raw bits queued and not clocked yet. */
  [[= py::doc{"Raw bits queued and not clocked yet."}]] [[nodiscard]] auto queued_bits() const
      -> std::uint64_t;
  /** @brief Bits clocked from an empty queue since power-on or the last discard. */
  [[= py::doc{"Bits clocked from an empty queue."}]] [[nodiscard]] auto idle_bits() const
      -> std::uint64_t;

  /** @brief queued_samples() as an op of MachineHandle.submit_ops(). */
  [[= py::doc{"Op of queued_samples."}]] [[nodiscard]] auto op_queued_samples() const -> access_op;
  /** @brief queued_bits() as an op of MachineHandle.submit_ops(). */
  [[= py::doc{"Op of queued_bits."}]] [[nodiscard]] auto op_queued_bits() const -> access_op;
  /** @brief idle_bits() as an op of MachineHandle.submit_ops(). */
  [[= py::doc{"Op of idle_bits."}]] [[nodiscard]] auto op_idle_bits() const -> access_op;

  /**
   * @brief A command of MachineHandle.submit_ops() that clocks the interfaces up to its time,
   *        then queues PCM samples.
   * @param samples Samples, -32768 to 32767.
   * @param hz Sample rate, at least 1.
   * @throws config_error when @p hz is 0, before anything is posted.
   */
  [[= py::doc{
      "Command queueing PCM samples at hz, after settling the interfaces."}]] [[nodiscard]] auto
      command_write_pcm(std::vector<std::int16_t> samples, std::uint32_t hz) const -> access_op;

  /**
   * @brief A command of MachineHandle.submit_ops() that clocks the interfaces up to its time,
   *        then queues raw bits, MSB of the first byte first.
   * @param data Packed bits.
   * @param bits Bits taken from @p data.
   * @throws config_error when @p bits exceeds 8 per byte of @p data, before anything is posted.
   */
  [[= py::doc{
      "Command queueing raw bits, MSB first, after settling the interfaces."}]] [[nodiscard]] auto
      command_write_pdm(reflex::py::nb::bytes const& data, std::uint64_t bits) const -> access_op;

  /** @brief Drops the queued segments, resets the modulator and clears the idle count. */
  [[= py::doc{"Drop queued input, reset the modulator, clear the idle count."}]] void discard();

private:
  [[nodiscard]] auto source() const -> detail::pdm_queue_source&;

  std::shared_ptr<detail::pdm_queue_source> source_;
  periph::stm32::pdm_port*                  port_;
  detail::alive_flag                        alive_;
};

/**
 * @brief The samples one MDF or ADF digital filter writes, kept for Python. Valid while the
 *        machine lives, every method raises ConfigError after.
 */
class[[= py::rename{"PdmTapLink"}]] pdm_tap_link
{
public:
  /**
   * @brief Wires a queue tap to a filter, replacing the board's attachment.
   * @param port The MDF or ADF.
   * @param filter Digital filter index, below port.sitfs().
   * @param resets The machine's reset count, which tags each kept sample.
   * @param alive Cleared when the machine is destroyed.
   * @param host The machine's host, which predicates run on.
   */
  [[= py::skip]] pdm_tap_link(
      periph::stm32::pdm_port& port,
      std::size_t              filter,
      core::reset_counter      resets,
      detail::alive_flag       alive,
      detail::host_ref         host);

  /**
   * @brief available() and the machine's reset count, read in one hold of the link lock, so
   *        every sample counted carries a reset count at most the one returned. Passed to
   *        drain_until(), that count keeps a reset run after this call from widening the drain.
   */
  [[= py::doc{"available() and the reset count, read together: no counted sample has a later "
              "reset count."}]] [[nodiscard]] auto available_tagged() const
      -> std::pair<std::uint64_t, std::uint64_t>;

  /** @brief Samples kept and not drained yet. */
  [[= py::doc{"Samples kept and not drained yet."}]] [[nodiscard]] auto available() const
      -> std::uint64_t;
  /** @brief Samples dropped because the queue held its limit. */
  [[= py::doc{"Samples dropped because the queue was full."}]] [[nodiscard]] auto dropped() const
      -> std::uint64_t;
  /** @brief available() as an op of MachineHandle.submit_ops(). */
  [[= py::doc{"Op of available."}]] [[nodiscard]] auto op_available() const -> access_op;
  /** @brief dropped() as an op of MachineHandle.submit_ops(). */
  [[= py::doc{"Op of dropped."}]] [[nodiscard]] auto op_dropped() const -> access_op;

  /**
   * @brief Takes the oldest kept samples.
   * @param count Most samples taken.
   */
  [[= py::doc{"Take at most count of the oldest samples."}]] auto drain(std::uint64_t count)
      -> std::vector<pdm_sample_record>;
  /**
   * @brief Takes the oldest kept samples written by @p until_ps, or before the reset it counts
   *        from, in one hold of the link lock: later samples stay queued.
   * @param count Most samples taken.
   * @param until_ps Latest write time taken, in picoseconds since the reset @p resets names.
   * @param resets The machine's reset count @p until_ps belongs to, the count now when none.
   */
  [[= py::doc{"Take at most count of the oldest samples written by until_ps, or before the "
              "reset resets counts from."}]] auto
      drain_until(std::uint64_t count, std::uint64_t until_ps, std::optional<std::uint64_t> resets)
          -> std::vector<pdm_sample_record>;

  /** @brief A predicate true once available() reaches @p count. */
  [[= py::doc{"Predicate: available() reaches count."}]] [[nodiscard]] auto
      predicate_available(std::uint64_t count) const -> native_predicate;

  /** @brief Drops the kept samples and clears the drop count. */
  [[= py::doc{"Drop kept samples and clear the drop count."}]] void discard();

  /**
   * @brief Sets the most samples the queue keeps, pdm_sample_limit by default.
   * @param limit Samples, at least 1.
   * @throws config_error when @p limit is 0.
   */
  [[= py::doc{"Set the most samples the queue keeps, for tests."}]] void
      set_limit(std::uint64_t limit);

private:
  [[nodiscard]] auto tap() const -> detail::pdm_queue_tap&;

  std::shared_ptr<detail::pdm_queue_tap> tap_;
  detail::alive_flag                     alive_;
  detail::host_ref                       host_;
};
}
