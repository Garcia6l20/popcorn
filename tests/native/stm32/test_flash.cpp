#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace periph = popcorn::periph;
using popcorn::address;
using popcorn::bus_error;
using popcorn::icount;
using popcorn::initiator;
using popcorn::stop_reason;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;
using popcorn::testing::u575;

constexpr address       bank2      = 0x0808'0000;
constexpr address       last_page  = 0x080f'e000;
constexpr std::uint32_t page_bytes = 0x2000;
constexpr std::uint32_t flash_irq  = 6;
constexpr std::uint32_t pg         = 1u << 0;
constexpr std::uint32_t per        = 1u << 1;
constexpr std::uint32_t mer1       = 1u << 2;
constexpr std::uint32_t bker       = 1u << 11;
constexpr std::uint32_t bwr        = 1u << 14;
constexpr std::uint32_t mer2       = 1u << 15;
constexpr std::uint32_t strt       = 1u << 16;
constexpr std::uint32_t optstrt    = 1u << 17;
constexpr std::uint32_t eopie      = 1u << 24;
constexpr std::uint32_t errie      = 1u << 25;
constexpr std::uint32_t all_errors = 0x20fb;
constexpr std::uint32_t eop        = 1u << 0;
constexpr std::uint32_t hsion      = 1u << 8;
constexpr std::uint32_t sw_hsi16   = 1;
constexpr std::uint32_t vos_range1 = 3u << 16;
constexpr std::uint32_t lpm        = 1u << 11;

auto pnb(std::uint32_t page) -> std::uint32_t
{
  return page << 3;
}

class services_probe final : public periph::model
{
public:
  explicit services_probe(periph::model_context& ctx) : ctx_{ctx}
  {}

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }
  void reset(periph::register_file&, popcorn::reset_kind) override
  {}
  auto on_read(periph::register_file&, std::uint32_t, std::uint8_t, periph::access_ctx)
      -> std::expected<std::uint32_t, bus_error> override
  {
    return 0u;
  }
  auto on_write(
      periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t,
      periph::access_ctx) -> std::expected<void, bus_error> override
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

auto board_options(
    periph::stm32::board const&      board,
    bool                             strict,
    std::shared_ptr<services_probe>* probe = nullptr) -> core::machine_options
{
  auto options               = popcorn::testing::stm32_options();
  options.strict_peripherals = strict;
  for(auto& [name, factory] : periph::stm32::factories(board, u575()))
  {
    options.models.emplace(name, std::move(factory));
  }
  if(probe != nullptr)
  {
    options.models.insert_or_assign(
        "CRC",
        [probe](periph::model_context& ctx, popcorn::peripheral_desc const&)
            -> std::shared_ptr<periph::model> {
          *probe = std::make_shared<services_probe>(ctx);
          return *probe;
        });
  }
  return options;
}

struct flash_rig
{
  std::shared_ptr<services_probe> probe{};
  stm32_fixture                   f;
  std::vector<core::log_record>   logged{};
  core::hook_handle               sink{};

