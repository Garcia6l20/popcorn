#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>

#include "../support/test_arch.hpp"

#include <doctest/doctest.h>
#include <unicorn/unicorn.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::address;

auto description(char const* variable) -> popcorn::mcu_description
{
  char const* const path = std::getenv(variable);
  REQUIRE_MESSAGE(path != nullptr, variable);
  return popcorn::load_description(path);
}

auto m33() -> popcorn::mcu_description
{
  return description("POPCORN_M33_DESC");
}

auto m33_code_alias() -> popcorn::mcu_description
{
  return description("POPCORN_M33_CODE_ALIAS_DESC");
}

struct recorded
{
  popcorn::testing::test_arch* module = nullptr;

  auto factory() -> popcorn::arch::arch_factory
  {
    return [this](popcorn::core_desc const& core) -> std::unique_ptr<popcorn::arch::arch> {
      auto made = std::make_unique<popcorn::testing::test_arch>(core);
      module    = made.get();
      return made;
    };
  }
};

struct mapping
{
  std::uint64_t begin{};
  std::uint64_t end{};
  std::uint32_t perms{};

  friend auto operator==(mapping, mapping) -> bool = default;
};

auto mappings(uc_engine* engine) -> std::vector<mapping>
{
  uc_mem_region* regions = nullptr;
  std::uint32_t  count   = 0;
  REQUIRE(uc_mem_regions(engine, &regions, &count) == UC_ERR_OK);
  std::vector<mapping> out;
  for(auto const& region : std::span{regions, count})
  {
    out.push_back({region.begin, region.end, region.perms});
  }
  uc_free(regions);
  return out;
}

auto config_key(auto&& build) -> std::string
{
  try
  {
    build();
  }
  catch(popcorn::config_error const& e)
  {
    return e.key();
  }
  return {};
}

class bkpt_runner
{
public:
  explicit bkpt_runner(core::machine& target) : target_{target}
  {}

  auto run(address at) -> std::uint64_t
  {
    auto const registers = target_.regs();
    registers.write(*registers.find("pc"), at);
    auto const slice = target_.run_slice({.wall_budget = std::chrono::seconds{10}});
    REQUIRE(slice.stopped.has_value());
    REQUIRE(slice.stopped->reason == popcorn::stop_reason::user);
    return registers.read(*registers.find("r0"));
  }

private:
  core::machine& target_;
};

constexpr std::uint32_t rx  = UC_PROT_READ | UC_PROT_EXEC;
constexpr std::uint32_t rw  = UC_PROT_READ | UC_PROT_WRITE;
constexpr std::uint32_t rwx = UC_PROT_ALL;
}

