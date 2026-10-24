#include "../../../native/src/core/machine_impl.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <doctest/doctest.h>
#include <reflex/serde/bson.hpp>

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
#include <optional>
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

constexpr address       bank2       = 0x0808'0000;
constexpr address       last_page   = 0x080f'e000;
constexpr std::uint32_t page_bytes  = 0x2000;
constexpr std::uint32_t flash_irq   = 6;
constexpr std::uint32_t pg          = 1u << 0;
constexpr std::uint32_t per         = 1u << 1;
constexpr std::uint32_t mer1        = 1u << 2;
constexpr std::uint32_t bker        = 1u << 11;
constexpr std::uint32_t bwr         = 1u << 14;
constexpr std::uint32_t mer2        = 1u << 15;
constexpr std::uint32_t strt        = 1u << 16;
constexpr std::uint32_t optstrt     = 1u << 17;
constexpr std::uint32_t eopie       = 1u << 24;
constexpr std::uint32_t errie       = 1u << 25;
constexpr std::uint32_t all_errors  = 0x20fb;
constexpr std::uint32_t eop         = 1u << 0;
constexpr std::uint32_t hsion       = 1u << 8;
constexpr std::uint32_t sw_hsi16    = 1;
constexpr std::uint32_t vos_range1  = 3u << 16;
constexpr std::uint32_t lpm         = 1u << 11;
constexpr std::uint32_t eccd        = 1u << 31;
constexpr std::uint32_t eccc        = 1u << 30;
constexpr std::uint32_t eccie       = 1u << 24;
constexpr std::uint32_t bk_ecc      = 1u << 21;
constexpr std::uint32_t exc_nmi     = 2;
constexpr address       torn_line   = last_page + 0x30;
constexpr address       secure      = 0x0400'0000;
constexpr std::uint32_t option_key1 = 0x0819'2a3b;
constexpr std::uint32_t option_key2 = 0x4c5d'6e7f;
constexpr std::uint32_t lock_bit    = 1u << 31;
constexpr std::uint32_t optlock_bit = 1u << 30;
constexpr std::uint32_t obl_launch  = 1u << 27;

auto pnb(std::uint32_t page) -> std::uint32_t
{
  return page << 3;
}

auto ps(std::chrono::nanoseconds time) -> std::uint64_t
{
  return static_cast<std::uint64_t>(time.count()) * 1000u;
}

struct held_store
{
  std::uint32_t addr{};
  std::uint8_t  size{};
  std::uint32_t value{};
  std::uint32_t control{};
  bool          dma{};

  friend auto operator==(held_store const&, held_store const&) -> bool = default;
};

struct old_flash_state
{
  bool                          first_key{};
  bool                          locked_up{};
  std::array<std::uint32_t, 32> buffer{};
  std::uint32_t                 filled{};
  bool                          burst{};
  std::uint32_t                 buffer_addr{};
  std::uint8_t                  running{};
  std::uint32_t                 target{};
  std::uint32_t                 length{};
  std::uint32_t                 generation{};
  std::vector<held_store>       held{};
};

struct new_flash_state
{
  bool                          first_key{};
  bool                          locked_up{};
  bool                          option_first_key{};
  bool                          option_locked_up{};
  std::array<std::uint32_t, 32> buffer{};
  std::uint32_t                 filled{};
  bool                          burst{};
  std::uint32_t                 buffer_addr{};
  std::uint8_t                  running{};
  std::uint32_t                 target{};
  std::uint32_t                 length{};
  std::uint32_t                 generation{};
  std::vector<held_store>       held{};
  std::uint64_t                 due_ps{};
  std::vector<std::uint32_t>    torn{};

  friend auto operator==(new_flash_state const&, new_flash_state const&) -> bool = default;
};

struct secure_flash_state
{
  bool                      first_key{};
  bool                      locked_up{};
  bool                      buffer_secure{};
  bool                      running_secure{};
  std::vector<std::uint8_t> held_attrs{};
};

