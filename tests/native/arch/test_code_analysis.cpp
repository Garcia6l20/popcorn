#include <popcorn/arch/arch.hpp>
#include <popcorn/arch/armv8m/armv8m.hpp>
#include <popcorn/arch/armv8m/thumb.hpp>
#include <popcorn/core/description.hpp>

#include <doctest/doctest.h>

#include <elf.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{
namespace arch  = popcorn::arch;
namespace thumb = popcorn::arch::armv8m::thumb;
using popcorn::address;
using popcorn::core_desc;

constexpr address at = 0x0800'1000;

auto analysis() -> arch::code_analysis const&
{
  return arch::armv8m::analysis();
}

auto little_endian(std::span<std::uint16_t const> halves) -> std::vector<std::byte>
{
  std::vector<std::byte> bytes;
  for(auto const half : halves)
  {
    bytes.push_back(static_cast<std::byte>(half & 0xff));
    bytes.push_back(static_cast<std::byte>(half >> 8));
  }
  return bytes;
}

auto little_endian(std::initializer_list<std::uint16_t> halves) -> std::vector<std::byte>
{
  return little_endian(std::span{halves.begin(), halves.size()});
}

auto core_named(std::string cpu) -> core_desc
{
  core_desc core;
  core.cpu = std::move(cpu);
  return core;
}

auto old_trap(thumb::insn const& i) -> bool
{
  return (i.size == 2 and (i.bits & 0xff00) == 0xde00)
      or (i.size == 4 and (i.bits & 0xfff0'f000) == 0xf7f0'a000);
}

auto decoded_by_thumb(address where, std::span<std::byte const> bytes)
    -> std::optional<arch::decoded_insn>
{
  auto const read = thumb::read(where, bytes);
  if(not read)
  {
    return std::nullopt;
  }
  auto const control = thumb::classify(*read);
  return arch::decoded_insn{
      .at          = read->at,
      .size        = read->size,
      .kind        = control.kind,
      .target      = control.target,
      .conditional = control.conditional,
      .call        = thumb::is_call(*read),
      .trap        = old_trap(*read),
      .predicates  = thumb::is_it(*read) ? thumb::it_length(*read) : std::uint8_t{0},
  };
}

void check_against_thumb(std::span<std::byte const> bytes)
{
  auto const decoded = analysis().decode(at, bytes);
  CHECK(decoded == decoded_by_thumb(at, bytes));
  auto const read = thumb::read(at, bytes);
  REQUIRE(read.has_value());
  REQUIRE(decoded.has_value());
  auto const branch = thumb::conditional_branch(*read);
  auto const as_branch =
      decoded->kind == arch::ending::direct and decoded->conditional and decoded->target;
  CHECK(as_branch == branch.has_value());
  if(branch)
  {
    CHECK(*decoded->target == branch->taken);
    CHECK(decoded->at + decoded->size == branch->fallthrough);
  }
}

struct row
{
  char const*                name;
  std::vector<std::uint16_t> halves;
  std::uint8_t               size;
  arch::ending               kind;
  std::optional<address>     target;
  bool                       conditional;
  bool                       call;
  bool                       trap;
  std::uint8_t               predicates;
};

auto rows() -> std::vector<row>
{
  using enum arch::ending;
  return {
      {"movs r0, #1",      {0x2001},         2, none,      {},     false, false, false, 0},
      {"add.w r0, r1, r2", {0xeb01, 0x0002}, 4, none,      {},     false, false, false, 0},
      {"udf #0",           {0xde00},         2, exception, {},     false, false, true,  0},
      {"udf.w #0",         {0xf7f0, 0xa000}, 4, exception, {},     false, false, true,  0},
      {"svc #0",           {0xdf00},         2, exception, {},     false, false, false, 0},
      {"bl .+8",           {0xf000, 0xf802}, 4, direct,    at + 8, false, true,  false, 0},
      {"beq .+8",          {0xd002},         2, direct,    at + 8, true,  false, false, 0},
      {"bne.w .+8",        {0xf040, 0x8002}, 4, direct,    at + 8, true,  false, false, 0},
      {"cbz r0, .+8",      {0xb110},         2, direct,    at + 8, true,  false, false, 0},
      {"b .+8",            {0xe002},         2, direct,    at + 8, false, false, false, 0},
      {"bx lr",            {0x4770},         2, indirect,  {},     false, false, false, 0},
      {"blx r3",           {0x4798},         2, indirect,  {},     false, true,  false, 0},
      {"it eq",            {0xbf08},         2, none,      {},     false, false, false, 1},
      {"itt eq",           {0xbf04},         2, none,      {},     false, false, false, 2},
      {"ittt eq",          {0xbf02},         2, none,      {},     false, false, false, 3},
      {"itttt eq",         {0xbf01},         2, none,      {},     false, false, false, 4},
      {"wfi",              {0xbf30},         2, system,    {},     false, false, false, 0},
      {"nop",              {0xbf00},         2, none,      {},     false, false, false, 0},
  };
}
}

