#pragma once

#include "../periph/stm32/sigma_delta.hpp"
#include "link_queues.hpp"

#include <popcorn/core/link_guard.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/periph/stm32/pdm.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <utility>
#include <vector>

namespace popcorn::bind::detail
{
/**
 * @brief Samples a filter tap keeps before dropping the oldest: 16.4 s at 16 kHz, 4 MiB at 16
 *        bytes a sample, 28 MiB for the 7 filters of an STM32U575.
 */
inline constexpr std::uint64_t pdm_sample_limit = std::uint64_t{1} << 18;

/** @brief One sample a digital filter wrote to its RXFIFO, or tried to, as its tap keeps it. */
struct queued_pdm_sample
{
  /** @brief Signed 24-bit sample. */
  std::int32_t pcm{};
  /** @brief The RXFIFO was full: DOVRF set and the sample lost. */
  bool overrun{};
  /** @brief False when the ADF's sound activity detector kept it out of the RXFIFO. */
  bool captured{};
  /** @brief Virtual time of the write, in picoseconds since reset. */
  std::uint64_t at_ps{};
  /** @brief The machine's reset count when it was written. */
  std::uint64_t resets{};
};

/**
 * @brief A sensor fed from Python: a queue of segments, each 16-bit PCM samples at a rate in Hz,
 *        which a 2nd-order modulator turns into bits, or packed raw bits. Bit k of a PCM segment
 *        carries sample `floor(k x hz x divider / kernel_hz)` of it. An empty queue feeds the
 *        modulator 0, silence, and counts the bits as idle. The model clocks it on the machine
 *        thread, the binding writes and reads it, each under its link lock.
 */
class pdm_queue_source final : public periph::stm32::pdm_source, public link_lockable
{
public:
  void clock(std::span<std::uint8_t> out, periph::stm32::bit_rate rate) override
  {
    auto const held = lock();
    auto       bit  = out.begin();
    while(bit != out.end())
    {
      if(segments_.empty())
      {
        idle_ += static_cast<std::uint64_t>(out.end() - bit);
        for(; bit != out.end(); ++bit)
        {
          *bit = periph::stm32::modulate(modulator_, 0) ? 1 : 0;
        }
        break;
      }
      auto& segment = segments_.front();
      bit           = segment.samples.empty() ? raw(held, segment, bit, out.end())
                                              : pcm(held, segment, bit, out.end(), rate);
      if(segment.done())
      {
        segments_.pop_front();
      }
    }
  }

  /**
   * @brief Queues PCM samples behind the queued segments.
   * @param held The queue's lock.
   * @param samples Samples, -32768 to 32767, full scale 1.0.
   * @param hz Sample rate, at least 1.
   */
  void write_pcm(
      core::link_guard const&       held,
      std::span<std::int16_t const> samples,
      std::uint32_t                 hz) pre(held.guards(mutex_)) pre(hz != 0)
  {
    if(samples.empty())
    {
      return;
    }
    segments_.push_back({
        .samples = {samples.begin(), samples.end()},
          .hz = hz
    });
    samples_ += samples.size();
  }

  /**
   * @brief Queues raw bits behind the queued segments, MSB of the first byte first.
   * @param held The queue's lock.
   * @param data Packed bits.
   * @param bits Bits taken from @p data, at most 8 per byte.
   */
  void write_pdm(
      core::link_guard const&       held,
      std::span<std::uint8_t const> data,
      std::uint64_t bits) pre(held.guards(mutex_)) pre(bits <= 8 * std::uint64_t{data.size()})
  {
    if(bits == 0)
    {
      return;
    }
    segments_.push_back({
        .bytes = {data.begin(), data.begin() + (bits + 7) / 8},
          .bits = bits
    });
    bits_ += bits;
  }

  /** @brief PCM samples queued and not reached yet, the one being sent included. */
  [[nodiscard]] auto queued_samples(core::link_guard const& held) const noexcept -> std::uint64_t
      pre(held.guards(mutex_))
  {
    return samples_;
  }

