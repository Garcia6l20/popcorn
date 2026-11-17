#pragma once

#include <popcorn/core/fwd.hpp>
#include <popcorn/core/trace.hpp>
#include <popcorn/core/types.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
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
  /** @brief Shapes of the blocks, which turn on the call stack tracker, profile_result::frames.
   * Empty for no tracker. */
  code_reader code{};
  /** @brief Most call frames the tracker keeps, exception frames apart. A call past it stays in
   * its caller's frame, is counted in profile_result::deep_calls, and its return pops nothing. */
  std::uint32_t max_call_depth{256};
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
  /** @brief Call tree, parents before children, empty without profile_options::code. */
  std::vector<profile_frame> frames{};
  /** @brief Calls that pushed a frame. */
  std::uint64_t calls{};
  /** @brief Returns that matched no frame: the frames above the innermost exception frame were
   * dropped and the code went on in an entry node. */
  std::uint64_t unmatched_returns{};
  /** @brief Calls past profile_options::max_call_depth. */
  std::uint64_t deep_calls{};

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
 * With profile_options::code the sink keeps a shadow call stack. A block ending in a call pushes
 * a frame for the next block unless that block is the call's fall-through. A block ending in a
 * return pops to the frame returning to the next block, unless that block is the fall-through.
 * An exception entry pushes an exception frame holding the pending call or return, its exit pops
 * to it and restores them, so a return through EXC_RETURN pops no call frame. A next block that
 * starts inside the previous one shows that block was left before its last instruction, by a
 * precise exit or a stop, and moves no frame. A cut clears the stack. A call past
 * profile_options::max_call_depth pushes nothing and the next return pops nothing, so deep
 * returns match their own calls. A tail call, a branch to a function, pushes nothing, so the
 * callee's instructions count in the caller. After an exception exit into other code than it
 * interrupted, an RTOS context switch through PendSV or SVC for example, the blocks of that code
 * count in the node of the interrupted code until one of its returns matches no frame, which is
 * counted and starts an entry node. A longjmp or a return to another frame does the same. Shapes
 * are cached per pc and instruction count: code rewritten after its first run keeps its first
 * shape.
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
  auto count_block(address pc, std::uint32_t instructions) -> std::size_t;
  void grow();
  auto shape_of(std::size_t at, std::uint32_t instructions) -> block_shape;
  void track(std::size_t at, std::uint32_t instructions);
  void walk(std::uint32_t instructions, block_shape const& shape, address pc);
  auto child(std::uint32_t parent, frame_kind kind, std::uint32_t key) -> std::uint32_t;
  void push_call(address next, address function);
  void pop_to(address pc);
  void enter_frame(std::uint16_t number);
  void exit_frame();
  void clear_stack();

  profile_options                                      options_;
  icount                                               start_;
  icount                                               now_;
  icount                                               last_end_;
  std::optional<icount>                                exit_at_{};
  std::uint64_t                                        elapsed_{};
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
  std::unordered_map<std::uint64_t, register_state>    other_registers_{};
  std::vector<profile_write_log>                       write_logs_{};
  std::unordered_map<std::uint64_t, block_shape>       shapes_{};
  std::vector<profile_frame>                           nodes_{};
  std::unordered_map<std::uint64_t, std::uint32_t>     children_{};
  std::vector<std::pair<std::uint64_t, std::uint32_t>> last_child_;
  bool                                                 tracking_;
  std::vector<stack_entry>                             stack_{};
  std::uint32_t                                        call_frames_{};
  std::uint64_t                                        deep_open_{};
  std::uint32_t                                        current_{};
  bool                                                 reroot_{true};
  pending                                              pending_{};
  std::uint64_t                                        calls_{};
  std::uint64_t                                        unmatched_returns_{};
  std::uint64_t                                        deep_calls_{};
};

/**
 * @brief A code_reader over a machine: code bytes from its loaded images, else from its memory,
 *        decoded by the code analysis of its core, functions from the function symbols of its
 *        observed images, read once now. Valid while the machine lives. A block whose code cannot
 *        be read or decoded is block_exit::other.
 * @param target Machine.
 */
auto machine_code_reader(core::machine& target) -> code_reader;
}