  explicit flash_rig(periph::stm32::board const& board = {}, bool strict = false)
      : f{u575(), "flash_ll", board_options(board, strict, &probe)}
  {
    sink = f.target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
    write("FLASH_NSKEYR", 0x4567'0123u);
    write("FLASH_NSKEYR", 0xcdef'89abu);
    REQUIRE(field("FLASH_NSCR", "LOCK") == 0);
  }

  [[nodiscard]] auto at(std::string_view reg, std::string_view peripheral = "FLASH") const
      -> address
  {
    auto const& desc  = f.target->peripheral(peripheral).description();
    auto const  found = std::ranges::find(desc.registers, reg, &popcorn::register_desc::name);
    REQUIRE(found != desc.registers.end());
    return desc.base + found->offset;
  }

  void write(std::string_view reg, std::uint32_t value) const
  {
    REQUIRE(f.target->mem().write(at(reg), 4, value).has_value());
  }

  void write(std::string_view peripheral, std::string_view reg, std::uint32_t value) const
  {
    REQUIRE(f.target->mem().write(at(reg, peripheral), 4, value).has_value());
  }

  void spin() const
  {
    call("spin", {2000});
  }

  [[nodiscard]] auto latency_warnings() const -> std::size_t
  {
    return static_cast<std::size_t>(
        std::ranges::count_if(logged, [](core::log_record const& record) {
          return record.peripheral == "FLASH" and record.reg == "FLASH_ACR";
        }));
  }

  void switch_to_hsi16() const
  {
    write("RCC", "RCC_CR", read_reg("RCC", "RCC_CR") | hsion);
    spin();
    REQUIRE(f.field("RCC", "RCC_CR", "HSIRDY") == 1);
    write("RCC", "RCC_CFGR1", sw_hsi16);
    spin();
    REQUIRE(f.field("RCC", "RCC_CFGR1", "SWS") == sw_hsi16);
  }

  [[nodiscard]] auto read_reg(std::string_view peripheral, std::string_view reg) const
      -> std::uint32_t
  {
    return word(at(reg, peripheral));
  }

  [[nodiscard]] auto word(address from) const -> std::uint32_t
  {
    auto const value = f.target->mem().read(from, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  [[nodiscard]] auto read(std::string_view reg) const -> std::uint32_t
  {
    return word(at(reg));
  }

  [[nodiscard]] auto field(std::string_view reg, std::string_view name) const -> std::uint32_t
  {
    return f.field("FLASH", reg, name);
  }

  [[nodiscard]] auto status(std::string_view name) const -> std::uint32_t
  {
    return field("FLASH_NSSR", name);
  }

  void clear_errors() const
  {
    write("FLASH_NSSR", all_errors);
  }

  void control(std::uint32_t value) const
  {
    write("FLASH_NSCR", value);
  }

  void call(std::string_view entry, std::initializer_list<std::uint32_t> args) const
  {
    auto index = 0;
    for(auto const arg : args)
    {
      f.set(std::format("r{}", index++), arg);
    }
    f.set("pc", sym("flash_ll", entry));
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
  }

  void store_words(address to, std::vector<std::uint32_t> const& values) const
  {
    auto const buffer = sym("flash_ll", "words");
    for(std::size_t i = 0; i < values.size(); ++i)
    {
      REQUIRE(f.target->mem().write(buffer + 4 * i, 4, values[i]).has_value());
    }
    call("copy_words", {to, buffer, static_cast<std::uint32_t>(values.size())});
  }

  void wait_idle() const
  {
    call("wait_idle", {});
  }

  [[nodiscard]] auto words(address from, std::size_t count) const -> std::vector<std::uint32_t>
  {
    std::vector<std::uint32_t> out;
    for(std::size_t i = 0; i < count; ++i)
    {
      out.push_back(word(from + 4 * i));
    }
    return out;
  }

  void erase_page(address page) const
  {
    auto const bank = page >= bank2 ? bker : 0u;
    auto const base = page >= bank2 ? bank2 : address{0x0800'0000};
    control(per | bank | pnb((page - base) / page_bytes) | strt);
    wait_idle();
    control(0);
  }

  void program(address to, std::vector<std::uint32_t> const& values, std::uint32_t extra = 0) const
  {
    control(pg | extra);
    store_words(to, values);
    wait_idle();
  }

  [[nodiscard]] auto pending() const -> bool
  {
    auto const word = f.target->mem().read(0xe000'e200u, 4);
    REQUIRE(word.has_value());
    return (*word >> flash_irq & 1u) != 0;
  }
};

auto erased(std::size_t count) -> std::vector<std::uint32_t>
{
  return std::vector<std::uint32_t>(count, 0xffff'ffffu);
}

auto burst_data(std::uint32_t seed) -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(std::uint32_t i = 0; i < 32; ++i)
  {
    out.push_back(0x1234'0000u + seed + i);
  }
  return out;
}
}

TEST_CASE("a page erase holds BSY for its time, then the page reads erased, EOP only with EOPIE")
{
  flash_rig rig;
  rig.program(last_page, {1, 2, 3, 4});
  rig.control(0);
  REQUIRE(rig.words(last_page, 4) == std::vector<std::uint32_t>{1, 2, 3, 4});
  for(auto const end_irq : {0u, eopie})
  {
    CAPTURE(end_irq);
    if(end_irq != 0)
    {
      rig.program(last_page, {5, 6, 7, 8});
      rig.control(0);
    }
    auto const start = rig.f.target->virtual_clock().time().ps;
    rig.control(per | bker | pnb(63) | strt | end_irq);
    CHECK(rig.status("BSY") == 1);
    CHECK(rig.field("FLASH_NSCR", "STRT") == 1);
    rig.wait_idle();
    auto const took = rig.f.target->virtual_clock().time().ps - start;
    CHECK(took >= 1'500'000'000u);
    CHECK(took < 1'600'000'000u);
    CHECK(rig.words(last_page, 4) == erased(4));
    CHECK(rig.words(last_page + page_bytes - 16, 4) == erased(4));
    CHECK(rig.status("BSY") == 0);
    CHECK(rig.field("FLASH_NSCR", "STRT") == 0);
    CHECK(rig.status("EOP") == (end_irq != 0 ? 1u : 0u));
    CHECK(rig.pending() == (end_irq != 0));
    rig.write("FLASH_NSSR", 1);
    CHECK(rig.status("EOP") == 0);
    rig.control(0);
  }
}

TEST_CASE("BKER selects bank 2 at half the flash, a PNB past the bank erases nothing")
{
  flash_rig rig;
  rig.program(bank2, {1, 2, 3, 4});
  rig.control(0);
  rig.program(0x0800'0000 + 63 * page_bytes, {5, 6, 7, 8});
  rig.control(0);
  rig.erase_page(bank2);
  CHECK(rig.words(bank2, 4) == erased(4));
  CHECK(rig.words(0x0800'0000 + 63 * page_bytes, 4) == std::vector<std::uint32_t>{5, 6, 7, 8});
  rig.logged.clear();
  rig.control(per | pnb(64) | strt);
  CHECK(rig.status("BSY") == 0);
  CHECK(rig.field("FLASH_NSCR", "STRT") == 0);
  CHECK((rig.read("FLASH_NSSR") & all_errors) == 0);
  REQUIRE(rig.logged.size() == 1);
  CHECK(rig.logged[0].kind == core::log_kind::model_warning);
  CHECK(rig.logged[0].message.contains("PNB 64"));
  rig.control(0);
  rig.control(per | pnb(70) | strt);
  CHECK(rig.logged.size() == 1);
}

TEST_CASE("under strict_peripherals a PNB past the bank raises model_error")
{
  flash_rig rig{{}, true};
  CHECK_THROWS_AS(
      static_cast<void>(rig.f.target->mem().write(rig.at("FLASH_NSCR"), 4, per | pnb(64) | strt)),
      popcorn::model_error);
  CHECK(rig.status("BSY") == 0);
}

TEST_CASE("bank and mass erase take their own times and fill their banks")
{
  periph::stm32::board board;
  board.flash.bank_erase = std::chrono::microseconds{200};
  board.flash.mass_erase = std::chrono::microseconds{400};
  flash_rig rig{board};
  rig.program(last_page, {1, 2, 3, 4});
  rig.control(0);
  auto const start = rig.f.target->virtual_clock().time().ps;
  rig.control(mer2 | strt);
  rig.wait_idle();
  auto const took = rig.f.target->virtual_clock().time().ps - start;
  CHECK(took >= 200'000'000u);
  CHECK(took < 300'000'000u);
  CHECK(rig.words(last_page, 4) == erased(4));
  CHECK(rig.words(0x0800'0000, 1) != erased(1));
}

TEST_CASE("a quad-word program lands after its time, through WDW then BSY")
{
  flash_rig rig;
  rig.control(pg);
  rig.store_words(last_page, {0x11, 0x22});
  CHECK(rig.status("WDW") == 1);
  CHECK(rig.status("BSY") == 0);
  CHECK(rig.words(last_page, 2) == erased(2));
  auto const start = rig.f.target->virtual_clock().time().ps;
  rig.store_words(last_page + 8, {0x33, 0x44});
  CHECK(rig.status("WDW") == 0);
  CHECK(rig.status("BSY") == 1);
  CHECK(rig.words(last_page, 4) == erased(4));
  rig.wait_idle();
  auto const took = rig.f.target->virtual_clock().time().ps - start;
  CHECK(took >= 118'000'000u);
  CHECK(took < 200'000'000u);
  CHECK(rig.words(last_page, 4) == std::vector<std::uint32_t>{0x11, 0x22, 0x33, 0x44});
  CHECK((rig.read("FLASH_NSSR") & all_errors) == 0);
}

TEST_CASE("code programmed into flash runs, after an erase and a second program too")
{
  flash_rig rig;
  for(std::uint32_t const value : {1u, 2u})
  {
    CAPTURE(value);
    rig.erase_page(last_page);
    rig.program(last_page, {0xbe00'2000u | value, 0xffff'ffffu, 0xffff'ffffu, 0xffff'ffffu});
    rig.control(0);
    rig.f.set("r0", 0);
    rig.f.set("pc", last_page | 1u);
    REQUIRE(rig.f.run_to_stop().reason == stop_reason::user);
    CHECK(rig.f.target->regs().read(*rig.f.target->regs().find("r0")) == value);
  }
}

TEST_CASE("PROGERR follows the 7.3.9 list")
{
  flash_rig rig;
  rig.program(last_page, {0x0000'ffffu, 0, 0, 0});
  CHECK((rig.read("FLASH_NSSR") & all_errors) == 0);
  SUBCASE("a programmed target refuses non-zero data")
  {
    rig.program(last_page, {1, 2, 3, 4});
    CHECK(rig.status("PROGERR") == 1);
    CHECK(rig.words(last_page, 4) == std::vector<std::uint32_t>{0x0000'ffffu, 0, 0, 0});
  }
  SUBCASE("a partly programmed target takes all-zero data, then refuses it")
  {
    rig.program(last_page, {0, 0, 0, 0});
    CHECK(rig.status("PROGERR") == 0);
    CHECK(rig.words(last_page, 4) == std::vector<std::uint32_t>{0, 0, 0, 0});
    rig.program(last_page, {0, 0, 0, 0});
    CHECK(rig.status("PROGERR") == 1);
  }
}

TEST_CASE("byte and half-word stores set SIZERR, misplaced words PGAERR, and flush the buffer")
{
  flash_rig rig;
  rig.control(pg);
  rig.call("store_half", {last_page, 0x1234});
  CHECK(rig.status("SIZERR") == 1);
  rig.clear_errors();
  rig.call("store_byte", {last_page, 0x12});
  CHECK(rig.status("SIZERR") == 1);
  rig.clear_errors();
  rig.store_words(last_page + 4, {1});
  CHECK(rig.status("PGAERR") == 1);
  CHECK(rig.status("WDW") == 0);
  rig.clear_errors();
  rig.store_words(last_page, {1});
  CHECK(rig.status("WDW") == 1);
  rig.store_words(last_page + 16, {2});
  CHECK(rig.status("PGAERR") == 1);
  CHECK(rig.status("WDW") == 0);
  rig.clear_errors();
  rig.store_words(last_page + 16, {5, 6, 7, 8});
  rig.wait_idle();
  CHECK(rig.status("PGAERR") == 0);
  CHECK(rig.words(last_page + 16, 4) == std::vector<std::uint32_t>{5, 6, 7, 8});
  CHECK(rig.words(last_page, 4) == erased(4));
}

TEST_CASE("a VSTR.64 into flash programs two words of the quad-word, low word first")
{
  flash_rig rig;
  rig.control(pg);
  rig.call("store_double", {last_page, 0x1111'1111u, 0x2222'2222u});
  CHECK(rig.status("WDW") == 1);
  rig.call("store_double", {last_page + 8, 0x3333'3333u, 0x4444'4444u});
  rig.wait_idle();
  CHECK(
      rig.words(last_page, 4)
      == std::vector<std::uint32_t>{0x1111'1111u, 0x2222'2222u, 0x3333'3333u, 0x4444'4444u});
}

TEST_CASE("every PGSERR case of RM0456 7.3.9 sets it and starts nothing")
{
  flash_rig  rig;
  auto const pgserr = [&rig] {
    auto const set = rig.status("PGSERR") == 1;
    rig.control(0);
    rig.clear_errors();
    return set;
  };
  SUBCASE("a store while PG is clear")
  {
    rig.store_words(last_page, {1});
    CHECK(rig.status("WDW") == 0);
    CHECK(pgserr());
  }
  SUBCASE("PG set while PER, MER1 or MER2 is set")
  {
    for(auto const erase : {per, mer1, mer2})
    {
      CAPTURE(erase);
      rig.control(erase);
      rig.control(erase | pg);
      CHECK(pgserr());
      rig.control(pg | erase);
      CHECK(pgserr());
    }
  }
  SUBCASE("PG set while STRT is set aborts the erase")
  {
    rig.program(last_page, {1, 2, 3, 4});
    rig.control(0);
    rig.control(per | bker | pnb(63) | strt);
    REQUIRE(rig.status("BSY") == 1);
    rig.control(0);
    CHECK(rig.field("FLASH_NSCR", "STRT") == 1);
    CHECK(rig.status("BSY") == 1);
    rig.control(pg);
    CHECK(rig.status("BSY") == 0);
    CHECK(rig.field("FLASH_NSCR", "STRT") == 0);
    CHECK(pgserr());
    rig.wait_idle();
    CHECK(rig.words(last_page, 4) == std::vector<std::uint32_t>{1, 2, 3, 4});
  }
  SUBCASE("STRT with none of PER, MER1, MER2")
  {
    rig.control(strt);
    CHECK(rig.field("FLASH_NSCR", "STRT") == 0);
    CHECK(pgserr());
  }
  SUBCASE("STRT and PER while MER1 or MER2 is set")
  {
    rig.control(per | mer1 | strt);
    CHECK(rig.status("BSY") == 0);
    CHECK(pgserr());
    rig.control(per | mer2 | strt);
    CHECK(rig.status("BSY") == 0);
    CHECK(pgserr());
  }
  SUBCASE("STRT or OPTSTRT while WDW is set")
  {
    rig.control(pg);
    rig.store_words(last_page, {1});
    REQUIRE(rig.status("WDW") == 1);
    rig.control(pg | per | bker | pnb(63) | strt);
    CHECK(rig.status("WDW") == 0);
    CHECK(rig.status("BSY") == 0);
    CHECK(pgserr());
    rig.control(pg);
    rig.store_words(last_page, {1});
    rig.control(pg | optstrt);
    CHECK(rig.status("WDW") == 0);
    CHECK(pgserr());
  }
  SUBCASE("STRT and OPTSTRT together")
  {
    rig.control(per | bker | pnb(63) | strt | optstrt);
    CHECK(rig.status("BSY") == 0);
    CHECK(pgserr());
  }
  SUBCASE("STRT or a first store while an error flag is still set")
  {
    rig.control(pg);
    rig.call("store_byte", {last_page, 1});
    REQUIRE(rig.status("SIZERR") == 1);
    rig.control(0);
    rig.control(per | bker | pnb(63) | strt);
    CHECK(rig.status("BSY") == 0);
    CHECK(rig.status("PGSERR") == 1);
    rig.control(pg);
    rig.store_words(last_page, {1});
    CHECK(rig.status("WDW") == 0);
    CHECK(pgserr());
  }
}

TEST_CASE("ERRIE adds OPERR and the FLASH interrupt to an error, EOPIE the end of a program")
{
  flash_rig rig;
  rig.control(pg);
  rig.call("store_byte", {last_page, 1});
  CHECK(rig.status("SIZERR") == 1);
  CHECK(rig.status("OPERR") == 0);
  CHECK_FALSE(rig.pending());
  rig.clear_errors();
  rig.control(pg | errie);
  rig.call("store_byte", {last_page, 1});
  CHECK(rig.status("OPERR") == 1);
  CHECK(rig.pending());
  rig.clear_errors();
  rig.program(last_page, {1, 2, 3, 4}, eopie);
  CHECK(rig.status("EOP") == 1);
  rig.program(last_page + 16, {1, 2, 3, 4});
  CHECK(rig.status("EOP") == 1);
  rig.write("FLASH_NSSR", 1);
  CHECK(rig.status("EOP") == 0);
}

TEST_CASE("clearing EOPIE or ERRIE with its flag still set lowers the FLASH interrupt")
{
  flash_rig  rig;
  auto const clear_pending = [&rig] {
    REQUIRE(rig.f.target->mem().write(0xe000'e280u, 4, 1u << flash_irq).has_value());
  };
  rig.program(last_page, {1, 2, 3, 4}, eopie);
  REQUIRE(rig.status("EOP") == 1);
  clear_pending();
  CHECK(rig.pending());
  rig.control(pg);
  clear_pending();
  CHECK_FALSE(rig.pending());
  rig.control(pg | errie);
  rig.call("store_byte", {last_page + 16, 1});
  REQUIRE(rig.status("OPERR") == 1);
  clear_pending();
  CHECK(rig.pending());
  rig.control(pg);
  clear_pending();
  CHECK_FALSE(rig.pending());
}

TEST_CASE("stores while BSY is set are applied when it clears")
{
  flash_rig rig;
  rig.program(last_page, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});
  CHECK(rig.status("BSY") == 0);
  CHECK(
      rig.words(last_page, 12)
      == std::vector<std::uint32_t>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});
}

TEST_CASE("DMA stores into flash take the same rules and never bus-fault")
{
  flash_rig rig;
  REQUIRE(rig.probe);
  auto& services = rig.probe->services();
  CHECK(services.write(last_page, 4, 1, initiator::dma));
  CHECK(rig.status("PGSERR") == 1);
  rig.clear_errors();
  rig.control(pg);
  for(std::uint32_t i = 0; i < 4; ++i)
  {
    CHECK(services.write(last_page + 4 * i, 4, 0x10 + i, initiator::dma));
  }
  rig.wait_idle();
  CHECK(rig.words(last_page, 4) == std::vector<std::uint32_t>{0x10, 0x11, 0x12, 0x13});
  CHECK(services.write(last_page + 16, 2, 1, initiator::dma));
  CHECK(rig.status("SIZERR") == 1);
}

TEST_CASE("a snapshot taken with two words in the buffer completes the program after restore")
{
  flash_rig rig;
  rig.control(pg);
  rig.store_words(last_page, {1, 2});
  REQUIRE(rig.status("WDW") == 1);
  auto const saved = rig.f.target->snapshot();
  rig.store_words(last_page + 8, {9, 9});
  rig.wait_idle();
  REQUIRE(rig.words(last_page, 4) == std::vector<std::uint32_t>{1, 2, 9, 9});
  rig.f.target->restore(saved);
  CHECK(rig.status("WDW") == 1);
  CHECK(rig.words(last_page, 4) == erased(4));
  rig.store_words(last_page + 8, {3, 4});
  rig.wait_idle();
  CHECK(rig.words(last_page, 4) == std::vector<std::uint32_t>{1, 2, 3, 4});
}

TEST_CASE("a snapshot taken while BSY is set completes the operation after restore")
{
  flash_rig rig;
  rig.control(pg);
  rig.store_words(last_page, {1, 2, 3, 4});
  REQUIRE(rig.status("BSY") == 1);
  rig.store_words(last_page + 16, {5, 6});
  auto const saved = rig.f.target->snapshot();
  rig.wait_idle();
  rig.f.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(rig.status("BSY") == 1);
  CHECK(rig.words(last_page, 8) == erased(8));
  rig.wait_idle();
  CHECK(rig.status("WDW") == 1);
  rig.store_words(last_page + 24, {7, 8});
  rig.wait_idle();
  CHECK(rig.words(last_page, 8) == std::vector<std::uint32_t>{1, 2, 3, 4, 5, 6, 7, 8});
  rig.control(0);
  rig.control(per | bker | pnb(63) | strt);
  auto const erasing = rig.f.target->snapshot();
  rig.f.target->restore(erasing);
  rig.wait_idle();
  CHECK(rig.words(last_page, 4) == erased(4));
}

TEST_CASE("a restore into a fresh machine keeps stores reaching its FLASH model")
{
  flash_rig  rig;
  auto const saved = rig.f.target->snapshot();
  flash_rig  other;
  other.f.target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  other.program(last_page, {1, 2, 3, 4});
  CHECK(other.words(last_page, 4) == std::vector<std::uint32_t>{1, 2, 3, 4});
}

TEST_CASE("HSI16 runs while PG or STRT is set, then stops unless HSION keeps it")
{
  flash_rig rig;
  REQUIRE(rig.f.field("RCC", "RCC_CR", "HSION") == 0);
  rig.program(last_page, {1, 2, 3, 4});
  CHECK(rig.f.field("RCC", "RCC_CR", "HSIRDY") == 1);
  rig.control(0);
  CHECK(rig.f.field("RCC", "RCC_CR", "HSIRDY") == 0);
  rig.control(per | bker | pnb(63) | strt);
  rig.wait_idle();
  CHECK(rig.f.field("RCC", "RCC_CR", "HSIRDY") == 0);
}

TEST_CASE("HSI16 stays on while a held program runs after PG is cleared")
{
  flash_rig rig;
  REQUIRE(rig.f.field("RCC", "RCC_CR", "HSION") == 0);
  rig.control(pg);
  rig.store_words(last_page, {1, 2, 3, 4});
  rig.wait_idle();
  REQUIRE(rig.f.field("RCC", "RCC_CR", "HSIRDY") == 1);
  rig.store_words(last_page + 16, {5, 6, 7, 8});
  REQUIRE(rig.status("BSY") == 1);
  rig.control(0);
  CHECK(rig.f.field("RCC", "RCC_CR", "HSIRDY") == 1);
  rig.wait_idle();
  CHECK(rig.f.field("RCC", "RCC_CR", "HSIRDY") == 0);
}

TEST_CASE("write protection is logged once as not modelled")
{
  flash_rig rig;
  rig.logged.clear();
  rig.program(last_page, {1, 2, 3, 4});
  rig.program(last_page + 16, {1, 2, 3, 4});
  REQUIRE(rig.logged.size() == 1);
  CHECK(rig.logged.front().message.contains("write protection"));
  CHECK(rig.words(last_page + 16, 4) == std::vector<std::uint32_t>{1, 2, 3, 4});
}

TEST_CASE("a BWR burst collects 32 words then programs 128 bytes under one BSY")
{
  flash_rig rig;
  rig.logged.clear();
  auto const data = burst_data(0x100);
  rig.control(pg | bwr | eopie);
  rig.store_words(last_page, std::vector<std::uint32_t>(data.begin(), data.end() - 1));
  CHECK(rig.status("WDW") == 1);
  CHECK(rig.status("BSY") == 0);
  CHECK(rig.words(last_page, 32) == erased(32));
  auto const start = rig.f.target->virtual_clock().time().ps;
  rig.store_words(last_page + 124, {data.back()});
  CHECK(rig.status("WDW") == 0);
  CHECK(rig.status("BSY") == 1);
  rig.control(eopie);
  rig.call("spin", {100});
  CHECK(rig.f.target->virtual_clock().time().ps - start >= 100'000'000u);
  CHECK(rig.status("BSY") == 1);
  CHECK(rig.words(last_page, 32) == erased(32));
  rig.wait_idle();
  auto const took = rig.f.target->virtual_clock().time().ps - start;
  CHECK(took >= 384'000'000u);
  CHECK(took < 450'000'000u);
  CHECK(rig.words(last_page, 32) == data);
  CHECK(rig.words(last_page + 128, 4) == erased(4));
  CHECK(rig.status("EOP") == 1);
  CHECK((rig.read("FLASH_NSSR") & all_errors & ~eop) == 0);
  CHECK(rig.logged.size() == 1);
}

TEST_CASE("firmware bursts with PG and BWR cleared before or after BSY drops")
{
  flash_rig  rig;
  auto const buffer = sym("flash_ll", "words");
  for(std::uint32_t const clear_first : {1u, 0u})
  {
    CAPTURE(clear_first);
    auto const to   = last_page + (clear_first != 0 ? 0u : 128u);
    auto const data = burst_data(clear_first << 8);
    for(std::size_t i = 0; i < data.size(); ++i)
    {
      REQUIRE(rig.f.target->mem().write(buffer + 4 * i, 4, data[i]).has_value());
    }
    rig.call("burst_program", {to, buffer, clear_first});
    CHECK(rig.status("BSY") == 0);
    CHECK(rig.status("WDW") == 0);
    CHECK(rig.field("FLASH_NSCR", "PG") == 0);
    CHECK((rig.read("FLASH_NSSR") & all_errors) == 0);
    CHECK(rig.words(to, 32) == data);
  }
}

TEST_CASE("a quad-word stored while BSY is set programs after PG is cleared")
{
  flash_rig rig;
  rig.control(pg);
  rig.store_words(last_page, {1, 2, 3, 4, 5, 6, 7, 8});
  REQUIRE(rig.status("BSY") == 1);
  rig.control(0);
  rig.wait_idle();
  CHECK((rig.read("FLASH_NSSR") & all_errors) == 0);
  CHECK(rig.words(last_page, 8) == std::vector<std::uint32_t>{1, 2, 3, 4, 5, 6, 7, 8});
}

TEST_CASE("a burst stored while BSY is set keeps BWR after PG and BWR are cleared")
{
  flash_rig  rig;
  auto const data = burst_data(0x40);
  rig.control(pg);
  rig.store_words(last_page, {1, 2, 3, 4});
  REQUIRE(rig.status("BSY") == 1);
  rig.control(pg | bwr);
  rig.store_words(last_page + 128, data);
  rig.control(0);
  rig.wait_idle();
  CHECK((rig.read("FLASH_NSSR") & all_errors) == 0);
  CHECK(rig.words(last_page, 4) == std::vector<std::uint32_t>{1, 2, 3, 4});
  CHECK(rig.words(last_page + 128, 32) == data);
}

TEST_CASE("a DMA store held while BSY is set takes the PG of the time BSY clears")
{
  flash_rig  rig;
  auto const buffer = sym("flash_ll", "words");
  auto const bit    = [&](std::string_view reg, std::string_view name) {
    auto const& unit = rig.f.target->peripheral("GPDMA1").description();
    auto const  regd = std::ranges::find(unit.registers, reg, &popcorn::register_desc::name);
    REQUIRE(regd != unit.registers.end());
    auto const bits = std::ranges::find(regd->fields, name, &popcorn::field_desc::name);
    REQUIRE(bits != regd->fields.end());
    return std::uint32_t{1} << bits->lsb;
  };
  for(std::uint32_t i = 0; i < 4; ++i)
  {
    REQUIRE(rig.f.target->mem().write(buffer + 64 + 4 * i, 4, 0x5000 + i).has_value());
  }
  rig.control(pg);
  rig.store_words(last_page, {1, 2, 3, 4});
  REQUIRE(rig.status("BSY") == 1);
  auto const start = rig.f.target->virtual_clock().time().ps;
  rig.write(
      "GPDMA1", "GPDMA_C0TR1",
      2u * bit("GPDMA_C0TR1", "SDW_LOG2")
          | 2u * bit("GPDMA_C0TR1", "DDW_LOG2")
          | bit("GPDMA_C0TR1", "SINC")
          | bit("GPDMA_C0TR1", "DINC"));
  rig.write("GPDMA1", "GPDMA_C0TR2", bit("GPDMA_C0TR2", "SWREQ"));
  rig.write("GPDMA1", "GPDMA_C0BR1", 16);
  rig.write("GPDMA1", "GPDMA_C0SAR", static_cast<std::uint32_t>(buffer + 64));
  rig.write("GPDMA1", "GPDMA_C0DAR", static_cast<std::uint32_t>(last_page + 16));
  rig.write("GPDMA1", "GPDMA_C0CR", bit("GPDMA_C0CR", "EN"));
  rig.call("spin", {20});
  REQUIRE(rig.status("BSY") == 1);
  REQUIRE(rig.f.target->virtual_clock().time().ps - start < 100'000'000u);
  rig.control(0);
  rig.wait_idle();
  CHECK(rig.status("PGSERR") == 1);
  CHECK(rig.words(last_page, 4) == std::vector<std::uint32_t>{1, 2, 3, 4});
  CHECK(rig.words(last_page + 16, 4) == erased(4));
}

TEST_CASE("burst errors follow RM0456 7.3.9 and flush the buffer")
{
  flash_rig  rig;
  auto const data  = burst_data(0x80);
  auto const flags = [&rig] {
    auto const value = rig.read("FLASH_NSSR") & all_errors;
    rig.control(0);
    rig.clear_errors();
    return value;
  };
  constexpr std::uint32_t pgaerr  = 1u << 5;
  constexpr std::uint32_t pgserr  = 1u << 7;
  constexpr std::uint32_t sizerr  = 1u << 6;
  constexpr std::uint32_t progerr = 1u << 3;
  SUBCASE("a first word off a 128-byte address")
  {
    rig.control(pg | bwr);
    rig.store_words(last_page + 16, {1});
    CHECK(rig.status("WDW") == 0);
    CHECK(flags() == pgaerr);
  }
  SUBCASE("a word that skips one")
  {
    rig.control(pg | bwr);
    rig.store_words(last_page, {1, 2});
    rig.store_words(last_page + 12, {4});
    CHECK(rig.status("WDW") == 0);
    CHECK(flags() == pgaerr);
  }
  SUBCASE("a word stored twice")
  {
    rig.control(pg | bwr);
    rig.store_words(last_page, {1, 2});
    rig.store_words(last_page + 4, {2});
    CHECK(rig.status("WDW") == 0);
    CHECK(flags() == pgaerr);
  }
  SUBCASE("a word past PG cleared")
  {
    rig.control(pg | bwr);
    rig.store_words(last_page, {1, 2});
    rig.control(bwr);
    rig.store_words(last_page + 8, {3});
    CHECK(rig.status("WDW") == 0);
    CHECK(flags() == pgserr);
  }
  SUBCASE("a half-word in the burst")
  {
    rig.control(pg | bwr);
    rig.store_words(last_page, {1, 2});
    rig.call("store_half", {last_page + 8, 3});
    CHECK(rig.status("WDW") == 0);
    CHECK(flags() == sizerr);
  }
  SUBCASE("one programmed quad-word in the row")
  {
    rig.program(last_page + 112, {1, 2, 3, 4});
    rig.control(pg | bwr);
    rig.store_words(last_page, data);
    rig.wait_idle();
    CHECK(flags() == progerr);
    CHECK(rig.words(last_page, 28) == erased(28));
  }
  SUBCASE("BWR cleared after the first word keeps the burst")
  {
    rig.control(pg | bwr);
    rig.store_words(last_page, {data.front()});
    rig.control(pg);
    rig.store_words(last_page + 4, std::vector<std::uint32_t>(data.begin() + 1, data.end() - 1));
    CHECK(rig.status("BSY") == 0);
    CHECK(rig.status("WDW") == 1);
    rig.store_words(last_page + 124, {data.back()});
    rig.wait_idle();
    CHECK(flags() == 0);
    CHECK(rig.words(last_page, 32) == data);
  }
  rig.control(pg | bwr);
  rig.store_words(last_page + 128, data);
  rig.wait_idle();
  CHECK(flags() == 0);
  CHECK(rig.words(last_page + 128, 32) == data);
}

TEST_CASE("a snapshot taken mid burst or while it is BSY completes it after restore")
{
  flash_rig  rig;
  auto const data = burst_data(0x20);
  auto const half = std::vector<std::uint32_t>(data.begin(), data.begin() + 16);
  auto const rest = std::vector<std::uint32_t>(data.begin() + 16, data.end());
  rig.control(pg | bwr);
  rig.store_words(last_page, half);
  REQUIRE(rig.status("WDW") == 1);
  auto const collecting = rig.f.target->snapshot();
  rig.store_words(last_page + 64, std::vector<std::uint32_t>(16, 0));
  REQUIRE(rig.status("BSY") == 1);
  auto const busy =
      core::snapshot{core::decode_snapshot(core::encode_snapshot(rig.f.target->snapshot().data()))};
  rig.wait_idle();
  rig.f.target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(collecting.data()))});
  CHECK(rig.status("WDW") == 1);
  CHECK(rig.words(last_page, 32) == erased(32));
  rig.store_words(last_page + 64, rest);
  rig.wait_idle();
  CHECK((rig.read("FLASH_NSSR") & all_errors) == 0);
  CHECK(rig.words(last_page, 32) == data);
  rig.f.target->restore(busy);
  CHECK(rig.status("BSY") == 1);
  CHECK(rig.words(last_page, 32) == erased(32));
  rig.wait_idle();
  CHECK(rig.words(last_page, 16) == half);
  CHECK(rig.words(last_page + 64, 16) == std::vector<std::uint32_t>(16, 0));
}

TEST_CASE("raising HCLK past LATENCY warns once, the RM0456 order does not")
{
  flash_rig rig;
  rig.logged.clear();
  rig.write("FLASH_ACR", 1);
  rig.switch_to_hsi16();
  CHECK(rig.latency_warnings() == 0);

  flash_rig late;
  late.write("FLASH_ACR", 0);
  late.logged.clear();
  late.switch_to_hsi16();
  REQUIRE(late.latency_warnings() == 1);
  CHECK(late.logged.back().message.contains("after an HCLK change"));
  CHECK(late.logged.back().message.contains("16000000"));
  CHECK(late.logged.back().message.contains("range 4"));
}

TEST_CASE("a FLASH_ACR write below the table warns once per HCLK, range and LATENCY")
{
  flash_rig rig;
  rig.write("FLASH_ACR", 1);
  rig.switch_to_hsi16();
  rig.logged.clear();
  rig.write("FLASH_ACR", 0);
  rig.write("FLASH_ACR", 0);
  REQUIRE(rig.latency_warnings() == 1);
  CHECK(rig.logged.back().message.contains("on a FLASH_ACR write"));
  rig.write("FLASH_ACR", 1);
  CHECK(rig.latency_warnings() == 1);
  auto const saved = rig.f.target->snapshot();
  rig.f.target->restore(saved);
  rig.write("FLASH_ACR", 0);
  CHECK(rig.latency_warnings() == 2);
}

TEST_CASE("with LPM set, Table 55 asks one wait state for 16 MHz in range 1")
{
  flash_rig rig;
  rig.write("PWR", "PWR_VOSR", vos_range1);
  rig.spin();
  rig.write("FLASH_ACR", 0);
  rig.switch_to_hsi16();
  rig.logged.clear();
  rig.write("FLASH_ACR", 1 | lpm);
  CHECK(rig.latency_warnings() == 0);
  rig.write("FLASH_ACR", lpm);
  CHECK(rig.latency_warnings() == 1);
}

TEST_CASE("lowering the voltage range under a fast HCLK warns when the range applies")
{
  flash_rig rig;
  rig.write("PWR", "PWR_VOSR", vos_range1);
  rig.spin();
  REQUIRE(rig.f.field("PWR", "PWR_SVMSR", "ACTVOS") == 3);
  rig.write("FLASH_ACR", 0);
  rig.switch_to_hsi16();
  rig.logged.clear();
  rig.write("PWR", "PWR_VOSR", 0);
  CHECK(rig.latency_warnings() == 0);
  rig.spin();
  CHECK(rig.f.field("PWR", "PWR_SVMSR", "ACTVOS") == 0);
  REQUIRE(rig.latency_warnings() == 1);
  CHECK(rig.logged.back().message.contains("after a voltage range change"));
  CHECK(rig.logged.back().message.contains("range 4"));
}

TEST_CASE("under strict_peripherals each latency violation raises model_error")
{
  flash_rig rig{{}, true};
  rig.write("FLASH_ACR", 1);
  rig.switch_to_hsi16();
  rig.logged.clear();
  CHECK_THROWS_AS(
      static_cast<void>(rig.f.target->mem().write(rig.at("FLASH_ACR"), 4, 0)),
      popcorn::model_error);
  CHECK_THROWS_AS(
      static_cast<void>(rig.f.target->mem().write(rig.at("FLASH_ACR"), 4, 0)),
      popcorn::model_error);
  CHECK(rig.latency_warnings() == 2);

  flash_rig late{{}, true};
  late.write("FLASH_ACR", 0);
  late.write("RCC", "RCC_CR", late.read_reg("RCC", "RCC_CR") | hsion);
  late.spin();
  late.write("RCC", "RCC_CFGR1", sw_hsi16);
  late.f.set("r0", 2000);
  late.f.set("pc", sym("flash_ll", "spin"));
  CHECK_THROWS_AS(
      static_cast<void>(late.f.target->run_slice({.max_instructions = icount{100'000}})),
      popcorn::model_error);
}
