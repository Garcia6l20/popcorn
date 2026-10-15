#pragma once

#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
/** @brief Audio block of an SAI. */
enum class audio_block : std::uint8_t
{
  /** @brief Block A. */
  a,
  /** @brief Block B. */
  b,
};

/** @brief Most slots an audio frame has, NBSLOT + 1 at its maximum. */
inline constexpr std::size_t audio_max_slots = 16;

/**
 * @brief Layout of one audio frame, as the block was configured for it: the slot words and
 *        where each slot sits on the bus, in bit clocks from the frame sync edge.
 */
struct audio_format
{
  /** @brief Data size in bits, 8 to 32: the bits of each slot word that carry data. */
  std::uint8_t data_bits{};
  /** @brief Active slots, bit n for slot n, SLOTEN. */
  std::uint16_t active_slots{};
  /** @brief Mono mode with two slots: slot 1 repeats slot 0 when sending, and is dropped when
   * receiving. */
  bool mono{};
  /** @brief Bit clocks per frame, FRL + 1. */
  std::uint16_t frame_bits{};
  /** @brief Bits per slot, SLOTSZ resolved: the data size, 16 or 32. */
  std::uint8_t slot_bits{};
  /** @brief First data bit within a slot, FBOFF: the data occupies bits FBOFF to
   * FBOFF + data_bits - 1 of the slot. */
  std::uint8_t first_bit_offset{};
  /** @brief Frame sync marks the start of frame and the channel side, FSDEF: the second half of
   * the slots starts at half the frame. */
  bool fs_definition{};
  /** @brief Frame sync asserted one bit clock before slot 0, FSOFF. */
  bool fs_offset{};
  /** @brief Frame sync active high, FSPOL. */
  bool fs_active_high{};
  /** @brief Frame sync active level length in bit clocks, FSALL + 1. */
  std::uint8_t fs_active_bits{};
  /** @brief Slots per frame, NBSLOT + 1: the slot word count. */
  std::uint8_t slot_count{};
  /** @brief Start of each slot in bit clocks from the frame sync edge, FSOFF included. The first
   * slot_count entries are used, the others are 0. A start at or past frame_bits is a slot that
   * does not fit in the frame and carries no data. */
  std::array<std::uint16_t, audio_max_slots> slot_starts{};

  /** @brief Member-wise equality. */
  friend auto operator==(audio_format const&, audio_format const&) -> bool = default;
};

/**
 * @brief The far end of an SAI audio block, such as a codec. Sees one audio frame at a time. A
 *        frame carries one word per slot, NBSLOT + 1 of them, data bits right-aligned. Called
 *        from the SAI model's event: it must not run the machine. Like the host queue of a UART,
 *        an endpoint is outside the machine: restoring a snapshot does not rewind it, and a
 *        source goes on from where it is.
 */
