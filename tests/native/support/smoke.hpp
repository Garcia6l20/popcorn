#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

struct uc_struct;

namespace popcorn::testing
{
/** @brief Closes a Unicorn engine. */
struct engine_closer
{
  /** @brief Closes @p engine. */
  void operator()(uc_struct* engine) const noexcept;
};

/** @brief Move-only owner of an open Unicorn engine. */
using engine_handle = std::unique_ptr<uc_struct, engine_closer>;

/**
 * @brief Opens a Cortex-M33 engine directly, without a machine.
 * @throws emulation_error when the linked Unicorn lacks the dev WFI instruction hook.
 */
auto open_m33() -> engine_handle;

/**
 * @brief Loads the PT_LOAD segments of an ELF into flash and SRAM and runs it from its reset
 *        vector until the first bkpt. Test-local smoke path, not popcorn API.
 * @param engine An engine from open_m33() with nothing mapped yet.
 * @param elf Path of a Cortex-M ELF linked for flash at 0x08000000 and SRAM at 0x20000000.
 * @return r0 when the bkpt is reached.
 * @throws image_error when the file is not such an ELF.
 * @throws emulation_error when the run stops elsewhere than the bkpt.
 */
auto run_to_bkpt(engine_handle const& engine, std::filesystem::path const& elf) -> std::uint32_t;
}