TEST_CASE("m33 description loads with its regions and test peripherals")
{
  auto const loaded = m33();
  CHECK(loaded.core.cpu == "CM33");
  CHECK(loaded.core.num_irqs == 8);
  REQUIRE(loaded.regions.size() == 3);
  CHECK(loaded.regions[1].physical == address{0x0800'0000});
  REQUIRE(loaded.peripherals.size() == 3);
  CHECK(loaded.peripherals[1].alias_of == "TESTDEV");
  CHECK(m33_code_alias().regions.back().physical == address{0x2000'0000});
}

TEST_CASE("machine maps every region and the arch system page with their protections")
{
  recorded      made;
  core::machine target{m33(), made.factory(), {}};
  REQUIRE(made.module != nullptr);
  auto* const engine = made.module->attached_engine();
  REQUIRE(engine != nullptr);

  std::uint32_t page = 0;
  REQUIRE(uc_ctl_get_page_size(engine, &page) == UC_ERR_OK);
  CHECK(page == 1024);

  auto const found = mappings(engine);
  REQUIRE(found.size() == 4);
  CHECK(found[0] == mapping{0x0000'0000, 0x001f'ffff, rx});
  CHECK(found[1] == mapping{0x0800'0000, 0x081f'ffff, rx});
  CHECK(found[2] == mapping{0x2000'0000, 0x200b'ffff, rwx});
  CHECK(found[3].begin == popcorn::testing::test_arch::system_base);
  CHECK(found[3].end == popcorn::testing::test_arch::system_base + 0xfff);
}

TEST_CASE("machine construction refuses what it cannot map")
{
  SUBCASE("a core Unicorn has no model for")
  {
    auto desc     = m33();
    desc.core.cpu = "CM99";
    CHECK(
        config_key([&] { core::machine{desc, popcorn::testing::test_arch_factory(), {}}; })
        == "core.cpu");
  }
  SUBCASE("a factory returning no module")
  {
    auto const none = [](popcorn::core_desc const&) -> std::unique_ptr<popcorn::arch::arch> {
      return nullptr;
    };
    CHECK(config_key([&] { core::machine{m33(), none, {}}; }) == "core.cpu");
  }
  SUBCASE("no executable region and no vector table")
  {
    auto desc = m33();
    for(auto& region : desc.regions)
    {
      region.acc = {.read = true, .write = true, .execute = false};
    }
    CHECK(
        config_key([&] {
          desc.regions.erase(desc.regions.begin() + 1);
          core::machine{desc, popcorn::testing::test_arch_factory(), {}};
        })
        == "regions");
    core::machine_options options;
    options.vector_table = 0x0800'0000;
    CHECK_NOTHROW(core::machine{desc, popcorn::testing::test_arch_factory(), options});
  }
  SUBCASE("a region not aligned to the engine page")
  {
    auto desc = m33();
    desc.regions[2].size += 512;
    CHECK(
        config_key([&] { core::machine{desc, popcorn::testing::test_arch_factory(), {}}; })
        == "regions.SRAM");
  }
  SUBCASE("an alias whose physical address is in no region")
  {
    auto desc                = m33();
    desc.regions[1].physical = 0x0900'0000;
    CHECK(
        config_key([&] { core::machine{desc, popcorn::testing::test_arch_factory(), {}}; })
        == "regions.FLASH_ALIAS.physical");
  }
}

TEST_CASE("a debugger write through an alias reads back through its target and the reverse")
{
  core::machine target{m33(), popcorn::testing::test_arch_factory(), {}};
  auto const    memory = target.mem();
  REQUIRE(memory.write(0x0800'0100, 4, 0x1234'5678));
  CHECK(memory.read(0x0000'0100, 4) == 0x1234'5678u);
  REQUIRE(memory.write(0x0000'0200, 2, 0xbeef));
  CHECK(memory.read(0x0800'0200, 2) == 0xbeefu);
  CHECK(memory.read(0x0800'0300, 4) == 0xffff'ffffu);
}

TEST_CASE("an rx alias over rw SRAM shares the buffer and keeps each protection")
{
  auto desc = m33_code_alias();
  SUBCASE("alias listed after its target")
  {}
  SUBCASE("alias listed before its target")
  {
    std::ranges::rotate(desc.regions.begin() + 2, desc.regions.begin() + 3, desc.regions.end());
    REQUIRE(desc.regions[2].name == "SRAM_CODE");
  }
  recorded      made;
  core::machine target{desc, made.factory(), {}};
  auto const    found = mappings(made.module->attached_engine());
  REQUIRE(found.size() == 5);
  CHECK(found[2] == mapping{0x0a00'0000, 0x0a0b'ffff, rx});
  CHECK(found[3] == mapping{0x2000'0000, 0x200b'ffff, rw});

  auto const memory = target.mem();
  REQUIRE(memory.write(0x2000'0100, 4, 0xcafe'f00d));
  CHECK(memory.read(0x0a00'0100, 4) == 0xcafe'f00du);
}

TEST_CASE("debugger writes into executable memory replace translated code at every mapping")
{
  constexpr std::array<std::byte, 4> movs_r0_1_bkpt{
      std::byte{0x01}, std::byte{0x20}, std::byte{0x00}, std::byte{0xbe}};
  constexpr std::array<std::byte, 2> movs_r0_2{std::byte{0x02}, std::byte{0x20}};

  SUBCASE("flash and its alias")
  {
    recorded      made;
    core::machine target{m33(), made.factory(), {}};
    bkpt_runner   runner{target};
    auto const    memory = target.mem();
    REQUIRE(memory.write_block(0x0800'0400, movs_r0_1_bkpt));
    CHECK(runner.run(0x0800'0400) == 1);
    CHECK(runner.run(0x0000'0400) == 1);
    REQUIRE(memory.write_block(0x0000'0400, movs_r0_2));
    CHECK(runner.run(0x0800'0400) == 2);
    CHECK(runner.run(0x0000'0400) == 2);
    REQUIRE(memory.write(0x0800'0400, 2, 0x2003));
    CHECK(runner.run(0x0000'0400) == 3);
    CHECK(runner.run(0x0800'0400) == 3);
  }
  SUBCASE("code written through rw SRAM runs through its rx alias")
  {
    recorded      made;
    core::machine target{m33_code_alias(), made.factory(), {}};
    bkpt_runner   runner{target};
    auto const    memory = target.mem();
    REQUIRE(memory.write_block(0x2000'0400, movs_r0_1_bkpt));
    CHECK(runner.run(0x0a00'0400) == 1);
    REQUIRE(memory.write(0x2000'0400, 2, 0x2002));
    CHECK(runner.run(0x0a00'0400) == 2);
  }
}

TEST_CASE("memory view reads and writes RAM and flash as the debugger")
{
  core::machine target{m33(), popcorn::testing::test_arch_factory(), {}};
  auto const    memory = target.mem();

  REQUIRE(memory.write(0x2000'0000, 1, 0xab));
  REQUIRE(memory.write(0x2000'0002, 2, 0xcdef));
  REQUIRE(memory.write(0x2000'0004, 4, 0x0102'0304));
  CHECK(memory.read(0x2000'0000, 1) == 0xabu);
  CHECK(memory.read(0x2000'0002, 2) == 0xcdefu);
  CHECK(memory.read(0x2000'0000, 4) == 0xcdef'00abu);
  CHECK(memory.read(0x2000'0004, 1) == 0x04u);
  CHECK(memory.read(0x2000'0006, 2) == 0x0102u);

  REQUIRE(memory.store<std::uint64_t>(0x2000'0101, 0x1122'3344'5566'7788));
  CHECK(memory.load<std::uint64_t>(0x2000'0101) == 0x1122'3344'5566'7788u);
  CHECK(memory.read(0x2000'0100, 4) == 0x6677'8800u);
  REQUIRE(memory.store<double>(0x2000'0203, 2.5));
  CHECK(memory.load<double>(0x2000'0203) == 2.5);

  std::array<std::byte, 4> tail{};
  REQUIRE(memory.store<std::uint16_t>(0x200b'fffe, 0x5aa5));
  auto const read = memory.read_block(0x200b'fffe, tail);
  REQUIRE_FALSE(read.has_value());
  CHECK(read.error() == core::block_error{popcorn::bus_error::unmapped, 0x200c'0000});
  CHECK(tail[0] == std::byte{0xa5});
  CHECK(tail[1] == std::byte{0x5a});
  CHECK(memory.read(0x6000'0000, 4).error() == popcorn::bus_error::unmapped);
  CHECK(memory.write(0x6000'0000, 4, 0).error() == popcorn::bus_error::unmapped);

  REQUIRE(memory.write(0x0800'1000, 4, 0x4770'4770));
  CHECK(memory.read(0x0800'1000, 4) == 0x4770'4770u);
}

TEST_CASE("flash denies CPU and DMA writes and accepts debugger writes")
{
  recorded                           made;
  core::machine                      target{m33(), made.factory(), {}};
  auto&                              services = made.module->attached_services();
  constexpr std::array<std::byte, 2> word{std::byte{0x12}, std::byte{0x34}};

  for(auto const by : {popcorn::initiator::cpu, popcorn::initiator::dma})
  {
    CAPTURE(by);
    CHECK(services.write(0x0800'0000, 4, 0, by).error() == popcorn::bus_error::denied);
    CHECK(
        services.write_block(0x0800'0000, word, by).error()
        == core::block_error{popcorn::bus_error::denied, 0x0800'0000});
    CHECK(services.read(0x0800'0000, 4, by) == 0xffff'ffffu);
  }
  REQUIRE(services.write(0x0800'0000, 4, 0x1234'5678, popcorn::initiator::debugger));
  CHECK(services.read(0x0800'0000, 4, popcorn::initiator::cpu) == 0x1234'5678u);
  REQUIRE(services.write_block(0x0800'0004, word, popcorn::initiator::debugger));
  CHECK(services.read(0x0800'0004, 2, popcorn::initiator::dma) == 0x3412u);
}

TEST_CASE("register view round-trips every test arch register")
{
  core::machine target{m33(), popcorn::testing::test_arch_factory(), {}};
  auto const    registers = target.regs();
  REQUIRE(registers.list().size() == 21);
  CHECK_FALSE(registers.find("basepri").has_value());
  REQUIRE(registers.find("r0").has_value());

  for(auto const& reg : registers.list())
  {
    CAPTURE(reg.name);
    auto const value = reg.name == "xpsr"    ? std::uint64_t{0xf100'0000}
                     : reg.name == "control" ? std::uint64_t{0}
                     : reg.name == "primask" ? std::uint64_t{1}
                                             : std::uint64_t{0x2000'1230};
    registers.write(reg, value);
    CHECK(registers.read(reg) == value);
  }
  registers.write(*registers.find("pc"), 0x0800'0120);
  CHECK(registers.pc() == 0x0800'0120);
}
