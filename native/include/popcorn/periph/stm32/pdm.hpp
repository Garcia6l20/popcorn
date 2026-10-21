#pragma once

#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace popcorn::periph::stm32
{
/** @brief Edge of a serial interface clock on which a sensor drives its data line. */
enum class pdm_edge : std::uint8_t
{
  /** @brief Rising edge, the bsx_r stream of RM0456 39.4.6. */
  rising,
  /** @brief Falling edge, the bsx_f stream. */
  falling,
};

/**
 * @brief Rate of a serial interface bit clock: one bit per edge of its kind every `divider`
 *        kernel clock cycles, `divider` = (PROCDIV + 1) x (CCKDIV + 1).
 */
struct bit_rate
{
  /** @brief Kernel clock of the MDF or ADF in Hz. */
  std::uint64_t kernel_hz{};
  /** @brief Kernel clock cycles per bit clock period. */
  std::uint64_t divider{};

  /** @brief Member-wise equality. */
  friend auto operator==(bit_rate const&, bit_rate const&) -> bool = default;
};

/**
 * @brief A sensor on one edge of one serial interface data line. Called from the model on the
 *        machine thread: it must not run the machine. Like the far end of a UART, a source is
 *        outside the machine: restoring a snapshot does not rewind it.
 */
class POPCORN_API pdm_source
{
public:
  /** @brief Creates the source. */
  pdm_source() = default;
  /** @brief Not copyable. */
  pdm_source(pdm_source const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(pdm_source const&) -> pdm_source& = delete;
  /** @brief Not movable. */
  pdm_source(pdm_source&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(pdm_source&&) -> pdm_source& = delete;
  /** @brief Destroys the source. */
  virtual ~pdm_source();

  /**
   * @brief Writes the next @p out.size() bits the sensor drives, oldest first, 0 or 1 each.
   * @param out Bits to fill.
   * @param rate Bit clock the interface ran at for these bits.
   */
  virtual void clock(std::span<std::uint8_t> out, bit_rate rate) = 0;
};

/**
 * @brief A sink of the samples a digital filter writes to its RXFIFO. Called from the model
 *        on the machine thread: it must not run the machine.
 */
class POPCORN_API pdm_tap
{
public:
  /** @brief Creates the tap. */
  pdm_tap() = default;
  /** @brief Not copyable. */
  pdm_tap(pdm_tap const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(pdm_tap const&) -> pdm_tap& = delete;
  /** @brief Not movable. */
  pdm_tap(pdm_tap&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(pdm_tap&&) -> pdm_tap& = delete;
  /** @brief Destroys the tap. */
  virtual ~pdm_tap();

  /**
   * @brief The filter wrote a sample, or tried to.
   * @param pcm Signed 24-bit sample, PCM[23:0] as DFLTxDR bits 31:8 hold it.
   * @param at Virtual time of the write since the last reset, the base of `m.time`: the clock
   *        edge of the bit that completed it.
   * @param dropped The RXFIFO was full: DOVRF set and the sample lost.
   * @param captured Always true for the MDF. False when the ADF's sound activity detector kept
   *        the sample out of the RXFIFO.
   */
  virtual void wrote(std::int32_t pcm, vtime at, bool dropped, bool captured) = 0;
};

/**
 * @brief The board side of an MDF or ADF, implemented by its model. Reach it with
 *        `dynamic_cast<pdm_port*>(machine.model("MDF1"))`.
 */
class POPCORN_API pdm_port
{
public:
  /** @brief Creates the interface. */
  pdm_port() = default;
  /** @brief Not copyable. */
  pdm_port(pdm_port const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(pdm_port const&) -> pdm_port& = delete;
  /** @brief Not movable. */
  pdm_port(pdm_port&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(pdm_port&&) -> pdm_port& = delete;

  /**
   * @brief Wires a sensor to one edge of a serial interface, replacing the previous one. Not
   *        saved in snapshots. Call settle() first when the machine has run.
   * @param sitf Serial interface index, below sitfs().
   * @param edge Clock edge the sensor drives.
   * @param source Sensor, empty for none: the edge then carries the bits of a modulator held
   *        at 0, silence.
   * @throws std::out_of_range When @p sitf is not below sitfs().
   */
  virtual void attach(std::size_t sitf, pdm_edge edge, std::shared_ptr<pdm_source> source) = 0;

  /**
   * @brief Wires a sink to the samples a filter writes, replacing the previous one. Not saved.
   * @param filter Digital filter index, below sitfs().
   * @param tap Sink, empty for none.
   * @throws std::out_of_range When @p filter is not below sitfs().
   */
  virtual void attach_tap(std::size_t filter, std::shared_ptr<pdm_tap> tap) = 0;

  /** @brief Number of serial interfaces, equal to the number of digital filters, so it bounds
   *  both the interface and the filter indices. */
  [[nodiscard]] virtual auto sitfs() const -> std::size_t = 0;

  /**
   * @brief Clocks every serial interface up to the model's current virtual time, pulling from
   *        the sources every bit whose edge is at or before it. Called on the machine thread
   *        before a source's content changes, so edges before the change see the old content
   *        and later edges the new.
   */
  virtual void settle() = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~pdm_port() = default;
};
}