struct trustzone_flash_state
{
  new_flash_state    flash{};
  secure_flash_state secure{};
};

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

  [[nodiscard]] auto trustzone() const -> bool
  {
    return ctx_.trustzone;
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
  options.trustzone          = board.flash.option_bytes and board.flash.option_bytes->tzen;
  if(options.trustzone)
  {
    auto const idau = periph::stm32::idau_map(u575());
    REQUIRE(idau.has_value());
    options.idau.assign(idau->begin(), idau->end());
  }
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

  bool secure_set{};

  explicit flash_rig(periph::stm32::board const& board = {}, bool strict = false)
      : f{u575(), "flash_ll", board_options(board, strict, &probe)},
        secure_set{board.flash.option_bytes and board.flash.option_bytes->tzen}
  {
    sink = f.target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
    unlock();
  }

  [[nodiscard]] auto set(std::string_view reg) const -> std::string
  {
    return std::format("FLASH_{}{}", secure_set ? "SEC" : "NS", reg);
  }

  void unlock() const
  {
    write(set("KEYR"), 0x4567'0123u);
    write(set("KEYR"), 0xcdef'89abu);
    REQUIRE(field(set("CR"), "LOCK") == 0);
  }

  [[nodiscard]] auto try_write(std::string_view reg, std::uint32_t value) const -> bool
  {
    auto&      regs = f.target->peripheral("FLASH");
    auto const word = popcorn::find_register(regs.description(), reg);
    REQUIRE(word != nullptr);
    return f.target->model("FLASH")
        ->on_write(
            regs, word->offset, 4, regs.peek(word->offset), value, {.pc = 0, .now = icount{0}})
        .has_value();
  }

  void unlock_options() const
  {
    write("FLASH_OPTKEYR", option_key1);
    write("FLASH_OPTKEYR", option_key2);
  }

  [[nodiscard]] auto at(std::string_view reg, std::string_view peripheral = "FLASH") const
      -> address
  {
    auto const& desc  = f.target->peripheral(peripheral).description();
    auto const  found = popcorn::find_register(desc, reg);
    REQUIRE(found != nullptr);
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
    return field(set("SR"), name);
  }

  void clear_errors() const
  {
    write(set("SR"), all_errors);
  }

  void control(std::uint32_t value) const
  {
    write(set("CR"), value);
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

  [[nodiscard]] auto state() const -> new_flash_state
  {
    std::vector<std::byte> saved;
    f.target->model("FLASH")->save(saved);
    std::vector<std::byte> redumped;
    if(secure_set)
    {
      auto const state = reflex::serde::bson::deserializer{
          std::span<std::byte const>{
              saved}}.load<trustzone_flash_state>();
      reflex::serde::bson::serializer{redumped}.dump(state);
      REQUIRE(redumped == saved);
      return state.flash;
    }
    auto const state = reflex::serde::bson::deserializer{
        std::span<std::byte const>{
            saved}}.load<new_flash_state>();
    reflex::serde::bson::serializer{redumped}.dump(state);
    REQUIRE(redumped == saved);
    return state;
  }

  [[nodiscard]] auto torn() const -> std::vector<std::uint32_t>
  {
    return state().torn;
  }

  void cycle() const
  {
    f.target->power_cycle();
    unlock();
  }

  void run_sliced(icount slice, std::optional<popcorn::vtime> until, stop_reason reason) const
  {
    for(;;)
    {
      auto const result = f.target->run_slice(
          {.max_instructions = slice,
           .wall_budget      = std::chrono::seconds{60},
           .until_time       = until});
      if(result.stopped)
      {
        REQUIRE(result.stopped->reason == reason);
        return;
      }
    }
  }

  void store_words_sliced(address to, std::vector<std::uint32_t> const& values, icount slice) const
  {
    auto const buffer = sym("flash_ll", "words");
    for(std::size_t i = 0; i < values.size(); ++i)
    {
      REQUIRE(f.target->mem().write(buffer + 4 * i, 4, values[i]).has_value());
    }
    f.set("r0", static_cast<std::uint32_t>(to));
    f.set("r1", static_cast<std::uint32_t>(buffer));
    f.set("r2", static_cast<std::uint32_t>(values.size()));
    f.set("pc", sym("flash_ll", "copy_words"));
    run_sliced(slice, std::nullopt, stop_reason::user);
  }

  void run_into(
      std::chrono::nanoseconds total,
      std::chrono::nanoseconds elapsed,
      icount                   slice = icount{100'000'000}) const
  {
    constexpr std::uint64_t launch_slack_ps = 10'000'000;
    REQUIRE(status("BSY") == 1);
    auto const& clock  = f.target->virtual_clock();
    auto const  launch = clock.since_power_on().ps;
    auto const  base   = launch - clock.time().ps;
    auto const  due    = state().due_ps;
    REQUIRE(due <= launch + ps(total));
    REQUIRE(due + launch_slack_ps >= launch + ps(total));
    f.set("pc", sym("flash_ll", "wait_idle"));
    run_sliced(slice, popcorn::vtime{launch + ps(elapsed) - base}, stop_reason::time);
    REQUIRE(status("BSY") == 1);
  }

  void cut_after(std::chrono::nanoseconds total, std::chrono::nanoseconds elapsed) const
  {
    run_into(total, elapsed);
    cycle();
  }

  void tear(address line, icount slice = icount{100'000'000}) const
  {
    constexpr periph::stm32::flash_options timing{};
    control(pg);
    store_words_sliced(line, {1, 2, 3, 4}, slice);
    run_into(timing.quad_word_program, timing.quad_word_program / 2, slice);
    cycle();
    control(0);
    REQUIRE(std::ranges::contains(torn(), static_cast<std::uint32_t>(line)));
  }

  [[nodiscard]] auto eccr() const -> std::uint32_t
  {
    return read("FLASH_ECCR");
  }

  [[nodiscard]] auto nmi_level() const -> bool
  {
    auto const  saved  = f.target->snapshot();
    auto const& images = saved.data().peripherals;
    auto const  found =
        std::ranges::find(images, std::string_view{"FLASH"}, &core::peripheral_image::peripheral);
    REQUIRE(found != images.end());
    return found->nmi_level != 0;
  }

  [[nodiscard]] auto core_reg(std::string_view name) const -> std::uint64_t
  {
    return f.target->regs().read(*f.target->regs().find(name));
  }

  void handle_nmi()
  {
    nmi_hook = f.target->add_intercept(
        sym("flash_ll", "NMI_Handler"), [this](core::intercept_call const&) {
          nmi_eccr.push_back(eccr());
          if(not clear_nmi)
          {
            return core::intercept_result::stop(stop_reason::user);
          }
          write("FLASH_ECCR", eccd);
          return core::intercept_result::returns(std::nullopt);
        });
  }

  void load(address from, icount slice = icount{100'000'000}) const
  {
    REQUIRE(f.target->mem().write(sym("flash_ll", "words"), 4, 0xdead'beefu).has_value());
    f.set("r0", sym("flash_ll", "words"));
    f.set("r1", from);
    f.set("r2", 1);
    f.set("pc", sym("flash_ll", "copy_words"));
    for(icount ran{}; ran.value < 10'000; ran.value += slice.value)
    {
      auto const result = f.target->run_slice(
          {.max_instructions = icount{std::min(slice.value, 10'000 - ran.value)},
           .wall_budget      = std::chrono::seconds{60}});
      if(result.stopped)
      {
        INFO(result.stopped->where.pc);
        REQUIRE(result.stopped->reason == stop_reason::user);
        return;
      }
    }
    FAIL("no stop within 10000 instructions");
  }

  [[nodiscard]] auto dma_load(address from) const -> bool
  {
    return probe->services().read(from, 4, initiator::dma).has_value();
  }

  std::vector<std::uint32_t> nmi_eccr{};
  core::intercept_handle     nmi_hook{};
  bool                       clear_nmi{true};
};

auto reported(address line) -> std::uint32_t
{
  auto const offset = static_cast<std::uint32_t>(line - 0x0800'0000);
  return line >= bank2 ? eccd | bk_ecc | (offset - 0x8'0000) : eccd | offset;
}

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

TEST_CASE("a sub-word FLASH_NSSR write clears flags only in the lanes it writes")
{
  flash_rig rig;
  rig.store_words(last_page, {1});
  REQUIRE(rig.status("PGSERR") == 1);
  auto const status = rig.at("FLASH_NSSR");
  REQUIRE(rig.f.target->mem().write(status + 1, 1, 0xff).has_value());
  CHECK(rig.status("PGSERR") == 1);
  REQUIRE(rig.f.target->mem().write(status + 2, 2, 0xffff).has_value());
  CHECK(rig.status("PGSERR") == 1);
  REQUIRE(rig.f.target->mem().write(status, 1, 0x7f).has_value());
  CHECK(rig.status("PGSERR") == 1);
  REQUIRE(rig.f.target->mem().write(status, 1, 0x80).has_value());
  CHECK(rig.status("PGSERR") == 0);
  constexpr std::uint32_t pgserr   = 1u << 7;
  constexpr std::uint32_t optwerr  = 1u << 13;
  auto&                   regs     = rig.f.target->peripheral("FLASH");
  auto const              offset   = static_cast<std::uint32_t>(status - regs.description().base);
  auto const              set_both = [&] {
    regs.poke(offset, regs.peek(offset) | pgserr | optwerr);
    REQUIRE(rig.status("PGSERR") == 1);
    REQUIRE(rig.status("OPTWERR") == 1);
  };
  set_both();
  REQUIRE(rig.f.target->mem().write(status, 2, 0x2000).has_value());
  CHECK(rig.status("OPTWERR") == 0);
  CHECK(rig.status("PGSERR") == 1);
  set_both();
  REQUIRE(rig.f.target->mem().write(status + 1, 1, 0x20).has_value());
  CHECK(rig.status("OPTWERR") == 0);
  CHECK(rig.status("PGSERR") == 1);
  set_both();
  REQUIRE(rig.f.target->mem().write(status, 4, all_errors).has_value());
  CHECK(rig.status("OPTWERR") == 0);
  CHECK(rig.status("PGSERR") == 0);
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
    rig.unlock_options();
    rig.control(pg);
    rig.store_words(last_page, {1});
    rig.control(pg | optstrt);
    CHECK(rig.status("WDW") == 0);
    CHECK(pgserr());
  }
  SUBCASE("STRT and OPTSTRT together")
  {
    rig.unlock_options();
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
    auto const  regd = popcorn::find_register(unit, reg);
    REQUIRE(regd != nullptr);
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

TEST_CASE("a power cut during a BWR burst programs the quad-words its elapsed time reached")
{
  constexpr periph::stm32::flash_options timing{};
  flash_rig                              rig;
  auto const                             data = burst_data(0x300);
  rig.control(pg | bwr);
  SUBCASE("cut at 3.5 quad-word times tears quad-word 3")
  {
    rig.store_words(last_page, data);
    rig.cut_after(timing.burst_program, std::chrono::microseconds{168});
    CHECK(rig.status("BSY") == 0);
    CHECK(rig.words(last_page, 14) == std::vector<std::uint32_t>(data.begin(), data.begin() + 14));
    CHECK(rig.words(last_page + 56, 26) == erased(26));
    CHECK(rig.torn() == std::vector<std::uint32_t>{last_page + 48});
  }
  SUBCASE("cut right after the 32nd word tears quad-word 0")
  {
    rig.store_words(last_page, data);
    REQUIRE(rig.status("BSY") == 1);
    rig.cycle();
    CHECK(rig.words(last_page, 2) == std::vector<std::uint32_t>(data.begin(), data.begin() + 2));
    CHECK(rig.words(last_page + 8, 30) == erased(30));
    CHECK(rig.torn() == std::vector<std::uint32_t>{last_page});
  }
  SUBCASE("cut after BSY cleared keeps the whole burst")
  {
    rig.store_words(last_page, data);
    rig.wait_idle();
    rig.cycle();
    CHECK(rig.words(last_page, 32) == data);
    CHECK(rig.torn().empty());
  }
  SUBCASE("a burst into a programmed quad-word changes nothing")
  {
    rig.program(last_page + 112, {1, 2, 3, 4});
    rig.control(pg | bwr);
    rig.store_words(last_page, data);
    rig.cut_after(timing.burst_program, std::chrono::microseconds{192});
    CHECK(rig.words(last_page, 28) == erased(28));
    CHECK(rig.words(last_page + 112, 4) == std::vector<std::uint32_t>{1, 2, 3, 4});
    CHECK(rig.torn().empty());
    CHECK((rig.read("FLASH_NSSR") & all_errors) == 0);
  }
  SUBCASE("a partly filled write buffer is dropped")
  {
    rig.store_words(last_page, std::vector<std::uint32_t>(data.begin(), data.begin() + 16));
    REQUIRE(rig.status("WDW") == 1);
    rig.cycle();
    CHECK(rig.status("WDW") == 0);
    CHECK(rig.words(last_page, 32) == erased(32));
    CHECK(rig.torn().empty());
  }
}

TEST_CASE("a power cut at half a quad-word program tears it")
{
  constexpr periph::stm32::flash_options timing{};
  for(bool const after_reset : {false, true})
  {
    CAPTURE(after_reset);
    flash_rig rig;
    if(after_reset)
    {
      rig.spin();
      rig.f.target->reset(popcorn::reset_kind::software);
      rig.write("FLASH_NSKEYR", 0x4567'0123u);
      rig.write("FLASH_NSKEYR", 0xcdef'89abu);
      REQUIRE(
          rig.f.target->virtual_clock().since_power_on() > rig.f.target->virtual_clock().time());
    }
    rig.control(pg);
    rig.store_words(last_page, {1, 2, 3, 4});
    rig.cut_after(timing.quad_word_program, timing.quad_word_program / 2);
    CHECK(rig.words(last_page, 4) == std::vector<std::uint32_t>{1, 2, 0xffff'ffffu, 0xffff'ffffu});
    CHECK(rig.torn() == std::vector<std::uint32_t>{last_page});
  }
}

TEST_CASE("a power cut at half a page erase erases its first half and tears the boundary")
{
  constexpr periph::stm32::flash_options timing{};
  flash_rig                              rig;
  auto const line = [](std::uint32_t index) { return last_page + 16 * index; };
  SUBCASE("a programmed boundary is torn")
  {
    for(std::uint32_t const index : {0u, 255u, 256u, 257u, 511u})
    {
      rig.program(line(index), {index, 2, 3, 4});
    }
    rig.control(0);
    rig.control(per | bker | pnb(63) | strt);
    rig.cut_after(timing.page_erase, timing.page_erase / 2);
    CHECK(rig.words(line(0), 4) == erased(4));
    CHECK(rig.words(line(255), 4) == erased(4));
    for(std::uint32_t const index : {256u, 257u, 511u})
    {
      CAPTURE(index);
      CHECK(rig.words(line(index), 4) == std::vector<std::uint32_t>{index, 2, 3, 4});
    }
    CHECK(rig.torn() == std::vector<std::uint32_t>{line(256)});
    CHECK(rig.field("FLASH_NSCR", "STRT") == 0);
  }
  SUBCASE("a blank boundary is never torn")
  {
    rig.program(line(0), {1, 2, 3, 4});
    rig.program(line(257), {5, 6, 7, 8});
    rig.control(0);
    rig.control(per | bker | pnb(63) | strt);
    rig.cut_after(timing.page_erase, timing.page_erase / 2);
    CHECK(rig.words(line(0), 4) == erased(4));
    CHECK(rig.words(line(257), 4) == std::vector<std::uint32_t>{5, 6, 7, 8});
    CHECK(rig.torn().empty());
  }
}

TEST_CASE("a power cut at a quarter of a bank 2 erase erases its first quarter")
{
  constexpr periph::stm32::flash_options timing{};
  constexpr address                      boundary = bank2 + 0x2'0000;
  flash_rig                              rig;
  auto const                             code = rig.words(0x0800'0000, 4);
  rig.program(bank2, {1, 2, 3, 4});
  rig.program(boundary - 16, {5, 6, 7, 8});
  rig.program(boundary, {9, 10, 11, 12});
  rig.program(boundary + 16, {13, 14, 15, 16});
  rig.control(0);
  rig.control(mer2 | strt);
  rig.cut_after(timing.bank_erase, timing.bank_erase / 4);
  CHECK(rig.words(bank2, 4) == erased(4));
  CHECK(rig.words(boundary - 16, 4) == erased(4));
  CHECK(rig.words(boundary, 4) == std::vector<std::uint32_t>{9, 10, 11, 12});
  CHECK(rig.words(boundary + 16, 4) == std::vector<std::uint32_t>{13, 14, 15, 16});
  CHECK(rig.words(0x0800'0000, 4) == code);
  CHECK(rig.torn() == std::vector<std::uint32_t>{boundary});
}

TEST_CASE("torn quad-words survive every reset and a restore into a fresh machine")
{
  constexpr periph::stm32::flash_options timing{};
  flash_rig                              rig;
  auto const                             before = rig.f.target->snapshot();
  rig.control(pg);
  rig.store_words(last_page, {1, 2, 3, 4});
  rig.cut_after(timing.quad_word_program, timing.quad_word_program / 2);
  auto const torn = std::vector<std::uint32_t>{last_page};
  REQUIRE(rig.torn() == torn);
  rig.f.target->reset(popcorn::reset_kind::power_on);
  CHECK(rig.torn() == torn);
  rig.f.target->reset(popcorn::reset_kind::software);
  CHECK(rig.torn() == torn);
  auto const saved = rig.f.target->snapshot();
  flash_rig  other;
  REQUIRE(other.torn().empty());
  other.f.target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.torn() == torn);
  rig.f.target->restore(before);
  CHECK(rig.torn().empty());
  CHECK(rig.words(last_page, 4) == erased(4));
}

TEST_CASE("a program into a torn quad-word sets PROGERR until an erase of its page")
{
  constexpr periph::stm32::flash_options timing{};
  flash_rig                              rig;
  rig.control(pg);
  rig.store_words(last_page, {1, 2, 3, 4});
  rig.cut_after(timing.quad_word_program, timing.quad_word_program / 2);
  auto const torn = std::vector<std::uint32_t>{1, 2, 0xffff'ffffu, 0xffff'ffffu};
  rig.program(last_page, {0, 0, 0, 0});
  CHECK(rig.status("PROGERR") == 1);
  CHECK(rig.words(last_page, 4) == torn);
  rig.clear_errors();
  rig.control(pg | bwr);
  rig.store_words(last_page, std::vector<std::uint32_t>(32, 0));
  rig.wait_idle();
  CHECK(rig.status("PROGERR") == 1);
  CHECK(rig.words(last_page, 4) == torn);
  rig.clear_errors();
  rig.control(0);
  rig.erase_page(last_page - page_bytes);
  CHECK(rig.torn() == std::vector<std::uint32_t>{last_page});
  rig.erase_page(last_page);
  CHECK(rig.torn().empty());
  rig.program(last_page, {0, 0, 0, 0});
  CHECK((rig.read("FLASH_NSSR") & all_errors) == 0);
  CHECK(rig.words(last_page, 4) == std::vector<std::uint32_t>{0, 0, 0, 0});
}

TEST_CASE("a cut reached in one slice or in many small slices leaves the same torn state")
{
  constexpr periph::stm32::flash_options timing{};
  auto const                             data = burst_data(0x500);
  auto const                             cut  = [&](icount slice) {
    auto rig = std::make_unique<flash_rig>();
    rig->control(pg | bwr);
    rig->store_words_sliced(last_page, data, slice);
    rig->run_into(timing.burst_program, std::chrono::microseconds{250}, slice);
    rig->cycle();
    return rig;
  };
  auto const parked = cut(icount{100'000'000});
  CHECK(parked->torn() == std::vector<std::uint32_t>{last_page + 80});
  for(auto const slice : {icount{1}, icount{7}})
  {
    CAPTURE(slice.value);
    auto const running = cut(slice);
    CHECK(running->words(last_page, 32) == parked->words(last_page, 32));
    CHECK(running->state() == parked->state());
  }
}

TEST_CASE("a FLASH save made before the power-cut fields loads with no torn quad-word")
{
  flash_rig rig;
  rig.control(pg);
  rig.store_words(last_page, {1, 2, 3, 4});
  rig.store_words(last_page + 16, {5, 6});
  REQUIRE(rig.status("BSY") == 1);
  auto const now = rig.state();
  REQUIRE(now.due_ps != 0);
  old_flash_state const old{
      .first_key   = now.first_key,
      .locked_up   = now.locked_up,
      .buffer      = now.buffer,
      .filled      = now.filled,
      .burst       = now.burst,
      .buffer_addr = now.buffer_addr,
      .running     = now.running,
      .target      = now.target,
      .length      = now.length,
      .generation  = now.generation,
      .held        = now.held,
  };
  std::vector<std::byte> bytes;
  reflex::serde::bson::serializer{bytes}.dump(old);
  rig.f.target->model("FLASH")->load(bytes);
  auto expected   = now;
  expected.due_ps = 0;
  CHECK(rig.state() == expected);
  rig.cycle();
  CHECK(rig.words(last_page, 4) == std::vector<std::uint32_t>{1, 2, 3, 4});
  CHECK(rig.words(last_page + 16, 4) == erased(4));
  CHECK(rig.torn().empty());
}

TEST_CASE("a CPU load of a torn quad-word sets ECCD, BK_ECC and ADDR_ECC and takes the NMI")
{
  for(address const line : {torn_line, address{0x0807'e030}})
  {
    for(address const alias : {address{0}, secure})
    {
      CAPTURE(line);
      CAPTURE(alias);
      flash_rig rig;
      rig.tear(line);
      REQUIRE(rig.eccr() == 0);
      REQUIRE_FALSE(rig.nmi_level());
      rig.clear_nmi = false;
      rig.handle_nmi();
      rig.load(line + alias);
      REQUIRE(rig.nmi_eccr.size() == 1);
      CHECK(rig.eccr() == reported(line));
      CHECK(rig.nmi_eccr.front() == reported(line));
      CHECK(rig.nmi_level());
      CHECK((rig.core_reg("xpsr") & 0x1ff) == exc_nmi);
      CHECK(rig.word(sym("flash_ll", "words")) == 0xdead'beefu);
    }
  }
  CHECK(reported(torn_line) == (eccd | bk_ecc | 0x7'e030u));
  CHECK(reported(0x0807'e030) == (eccd | 0x7'e030u));
}

TEST_CASE("loads of the quad-words around a torn one report nothing")
{
  flash_rig rig;
  rig.tear(torn_line);
  rig.handle_nmi();
  for(address const from : {torn_line - 16, torn_line - 4, torn_line + 16, torn_line + 28})
  {
    CAPTURE(from);
    rig.load(from);
    CHECK(rig.word(sym("flash_ll", "words")) == 0xffff'ffffu);
  }
  CHECK(rig.nmi_eccr.empty());
  CHECK(rig.eccr() == 0);
  CHECK_FALSE(rig.nmi_level());
  rig.load(torn_line + 12);
  CHECK(rig.nmi_eccr == std::vector<std::uint32_t>{reported(torn_line)});
}

TEST_CASE("two loads of one torn quad-word with ECCD cleared between report twice")
{
  flash_rig rig;
  rig.tear(torn_line);
  rig.handle_nmi();
  rig.load(torn_line);
  CHECK(rig.word(sym("flash_ll", "words")) == 1);
  rig.load(torn_line + 4);
  CHECK(rig.word(sym("flash_ll", "words")) == 2);
  CHECK(rig.nmi_eccr == std::vector<std::uint32_t>(2, reported(torn_line)));
  CHECK((rig.eccr() & (eccd | eccc)) == 0);
  CHECK_FALSE(rig.nmi_level());
  CHECK((rig.core_reg("xpsr") & 0x1ff) == 0);
}

TEST_CASE("a second torn load before ECCD is cleared changes nothing")
{
  flash_rig rig;
  rig.tear(torn_line);
  rig.tear(0x0807'e030);
  REQUIRE(rig.dma_load(torn_line));
  REQUIRE(rig.eccr() == reported(torn_line));
  CHECK(rig.dma_load(0x0807'e030));
  CHECK(rig.eccr() == reported(torn_line));
  CHECK(rig.nmi_level());
  rig.write("FLASH_ECCR", eccd);
  CHECK(rig.dma_load(0x0807'e030));
  CHECK(rig.eccr() == reported(0x0807'e030));
}

TEST_CASE("FLASH_ECCR clears ECCD and ECCC on a 1, stores ECCIE and keeps its read-only fields")
{
  flash_rig rig;
  rig.tear(torn_line);
  rig.write("FLASH_ECCR", ~std::uint32_t{0});
  CHECK(rig.eccr() == eccie);
  CHECK_FALSE(rig.nmi_level());
  CHECK(rig.dma_load(torn_line));
  CHECK(rig.eccr() == (reported(torn_line) | eccie));
  CHECK(rig.nmi_level());
  rig.write("FLASH_ECCR", 0);
  CHECK(rig.eccr() == reported(torn_line));
  CHECK(rig.nmi_level());
  REQUIRE(rig.f.target->mem().write(rig.at("FLASH_ECCR"), 1, 0xff).has_value());
  CHECK(rig.eccr() == reported(torn_line));
  CHECK(rig.nmi_level());
  rig.write("FLASH_ECCR", eccc);
  CHECK(rig.eccr() == reported(torn_line));
  CHECK(rig.nmi_level());
  rig.write("FLASH_ECCR", eccd);
  CHECK(rig.eccr() == (reported(torn_line) & ~eccd));
  CHECK_FALSE(rig.nmi_level());
  CHECK(rig.dma_load(torn_line));
  CHECK(rig.eccr() == reported(torn_line));
  CHECK(rig.nmi_level());
}

TEST_CASE("a byte or half-word write of ECCD in its lane clears it")
{
  for(std::uint8_t const size : {std::uint8_t{1}, std::uint8_t{2}})
  {
    CAPTURE(size);
    flash_rig rig;
    rig.tear(torn_line);
    REQUIRE(rig.dma_load(torn_line));
    REQUIRE(rig.nmi_level());
    auto const offset = size == 1 ? 3u : 2u;
    auto const value  = size == 1 ? 0x80u : 0x8000u;
    REQUIRE(rig.f.target->mem().write(rig.at("FLASH_ECCR") + offset, size, value).has_value());
    CHECK(rig.eccr() == (reported(torn_line) & ~eccd));
    CHECK_FALSE(rig.nmi_level());
  }
}

TEST_CASE("a load that ends where a torn quad-word starts reports nothing")
{
  flash_rig rig;
  rig.tear(torn_line);
  auto const& regions = rig.f.target->description().regions;
  auto const found = std::ranges::find(regions, address{0x0800'0000}, &popcorn::region_desc::start);
  REQUIRE(found != regions.end());
  auto* const model = rig.f.target->model("FLASH");
  REQUIRE(model != nullptr);
  model->on_memory_read(*found, torn_line - 4, 4, {.pc = 0, .now = {}, .by = initiator::cpu});
  model->on_memory_read(*found, torn_line - 8, 8, {.pc = 0, .now = {}, .by = initiator::cpu});
  CHECK(rig.eccr() == 0);
  CHECK_FALSE(rig.nmi_level());
  model->on_memory_read(*found, torn_line - 4, 8, {.pc = 0, .now = {}, .by = initiator::cpu});
  CHECK(rig.eccr() == reported(torn_line));
}

TEST_CASE("a DMA load of a torn quad-word reports, a host or debugger read does not")
{
  flash_rig rig;
  rig.tear(torn_line);
  CHECK(rig.f.target->mem().read(torn_line, 4) == 1);
  CHECK(rig.probe->services().read(torn_line, 4, initiator::debugger) == 1);
  std::array<std::byte, 16> block{};
  CHECK(rig.probe->services().read_block(torn_line, block, initiator::debugger));
  CHECK(rig.eccr() == 0);
  CHECK_FALSE(rig.nmi_level());
  CHECK(rig.dma_load(torn_line + secure + 8));
  CHECK(rig.eccr() == reported(torn_line));
  CHECK(rig.nmi_level());
}

TEST_CASE("an erase of a torn quad-word's page drops its watch and its reports")
{
  constexpr periph::stm32::flash_options timing{};
  flash_rig                              rig;
  rig.tear(torn_line);
  CHECK(rig.f.target->controller_watches() == 1);
  rig.tear(torn_line + 16);
  CHECK(rig.f.target->controller_watches() == 2);
  rig.erase_page(last_page - page_bytes);
  CHECK(rig.f.target->controller_watches() == 2);
  rig.erase_page(last_page);
  CHECK(rig.torn().empty());
  CHECK(rig.f.target->controller_watches() == 0);
  CHECK(rig.f.target->controller_watch_hooks() == 0);
  CHECK(rig.dma_load(torn_line));
  CHECK(rig.eccr() == 0);
  rig.program(torn_line, {1, 2, 3, 4});
  rig.control(0);
  rig.control(per | bker | pnb(63) | strt);
  rig.cut_after(timing.page_erase, timing.page_erase * 3 / 512 + timing.page_erase / 1024);
  REQUIRE(rig.torn() == std::vector<std::uint32_t>{torn_line});
  CHECK(rig.f.target->controller_watches() == 1);
  CHECK(rig.dma_load(torn_line));
  CHECK(rig.eccr() == reported(torn_line));
}

TEST_CASE("a restore watches the torn quad-words of the snapshot")
{
  flash_rig  rig;
  auto const before = rig.f.target->snapshot();
  rig.tear(torn_line);
  auto const after = rig.f.target->snapshot();
  rig.f.target->restore(before);
  CHECK(rig.f.target->controller_watches() == 0);
  CHECK(rig.dma_load(torn_line));
  CHECK(rig.eccr() == 0);
  CHECK_FALSE(rig.nmi_level());
  flash_rig other;
  other.f.target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(after.data()))});
  CHECK(other.f.target->controller_watches() == 1);
  CHECK(other.dma_load(torn_line));
  CHECK(other.eccr() == reported(torn_line));
  CHECK(other.nmi_level());
  auto const reporting = other.f.target->snapshot();
  flash_rig  third;
  third.f.target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(reporting.data()))});
  CHECK(third.eccr() == reported(torn_line));
  CHECK(third.nmi_level());
}

TEST_CASE("a software reset clears FLASH_ECCR and lowers the NMI, the torn line still reports")
{
  flash_rig rig;
  rig.tear(torn_line);
  REQUIRE(rig.dma_load(torn_line));
  REQUIRE(rig.nmi_level());
  rig.f.target->reset(popcorn::reset_kind::software);
  CHECK(rig.eccr() == 0);
  CHECK_FALSE(rig.nmi_level());
  CHECK(rig.torn() == std::vector<std::uint32_t>{torn_line});
  rig.handle_nmi();
  rig.load(torn_line);
  CHECK(rig.nmi_eccr == std::vector<std::uint32_t>{reported(torn_line)});
}

TEST_CASE("a torn load in one slice or in small slices reports the same")
{
  auto const run = [](icount slice) {
    auto rig = std::make_unique<flash_rig>();
    rig->tear(torn_line, slice);
    rig->handle_nmi();
    rig->load(torn_line + 4, slice);
    return rig;
  };
  auto const parked = run(icount{100'000'000});
  REQUIRE(parked->nmi_eccr == std::vector<std::uint32_t>{reported(torn_line)});
  for(auto const slice : {icount{1}, icount{7}})
  {
    CAPTURE(slice.value);
    auto const running = run(slice);
    CHECK(running->nmi_eccr == parked->nmi_eccr);
    CHECK(running->word(sym("flash_ll", "words")) == parked->word(sym("flash_ll", "words")));
    CHECK(running->eccr() == parked->eccr());
    CHECK(running->state() == parked->state());
  }
}

auto option_board(periph::stm32::option_bytes const& wanted) -> periph::stm32::board
{
  periph::stm32::board board;
  board.flash.option_bytes = wanted;
  board.flash.bank_erase   = std::chrono::microseconds{200};
  return board;
}

constexpr std::array<std::string_view, 8> option_registers{
    "FLASH_OPTR",     "FLASH_NSBOOTADD0R", "FLASH_NSBOOTADD1R", "FLASH_SECBOOTADD0R",
    "FLASH_SECWM1R1", "FLASH_SECWM1R2",    "FLASH_SECWM2R1",    "FLASH_SECWM2R2",
};

auto option_readback(flash_rig const& rig) -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(auto const name : option_registers)
  {
    out.push_back(rig.read(name));
  }
  return out;
}

TEST_CASE("option bytes load into the option registers at each reset, RM0456 7.9.13 to 7.9.22")
{
  flash_rig const rig{
      option_board({
                    .tzen    = true,
                    .secwm1  = periph::stm32::page_range{.first = 0, .last = 9},
                    .secwm2  = periph::stm32::no_area{},
                    .hdp1    = periph::stm32::page_range{.first = 0, .last = 5},
                    .secboot = 0x3000'0000,
                    .nsboot  = 0x2000'0000,
                    }
      )
  };
  std::vector<std::uint32_t> const expected{
      0x9fef'f8aa, 0x2000'007f, 0x0bf9'007f, 0x3000'007c,
      0xff89'ff80, 0xff85'7f80, 0xff80'ffff, 0x7f80'7f80,
  };
  CHECK(option_readback(rig) == expected);
  for(auto const name : option_registers)
  {
    CAPTURE(name);
    rig.write(name, 0);
    REQUIRE(rig.f.target->mem().write(rig.at(name) + 2, 1, 0x55).has_value());
  }
  CHECK(option_readback(rig) == expected);
  rig.f.target->reset(popcorn::reset_kind::software);
  CHECK(option_readback(rig) == expected);
}

namespace
{
constexpr address       sau_ctrl  = 0xe000'edd0;
constexpr std::uint32_t sau_allns = 2;

void engine_secure(flash_rig const& rig, bool secure)
{
  std::uint32_t value = secure ? 1 : 0;
  REQUIRE(
      uc_reg_write(core::machine_internals::engine(*rig.f.target), UC_ARM_REG_SECURE, &value)
      == UC_ERR_OK);
}
}

TEST_CASE(
    "a torn Secure program through SECCR: Non-secure DMA and CPU reads are RAZ with no ECC "
    "report, a Secure load through either alias reports the same ADDR_ECC")
{
  flash_rig rig{option_board({.tzen = true})};
  rig.tear(torn_line);
  rig.handle_nmi();
  auto const nonsecure = popcorn::bus_attrs{.by = initiator::dma, .secure = false};
  auto const flagged   = [&rig] {
    auto const* sr4 = popcorn::find_register(
        *std::ranges::find(
            u575().peripherals, std::string_view{"GTZC1_TZIC"}, &popcorn::peripheral_desc::name),
        "SR4");
    auto const flash =
        std::ranges::find(sr4->fields, std::string_view{"FLASHF"}, &popcorn::field_desc::name);
    return (rig.word(rig.at("SR4", "GTZC1_TZIC")) >> flash->lsb & 1u) != 0;
  };
  CHECK(rig.probe->services().read(torn_line + 16, 4, nonsecure) == 0u);
  CHECK(flagged());
  CHECK(rig.eccr() == 0);

  constexpr address code = 0x2002'0000;
  rig.write("GTZC1_TZIC", "FCR4", ~0u);
  REQUIRE_FALSE(flagged());
  rig.write(
      "GTZC1_MPCBB1", "SECCFGR8", rig.read_reg("GTZC1_MPCBB1", "SECCFGR8") & ~std::uint32_t{1});
  REQUIRE(rig.f.target->mem().write(code, 2, 0x6802).has_value());
  REQUIRE(rig.f.target->mem().write(code + 2, 2, 0xbe01).has_value());
  REQUIRE(rig.f.target->mem().write(sau_ctrl, 4, sau_allns).has_value());
  engine_secure(rig, false);
  rig.f.set("r0", torn_line);
  rig.f.set("r2", 0xdead'beef);
  rig.f.set("pc", code | 1u);
  auto const loaded = rig.f.target->run_slice({.max_instructions = icount{2}});
  REQUIRE(loaded.stopped.has_value());
  CHECK(loaded.stopped->reason == stop_reason::user);
  CHECK(rig.f.target->regs().read(*rig.f.target->regs().find("r2")) == 0);
  CHECK(flagged());
  CHECK(rig.eccr() == 0);
  CHECK_FALSE(rig.nmi_level());
  CHECK(rig.nmi_eccr.empty());
  engine_secure(rig, true);
  REQUIRE(rig.f.target->mem().write(sau_ctrl, 4, 0).has_value());

  rig.load(torn_line);
  CHECK(rig.nmi_eccr == std::vector<std::uint32_t>{reported(torn_line)});
  CHECK(rig.word(sym("flash_ll", "words")) == 1);
  rig.load(torn_line + secure);
  CHECK(rig.nmi_eccr == std::vector<std::uint32_t>(2, reported(torn_line)));
}

TEST_CASE("with TZEN = 0 the secure option registers read as zero, RM0456 7.5.1")
{
  flash_rig const rig{
      option_board({.secwm1 = periph::stm32::page_range{.first = 1, .last = 3}}
      )
  };
  CHECK(
      option_readback(rig)
      == std::vector<std::uint32_t>{0x1fef'f8aa, 0x0800'007f, 0x0bf9'007f, 0, 0, 0, 0, 0});
}

TEST_CASE("without option bytes the option registers are the description's storage")
{
  flash_rig const rig;
  CHECK(rig.read("FLASH_OPTR") == 0);
  rig.write("FLASH_SECWM1R1", 0x0005'0003);
  CHECK(rig.read("FLASH_SECWM1R1") == 0xff00'ff00);
  rig.unlock_options();
  rig.write("FLASH_SECWM1R1", 0x0005'0003);
  CHECK(rig.read("FLASH_SECWM1R1") == 0xff05'ff03);
}

TEST_CASE("SWAP_BANK puts bank 1 at half the flash for BKER, MER1, MER2 and BK_ECC, RM0456 7.9.13")
{
  flash_rig         rig{option_board({.swap_bank = true})};
  constexpr address low_page = 0x0800'0000 + 63 * page_bytes;
  rig.program(bank2, {1, 2, 3, 4});
  rig.control(0);
  rig.program(low_page, {5, 6, 7, 8});
  rig.control(0);
  rig.control(per | pnb(0) | strt);
  rig.wait_idle();
  rig.control(0);
  CHECK(rig.words(bank2, 4) == erased(4));
  CHECK(rig.words(low_page, 4) == std::vector<std::uint32_t>{5, 6, 7, 8});
  rig.control(per | bker | pnb(63) | strt);
  rig.wait_idle();
  rig.control(0);
  CHECK(rig.words(low_page, 4) == erased(4));
  rig.program(bank2, {1, 2, 3, 4});
  rig.control(0);
  rig.program(low_page, {5, 6, 7, 8});
  rig.control(0);
  rig.control(mer1 | strt);
  rig.wait_idle();
  rig.control(0);
  CHECK(rig.words(bank2, 4) == erased(4));
  CHECK(rig.words(low_page, 4) == std::vector<std::uint32_t>{5, 6, 7, 8});
  rig.tear(torn_line);
  REQUIRE(rig.dma_load(torn_line));
  CHECK(rig.eccr() == (eccd | static_cast<std::uint32_t>(torn_line - bank2)));
  rig.control(mer2 | strt);
  CHECK(rig.status("BSY") == 1);
  CHECK(rig.state().target == 0x0800'0000);
  CHECK(rig.state().length == bank2 - 0x0800'0000);
}

TEST_CASE("a FLASH model given option bytes its family refuses throws config_error")
{
  periph::stm32::register_builtin_models();
  for(auto const& wanted : std::vector<periph::stm32::option_bytes>{
          {.secwm2 = periph::stm32::page_range{.first = 0, .last = 64}},
          {.hdp1 = periph::stm32::page_range{.first = 1, .last = 2}},
          {.secboot = 0x0c00'0001},
          {.dualbank = false},
      })
  {
    CHECK_THROWS_AS(
        core::machine(
            u575(), popcorn::arch::armv8m::factory(), board_options(option_board(wanted), false)),
        popcorn::config_error);
  }
}

TEST_CASE(
    "model_context::trustzone follows machine_options::trustzone, FLASH refuses a TZEN that "
    "differs")
{
  periph::stm32::register_builtin_models();
  CHECK(flash_rig{option_board({.tzen = true})}.probe->trustzone());
  CHECK_FALSE(flash_rig{option_board({})}.probe->trustzone());
  CHECK_FALSE(flash_rig{}.probe->trustzone());
  auto const refusal = [](bool tzen, bool trustzone) -> std::string {
    auto options      = board_options(option_board({.tzen = tzen}), false);
    options.trustzone = trustzone;
    if(not trustzone)
    {
      options.idau.clear();
    }
    try
    {
      core::machine const built{u575(), popcorn::arch::armv8m::factory(), std::move(options)};
    }
    catch(popcorn::config_error const& error)
    {
      return error.key();
    }
    return {};
  };
  CHECK(refusal(true, false) == "option_bytes.tzen");
  CHECK(refusal(false, true) == "option_bytes.tzen");
  CHECK(refusal(true, true).empty());
}

TEST_CASE("check_option_bytes returns the boot address and the refusal of each field")
{
  using periph::stm32::check_option_bytes;
  CHECK(check_option_bytes(u575(), {}) == address{0x0800'0000});
  CHECK(check_option_bytes(u575(), {.tzen = true}) == address{0x0c00'0000});
  CHECK(check_option_bytes(u575(), {.tzen = true, .secboot = 0x0c00'0080}) == address{0x0c00'0080});
  CHECK(check_option_bytes(u575(), {.nsboot = 0x0800'4000}) == address{0x0800'4000});
  auto const refused = [](periph::stm32::option_bytes const& wanted) {
    auto const result = check_option_bytes(u575(), wanted);
    REQUIRE_FALSE(result.has_value());
    return result.error().key;
  };
  CHECK(
      refused({
          .secwm1 = periph::stm32::page_range{.first = 0, .last = 0x80}
  })
      == "option_bytes.secwm1");
  CHECK(
      refused({
          .secwm1 = periph::stm32::page_range{.first = 3, .last = 2}
  })
      == "option_bytes.secwm1");
  CHECK(
      refused({.secwm1 = periph::stm32::no_area{}, .hdp1 = periph::stm32::page_range{}})
      == "option_bytes.hdp1");
  CHECK(
      refused({
          .secwm2 = periph::stm32::page_range{.first = 2, .last = 9 },
          .hdp2   = periph::stm32::page_range{.first = 2, .last = 10}
  })
      == "option_bytes.hdp2");
  CHECK(refused({.nsboot = 0x0800'0040}) == "option_bytes.nsboot");
  CHECK(refused({.dualbank = false}) == "option_bytes.dualbank");
  CHECK(check_option_bytes(u575(), {.dualbank = true}).has_value());
  auto other        = u575();
  other.part        = "STM32H573IIKx";
  auto const result = check_option_bytes(other, {});
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().key == "option_bytes");
}

TEST_CASE("the U5 IDAU map is RM0456 2.2 Table 3, other families have none")
{
  auto const map = periph::stm32::idau_map(u575());
  REQUIRE(map.has_value());
  std::vector<popcorn::idau_region> const expected{
      {.start = 0x0000'0000, .end = 0x0c00'0000,   .nsc = false, .exempt = false},
      {.start = 0x0c00'0000, .end = 0x1000'0000,   .nsc = true,  .exempt = false},
      {.start = 0x1000'0000, .end = 0x3000'0000,   .nsc = false, .exempt = false},
      {.start = 0x3000'0000, .end = 0x4000'0000,   .nsc = true,  .exempt = false},
      {.start = 0x4000'0000, .end = 0x5000'0000,   .nsc = false, .exempt = false},
      {.start = 0x5000'0000, .end = 0x6000'0000,   .nsc = true,  .exempt = false},
      {.start = 0x6000'0000, .end = 0xe000'0000,   .nsc = false, .exempt = false},
      {.start = 0xe000'0000, .end = 0xe004'0000,   .nsc = false, .exempt = false},
      {.start = 0xe004'0000, .end = 0xe010'0000,   .nsc = false, .exempt = true },
      {.start = 0xe010'0000, .end = 0x1'0000'0000, .nsc = false, .exempt = false},
  };
  CHECK(std::ranges::equal(*map, expected));
  auto other = u575();
  other.part = "STM32H573IIKx";
  CHECK_FALSE(periph::stm32::idau_map(other).has_value());
}

TEST_CASE("configure_option_bytes wires the FLASH model, the boot address and TrustZone")
{
  using periph::stm32::configure_option_bytes;
  core::machine_options options;
  REQUIRE(configure_option_bytes(u575(), {.tzen = true}, options).has_value());
  CHECK(options.models.contains("FLASH"));
  CHECK(options.vector_table == address{0x0c00'0000});
  CHECK(options.trustzone);
  CHECK(std::ranges::equal(options.idau, *periph::stm32::idau_map(u575())));
  core::machine_options given;
  given.vector_table = 0x0800'4000;
  REQUIRE(configure_option_bytes(u575(), {.nsboot = 0x0800'2000}, given).has_value());
  CHECK(given.vector_table == address{0x0800'4000});
  CHECK_FALSE(given.trustzone);
  CHECK(given.idau.empty());
}

TEST_CASE("configure_option_bytes refuses before it changes the options")
{
  using periph::stm32::configure_option_bytes;
  auto const refused = [](popcorn::part_description const&   part,
                          periph::stm32::option_bytes const& wanted,
                          core::machine_options&             options) {
    auto const result = configure_option_bytes(part, wanted, options);
    REQUIRE_FALSE(result.has_value());
    CHECK(options.models.size() <= 1);
    CHECK_FALSE(options.trustzone);
    CHECK(options.idau.empty());
    return result.error().key;
  };
  core::machine_options options;
  auto                  no_extension = u575();
  no_extension.core.trustzone        = false;
  CHECK(refused(no_extension, {.tzen = true}, options) == "option_bytes.tzen");
  CHECK(options.models.empty());
  CHECK_FALSE(options.vector_table);
  auto other_family = u575();
  other_family.part = "STM32H573IIKx";
  CHECK(refused(other_family, {.tzen = true}, options) == "option_bytes.tzen");
  CHECK(refused(other_family, {}, options) == "option_bytes");
  CHECK(
      refused(
          u575(),
          {
              .secwm1 = periph::stm32::page_range{.first = 0, .last = 64}
  },
          options)
      == "option_bytes.secwm1");
  CHECK(options.models.empty());
  options.models.emplace("FLASH", periph::model_factory{});
  CHECK(refused(u575(), {.tzen = true}, options) == "option_bytes");
  CHECK_FALSE(options.vector_table);
  CHECK(configure_option_bytes(no_extension, {}, (options = {})).has_value());
}

TEST_CASE("OPTLOCK clears on OPTKEY1 then OPTKEY2 with LOCK clear, RM0456 7.4.2")
{
  flash_rig rig;
  REQUIRE(rig.field("FLASH_NSCR", "OPTLOCK") == 1);
  rig.write("FLASH_OPTKEYR", option_key1);
  CHECK(rig.field("FLASH_NSCR", "OPTLOCK") == 1);
  CHECK(rig.read("FLASH_OPTKEYR") == 0);
  rig.write("FLASH_OPTKEYR", option_key2);
  CHECK(rig.field("FLASH_NSCR", "OPTLOCK") == 0);
  CHECK(rig.field("FLASH_NSCR", "LOCK") == 0);
}

TEST_CASE("a wrong OPTKEYR sequence keeps OPTLOCK set until reset and returns a bus error")
{
  for(auto const wrong : {0x1234'5678u, option_key2})
  {
    CAPTURE(wrong);
    flash_rig rig;
    CHECK_FALSE(rig.try_write("FLASH_OPTKEYR", wrong));
    CHECK(rig.try_write("FLASH_OPTKEYR", option_key1) == false);
    CHECK_FALSE(rig.try_write("FLASH_OPTKEYR", option_key2));
    CHECK(rig.field("FLASH_NSCR", "OPTLOCK") == 1);
    rig.f.target->reset(popcorn::reset_kind::software);
    rig.unlock();
    rig.unlock_options();
    CHECK(rig.field("FLASH_NSCR", "OPTLOCK") == 0);
  }
}

TEST_CASE("KEY1 twice, or a key while OPTLOCK is clear, is a wrong OPTKEYR sequence")
{
  flash_rig rig;
  REQUIRE(rig.try_write("FLASH_OPTKEYR", option_key1));
  CHECK_FALSE(rig.try_write("FLASH_OPTKEYR", option_key1));
  CHECK_FALSE(rig.try_write("FLASH_OPTKEYR", option_key2));
  CHECK(rig.field("FLASH_NSCR", "OPTLOCK") == 1);
  rig.f.target->reset(popcorn::reset_kind::software);
  rig.unlock();
  rig.unlock_options();
  REQUIRE(rig.field("FLASH_NSCR", "OPTLOCK") == 0);
  CHECK_FALSE(rig.try_write("FLASH_OPTKEYR", option_key1));
  CHECK(rig.field("FLASH_NSCR", "OPTLOCK") == 0);
}

TEST_CASE("an OPTKEYR sequence while NSCR.LOCK is set does not unlock OPTLOCK")
{
  flash_rig rig;
  rig.control(lock_bit);
  CHECK_FALSE(rig.try_write("FLASH_OPTKEYR", option_key1));
  CHECK_FALSE(rig.try_write("FLASH_OPTKEYR", option_key2));
  CHECK(rig.field("FLASH_NSCR", "OPTLOCK") == 1);
}

TEST_CASE("writing OPTLOCK or LOCK relocks the option registers, and a 0 write does not unlock")
{
  flash_rig rig;
  rig.unlock_options();
  REQUIRE(rig.field("FLASH_NSCR", "OPTLOCK") == 0);
  rig.control(0);
  CHECK(rig.field("FLASH_NSCR", "OPTLOCK") == 0);
  rig.control(optlock_bit);
  CHECK(rig.field("FLASH_NSCR", "OPTLOCK") == 1);
  rig.control(0);
  CHECK(rig.field("FLASH_NSCR", "OPTLOCK") == 1);
  rig.unlock_options();
  REQUIRE(rig.field("FLASH_NSCR", "OPTLOCK") == 0);
  rig.control(lock_bit);
  CHECK(rig.field("FLASH_NSCR", "OPTLOCK") == 1);
  CHECK(rig.field("FLASH_NSCR", "LOCK") == 1);
}

TEST_CASE("OBL_LAUNCH and the option registers ignore writes while OPTLOCK is set")
{
  flash_rig rig;
  rig.control(obl_launch);
  CHECK(rig.field("FLASH_NSCR", "OBL_LAUNCH") == 0);
  rig.write("FLASH_SECWM1R1", 0x0005'0003);
  CHECK(rig.read("FLASH_SECWM1R1") == 0xff00'ff00);
  rig.unlock_options();
  rig.write("FLASH_SECWM1R1", 0x0005'0003);
  CHECK(rig.read("FLASH_SECWM1R1") == 0xff05'ff03);
  rig.control(obl_launch);
  CHECK(rig.field("FLASH_NSCR", "OBL_LAUNCH") == 1);
}
