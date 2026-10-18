#include "../../../native/src/core/machine_impl.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/arch/armv8m/armv8m.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/builtin.hpp>

#include <doctest/doctest.h>

#include <unicorn/unicorn.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <initializer_list>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace periph = popcorn::periph;
namespace stm32  = popcorn::periph::stm32;
using popcorn::address;
using popcorn::bus_attrs;
using popcorn::icount;
using popcorn::initiator;
using popcorn::testing::u575;

constexpr bus_attrs secure_priv{.by = initiator::cpu, .secure = true, .privileged = true};
constexpr bus_attrs secure_unpriv{.by = initiator::cpu, .secure = true, .privileged = false};
constexpr bus_attrs nonsecure_priv{.by = initiator::cpu, .secure = false, .privileged = true};
constexpr bus_attrs nonsecure_unpriv{.by = initiator::cpu, .secure = false, .privileged = false};

constexpr address       flash_ns       = 0x0800'0000;
constexpr address       flash_s        = 0x0c00'0000;
constexpr address       page_bytes     = 0x2000;
constexpr address       idle_at        = 0x2000'2000;
constexpr address       code_at        = 0x2000'2100;
constexpr address       vectors_at     = 0x2000'3000;
constexpr address       handler_at     = 0x2000'3100;
constexpr address       stack_top      = 0x2000'3800;
constexpr address       vtor           = 0xe000'ed08;
constexpr address       hfsr           = 0xe000'ed2c;
constexpr address       cfsr           = 0xe000'ed28;
constexpr address       sau_ctrl       = 0xe000'edd0;
constexpr address       sau_rnr        = 0xe000'edd8;
constexpr address       sau_rbar       = 0xe000'eddc;
constexpr address       sau_rlar       = 0xe000'ede0;
constexpr std::uint32_t sau_enable     = 1;
constexpr address       nonsecure_cfsr = 0xe002'ed28;
constexpr std::uint32_t sau_allns      = 2;
constexpr std::uint32_t key1           = 0x4567'0123;
constexpr std::uint32_t key2           = 0xcdef'89ab;
constexpr std::uint32_t pg             = 1u << 0;
constexpr std::uint32_t per            = 1u << 1;
constexpr std::uint32_t mer1           = 1u << 2;
constexpr std::uint32_t strt           = 1u << 16;
constexpr std::uint32_t optstrt        = 1u << 17;
constexpr std::uint32_t eopie          = 1u << 24;
constexpr std::uint32_t inv            = 1u << 29;
constexpr std::uint32_t eop            = 1u << 0;
constexpr std::uint32_t wrperr         = 1u << 4;
constexpr std::uint32_t pgserr         = 1u << 7;
constexpr std::uint32_t bsy            = 1u << 16;
constexpr std::uint32_t flash_irq      = 6;
constexpr std::uint32_t flash_s_irq    = 7;
constexpr std::uint32_t cfsr_ibuserr   = 1u << 8;
constexpr std::uint32_t hfsr_forced    = 1u << 30;
constexpr std::uint16_t bkpt           = 0xbe01;
constexpr std::uint16_t ldr_r2_r0      = 0x6802;

auto pnb(std::uint32_t page) -> std::uint32_t
{
  return page << 3;
}

class probe final : public periph::model
{
public:
  explicit probe(periph::model_context& ctx) : ctx_{ctx}
  {}

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }

  void reset(periph::register_file&, popcorn::reset_kind) override
  {}

  auto on_read(periph::register_file&, std::uint32_t, std::uint8_t, periph::access_ctx)
      -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    return 0;
  }

  auto on_write(
      periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t,
      periph::access_ctx) -> std::expected<void, popcorn::bus_error> override
  {
    return {};
  }

  void on_event(std::uint32_t, std::uint64_t) override
  {}

  void save(std::vector<std::byte>&) const override
  {}

  void load(std::span<std::byte const>) override
  {}

  [[nodiscard]] auto services() const -> core::machine_services&
  {
    return ctx_.services;
  }

private:
  periph::model_context& ctx_;
};

auto unit(std::string_view name) -> popcorn::peripheral_desc const&
{
  return *std::ranges::find(u575().peripherals, name, &popcorn::peripheral_desc::name);
}

auto address_of(std::string_view peripheral, std::string_view reg) -> address
{
  auto const& p     = unit(peripheral);
  auto const* found = popcorn::find_register(p, reg);
  REQUIRE(found != nullptr);
  return p.base + found->offset;
}

auto bit(std::string_view peripheral, std::string_view reg, std::string_view field) -> std::uint32_t
{
  auto const* found = popcorn::find_register(unit(peripheral), reg);
  REQUIRE(found != nullptr);
  auto const bits = std::ranges::find(found->fields, field, &popcorn::field_desc::name);
  REQUIRE(bits != found->fields.end());
  return std::uint32_t{1} << bits->lsb;
}

struct rig
{
  std::unique_ptr<core::machine> target;
  probe*                         bus{};
  std::vector<core::log_record>  logged{};
  core::hook_handle              sink{};

  explicit rig(
      bool                        trustzone,
      stm32::option_bytes         wanted = {.tzen = true},
      popcorn::arch::fault_policy faults = popcorn::arch::fault_policy::stop)
  {
    stm32::register_builtin_models();
    auto options   = popcorn::testing::stm32_options();
    options.faults = faults;
    options.raz_wi = stm32::raz_wi_map(u575());
    if(trustzone)
    {
      wanted.tzen = true;
      REQUIRE(stm32::configure_option_bytes(u575(), wanted, options).has_value());
    }
    options.models.emplace(
        "LPGPIO1", [this](periph::model_context& ctx, popcorn::peripheral_desc const&) {
          auto made = std::make_shared<probe>(ctx);
          bus       = made.get();
          return made;
        });
    target = std::make_unique<core::machine>(u575(), popcorn::arch::armv8m::factory(), options);
    sink =
        target->add_log_sink([this](core::log_record const& record) { logged.push_back(record); });
    target->reset();
    REQUIRE(target->mem().write(idle_at, 2, 0xe7fe));
  }

