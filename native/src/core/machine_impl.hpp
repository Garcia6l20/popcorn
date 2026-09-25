#pragma once

#include "../periph/line_bus.hpp"
#include "../periph/register_layer.hpp"
#include "engine.hpp"
#include "memory_map.hpp"

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/machine.hpp>

#include <unicorn/unicorn.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
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
    if(running)
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

  void set_irq(std::uint32_t source, bool level) override
  {
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
};

/** @brief How a slice ends: with a stop, or empty when only a yield bound was reached. */
struct slice_end
{
  /** @brief Stop record, empty for a yield. */
  std::optional<stop> stopped{};
};

/** @brief Run state of a machine: slice bounds, deferred requests, hooks and exception slot. */
struct machine::run_state
{
  /** @brief Blocks between two wall clock reads, reading it every block costs 20 ns. */
  static constexpr std::uint32_t wall_check_blocks = 256;

  /** @brief Writable window of an executable buffer mapped more than once, watched for guest
   * stores: Unicorn invalidates translated code on a store only through the window its RAM block
   * lookup attributes the code to. */
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
    /** @brief Bytes mapped, the peripheral size rounded up to the engine page. */
    std::uint32_t size{};
    /** @brief Whether accesses through the window are secure: an alias window, a `SEC_`
     * instance of the peripheral it aliases. */
    bool secure{};
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

