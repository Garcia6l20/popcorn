#pragma once

#include <popcorn/core/clock.hpp>
#include <popcorn/core/scheduler.hpp>
#include <popcorn/core/types.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace popcorn::core
{
/** @brief Version of the snapshot_data layout, stamped by the machine and checked on restore. */
inline constexpr std::uint32_t snapshot_format_version = 2;

/** @brief Opaque state saved by one owner: the arch module or one peripheral model. */
struct snapshot_part
{
  /** @brief Owner: `arch` for the arch module, the peripheral name for a model. */
  std::string owner{};
  /** @brief Bytes the owner's `save()` appended, handed back to its `load()`. */
  std::vector<std::byte> data{};

  /** @brief Member-wise equality. */
  friend auto operator==(snapshot_part const&, snapshot_part const&) -> bool = default;
};

/**
 * @brief Content of one mapped memory region, the popcorn-owned host buffer mapped in the engine.
 *        Flash is included with RAM: a flash controller model programs it.
 */
struct memory_image
{
  /** @brief Region name, as the description spells it. */
  std::string region{};
  /** @brief First guest address. */
  address start{};
  /** @brief Region bytes, the whole region. */
  std::vector<std::byte> bytes{};

  /** @brief Member-wise equality. */
  friend auto operator==(memory_image const&, memory_image const&) -> bool = default;
};

/** @brief Value of one CPU register, by the name `arch::arch::registers()` gives it. */
struct register_value
{
  /** @brief Register name. */
  std::string name{};
  /** @brief Value, right-aligned to the register width. */
  std::uint64_t value{};

  /** @brief Member-wise equality. */
  friend auto operator==(register_value const&, register_value const&) -> bool = default;
};

/**
 * @brief Machine-held state of one peripheral: the storage words of its `periph::register_file`,
 *        as `peek()` shows them, and the levels of its IRQ and DMA request lines.
 */
struct peripheral_image
{
  /** @brief Peripheral name, as the description spells it. */
  std::string peripheral{};
  /** @brief Word at each word-aligned offset, from offset 0. */
  std::vector<std::uint32_t> words{};
  /** @brief Level of each `periph::irq_line`, in description order, 1 when active. */
  std::vector<std::uint8_t> irq_levels{};
  /** @brief Level of each `periph::dma_request`, in description order, 1 when active. */
  std::vector<std::uint8_t> dma_levels{};
  /** @brief Write-once lock bit of each word, bit `i % 32` of element `i / 32` for word `i`, set
   * after the first write since reset. Empty for a peripheral without write-once registers. */
  std::vector<std::uint32_t> write_locked{};

  /** @brief Member-wise equality. */
  friend auto operator==(peripheral_image const&, peripheral_image const&) -> bool = default;
};

/** @brief Content of the event queue: what scheduler::pending() and next_seq() return. */
struct scheduler_state
{
  /** @brief Pending events in dispatch order. */
  std::vector<event> pending{};
  /** @brief Sequence number the next schedule returns. */
  std::uint64_t next_seq{};

  /** @brief Member-wise equality. */
  friend auto operator==(scheduler_state const&, scheduler_state const&) -> bool = default;
};

/** @brief Run state of one intercept, part of the machine state, unlike the intercept table. */
struct intercept_state
{
  /** @brief Intercepted code address. */
  address at{};
  /** @brief Calls seen so far, the occurrence index of scripted replies. */
  std::uint64_t calls{};
  /** @brief Whether a once-mode intercept has fired and no longer does. */
  bool spent{};

  /** @brief Member-wise equality. */
  friend auto operator==(intercept_state const&, intercept_state const&) -> bool = default;
};

/** @brief A call a return hook saw enter and not yet return. */
struct pending_return
{
  /** @brief Address the call returns to, where the hook fires. */
  address return_address{};
  /** @brief Stack pointer at entry, telling recursive calls apart. */
  address stack_pointer{};
  /** @brief Argument bits captured at entry. */
  std::vector<std::uint64_t> args{};
  /** @brief Instruction count at entry. */
  icount entered{};

  /** @brief Member-wise equality. */
  friend auto operator==(pending_return const&, pending_return const&) -> bool = default;
};

/** @brief Run state of one return hook. */
struct return_hook_state
{
  /** @brief Hooked function entry. */
  address at{};
  /** @brief Returns seen so far. */
  std::uint64_t calls{};
  /** @brief Calls entered and not yet returned, innermost last. */
  std::vector<pending_return> in_flight{};

  /** @brief Member-wise equality. */
  friend auto operator==(return_hook_state const&, return_hook_state const&) -> bool = default;
};

/**
 * @brief Plain-data content of a snapshot: everything but the same-engine CPU context.
 *        Aggregates of strings, integers, vectors and the plain clock and event types, the base
 *        of the saved form. Members after the header appear in restore order.
 */
struct snapshot_data
{
  /** @brief Layout version, snapshot_format_version when taken. */
  std::uint32_t format{};
  /** @brief popcorn version that took the snapshot, `core::version()`. */
  std::string popcorn_version{};
  /** @brief Commit of the Unicorn engine that took the snapshot. */
  std::string unicorn_commit{};
  /** @brief `description_digest()` of the description the snapshot was taken on. */
  std::string description_digest{};
  /** @brief Digest of each loaded image, in load order. */
  std::vector<std::string> image_digests{};
  /** @brief `machine_options::config_digest` of the machine that took the snapshot. */
  std::string config_digest{};
  /** @brief Machine generation when the snapshot was taken. */
  std::uint64_t generation{};
  /** @brief Every mapped RAM and flash region, in description order, then the scratch region of
   * `machine_options::scratch`. MMIO is not memory. */
  std::vector<memory_image> memory{};
  /** @brief Every register of `arch::arch::registers()`, in its order. */
  std::vector<register_value> registers{};
  /** @brief Bytes of `arch::arch::save()`, owner `arch`. */
  snapshot_part arch{};
  /** @brief Register storage and line levels of each peripheral, in description order. */
  std::vector<peripheral_image> peripherals{};
  /** @brief Bytes of each model's `periph::model::save()`, in description order. */
  std::vector<snapshot_part> models{};
  /** @brief Event queue, arch and model events alike. */
  scheduler_state events{};
  /** @brief Clock, from `clock_driver::state()`. */
  clock_state clock{};
  /** @brief Intercept call counters and once-mode state. */
  std::vector<intercept_state> intercepts{};
  /** @brief Return hook counters and calls in flight. */
  std::vector<return_hook_state> return_hooks{};

  /** @brief Member-wise equality. */
  friend auto operator==(snapshot_data const&, snapshot_data const&) -> bool = default;
};

/** @brief Saved engine CPU context, valid only in the engine that saved it. */
class engine_context;

/**
 * @brief Full copy of a machine's state, taken by `machine::snapshot()` and restored by
 *        `machine::restore()` any number of times. Holds snapshot_data plus, when taken, the
 *        engine's own CPU context for the same-engine fast path. Copies share the immutable
 *        context.
 */
class snapshot
{
public:
  /**
   * @brief A snapshot read back from its saved form: no engine context, so a restore writes the
   *        register list.
   * @param data Content.
   */
  explicit snapshot(snapshot_data data);

  /** @brief Plain-data content. */
  [[nodiscard]] auto data() const noexcept -> snapshot_data const&;

  /** @brief Whether the snapshot carries the engine context of the machine that took it. */
  [[nodiscard]] auto has_engine_context() const noexcept -> bool;

  /** @brief Identity of the machine that took the snapshot, 0 for one read back from bytes. */
  [[nodiscard]] auto machine_id() const noexcept -> std::uint64_t;

private:
  friend class machine;

  snapshot(
      snapshot_data                         data,
      std::shared_ptr<engine_context const> context,
      std::uint64_t                         machine_id);

  snapshot_data                         data_{};
  std::shared_ptr<engine_context const> context_{};
  std::uint64_t                         machine_id_ = 0;
};

/**
 * @brief Serializes snapshot content to its saved form, which another machine built from the
 *        same description and images can restore.
 * @param data Content to serialize.
 * @return Bytes that decode_snapshot() reads back to a value equal to @p data.
 */
auto encode_snapshot(snapshot_data const& data) -> std::vector<std::byte>;

/**
 * @brief Reads back bytes encode_snapshot() wrote.
 * @param bytes Serialized content.
 * @return The content.
 * @throws config_error when @p bytes is malformed or its format is not snapshot_format_version.
 */
auto decode_snapshot(std::span<std::byte const> bytes) -> snapshot_data;
}