  /** @brief Raw bits queued and not clocked yet. */
  [[nodiscard]] auto queued_bits(core::link_guard const& held) const noexcept -> std::uint64_t
      pre(held.guards(mutex_))
  {
    return bits_;
  }

  /** @brief Bits clocked from an empty queue. */
  [[nodiscard]] auto idle_bits(core::link_guard const& held) const noexcept -> std::uint64_t
      pre(held.guards(mutex_))
  {
    return idle_;
  }

  /** @brief Drops every queued segment, resets the modulator and clears the idle count. */
  void discard(core::link_guard const& held) pre(held.guards(mutex_))
  {
    segments_.clear();
    modulator_ = {};
    samples_   = 0;
    bits_      = 0;
    idle_      = 0;
  }

private:
  struct segment
  {
    std::vector<std::int16_t> samples{};
    std::uint32_t             hz{};
    std::size_t               index{};
    std::uint64_t             phase{};
    std::uint64_t             kernel_hz{};
    std::vector<std::uint8_t> bytes{};
    std::uint64_t             bits{};
    std::uint64_t             next{};

    [[nodiscard]] auto done() const noexcept -> bool
    {
      return samples.empty() ? next == bits : index >= samples.size();
    }
  };

  using bit_iterator = std::span<std::uint8_t>::iterator;

  auto raw(core::link_guard const&, segment& from, bit_iterator bit, bit_iterator end)
      -> bit_iterator
  {
    for(; bit != end and from.next < from.bits; ++bit, ++from.next)
    {
      *bit = (from.bytes[from.next / 8] >> (7 - from.next % 8)) & 1u;
      --bits_;
    }
    return bit;
  }

  auto
      pcm(core::link_guard const&,
          segment&                from,
          bit_iterator            bit,
          bit_iterator            end,
          periph::stm32::bit_rate rate) -> bit_iterator
  {
    if(from.kernel_hz != rate.kernel_hz)
    {
      from.phase =
          from.kernel_hz == 0
              ? 0
              : static_cast<std::uint64_t>(
                    static_cast<unsigned __int128>(from.phase) * rate.kernel_hz / from.kernel_hz);
      from.kernel_hz = rate.kernel_hz;
    }
    auto const step = std::uint64_t{from.hz} * rate.divider;
    for(; bit != end and from.index < from.samples.size(); ++bit)
    {
      *bit = periph::stm32::modulate(modulator_, from.samples[from.index]) ? 1 : 0;
      from.phase += step;
      if(from.phase >= rate.kernel_hz)
      {
        auto const passed =
            std::min<std::uint64_t>(from.phase / rate.kernel_hz, from.samples.size() - from.index);
        from.index += static_cast<std::size_t>(passed);
        from.phase %= rate.kernel_hz;
        samples_ -= passed;
      }
    }
    return bit;
  }

  std::deque<segment>            segments_;
  periph::stm32::modulator_state modulator_{};
  std::uint64_t                  samples_{};
  std::uint64_t                  bits_{};
  std::uint64_t                  idle_{};
};

/**
 * @brief The samples one digital filter writes, kept for Python, at most `limit` of them: a full
 *        queue drops its oldest sample and counts it. The model fills it on the machine thread,
 *        the binding drains it, each under its link lock.
 */
class pdm_queue_tap final : public periph::stm32::pdm_tap, public tagged_queue<queued_pdm_sample>
{
public:
  /**
   * @brief Creates an empty tap.
   * @param resets The machine's reset count, which tags each sample: a sample from before the
   *        last reset is taken whatever the time bound of take().
   */
  explicit pdm_queue_tap(core::reset_counter resets = {})
      : tagged_queue{std::move(resets), pdm_sample_limit}
  {}

  void wrote(std::int32_t pcm, vtime at, bool dropped, bool captured) override
  {
    auto const held = lock();
    push(held, {.pcm = pcm, .overrun = dropped, .captured = captured, .at_ps = at.ps});
  }

  /** @brief Drops the kept samples and clears the drop count. */
  void discard(core::link_guard const& held) pre(held.guards(mutex_))
  {
    items_.clear();
    dropped_ = 0;
  }
};
}
