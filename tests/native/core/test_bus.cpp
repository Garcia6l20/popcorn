#include "../../../native/src/core/engine.hpp"

#include <doctest/doctest.h>
#include <unicorn/unicorn.h>

#include <array>
#include <cstdint>
#include <vector>

namespace
{
constexpr std::uint64_t code_base = 0x0800'0000;
constexpr std::uint64_t mmio_base = 0x4001'0000;

std::vector<std::uint32_t> seen_pcs;

auto read_pc(uc_engine* engine, std::uint64_t, unsigned, void*) noexcept -> std::uint64_t
{
  std::uint32_t pc = 0;
  uc_reg_read(engine, UC_ARM_REG_PC, &pc);
  seen_pcs.push_back(pc);
  return 0;
}

void ignore_write(uc_engine*, std::uint64_t, unsigned, std::uint64_t, void*) noexcept
{}

void block(uc_engine*, std::uint64_t, std::uint32_t, void*) noexcept
{}

auto engine() -> popcorn::core::engine_handle
{
  auto handle = popcorn::core::open_engine(
      {.arch = UC_ARCH_ARM, .mode = UC_MODE_THUMB | UC_MODE_MCLASS, .cpu_model = std::nullopt});
  REQUIRE(uc_mem_map(handle.get(), code_base, 0x1000, UC_PROT_ALL) == UC_ERR_OK);
  REQUIRE(
      uc_mmio_map(handle.get(), mmio_base, 0x400, &read_pc, nullptr, &ignore_write, nullptr)
      == UC_ERR_OK);
  uc_hook hook = 0;
  REQUIRE(
      uc_hook_add(
          handle.get(), &hook, UC_HOOK_BLOCK_ICOUNT, reinterpret_cast<void*>(&block), nullptr, 1, 0)
      == UC_ERR_OK);
  constexpr std::array<std::uint16_t, 7> code{0xf240, 0x0100, 0xf2c4, 0x0101,
                                              0xbf00, 0x6808, 0xbf00};
  REQUIRE(uc_mem_write(handle.get(), code_base, code.data(), sizeof code) == UC_ERR_OK);
  return handle;
}

auto load_pc(uc_engine* target) -> std::uint32_t
{
  seen_pcs.clear();
  REQUIRE(uc_emu_start(target, code_base | 1, code_base + 14, 0, 0) == UC_ERR_OK);
  REQUIRE(seen_pcs.size() == 1);
  return seen_pcs.front();
}
}

TEST_CASE("the exact pc hook makes MMIO pc exact while referenced")
{
  auto const                   handle = engine();
  popcorn::core::exact_pc_hook exact;
  exact.configure(
      handle.get(), {
                        {mmio_base, mmio_base + 0x3ff}
  });
  CHECK(load_pc(handle.get()) == code_base);
  exact.acquire();
  exact.acquire();
  CHECK(exact.installed());
  CHECK(exact.users() == 2);
  CHECK(load_pc(handle.get()) == code_base + 10);
  exact.release();
  CHECK(exact.installed());
  CHECK(load_pc(handle.get()) == code_base + 10);
  exact.release();
  CHECK_FALSE(exact.installed());
  CHECK(exact.users() == 0);
  CHECK(load_pc(handle.get()) == code_base);
  exact.release();
  CHECK(exact.users() == 0);
}