TEST_CASE("decode gives each Thumb row its size, ending, call, trap and predication")
{
  for(auto const& expected : rows())
  {
    CAPTURE(expected.name);
    auto const bytes   = little_endian(expected.halves);
    auto const decoded = analysis().decode(at, bytes);
    REQUIRE(decoded.has_value());
    CHECK(
        *decoded
        == arch::decoded_insn{
            .at          = at,
            .size        = expected.size,
            .kind        = expected.kind,
            .target      = expected.target,
            .conditional = expected.conditional,
            .call        = expected.call,
            .trap        = expected.trap,
            .predicates  = expected.predicates});
    CHECK(analysis().size(bytes) == expected.size);
    check_against_thumb(bytes);
  }
}

TEST_CASE("decode equals the thumb functions on every 16-bit encoding")
{
  for(std::uint32_t first = 0; first <= 0xffff; ++first)
  {
    auto const half = static_cast<std::uint16_t>(first);
    if(thumb::size(half) != 2)
    {
      continue;
    }
    CAPTURE(half);
    check_against_thumb(little_endian({half}));
  }
}

TEST_CASE("decode equals the thumb functions on 32-bit encodings")
{
  constexpr std::array seconds{
      std::uint16_t{0x0000}, std::uint16_t{0x8000}, std::uint16_t{0x8002}, std::uint16_t{0x9000},
      std::uint16_t{0xa000}, std::uint16_t{0xc000}, std::uint16_t{0xd000}, std::uint16_t{0xd7ff},
      std::uint16_t{0xe800}, std::uint16_t{0xf000}, std::uint16_t{0xf802}, std::uint16_t{0xffff},
      std::uint16_t{0x0a10}, std::uint16_t{0x0a00}, std::uint16_t{0x8f60}, std::uint16_t{0x8001},
      std::uint16_t{0x8003}, std::uint16_t{0x2fff}, std::uint16_t{0xa7ff}, std::uint16_t{0x5555},
  };
  for(std::uint32_t first = 0xe800; first <= 0xffff; ++first)
  {
    for(auto const second : seconds)
    {
      CAPTURE(first);
      CAPTURE(second);
      check_against_thumb(little_endian({static_cast<std::uint16_t>(first), second}));
    }
  }
}

TEST_CASE("decode is empty for a truncated instruction, size tells what it needs")
{
  auto const wide = little_endian({0xf000, 0xf802});
  CHECK_FALSE(analysis().decode(at, std::span{wide}.first(2)).has_value());
  CHECK_FALSE(analysis().decode(at, std::span{wide}.first(3)).has_value());
  CHECK(analysis().size(std::span{wide}.first(2)) == 4);
  CHECK_FALSE(analysis().size(std::span{wide}.first(1)).has_value());
  CHECK_FALSE(analysis().decode(at, {}).has_value());
}

TEST_CASE("predicated_at_most sees an IT within the 8 halfwords before an address")
{
  constexpr address base = 0x100;
  auto const        code = little_endian(
      {0xbf08, 0x2001, 0x2001, 0x2001, 0x2001, 0x2001, 0x2001, 0x2001, 0x2001, 0x2001});
  CHECK(analysis().predicated_at_most(base + 2, base, code) == 4);
  CHECK(analysis().predicated_at_most(base + 16, base, code) == 4);
  CHECK(analysis().predicated_at_most(base + 18, base, code) == 0);
  CHECK(analysis().predicated_at_most(base, base, code) == 0);
  auto const short_code = little_endian({0x2001, 0xbf08});
  CHECK(analysis().predicated_at_most(base + 12, base, short_code) == 4);
  CHECK(analysis().predicated_at_most(base + 12, base, std::span{short_code}.first(3)) == 0);
  auto const nop = little_endian({0xbf00, 0x2001});
  CHECK(analysis().predicated_at_most(base + 4, base, nop) == 0);
}

