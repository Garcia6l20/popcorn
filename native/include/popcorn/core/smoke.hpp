#pragma once

#include <popcorn/core/engine.hpp>

#include <cstdint>
#include <filesystem>

namespace popcorn::core
{
/**
 * @brief Loads the PT_LOAD segments of an ELF into flash and SRAM and runs it from its reset
 * vector until the first bkpt.
 * @param engine An engine from open_m33() with nothing mapped yet.
 * @param elf Path of a Cortex-M ELF linked for flash at 0x08000000 and SRAM at 0x20000000.
 * @return r0 when the bkpt is reached.
 * @throws EngineError when the file is not such an ELF or the run stops elsewhere.
 */
auto run_to_bkpt(engine_handle const& engine, std::filesystem::path const& elf) -> std::uint32_t;
}
