#pragma once

#include <popcorn/core/types.hpp>

#include <reflex/enum.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace popcorn::core
{
/**
 * @brief Block entry counters of one code range: one counter per halfword, index
 *        `(addr - start) >> 1`, incremented each time a translated block starting at that address
 *        runs. Instruction and line counts are derived from these by decoding each block's byte
 *        range, since the engine reports a block's instruction count, not its size.
 */
struct block_counters
{
  /** @brief Range the counters cover. */
  address_range range{};
  /** @brief Entry count per halfword of the range. */
  std::vector<std::uint32_t> hits{};

  /** @brief Member-wise equality. */
  friend auto operator==(block_counters const&, block_counters const&) -> bool = default;
};

/** @brief Count of one control-flow edge between two translated blocks, the input of branch
 * coverage. */
struct edge_counter
{
  /** @brief Start address of the block the edge leaves. */
  address from{};
  /** @brief Start of the block the edge enters. */
  address to{};
  /** @brief Times the edge was taken. */
  std::uint64_t hits{};

  /** @brief Member-wise equality. */
  friend auto operator==(edge_counter const&, edge_counter const&) -> bool = default;
};

/**
 * @brief Count of one way a translated block was left before its last instruction ran: a stop, a
 *        fault, an intercept that returned or stopped, or an exit address inside the block.
 */
struct cut_counter
{
  /** @brief Start address of the block. */
  address start{};
  /** @brief Address of the first instruction of the block that did not run, @p start when none
   * ran. */
  address at{};
  /** @brief Times the block was left there. */
  std::uint64_t hits{};

  /** @brief Member-wise equality. */
  friend auto operator==(cut_counter const&, cut_counter const&) -> bool = default;
};

/**
 * @brief Why a CPU access reached a register no model implements. none: a register a model
 *        serves. no_register: an offset of a peripheral window where the description has no
 *        register. storage_only: a register the model claims and calls storage only,
 *        periph::model::storage_only. no_model: a register of a peripheral without a model.
 *        unimplemented: a register of a modelled peripheral its model does not claim. raz_wi: an
 *        access to a machine_options::raz_wi range.
 */
enum class[[= reflex::derive{reflex::Format}]] unmodelled_reason : std::uint8_t
{
  none,
  no_register,
  storage_only,
  no_model,
  unimplemented,
  raz_wi,
};

/** @brief Count of CPU accesses from one instruction to one register no model implements. */
struct unmodelled_access
{
  /** @brief Address of the accessing instruction. */
  address pc{};
  /** @brief Peripheral as the window names it, an alias's own name, or the raz_wi range name. */
  std::string peripheral{};
  /** @brief Register name, empty where the description has none. */
  std::string reg{};
  /** @brief Byte offset of the accessed word in the peripheral or range, a multiple of 4. */
  std::uint32_t offset{};
  /** @brief Whether the access was a write. */
  bool write{};
  /** @brief Why the register is unmodelled. */
  unmodelled_reason reason{};
  /** @brief Times it was accessed. */
  std::uint64_t hits{};

  /** @brief Member-wise equality. */
  friend auto operator==(unmodelled_access const&, unmodelled_access const&) -> bool = default;
};

/**
 * @brief Raw coverage counters of a machine, plain data: the input of `.pcov` files, lcov and
 *        drcov exports, never lcov text. A `.pcov` file stores them per translated block with
 *        64-bit sums, so files from runs with different block boundaries merge.
 */
struct coverage_data
{
  /** @brief Block counters per enabled range, in enable order. */
  std::vector<block_counters> blocks{};
  /** @brief Edge counters with a non-zero count, ordered by from then to. */
  std::vector<edge_counter> edges{};
  /** @brief Blocks entered but left before their end, ordered by start then at. Their entries
   * are in blocks too. */
  std::vector<cut_counter> cuts{};
  /** @brief CPU accesses to unmodelled registers, ordered by pc, peripheral, register, offset,
   * write and reason. Empty unless coverage was enabled with record_unmodelled. */
  std::vector<unmodelled_access> unmodelled{};

  /** @brief Member-wise equality. */
  friend auto operator==(coverage_data const&, coverage_data const&) -> bool = default;
};
}
