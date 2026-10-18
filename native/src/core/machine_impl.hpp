#pragma once

#include "../periph/line_bus.hpp"
#include "../periph/register_layer.hpp"
#include "engine.hpp"
#include "memory_map.hpp"

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/trace.hpp>
#include <popcorn/periph/firewall.hpp>

#include <unicorn/unicorn.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <limits>
#include <map>
#include <meta>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace popcorn::core
{
/** @brief Table behind every hook_handle of a machine. */
class hook_registry
{
public:
  /** @brief Whether a slice runs, so a handle refuses removal. */
  bool running = false;
  /** @brief Trace deliveries on the stack, during which removals wait like during a slice. */
  std::uint32_t delivering = 0;
  /** @brief Last id handed out. */
  std::uint64_t last_id = 0;
  /** @brief Log sinks by id, in registration order. */
  std::map<std::uint64_t, log_sink> log_sinks{};
  /** @brief Ids whose removal waits for the end of the running slice. */
  std::vector<std::uint64_t> pending_removal{};
  /** @brief Removes the engine side of a hook, set by the machine. */
  std::function<void(std::uint64_t)> remover{};

  /** @brief Removes a hook by id, at the end of the slice when one runs. */
  void remove(std::uint64_t id) noexcept
  {
    if(log_sinks.erase(id) != 0)
    {
      return;
    }
    if(running or delivering != 0)
    {
      pending_removal.push_back(id);
      return;
    }
    if(remover)
    {
      remover(id);
    }
  }

  /** @brief Removes the hooks whose removal waited for the slice end. */
  void drain() noexcept
  {
    for(auto const id : std::exchange(pending_removal, {}))
    {
      if(remover)
      {
        remover(id);
      }
    }
  }

  /** @brief Hands a record to every log sink, in registration order. The sinks are copied
   * first, so a sink may add or remove sinks. */
  void log(log_record const& record) const
  {
    auto const sinks = log_sinks;
    for(auto const& [id, sink] : sinks)
    {
      sink(record);
    }
  }
};

/** @brief Routes IRQ line levels of every peripheral to the arch module, wired-OR per source:
 * the source is active while any line on it is. */
class machine_lines final : public periph::line_bus
{
public:
  /** @brief Arch module receiving the levels, set before any line is built. */
  arch::arch* arch{};
  /** @brief Active lines per interrupt source. */
  std::vector<std::uint32_t> active{};
  /** @brief Active lines on periph::nmi_source. */
  std::uint32_t nmi_active{};

  void set_irq(std::uint32_t source, bool level) override
  {
    if(source == periph::nmi_source)
    {
      if(level)
      {
        if(nmi_active++ == 0)
        {
          arch->set_nmi_line(true);
        }
      }
      else if(nmi_active > 0 and --nmi_active == 0)
      {
        arch->set_nmi_line(false);
      }
      return;
    }
    if(source >= active.size())
    {
      active.resize(source + 1);
    }
    auto& count = active[source];
    if(level)
    {
      if(count++ == 0)
      {
        arch->set_irq_line(source, true);
      }
    }
    else if(count > 0 and --count == 0)
    {
      arch->set_irq_line(source, false);
    }
  }

  void set_dma(std::uint32_t, bool) override
  {}
};

/** @brief Peer directory of a machine, filled by attach_peripherals once every model exists. */
class machine_peers final : public periph::peer_directory
{
public:
  /**
   * @brief Fills the directory.
   * @param by_name Model per peripheral name, aliases included, sorted by name.
   * @param models Every model, in description order.
   */
  void fill(
      std::vector<std::pair<std::string_view, periph::model*>> by_name,
      std::vector<periph::model*>                              models) noexcept;

  [[nodiscard]] auto find(std::string_view peripheral) const noexcept -> periph::model* override;
  [[nodiscard]] auto all() const noexcept -> std::span<periph::model* const> override;

private:
  std::vector<std::pair<std::string_view, periph::model*>> by_name_{};
  std::vector<periph::model*>                              models_{};
};

/** @brief Per peripheral state of a machine: storage, compiled semantics, lines, model. */
struct machine::peripheral_slot
{
  /**
   * @brief Creates the storage and layout of a peripheral.
   * @param peripheral Description, which the machine keeps.
   * @param queue Event queue of the machine.
   * @param index Index of the peripheral in the description, the owner of its events.
   */
  peripheral_slot(peripheral_desc const& peripheral, scheduler& queue, std::uint32_t index)
      : desc{peripheral}, regs{peripheral}, layout{peripheral}, events{queue, index},
        claimed(layout.words(), 0)
  {}

  /** @brief Description of the peripheral. */
  peripheral_desc const& desc;
  /** @brief Storage. */
  periph::register_file regs;
  /** @brief Compiled semantics of the registers no model claims. */
  periph::register_layout layout;
  /** @brief Write-once locks. */
  periph::layer_state layer{};
  /** @brief IRQ lines, one per `irqs` entry, frozen before the context spans them. */
  std::vector<periph::irq_line> irqs{};
  /** @brief Line to the non-maskable interrupt, model_context::nmi. */
  periph::irq_line nmi{};
  /** @brief DMA request lines, none until the description names them. */
  std::vector<periph::dma_request> dma{};
  /** @brief The model's event port. */
  event_port events;
  /** @brief Context handed to the model factory. */
  std::optional<periph::model_context> context{};
  /** @brief Model serving the claimed registers, null when none. */
  std::shared_ptr<periph::model> model{};
  /** @brief Non-zero for each storage word the model claims. */
  std::vector<std::uint8_t> claimed;
  /** @brief Non-zero for each word whose unmodelled access was logged, once per machine life. */
  std::vector<std::uint8_t> logged = std::vector<std::uint8_t>(claimed.size(), 0);
  /** @brief Offsets without a register whose access was logged. */
  std::set<std::uint32_t> warned{};
  /** @brief aux of an mmio record per rule index of the layout, less one. */
  std::vector<std::uint16_t> trace_ids{};
  /** @brief aux per byte offset of the words holding several registers, sorted by offset. */
  std::vector<std::pair<std::uint32_t, std::uint16_t>> byte_trace_ids{};

  /** @brief aux of an mmio record at byte @p offset, trace_id_overflow where no register lies. */
  [[nodiscard]] auto trace_id_at(std::uint32_t offset) const noexcept -> std::uint16_t
      pre(offset / 4 < layout.words())
  {
    if(not byte_trace_ids.empty()) [[unlikely]]
    {
      auto const found = std::ranges::lower_bound(
          byte_trace_ids, offset, {}, &std::pair<std::uint32_t, std::uint16_t>::first);
      if(found != byte_trace_ids.end() and found->first == offset)
      {
        return found->second;
      }
    }
    auto const rule = layout.rule_index(offset / 4);
    return rule == 0 ? trace_id_overflow : trace_ids[rule - 1];
  }
};

/** @brief Edge counters by `(from, to)`, open addressing over a power-of-two table. */
class flat_edge_table
{
public:
  /** @brief Adds one to the count of an edge. @p key is `from << 32 | to`. */
  [[gnu::always_inline]] inline void count(std::uint64_t key);

  /** @brief Edges with a non-zero count, ordered by from then to. */
  [[nodiscard]] auto edges() const -> std::vector<edge_counter>;

  /** @brief Drops every edge. */
  void clear() noexcept;

private:
  struct slot
  {
    std::uint64_t key{};
    std::uint64_t hits{};
  };

  static auto hash(std::uint64_t key) noexcept -> std::uint64_t
  {
    return key * 0x9e37'79b9'7f4a'7c15;
  }

  void grow();

  std::vector<slot> slots_{};
  std::size_t       used_ = 0;
};

void flat_edge_table::count(std::uint64_t key)
{
  if(2 * (used_ + 1) > slots_.size())
  {
    grow();
  }
  auto const mask = slots_.size() - 1;
  for(auto at = hash(key) >> 32 & mask;; at = (at + 1) & mask)
  {
    auto& held = slots_[at];
    if(held.hits == 0)
    {
      held = {.key = key, .hits = 1};
      ++used_;
      return;
    }
    if(held.key == key)
    {
      ++held.hits;
      return;
    }
  }
}

/** @brief Largest instruction of the supported cores in bytes, the bound of every decode from
 * a block start: a block of n instructions spans at most n times this. */
inline constexpr address max_insn_bytes = 4;

/** @brief Coverage counters of a machine, cumulative across restores. */
struct coverage_state
{
  /** @brief Block counters per range, in enable order. */
  std::vector<block_counters> blocks{};
  /** @brief Whether some ranges overlap, so a block may count in several. */
  bool overlapping = false;
  /** @brief Range the previous block fell in, searched first. */
  std::size_t last_range = 0;
  /** @brief Edge counters. */
  flat_edge_table edges{};
  /** @brief Start of the previous counted block, the high half of the next edge key. Kept in
   * 64 bits: a 32-bit spill of it reloaded as the 64-bit key fails store forwarding once per
   * block. */
  std::uint64_t previous{};
  /** @brief Whether previous starts an edge: false after a block outside every range or a
   * control transfer that is no branch. */
  bool has_previous = false;
  /** @brief Instructions of the block at previous while it may still be left before its end, 0
   * once the engine entered another block or the block fell outside every range. */
  std::uint32_t open_insns = 0;
  /** @brief Blocks left before their end, by block start then first instruction not run. */
  std::map<std::pair<address, address>, std::uint64_t> cuts{};

  /**
   * @brief Counts a block starting at @p addr and the edge that reached it.
   * @param addr Block start.
   * @param insns Instructions of the block, as the engine translated it.
   */
  [[gnu::always_inline]] inline void count(address addr, std::uint32_t insns);

  /**
   * @brief Closes the block counted last as execution leaves it: a cut at @p pc when @p pc is one
   *        of its instructions, which therefore did not run. Called after each engine run, which
   *        ends with the block it entered last, and where an intercept returns. The engine counts
   *        no instruction at an exit, so a block left at an exit after all its counted
   *        instructions is cut too, unless the last of them ends a block by itself.
   * @param core Arch decoding the block's instructions.
   * @param pc Next instruction to run.
   * @param at_exit Whether the engine stopped at @p pc because it is one of its exits.
   */
  void leave(arch::arch const& core, address pc, bool at_exit);

private:
  static auto holds(address_range const& range, address addr) noexcept -> bool
  {
    return range.start <= addr and std::uint64_t{addr} < std::uint64_t{range.start} + range.size;
  }

  static void bump(block_counters& counters, address addr) noexcept
  {
    auto& hits = counters.hits[(addr - counters.range.start) >> 1];
    if(hits != std::numeric_limits<std::uint32_t>::max())
    {
      ++hits;
    }
  }
};

void coverage_state::count(address addr, std::uint32_t insns)
{
  auto inside = false;
  if(last_range < blocks.size() and holds(blocks[last_range].range, addr) and not overlapping)
  {
    bump(blocks[last_range], addr);
    inside = true;
  }
  else
  {
    for(auto const& [index, counters] : std::views::enumerate(blocks))
    {
      if(holds(counters.range, addr))
      {
        bump(counters, addr);
        last_range = static_cast<std::size_t>(index);
        inside     = true;
        if(not overlapping)
        {
          break;
        }
      }
    }
  }
  if(not inside)
  {
    has_previous = false;
    open_insns   = 0;
    return;
  }
  if(has_previous)
  {
    edges.count(previous << 32 | addr);
  }
  previous     = addr;
  has_previous = true;
  open_insns   = insns;
}

/** @brief Loop tracking of spin detection, active while machine_options::spin_threshold is set.
 * One loop is tracked per exception depth, so a handler's loops never restart the tracking of
 * the loop it interrupted. */
struct spin_state
{
  /** @brief Bytes after a loop head in which blocks and MMIO reads belong to the loop. */
  static constexpr address loop_window = 256;
  /** @brief MMIO reads kept per iteration: past them the iteration is not comparable. */
  static constexpr std::size_t max_reads = 64;
  /** @brief Depths tracked apart, deeper nesting shares the last level. */
  static constexpr std::uint32_t max_levels = 32;

  /** @brief One MMIO read of an iteration. */
  struct mmio_read
  {
    /** @brief Guest address read. */
    address addr{};
    /** @brief Value read. */
    std::uint32_t value{};
    /** @brief Flat register id, trace_id_overflow when no register lies there. */
    std::uint16_t reg{};
    /** @brief Description index of the peripheral, the owner of its model's events. */
    std::uint32_t peripheral{};

    /** @brief Member-wise equality. */
    friend auto operator==(mmio_read const&, mmio_read const&) -> bool = default;
  };

  /** @brief The loop tracked at one exception depth. */
  struct loop_state
  {
    /** @brief Head of the loop being tracked, 0 before the first backward branch. */
    address head{};
    /** @brief Back-edge target inside the loop window, adopted as head after two back edges to
     * it that do not pass the head: an inner loop that took over. */
    address candidate{};
    /** @brief Consecutive back edges to candidate since the head was last reached. */
    std::uint32_t candidate_hits = 0;
    /** @brief Highest block start of the current iteration inside the loop window. */
    address last{};
    /** @brief Registers at the end of the previous iteration, valid while known. */
    std::vector<std::uint64_t> registers{};
    /** @brief Whether registers holds the previous iteration's values. */
    bool known = false;
    /** @brief MMIO reads of the previous iteration, callees included. */
    std::vector<mmio_read> reads{};
    /** @brief MMIO reads of the current iteration. */
    std::vector<mmio_read> reading{};
    /** @brief Whether the current iteration stayed within max_reads. */
    bool comparable = true;
    /** @brief Consecutive iterations without progress. */
    std::uint32_t count = 0;
    /** @brief Whether this run of iterations already raised its spin stop. */
    bool reported = false;

    /** @brief Restarts the count, the loop made progress or slept. */
    void progress() noexcept
    {
      count    = 0;
      reported = false;
    }

    /** @brief Forgets the loop, keeping the capacity of its buffers. */
    void clear() noexcept
    {
      head           = {};
      candidate      = {};
      candidate_hits = 0;
      last           = {};
      known          = false;
      comparable     = true;
      progress();
      reads.clear();
      reading.clear();
    }
  };

  /** @brief Engine ids of the core registers compared, pc excluded. */
  std::vector<int> register_ids{};
  /** @brief Positions in register_ids of sp, r0-r3 and lr, compared first. */
  std::vector<std::size_t> subset{};
  /** @brief Engine ids of the subset. */
  std::vector<int> subset_ids{};
  /** @brief Exceptions entered and not returned from since tracking began. */
  std::uint32_t depth = 0;
  /** @brief Start of the previous block, 0 after an engine restart or an exception. */
  address previous{};
  /** @brief Full register read of this iteration. */
  std::vector<std::uint64_t> scratch{};
  /** @brief Pointers into scratch for the batch read, built once. */
  std::vector<void*> pointers{};
  /** @brief Subset read of this iteration. */
  std::vector<std::uint64_t> subset_scratch{};
  /** @brief Pointers into subset_scratch, built once. */
  std::vector<void*> subset_pointers{};
  /** @brief Tracked loop per depth, grown on first use, never shrunk. */
  std::vector<loop_state> levels = std::vector<loop_state>(1);

  /** @brief The loop tracked at the current depth. */
  [[nodiscard]] auto loop() noexcept -> loop_state&
  {
    return levels[std::min(depth, max_levels - 1)];
  }

  /** @brief The loop tracked at the current depth. */
  [[nodiscard]] auto loop() const noexcept -> loop_state const&
  {
    return levels[std::min(depth, max_levels - 1)];
  }

  /** @brief An exception was entered: one level deeper, starting with no loop. */
  void enter()
  {
    ++depth;
    auto const level = std::min(depth, max_levels - 1);
    if(levels.size() <= level)
    {
      levels.resize(level + 1);
    }
    levels[level].clear();
  }

  /** @brief An exception returned: back to the interrupted loop, clamped at depth 0. */
  void leave() noexcept
  {
    if(depth != 0)
    {
      --depth;
    }
  }

  /** @brief Restarts the count of the loop at the current depth. */
  void progress() noexcept
  {
    loop().progress();
  }

  /** @brief Forgets every tracked loop. */
  void clear() noexcept
  {
    depth    = 0;
    previous = {};
    for(auto& level : levels)
    {
      level.clear();
    }
  }
};

/** @brief Buffer bytes whose translated code must be removed at the next block boundary. */
struct stale_range
{
  /** @brief Buffer written. */
  host_buffer const* buffer{};
  /** @brief First written buffer offset. */
  std::size_t first{};
  /** @brief Buffer offset past the last written byte. */
  std::size_t last{};
};

/** @brief Why an engine run ended, as the hook that stopped it saw it. */
enum class engine_end : std::uint8_t
{
  /** @brief No hook stopped it: an exit address, or the engine returned on its own. */
  exit,
  /** @brief The block or step hook reached the deadline or the earliest event. */
  event,
  /** @brief A pending exception became deliverable, at a block or after a trap hook. */
  interrupt,
  /** @brief A write inside the engine into an executable buffer left stale code to remove: a
   * guest store, or a model's write from an engine callback. */
  store,
  /** @brief The stop bits, set by a request, a model or another thread, without stale code. */
  request,
  /** @brief The wall budget ran out. */
  wall,
  /** @brief A model asked for a precise exit after the access instruction. */
  precise,
  /** @brief An engine error, a trap, a stopping intercept, until_thread, or a callback that
   * threw, whichever hook then stopped the engine. */
  other,
};

/** @brief Number of engine_end values. */
inline constexpr std::size_t engine_end_count = std::meta::enumerators_of(^^engine_end).size();

/** @brief Engine work counters of a machine, plain counts kept on the machine's own thread,
 * never saved in snapshots. */
struct run_counters
{
  /** @brief Exception entries, counted from the arch's entry trace record. */
  std::uint64_t entries{};
  /** @brief `uc_emu_start` calls. */
  std::uint64_t engine_runs{};
  /** @brief Guest stores the store hook of a multiply mapped executable buffer saw. */
  std::uint64_t guest_stores_hooked{};
  /** @brief Code ranges written inside the engine, removed later by a drain. */
  std::uint64_t removes_in_engine{};
  /** @brief Code ranges written between engine runs, removed at once. Restores are not
   * counted. */
  std::uint64_t removes_direct{};
  /** @brief `uc_ctl_remove_cache` calls of removes_in_engine. */
  std::uint64_t remove_calls_in_engine{};
  /** @brief `uc_ctl_remove_cache` calls of removes_direct. */
  std::uint64_t remove_calls_direct{};
  /** @brief Engine runs by engine_end. */
  std::array<std::uint64_t, engine_end_count> ends{};

  /** @brief Engine runs that ended as @p why. */
  [[nodiscard]] auto ended(engine_end why) const noexcept -> std::uint64_t
  {
    return ends[std::to_underlying(why)];
  }
};

/** @brief Stop request recorded by machine_services::request_stop. */
struct requested_stop
{
  /** @brief Reason the run reports. */
  stop_reason reason{};
  /** @brief Text of the request. */
  std::string detail{};
};

/** @brief Unmapped or protected access recorded by the engine's memory event hook. */
struct unmapped_access
{
  /** @brief Address of the access. */
  address addr{};
  /** @brief Kind of the access. */
  arch::memory_op op{};
};

/** @brief Caller context saved by machine::begin_call(). */
struct saved_call
{
  /** @brief Frame of the call. */
  arch::call_frame frame{};
  /** @brief Values of `arch::registers()`, in its order. */
  std::vector<std::uint64_t> registers{};
  /** @brief Bytes of `arch::save()`. */
  std::vector<std::byte> arch_state{};
  /** @brief Whether a slice of the call reached the return sentinel. */
  bool returned{};
  /** @brief Return frames in flight when the call began, those of the caller. */
  std::size_t frames{};
  /** @brief Caller's pending intercept resume point. */
  std::optional<std::pair<address, std::size_t>> resume_past{};
  /** @brief Caller's spin exception depth: a call stopped inside a handler must not leave the
   * depth one too high. */
  std::uint32_t spin_depth{};
  /** @brief Caller's loop at spin_depth, so a call does not restart the caller's spin count. */
  spin_state::loop_state paused_loop{};
};

/** @brief How a slice ends: with a stop, or empty when only a yield bound was reached. */
struct slice_end
{
  /** @brief Stop record, empty for a yield. */
  std::optional<stop> stopped{};
  /** @brief Whether the stop is the stop_when predicate's. */
  bool asked = false;
  /** @brief Whether the slice ended for slice_request::until_thread. */
  bool thread_ready = false;
};

/** @brief Run state of a machine: slice bounds, deferred requests, hooks and exception slot. */
struct machine::run_state
{
  /** @brief Blocks between two wall clock reads, reading it every block costs 20 ns. */
  static constexpr std::uint32_t wall_check_blocks = 256;

  /** @brief Writable window of an executable buffer with two or more windows of their own engine
   * memory, watched for guest stores: Unicorn invalidates translated code on a store only through
   * the engine memory the code was translated from. Windows mapped with `uc_mem_map_alias` share
   * their owner's, so a buffer whose other windows are all such aliases is not watched. */
  struct guest_window
  {
    /** @brief Run state recording the stores. */
    run_state* run{};
    /** @brief Buffer behind the window. */
    host_buffer const* buffer{};
    /** @brief Window watched. */
    buffer_window window{};
  };

  /** @brief One engine MMIO mapping of a peripheral window, its own or an alias's. */
  struct mmio_window
  {
    /** @brief Run state serving the accesses. */
    run_state* run{};
    /** @brief Slot of the peripheral the window reaches. */
    peripheral_slot* slot{};
    /** @brief Peripheral named by the window, the alias for an alias window. */
    peripheral_desc const* unit{};
    /** @brief Guest address of the window. */
    address base{};
    /** @brief Bytes the window serves from base: the peripheral size rounded up to the engine
     * page, or in a page shared with other windows, up to the next window's base or the end of
     * the shared pages. */
    std::uint32_t size{};
    /** @brief Whether the window is a secure alias window, a `SEC_` instance of the
     * peripheral it aliases. */
    bool secure_alias{};
    /** @brief Index in the description of the peripheral the window reaches, the aliased one
     * for an alias window. */
    std::size_t serving{};
  };

  /** @brief An intercept added with machine::add_intercept(). */
  struct intercept_entry
  {
    /** @brief Handle id. */
    std::uint64_t id{};
    /** @brief Intercepted address. */
    address at{};
    /** @brief Handler. */
    intercept_handler handler{};
    /** @brief Every call or the first only. */
    intercept_mode mode{};
    /** @brief Whether a once intercept already fired. */
    bool spent{};
    /** @brief Calls handled so far. */
    std::uint64_t calls{};
  };

  /** @brief A return hook added with machine::add_return_hook(). */
  struct return_entry
  {
    /** @brief Handle id. */
    std::uint64_t id{};
    /** @brief Function entry. */
    address at{};
    /** @brief Argument kinds captured at entry. */
    std::vector<arch::value_kind> args{};
    /** @brief Return value kind. */
    arch::value_kind returns{};
    /** @brief Handler. */
    return_handler handler{};
    /** @brief Returns handled so far. */
    std::uint64_t calls{};
  };

  /** @brief A probe added with machine::add_probe(), held by its code site. */
  struct probe_entry
  {
    /** @brief Handle id. */
    std::uint64_t id{};
    /** @brief Arrivals counted, shared with the handle, so a probe left in its site after
     * removal still counts through a live object. */
    std::shared_ptr<probe_hits> hits{};
    /** @brief Security state an arrival must run in to count, empty for any. */
    std::optional<bool> secure{};
  };

  /** @brief A watch added with machine::add_watch(). */
  struct watch_entry
  {
    /** @brief Handle id. */
    std::uint64_t id{};
    /** @brief Run state serving the hits. */
    run_state* run{};
    /** @brief First watched address. */
    address at{};
    /** @brief Watched bytes. */
    std::uint32_t size{};
    /** @brief Accesses reported. */
    watch_kind kind{};
    /** @brief Handler. */
    watch_handler handler{};
    /** @brief Inclusive guest ranges reaching the watched bytes, every alias mapping included. */
    std::vector<std::pair<std::uint64_t, std::uint64_t>> spans{};
    /** @brief Memory hooks over the RAM and flash spans, each starting 3 bytes lower, since
     * Unicorn matches a hook on the access start address only. */
    std::vector<uc_hook> hooks{};

    /** @brief Whether [@p addr, @p addr + @p size) overlaps a span. */
    [[nodiscard]] auto covers(address addr, std::uint8_t size) const noexcept -> bool
    {
      auto const last = std::uint64_t{addr} + size - 1;
      return std::ranges::any_of(
          spans, [&](auto const& span) { return addr <= span.second and span.first <= last; });
    }
  };

  /** @brief Engine read hook over one guest span of a watched engine page of a host buffer,
   * the region's own mapping or an alias. */
  struct controller_hook
  {
    /** @brief Run state serving the hits. */
    run_state* owner{};
    /** @brief Buffer holding the page. */
    host_buffer const* buffer{};
    /** @brief Region the span lies in, an alias included. */
    mapped_region const* region{};
    /** @brief Buffer offset of the span's first byte, less its guest address. */
    std::int64_t to_buffer{};
    /** @brief Order of creation, from controller_hook_serial. */
    std::uint64_t serial{};
    /** @brief Engine hook. */
    uc_hook hook{};
  };

  /** @brief Hooks of one watched engine page, one per guest span mapping it. */
  struct controller_page
  {
    /** @brief The hooks. */
    std::vector<std::unique_ptr<controller_hook>> hooks{};
  };

  /** @brief Watched ranges of one host buffer whose spans overlap, each span starting
   * controller_lead bytes before its range, merged so that a load start lies in one run only.
   * Keyed by its first span offset. */
  struct controller_run
  {
    /** @brief Last offset of the run. */
    std::size_t last{};
    /** @brief Model reported to, the controller of the region the run's first range was named
     * in. */
    periph::model* controller{};
    /** @brief Watched ranges of the run, inclusive buffer offsets, sorted. */
    std::vector<std::pair<std::size_t, std::size_t>> ranges{};
    /** @brief Largest last offset of ranges up to each index. */
    std::vector<std::size_t> reach{};

    /** @brief Whether buffer offsets [@p from, @p to] overlap a range. */
    [[nodiscard]] auto overlaps(std::size_t from, std::size_t to) const noexcept -> bool
    {
      auto const after =
          std::ranges::upper_bound(ranges, to, {}, [](auto const& range) { return range.first; });
      return after != ranges.begin()
         and reach[static_cast<std::size_t>(after - ranges.begin()) - 1] >= from;
    }
  };

  /** @brief A load whose on_memory_read call rehooked the page holding it: the new hook of
   * that span, which the same pass of the engine's hook list reaches, skips that load. */
  struct controller_reentry
  {
    /** @brief Address of the load. */
    std::uint64_t addr{};
    /** @brief Size of the load. */
    int size{};
    /** @brief Serial of the new hook over the load. */
    std::uint64_t serial{};
    /** @brief Whether the guard applies to the next hook call. */
    bool armed{};
  };

  /** @brief Bytes a controller span starts before its range: Unicorn matches a hook on the
   * access start only, and a load is at most 8 bytes. */
  static constexpr std::size_t controller_lead = 7;

  /** @brief One engine code hook serving every intercept, return hook and probe of an
   * address. */
  struct code_site
  {
    /** @brief Run state serving the calls. */
    run_state* run{};
    /** @brief Hooked address. */
    address at{};
    /** @brief Engine hook. */
    uc_hook hook{};
    /** @brief Intercepts and return hooks of the address, in add order. */
    std::vector<std::variant<intercept_entry*, return_entry*>> entries{};
    /** @brief Probes of the address, counted before the entries, outside their indices so
     * that resume_past never counts an arrival twice. */
    std::vector<probe_entry> probes{};
  };

  /** @brief dt stamping of one sink's record stream, as trace_ring::append() stamps the ring. */
  struct dt_stream
  {
    /** @brief Instruction count the next dt counts from. */
    icount last{};
    /** @brief Whether the time base moved, so the next record follows a time record. */
    bool rebased = false;

    /** @brief Appends @p record to @p out with its dt, after a time record when needed. */
    void stamp(trace_record record, icount now, std::vector<trace_record>& out);
  };

  /** @brief A sink added with machine::add_trace_sink(). */
  struct sink_entry
  {
    /** @brief Kinds delivered. */
    trace_kinds kinds{};
    /** @brief Receiver of the batches. */
    trace_sink sink{};
    /** @brief Code ranges whose records are delivered, empty for everywhere. */
    std::vector<address_range> ranges{};
    /** @brief Records not delivered yet. */
    std::vector<trace_record> batch{};
    /** @brief dt stamping of the sink's stream. */
    dt_stream stream{};
  };

  /** @brief Address lookup entry of the machine's symbol table. */
  struct symbol_entry
  {
    /** @brief First address. */
    address value{};
    /** @brief Address past the last byte covered: value plus size, or for a size-0 symbol the
     * next entry's address, bounded by the end of the memory region holding it. */
    std::uint64_t end{};
    /** @brief Index in the symbol table. */
    std::uint32_t index{};
  };

  /** @brief Function and object symbols of some images by address. */
  struct symbol_lookup
  {
    /** @brief Entries sorted by address. */
    std::vector<symbol_entry> entries{};
    /** @brief Largest end of entries up to each position, which bounds the scan back. */
    std::vector<std::uint64_t> max_end{};
  };

  /** @brief Records a sink collects before its batch is delivered. */
  static constexpr std::size_t sink_batch = 4096;

  /** @brief A call a return hook saw enter, waiting for its return. */
  struct return_frame
  {
    /** @brief Hook that saw the entry. */
    return_entry* entry{};
    /** @brief Address the call returns to. */
    address ret{};
    /** @brief Stack pointer at entry, the same at the return under AAPCS. */
    std::uint64_t sp{};
    /** @brief Argument bits captured at entry. */
    std::vector<std::uint64_t> args{};
    /** @brief Clock at entry. */
    icount entered{};
  };

  /** @brief Creates the run state of @p machine. */
  explicit run_state(machine& machine) noexcept : owner{machine}
  {}

  /** @brief Machine the state belongs to. */
  machine& owner;
  /** @brief Engine of the machine, set once it is open. */
  uc_struct* engine{};
  /** @brief Arch module of the machine, cached for the block hook, set by install_hooks(). */
  arch::arch* arch{};
  /** @brief Whether a slice runs. */
  bool running = false;
  /** @brief Whether a handler of the running slice is on the stack. */
  bool in_handler = false;
  /** @brief Caller context of the call in progress. */
  std::optional<saved_call> call{};
  /** @brief Resets done so far, which tells the loop a reset superseded an engine result. */
  std::uint64_t resets{};
  /** @brief Whether a failed restore left the machine unusable. */
  bool invalid = false;
  /** @brief Owners of the stop bits. Each side clears only its own bit, so no clear erases a set
   * of the other side. */
  enum stop_bit : std::uint8_t
  {
    /** @brief Engine thread: a request, a failure or stale code is pending. */
    stop_internal = 1U << 0,
    /** @brief Any thread, through machine::interrupt(): end the slice at the next boundary. */
    stop_mail = 1U << 1,
  };
  /** @brief The bits the block hook tests. */
  std::atomic<std::uint8_t> stop_bits{};

  /**
   * @brief Sets the engine thread's bit, also from another thread through do_request_stop(): the
   *        release pairs with clear_internal_stop(), so a clear that erases this set sees the
   *        request made before it.
   */
  void raise_stop() noexcept
  {
    stop_bits.fetch_or(stop_internal, std::memory_order_release);
  }

  /** @brief Clears the engine thread's bit, leaving a mail bit set. */
  void clear_internal_stop() noexcept
  {
    stop_bits.fetch_and(static_cast<std::uint8_t>(~stop_internal), std::memory_order_acq_rel);
  }

  /** @brief Whether either bit is set. */
  [[nodiscard]] auto stop_pending() const noexcept -> bool
  {
    return stop_bits.load(std::memory_order_relaxed) != 0;
  }

  /** @brief Clears the mail bit. @return Whether it was set. */
  auto take_mail() noexcept -> bool
  {
    return (stop_bits.fetch_and(static_cast<std::uint8_t>(~stop_mail), std::memory_order_relaxed)
            & stop_mail)
        != 0;
  }
  /** @brief Stop requested through machine_services, from any thread, under requested_mutex.
   * Kept across slices until a slice takes it. */
  std::optional<requested_stop> requested{};
  /** @brief Whether a stop was requested. */
  [[nodiscard]] auto stop_requested() const noexcept -> bool
  {
    return has_requested.load(std::memory_order_acquire);
  }

  /** @brief Takes the requested stop, under requested_mutex when there is one. */
  auto take_requested() -> std::optional<requested_stop>
  {
    if(not stop_requested())
    {
      return std::nullopt;
    }
    std::scoped_lock const lock{requested_mutex};
    has_requested.store(false, std::memory_order_relaxed);
    return std::exchange(requested, std::nullopt);
  }
  /** @brief Reset requested through machine_services. */
  std::optional<reset_kind> reset_requested{};
  /** @brief Slice end requested through machine_services. */
  bool end_requested = false;
  /** @brief Stop an arch trap handler returned from inside the engine. */
  std::optional<arch::trap_result> trap{};
  /** @brief Access the engine refused, handled between engine runs. */
  std::optional<unmapped_access> unmapped{};
  /** @brief First bus error a model returned on a CPU access in the engine run, W5f. */
  std::optional<unmapped_access> bus_fault{};
  /** @brief Intercepts by handle id. */
  std::map<std::uint64_t, std::unique_ptr<intercept_entry>> intercepts{};
  /** @brief Return hooks by handle id. */
  std::map<std::uint64_t, std::unique_ptr<return_entry>> return_hooks{};
  /** @brief Probed address by handle id. */
  std::map<std::uint64_t, address> probes{};
  /** @brief Watches by handle id. */
  std::map<std::uint64_t, std::unique_ptr<watch_entry>> watches{};
  /** @brief Ranges watched by machine_services::watch_reads, by buffer and inclusive offsets,
   * with the controller model of the region each was named in. */
  std::map<std::tuple<host_buffer const*, std::size_t, std::size_t>, periph::model*>
      controller_ranges{};
  /** @brief Runs of controller_ranges, by buffer and first span offset. */
  std::map<std::pair<host_buffer const*, std::size_t>, controller_run> controller_runs{};
  /** @brief Hooked engine pages, every page a run's span touches, by buffer and page index. */
  std::map<std::pair<host_buffer const*, std::size_t>, controller_page> controller_pages{};
  /** @brief Engine page size in bytes, read on the first watch_reads(). */
  std::size_t controller_page_bytes{};
  /** @brief Serial of the last controller hook added. */
  std::uint64_t controller_hook_serial{};
  /** @brief Guard against a hook added during an on_memory_read call reporting its load. */
  controller_reentry controller_guard{};
  /** @brief Trace sinks by handle id. */
  std::map<std::uint64_t, std::unique_ptr<sink_entry>> sinks{};
  /** @brief Address of the block the engine runs, the pc MMIO callbacks see without the exact
   * pc hook, read by mmio records instead of the engine register. */
  address block_pc{};
  /** @brief Instructions of that block, as the block hook counted them, also while stepping. */
  std::uint32_t block_size{};
  /** @brief Whether the MMIO write being served comes from the CPU through a window whose model
   * drives the core clock, so that pc is exact. */
  bool exact_write = false;
  /** @brief Bus attributes of the CPU access the MMIO callbacks serve, kept after it. */
  uc_mem_attrs access{};
  /** @brief Whether the firewall refused the CPU access being dispatched, until its MMIO
   * callback reads it or the host access that dispatched it returns. */
  bool firewall_refused = false;
  /** @brief Whether the last engine run ended at a block boundary through the block hook, so
   * the next run resumes the block the edge chain expects. */
  bool boundary_stop = false;
  /** @brief Whether arrive_emulated() in the running trap ended the run before the emulated
   * instruction, so on_interrupt stops the engine once the arch resumes. */
  bool arrival_stopped = false;
  /** @brief slice_request::until_thread of the running slice: an exception return to thread
   * mode ends the engine run. Cleared with the slice. */
  bool until_thread = false;
  /** @brief slice_request::until_return of the running slice. */
  std::optional<frame_return> until_return{};
  /** @brief until_return's ret, the one value the block hook compares, 0 for none. */
  address return_block{};
  /** @brief The until_return an exception return met in on_interrupt, the stop taken by
   * bound_reached once the core unstacked, again at each slice start of the same goal, as a
   * block-path return is found again, until the first slice of the next goal drops it. */
  std::optional<frame_return> returned_at{};
  /** @brief Whether an access in the engine run asked the engine to stop right after its
   * instruction, set by request_precise_exit() and taken by take_precise_exit(). */
  bool precise_exit = false;
  /** @brief Whether spin detection runs, machine_options::spin_threshold above 0. */
  bool spin_on = false;
  /** @brief Spin detection state. */
  spin_state spin{};
  /** @brief Events dispatched at one instruction count, beyond which the run stops: a handler
   * that reschedules itself at the same count would otherwise never let time pass. */
  static constexpr std::uint32_t max_events_per_instant = 65536;
  /** @brief Instruction count the events of instant_events were dispatched at. */
  icount instant{};
  /** @brief Events dispatched at instant, across slices. */
  std::uint32_t instant_events = 0;
  /** @brief Whether coverage is collected, tested by the block hook. */
  bool coverage_on = false;
  /** @brief Coverage counters. */
  coverage_state coverage{};
  /** @brief Whether some sink selects block records. */
  bool sinks_want_block = false;
  /** @brief Whether block records go to the trace ring. */
  bool ring_blocks = false;
  /** @brief sinks_want_block or ring_blocks, the one flag the block hook tests. */
  bool block_records = false;
  /** @brief Whether the mem record hooks and their exact pc reference are held. */
  bool sink_mem_active = false;
  /** @brief Memory hooks feeding mem records while some sink selects them. */
  std::vector<uc_hook> sink_mem_hooks{};
  /** @brief Symbols of machine::observed_images(), concatenated in that order. */
  std::vector<popcorn::image::symbol const*> symbols{};
  /** @brief Symbols of the loaded images by address. */
  symbol_lookup symbol_index{};
  /** @brief Symbols of the symbols-only images by address, asked where the loaded ones give
   * none. */
  symbol_lookup extra_symbol_index{};
  /** @brief Code hook sites by address. */
  std::map<address, std::unique_ptr<code_site>> sites{};
  /** @brief Calls in flight, innermost last. */
  std::vector<return_frame> frames{};
  /** @brief Return address of the innermost frame, what the block hook compares. */
  address return_watch{};
  /** @brief Whether return_watch or until_return is armed, the one flag the block hook tests
   * before either compare. */
  bool block_watch = false;

  /** @brief Recomputes block_watch after return_watch or until_return changed. */
  void rewatch() noexcept
  {
    block_watch = return_watch != 0 or until_return.has_value();
  }
  /** @brief Site address a stop left pc on and index of the entry after the stopping one: the
   * site resumes there once when the run restarts at that address. */
  std::optional<std::pair<address, std::size_t>> resume_past{};
  /** @brief Engine MMIO windows, sorted by base. */
  std::vector<std::unique_ptr<mmio_window>> windows{};
  /** @brief Memory hook making MMIO pc exact while a consumer needs it. */
  exact_pc_hook exact_pc{};
  /** @brief Store hook over the windows of models that drive the core clock, installed for the
   * machine's life. */
  exact_pc_hook clock_pc{};
  /** @brief Clock value at which the block hook stops the engine. */
  icount deadline{};
  /** @brief Instructions executed in the slice. */
  icount executed{};
  /** @brief Instructions the clock moved by idle jumps in the slice. */
  icount jumped{};
  /** @brief Blocks left before the next wall clock read. */
  std::uint32_t wall_countdown = wall_check_blocks;
  /** @brief Wall time at which the slice ends. */
  std::chrono::steady_clock::time_point wall_deadline{};
  /** @brief Whether the wall budget ended the engine run. */
  bool wall_expired = false;
  /** @brief First exception a callback raised in the slice. */
  std::exception_ptr failure{};
  /** @brief Whether the core is held, machine::hold_core(). */
  bool core_held = false;
  /** @brief Count up to which a hold_from_until slice left events held, machine::held_through().
   */
  std::optional<icount> held_through{};
  /** @brief `description_digest()` of the machine's description, computed once. */
  std::string description_digest{};
  /** @brief Code written while the engine ran, removed at the next block boundary. */
  std::vector<stale_range> stale_code{};
  /** @brief Whether `uc_emu_start` is on the stack. */
  bool in_engine = false;
  /** @brief Whether the tail of exact_instructions is single-stepped. */
  bool stepping = false;
  /** @brief Per-instruction hook while stepping. */
  uc_hook step_hook{};
  /** @brief Exit addresses set in the engine. */
  std::vector<address> exits{};
  /** @brief Windows watched for guest stores, fixed after construction. */
  std::vector<guest_window> guest_windows{};
  /** @brief Line bus of every peripheral's IRQ lines. */
  machine_lines lines{};
  /** @brief Peripheral names, aliases included, sorted, to the description index of the
   * peripheral serving them. */
  std::vector<std::pair<std::string_view, std::uint32_t>> peripheral_index{};
  /** @brief Models of the machine for model contexts, empty until attach_peripherals ends. */
  machine_peers peers{};
  /** @brief Bus firewall a model installed, asked before every peripheral window access, null
   * when none. Set only while attach_peripherals runs. */
  periph::bus_firewall* firewall{};
  /** @brief Peripheral whose model attach_peripherals is building, the only time a firewall may
   * be installed, null otherwise. */
  peripheral_desc const* building{};
  /** @brief Whether the firewall filters memory, its engine filter installed by install_hooks. */
  bool memory_filter{};
  /** @brief Whether the arch reset runs, before the models reset: its vector reads are not
   * filtered, since the firewall still holds the policy from before the reset. */
  bool resetting_arch{};
  /** @brief Engine page size in bytes, read when the memory filter is installed. */
  std::uint32_t filter_page_bytes{};
  /** @brief Generation of the memory policy, which memory_grant holders compare: bumped by
   * every memory_policy_changed, restore and reset. */
  std::uint64_t memory_generation{1};

  /** @brief Whether @p grant is current and holds every byte of the access. */
  [[nodiscard]] auto
      granted(memory_grant const& grant, address addr, std::uint8_t size) const noexcept -> bool
  {
    return grant.generation == memory_generation
       and addr >= grant.first
       and std::uint64_t{addr} + size - 1u <= grant.last;
  }

  /**
   * @brief A grant for an access check_memory() just allowed: the region, or the part of it the
   *        firewall's bus_firewall::allowed_extent gives when it filters the access.
   * @param region Region of the access.
   */
  [[nodiscard]] auto grant(
      region_desc const& region,
      address            addr,
      std::uint8_t       size,
      bus_attrs          attrs,
      arch::memory_op    op) -> memory_grant;

  /** @brief Installs the engine's bus filter when the firewall filters memory. */
  void install_memory_filter();

  /**
   * @brief Asks the firewall about a host memory access, when it filters memory and @p attrs
   *        is not an unfiltered debugger access.
   * @return The verdict, `allow` when nothing filters the access.
   */
  [[nodiscard]] auto
      check_memory(address addr, std::uint8_t size, bus_attrs attrs, arch::memory_op op)
          -> periph::bus_verdict;

  /** @brief Whether host accesses with @p attrs go through check_memory one at a time: CPU
   * and DMA accesses, and filtered debugger accesses. */
  [[nodiscard]] auto filters_host(bus_attrs attrs) const noexcept -> bool
  {
    return memory_filter
       and not resetting_arch
       and (attrs.by != initiator::debugger or attrs.filtered);
  }

  /**
   * @brief Installs the machine's bus firewall.
   * @param installed Firewall.
   * @throws model_error naming the peripheral being built when a firewall is installed already,
   *         and naming no peripheral outside attach_peripherals.
   */
  void install_firewall(periph::bus_firewall& installed);

  /**
   * @brief Attributes of an access as models and the firewall see them: not Secure on a machine
   *        without TrustZone, unchanged otherwise.
   * @param attrs Attributes the master gave.
   */
  [[nodiscard]] auto seen(bus_attrs attrs) const noexcept -> bus_attrs;

  /**
   * @brief Context a model sees for an access.
   * @param attrs Attributes of the access, as seen() returns them.
   * @param via_secure_alias Whether the access came through a secure alias.
   */
  [[nodiscard]] auto context_of(bus_attrs attrs, bool via_secure_alias) const -> periph::access_ctx;

  /**
   * @brief Builds one slot per peripheral of the description, aliases excepted, its lines, its
   *        context and its model, in description order. A cross rule whose source register the
   *        model claims never runs in the register layer, which only sees unclaimed registers:
   *        the model owns it, whatever its target.
   * @throws config_error when an alias names no peripheral, when a model claims a register its
   *         peripheral does not have, or when a cross rule from an unclaimed register would
   *         write a register the model claims.
   */
  void attach_peripherals();

  /**
   * @brief Maps every peripheral window, aliases included, as engine MMIO: one map per window
   *        alone in its engine pages, one map per run of pages windows share.
   * @throws config_error when two peripherals overlap, when the first window of its pages does
   *         not start on an engine page, or when a window sharing a page is not word-aligned.
   */
  void map_peripherals();

  /** @brief Window holding [@p addr, @p addr + @p size), null when none. */
  [[nodiscard]] auto window_at(address addr, std::uint8_t size) const noexcept -> mmio_window*;

  /**
   * @brief Serves a read of a peripheral window: the model for a claimed register, the register
   *        layer otherwise.
   * @param window Window accessed.
   * @param offset Byte offset in the window, aligned to @p size.
   * @param size Access size in bytes, 1, 2 or 4.
   * @param attrs Bus master and attributes of the access, as the master gave them.
   * @return Value right-aligned to @p size, or the firewall's or the model's bus error for a DMA
   *         or debugger access. A CPU access records the error and reads 0.
   * @throws model_error on every unmodelled access under strict_peripherals.
   */
  auto dispatch_read(
      mmio_window const& window,
      std::uint32_t      offset,
      std::uint8_t       size,
      bus_attrs          attrs) -> std::expected<std::uint32_t, bus_error>;

  /**
   * @brief Serves a write of a peripheral window, as dispatch_read().
   * @throws model_error on every unmodelled access under strict_peripherals.
   */
  auto dispatch_write(
      mmio_window const& window,
      std::uint32_t      offset,
      std::uint8_t       size,
      std::uint32_t      value,
      bus_attrs          attrs) -> std::expected<void, bus_error>;

  /**
   * @brief Hands a store into a read-only region to the model of its controller, a 64-bit store
   *        as two 4-byte stores, low address first, the second skipped after a bus error. A
   *        store another master made and the model took is reported to the watches: CPU stores
   *        reach them through the engine's write hook.
   * @param region Region holding every byte of the store, its controller set.
   * @param addr Address of the store.
   * @param size Store size in bytes, 1, 2, 4 or 8.
   * @param value Value stored, right-aligned to @p size.
   * @param attrs Bus master and attributes of the store, never the debugger.
   */
  auto dispatch_controller_write(
      mapped_region const& region,
      address              addr,
      std::uint8_t         size,
      std::uint64_t        value,
      bus_attrs attrs) -> std::expected<void, bus_error> pre(region.controller != nullptr)
      pre(attrs.by != initiator::debugger) pre(size == 1 or size == 2 or size == 4 or size == 8);

  /** @brief Lowers the engine deadline to the earliest event after a model call, and ends the
   * engine run when the call changed the instruction rate. */
  void after_model(std::uint64_t instruction_hz);

  /** @brief Instructions of the running block after the current one, charged to the clock but
   * not retired yet, 0 unless an exact CPU write to a clock-driving model is being served. */
  [[nodiscard]] auto charged_ahead() const -> std::uint64_t;

  /** @brief Asks the engine to stop right after the instruction being executed, so a pending
   * interrupt an access of that instruction made deliverable is taken before the next one. */
  void request_precise_exit();

  /** @brief Serves a CPU access from an engine callback through @p access, then asks for a
   * precise exit when the access made a pending interrupt deliverable. */
  template <typename Access> auto raising_access(Access access) -> decltype(access());

  /** @brief Records the first model bus error of a CPU access in the engine run. */
  void record_bus_fault(address addr, arch::memory_op op) noexcept;

  /**
   * @brief Answers a CPU access a dispatch failed, from its engine MMIO callback: a firewall
   *        refusal refuses the engine access, which faults precisely at the instruction, any
   *        other bus error is recorded with record_bus_fault().
   * @param addr Address of the access.
   * @param op Kind of the access.
   */
  void cpu_bus_error(address addr, arch::memory_op op);

  /**
   * @brief Logs the first access to a register of a peripheral without model, once per machine
   *        life, restore included. An arch or model debugger access under strict_peripherals is
   *        neither logged nor refused.
   * @throws model_error under strict_peripherals, on every CPU or DMA access.
   */
  void note_unmodelled(mmio_window const& window, std::uint32_t offset, bool write, initiator by);

  /** @brief Logs the first access to an offset without register. */
  void warn_unregistered(mmio_window const& window, std::uint32_t offset, bool write, initiator by);

  /**
   * @brief Refuses a host view access, memory_view, to data no model serves: a register of a
   *        peripheral without a model, a claimed register its model calls storage only, an offset
   *        no register covers, a raz_wi range. Logs
   *        nothing. Scans up to the first byte outside every region, window and range, where the
   *        access itself fails.
   * @param addr First guest address of the access.
   * @param size Bytes of the access.
   * @throws model_error naming the peripheral, the register when there is one, and the offset,
   *         or the range with an empty register.
   */
  void refuse_unmodelled(address addr, std::size_t size) const;

  static auto on_mmio_read(
      uc_struct*    engine,
      std::uint64_t offset,
      unsigned      size,
      uc_mem_attrs  attrs,
      void*         user) noexcept -> std::uint64_t;
  static void on_mmio_write(
      uc_struct*    engine,
      std::uint64_t offset,
      unsigned      size,
      std::uint64_t value,
      uc_mem_attrs  attrs,
      void*         user) noexcept;

  /** @brief Engine pages several windows share, mapped once and dispatched by address. */
  struct mmio_page
  {
    /** @brief Run state serving the accesses. */
    run_state* run{};
    /** @brief Guest address of the first shared page. */
    address base{};
    /** @brief Windows in the pages, by base. */
    std::vector<mmio_window*> members{};

    /** @brief The window serving @p addr: the last one whose base is at or below it. */
    [[nodiscard]] auto member_at(address addr) const noexcept -> mmio_window&;
  };

  /** @brief Shared pages, each mapped once. */
  std::vector<std::unique_ptr<mmio_page>> shared_pages{};

  /** @brief A machine_options::raz_wi range, mapped as engine MMIO. */
  struct raz_window
  {
    /** @brief Run state serving the accesses. */
    run_state* run{};
    /** @brief The range. */
    address_range const* range{};
    /** @brief Whether its first access was logged. */
    bool warned{};
  };

  /** @brief The machine_options::raz_wi windows, in option order. */
  std::vector<std::unique_ptr<raz_window>> raz_windows{};

  /**
   * @brief Maps every machine_options::raz_wi range as engine MMIO.
   * @return The ranges, first and last address each, for the exact pc hook.
   * @throws config_error keyed `raz_wi[i]` for a range machine_options::raz_wi refuses.
   * @throws emulation_error when the engine refuses a mapping.
   */
  auto map_raz_wi(std::uint32_t page) -> std::vector<std::pair<std::uint64_t, std::uint64_t>>;

  /**
   * @brief Logs the first access to @p window, once per machine life, restore included. A
   *        debugger access under strict_peripherals is neither logged nor refused.
   * @throws model_error naming the range under strict_peripherals, on every CPU or DMA access.
   */
  void note_raz(raz_window& window, std::uint32_t offset, bool write, initiator by);

  /** @brief The machine_options::raz_wi window holding [@p addr, @p addr + @p size), null when
   * none. */
  [[nodiscard]] auto raz_at(address addr, std::uint8_t size) const noexcept -> raz_window*;

  static auto on_raz_read(
      uc_struct*    engine,
      std::uint64_t offset,
      unsigned      size,
      uc_mem_attrs  attrs,
      void*         user) noexcept -> std::uint64_t;
  static void on_raz_write(
      uc_struct*    engine,
      std::uint64_t offset,
      unsigned      size,
      std::uint64_t value,
      uc_mem_attrs  attrs,
      void*         user) noexcept;

  static auto on_shared_read(
      uc_struct*    engine,
      std::uint64_t offset,
      unsigned      size,
      uc_mem_attrs  attrs,
      void*         user) noexcept -> std::uint64_t;
  static void on_shared_write(
      uc_struct*    engine,
      std::uint64_t offset,
      unsigned      size,
      std::uint64_t value,
      uc_mem_attrs  attrs,
      void*         user) noexcept;

  /**
   * @brief Adds an intercept or return hook to the code site of its address, creating the site
   *        and its engine hook, and removing the translated code there, on first use.
   * @throws emulation_error when the engine refuses the hook.
   */
  void add_to_site(address at, std::variant<intercept_entry*, return_entry*> entry);

  /**
   * @brief Adds a probe to the code site of its address, as add_to_site() does.
   * @throws emulation_error when the engine refuses the hook.
   */
  void add_probe_to_site(address at, probe_entry entry);

  /**
   * @brief The code site of @p at, created with its engine hook and the translated code there
   *        removed when it does not exist yet.
   * @throws emulation_error when the engine refuses the hook.
   */
  auto site_at(address at) -> code_site&;

  /** @brief How a visit of a code site ended: on to the instruction, returned by an intercept,
   * or stopped by one. */
  enum class site_visit : std::uint8_t
  {
    proceed,
    returned,
    stopped,
  };

  /**
   * @brief Runs the probes, intercepts and return hooks of @p site, arrived at @p addr, as its
   *        code hook does. An intercept return calls arch::return_to_caller() and leaves
   *        arch::return_interrupt() to the caller. A stop records the trap and the resume point.
   * @param site Site of @p addr.
   * @param addr Address reached.
   * @param in_block Whether a counted block holds @p addr, so a return closes its coverage.
   */
  auto visit_site(code_site& site, address addr, bool in_block) -> site_visit;

  /** @brief machine_services::emulated_instruction() during a run. */
  auto arrive_emulated(address at) -> emulated_arrival;

  /** @brief Whether the stepping hook ends the engine run before the instruction it sees at
   * instruction count @p now. */
  [[nodiscard]] auto step_ends(icount now) const noexcept -> bool
  {
    return stop_pending() or now >= deadline or now >= owner.scheduler_.earliest();
  }

  /** @brief Virtual time at the start of the instruction at @p addr, which the code hook of
   * the running block reports. A clock rate write does not end the block. A CPU write to a
   * model that drives the core clock starts the new epoch inside the block, before any later
   * arrival in it, so the arrival takes the time a parked run to @p addr stops at. A rate
   * change by another initiator during the block starts the epoch at the block's charged end:
   * an arrival before that is raised to the epoch start, at most one block late. */
  [[nodiscard]] auto arrival_time(address addr) const -> vtime;

  /** @brief Removes the engine side and the entry of a hook by handle id, between slices. */
  void remove_hook(std::uint64_t id) noexcept;

  /** @brief Floating-point convention of the loaded image holding @p at, soft when none. */
  [[nodiscard]] auto abi_at(address at) const -> arch::float_abi;

  /** @brief Stack pointer, the arch register named `sp`. */
  [[nodiscard]] auto stack_pointer() const -> std::uint64_t;

  /** @brief Reports every innermost return frame a block about to run at @p addr completes. */
  void on_return_block(address addr);

  /** @brief Reports every innermost return frame the engine exception @p intno completes, as
   * arch::interrupt_returns() tells, before the arch handles it. */
  void on_exception_return(std::uint32_t intno);

  /**
   * @brief Drops the frames the stack unwound past, then reports, innermost first, each frame
   *        at the current stack pointer whose return address @p returns accepts. When a handler
   *        throws, drops the frames still matching without reporting them, then rethrows.
   */
  void complete_returns(std::function_ref<bool(address) const> returns);

  /** @brief Pops frames the stack has unwound past and recomputes return_watch. */
  void refresh_return_watch() noexcept;

  /**
   * @brief Reports an access to every watch it hits, as the bus and MMIO dispatch see it. Debugger
   *        accesses never hit.
   */
  void note_watched(address addr, std::uint8_t size, std::uint32_t value, bool write, initiator by);

  /**
   * @brief Copies each saved region into its host buffer. Buffers mapped executable anywhere are
   *        compared page by page, only changed pages are copied, and the translated code over
   *        each run of changed pages is removed at every mapping of the buffer, W5g.
   * @throws emulation_error when the engine refuses a removal.
   */
  void restore_memory(std::span<std::pair<mapped_region const*, memory_image const*> const> plan);

  /** @brief Restores intercept and return hook counters, matched by address in add order.
   * Hooks without a saved state start over. */
  void restore_hook_state(
      std::span<intercept_state const>   saved_intercepts,
      std::span<return_hook_state const> saved_returns);

  /**
   * @brief Hands a record the machine produced to the ring, when @p ring, and to every sink
   *        selecting its kind whose ranges hold @p pc, stamped at the current count.
   */
  void emit(trace_record const& record, address pc, bool ring);

  /**
   * @brief Records an MMIO access: to the ring for a CPU access, to the sinks for any, with
   *        pc 0 for DMA and debugger accesses.
   */
  void trace_mmio(
      peripheral_slot const& slot,
      address                addr,
      std::uint32_t          offset,
      std::uint8_t           size,
      std::uint32_t          value,
      bool                   write,
      initiator              by);

  /**
   * @brief Tracks loops at a block starting at @p addr: at a backward branch within 256 bytes,
   *        compares the finished iteration with the previous one and requests a spin stop after
   *        machine_options::spin_threshold iterations without progress.
   */
  void spin_block(address addr);

  /** @brief Whether a pending event belongs to the model of a peripheral the loop polls. */
  [[nodiscard]] auto model_due() const -> bool;

  /** @brief Records a CPU MMIO read into the iteration of the loop tracked at the current
   * exception depth, callees included, at most spin_state::max_reads per iteration. */
  void spin_read(
      peripheral_slot const& slot,
      address                addr,
      std::uint32_t          offset,
      std::uint32_t          value);

  /** @brief Text naming the loop, its function and the MMIO register it polls. */
  [[nodiscard]] auto spin_detail() const -> std::string;

  /** @brief Records an intercepted call at @p at: pc, r0 and the called symbol's index. */
  void trace_call(address at);

  /** @brief Delivers a sink's batch. */
  void flush(sink_entry& entry);

  /** @brief Delivers every sink's batch. */
  void flush_sinks();

  /** @brief Moves every sink's time base to @p now, each gets a time record first. */
  void rebase_sinks(icount now) noexcept;

  /** @brief Installs or removes the hooks that feed the selected kinds, between slices. */
  void update_sink_hooks();

  /** @brief Removes a sink, delivering its batch first, its failure dropped. */
  void remove_sink(std::uint64_t id) noexcept;

  /** @brief Rebuilds the symbol table and its address indexes from the observed images. */
  void build_symbol_index();

  /** @brief Address index of @p images, whose symbols are appended to the symbol table. */
  [[nodiscard]] auto index_symbols(std::span<popcorn::image::image const> images) -> symbol_lookup;

  /** @brief Symbol index and offset of the symbol holding @p pc, empty when none does: the
   * loaded images first, then the symbols-only images. */
  [[nodiscard]] auto symbol_at(address pc) const noexcept
      -> std::optional<std::pair<std::uint32_t, std::uint32_t>>;

  /** @brief Location of @p pc: symbol and offset from the symbol index, file and line from the
   * last loaded image whose segments cover @p pc. */
  [[nodiscard]] auto locate(address pc) const -> location;

  static void on_sink_access(
      uc_struct*    engine,
      uc_mem_type   type,
      std::uint64_t addr,
      int           size,
      std::int64_t  value,
      void*         user) noexcept;

  /** @brief Guest ranges reaching [@p at, @p last] through every mapping of the same bytes. */
  [[nodiscard]] auto watch_spans(address at, std::uint64_t last) const
      -> std::vector<std::pair<std::uint64_t, std::uint64_t>>;

  /** @brief Calls a watch handler and records a deferred watchpoint stop when it asks. */
  void hit_watch(watch_entry& watch, watch_hit const& hit);

  /**
   * @brief Controller model of the run holding buffer offset @p first of @p buffer when one of
   *        its ranges overlaps [@p first, @p last], null otherwise.
   */
  [[nodiscard]] auto controller_read_hit(
      host_buffer const* buffer,
      std::size_t        first,
      std::size_t        last) const noexcept -> periph::model*;

  /**
   * @brief Reports a DMA load to the controller model watching its bytes, if any.
   * @param region Region holding every byte of the load.
   * @param addr Address loaded.
   * @param size Load size in bytes.
   * @param attrs Bus master and attributes of the load.
   */
  void note_controller_read(
      mapped_region const& region,
      address              addr,
      std::uint8_t         size,
      bus_attrs            attrs);

  /**
   * @brief Rebuilds the runs of @p buffer from controller_ranges, hooks every engine page a
   *        run touches that has no hooks yet, then removes the hooks of the pages no run
   *        touches. Nothing changes when a hook is refused.
   * @throws emulation_error when the engine refuses a hook.
   */
  void sync_controller_runs(host_buffer const* buffer);

  /**
   * @brief Hooks engine page @p page of @p buffer, one hook per guest span mapping it.
   * @throws emulation_error when the engine refuses a hook, every hook of the page removed.
   */
  auto new_controller_page(host_buffer const& buffer, std::size_t page) -> controller_page;

  /** @brief Removes the engine hooks of a page. */
  void drop_controller_hooks(controller_page& page) noexcept;

  /**
   * @brief After an on_memory_read call that added hooks, arms controller_guard with the hook
   *        made during the call over the span of @p region holding buffer offset @p first, if
   *        any.
   */
  void arm_controller_guard(
      host_buffer const*   buffer,
      mapped_region const* region,
      std::size_t          first,
      std::uint64_t        before,
      std::uint64_t        addr,
      int                  size) noexcept;

  /**
   * @brief Whether a run of @p buffer reaches buffer offsets [@p first, @p last].
   */
  [[nodiscard]] auto controller_block_hit(
      host_buffer const* buffer,
      std::size_t        first,
      std::size_t        last) const noexcept -> bool;

  static void
      on_code_site(uc_struct* engine, std::uint64_t addr, std::uint32_t size, void* user) noexcept;
  static void on_watch_access(
      uc_struct*    engine,
      uc_mem_type   type,
      std::uint64_t addr,
      int           size,
      std::int64_t  value,
      void*         user) noexcept;
  static void on_controller_read(
      uc_struct*    engine,
      uc_mem_type   type,
      std::uint64_t addr,
      int           size,
      std::int64_t  value,
      void*         user) noexcept;

  /**
   * @brief Takes a reference on the exact pc hook, between slices only.
   * @throws emulation_error when the engine refuses the hook.
   */
  void acquire_exact_pc() pre(not in_engine)
  {
    exact_pc.acquire();
  }

  /** @brief Drops a reference on the exact pc hook, between slices only. */
  void release_exact_pc() noexcept pre(not in_engine)
  {
    exact_pc.release();
  }

  /**
   * @brief Slot serving a peripheral name.
   * @throws config_error when the description has no such peripheral.
   */
  [[nodiscard]] auto slot(std::string_view name) const -> peripheral_slot&;

  /** @brief Address the arch module's reset reads the vector table at. */
  address reset_vector{};
  /** @brief Context handed to arch::attach(), which the module may keep. */
  std::optional<arch::arch_context> arch_context{};

  /**
   * @brief Installs the block, trap and guest store hooks and enables exits, once, after
   *        arch::attach().
   * @throws emulation_error when the engine refuses a hook.
   */
  void install_hooks();

  /**
   * @brief Runs one slice, what machine::run_slice() does.
   * @param request Bounds and stop conditions.
   */
  auto run(slice_request const& request) -> slice_result;

  /**
   * @brief Records code bytes that changed: removed at once between engine runs, at the next
   *        block boundary from inside one.
   * @throws emulation_error when the engine refuses an immediate removal.
   */
  void code_changed(host_buffer const& buffer, std::size_t first, std::size_t last);

  /**
   * @brief Resets the machine as @p kind in the W5a order, and drops the per-run state of the
   *        execution the reset discards: refused access, trap, stepping.
   */
  void reset_machine(reset_kind kind);

  /**
   * @brief Calls every model's `power_cut()` in description order, inside a handler scope,
   *        going on past a throw.
   * @return The first exception a model threw, null when none did.
   */
  [[nodiscard]] auto power_cut_models() noexcept -> std::exception_ptr;

  /**
   * @brief Zeroes every RAM region without `physical`, its aliases with it, and drops the
   *        translated code over it. The scratch region is kept.
   * @throws emulation_error when the engine refuses the removal of translated code.
   */
  void zero_ram();

  /** @brief Removes the translated code of every stale range, then clears the flag when nothing
   * else is pending. */
  void drain_stale_code();

  /** @brief Whether a request, trap, failure or refused access waits for the run loop. */
  [[nodiscard]] auto payload_pending() const noexcept -> bool;

  /**
   * @brief Runs a callback body, keeping the first exception in the slot and raising the flag.
   * @return The body's result, true when it threw.
   */
  template <typename F> auto guarded(F&& body) noexcept -> bool
  {
    try
    {
      return std::forward<F>(body)();
    }
    catch(...)
    {
      if(not failure)
      {
        failure = std::current_exception();
      }
      raise_stop();
      return true;
    }
  }

  /** @brief Why the block hook stops the engine at clock @p now, in the order its condition
   * tests, only once a stop is due. */
  [[nodiscard]] auto boundary_cause(icount now) const noexcept -> engine_end;

  /** @brief Reads the wall clock and restarts the countdown, off the block hook's hot path. */
  [[gnu::noinline]] auto wall_expired_now() noexcept -> bool;

  /** @brief Asks the arch whether a pending interrupt is deliverable, under the guard, off the
   * block hook's hot path. */
  [[gnu::noinline]] auto deliverable() noexcept -> bool;

  /** @brief Ends the engine run at the block starting at block_pc, before it runs, for
   * @p cause. Off the block hook's hot path. */
  [[gnu::noinline]] void stop_at_block(uc_struct* engine, engine_end cause) noexcept;

  /** @brief stop_at_block for the boundary_cause of clock @p now. Off the block hook's hot path,
   * so the hook keeps no register across a call. */
  [[gnu::noinline]] void stop_at_boundary(uc_struct* engine, icount now) noexcept;

  /** @brief Completes the watched returns at block_pc and stops the engine when the until
   * return is reached there. Off the block hook's hot path.
   * @return Whether the engine was stopped. */
  [[gnu::noinline]] auto watch_block(uc_struct* engine) noexcept -> bool;

  /** @brief spin_block for the block at block_pc, under the guard, off the hot path. */
  [[gnu::noinline]] void spin_at_block() noexcept;

  /** @brief Counts the block at block_pc of block_size instructions into coverage, under the
   * guard, off the hot path. */
  [[gnu::noinline]] void count_block() noexcept;

  /** @brief Emits the block record of the block at block_pc, under the guard, off the hot
   * path. */
  [[gnu::noinline]] void record_block() noexcept;

  /** @brief After a trap hook resumed: whether it left a deliverable exception pending, then
   * marked as a block-boundary stop so the loop enters the handler. A fetch abort forms no
   * block, so no block hook would ever see the pending exception.
   * @return Whether the caller must stop the engine. */
  auto stop_on_pending() -> bool;

  static void
      on_block(uc_struct* engine, std::uint64_t addr, std::uint32_t size, void* user) noexcept;
  static void
      on_step(uc_struct* engine, std::uint64_t addr, std::uint32_t size, void* user) noexcept;
  static void on_interrupt(uc_struct* engine, std::uint32_t intno, void* user) noexcept;
  static auto on_invalid_insn(uc_struct* engine, void* user) noexcept -> bool;
  static auto on_bad_access(
      uc_struct*    engine,
      uc_mem_type   type,
      std::uint64_t addr,
      int           size,
      std::int64_t  value,
      void*         user) noexcept -> bool;
  static void on_guest_store(
      uc_struct*    engine,
      uc_mem_type   type,
      std::uint64_t addr,
      int           size,
      std::int64_t  value,
      void*         user) noexcept;
  static auto on_bus_page(
      uc_struct*    engine,
      std::uint64_t page,
      std::uint64_t addr,
      uc_mem_attrs  attrs,
      uc_mem_type   type,
      void*         user) noexcept -> unsigned;
  static auto on_bus_access(
      uc_struct*     engine,
      std::uint64_t  addr,
      unsigned       size,
      uc_mem_attrs   attrs,
      uc_mem_type    type,
      std::uint64_t* value,
      void*          user) noexcept -> int;

  void               clear_slice() noexcept;
  void               set_exits(std::vector<address> list);
  void               update_stepping(slice_request const& request);
  void               remove_step_hook() noexcept;
  [[nodiscard]] auto next_deadline(slice_request const& request) const -> icount;
  [[nodiscard]] auto handle_requests() -> std::optional<slice_end>;
  [[nodiscard]] auto dispatch_due(slice_request const& request) -> std::optional<slice_end>;
  /** @brief Counts an event dispatched now, a spin stop naming its owner once the count at this
   * instant reaches max_events_per_instant. */
  [[nodiscard]] auto count_instant(event const& due) -> std::optional<slice_end>;
  /** @brief Evaluates the request's stop_when as a handler: views work, state changes throw
   * machine_running. */
  [[nodiscard]] auto asks_stop(slice_request const& request) -> bool;
  [[nodiscard]] auto idle_jump(slice_request const& request) -> std::optional<slice_end>;
  [[nodiscard]] auto exit_list(slice_request const& request) const -> std::vector<address>;
  [[nodiscard]] auto bound_reached(slice_request const& request) -> std::optional<slice_end>;
  /** @brief Whether @p pc is until or an address of until_also. */
  [[nodiscard]] static auto at_until(slice_request const& request, address pc) -> bool;
  /** @brief Whether the until address the pc stands on must not stop the slice yet: leave_until
   * is set, nothing executed, and the address is not in arrive_at. */
  [[nodiscard]] auto leaving_until(slice_request const& request) const -> bool;
  /** @brief Arms the engine to execute the block at the until address the pc stands on without
   * stopping on it: that address is out of the exits and the deadline is one instruction away, so
   * the next block entry ends the engine run, a branch to itself included, and no later hit of
   * until is skipped. */
  void               step_off_until(slice_request const& request);
  [[nodiscard]] auto trap_after(uc_err err) -> std::optional<slice_end>;
  [[nodiscard]] auto engine_run() -> uc_err;
  /** @brief After an engine run that requested a precise exit: when the engine took it inside
   * the running block, takes back the block's instructions after the one it completed, none
   * while stepping, where every instruction is charged as it runs. When it took it after the
   * block's last instruction, at a branch target, the block ran whole: no refund, and coverage
   * records no cut. When it took none, a fault or a trap ended the run: coverage keeps its cut. */
  void take_precise_exit();
  void rethrow_failure();
  void notify_core_hold(bool held);
  [[nodiscard]] auto
      make_stop(stop_reason reason, std::string detail, std::optional<fault_info> fault) const
      -> stop;
  [[nodiscard]] auto stop_from(arch::trap_result const& result) const -> slice_end;
  [[nodiscard]] auto finish(slice_request const& request, slice_end end) -> slice_result;

  /** @brief Guards requested. A leaf: nothing is taken under it. */
  mutable std::mutex requested_mutex;
  /** @brief Whether requested holds a stop, read without the mutex on the engine's hot path. */
  std::atomic<bool> has_requested{};
  /** @brief Why the engine run in progress was stopped, set by the hook that stopped it. */
  engine_end end_cause = engine_end::exit;
  /** @brief Engine work counters, last so they do not split the block hook's fields. */
  run_counters counters{};
};

/** @brief Read access to a machine's run state for native tests and benches. */
struct machine_internals
{
  /** @brief The engine work counters of @p target. */
  [[nodiscard]] static auto counters(machine const& target) noexcept -> run_counters const&
  {
    return target.run_->counters;
  }

  /** @brief The engine of @p target. */
  [[nodiscard]] static auto engine(machine const& target) noexcept -> uc_struct*
  {
    return target.engine_.get();
  }

  /** @brief Bus attributes of the last CPU access to a peripheral window of @p target. */
  [[nodiscard]] static auto access(machine const& target) noexcept -> uc_mem_attrs
  {
    return target.run_->access;
  }

  /** @brief The services of @p target, whose debugger accesses are the arch module's and the
   * models' own, not a host view's. */
  [[nodiscard]] static auto services(machine& target) noexcept -> machine_services&
  {
    return target;
  }

  /** @brief The arch module of @p target. */
  [[nodiscard]] static auto arch(machine const& target) noexcept -> arch::arch const&
  {
    return *target.arch_;
  }

  /** @brief The bus firewall a model of @p target installed, or null. */
  [[nodiscard]] static auto firewall(machine const& target) noexcept -> periph::bus_firewall*
  {
    return target.run_->firewall;
  }
};

/**
 * @brief Throws unless the machine is stopped.
 * @throws machine_running while a slice runs, handlers included.
 */
void require_stopped(machine const& target);

/**
 * @brief Reads @p registers through one batch.
 * @return Values in the order of @p registers.
 * @throws emulation_error when the engine refuses the read.
 */
auto read_registers(uc_struct* engine, std::span<arch::cpu_register const> registers)
    -> std::vector<std::uint64_t>;

/**
 * @brief Writes @p values to @p registers through one batch.
 * @throws emulation_error when the engine refuses the write.
 */
void write_registers(
    uc_struct*                          engine,
    std::span<arch::cpu_register const> registers,
    std::vector<std::uint64_t>          values) pre(values.size() == registers.size());

/**
 * @brief Power-on core clock of a machine.
 * @throws config_error when neither the options nor the description give one.
 */
auto core_clock_hz(part_description const& description, machine_options const& options)
    -> std::uint64_t;
}