TEST_CASE("crosses_page follows the engine: a 32-bit instruction in the page's last 3 bytes")
{
  CHECK_FALSE(analysis().crosses_page(1020, 4));
  CHECK(analysis().crosses_page(1021, 4));
  CHECK(analysis().crosses_page(1022, 4));
  CHECK_FALSE(analysis().crosses_page(1022, 2));
  CHECK_FALSE(analysis().crosses_page(1023, 2));
  CHECK(analysis().crosses_page(1024, 2));
  CHECK(analysis().limits() == arch::block_limits{.page = 1024, .max_insns = 512});
}

namespace
{
struct fake_analysis final : arch::code_analysis
{
  [[nodiscard]] auto size(std::span<std::byte const>) const noexcept
      -> std::optional<std::uint8_t> override
  {
    return std::nullopt;
  }

  [[nodiscard]] auto decode(address, std::span<std::byte const>) const noexcept
      -> std::optional<arch::decoded_insn> override
  {
    return std::nullopt;
  }

  [[nodiscard]] auto predicated_at_most(address, address, std::span<std::byte const>) const noexcept
      -> std::uint8_t override
  {
    return 0;
  }

  [[nodiscard]] auto crosses_page(std::uint32_t, std::uint8_t) const noexcept -> bool override
  {
    return false;
  }

  [[nodiscard]] auto limits() const noexcept -> arch::block_limits override
  {
    return {.page = 4, .max_insns = 1};
  }
};

fake_analysis const fake{};

void register_fake(
    std::string                name,
    std::string                cpu,
    arch::code_analysis const* with,
    std::uint16_t              elf_machine)
{
  arch::register_arch(
      {.name        = std::move(name),
       .implements  = [cpu = std::move(cpu)](core_desc const& core) { return core.cpu == cpu; },
       .factory     = [](core_desc const&) -> std::unique_ptr<arch::arch> { return nullptr; },
       .analysis    = with,
       .elf_machine = elf_machine});
}
}

TEST_CASE("register_builtin registers the Thumb analysis for EM_ARM and the cores it implements")
{
  arch::armv8m::register_builtin();
  CHECK(arch::find_code_analysis(std::uint16_t{EM_ARM}) == &analysis());
  CHECK(arch::find_code_analysis(core_named("CM33")) == &analysis());
  CHECK(arch::find_code_analysis(std::uint16_t{EM_RISCV}) == nullptr);
}

TEST_CASE("find_code_analysis is null for a module without one, ELF machine 0 or no module")
{
  register_fake("fake-without", "FAKE-WITHOUT", nullptr, 0xfff0);
  register_fake("fake-zero", "FAKE-ZERO", &fake, 0);
  CHECK(arch::find_code_analysis(core_named("FAKE-WITHOUT")) == nullptr);
  CHECK(arch::find_code_analysis(std::uint16_t{0xfff0}) == nullptr);
  CHECK(arch::find_code_analysis(core_named("FAKE-ZERO")) == &fake);
  CHECK(arch::find_code_analysis(std::uint16_t{0}) == nullptr);
  CHECK(arch::find_code_analysis(core_named("FAKE-NONE")) == nullptr);
}

TEST_CASE("find_code_analysis takes the latest registered module for an ELF machine")
{
  register_fake("fake-older", "FAKE-OLDER", &analysis(), 0xfff1);
  register_fake("fake-newer", "FAKE-NEWER", &fake, 0xfff1);
  CHECK(arch::find_code_analysis(std::uint16_t{0xfff1}) == &fake);
  register_fake("fake-older", "FAKE-OLDER", &analysis(), 0xfff1);
  CHECK(arch::find_code_analysis(std::uint16_t{0xfff1}) == &analysis());
}

TEST_CASE("find_code_analysis takes the latest registered module implementing a core")
{
  register_fake("fake-same-older", "FAKE-SAME", &analysis(), 0);
  register_fake("fake-same-newer", "FAKE-SAME", &fake, 0);
  CHECK(arch::find_code_analysis(core_named("FAKE-SAME")) == &fake);
  register_fake("fake-same-older", "FAKE-SAME", &analysis(), 0);
  CHECK(arch::find_code_analysis(core_named("FAKE-SAME")) == &analysis());
}

TEST_CASE("find_code_analysis by ELF machine passes over a newer module without an analysis")
{
  register_fake("fake-elf-with", "FAKE-ELF-WITH", &fake, 0xfff2);
  register_fake("fake-elf-without", "FAKE-ELF-WITHOUT", nullptr, 0xfff2);
  CHECK(arch::find_code_analysis(std::uint16_t{0xfff2}) == &fake);
}