class POPCORN_API audio_endpoint
{
public:
  /** @brief Creates the endpoint. */
  audio_endpoint() = default;
  /** @brief Not copyable. */
  audio_endpoint(audio_endpoint const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(audio_endpoint const&) -> audio_endpoint& = delete;
  /** @brief Not movable. */
  audio_endpoint(audio_endpoint&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(audio_endpoint&&) -> audio_endpoint& = delete;
  /** @brief Destroys the endpoint. */
  virtual ~audio_endpoint();

  /**
   * @brief A transmitting block sent a frame. Called when the frame ends.
   * @param slots One word per slot. Inactive slots and slots sent while the FIFO underran are 0.
   * @param format Layout of the words.
   * @param at Virtual time the frame started.
   * @param end Virtual time the frame ended, the time of the call.
   */
  virtual void transmitted(
      std::span<std::uint32_t const> slots,
      audio_format                   format,
      vtime                          at,
      vtime                          end) = 0;

  /**
   * @brief A receiving block takes a frame. Called when the frame's first active slot ends,
   *        before that slot's word is stored, at the latest at the frame end.
   * @param slots One word per slot to fill, all 0 on entry. Words of inactive slots are ignored,
   *        bits above the data size dropped.
   * @param format Layout of the words.
   * @param at Virtual time the frame started.
   */
  virtual void receive(std::span<std::uint32_t> slots, audio_format format, vtime at) = 0;
};

/** @brief What a board wires to one SAI audio block. */
struct audio_attachment
{
  /** @brief Far end of the block, empty for none: transmitted frames are then dropped and
   * received frames are all 0. */
  std::shared_ptr<audio_endpoint> endpoint{};
  /** @brief Frame time multiplier, 1 for the configured rate. The model logs it once when the
   * block starts with a value above 1. 0 counts as 1. */
  std::uint32_t decimation{1};
  /** @brief Frame rate in Hz of the external clock master of a slave block in asynchronous mode,
   * 0 for none: such a block then sees no frame. Unused by master and synchronous blocks. The
   * clock it describes already runs: its phase is taken at the block's first enable. */
  std::uint32_t external_frame_hz{};
  /** @brief A bus device drives the external clock of the block at run time. A slave block
   * enabled with that clock stopped then waits for it without logging. */
  bool clock_wired{};
};

/**
 * @brief The firmware side of an SAI as a board sees it, implemented by the SAI model. Reach it
 *        with `dynamic_cast<audio_port*>(machine.model("SAI1"))`.
 */
class POPCORN_API audio_port
{
public:
  /** @brief Creates the interface. */
  audio_port() = default;
  /** @brief Not copyable. */
  audio_port(audio_port const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(audio_port const&) -> audio_port& = delete;
  /** @brief Not movable. */
  audio_port(audio_port&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(audio_port&&) -> audio_port& = delete;

  /**
   * @brief Wires an audio block, replacing the board's attachment. The endpoint and the
   *        decimation are not saved in snapshots, a decimation change takes effect at the block's
   *        next frame. The external frame rate is set as set_external_clock() sets it, except
   *        that a clock started from stopped takes its phase at the block's next enable, or at
   *        once when the block is enabled and waits for a clock.
   * @param block Audio block.
   * @param attachment Endpoint, decimation and external frame rate.
   */
  virtual void attach(audio_block block, audio_attachment attachment) = 0;

  /**
   * @brief Sets the frame rate of the external clock master of a slave block, as the board
   *        drives it. Board state: a snapshot restores it, a reset other than power-on or
   *        brown-out keeps it in phase, those two return it to the attachment's rate. A running
   *        clock changes rate or stops at the end of its frame in progress, so a stop and a
   *        start within one frame keep its phase. A stopped clock starts now: an enabled
   *        asynchronous slave block starts its first frame at once, one enabled later starts on
   *        the clock's next frame boundary.
   * @param block Audio block.
   * @param frame_hz Frames per second, 0 to stop.
   */
  virtual void set_external_clock(audio_block block, std::uint32_t frame_hz) = 0;

  /**
   * @brief The external frame rate set last.
   * @param block Audio block.
   * @return Frames per second, 0 when stopped.
   */
  [[nodiscard]] virtual auto external_clock(audio_block block) const -> std::uint32_t = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  virtual ~audio_port();
};

/** @brief One frame an audio_capture_endpoint kept. */
struct captured_audio_frame
{
  /** @brief One word per slot. */
  std::vector<std::uint32_t> slots{};
  /** @brief Layout of the words. */
  audio_format format{};
  /** @brief Virtual time the frame started. */
  vtime at{};

  /** @brief Member-wise equality. */
  friend auto operator==(captured_audio_frame const&, captured_audio_frame const&)
      -> bool = default;
};

/** @brief Keeps every transmitted frame with its time, and receives silence. */
class POPCORN_API audio_capture_endpoint final : public audio_endpoint
{
public:
  /** @brief Frames received, in order. */
  std::vector<captured_audio_frame> frames;

  void transmitted(std::span<std::uint32_t const> slots, audio_format format, vtime at, vtime)
      override
  {
    frames.push_back({
        .slots = {slots.begin(), slots.end()},
          .format = format, .at = at
    });
  }

  void receive(std::span<std::uint32_t>, audio_format, vtime) override
  {}

  /** @brief Destroys the endpoint. Declared out-of-line: the key function pins the vtable. */
  ~audio_capture_endpoint() override;
};

/** @brief Serves given words to a receiving block in slot order, then 0. Drops what it is sent. */
class POPCORN_API audio_source_endpoint final : public audio_endpoint
{
public:
  /**
   * @brief Serves @p words.
   * @param words Words in the order slots take them, inactive slots included.
   */
  explicit audio_source_endpoint(std::vector<std::uint32_t> words) : words_{std::move(words)}
  {}

  void transmitted(std::span<std::uint32_t const>, audio_format, vtime, vtime) override
  {}

  void receive(std::span<std::uint32_t> slots, audio_format, vtime) override
  {
    for(auto& slot : slots)
    {
      slot = next_ < words_.size() ? words_[next_++] : 0;
    }
  }

  /** @brief Words served so far. */
  [[nodiscard]] auto served() const -> std::size_t
  {
    return next_;
  }

  /** @brief Destroys the endpoint. Declared out-of-line: the key function pins the vtable. */
  ~audio_source_endpoint() override;

private:
  std::vector<std::uint32_t> words_;
  std::size_t                next_{};
};
}