  auto write(std::string_view reg, std::uint32_t value, bus_attrs attrs) const -> void
  {
    REQUIRE(bus->services().write(address_of("FLASH", reg), 4, value, attrs).has_value());
  }

  auto write(std::string_view peripheral, std::string_view reg, std::uint32_t value) const -> void
  {
    REQUIRE(bus->services().write(address_of(peripheral, reg), 4, value, secure_priv).has_value());
  }

  [[nodiscard]] auto read(std::string_view reg, bus_attrs attrs) const -> std::uint32_t
  {
    auto const value = bus->services().read(address_of("FLASH", reg), 4, attrs);
    REQUIRE(value.has_value());
    return *value;
  }

  [[nodiscard]] auto peek(std::string_view peripheral, std::string_view reg) const -> std::uint32_t
  {
    auto const* found = popcorn::find_register(unit(peripheral), reg);
    REQUIRE(found != nullptr);
    return target->peripheral(peripheral).peek(found->offset & ~3u);
  }

  [[nodiscard]] auto peek(std::string_view reg) const -> std::uint32_t
  {
    return peek("FLASH", reg);
  }

  [[nodiscard]] auto word(address at) const -> std::uint32_t
  {
    auto const value = target->mem().read(at, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  [[nodiscard]] auto pending(std::uint32_t irq) const -> bool
  {
    return (word(0xe000'e200u + 4 * (irq / 32)) >> (irq % 32) & 1u) != 0;
  }

  [[nodiscard]] auto tzic(std::string_view field) const -> bool
  {
    return (peek("GTZC1_TZIC", "SR4") & bit("GTZC1_TZIC", "SR4", field)) != 0;
  }

  auto clear_events() const -> void
  {
    write("GTZC1_TZIC", "FCR4", ~0u);
    REQUIRE(peek("GTZC1_TZIC", "SR4") == 0);
  }

  auto unlock(bool secure) const -> void
  {
    auto const attrs = secure ? secure_priv : nonsecure_priv;
    write(secure ? "SECKEYR" : "NSKEYR", key1, attrs);
    write(secure ? "SECKEYR" : "NSKEYR", key2, attrs);
    REQUIRE((peek(secure ? "SECCR" : "NSCR") >> 31) == 0);
  }

  auto store(address at, std::initializer_list<std::uint32_t> words, bus_attrs attrs) const -> void
  {
    for(auto const [index, value] : std::views::enumerate(words))
    {
      REQUIRE(
          bus->services().write(at + 4 * static_cast<address>(index), 4, value, attrs).has_value());
    }
  }

  auto settle(std::chrono::microseconds wait = std::chrono::microseconds{2000}) const -> void
  {
    auto const& clock = target->virtual_clock();
    auto const  until =
        popcorn::vtime{clock.time().ps + static_cast<std::uint64_t>(wait.count()) * 1'000'000u};
    target->regs().write(*target->regs().find("pc"), idle_at | 1u);
    for(;;)
    {
      auto const result = target->run_slice(
          {.max_instructions = icount{50'000'000},
           .wall_budget      = std::chrono::seconds{60},
           .until_time       = until});
      if(result.stopped)
      {
        REQUIRE(result.stopped->reason == popcorn::stop_reason::time);
        return;
      }
    }
  }

  auto set(std::string_view name, std::uint64_t value) const -> void
  {
    target->regs().write(*target->regs().find(name), value);
  }

  [[nodiscard]] auto get(std::string_view name) const -> std::uint64_t
  {
    return target->regs().read(*target->regs().find(name));
  }

  auto engine_secure(bool secure) const -> void
  {
    std::uint32_t value = secure ? 1 : 0;
    REQUIRE(
        uc_reg_write(core::machine_internals::engine(*target), UC_ARM_REG_SECURE, &value)
        == UC_ERR_OK);
  }

  auto put(address at, std::initializer_list<std::uint16_t> code) const -> void
  {
    for(auto const [index, insn] : std::views::enumerate(code))
    {
      REQUIRE(target->mem().write(at + 2 * static_cast<address>(index), 2, insn));
    }
  }

  auto sau_region(std::uint32_t number, address base, address limit) const -> void
  {
    REQUIRE(target->mem().write(sau_rnr, 4, number));
    REQUIRE(target->mem().write(sau_rbar, 4, base));
    REQUIRE(target->mem().write(sau_rlar, 4, (limit & ~0x1fu) | 1u));
  }

  auto run_code(std::initializer_list<std::uint16_t> code, icount bound) const -> popcorn::stop
  {
    put(code_at, code);
    set("pc", code_at | 1u);
    auto const result = target->run_slice({.max_instructions = bound});
    REQUIRE(result.stopped.has_value());
    return *result.stopped;
  }

  [[nodiscard]] auto warned(std::string_view text) const -> std::size_t
  {
    return static_cast<std::size_t>(std::ranges::count_if(logged, [text](auto const& record) {
      return record.peripheral == "FLASH" and record.message.contains(text);
    }));
  }
};

auto nonsecure_code(rig const& r) -> void
{
  auto const block = (code_at - 0x2000'0000) / 0x200;
  for(auto const prefix : {"SECCFGR", "PRIVCFGR"})
  {
    auto const name = std::format("{}{}", prefix, block / 32);
    r.write("GTZC1_MPCBB1", name, r.peek("GTZC1_MPCBB1", name) & ~(1u << (block % 32)));
  }
  REQUIRE(r.target->mem().write(sau_ctrl, 4, sau_allns));
  r.engine_secure(false);
}

constexpr stm32::option_bytes four_secure_pages{
    .tzen   = true,
    .secwm1 = stm32::page_range{.first = 0, .last = 3},
    .secwm2 = stm32::no_area{}
};

TEST_CASE(
    "with TZEN = 0 SECKEYR, SECSR, SECCR, SECBB and SECHDPCR read zero and ignore writes, SECCR "
    "no longer 0x80000000, RM0456 7.5.1")
{
  rig const r{false};
  CHECK(r.peek("SECCR") == 0x8000'0000u);
  for(auto const reg : {"SECKEYR", "SECSR", "SECCR", "SEC1BBR1", "SEC2BBR8", "SECHDPCR"})
  {
    CAPTURE(reg);
    r.write(reg, 0x5a5a'0003, secure_priv);
    r.write(reg, 0x5a5a'0003, nonsecure_priv);
    CHECK(r.read(reg, secure_priv) == 0);
    CHECK(r.read(reg, nonsecure_priv) == 0);
    CHECK(r.word(address_of("FLASH", reg)) == 0);
  }
  CHECK(r.peek("SECCR") == 0x8000'0000u);
  CHECK(r.peek("SEC1BBR1") == 0);
  r.write("PRIVCFGR", 3, nonsecure_unpriv);
  CHECK(r.read("PRIVCFGR", nonsecure_unpriv) == 3);
  r.write("PRIV1BBR1", 0x10, nonsecure_unpriv);
  CHECK(r.read("PRIV1BBR1", nonsecure_unpriv) == 0x10);
}

TEST_CASE(
    "RM0456 Table 68 and 7.3.7: a program through each register set lands only on a page of its "
    "security, SECWM1 pages 0 to 3")
{
  rig const r{true, four_secure_pages};
  r.unlock(true);
  r.unlock(false);
  r.write("SECCR", pg | eopie, secure_priv);
  r.store(flash_s, {1, 2, 3, 4}, secure_priv);
  CHECK((r.peek("SECSR") & bsy) != 0);
  CHECK((r.peek("NSSR") & bsy) != 0);
  r.settle();
  CHECK(r.word(flash_s + 12) == 4);
  CHECK(r.peek("SECSR") == eop);
  CHECK(r.peek("NSSR") == 0);
  CHECK(r.pending(flash_s_irq));
  CHECK_FALSE(r.pending(flash_irq));
  CHECK_FALSE(r.tzic("FLASHF"));
  r.write("SECSR", eop, secure_priv);

  r.store(flash_s + 8 * page_bytes, {5, 6, 7, 8}, secure_priv);
  r.settle();
  CHECK(r.word(flash_ns + 8 * page_bytes) == 0xffff'ffff);
  CHECK((r.peek("SECSR") & wrperr) != 0);
  CHECK((r.peek("NSSR") & wrperr) == 0);
  CHECK(r.tzic("FLASHF"));
  r.clear_events();
  r.write("SECCR", 0, secure_priv);
  r.write("SECSR", ~0u, secure_priv);

  r.write("NSCR", pg, nonsecure_priv);
  r.store(flash_ns + 8 * page_bytes, {5, 6, 7, 8}, nonsecure_priv);
  r.settle();
  CHECK(r.word(flash_ns + 8 * page_bytes + 4) == 6);
  CHECK(r.peek("NSSR") == 0);
  CHECK_FALSE(r.tzic("FLASHF"));

  r.store(flash_ns + 16, {9, 10, 11, 12}, nonsecure_priv);
  r.settle();
  CHECK(r.word(flash_s + 16) == 0xffff'ffff);
  CHECK((r.peek("NSSR") & wrperr) != 0);
  CHECK((r.peek("SECSR") & wrperr) == 0);
  CHECK(r.tzic("FLASHF"));
}

TEST_CASE(
    "RM0456 Table 68: the bootloader's straddling erase, page 3 through SECCR and page 4 through "
    "NSCR, erases both, and each set refuses the other's page")
{
  rig const r{true, four_secure_pages};
  for(address const at : {flash_ns + 3 * page_bytes, flash_ns + 4 * page_bytes})
  {
    REQUIRE(r.target->mem().write(at, 4, 0));
  }
  r.unlock(true);
  r.unlock(false);
  r.write("NSCR", per | pnb(3) | strt, nonsecure_priv);
  CHECK((r.peek("NSSR") & wrperr) != 0);
  CHECK((r.peek("NSSR") & bsy) == 0);
  CHECK((r.peek("NSCR") & strt) == 0);
  CHECK(r.tzic("FLASHF"));
  r.clear_events();
  r.write("SECCR", per | pnb(4) | strt, secure_priv);
  CHECK((r.peek("SECSR") & wrperr) != 0);
  CHECK(r.tzic("FLASHF"));
  r.write("NSSR", ~0u, nonsecure_priv);
  r.write("SECSR", ~0u, secure_priv);

  r.write("SECCR", per | pnb(3) | strt, secure_priv);
  r.settle();
  r.write("SECCR", 0, secure_priv);
  r.write("NSCR", per | pnb(4) | strt, nonsecure_priv);
  r.settle();
  CHECK(r.word(flash_ns + 3 * page_bytes) == 0xffff'ffff);
  CHECK(r.word(flash_ns + 4 * page_bytes) == 0xffff'ffff);
  CHECK(r.peek("NSSR") == 0);
  CHECK(r.peek("SECSR") == 0);
}

TEST_CASE(
    "RM0456 Table 70: a bank erase over Secure and Non-secure pages is refused whole by either "
    "set, a bank erase of an all-Secure bank runs")
{
  {
    rig const r{true, four_secure_pages};
    REQUIRE(r.target->mem().write(flash_ns + 5 * page_bytes, 4, 0));
    r.unlock(true);
    r.unlock(false);
    r.write("SECCR", mer1 | strt, secure_priv);
    CHECK((r.peek("SECSR") & wrperr) != 0);
    CHECK(r.tzic("FLASHF"));
    r.clear_events();
    r.write("NSCR", mer1 | strt, nonsecure_priv);
    CHECK((r.peek("NSSR") & wrperr) != 0);
    CHECK(r.tzic("FLASHF"));
    CHECK(r.word(flash_ns + 5 * page_bytes) == 0);
  }
  {
    rig const r{true};
    REQUIRE(r.target->mem().write(flash_ns + 5 * page_bytes, 4, 0));
    r.unlock(true);
    r.write("SECCR", mer1 | strt, secure_priv);
    CHECK((r.peek("SECSR") & bsy) != 0);
    r.settle(std::chrono::microseconds{200'000});
    CHECK(r.word(flash_ns + 5 * page_bytes) == 0xffff'ffff);
    CHECK(r.peek("SECSR") == 0);
  }
}

TEST_CASE(
    "RM0456 7.3.9: the Non-secure STRT set by a Secure access is a Non-secure PGSERR, a store of "
    "the other set into a filling buffer a PGSERR of its own set, popcorn's rule, RM0456 leaves "
    "it unstated")
{
  rig const r{true, four_secure_pages};
  r.unlock(true);
  r.unlock(false);
  r.write("NSCR", per | pnb(5) | strt, secure_priv);
  CHECK((r.peek("NSSR") & pgserr) != 0);
  CHECK((r.peek("NSSR") & bsy) == 0);
  r.write("NSSR", ~0u, nonsecure_priv);
  r.write("NSCR", pg, nonsecure_priv);
  r.write("SECCR", pg, secure_priv);
  r.store(flash_ns + 8 * page_bytes, {1}, nonsecure_priv);
  CHECK((r.peek("NSSR") & (1u << 17)) != 0);
  CHECK((r.peek("SECSR") & (1u << 17)) != 0);
  r.store(flash_s, {2}, secure_priv);
  CHECK((r.peek("SECSR") & pgserr) != 0);
  CHECK((r.peek("NSSR") & pgserr) == 0);
  CHECK((r.peek("NSSR") & (1u << 17)) == 0);
}

TEST_CASE(
    "RM0456 Table 72: a Non-secure access to SECCR is RAZ/WI with FLASH_REGIE, SPRIV and NSPRIV "
    "make their set RAZ/WI to unprivileged accesses")
{
  rig const r{true, four_secure_pages};
  CHECK(r.read("SECCR", nonsecure_priv) == 0);
  CHECK(r.tzic("FLASH_REGF"));
  r.clear_events();
  r.write("SECKEYR", key1, nonsecure_priv);
  r.write("SECKEYR", key2, nonsecure_priv);
  CHECK((r.peek("SECCR") >> 31) == 1);
  CHECK(r.tzic("FLASH_REGF"));
  r.clear_events();
  CHECK(r.read("SECCR", secure_unpriv) == 0x8000'0000u);
  CHECK(r.read("NSCR", nonsecure_unpriv) == 0xc000'0000u);
  CHECK_FALSE(r.tzic("FLASH_REGF"));

  r.write("PRIVCFGR", 3, nonsecure_unpriv);
  CHECK(r.peek("PRIVCFGR") == 0);
  r.write("PRIVCFGR", 3, nonsecure_priv);
  CHECK(r.peek("PRIVCFGR") == 2);
  r.write("PRIVCFGR", 3, secure_priv);
  CHECK(r.peek("PRIVCFGR") == 3);
  CHECK(r.read("PRIVCFGR", nonsecure_unpriv) == 3);
  CHECK(r.read("SECCR", secure_unpriv) == 0);
  CHECK(r.read("NSCR", nonsecure_unpriv) == 0);
  CHECK(r.read("SECCR", secure_priv) == 0x8000'0000u);
  r.write("NSKEYR", key1, nonsecure_unpriv);
  r.write("NSKEYR", key2, nonsecure_unpriv);
  CHECK((r.peek("NSCR") >> 31) == 1);
  CHECK_FALSE(r.tzic("FLASH_REGF"));
}

TEST_CASE(
    "RM0456 Tables 75 and 76: SECBB takes a Secure write, from an unprivileged one only the "
    "unprivileged pages, PRIVBB a privileged one, from Non-secure only Non-secure pages")
{
  rig const r{true, four_secure_pages};
  r.write("SEC1BBR1", 0xf0, nonsecure_priv);
  CHECK(r.peek("SEC1BBR1") == 0);
  CHECK(r.tzic("FLASH_REGF"));
  CHECK(r.read("SEC1BBR1", nonsecure_unpriv) == 0);
  r.clear_events();
  r.write("PRIV1BBR1", 0xff, nonsecure_unpriv);
  CHECK(r.peek("PRIV1BBR1") == 0);
  r.write("PRIV1BBR1", 0xff, nonsecure_priv);
  CHECK(r.peek("PRIV1BBR1") == 0xf0);
  r.write("PRIV1BBR1", 0x30, secure_priv);
  CHECK(r.peek("PRIV1BBR1") == 0x30);
  r.write("SEC1BBR1", 0x330, secure_unpriv);
  CHECK(r.peek("SEC1BBR1") == 0x300);
  r.write("SEC1BBR1", 0x330, secure_priv);
  CHECK(r.peek("SEC1BBR1") == 0x330);
  CHECK(r.read("SEC1BBR1", nonsecure_unpriv) == 0x330);
  r.write("PRIV1BBR1", 0x00, nonsecure_priv);
  CHECK(r.peek("PRIV1BBR1") == 0x30);
  r.write("PRIV1BBR2", ~0u, nonsecure_priv);
  CHECK(r.peek("PRIV1BBR2") == ~0u);
  r.write("PRIV1BBR3", ~0u, nonsecure_priv);
  CHECK(r.peek("PRIV1BBR3") == 0);
  CHECK_FALSE(r.tzic("FLASH_REGF"));
}

TEST_CASE(
    "RM0456 7.5.4 and Table 68: a SECBB bit makes page 9 Secure: a Non-secure program is refused "
    "and a Non-secure load that read data before reads 0")
{
  rig const         r{true, four_secure_pages};
  constexpr address page9 = flash_ns + 9 * page_bytes;
  REQUIRE(r.target->mem().write(page9 + 0x100, 4, 0x1234'5678));
  nonsecure_code(r);
  auto const load = [&r] {
    r.set("r0", page9 + 0x100);
    r.set("r2", 0xdead'beef);
    REQUIRE(r.run_code({ldr_r2_r0, bkpt}, icount{2}).reason == popcorn::stop_reason::user);
    return r.get("r2");
  };
  CHECK(load() == 0x1234'5678);
  r.write("SEC1BBR1", 1u << 9, secure_priv);
  CHECK(load() == 0);
  CHECK(r.tzic("FLASHF"));
  r.clear_events();
  r.unlock(false);
  r.write("NSCR", pg, nonsecure_priv);
  r.store(page9, {1, 2, 3, 4}, nonsecure_priv);
  CHECK((r.peek("NSSR") & wrperr) != 0);
  CHECK(r.word(page9) == 0xffff'ffff);
  CHECK(r.tzic("FLASHF"));
}

TEST_CASE(
    "RM0456 Table 73: an unprivileged program or erase of a privileged page is refused with "
    "WRPERR and no event, a privileged one lands")
{
  rig const         r{true, four_secure_pages};
  constexpr address page6 = flash_ns + 6 * page_bytes;
  r.write("PRIV1BBR1", 1u << 6, secure_priv);
  r.unlock(false);
  r.write("NSCR", pg, nonsecure_priv);
  r.store(page6, {1, 2, 3, 4}, nonsecure_unpriv);
  CHECK((r.peek("NSSR") & wrperr) != 0);
  CHECK_FALSE(r.tzic("FLASHF"));
  r.write("NSSR", ~0u, nonsecure_priv);
  r.store(page6, {1, 2, 3, 4}, nonsecure_priv);
  r.settle();
  CHECK(r.word(page6 + 8) == 3);
  r.write("NSCR", per | pnb(6) | strt, nonsecure_unpriv);
  CHECK((r.peek("NSSR") & wrperr) != 0);
  CHECK(r.word(page6 + 8) == 3);
}

TEST_CASE(
    "RM0456 7.5.3 and Table 68: with HDP1_ACCDIS set a Secure program and erase of the HDP area "
    "are refused, the bit is set-only and a system reset clears it")
{
  rig const r{
      true,
      {.secwm1 = stm32::page_range{.first = 0, .last = 3},
        .secwm2 = stm32::no_area{},
        .hdp1   = stm32::page_range{.first = 0, .last = 1}}
  };
  r.unlock(true);
  r.write("SECCR", pg, secure_priv);
  r.store(flash_s + page_bytes, {1, 2, 3, 4}, secure_priv);
  r.settle();
  CHECK(r.word(flash_s + page_bytes) == 1);
  r.write("SECHDPCR", 1, secure_priv);
  r.write("SECHDPCR", 0, secure_priv);
  CHECK(r.peek("SECHDPCR") == 1);
  r.store(flash_s + page_bytes + 16, {1, 2, 3, 4}, secure_priv);
  CHECK((r.peek("SECSR") & wrperr) != 0);
  CHECK_FALSE(r.tzic("FLASHF"));
  r.write("SECSR", ~0u, secure_priv);
  r.write("SECCR", per | pnb(1) | strt, secure_priv);
  CHECK((r.peek("SECSR") & wrperr) != 0);
  r.write("SECSR", ~0u, secure_priv);
  r.write("SECCR", per | pnb(2) | strt, secure_priv);
  CHECK((r.peek("SECSR") & bsy) != 0);
  r.settle();
  r.target->reset(popcorn::reset_kind::software);
  CHECK(r.peek("SECHDPCR") == 0);
}

TEST_CASE("SECCR.INV is stored, and OPTSTRT under TrustZone warns once, RM0456 7.5.5 and 7.9.9")
{
  rig const r{true, four_secure_pages};
  r.unlock(true);
  r.write("SECCR", inv, secure_priv);
  CHECK(r.read("SECCR", secure_priv) == inv);
  r.unlock(false);
  r.write("NSCR", optstrt, nonsecure_priv);
  r.write("NSCR", optstrt, nonsecure_priv);
  CHECK((r.peek("NSCR") & optstrt) == 0);
  CHECK(r.warned("option-byte programming is not modelled") == 1);
}

TEST_CASE(
    "a Secure core's store through an SAU Non-secure region programs a Non-secure page through "
    "NSCR, without the region it is a Secure store refused with SECSR.WRPERR, B10.2")
{
  constexpr address page8 = flash_ns + 8 * page_bytes;
  for(auto const sau_over : {true, false})
  {
    CAPTURE(sau_over);
    rig const r{true, four_secure_pages};
    r.unlock(false);
    r.write("NSCR", pg, nonsecure_priv);
    r.sau_region(0, page8, page8 + page_bytes - 32);
    REQUIRE(r.target->mem().write(sau_ctrl, 4, sau_over ? sau_enable : 0u));
    r.set("r0", page8);
    r.set("r1", 0x0bad'f00d);
    auto const stopped = r.run_code({0x6001, 0x6041, 0x6081, 0x60c1, bkpt}, icount{5});
    CHECK(stopped.reason == popcorn::stop_reason::user);
    REQUIRE(r.target->mem().write(sau_ctrl, 4, 0));
    r.settle();
    CHECK(r.word(page8 + 12) == (sau_over ? 0x0bad'f00du : 0xffff'ffffu));
    CHECK((r.peek("NSSR") & wrperr) == 0);
    CHECK(((r.peek("SECSR") & wrperr) != 0) == not sau_over);
  }
}

TEST_CASE(
    "a SECBB store that makes the running Secure loop's flash page Non-secure faults its next "
    "fetch in an SAU-split page, RM0456 7.5.4, Table 68 and B3.11 RJQMQ")
{
  rig const         r{true, four_secure_pages, popcorn::arch::fault_policy::enter_handler};
  constexpr address page4 = flash_ns + 4 * page_bytes;
  r.write("SEC1BBR1", 1u << 4, secure_priv);
  r.put(handler_at, {bkpt});
  REQUIRE(r.target->mem().write(vectors_at, 4, stack_top));
  REQUIRE(r.target->mem().write(vectors_at + 4 * 3, 4, handler_at | 1u));
  REQUIRE(r.target->mem().write(vtor, 4, vectors_at));
  r.set("msp", stack_top);
  r.put(page4, {0x3b01, 0x42a3, 0xd100, 0x6001, 0x2b00, 0xd1f9, bkpt});
  r.sau_region(0, page4 + 0x200, page4 + 0x3e0);
  REQUIRE(r.target->mem().write(sau_ctrl, 4, sau_enable));
  r.set("r0", address_of("FLASH", "SEC1BBR1"));
  r.set("r1", 0);
  r.set("r3", 100);
  r.set("r4", 50);
  r.set("pc", page4 | 1u);
  auto const result = r.target->run_slice({.max_instructions = icount{2000}});
  REQUIRE(result.stopped.has_value());
  CHECK(result.stopped->reason == popcorn::stop_reason::user);
  CHECK(r.get("pc") == handler_at + 2);
  CHECK((r.get("xpsr") & 0x1ff) == 3);
  CHECK((r.word(hfsr) & hfsr_forced) != 0);
  CHECK((r.word(cfsr) & cfsr_ibuserr) != 0);
  CHECK(r.word(static_cast<address>(r.get("msp")) + 24) == page4 + 8);
  CHECK(r.get("r3") == 50);
}

TEST_CASE("rewriting SECBB with its value keeps the page verdicts and the engine run")
{
  auto const runs = [](std::uint32_t toggle) {
    rig const  r{true, four_secure_pages};
    auto const before = core::machine_internals::counters(*r.target).engine_runs;
    r.put(code_at, {0x4071, 0x6001, 0x3b01, 0xd1fb, bkpt});
    r.set("r0", address_of("FLASH", "SEC1BBR1"));
    r.set("r1", 0);
    r.set("r6", toggle);
    r.set("r3", 1000);
    r.set("pc", code_at | 1u);
    for(;;)
    {
      auto const result = r.target->run_slice({.max_instructions = icount{10'000}});
      if(result.stopped)
      {
        REQUIRE(result.stopped->reason == popcorn::stop_reason::user);
        break;
      }
    }
    return core::machine_internals::counters(*r.target).engine_runs - before;
  };
  CHECK(runs(0) <= 2);
  CHECK(runs(1u << 5) >= 1000);
}

TEST_CASE(
    "a TZEN=1 snapshot taken while a Secure program is BSY ends it in SECSR after a restore into "
    "a fresh machine, the Secure lock state kept")
{
  rig const r{true, four_secure_pages};
  r.unlock(true);
  r.write("SECCR", pg | eopie, secure_priv);
  r.store(flash_s + 32, {1, 2, 3, 4}, secure_priv);
  REQUIRE((r.peek("SECSR") & bsy) != 0);
  auto const taken = r.target->snapshot();
  rig const  fresh{true, four_secure_pages};
  fresh.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(taken.data()))});
  fresh.settle();
  CHECK(fresh.word(flash_s + 44) == 4);
  CHECK(fresh.peek("SECSR") == eop);
  CHECK((fresh.peek("SECCR") >> 31) == 0);
  CHECK(fresh.pending(flash_s_irq));
}

TEST_CASE(
    "power_cycle under TrustZone keeps the option bytes, resets SECBB, HDP1_ACCDIS, the MPCBB, "
    "the SAU and the security state")
{
  rig const r{
      true,
      {.secwm1 = stm32::page_range{.first = 0, .last = 3},
        .secwm2 = stm32::no_area{},
        .hdp1   = stm32::page_range{.first = 0, .last = 1}}
  };
  auto const secwm = r.peek("SECWM1R1");
  r.write("SEC1BBR1", 0xf0, secure_priv);
  r.write("SECHDPCR", 1, secure_priv);
  r.write("GTZC1_MPCBB1", "SECCFGR0", 0xffff'fffe);
  r.sau_region(0, 0x2000'0000, 0x2000'03e0);
  REQUIRE(r.target->mem().write(sau_ctrl, 4, sau_enable));
  r.engine_secure(false);
  r.target->power_cycle();
  CHECK(r.peek("SECWM1R1") == secwm);
  CHECK(r.peek("SEC1BBR1") == 0);
  CHECK(r.peek("SECHDPCR") == 0);
  CHECK(r.peek("GTZC1_MPCBB1", "SECCFGR0") == 0xffff'ffff);
  CHECK(r.word(sau_ctrl) == 0);
  std::uint32_t secure = 0;
  REQUIRE(
      uc_reg_read(core::machine_internals::engine(*r.target), UC_ARM_REG_SECURE, &secure)
      == UC_ERR_OK);
  CHECK(secure == 1);
  CHECK((r.get("pc") & ~1u) == (r.word(flash_s + 4) & ~1u));
}

constexpr stm32::option_bytes hidden_pages{
    .tzen   = true,
    .secwm1 = stm32::page_range{.first = 0, .last = 3},
    .secwm2 = stm32::no_area{},
    .hdp1   = stm32::page_range{.first = 0, .last = 1}
};

auto secure_vectors(rig const& r) -> void
{
  r.put(handler_at, {bkpt});
  REQUIRE(r.target->mem().write(vectors_at, 4, stack_top));
  REQUIRE(r.target->mem().write(vectors_at + 4 * 3, 4, handler_at | 1u));
  REQUIRE(r.target->mem().write(vtor, 4, vectors_at));
  r.set("msp", stack_top);
}

auto load_from(rig const& r, address at, bool nonsecure, bool unprivileged) -> std::uint64_t
{
  if(nonsecure)
  {
    nonsecure_code(r);
  }
  r.set("control", unprivileged ? 1 : 0);
  r.set("r0", at);
  r.set("r2", 0xdead'beef);
  REQUIRE(r.run_code({ldr_r2_r0, bkpt}, icount{2}).reason == popcorn::stop_reason::user);
  r.set("control", 0);
  if(nonsecure)
  {
    r.engine_secure(true);
    REQUIRE(r.target->mem().write(sau_ctrl, 4, 0));
  }
  return r.get("r2");
}

TEST_CASE(
    "RM0456 7.5.3 and Table 68: with HDP1_ACCDIS set the HDP area reads 0 to a Secure load "
    "with no event and to a Non-secure one with FLASHF, a Secure fetch is a bus fault")
{
  rig const r{true, hidden_pages, popcorn::arch::fault_policy::enter_handler};
  REQUIRE(r.target->mem().write(flash_ns + page_bytes + 0x40, 4, 0x4444'4444));
  REQUIRE(r.target->mem().write(flash_ns + 2 * page_bytes, 4, 0x2222'2222));
  r.put(flash_ns + page_bytes, {bkpt});
  CHECK(load_from(r, flash_s + page_bytes + 0x40, false, false) == 0x4444'4444);
  r.write("SECHDPCR", 1, secure_priv);
  CHECK(load_from(r, flash_s + page_bytes + 0x40, false, false) == 0);
  CHECK_FALSE(r.tzic("FLASHF"));
  CHECK(load_from(r, flash_s + 2 * page_bytes, false, false) == 0x2222'2222);
  CHECK(load_from(r, flash_ns + page_bytes + 0x40, true, false) == 0);
  CHECK(r.tzic("FLASHF"));
  r.clear_events();
  secure_vectors(r);
  r.set("pc", (flash_s + page_bytes) | 1u);
  auto const fetched = r.target->run_slice({.max_instructions = icount{4}});
  REQUIRE(fetched.stopped.has_value());
  CHECK(fetched.stopped->reason == popcorn::stop_reason::user);
  CHECK(r.get("pc") == handler_at + 2);
  CHECK((r.get("xpsr") & 0x1ff) == 3);
  CHECK((r.word(hfsr) & hfsr_forced) != 0);
  CHECK((r.word(cfsr) & cfsr_ibuserr) != 0);
  CHECK(r.word(static_cast<address>(r.get("msp")) + 24) == flash_s + page_bytes);
  r.target->reset(popcorn::reset_kind::software);
  REQUIRE(r.target->mem().write(idle_at, 2, 0xe7fe));
  CHECK(load_from(r, flash_s + page_bytes + 0x40, false, false) == 0x4444'4444);
}

TEST_CASE(
    "RM0456 7.5.6 and Table 73: a PRIVBB page reads 0 to an unprivileged load with no event and "
    "data to a privileged one, an unprivileged fetch is a bus fault, popcorn's deviation from the "
    "zeros of Table 73")
{
  rig const         r{true, four_secure_pages, popcorn::arch::fault_policy::enter_handler};
  constexpr address page6 = flash_ns + 6 * page_bytes;
  REQUIRE(r.target->mem().write(page6 + 0x20, 4, 0x6666'6666));
  CHECK(load_from(r, page6 + 0x20, true, true) == 0x6666'6666);
  r.write("PRIV1BBR1", 1u << 6, secure_priv);
  CHECK(load_from(r, page6 + 0x20, true, true) == 0);
  CHECK(load_from(r, page6 + 0x20, true, false) == 0x6666'6666);
  CHECK_FALSE(r.tzic("FLASHF"));
  r.put(page6, {bkpt});
  r.put(page6 + page_bytes, {bkpt});
  nonsecure_code(r);
  r.set("control", 1);
  r.set("pc", (page6 + page_bytes) | 1u);
  auto const control = r.target->run_slice({.max_instructions = icount{2}});
  REQUIRE(control.stopped.has_value());
  CHECK(control.stopped->reason == popcorn::stop_reason::user);
  secure_vectors(r);
  REQUIRE(r.target->mem().write(sau_ctrl, 4, 0));
  r.sau_region(0, page6, page6 + 2 * page_bytes - 0x20);
  r.sau_region(1, code_at & ~0x1ffu, (code_at & ~0x1ffu) + 0x1e0);
  REQUIRE(r.target->mem().write(sau_ctrl, 4, sau_enable));
  std::uint32_t stack = (code_at & ~0x1ffu) + 0x1f0;
  REQUIRE(
      uc_reg_write(core::machine_internals::engine(*r.target), UC_ARM_REG_MSP_NS, &stack)
      == UC_ERR_OK);
  r.set("pc", page6 | 1u);
  auto const fetched = r.target->run_slice({.max_instructions = icount{4}});
  REQUIRE(fetched.stopped.has_value());
  CHECK(fetched.stopped->reason == popcorn::stop_reason::user);
  CHECK(r.get("pc") == handler_at + 2);
  CHECK((r.get("xpsr") & 0x1ff) == 3);
  CHECK((r.word(hfsr) & hfsr_forced) != 0);
  CHECK((r.word(cfsr) & cfsr_ibuserr) != 0);
  CHECK((r.word(nonsecure_cfsr) & cfsr_ibuserr) == 0);
  std::uint32_t framed = 0;
  REQUIRE(
      uc_reg_read(core::machine_internals::engine(*r.target), UC_ARM_REG_MSP_NS, &framed)
      == UC_ERR_OK);
  CHECK(framed == stack - 32);
  CHECK(r.word(framed + 24) == page6);
}

TEST_CASE(
    "an SECHDPCR store that hides the running Secure loop's flash page faults its next fetch in "
    "an SAU-split page, RM0456 7.5.3 and B3.11 RJQMQ")
{
  rig const         r{true, hidden_pages, popcorn::arch::fault_policy::enter_handler};
  constexpr address page1 = flash_ns + page_bytes;
  r.put(handler_at, {bkpt});
  REQUIRE(r.target->mem().write(vectors_at, 4, stack_top));
  REQUIRE(r.target->mem().write(vectors_at + 4 * 3, 4, handler_at | 1u));
  REQUIRE(r.target->mem().write(vtor, 4, vectors_at));
  r.set("msp", stack_top);
  r.put(page1, {0x3b01, 0x42a3, 0xd100, 0x6001, 0x2b00, 0xd1f9, bkpt});
  r.sau_region(0, page1 + 0x200, page1 + 0x3e0);
  REQUIRE(r.target->mem().write(sau_ctrl, 4, sau_enable));
  r.set("r0", address_of("FLASH", "SECHDPCR"));
  r.set("r1", 1);
  r.set("r3", 100);
  r.set("r4", 50);
  r.set("pc", page1 | 1u);
  auto const result = r.target->run_slice({.max_instructions = icount{2000}});
  REQUIRE(result.stopped.has_value());
  CHECK(result.stopped->reason == popcorn::stop_reason::user);
  CHECK(r.get("pc") == handler_at + 2);
  CHECK((r.get("xpsr") & 0x1ff) == 3);
  CHECK((r.word(hfsr) & hfsr_forced) != 0);
  CHECK((r.word(cfsr) & cfsr_ibuserr) != 0);
  CHECK(r.word(static_cast<address>(r.get("msp")) + 24) == page1 + 8);
  CHECK(r.get("r3") == 50);
}
}

TEST_CASE(
    "a Non-secure sequence error while a Secure page erase is BSY leaves the erase running, "
    "RM0456 7.3.9 per set, the bus stall of RM0456 7.3.5 not modelled")
{
  rig const r{true, four_secure_pages};
  REQUIRE(r.target->mem().write(flash_ns + 2 * page_bytes, 4, 0));
  r.unlock(true);
  r.unlock(false);
  r.write("SECCR", per | pnb(2) | strt | eopie, secure_priv);
  REQUIRE((r.peek("SECSR") & bsy) != 0);
  r.write("NSCR", pg | per, nonsecure_priv);
  CHECK((r.peek("NSSR") & pgserr) != 0);
  CHECK((r.peek("SECSR") & bsy) != 0);
  CHECK((r.peek("SECCR") & strt) != 0);
  r.settle();
  CHECK(r.word(flash_ns + 2 * page_bytes) == 0xffff'ffff);
  CHECK((r.peek("SECSR") & eop) != 0);
  CHECK((r.peek("SECSR") & bsy) == 0);
  CHECK(r.pending(flash_s_irq));
}

TEST_CASE("a TrustZone machine whose FLASH has no option bytes is refused at construction")
{
  stm32::register_builtin_models();
  auto options      = popcorn::testing::stm32_options();
  options.trustzone = true;
  try
  {
    core::machine const target{u575(), popcorn::arch::armv8m::factory(), options};
    FAIL("built a TrustZone FLASH without option bytes");
  }
  catch(popcorn::config_error const& refused)
  {
    CHECK(refused.key() == "option_bytes");
  }
  options.trustzone = false;
  CHECK_NOTHROW(core::machine{u575(), popcorn::arch::armv8m::factory(), options});
}
