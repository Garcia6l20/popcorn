#pragma once

#include <popcorn/core/clock.hpp>
#include <popcorn/core/fwd.hpp>
#include <popcorn/core/trace.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <meta>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace popcorn::obs
{
/** @brief How a translated block hands control on, by its last instruction. */
enum class block_exit : std::uint8_t
{
  other,
  call,
  ret,
};

/** @brief What the call stack tracker needs of one translated block. */
struct block_shape
{
  /** @brief How its last instruction hands control on. */
  block_exit exit{};
  /** @brief Address after its last instruction: where a call returns to, and where a return or
   * a call that did not execute falls through to. */
  address next{};
  /** @brief Start of the function symbol covering the block's start, the start itself when no
   * symbol covers it. */
  address function{};

  /** @brief Member-wise equality. */
  friend auto operator==(block_shape const&, block_shape const&) -> bool = default;
};

/**
 * @brief Shape of the translated block starting at an address with a given instruction count.
 *        Called on the machine thread, once per distinct block.
 */
using code_reader = std::move_only_function<block_shape(address pc, std::uint32_t instructions)>;

/** @brief Registers and key values of a reload tracker, see profile_options::reload_tracker. */
struct profile_reload_tracker
{
  /** @brief Flat ids of the key registers, one per alias. A CPU write to one is a key write, also
   * when its id is in change_ids. */
  std::vector<std::uint16_t> key_ids{};
  /** @brief Flat ids of the registers whose write opens a new epoch, one per alias. */
  std::vector<std::uint16_t> change_ids{};
  /** @brief Low 16 bits of a key write that reloads, the index is the reload kind. */
  std::vector<std::uint16_t> reload_values{};
};

/** @brief Options of a profile_sink. */
struct profile_options
{
  /** @brief Length in picoseconds of the sliding window profile_exception::burst counts entries
   * in, 0 for no burst count. Entry times are converted with the clock records, so the window
   * keeps its length across a core rate change. */
  std::uint64_t burst_window_ps{};
  /** @brief Flat register ids whose CPU writes are kept one by one in profile_result::write_logs.
   */
  std::vector<std::uint16_t> logged_registers{};
  /** @brief Writes kept per logged register. Later writes are counted in
   * profile_write_log::dropped. */
  std::size_t write_log_limit{4096};
  /** @brief Clock domain whose rate each logged write records in profile_write::clock_hz, and
   * whose rates profile_result::domain_rates keeps. The sink must be added with this domain, see
   * core::machine::add_trace_sink(). Empty for none. */
  std::optional<core::clock_domain> write_domain{};
  /** @brief Shapes of the blocks, which turn on the call stack tracker, profile_result::frames.
   * Empty for no tracker. */
  code_reader code{};
  /** @brief Most call frames the tracker keeps, exception frames apart. A call past it stays in
   * its caller's frame, is counted in profile_result::deep_calls, and its return pops nothing. */
  std::uint32_t max_call_depth{256};
  /** @brief Reload tracker, profile_result::reload_tracks. It needs write_domain and a
   * write_log_limit of at least 1, the sink constructor throws std::invalid_argument without
   * them. Its epoch table holds at most write_log_limit epochs. */
  std::optional<profile_reload_tracker> reload_tracker{};
};

/** @brief Kind of a node of the call tree. */
enum class frame_kind : std::uint8_t
{
  root,
  entry,
  call,
  exception,
};

/**
 * @brief Node of the call tree. root is node 0. entry is code reached with no call seen: the
 *        first block of the profile, of an exception handler, after a cut or after a return that
 *        matched no frame, keyed by its function. call is a call, keyed by the callee's
 *        function. exception is an exception entry, keyed by its number. Instructions of a block
 *        go to the node current when it ran.
 */
struct profile_frame
{
  /** @brief Index of the parent node, 0 for the root itself. */
  std::uint32_t parent{};
  /** @brief Kind. */
  frame_kind kind{};
  /** @brief Function start or exception number, 0 for the root. */
  std::uint32_t key{};
  /** @brief Times the node was entered: calls, exception entries or entries with no call. */
  std::uint64_t entries{};
  /** @brief Instructions of the blocks run in the node itself. */
  std::uint64_t self{};
  /** @brief self plus the inclusive instructions of its children. */
  std::uint64_t inclusive{};

  /** @brief Member-wise equality. */
  friend auto operator==(profile_frame const&, profile_frame const&) -> bool = default;
};

/** @brief Executions of the translated blocks starting at one pc. */
struct profile_block
{
  /** @brief Block start address. */
  address pc{};
  /** @brief Times a block started at pc. */
  std::uint64_t entries{};
  /** @brief Instructions those blocks executed. */
  std::uint64_t instructions{};
  /** @brief Profile time in picoseconds from the sink's start to the start of the first block at
   * pc, see profile_sink. */
  std::uint64_t first_ps{};

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
  /** @brief Longest gap in instructions of time. */
  std::uint64_t longest{};
  /** @brief Picoseconds in them. */
  std::uint64_t total_ps{};
  /** @brief Longest gap in picoseconds. */
  std::uint64_t longest_ps{};

  /** @brief Member-wise equality. */
  friend auto operator==(profile_gaps const&, profile_gaps const&) -> bool = default;
};

/**
 * @brief Entries and time of one exception number. Times are instruction counts and
 *        picoseconds, sleeps included. Inclusive runs from the entry record to the matching exit
 *        record, self is inclusive less the inclusive time of the exceptions that preempted it.
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
  /** @brief inclusive in picoseconds. */
  std::uint64_t inclusive_ps{};
  /** @brief self in picoseconds. */
  std::uint64_t self_ps{};
  /** @brief Longest inclusive time of one frame in picoseconds. */
  std::uint64_t longest_ps{};
  /** @brief Most frames open at once at one of its entries, itself included. */
  std::uint32_t max_depth{};
  /** @brief Entries of other exceptions while this one was the innermost frame, by number. */
  std::map<std::uint16_t, std::uint64_t> preempted_by{};
  /** @brief Entries at the instruction count of the exit record before them, with no block
   * record between. An entry that wakes the core after a sleep on exit is a wake, not a tail
   * chain. */
  std::uint64_t tail_chained{};
  /** @brief Most entries inside one profile_options::burst_window_ps, 0 without a window. */
  std::uint64_t burst{};
  /** @brief Frames a cut closed, see profile_sink. */
  std::uint64_t cut{};
  /** @brief Frames open when the result was taken. */
  std::uint64_t open{};
  /** @brief Sleeps that this exception's entry ended. */
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
  /** @brief Address of the writing instruction. */
  address pc{};
  /** @brief Profile time in picoseconds from the sink's start to the write, see profile_sink.
   */
  std::uint64_t ps{};
  /** @brief Rate in Hz of profile_options::write_domain at the write, from the clock records
   * before it, 0 without the domain. */
  std::uint64_t clock_hz{};
  /** @brief profile_result::records counted up to the write, itself included: the order of
   * writes to different registers at one instruction count. */
  std::uint64_t record{};

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

/** @brief Two consecutive reloads of one segment and the time between them. */
struct profile_reload_gap
{
  /** @brief The earlier reload. */
  profile_write start{};
  /** @brief The later reload. */
  profile_write end{};
  /** @brief Profile time between them in picoseconds. */
  std::uint64_t ps{};

  /** @brief Member-wise equality. */
  friend auto operator==(profile_reload_gap const&, profile_reload_gap const&) -> bool = default;
};

/** @brief What opened an epoch of a reload track. */
enum class epoch_kind : std::uint8_t
{
  start,
  cut,
  change,
};

/** @brief Stretch of a profile where the registers of profile_reload_tracker::change_ids hold
 *         one setting: from the start, a cut or a write to one of them to the next of these. */
struct profile_epoch
{
  /** @brief What opened it. */
  epoch_kind kind{};
  /** @brief Cuts seen before it opened. */
  std::uint64_t segment{};
  /** @brief Profile time in picoseconds when it opened. */
  std::uint64_t ps{};
  /** @brief profile_result::records counted when it opened. */
  std::uint64_t record{};
  /** @brief Register id written, 0 unless kind is epoch_kind::change. */
  std::uint16_t change_id{};
  /** @brief The write that opened it, empty unless kind is epoch_kind::change. */
  profile_write change{};
  /** @brief Low 16 bits of the last key write of the segment before the opening write, ~0 when
   * the segment had none. */
  std::uint32_t key_before{~std::uint32_t{0}};
  /** @brief Per reload kind, the pair of consecutive reloads whose first reload is in the
   * epoch and that has the most ticks of profile_options::write_domain between them, the first
   * of equal pairs. The pair may end in a later epoch. */
  std::vector<std::optional<profile_reload_gap>> longest{};

  /** @brief Member-wise equality. */
  friend auto operator==(profile_epoch const&, profile_epoch const&) -> bool = default;
};

/** @brief The last reload of one segment. */
struct profile_reload_tail
{
  /** @brief Segment. */
  std::uint64_t segment{};
  /** @brief The reload. */
  profile_write last{};
  /** @brief Its reload kind. */
  std::uint32_t kind{};
  /** @brief Index in profile_reload_track::epochs of the epoch it fell in. */
  std::uint32_t epoch{};

  /** @brief Member-wise equality. */
  friend auto operator==(profile_reload_tail const&, profile_reload_tail const&) -> bool = default;
};

/** @brief What profile_options::reload_tracker saw. */
struct profile_reload_track
{
  /** @brief CPU writes to the key registers, all of them. */
  std::uint64_t key_writes{};
  /** @brief Reloads per reload kind, all of them. */
  std::vector<std::uint64_t> counts{};
  /** @brief Epochs in order, the first of kind epoch_kind::start. At most
   * profile_options::write_log_limit. */
  std::vector<profile_epoch> epochs{};
  /** @brief The last reload of each segment that had one, in segment order. */
  std::vector<profile_reload_tail> tails{};
  /** @brief Epochs past the limit. From the first one, epochs, tails and the gaps stop changing
   * and only counts and key_writes grow. */
  std::uint64_t epochs_dropped{};
  /** @brief Profile time in picoseconds of the first dropped epoch, valid when epochs_dropped
   * is not 0. */
  std::uint64_t frozen_ps{};
  /** @brief Segment of the first dropped epoch, valid when epochs_dropped is not 0: of a dropped
   * change write, the new segment of a dropped cut. */
  std::uint64_t frozen_segment{};

  /** @brief Member-wise equality. */
  friend auto operator==(profile_reload_track const&, profile_reload_track const&)
      -> bool = default;
};

/** @brief A core instruction rate from a clock record. */
struct profile_clock
{
  /** @brief Instruction count from which the rate holds. */
  icount at{};
  /** @brief Cuts seen before the record, the time base @p at counts in. */
  std::uint64_t segment{};
  /** @brief Instructions per second. */
  std::uint64_t hz{};
  /** @brief Instructions of profile_result::elapsed that passed while the rate held, up to the
   * next clock record or to the last record. */
  std::uint64_t instructions{};
  /** @brief instructions in picoseconds at hz, rounded down. */
  std::uint64_t ps{};

  /** @brief Member-wise equality. */
  friend auto operator==(profile_clock const&, profile_clock const&) -> bool = default;
};

/** @brief A rate of profile_options::write_domain from a clock record. */
struct profile_rate
{
  /** @brief Instruction count from which the rate holds. */
  icount at{};
  /** @brief Cuts seen before the record, the time base @p at counts in. */
  std::uint64_t segment{};
  /** @brief Profile time in picoseconds from the sink's start to the record, see profile_sink. */
  std::uint64_t ps{};
  /** @brief Rate in Hz, 0 for a stopped clock. */
  std::uint64_t hz{};
  /** @brief profile_result::records counted before it: a write whose profile_write::record is
   * higher came after it. */
  std::uint64_t record{};

  /** @brief Member-wise equality. */
  friend auto operator==(profile_rate const&, profile_rate const&) -> bool = default;
};

/** @brief Runs of identical passes of one loop, from loop records, by head. */
struct profile_loop
{
  /** @brief Loop head. */
  address head{};
  /** @brief Highest block start of the loop over its runs. */
  address last{};
  /** @brief Runs reported. */
  std::uint64_t runs{};
  /** @brief Identical passes over the runs. */
  std::uint64_t passes{};
  /** @brief Instructions of time the runs covered since the sink's start or its last rebase.
   * A run reaches to its ender, so a run whose code left the loop before its ender also covers
   * the code from the exit to the ender. */
  std::uint64_t instructions{};
  /** @brief instructions in picoseconds, each run converted with the clock records it spans. */
  std::uint64_t ps{};
  /** @brief Runs by core::loop_ender. core::loop_ender::call is a host call, core::loop_ender::end
   * the end of the reports or of the sink. */
  std::array<std::uint64_t, std::meta::enumerators_of(^^core::loop_ender).size()> enders{};

  /** @brief Member-wise equality. */
  friend auto operator==(profile_loop const&, profile_loop const&) -> bool = default;
};

/** @brief Aggregates of a profile_sink. */
struct profile_result
{
  /** @brief Instruction count the sink started at. */
  icount start{};
  /** @brief Instruction time the records covered, the sum of every forward step between
   * consecutive records, from start to the last record. Steps across a cut are left out. */
  std::uint64_t elapsed{};
  /** @brief elapsed in picoseconds, the sum of profile_clock::ps over clocks. */
  std::uint64_t elapsed_ps{};
  /** @brief Records received, time records and clock records of a domain excluded. */
  std::uint64_t records{};
  /** @brief Block records. */
  std::uint64_t blocks{};
  /** @brief Instructions the blocks executed. */
  std::uint64_t instructions{};
  /** @brief Cuts, see profile_sink. */
  std::uint64_t cuts{};
  /** @brief Exit records with no open frame of their number on top of the stack. */
  std::uint64_t unmatched_exits{};
  /** @brief Blocks by pc, ascending. */
  std::vector<profile_block> code{};
  /** @brief Exceptions by number. */
  std::map<std::uint16_t, profile_exception> exceptions{};
  /** @brief Spans of time covered by no block, exception or sleep record, such as a held
   * core. */
  profile_gaps other_gaps{};
  /** @brief Sleeps, from sleep records, each from its start or the last cut to its wake. */
  profile_gaps sleeps{};
  /** @brief Sleeps by how the core went to sleep: unknown, WFI, WFE, sleep on exit. */
  std::array<std::uint64_t, 4> sleep_reasons{};
  /** @brief Sleeps cut short by the end of the profile. */
  std::uint64_t partial_sleeps{};
  /** @brief Instruction rates, the first at the start, then one per change and per cut. */
  std::vector<profile_clock> clocks{};
  /** @brief Registers the CPU accessed, by id, overflowed ids by address after them. */
  std::vector<profile_register> cpu_registers{};
  /** @brief Registers DMA accesses reached, ordered as cpu_registers. */
  std::vector<profile_register> dma_registers{};
  /** @brief Registers debugger accesses reached, ordered as cpu_registers. */
  std::vector<profile_register> debugger_registers{};
  /** @brief Write logs, in profile_options::logged_registers order. */
  std::vector<profile_write_log> write_logs{};
  /** @brief Rates of profile_options::write_domain, oldest first: one when the sink is added,
   * one per cut and one per change. Empty without the domain. */
  std::vector<profile_rate> domain_rates{};
  /** @brief Call tree, parents before children, empty without profile_options::code. */
  std::vector<profile_frame> frames{};
  /** @brief Calls that pushed a frame. */
  std::uint64_t calls{};
  /** @brief Returns that matched no frame: the frames above the innermost exception frame were
   * dropped and the code went on in an entry node. */
  std::uint64_t unmatched_returns{};
  /** @brief Calls past profile_options::max_call_depth. */
  std::uint64_t deep_calls{};
  /** @brief Loops by head, ascending, from loop records. */
  std::vector<profile_loop> loops{};
  /** @brief The reload track, one element with profile_options::reload_tracker, else empty. */
  std::vector<profile_reload_track> reload_tracks{};

  /** @brief Member-wise equality. */
  friend auto operator==(profile_result const&, profile_result const&) -> bool = default;
};

/**
 * @brief Trace sink keeping aggregates of block, mmio, irq, sleep, clock, cut and loop records,
 *        with no file: per block pc, per exception number, per register, per loop head, the
 *        sleeps, the core rates and the gaps between records. Time is the instruction count the
 *        records are stamped with, and in picoseconds from the clock records.
 *
 * A block record carries the instruction count of the whole block. A cut record that follows it,
 * for a block the engine left before its end, takes the instructions that did not run back.
 *
 * A sleep record covers its length up to its stamp, from the start or the last cut at the
 * earliest. A gap runs from the end of the previous block, exception or sleep record, or from the
 * start for the first record, to the start of the next one. So instructions, sleeps and gaps sum
 * to elapsed, and a profile opened during a sleep counts the rest of it as a sleep.
 *
 * Every time total has a picosecond companion. A span is converted piecewise with the clock
 * records of its segment, so a span across a rate change counts each part at its own rate. A
 * point in time is the picoseconds of the segment's first clock record plus, per later clock
 * record, its rate times its instructions, rounded down per clock span: a span differs from the
 * machine clock by less than 1 ps per clock record it covers. Without a clock record in the
 * segment, picoseconds are 0.
 *
 * Profile time is the time the records covered, the time across a cut left out:
 * profile_result::elapsed_ps at the end. A point's profile time is the picoseconds of the
 * segments before its own plus its picoseconds since its segment's first clock record. For a
 * profile opened at a reset with no cut, it is the machine time of the point.
 *
 * A time record flagged core::trace_rebase_flag, written for a reset or a restore, cuts the
 * stream: open frames are closed as cut, gaps count from the new base, the tail-chain and burst
 * state restarts and same-value write tracking starts again. An unflagged time record continues
 * the stream.
 *
 * With profile_options::code the sink keeps a shadow call stack. A block ending in a call pushes
 * a frame for the next block unless that block is the call's fall-through. A block ending in a
 * return pops to the frame returning to the next block, unless that block is the fall-through.
 * An exception entry pushes an exception frame holding the pending call or return, its exit pops
 * to it and restores them, so a return through EXC_RETURN pops no call frame. A next block that
 * starts inside the previous one, or a cut record of the previous block, shows that block was left
 * before its last instruction and moves no frame. A call past profile_options::max_call_depth
 * pushes nothing and the next return pops nothing, so deep returns match their own calls. A tail
 * call, a branch to a function, pushes nothing, so the callee's instructions count in the caller.
 *
 * Each context on the process stack, an RTOS task for example, keeps its own frames. An entry
 * from thread mode on the process stack saves the frames above the innermost exception frame, the
 * current node and the pending call or return under the address of its exception frame, the irq
 * record's payload. An exit to thread mode on the process stack restores what is saved under its
 * address and forgets it, or starts an empty stack with an entry node when nothing is. A task
 * deleted while suspended leaves its frames saved, and a later context whose exception frame lands
 * at the same address restores them. Code on the main stack keeps one stack, and a tail chain or a
 * nested exception keeps the exception frame rules above. A cut clears the stack and every saved
 * context. A return that matches no frame, after a longjmp for example, is counted and starts an
 * entry node. Shapes are cached per pc and instruction count: code rewritten after its first run
 * keeps its first shape.
 *
 * A loop record adds its run to the loop of its head: runs, passes, instructions, the enders and
 * the furthest block start. Loop records are not time spans of their own: their instructions are
 * also in the block records.
 *
 * mmio records of DMA and debugger accesses, flagged core::trace_dma_flag and
 * core::trace_debugger_flag, are counted apart from the CPU's. Memory grows with distinct block
 * pcs, exception numbers and registers, the open frames, the saved contexts, one per distinct
 * exception frame address of a suspended or deleted context, the entries inside a burst window
 * and the write logs, each log bounded by profile_options::write_log_limit. Neither copyable nor
 * movable: sink() captures it. Loops add one entry per distinct loop head.
 */
class POPCORN_API profile_sink
{
public:
  /** @brief Kinds the sink reads. */
  static constexpr core::trace_kinds kinds{core::trace_kind::block, core::trace_kind::mmio,
                                           core::trace_kind::irq,   core::trace_kind::sleep,
                                           core::trace_kind::clock, core::trace_kind::cut,
                                           core::trace_kind::loop};

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
    std::uint64_t first_ps{};
  };

  struct shape_slot
  {
    bool          known{};
    std::uint32_t instructions{};
    block_shape   shape{};
  };

  struct pending
  {
    block_exit exit{};
    address    start{};
    address    next{};
  };

  struct stack_entry
  {
    address       next{};
    std::uint32_t node{};
    std::uint32_t inner{};
    bool          exception{};
    pending       held{};
    bool          held_reroot{};
    std::uint64_t held_deep{};
  };

  struct saved_context
  {
    std::vector<stack_entry> frames{};
    std::uint32_t            current{};
    bool                     reroot{};
    pending                  held{};
    std::uint64_t            deep{};
  };

  struct frame
  {
    std::uint16_t number{};
    icount        entered{};
    std::uint64_t nested{};
    std::uint64_t nested_ps{};
  };

  struct exception_state
  {
    profile_exception         totals{};
    std::deque<std::uint64_t> window{};
  };

  struct rate_mark
  {
    icount        at{};
    std::uint64_t ps{};
    std::uint64_t hz{};
    std::uint64_t step{};
  };

  struct reload_state
  {
    static constexpr std::uint8_t none   = 0;
    static constexpr std::uint8_t key    = 1;
    static constexpr std::uint8_t change = 2;

    struct previous_reload
    {
      std::uint32_t     kind{};
      std::uint32_t     epoch{};
      unsigned __int128 ticks{};
    };

    std::vector<std::uint8_t>      roles{};
    std::vector<std::uint16_t>     values{};
    profile_reload_track           track{};
    std::vector<unsigned __int128> longest_ticks{};
    std::optional<std::uint32_t>   last_key{};
    std::optional<previous_reload> previous{};
    unsigned __int128              ticks{};
    std::uint64_t                  last_rate_ps{};
    bool                           frozen{};
  };

  struct register_state
  {
    profile_register             totals{};
    std::optional<std::uint32_t> last_written{};
  };

  void               time(icount to, bool rebase);
  void               clock(std::uint64_t hz);
  void               domain_clock(core::trace_record const& record);
  void               close_clock_span();
  [[nodiscard]] auto ps_at(icount at) const -> std::uint64_t;
  [[nodiscard]] auto ps_between(icount from, icount to) const -> std::uint64_t;
  [[nodiscard]] auto profile_ps(icount at) const -> std::uint64_t;
  void               sleep(core::trace_record const& record);
  void               loop(core::trace_record const& record);
  void               cut_block(core::trace_record const& record);
  void               cut();
  void               block(core::trace_record const& record);
  void               irq(core::trace_record const& record);
  void               mmio(core::trace_record const& record);
  [[nodiscard]] auto logged_write(core::trace_record const& record, std::uint32_t value) const
      -> profile_write;
  void track_write(std::uint8_t role, core::trace_record const& record, std::uint32_t value);
  void track_cut();
  void open_epoch(profile_epoch epoch);
  void track_change(core::trace_record const& record, std::uint32_t value);
  void gap_before(icount start, profile_gaps& into);
  void add_gap(profile_gaps& into, icount from, icount to) const;
  auto count_block(address pc, std::uint32_t instructions) -> std::size_t;
  void grow();
  auto shape_of(std::size_t at, std::uint32_t instructions) -> block_shape;
  void track(std::size_t at, std::uint32_t instructions);
  void walk(std::uint32_t instructions, block_shape const& shape, address pc);
  auto child(std::uint32_t parent, frame_kind kind, std::uint32_t key) -> std::uint32_t;
  void push_call(address next, address function);
  void pop_to(address pc);
  void enter_frame(std::uint16_t number, address context);
  void exit_frame(address context);
  auto thread_part() const -> std::size_t;
  void save_context(address context);
  void restore_context(address context);
  void clear_stack();

  profile_options                                      options_;
  icount                                               start_;
  icount                                               now_;
  icount                                               last_end_;
  icount                                               base_;
  std::optional<icount>                                exit_at_{};
  std::uint64_t                                        elapsed_{};
  std::uint64_t                                        clock_elapsed_{};
  std::uint64_t                                        records_{};
  std::uint64_t                                        blocks_{};
  std::uint64_t                                        instructions_{};
  std::uint64_t                                        cuts_{};
  std::uint64_t                                        unmatched_exits_{};
  std::vector<block_slot>                              slots_;
  std::vector<shape_slot>                              shaped_{};
  std::size_t                                          used_slots_{};
  std::vector<frame>                                   frames_{};
  std::map<std::uint16_t, exception_state>             exceptions_{};
  profile_gaps                                         other_gaps_{};
  std::unordered_map<std::uint64_t, register_state>    cpu_registers_{};
  std::unordered_map<std::uint64_t, register_state>    dma_registers_{};
  std::unordered_map<std::uint64_t, register_state>    debugger_registers_{};
  profile_gaps                                         sleeps_{};
  std::array<std::uint64_t, 4>                         sleep_reasons_{};
  std::uint64_t                                        partial_sleeps_{};
  std::vector<profile_clock>                           clocks_{};
  std::vector<rate_mark>                               marks_{};
  std::uint64_t                                        segments_ps_{};
  std::vector<profile_write_log>                       write_logs_{};
  std::uint64_t                                        domain_hz_{};
  std::vector<profile_rate>                            domain_rates_{};
  std::optional<reload_state>                          reload_{};
  std::unordered_map<std::uint64_t, block_shape>       shapes_{};
  std::vector<profile_frame>                           nodes_{};
  std::unordered_map<std::uint64_t, std::uint32_t>     children_{};
  std::vector<std::pair<std::uint64_t, std::uint32_t>> last_child_;
  bool                                                 tracking_;
  std::vector<stack_entry>                             stack_{};
  std::unordered_map<address, saved_context>           contexts_{};
  std::uint32_t                                        call_frames_{};
  std::uint64_t                                        deep_open_{};
  std::optional<address>                               walked_{};
  std::uint32_t                                        current_{};
  bool                                                 reroot_{true};
  pending                                              pending_{};
  std::uint64_t                                        calls_{};
  std::uint64_t                                        unmatched_returns_{};
  std::uint64_t                                        deep_calls_{};
  std::map<address, profile_loop>                      loops_{};
};

/**
 * @brief A code_reader over a machine: code bytes from its loaded images, else from its memory,
 *        decoded by the code analysis of its core, functions from the function symbols of its
 *        observed images, read once now. Valid while the machine lives. A block whose code cannot
 *        be read or decoded is block_exit::other.
 * @param target Machine.
 */
POPCORN_API auto machine_code_reader(core::machine& target) -> code_reader;
}
