#pragma once

#include <popcorn/core/trace.hpp>
#include <popcorn/core/types.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace popcorn::obs
{
/** @brief Options of a profile_sink. */
struct profile_options
{
  /** @brief Length in instructions of the sliding window profile_exception::burst counts entries
   * in, 0 for no burst count. */
  icount burst_window{};
  /** @brief Flat register ids whose CPU writes are kept one by one in profile_result::write_logs.
   */
  std::vector<std::uint16_t> logged_registers{};
  /** @brief Writes kept per logged register. Later writes are counted in
   * profile_write_log::dropped. */
  std::size_t write_log_limit{4096};
};

/** @brief Executions of the translated blocks starting at one pc. */
struct profile_block
{
  /** @brief Block start address. */
  address pc{};
  /** @brief Times a block started at pc. */
  std::uint64_t entries{};
  /** @brief Instructions of those blocks, each block counted in full, see profile_sink. */
  std::uint64_t instructions{};

  /** @brief Member-wise equality. */
  friend auto operator==(profile_block const&, profile_block const&) -> bool = default;
};

/** @brief Spans of instruction time with no block or exception record, such as a sleep. */
struct profile_gaps
{
  /** @brief Number of gaps. */
  std::uint64_t count{};
  /** @brief Instructions of time in them. */
  std::uint64_t total{};
  /** @brief Longest gap. */
  std::uint64_t longest{};

  /** @brief Member-wise equality. */
  friend auto operator==(profile_gaps const&, profile_gaps const&) -> bool = default;
};

/**
 * @brief Entries and time of one exception number. Times are instruction counts, sleeps
 *        included. Inclusive runs from the entry record to the matching exit record, self is
 *        inclusive less the inclusive time of the exceptions that preempted it.
 */
struct profile_exception
{
  /** @brief Entry records. */
  std::uint64_t entries{};
  /** @brief Inclusive time of the frames that exited. */
  std::uint64_t inclusive{};
  /** @brief Self time of the frames that exited. */
  std::uint64_t self{};
  /** @brief Longest inclusive time of one frame. */
  std::uint64_t longest{};
  /** @brief Most frames open at once at one of its entries, itself included. */
  std::uint32_t max_depth{};
  /** @brief Entries of other exceptions while this one was the innermost frame, by number. */
  std::map<std::uint16_t, std::uint64_t> preempted_by{};
  /** @brief Entries at the instruction count of the exit record before them, with no block
   * record between. An entry that wakes the core after a sleep on exit is a wake, not a tail
   * chain. */
  std::uint64_t tail_chained{};
  /** @brief Most entries inside one profile_options::burst_window, 0 without a window. */
  std::uint64_t burst{};
  /** @brief Frames a cut closed, see profile_sink. */
  std::uint64_t cut{};
  /** @brief Frames open when the result was taken. */
  std::uint64_t open{};
  /** @brief Gaps that this exception's entry ended. */
  profile_gaps wakes{};

  /** @brief Member-wise equality. */
  friend auto operator==(profile_exception const&, profile_exception const&) -> bool = default;
};

/** @brief Accesses to one register. */
struct profile_register
{
  /** @brief Flat register id, core::trace_id_overflow when the id did not fit the record. */
  std::uint16_t id{};
  /** @brief Address of the first access, which names the register when id overflowed. */
  address first_address{};
  /** @brief Reads. */
  std::uint64_t reads{};
  /** @brief Writes. */
  std::uint64_t writes{};
  /** @brief Writes of the value the previous write in the same segment stored. */
  std::uint64_t same_writes{};
  /** @brief Value of the last access, read or written. */
  std::uint32_t last_value{};

  /** @brief Member-wise equality. */
  friend auto operator==(profile_register const&, profile_register const&) -> bool = default;
};

/** @brief One CPU write to a logged register. */
struct profile_write
{
  /** @brief Instruction count of the write. */
  icount at{};
  /** @brief Value written. */
  std::uint32_t value{};
  /** @brief Cuts seen before the write, the time base @p at counts in. */
  std::uint64_t segment{};

  /** @brief Member-wise equality. */
  friend auto operator==(profile_write const&, profile_write const&) -> bool = default;
};

/** @brief CPU writes to one logged register, oldest first. */
struct profile_write_log
{
  /** @brief Flat register id. */
  std::uint16_t id{};
  /** @brief Writes kept, at most profile_options::write_log_limit. */
  std::vector<profile_write> writes{};
  /** @brief Writes past the limit. */
  std::uint64_t dropped{};

  /** @brief Member-wise equality. */
  friend auto operator==(profile_write_log const&, profile_write_log const&) -> bool = default;
};

/** @brief Aggregates of a profile_sink. */
struct profile_result
{
  /** @brief Instruction count the sink started at. */
  icount start{};
  /** @brief Instruction time the records covered, the sum of every forward step between
   * consecutive records, from start to the last record. Steps across a cut are left out. */
  std::uint64_t elapsed{};
  /** @brief Records received, time records excluded. */
  std::uint64_t records{};
  /** @brief Block records. */
  std::uint64_t blocks{};
  /** @brief Instructions of the block records, each counted in full, see profile_sink. */
  std::uint64_t instructions{};
  /** @brief Cuts, see profile_sink. */
  std::uint64_t cuts{};
  /** @brief Exit records with no open frame of their number on top of the stack. */
  std::uint64_t unmatched_exits{};
  /** @brief Blocks by pc, ascending. */
  std::vector<profile_block> code{};
  /** @brief Exceptions by number. */
  std::map<std::uint16_t, profile_exception> exceptions{};
  /** @brief Gaps ended by a block or an exit record. */
  profile_gaps other_gaps{};
  /** @brief Registers the CPU accessed, by id, overflowed ids by address after them. */
  std::vector<profile_register> cpu_registers{};
  /** @brief Registers reached by mmio records with pc 0, debugger and DMA accesses, ordered as
   * cpu_registers. */
  std::vector<profile_register> other_registers{};
  /** @brief Write logs, in profile_options::logged_registers order. */
  std::vector<profile_write_log> write_logs{};

  /** @brief Member-wise equality. */
  friend auto operator==(profile_result const&, profile_result const&) -> bool = default;
};

/**
 * @brief Trace sink keeping aggregates of block, mmio and irq records, with no file: per block
 *        pc, per exception number, per register, and the gaps between records. Time is the
 *        instruction count the records are stamped with.
 *
 * A block record carries the instruction count of the whole block. A block the engine leaves
 * after an MMIO access, a precise exit, counts its unrun tail too, so the instructions can exceed
 * the elapsed instruction time by those tails. elapsed follows the stamps.
 *
 * A gap runs from the end of the previous block or exception record, or from the start for the
 * first record, to the start of the next one, so a profile opened during a sleep counts the rest
 * of that sleep.
 *
 * A time record that moves the count back, or forward by at most 2^32 - 1 instructions, the most
 * a record's dt carries, is a rebase by a reset or a restore and cuts the stream: open frames are
 * closed as cut, gaps count from the new base, the tail-chain and burst state restarts and
 * same-value write tracking starts again. A time record further forward continues the stream, so
 * a restore forward by 2^32 instructions or more reads as time passing. The machine writes the
 * rebase time record before the next record it delivers: a rebase with no record after it is not
 * seen, and two rebases with no record between are one cut.
 *
 * An mmio record with pc 0 comes from a debugger or a DMA access and is counted apart from the
 * CPU's. Memory grows with distinct block pcs, exception numbers and registers, the open frames,
 * the entries inside a burst window and the write logs, each log bounded by
 * profile_options::write_log_limit. Neither copyable nor movable: sink() captures it.
 */
class profile_sink
{
public:
  /** @brief Kinds the sink reads. */
  static constexpr core::trace_kinds kinds{
      core::trace_kind::block, core::trace_kind::mmio, core::trace_kind::irq};

  /**
   * @brief Creates an empty sink.
   * @param start Instruction count the first record's dt counts from, the machine's count when
   *        the sink is added.
   * @param options Options.
   */
  explicit profile_sink(icount start, profile_options options = {});

  /** @brief Not copyable. */
  profile_sink(profile_sink const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(profile_sink const&) -> profile_sink& = delete;
  /** @brief Not movable. */
  profile_sink(profile_sink&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(profile_sink&&) -> profile_sink& = delete;
  /** @brief Destroys the sink. */
  ~profile_sink();

  /**
   * @brief Adds dt-stamped records, as a trace sink delivers them.
   * @param records Records, oldest first.
   */
  void feed(std::span<core::trace_record const> records);

  /** @brief A trace sink feeding this sink, valid while it lives. */
  [[nodiscard]] auto sink() -> core::trace_sink;

  /** @brief The aggregates of the records fed so far. */
  [[nodiscard]] auto result() const -> profile_result;

private:
  struct block_slot
  {
    address       pc{};
    bool          used{};
    std::uint64_t entries{};
    std::uint64_t instructions{};
  };

  struct frame
  {
    std::uint16_t number{};
    icount        entered{};
    std::uint64_t nested{};
  };

  struct exception_state
  {
    profile_exception  totals{};
    std::deque<icount> window{};
  };

  struct register_state
  {
    profile_register             totals{};
    std::optional<std::uint32_t> last_written{};
  };

  void time(icount to);
  void cut();
  void block(core::trace_record const& record);
  void irq(core::trace_record const& record);
  void mmio(core::trace_record const& record);
  void gap_before(icount start, profile_gaps& into);
  void count_block(address pc, std::uint32_t instructions);
  void grow();

  profile_options                                   options_;
  icount                                            start_;
  icount                                            now_;
  icount                                            last_end_;
  std::optional<icount>                             exit_at_{};
  std::uint64_t                                     elapsed_{};
  std::uint64_t                                     records_{};
  std::uint64_t                                     blocks_{};
  std::uint64_t                                     instructions_{};
  std::uint64_t                                     cuts_{};
  std::uint64_t                                     unmatched_exits_{};
  std::vector<block_slot>                           slots_;
  std::size_t                                       used_slots_{};
  std::vector<frame>                                frames_{};
  std::map<std::uint16_t, exception_state>          exceptions_{};
  profile_gaps                                      other_gaps_{};
  std::unordered_map<std::uint64_t, register_state> cpu_registers_{};
  std::unordered_map<std::uint64_t, register_state> other_registers_{};
  std::vector<profile_write_log>                    write_logs_{};
};
}