  /** @brief One engine code hook serving every intercept and return hook of an address. */
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
  };

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
  /** @brief The one flag the block hook tests: a request, a failure or stale code is pending. */
  bool stop_flag = false;
  /** @brief Stop requested through machine_services. */
  std::optional<requested_stop> requested{};
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
  /** @brief Watches by handle id. */
  std::map<std::uint64_t, std::unique_ptr<watch_entry>> watches{};
  /** @brief Code hook sites by address. */
  std::map<address, std::unique_ptr<code_site>> sites{};
  /** @brief Calls in flight, innermost last. */
  std::vector<return_frame> frames{};
  /** @brief Return address of the innermost frame, what the block hook compares. */
  address return_watch{};
  /** @brief Site address a stop left pc on and index of the entry after the stopping one: the
   * site resumes there once when the run restarts at that address. */
  std::optional<std::pair<address, std::size_t>> resume_past{};
  /** @brief Engine MMIO windows, sorted by base. */
  std::vector<std::unique_ptr<mmio_window>> windows{};
  /** @brief Memory hook making MMIO pc exact while a consumer needs it. */
  exact_pc_hook exact_pc{};
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
   * @brief Maps every peripheral window, aliases included, as engine MMIO, one map per window.
   * @throws config_error when two windows share an engine page.
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
   * @param by Bus master.
   * @return Value right-aligned to @p size, or the model's bus error for a DMA or debugger
   *         access. A CPU access records the error and reads 0.
   * @throws model_error on a first unmodelled access under strict_peripherals.
   */
  auto dispatch_read(
      mmio_window const& window,
      std::uint32_t      offset,
      std::uint8_t       size,
      initiator          by) -> std::expected<std::uint32_t, bus_error>;

  /**
   * @brief Serves a write of a peripheral window, as dispatch_read().
   * @throws model_error on a first unmodelled access under strict_peripherals.
   */
  auto dispatch_write(
      mmio_window const& window,
      std::uint32_t      offset,
      std::uint8_t       size,
      std::uint32_t      value,
      initiator          by) -> std::expected<void, bus_error>;

  /** @brief Lowers the engine deadline to the earliest event after a model call, and ends the
   * engine run when the call changed the instruction rate. */
  void after_model(std::uint64_t instruction_hz);

  /** @brief Records the first model bus error of a CPU access in the engine run. */
  void record_bus_fault(address addr, arch::memory_op op) noexcept;

  /**
   * @brief Logs the first access to a register of a peripheral without model. A debugger
   *        access under strict_peripherals is neither logged nor refused.
   * @throws model_error under strict_peripherals, on that first CPU or DMA access.
   */
  void note_unmodelled(mmio_window const& window, std::uint32_t offset, bool write, initiator by);

  /** @brief Logs the first access to an offset without register. */
  void warn_unregistered(mmio_window const& window, std::uint32_t offset, bool write, initiator by);

  static auto
      on_mmio_read(uc_struct* engine, std::uint64_t offset, unsigned size, void* user) noexcept
      -> std::uint64_t;
  static void on_mmio_write(
      uc_struct*    engine,
      std::uint64_t offset,
      unsigned      size,
      std::uint64_t value,
      void*         user) noexcept;

  /**
   * @brief Adds an intercept or return hook to the code site of its address, creating the site
   *        and its engine hook, and removing the translated code there, on first use.
   * @throws emulation_error when the engine refuses the hook.
   */
  void add_to_site(address at, std::variant<intercept_entry*, return_entry*> entry);

  /** @brief Removes the engine side and the entry of a hook by handle id, between slices. */
  void remove_hook(std::uint64_t id) noexcept;

  /** @brief Floating-point convention of the loaded image holding @p at, soft when none. */
  [[nodiscard]] auto abi_at(address at) const -> arch::float_abi;

  /** @brief Stack pointer, the arch register named `sp`. */
  [[nodiscard]] auto stack_pointer() const -> std::uint64_t;

  /** @brief Reports every innermost return frame a block about to run at @p addr completes. */
  void on_return_block(address addr);

  /** @brief Pops frames the stack has unwound past and recomputes return_watch. */
  void refresh_return_watch() noexcept;

  /**
   * @brief Reports an access to every watch it hits, as the bus and MMIO dispatch see it. Debugger
   *        accesses never hit.
   */
  void note_watched(address addr, std::uint8_t size, std::uint32_t value, bool write, initiator by);

  /** @brief Guest ranges reaching [@p at, @p last] through every mapping of the same bytes. */
  [[nodiscard]] auto watch_spans(address at, std::uint64_t last) const
      -> std::vector<std::pair<std::uint64_t, std::uint64_t>>;

  /** @brief Calls a watch handler and records a deferred watchpoint stop when it asks. */
  void hit_watch(watch_entry& watch, watch_hit const& hit);

  static void
      on_code_site(uc_struct* engine, std::uint64_t addr, std::uint32_t size, void* user) noexcept;
  static void on_watch_access(
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
      stop_flag = true;
      return true;
    }
  }

  /** @brief Reads the wall clock and restarts the countdown, off the block hook's hot path. */
  [[gnu::noinline]] auto wall_expired_now() noexcept -> bool;

  /** @brief Asks the arch whether a pending interrupt is deliverable, under the guard, off the
   * block hook's hot path. */
  [[gnu::noinline]] auto deliverable() noexcept -> bool;

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

  void               clear_slice() noexcept;
  void               set_exits(std::vector<address> list);
  void               update_stepping(slice_request const& request);
  void               remove_step_hook() noexcept;
  [[nodiscard]] auto next_deadline(slice_request const& request) const -> icount;
  [[nodiscard]] auto handle_requests() -> std::optional<slice_end>;
  [[nodiscard]] auto dispatch_due(slice_request const& request) -> std::optional<slice_end>;
  [[nodiscard]] auto idle_jump(slice_request const& request) -> std::optional<slice_end>;
  [[nodiscard]] auto exit_list(slice_request const& request) const -> std::vector<address>;
  [[nodiscard]] auto bound_reached(slice_request const& request) -> std::optional<slice_end>;
  [[nodiscard]] auto trap_after(uc_err err) -> std::optional<slice_end>;
  [[nodiscard]] auto engine_run() -> uc_err;
  void               rethrow_failure();
  [[nodiscard]] auto
      make_stop(stop_reason reason, std::string detail, std::optional<fault_info> fault) const
      -> stop;
  [[nodiscard]] auto stop_from(arch::trap_result const& result) const -> slice_end;
  [[nodiscard]] auto finish(slice_request const& request, slice_end end) const -> slice_result;
};

/**
 * @brief Throws unless the machine is stopped.
 * @throws machine_running while a slice runs, handlers included.
 */
void require_stopped(machine const& target);

/**
 * @brief Power-on core clock of a machine.
 * @throws config_error when neither the options nor the description give one.
 */
auto core_clock_hz(mcu_description const& description, machine_options const& options)
    -> std::uint64_t;
}
