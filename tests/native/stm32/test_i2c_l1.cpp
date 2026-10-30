#include "i2c_link_port.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/devices/i2c_eeprom.hpp>
#include <popcorn/periph/i2c.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/i2c.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core    = popcorn::core;
namespace periph  = popcorn::periph;
namespace stm32   = popcorn::periph::stm32;
namespace devices = popcorn::periph::devices;
using popcorn::address;
using popcorn::icount;
using popcorn::part_description;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::l152rb;
using popcorn::testing::l152re;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr std::string_view firmware   = "l152xe_l1_i2c_ll";
constexpr std::string_view firmware_b = "l152xb_l1_i2c_ll";
constexpr address          i2c1       = 0x4000'5400;
constexpr address          i2c2       = 0x4000'5800;
constexpr address          cr1        = 0x00;
constexpr address          cr2        = 0x04;
constexpr address          oar1       = 0x08;
constexpr address          oar2       = 0x0C;
constexpr address          dr         = 0x10;
constexpr address          sr1        = 0x14;
constexpr address          sr2        = 0x18;
constexpr address          ccr        = 0x1C;
constexpr address          trise      = 0x20;
constexpr std::uint32_t    pe         = 1u << 0;
constexpr std::uint32_t    smbus      = 1u << 1;
constexpr std::uint32_t    enarp      = 1u << 4;
constexpr std::uint32_t    enpec      = 1u << 5;
constexpr std::uint32_t    pec        = 1u << 12;
constexpr std::uint32_t    alert      = 1u << 13;
constexpr std::uint32_t    start      = 1u << 8;
constexpr std::uint32_t    stop       = 1u << 9;
constexpr std::uint32_t    ack_bit    = 1u << 10;
constexpr std::uint32_t    pos_bit    = 1u << 11;
constexpr std::uint32_t    swrst      = 1u << 15;
constexpr std::uint32_t    freq2      = 2;
constexpr std::uint32_t    iterren    = 1u << 8;
constexpr std::uint32_t    itevten    = 1u << 9;
constexpr std::uint32_t    itbufen    = 1u << 10;
constexpr std::uint32_t    dmaen      = 1u << 11;
constexpr std::uint32_t    sb         = 1u << 0;
constexpr std::uint32_t    addr       = 1u << 1;
constexpr std::uint32_t    btf        = 1u << 2;
constexpr std::uint32_t    rxne       = 1u << 6;
constexpr std::uint32_t    txe        = 1u << 7;
constexpr std::uint32_t    af         = 1u << 10;
constexpr std::uint32_t    msl        = 1u << 0;
constexpr std::uint32_t    busy       = 1u << 1;
constexpr std::uint32_t    tra        = 1u << 2;
constexpr std::uint32_t    fast       = 1u << 15;
constexpr std::uint32_t    duty       = 1u << 14;
constexpr address          rcc        = 0x4002'3800;
constexpr address          apb1rstr   = rcc + 0x18;
constexpr address          apb1enr    = rcc + 0x24;
constexpr std::uint32_t    i2c1_bit   = 1u << 21;
constexpr std::uint32_t    i2c2_bit   = 1u << 22;
constexpr address          nvic_ispr0 = 0xE000'E200;
constexpr address          nvic_ispr1 = nvic_ispr0 + 4;
constexpr std::uint32_t    event_irq  = 1u << 31;
constexpr std::uint32_t    error_irq  = 1u << 0;
constexpr std::uint64_t    pclk1      = 2'097'152;
constexpr std::uint8_t     device     = 0x50;
constexpr std::uint64_t    margin     = 1'000'000;

constexpr auto phase(std::uint64_t periods, std::uint64_t cycles = 20) -> std::uint64_t
{
  return periods * cycles * 1'000'000'000'000 / pclk1;
}

class probe_target final : public periph::i2c_target
{
public:
  struct heard
  {
    char          kind;
    std::uint8_t  byte;
    std::uint64_t at;
  };

  explicit probe_target(std::uint8_t at = device) : address_{at}
  {}

  std::vector<heard>         log;
  std::vector<std::uint8_t>  out;
  std::optional<std::size_t> nack_byte;
  std::uint64_t              stretch_ps{};
  periph::i2c_error          error{};
  bool                       defer{};
  int                        matched{};
  bool                       defer_read{};
  int                        attaches{};
  int                        detaches{};

  [[nodiscard]] auto address() const -> std::uint8_t override
  {
    return address_;
  }

  auto matches(std::uint8_t sent, vtime at) -> bool override
  {
    ++matched;
    return i2c_target::matches(sent, at);
  }

  auto start(bool read, vtime at) -> periph::i2c_reply override
  {
    log.push_back({'S', static_cast<std::uint8_t>(read ? 1 : 0), at.ps});
    return {.ack = true, .error = error, .stretch_ps = stretch_ps};
  }

  auto write(std::uint8_t byte, vtime at) -> periph::i2c_reply override
  {
    auto const index = bytes().size();
    log.push_back({'W', byte, at.ps});
    return {.ack = nack_byte != index, .stretch_ps = stretch_ps};
  }

  auto read(vtime at) -> periph::i2c_read_reply override
  {
    return read_acked(at, true);
  }

  auto read_acked(vtime at, bool acked) -> periph::i2c_read_reply override
  {
    auto const index = static_cast<std::size_t>(std::ranges::count(kinds(), 'R'));
    log.push_back({'R', static_cast<std::uint8_t>(acked ? 1 : 0), at.ps});
    return {.byte = index < out.size() ? out[index] : std::uint8_t{0xFF}, .stretch_ps = stretch_ps};
  }

  auto read_deferred(vtime at, bool acked, std::uint64_t ticket)
      -> std::optional<periph::i2c_read_reply> override
  {
    if(defer_read)
    {
      log.push_back({'Q', 0, at.ps});
      return std::nullopt;
    }
    return i2c_target::read_deferred(at, acked, ticket);
  }

  [[nodiscard]] auto acks() const -> std::vector<std::uint8_t>
  {
    std::vector<std::uint8_t> got;
    for(auto const& one : log)
    {
      if(one.kind == 'R')
      {
        got.push_back(one.byte);
      }
    }
    return got;
  }

  auto start_deferred(std::uint8_t sent, bool read, vtime at, std::uint64_t ticket)
      -> std::optional<periph::i2c_reply> override
  {
    if(defer)
    {
      log.push_back({'D', sent, at.ps});
      return std::nullopt;
    }
    return i2c_target::start_deferred(sent, read, at, ticket);
  }

  void stop(vtime at) override
  {
    log.push_back({'P', 0, at.ps});
  }

  void attached(periph::i2c_target_port&) override
  {
    ++attaches;
  }

  void detached() noexcept override
  {
    ++detaches;
  }

  void save(std::vector<std::byte>& out) const override
  {
    for(auto const& one : log)
    {
      out.push_back(static_cast<std::byte>(one.kind));
    }
  }

  void load(std::span<std::byte const> in) override
  {
    log.clear();
    for(auto const one : in)
    {
      log.push_back({static_cast<char>(one), 0, 0});
    }
  }

  [[nodiscard]] auto bytes() const -> std::vector<std::uint8_t>
  {
    std::vector<std::uint8_t> out;
    for(auto const& one : log)
    {
      if(one.kind == 'W')
      {
        out.push_back(one.byte);
      }
    }
    return out;
  }

  [[nodiscard]] auto kinds() const -> std::string
  {
    std::string out;
    for(auto const& one : log)
    {
      out.push_back(one.kind);
    }
    return out;
  }

private:
  std::uint8_t address_;
};

class bus_probe final : public periph::model
{
public:
  explicit bus_probe(periph::model_context& ctx) : context{ctx}
  {}

  periph::model_context& context;

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }
  void reset(periph::register_file&, popcorn::reset_kind) override
  {}
  auto on_read(periph::register_file&, std::uint32_t, std::uint8_t, periph::access_ctx)
      -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    return 0u;
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
};

struct rig
{
  std::shared_ptr<probe_target>         target = std::make_shared<probe_target>();
  std::shared_ptr<devices::eeprom_24xx> eeprom;
  stm32::board                          board{};
  std::unique_ptr<stm32_fixture>        f;
  std::vector<core::log_record>         logged;
  core::hook_handle                     log_hook;

  explicit rig(
      std::string_view                                 entry,
      std::vector<std::shared_ptr<periph::i2c_target>> extra  = {},
      part_description const&                          desc   = l152re(),
      std::string_view                                 elf    = firmware,
      bool                                             strict = false)
  {
    if(entry == "write_eeprom" or entry == "nack")
    {
      eeprom = std::make_shared<devices::eeprom_24xx>(
          devices::eeprom_24xx_options{.write_cycle = std::chrono::microseconds{100}});
      board.i2c["I2C1"].push_back(eeprom);
    }
    else if(entry.starts_with("hal_"))
    {
      eeprom = std::make_shared<devices::eeprom_24xx>();
      board.i2c["I2C1"].push_back(eeprom);
    }
    else
    {
      board.i2c["I2C1"].push_back(target);
    }
    for(auto& one : extra)
    {
      board.i2c["I2C1"].push_back(std::move(one));
    }
    auto options               = popcorn::testing::stm32_options();
    options.models             = stm32::factories(board, desc);
    options.strict_peripherals = strict;
    options.models.insert_or_assign(
        "DAC", [](periph::model_context& ctx, popcorn::peripheral_desc const&) {
          return std::make_shared<bus_probe>(ctx);
        });
    f        = std::make_unique<stm32_fixture>(desc, elf, std::move(options));
    log_hook = f->target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
    if(entry == "parked")
    {
      f->set("pc", sym(elf, "idle"));
      write(apb1enr, i2c1_bit | i2c2_bit);
      write(i2c1 + cr2, freq2);
      write(i2c1 + ccr, 10);
      write(i2c1 + trise, 3);
      write(i2c1 + cr1, pe);
      return;
    }
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{1'000'000}, .until = sym(elf, "main")}).reason
        == stop_reason::address);
    f->set("pc", sym(elf, entry));
  }

  auto finish() const -> std::vector<std::uint32_t>
  {
    auto const stopped = f->run_to_stop({.max_instructions = icount{2'000'000}});
    CAPTURE(stopped.detail);
    REQUIRE(stopped.reason == stop_reason::user);
    return f->log();
  }

  auto read(address at) const -> std::uint32_t
  {
    auto const value = f->target->mem().read(at, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void write(address at, std::uint32_t value) const
  {
    REQUIRE(f->target->mem().write(at, 4, value).has_value());
  }

  [[nodiscard]] auto services() const -> popcorn::core::machine_services&
  {
    auto* const probe = f->model_of<bus_probe>("DAC");
    REQUIRE(probe != nullptr);
    return probe->context.services;
  }

  auto cpu_read(address at) const -> std::uint32_t
  {
    auto* const probe = f->model_of<bus_probe>("DAC");
    REQUIRE(probe != nullptr);
    auto const word = probe->context.services.read(at, 4, popcorn::initiator::cpu);
    REQUIRE(word.has_value());
    return static_cast<std::uint32_t>(*word);
  }

  void cpu_write(address at, std::uint32_t value) const
  {
    auto* const probe = f->model_of<bus_probe>("DAC");
    REQUIRE(probe != nullptr);
    REQUIRE(probe->context.services.write(at, 4, value, popcorn::initiator::cpu).has_value());
  }

  void idle_to(std::uint64_t ps) const
  {
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{1'000'000}, .until_time = vtime{ps}}).reason
        == stop_reason::time);
  }

  [[nodiscard]] auto now_ps() const -> std::uint64_t
  {
    return f->target->virtual_clock().time().ps;
  }

  [[nodiscard]] auto warnings(std::string_view text) const -> std::ptrdiff_t
  {
    return std::ranges::count_if(logged, [&](core::log_record const& record) {
      return record.message.find(text) != std::string::npos;
    });
  }

  auto started(std::uint32_t control = pe) const -> std::uint64_t
  {
    auto const t0 = now_ps();
    cpu_write(i2c1 + cr1, control | start);
    idle_to(t0 + phase(1));
    return t0 + phase(1);
  }

  auto addressed(std::uint8_t to = device, bool read_bit = false, std::uint32_t control = pe) const
      -> std::uint64_t
  {
    static_cast<void>(started(control));
    static_cast<void>(cpu_read(i2c1 + sr1));
    auto const t1 = now_ps();
    cpu_write(i2c1 + dr, static_cast<std::uint32_t>(to << 1 | (read_bit ? 1 : 0)));
    idle_to(t1 + phase(9));
    return t1 + phase(9);
  }

  void transmitting() const
  {
    static_cast<void>(addressed());
    static_cast<void>(cpu_read(i2c1 + sr1));
    static_cast<void>(cpu_read(i2c1 + sr2));
  }

  auto receiving(std::uint32_t control = pe | ack_bit) const -> std::uint64_t
  {
    static_cast<void>(addressed(device, true, control));
    static_cast<void>(cpu_read(i2c1 + sr1));
    static_cast<void>(cpu_read(i2c1 + sr2));
    return now_ps();
  }
};

auto refusal(auto&& call) -> std::string
{
  try
  {
    call();
  }
  catch(popcorn::config_error const& error)
  {
    return error.what();
  }
  return {};
}

TEST_CASE("RM0038 Table 140: TRISE 0x0002, every other I2C register 0 after reset")
{
  for(auto const& [desc, elf] : {
          std::pair{&l152re(), firmware  },
          std::pair{&l152rb(), firmware_b}
  })
  {
    rig r{"idle", {}, *desc, elf};
    r.f->target->reset();
    for(auto const base : {i2c1, i2c2})
    {
      CAPTURE(base);
      for(auto const offset : {cr1, cr2, oar1, oar2, dr, sr1, sr2, ccr})
      {
        CHECK(r.read(base + offset) == 0);
      }
      CHECK(r.read(base + trise) == 2);
    }
  }
}

TEST_CASE("RM0038 26.3.3 Figure 212: LL writes the word address and three bytes to a 24xx EEPROM")
{
  rig r{"write_eeprom"};
  CHECK(r.finish() == std::vector<std::uint32_t>{msl | busy | tra, txe | btf, 0, 0});
  r.f->set("pc", sym(firmware, "idle"));
  r.idle_to(r.now_ps() + 200'000'000);
  auto const contents = r.eeprom->contents();
  CHECK(contents[0x10] == std::byte{0xA5});
  CHECK(contents[0x11] == std::byte{0x5A});
  CHECK(contents[0x12] == std::byte{0x3C});
  CHECK(std::ranges::none_of(r.logged, [](core::log_record const& one) {
    return one.peripheral == "I2C1";
  }));
}

TEST_CASE("RM0038 26.3.4 AF: LL addresses 0x51, no target acknowledges, AF kept by the STOP")
{
  rig r{"nack"};
  CHECK(r.finish() == std::vector<std::uint32_t>{af, msl | busy, af, 0, 0});
}

TEST_CASE("RM0038 26.6.8: START 1 SCL period, address 9, Sm 2 x CCR, Fm 3 x CCR and 25 x CCR")
{
  for(auto const& [mode, cycles] : {
          std::pair{0u,          20ull },
          std::pair{fast,        30ull },
          std::pair{fast | duty, 250ull}
  })
  {
    CAPTURE(mode);
    rig r{"parked"};
    r.write(i2c1 + cr1, 0);
    r.write(i2c1 + ccr, mode | 10);
    r.write(i2c1 + cr1, pe);
    auto const t0 = r.now_ps();
    r.cpu_write(i2c1 + cr1, pe | start);
    CHECK((r.read(i2c1 + sr2) & busy) == busy);
    r.idle_to(t0 + phase(1, cycles) - margin);
    CHECK((r.read(i2c1 + sr1) & sb) == 0);
    r.idle_to(t0 + phase(1, cycles));
    CHECK(r.read(i2c1 + sr1) == sb);
    CHECK(r.read(i2c1 + sr2) == (msl | busy));
    CHECK((r.read(i2c1 + cr1) & start) == 0);
    static_cast<void>(r.cpu_read(i2c1 + sr1));
    auto const t1 = r.now_ps();
    r.cpu_write(i2c1 + dr, device << 1);
    CHECK(r.read(i2c1 + sr1) == 0);
    r.idle_to(t1 + phase(9, cycles) - margin);
    CHECK(r.target->log.empty());
    r.idle_to(t1 + phase(9, cycles));
    REQUIRE(r.target->kinds() == "S");
    CHECK(r.target->log[0].at == t1 + phase(9, cycles));
    CHECK(r.read(i2c1 + sr1) == addr);
    CHECK(r.read(i2c1 + sr2) == (msl | busy | tra));
  }
}

TEST_CASE("RM0038 26.6.7 EV6: ADDR clears by an SR1 read then an SR2 read, debugger reads aside")
{
  rig r{"parked"};
  static_cast<void>(r.addressed());
  CHECK((r.cpu_read(i2c1 + sr2) & msl) == msl);
  CHECK((r.read(i2c1 + sr1) & addr) == addr);
  CHECK((r.cpu_read(i2c1 + sr1) & addr) == addr);
  CHECK((r.cpu_read(i2c1 + sr1) & addr) == addr);
  static_cast<void>(r.read(i2c1 + sr2));
  CHECK((r.read(i2c1 + sr1) & addr) == addr);
  CHECK((r.cpu_read(i2c1 + sr2) & tra) == tra);
  CHECK(r.read(i2c1 + sr1) == txe);
}

TEST_CASE("RM0038 26.6.7: an SR1 read before ADDR sets arms the SR2 read that clears it")
{
  rig r{"parked"};
  static_cast<void>(r.started());
  static_cast<void>(r.cpu_read(i2c1 + sr1));
  auto const t1 = r.now_ps();
  r.cpu_write(i2c1 + dr, device << 1);
  static_cast<void>(r.cpu_read(i2c1 + sr1));
  r.idle_to(t1 + phase(9));
  CHECK((r.read(i2c1 + sr1) & addr) == addr);
  static_cast<void>(r.cpu_read(i2c1 + sr2));
  CHECK(r.read(i2c1 + sr1) == txe);
}

TEST_CASE("RM0038 26.6.6 SB, inferred: a DR write with no SR1 read since keeps SB, sends nothing")
{
  rig r{"parked"};
  static_cast<void>(r.started());
  r.cpu_write(i2c1 + dr, device << 1);
  r.idle_to(r.now_ps() + phase(20));
  CHECK(r.read(i2c1 + sr1) == sb);
  CHECK(r.target->log.empty());
  static_cast<void>(r.read(i2c1 + sr1));
  r.cpu_write(i2c1 + dr, device << 1);
  r.idle_to(r.now_ps() + phase(20));
  CHECK(r.target->log.empty());
  static_cast<void>(r.cpu_read(i2c1 + sr1));
  r.cpu_write(i2c1 + dr, device << 1);
  r.idle_to(r.now_ps() + phase(9));
  CHECK(r.target->kinds() == "S");
}

TEST_CASE("RM0038 26.3.3 EV8_1 EV8: TxE kept by the first DR write, DR moves at the acknowledge")
{
  rig r{"parked"};
  r.transmitting();
  auto const t0 = r.now_ps();
  r.cpu_write(i2c1 + dr, 0x11);
  CHECK(r.read(i2c1 + sr1) == txe);
  r.cpu_write(i2c1 + dr, 0x22);
  CHECK(r.read(i2c1 + sr1) == 0);
  r.idle_to(t0 + phase(9) - margin);
  CHECK(r.target->bytes().empty());
  r.idle_to(t0 + phase(9));
  CHECK(r.target->bytes() == std::vector<std::uint8_t>{0x11});
  CHECK(r.target->log.back().at == t0 + phase(9));
  CHECK(r.read(i2c1 + sr1) == txe);
  r.idle_to(t0 + phase(18));
  CHECK(r.target->bytes() == std::vector<std::uint8_t>{0x11, 0x22});
  CHECK(r.read(i2c1 + sr1) == (txe | btf));
  r.idle_to(t0 + phase(30));
  CHECK(r.target->bytes().size() == 2);
  auto const t1 = r.now_ps();
  r.cpu_write(i2c1 + dr, 0x33);
  CHECK(r.read(i2c1 + sr1) == txe);
  r.idle_to(t1 + phase(9));
  CHECK(r.target->bytes() == std::vector<std::uint8_t>{0x11, 0x22, 0x33});
}

TEST_CASE("RM0038 26.6.6 BTF: a DR read clears BTF, SCL stays low until a DR write")
{
  rig r{"parked"};
  r.transmitting();
  r.cpu_write(i2c1 + dr, 0x11);
  r.idle_to(r.now_ps() + phase(9));
  CHECK(r.read(i2c1 + sr1) == (txe | btf));
  static_cast<void>(r.cpu_read(i2c1 + dr));
  CHECK(r.read(i2c1 + sr1) == txe);
  r.idle_to(r.now_ps() + phase(20));
  CHECK(r.target->bytes().size() == 1);
}

TEST_CASE("RM0038 26.3.3 EV8_2: STOP after BTF lasts 1 period, clears TxE, BTF, MSL, BUSY, TRA")
{
  rig r{"parked"};
  r.transmitting();
  r.cpu_write(i2c1 + dr, 0x11);
  r.idle_to(r.now_ps() + phase(9));
  auto const t0 = r.now_ps();
  r.cpu_write(i2c1 + cr1, pe | stop);
  r.idle_to(t0 + phase(1) - margin);
  CHECK(r.target->kinds() == "SW");
  CHECK(r.read(i2c1 + sr2) == (msl | busy | tra));
  r.idle_to(t0 + phase(1));
  CHECK(r.target->kinds() == "SWP");
  CHECK(r.target->log.back().at == t0 + phase(1));
  CHECK(r.read(i2c1 + sr1) == 0);
  CHECK(r.read(i2c1 + sr2) == 0);
  CHECK(r.read(i2c1 + cr1) == pe);
}

TEST_CASE("RM0038 26.6.1 STOP: a STOP set while a byte shifts goes after it, DR is not sent")
{
  rig r{"parked"};
  r.transmitting();
  auto const t0 = r.now_ps();
  r.cpu_write(i2c1 + dr, 0x11);
  r.cpu_write(i2c1 + dr, 0x22);
  r.cpu_write(i2c1 + cr1, pe | stop);
  r.idle_to(t0 + phase(9) + phase(1));
  CHECK(r.target->kinds() == "SWP");
  CHECK(r.target->bytes() == std::vector<std::uint8_t>{0x11});
  CHECK(r.target->log.back().at == t0 + phase(9) + phase(1));
  CHECK(r.read(i2c1 + sr1) == 0);
  r.idle_to(t0 + phase(30));
  CHECK(r.target->kinds() == "SWP");
}

TEST_CASE(
    "RM0038 26.6.6 AF: a NACKed byte sets AF, BTF 0, TxE kept and a DR write unsent, inferred")
{
  rig r{"parked"};
  r.target->nack_byte = 1;
  r.write(i2c1 + cr2, freq2 | iterren);
  r.transmitting();
  r.cpu_write(i2c1 + dr, 0x11);
  r.idle_to(r.now_ps() + phase(9));
  CHECK((r.read(nvic_ispr1) & error_irq) == 0);
  r.cpu_write(i2c1 + dr, 0x22);
  r.idle_to(r.now_ps() + phase(9));
  CHECK(r.read(i2c1 + sr1) == (txe | af));
  CHECK((r.read(nvic_ispr1) & error_irq) == error_irq);
  r.cpu_write(i2c1 + dr, 0x33);
  r.idle_to(r.now_ps() + phase(20));
  CHECK(r.target->bytes() == std::vector<std::uint8_t>{0x11, 0x22});
  r.cpu_write(i2c1 + cr1, pe | stop);
  r.idle_to(r.now_ps() + phase(1));
  CHECK(r.target->kinds() == "SWWP");
  CHECK(r.read(i2c1 + sr1) == af);
  CHECK(r.read(i2c1 + sr2) == 0);
}

TEST_CASE("RM0038 26.6.6: SR1 error flags clear by a write of 0, a write of 1 keeps them")
{
  rig r{"parked"};
  static_cast<void>(r.addressed(0x51));
  CHECK(r.read(i2c1 + sr1) == af);
  r.cpu_write(i2c1 + sr1, af);
  CHECK(r.read(i2c1 + sr1) == af);
  r.cpu_write(i2c1 + sr1, ~af);
  CHECK(r.read(i2c1 + sr1) == 0);
}

TEST_CASE(
    "RM0038 26.3.3: a repeated START clears TRA, the STOP reaches every target of the transfer")
{
  auto other = std::make_shared<probe_target>(0x51);
  rig  r{"parked", {other}};
  r.transmitting();
  r.cpu_write(i2c1 + dr, 0x11);
  r.idle_to(r.now_ps() + phase(9));
  auto const t0 = r.now_ps();
  r.cpu_write(i2c1 + cr1, pe | start);
  r.idle_to(t0 + phase(1));
  CHECK(r.read(i2c1 + sr1) == sb);
  CHECK(r.read(i2c1 + sr2) == (msl | busy));
  static_cast<void>(r.cpu_read(i2c1 + sr1));
  r.cpu_write(i2c1 + dr, 0x51 << 1);
  r.idle_to(r.now_ps() + phase(9));
  static_cast<void>(r.cpu_read(i2c1 + sr1));
  static_cast<void>(r.cpu_read(i2c1 + sr2));
  r.cpu_write(i2c1 + dr, 0x22);
  r.idle_to(r.now_ps() + phase(9));
  r.cpu_write(i2c1 + cr1, pe | stop);
  r.idle_to(r.now_ps() + phase(1));
  CHECK(r.target->kinds() == "SWP");
  CHECK(other->kinds() == "SWP");
  CHECK(other->bytes() == std::vector<std::uint8_t>{0x22});
}

TEST_CASE("RM0038 Table 139: SB and BTF on ITEVTEN, TxE only with ITBUFEN, AF on ITERREN")
{
  constexpr address nvic_icpr0 = 0xE000'E280;
  rig               r{"parked"};
  r.write(i2c1 + cr2, freq2 | itevten | iterren);
  static_cast<void>(r.started());
  CHECK((r.read(nvic_ispr0) & event_irq) == event_irq);
  static_cast<void>(r.cpu_read(i2c1 + sr1));
  r.cpu_write(i2c1 + dr, device << 1);
  r.write(nvic_icpr0, event_irq);
  CHECK((r.read(nvic_ispr0) & event_irq) == 0);
  r.idle_to(r.now_ps() + phase(9));
  CHECK((r.read(nvic_ispr0) & event_irq) == event_irq);
  static_cast<void>(r.cpu_read(i2c1 + sr1));
  static_cast<void>(r.cpu_read(i2c1 + sr2));
  r.write(nvic_icpr0, event_irq);
  CHECK(r.read(i2c1 + sr1) == txe);
  CHECK((r.read(nvic_ispr0) & event_irq) == 0);
  r.write(i2c1 + cr2, freq2 | itevten | itbufen | iterren);
  CHECK((r.read(nvic_ispr0) & event_irq) == event_irq);
  r.write(i2c1 + cr2, freq2 | itevten | iterren);
  r.write(nvic_icpr0, event_irq);
  r.cpu_write(i2c1 + dr, 0x11);
  r.idle_to(r.now_ps() + phase(9));
  CHECK(r.read(i2c1 + sr1) == (txe | btf));
  CHECK((r.read(nvic_ispr0) & event_irq) == event_irq);
  CHECK((r.read(nvic_ispr1) & error_irq) == 0);
}

TEST_CASE("a target's stretch_ps holds SCL after its byte, the next phase starts after it")
{
  rig r{"parked"};
  r.target->stretch_ps = 10'000'000;
  r.transmitting();
  auto const t0 = r.now_ps();
  r.cpu_write(i2c1 + dr, 0x11);
  r.cpu_write(i2c1 + dr, 0x22);
  r.idle_to(t0 + phase(9) + 5'000'000);
  CHECK(r.target->bytes().empty());
  r.idle_to(t0 + 2 * phase(9) + 20'000'000);
  REQUIRE(r.target->bytes().size() == 2);
  CHECK(r.target->log[1].at == t0 + phase(9) + 10'000'000);
  CHECK(r.target->log[2].at == t0 + 2 * phase(9) + 20'000'000);
}

TEST_CASE("PE 0 mid-byte, which RM0038 26.6.1 forbids in master mode: popcorn ends it at once")
{
  rig r{"parked"};
  r.transmitting();
  r.cpu_write(i2c1 + dr, 0x11);
  r.cpu_write(i2c1 + cr1, pe | stop);
  r.cpu_write(i2c1 + cr1, stop);
  CHECK(r.read(i2c1 + cr1) == 0);
  CHECK(r.read(i2c1 + sr1) == 0);
  CHECK(r.read(i2c1 + sr2) == 0);
  CHECK(r.warnings("PE 0 during a controller transfer, which RM0038 26.6.1 forbids") == 1);
  r.idle_to(r.now_ps() + phase(20));
  CHECK(r.target->kinds() == "S");
  r.cpu_write(i2c1 + cr1, pe);
  r.cpu_write(i2c1 + cr1, 0);
  r.cpu_write(i2c1 + cr1, pe);
  static_cast<void>(r.addressed());
  CHECK(r.target->kinds() == "SS");
  r.cpu_write(i2c1 + cr1, 0);
  CHECK(r.warnings("PE 0 during a controller transfer") == 1);
}

TEST_CASE("RM0038 26.6.1 SWRST: SWRST 1 resets the I2C registers and keeps SWRST")
{
  rig r{"parked"};
  static_cast<void>(r.addressed());
  r.cpu_write(i2c1 + cr1, pe | swrst);
  CHECK(r.read(i2c1 + cr1) == swrst);
  CHECK(r.read(i2c1 + cr2) == 0);
  CHECK(r.read(i2c1 + ccr) == 0);
  CHECK(r.read(i2c1 + trise) == 2);
  CHECK(r.read(i2c1 + sr1) == 0);
  CHECK(r.read(i2c1 + sr2) == 0);
  r.cpu_write(i2c1 + cr1, 0);
  CHECK(r.read(i2c1 + cr1) == 0);
}

TEST_CASE("RM0038 26.6.2 and 26.6.9: FREQ, CCR and TRISE are checked once at the first START")
{
  rig r{"parked"};
  r.write(i2c1 + cr1, 0);
  r.write(i2c1 + cr2, 8);
  r.write(i2c1 + ccr, 3);
  r.write(i2c1 + trise, 5);
  r.write(i2c1 + cr1, pe);
  static_cast<void>(r.started());
  r.cpu_write(i2c1 + cr1, pe | stop);
  r.idle_to(r.now_ps() + phase(1, 6));
  r.cpu_write(i2c1 + cr1, pe | start);
  r.idle_to(r.now_ps() + phase(1, 6));
  CHECK(r.warnings("FREQ 8 for a PCLK1 of 2097152 Hz, RM0038 26.6.2 asks 2") == 1);
  CHECK(r.warnings("CCR 3 below the minimum 4 of RM0038 26.6.8") == 1);
  CHECK(r.warnings("TRISE 5 below the 9 RM0038 26.6.9 asks for FREQ 8 in Sm mode") == 1);
  CHECK(r.warnings("below the 2 MHz") == 0);
  CHECK(r.read(i2c1 + sr1) == sb);
}

TEST_CASE("RM0038 26.3.3: a PCLK1 below 4 MHz in Fm mode and TRISE in Fm mode warn")
{
  rig r{"parked"};
  r.write(i2c1 + cr1, 0);
  r.write(i2c1 + ccr, fast | 10);
  r.write(i2c1 + trise, 1);
  r.write(i2c1 + cr1, pe);
  r.cpu_write(i2c1 + cr1, pe | start);
  CHECK(r.warnings("a PCLK1 of 2097152 Hz, below the 4 MHz RM0038 26.3.3 asks in Fm mode") == 1);
  CHECK(r.warnings("TRISE") == 0);
  CHECK(r.warnings("FREQ") == 0);
}

TEST_CASE("unmodelled CR1 and CR2 functions each warn once")
{
  rig r{"parked"};
  r.write(i2c1 + cr2, freq2 | dmaen);
  static_cast<void>(r.addressed(device, true, pe | smbus));
  CHECK(r.target->log[0].byte == 1);
  CHECK(r.read(i2c1 + sr2) == (msl | busy));
  static_cast<void>(r.cpu_read(i2c1 + sr1));
  static_cast<void>(r.cpu_read(i2c1 + sr2));
  CHECK(r.read(i2c1 + sr1) == 0);
  CHECK(r.warnings("SMBus, PEC, ALERT or ARP: not modelled") == 1);
  CHECK(r.warnings("the DMA requests of CR2.DMAEN: not modelled") == 1);
  CHECK(r.warnings("not modelled") == 2);
}

TEST_CASE("an error a target injects warns once, the byte completes as its acknowledge says")
{
  rig r{"parked"};
  r.target->error = periph::i2c_error::bus_error;
  static_cast<void>(r.addressed());
  CHECK(r.read(i2c1 + sr1) == addr);
  CHECK(r.warnings("not modelled, the byte completes as its acknowledge says") == 1);
}

TEST_CASE("a stopped PCLK1 starts no START and warns once")
{
  rig         r{"parked"};
  auto* const probe = r.f->model_of<bus_probe>("DAC");
  REQUIRE(probe != nullptr);
  probe->context.clock.set_hz(probe->context.clock.domain("pclk1"), 0);
  r.cpu_write(i2c1 + cr1, pe | start);
  r.cpu_write(i2c1 + cr1, pe | start);
  CHECK(r.read(i2c1 + sr1) == 0);
  CHECK(r.warnings("the I2C bus clock PCLK1 is stopped") == 1);
  CHECK(r.warnings("FREQ") == 0);
  CHECK(r.warnings("below the") == 0);
}

TEST_CASE("each target is attached at construction and detached at destruction")
{
  auto target = std::shared_ptr<probe_target>{};
  {
    rig r{"parked"};
    target = r.target;
    CHECK(target->attaches == 1);
    CHECK(target->detaches == 0);
  }
  CHECK(target->detaches == 1);
}

TEST_CASE("a target leaving its address answer pending fails the run, the phase takes a NACK")
{
  rig r{"parked"};
  r.target->defer = true;
  static_cast<void>(r.started());
  static_cast<void>(r.cpu_read(i2c1 + sr1));
  r.cpu_write(i2c1 + dr, device << 1);
  auto const refused = refusal([&] {
    static_cast<void>(r.f->target->run_slice(
        {.max_instructions = icount{1'000'000}, .until_time = vtime{r.now_ps() + phase(10)}}));
  });
  CHECK(refused.contains(
      "an answer at the phase's end from the target at 0x50, the STM32L1 I2C completes no "
      "deferred phase"));
  CHECK(r.target->kinds() == "D");
  CHECK(r.read(i2c1 + sr1) == af);
}

TEST_CASE("i2c_target_port::complete() and complete_read() throw config_error on the L1 I2C")
{
  rig         r{"parked"};
  auto* const port = dynamic_cast<periph::i2c_target_port*>(r.f->target->model("I2C1"));
  REQUIRE(port != nullptr);
  auto const at = vtime{r.now_ps()};
  CHECK(refusal([&] {
          port->complete(1, {}, at, 1);
        }).contains("the STM32L1 I2C completes no deferred phase"));
  CHECK(refusal([&] {
          port->complete_read(1, {}, at, 1);
        }).contains("the STM32L1 I2C completes no deferred phase"));
  CHECK(dynamic_cast<periph::i2c_controller_port*>(r.f->target->model("I2C1")) != nullptr);
}

class phase_probe final : public periph::i2c_controller_sink
{
public:
  struct heard
  {
    std::uint64_t     unit;
    periph::i2c_phase phase;
    bool              ack;
    std::uint8_t      byte;
    std::uint64_t     at;

    friend auto operator==(heard const&, heard const&) -> bool = default;
  };

  struct lost
  {
    std::uint64_t     unit;
    periph::i2c_phase phase;
    std::uint64_t     at;

    friend auto operator==(lost const&, lost const&) -> bool = default;
  };

  std::vector<heard> answers;
  std::vector<lost>  drops;

  void answered(std::uint64_t unit, periph::i2c_phase phase, periph::i2c_answer answer, vtime at)
      override
  {
    answers.push_back({unit, phase, answer.ack, answer.byte, at.ps});
  }

  void dropped(std::uint64_t unit, periph::i2c_phase phase, vtime at) override
  {
    drops.push_back({unit, phase, at.ps});
  }
};

using heard = phase_probe::heard;
using lost  = phase_probe::lost;

constexpr std::uint32_t     lane       = 1;
constexpr std::uint64_t     us         = 1'000'000;
constexpr std::uint64_t     target_ps  = 90 * us;
constexpr periph::i2c_phase on_address = periph::i2c_phase::address;
constexpr periph::i2c_phase on_write   = periph::i2c_phase::write;
constexpr periph::i2c_phase on_read    = periph::i2c_phase::read;

struct port_rig
{
  std::shared_ptr<phase_probe>   sink = std::make_shared<phase_probe>();
  stm32::board                   board{};
  std::unique_ptr<stm32_fixture> f;

  explicit port_rig(bool controlled = true)
  {
    if(controlled)
    {
      board.i2c_controllers["I2C1"] = sink;
    }
    auto options   = popcorn::testing::stm32_options();
    options.models = stm32::factories(board, l152re());
    f              = std::make_unique<stm32_fixture>(l152re(), firmware, std::move(options));
    f->set("pc", sym(firmware, "idle"));
    REQUIRE(f->target->mem().write(apb1enr, 4, i2c1_bit).has_value());
  }

  [[nodiscard]] auto port() const -> periph::i2c_controller_port&
  {
    auto* const found = dynamic_cast<periph::i2c_controller_port*>(f->target->model("I2C1"));
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto now() const -> std::uint64_t
  {
    return f->target->virtual_clock().time().ps;
  }

  void run_until(std::uint64_t ps) const
  {
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{1'000'000}, .until_time = vtime{ps}}).reason
        == stop_reason::time);
  }

  void rcc_pulse() const
  {
    REQUIRE(f->target->mem().write(apb1rstr, 4, i2c1_bit).has_value());
    REQUIRE(f->target->mem().write(apb1rstr, 4, 0).has_value());
  }

  void start(std::uint8_t to, bool read, std::uint64_t at, std::uint64_t unit) const
  {
    port().start(to, read, target_ps, vtime{at}, lane, unit);
  }

  void write(std::uint8_t byte, std::uint64_t at, std::uint64_t unit) const
  {
    port().write(byte, target_ps, vtime{at}, lane, unit);
  }

  void read(bool ack, std::uint64_t at, std::uint64_t unit) const
  {
    port().read(ack, target_ps, vtime{at}, lane, unit);
  }

  void stop(std::uint64_t at, std::uint64_t unit) const
  {
    port().stop(vtime{at}, lane, unit);
  }
};

TEST_CASE(
    "a board.i2c_controllers entry gives the L1 I2C a controller port, refused next to a target")
{
  port_rig const r;
  CHECK(r.port().last_unit() == 0);
  CHECK(r.port().queued() == 0);
  stm32::board board{};
  board.i2c_controllers["I2C1"] = std::make_shared<phase_probe>();
  board.i2c["I2C1"].push_back(std::make_shared<probe_target>());
  auto options   = popcorn::testing::stm32_options();
  options.models = stm32::factories(board, l152re());
  CHECK(refusal([&] {
          stm32_fixture{l152re(), firmware, std::move(options)};
        }).contains("an I2C controller alone on the instance, no target next to it"));
}

TEST_CASE("port phases end one after the other: address and written bytes NACK, a read byte 0xFF")
{
  port_rig const r;
  auto const     base = r.now() + us;
  r.start(0x42, false, base, 1);
  r.write(0x11, base, 2);
  r.read(true, base, 3);
  r.stop(base + 3 * target_ps, 4);
  r.read(false, base + 4 * target_ps, 5);
  CHECK(r.port().queued() == 5);
  r.run_until(base + 6 * target_ps);
  CHECK(
      r.sink->answers
      == std::vector<heard>{
          {1, on_address, false, 0xFF, base + target_ps    },
          {2, on_write,   false, 0xFF, base + 2 * target_ps},
          {3, on_read,    true,  0xFF, base + 3 * target_ps},
          {5, on_read,    false, 0xFF, base + 5 * target_ps},
  });
  CHECK(r.sink->drops.empty());
  CHECK(r.port().queued() == 0);
  CHECK(r.port().last_unit() == 5);
}

TEST_CASE("port phases queued back to back each start at the end of the one before")
{
  port_rig const r;
  auto const     base = r.now() + us;
  r.start(0x42, true, base, 1);
  r.read(true, base, 2);
  r.read(false, base, 3);
  r.stop(base + 3 * target_ps, 4);
  r.run_until(base + 4 * target_ps);
  CHECK(
      r.sink->answers
      == std::vector<heard>{
          {1, on_address, false, 0xFF, base + target_ps    },
          {2, on_read,    true,  0xFF, base + 2 * target_ps},
          {3, on_read,    false, 0xFF, base + 3 * target_ps},
  });
  CHECK(r.sink->drops.empty());
}

TEST_CASE("checklist 49: a port entry in the past keeps its own time, a bad argument throws")
{
  port_rig const r;
  r.run_until(r.now() + 10 * us);
  auto const past = r.now() - 3 * us;
  r.start(0x42, false, past, 1);
  r.port().drop(on_write, vtime{past}, lane, 2);
  r.run_until(r.now() + target_ps + us);
  CHECK(
      r.sink->answers
      == std::vector<heard>{
          {1, on_address, false, 0xFF, past + target_ps}
  });
  CHECK(
      r.sink->drops
      == std::vector<lost>{
          {2, on_write, past}
  });
  auto const at = r.now();
  CHECK_THROWS_AS(r.port().start(0x42, false, target_ps, vtime{at}, 0, 3), std::invalid_argument);
  CHECK_THROWS_AS(
      r.port().start(0x42, false, target_ps, vtime{at}, lane, 2), std::invalid_argument);
  CHECK_THROWS_AS(
      r.port().start(0x80, false, target_ps, vtime{at}, lane, 3), std::invalid_argument);
  CHECK_THROWS_AS(r.port().write(0x11, 0, vtime{at}, lane, 3), std::invalid_argument);
  CHECK_THROWS_AS(
      r.port().read(true, std::numeric_limits<std::uint64_t>::max(), vtime{at}, lane, 3),
      std::invalid_argument);
  CHECK_THROWS_AS(
      r.port().drop(static_cast<periph::i2c_phase>(3), vtime{at}, lane, 3), std::invalid_argument);
  CHECK_THROWS_AS(r.port().stop(vtime{at}, 0, 3), std::invalid_argument);
  CHECK(r.port().queued() == 0);
  CHECK(r.port().last_unit() == 2);
  port_rig const bare{false};
  CHECK(refusal([&] {
          bare.port().stop(vtime{bare.now()}, lane, 1);
        }).contains("an I2C controller attached to the instance"));
}

TEST_CASE("a STOP drops the lower units still queued or running at its time, later units run")
{
  port_rig const r;
  auto const     base = r.now() + us;
  r.start(0x42, false, base, 1);
  r.write(0x11, base, 2);
  r.stop(base + target_ps / 2, 3);
  r.start(0x42, false, base + target_ps, 4);
  r.run_until(base + 3 * target_ps);
  CHECK(
      r.sink->drops
      == std::vector<lost>{
          {1, on_address, base + target_ps / 2},
          {2, on_write,   base + target_ps / 2},
  });
  CHECK(
      r.sink->answers
      == std::vector<heard>{
          {4, on_address, false, 0xFF, base + 2 * target_ps}
  });
}

TEST_CASE("checklist 31: I2C1RST drops the phase started before it at its end, later phases run")
{
  port_rig const r;
  auto const     base = r.now() + us;
  r.start(0x42, false, base, 1);
  r.write(0x11, base, 2);
  r.run_until(base + target_ps / 2);
  auto const generation = popcorn::testing::saved_integer(*r.f, "I2C1", "port.generation");
  REQUIRE(generation.has_value());
  r.rcc_pulse();
  CHECK(popcorn::testing::saved_integer(*r.f, "I2C1", "port.generation") == *generation + 1);
  r.start(0x42, false, base, 3);
  r.run_until(base + 4 * target_ps);
  CHECK(
      r.sink->drops
      == std::vector<lost>{
          {1, on_address, base + target_ps}
  });
  CHECK(
      r.sink->answers
      == std::vector<heard>{
          {2, on_write,   false, 0xFF, base + 2 * target_ps},
          {3, on_address, false, 0xFF, base + 3 * target_ps},
  });
}

TEST_CASE("checklist 31: CR1.SWRST resets the port as I2C1RST does")
{
  port_rig const r;
  auto const     base = r.now() + us;
  r.start(0x42, false, base, 1);
  r.write(0x11, base, 2);
  r.run_until(base + target_ps / 2);
  auto const generation = popcorn::testing::saved_integer(*r.f, "I2C1", "port.generation");
  REQUIRE(generation.has_value());
  REQUIRE(r.f->target->mem().write(i2c1 + cr1, 4, swrst).has_value());
  REQUIRE(r.f->target->mem().write(i2c1 + cr1, 4, 0).has_value());
  CHECK(popcorn::testing::saved_integer(*r.f, "I2C1", "port.generation") == *generation + 1);
  r.start(0x42, false, base, 3);
  r.run_until(base + 4 * target_ps);
  CHECK(
      r.sink->drops
      == std::vector<lost>{
          {1, on_address, base + target_ps}
  });
  CHECK(
      r.sink->answers
      == std::vector<heard>{
          {2, on_write,   false, 0xFF, base + 2 * target_ps},
          {3, on_address, false, 0xFF, base + 3 * target_ps},
  });
}

TEST_CASE("a drop posted before the last I2C1RST is reported at the reset")
{
  port_rig const r;
  auto const     base = r.now() + us;
  r.run_until(base + target_ps / 2);
  r.rcc_pulse();
  auto const at_reset = r.now();
  r.port().drop(on_write, vtime{base}, lane, 1);
  r.run_until(at_reset + us);
  CHECK(
      r.sink->drops
      == std::vector<lost>{
          {1, on_write, at_reset}
  });
}

TEST_CASE("entries due at one time run lane by lane, a lower lane first whatever the unit")
{
  port_rig const r;
  auto const     base = r.now() + us;
  r.port().drop(on_write, vtime{base}, 2, 1);
  r.port().drop(on_read, vtime{base}, 1, 2);
  r.run_until(base + us);
  CHECK(
      r.sink->drops
      == std::vector<lost>{
          {2, on_read,  base},
          {1, on_write, base},
  });
}

TEST_CASE("after a machine reset a phase posted at 0 starts at the dropped phase's rebased end")
{
  port_rig const r;
  auto const     base = r.now() + us;
  r.start(0x42, false, base, 1);
  r.write(0x11, base, 2);
  r.run_until(base + target_ps + target_ps / 2);
  auto const at_reset = r.now();
  r.f->target->reset(popcorn::reset_kind::software);
  r.start(0x42, false, 0, 3);
  r.run_until(base + 4 * target_ps - at_reset);
  CHECK(
      r.sink->drops
      == std::vector<lost>{
          {2, on_write, base + 2 * target_ps - at_reset}
  });
  CHECK(
      r.sink->answers
      == std::vector<heard>{
          {1, on_address, false, 0xFF, base + target_ps               },
          {3, on_address, false, 0xFF, base + 3 * target_ps - at_reset},
  });
}

TEST_CASE("after I2C1RST a phase starting at or before it drops at its end, a STOP is due at it")
{
  port_rig const r;
  auto const     base = r.now() + us;
  r.run_until(base + target_ps / 2);
  r.rcc_pulse();
  auto const at_reset = r.now();
  r.start(0x42, false, base, 1);
  r.write(0x11, base, 2);
  r.stop(base, 3);
  r.start(0x42, false, at_reset + us, 4);
  r.run_until(at_reset + us + 2 * target_ps);
  CHECK(
      r.sink->drops
      == std::vector<lost>{
          {2, on_write,   at_reset        },
          {1, on_address, base + target_ps},
  });
  CHECK(
      r.sink->answers
      == std::vector<heard>{
          {4, on_address, false, 0xFF, at_reset + us + target_ps}
  });
}

TEST_CASE("a machine reset drops every queued phase rebased, removes queued STOPs, keeps the unit")
{
  port_rig const r;
  auto const     base = r.now() + us;
  r.start(0x42, false, base, 1);
  r.write(0x11, base, 2);
  r.stop(base + 4 * target_ps, 3);
  r.run_until(base + target_ps / 2);
  auto const at_reset = r.now();
  r.f->target->reset(popcorn::reset_kind::software);
  r.run_until(base + 3 * target_ps);
  CHECK(
      r.sink->drops
      == std::vector<lost>{
          {1, on_address, base + target_ps - at_reset    },
          {2, on_write,   base + 2 * target_ps - at_reset},
  });
  CHECK(r.sink->answers.empty());
  CHECK(r.port().queued() == 0);
  CHECK_THROWS_AS(r.port().drop(on_write, vtime{0}, lane, 3), std::invalid_argument);
  r.port().drop(on_write, vtime{0}, lane, 4);
  CHECK(popcorn::testing::saved_integer(*r.f, "I2C1", "port.last_unit") == 4);
}

TEST_CASE("checklist 6: 1000 port drops leave no entry, the port holds at most max_entries")
{
  port_rig const r;
  std::uint64_t  unit = 0;
  for(int round = 0; round < 1000; ++round)
  {
    r.port().drop(on_write, vtime{r.now()}, lane, ++unit);
    r.run_until(r.now() + us);
  }
  CHECK(r.sink->drops.size() == 1000);
  CHECK(r.port().queued() == 0);
  auto const at = vtime{r.now() + us};
  for(std::size_t index = 0; index < periph::i2c_controller_port::max_entries; ++index)
  {
    r.port().drop(on_write, at, lane, ++unit);
  }
  CHECK(refusal([&] {
          r.port().drop(on_write, at, lane, unit + 1);
        }).contains("at most 4096 I2C port entries queued"));
  r.run_until(at.ps + us);
  CHECK(r.port().queued() == 0);
}

TEST_CASE("a snapshot holding port entries restores them, an I2C with no controller refuses it")
{
  port_rig const r;
  auto const     base = r.now() + us;
  r.start(0x42, false, base, 1);
  r.write(0x11, base, 2);
  r.run_until(base + target_ps / 2);
  auto const     saved   = r.f->target->snapshot();
  auto const     encoded = core::encode_snapshot(saved.data());
  port_rig const other;
  other.f->target->restore(core::snapshot{core::decode_snapshot(encoded)});
  CHECK(other.port().queued() == 2);
  CHECK(other.port().last_unit() == 2);
  for(auto const* one : {&r, &other})
  {
    one->run_until(base + 3 * target_ps);
    CHECK(
        one->sink->answers
        == std::vector<heard>{
            {1, on_address, false, 0xFF, base + target_ps    },
            {2, on_write,   false, 0xFF, base + 2 * target_ps},
    });
  }
  port_rig const bare{false};
  CHECK(refusal([&] {
          bare.f->target->restore(core::snapshot{core::decode_snapshot(encoded)});
        }).contains("the snapshot holds port entries"));
}

TEST_CASE("a snapshot mid-byte restores the byte in flight, the targets and the transfer")
{
  rig r{"parked"};
  r.transmitting();
  auto const t0 = r.now_ps();
  r.cpu_write(i2c1 + dr, 0x11);
  r.cpu_write(i2c1 + dr, 0x22);
  r.idle_to(t0 + phase(4));
  auto const saved = r.f->target->snapshot();
  rig        other{"parked"};
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.target->kinds() == "S");
  for(auto const* one : {&r, &other})
  {
    one->idle_to(t0 + phase(18));
    one->cpu_write(i2c1 + cr1, pe | stop);
    one->idle_to(t0 + phase(19));
    CHECK(one->target->bytes() == std::vector<std::uint8_t>{0x11, 0x22});
    CHECK(one->target->kinds() == "SWWP");
    CHECK(one->read(i2c1 + sr2) == 0);
  }
  CHECK(other.target->log[1].at == t0 + phase(9));
}

TEST_CASE(
    "checklist 31: an I2C1RST pulse drops the byte in flight, new phases keep their own times")
{
  rig r{"parked"};
  r.transmitting();
  auto const t0 = r.now_ps();
  r.cpu_write(i2c1 + dr, 0x11);
  r.idle_to(t0 + phase(2));
  r.write(apb1rstr, i2c1_bit);
  r.write(apb1rstr, 0);
  CHECK(r.read(i2c1 + cr1) == 0);
  CHECK(r.read(i2c1 + sr2) == 0);
  r.write(i2c1 + cr2, freq2);
  r.write(i2c1 + ccr, 4);
  r.write(i2c1 + trise, 3);
  r.write(i2c1 + cr1, pe);
  auto const t1 = r.now_ps();
  r.cpu_write(i2c1 + cr1, pe | start);
  r.idle_to(t1 + phase(1, 8));
  static_cast<void>(r.cpu_read(i2c1 + sr1));
  r.cpu_write(i2c1 + dr, device << 1);
  r.idle_to(t1 + phase(10, 8));
  static_cast<void>(r.cpu_read(i2c1 + sr1));
  static_cast<void>(r.cpu_read(i2c1 + sr2));
  r.cpu_write(i2c1 + dr, 0x22);
  r.cpu_write(i2c1 + dr, 0x33);
  r.idle_to(t0 + phase(9));
  CHECK(r.target->kinds() == "SS");
  CHECK(r.read(i2c1 + sr1) == 0);
  r.idle_to(t1 + phase(28, 8));
  REQUIRE(r.target->bytes() == std::vector<std::uint8_t>{0x22, 0x33});
  CHECK(r.target->log[2].at == t1 + phase(19, 8));
  CHECK(r.target->log[3].at == t1 + phase(28, 8));
}

TEST_CASE("checklist 53: under strict_peripherals SMBus, PEC, ALERT, ARP and DMAEN raise at START")
{
  for(auto const& [cr1_bits, cr2_bits] : {
          std::pair{smbus, freq2        },
          std::pair{enarp, freq2        },
          std::pair{enpec, freq2        },
          std::pair{pec,   freq2        },
          std::pair{alert, freq2        },
          std::pair{0u,    freq2 | dmaen},
  })
  {
    CAPTURE(cr1_bits);
    rig r{"parked", {}, l152re(), firmware, true};
    r.write(i2c1 + cr2, cr2_bits);
    r.write(i2c1 + cr1, pe | cr1_bits);
    CHECK_THROWS_AS(
        static_cast<void>(
            r.services().write(i2c1 + cr1, 4, pe | cr1_bits | start, popcorn::initiator::cpu)),
        popcorn::model_error);
    CHECK(r.read(i2c1 + sr2) == 0);
    CHECK(r.warnings("not modelled") == 1);
  }
}

TEST_CASE("checklist 14: target times count from power-on after a software reset")
{
  rig r{"parked"};
  r.idle_to(r.now_ps() + 1'000'000'000);
  r.f->target->reset(popcorn::reset_kind::software);
  r.f->set("pc", sym(firmware, "idle"));
  r.write(apb1enr, i2c1_bit);
  r.write(i2c1 + cr2, freq2);
  r.write(i2c1 + ccr, 10);
  r.write(i2c1 + trise, 3);
  r.write(i2c1 + cr1, pe);
  auto const end = r.addressed();
  REQUIRE(r.target->kinds() == "S");
  auto const since = r.f->target->virtual_clock().since_power_on().ps;
  CHECK(since > end);
  CHECK(r.target->log[0].at == since);
}

TEST_CASE("checklist 14: a read byte counts its time from power-on after a software reset")
{
  rig r{"parked"};
  r.target->out = {0xC1};
  r.idle_to(r.now_ps() + 1'000'000'000);
  r.f->target->reset(popcorn::reset_kind::software);
  r.f->set("pc", sym(firmware, "idle"));
  r.write(apb1enr, i2c1_bit);
  r.write(i2c1 + cr2, freq2);
  r.write(i2c1 + ccr, 10);
  r.write(i2c1 + trise, 3);
  r.write(i2c1 + cr1, pe);
  auto const t0 = r.receiving(pe);
  r.idle_to(t0 + phase(9));
  REQUIRE(r.target->kinds() == "SR");
  auto const since  = r.f->target->virtual_clock().since_power_on().ps;
  auto const offset = since - r.now_ps();
  CHECK(offset >= 1'000'000'000);
  CHECK(r.target->log[1].at == t0 + phase(9) + offset);
}

TEST_CASE("RM0038 26.6.9: a new PE period checks the timing again, TRISE above the rule passes")
{
  rig r{"parked"};
  static_cast<void>(r.started());
  r.cpu_write(i2c1 + cr1, pe | stop);
  r.idle_to(r.now_ps() + phase(1));
  CHECK(r.warnings("CCR") == 0);
  r.cpu_write(i2c1 + cr1, 0);
  r.write(i2c1 + ccr, 3);
  r.write(i2c1 + trise, 7);
  r.cpu_write(i2c1 + cr1, pe);
  r.cpu_write(i2c1 + cr1, pe | start);
  CHECK(r.warnings("CCR 3 below the minimum 4 of RM0038 26.6.8") == 1);
  CHECK(r.warnings("TRISE") == 0);
}

TEST_CASE("PE 0 drops a target's stretch, the next START runs one period after its write")
{
  rig r{"parked"};
  r.transmitting();
  r.target->stretch_ps = 50'000'000;
  r.cpu_write(i2c1 + dr, 0x11);
  r.idle_to(r.now_ps() + phase(9));
  r.cpu_write(i2c1 + cr1, 0);
  r.cpu_write(i2c1 + cr1, pe);
  auto const t0 = r.now_ps();
  r.cpu_write(i2c1 + cr1, pe | start);
  r.idle_to(t0 + phase(1));
  CHECK(r.read(i2c1 + sr1) == sb);
}

TEST_CASE("the L1 I2C offers no i2c_link_port and no i2c_bus host hold")
{
  rig         r{"parked"};
  auto* const model = r.f->target->model("I2C1");
  REQUIRE(model != nullptr);
  CHECK(dynamic_cast<stm32::i2c_link_port*>(model) == nullptr);
  CHECK(dynamic_cast<stm32::i2c_bus*>(model) == nullptr);
}

TEST_CASE("a snapshot with another number of targets is refused, a matching one restores")
{
  rig        r{"parked"};
  auto const saved = r.f->target->snapshot();
  rig        other{"parked", {std::make_shared<probe_target>(0x51)}};
  auto const own = other.f->target->snapshot();
  CHECK(refusal([&] {
          other.f->target->restore(
              core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
        }).contains("a snapshot with 1 targets, the machine has 2"));
  other.f->target->restore(own);
  static_cast<void>(other.addressed());
  CHECK(other.target->kinds() == "S");
}

TEST_CASE("RM0038 26.6.1 SWRST: while SWRST is 1 a CR1 write changes nothing but the SWRST clear")
{
  rig r{"parked"};
  r.cpu_write(i2c1 + cr1, pe | swrst);
  r.cpu_write(i2c1 + cr1, pe | start | swrst);
  CHECK(r.read(i2c1 + cr1) == swrst);
  CHECK(r.read(i2c1 + sr2) == 0);
  r.cpu_write(i2c1 + cr1, pe | start);
  CHECK(r.read(i2c1 + cr1) == 0);
  CHECK(r.read(i2c1 + sr2) == 0);
}

TEST_CASE("a 10-bit header address byte takes the NACK path and reaches no matches()")
{
  rig r{"parked"};
  static_cast<void>(r.addressed(0x78));
  CHECK(r.read(i2c1 + sr1) == af);
  CHECK(r.target->matched == 0);
  r.cpu_write(i2c1 + cr1, pe | stop);
  r.idle_to(r.now_ps() + phase(1));
  static_cast<void>(r.addressed());
  CHECK(r.target->matched == 1);
}

TEST_CASE("RM0038 26.3.3 1-byte reception: LL clears ACK in EV6, then STOP, the byte is NACKed")
{
  rig r{"read_one"};
  r.target->out = {0xC1};
  CHECK(r.finish() == std::vector<std::uint32_t>{0xC1, 0, 0});
  CHECK(r.target->kinds() == "SRP");
  CHECK(r.target->acks() == std::vector<std::uint8_t>{0});
  CHECK(std::ranges::none_of(r.logged, [](core::log_record const& one) {
    return one.peripheral == "I2C1";
  }));
}

TEST_CASE("RM0038 26.3.3 2-byte reception: LL sets POS with ACK 0, the first byte is acknowledged")
{
  rig r{"read_two"};
  r.target->out = {0xC1, 0xC2};
  CHECK(r.finish() == std::vector<std::uint32_t>{0xC1, 0xC2, 0, 0});
  CHECK(r.target->kinds() == "SRRP");
  CHECK(r.target->acks() == std::vector<std::uint8_t>{1, 0});
}

TEST_CASE("RM0038 26.3.3 N-byte reception: LL clears ACK at BTF of N-2, STOP at BTF of N-1")
{
  rig r{"read_four"};
  r.target->out = {0xC1, 0xC2, 0xC3, 0xC4};
  CHECK(r.finish() == std::vector<std::uint32_t>{0xC1, 0xC2, 0xC3, 0xC4, 0, 0});
  CHECK(r.target->kinds() == "SRRRRP");
  CHECK(r.target->acks() == std::vector<std::uint8_t>{1, 1, 1, 0});
}

TEST_CASE("RM0038 26.3.3 EV7: RxNE after 9 periods, BTF holds the next byte until a DR read")
{
  rig r{"parked"};
  r.target->out = {0xC1, 0xC2, 0xC3};
  auto const t0 = r.receiving();
  r.idle_to(t0 + phase(9) - margin);
  CHECK(r.read(i2c1 + sr1) == 0);
  CHECK(r.target->kinds() == "S");
  r.idle_to(t0 + phase(9));
  CHECK(r.read(i2c1 + sr1) == rxne);
  CHECK(r.read(i2c1 + dr) == 0xC1);
  REQUIRE(r.target->kinds() == "SR");
  CHECK(r.target->log[1].at == t0 + phase(9));
  CHECK(r.read(i2c1 + sr2) == (msl | busy));
  r.idle_to(t0 + phase(18));
  CHECK(r.read(i2c1 + sr1) == (rxne | btf));
  CHECK(r.read(i2c1 + dr) == 0xC1);
  r.idle_to(t0 + phase(40));
  CHECK(r.target->kinds() == "SRR");
  auto const t1 = r.now_ps();
  CHECK(r.cpu_read(i2c1 + dr) == 0xC1);
  CHECK(r.read(i2c1 + sr1) == rxne);
  CHECK(r.read(i2c1 + dr) == 0xC2);
  r.idle_to(t1 + phase(9));
  REQUIRE(r.target->kinds() == "SRRR");
  CHECK(r.target->log[3].at == t1 + phase(9));
  CHECK(r.read(i2c1 + sr1) == (rxne | btf));
  CHECK(r.cpu_read(i2c1 + dr) == 0xC2);
  CHECK(r.cpu_read(i2c1 + dr) == 0xC3);
  CHECK(r.read(i2c1 + sr1) == 0);
}

TEST_CASE(
    "RM0038 26.3.3: ACK is decided at the end of the eighth period, before the pulse, POS 0, a "
    "write during the pulse inferred")
{
  for(auto const& [written, acks] : {
          std::pair{phase(16) + phase(1) / 2, std::vector<std::uint8_t>{1, 0}},
          std::pair{phase(17) + phase(1) / 2, std::vector<std::uint8_t>{1, 1}}
  })
  {
    CAPTURE(written);
    rig r{"parked"};
    r.target->out = {0xC1, 0xC2};
    auto const t0 = r.receiving();
    r.idle_to(t0 + phase(9));
    CHECK(r.cpu_read(i2c1 + dr) == 0xC1);
    r.idle_to(t0 + written);
    r.cpu_write(i2c1 + cr1, pe);
    r.idle_to(t0 + phase(18));
    CHECK(r.target->acks() == acks);
  }
  {
    rig r{"parked"};
    r.target->out = {0xC1, 0xC2};
    auto const t0 = r.receiving();
    r.idle_to(t0 + phase(9));
    r.cpu_write(i2c1 + cr1, pe);
    r.idle_to(t0 + phase(18));
    CHECK(r.target->acks() == std::vector<std::uint8_t>{1, 0});
  }
}

TEST_CASE("RM0038 26.6.1 POS, inferred: with POS 1 a byte takes ACK from the phase before it")
{
  for(auto const pos : {0u, pos_bit})
  {
    CAPTURE(pos);
    rig r{"parked"};
    r.target->out = {0xC1, 0xC2};
    static_cast<void>(r.addressed(device, true, pe | ack_bit));
    r.cpu_write(i2c1 + cr1, pe | pos);
    static_cast<void>(r.cpu_read(i2c1 + sr1));
    static_cast<void>(r.cpu_read(i2c1 + sr2));
    auto const t0 = r.now_ps();
    r.idle_to(t0 + phase(18));
    if(pos != 0)
    {
      CHECK(r.target->acks() == std::vector<std::uint8_t>{1, 0});
      CHECK(r.read(i2c1 + sr1) == (rxne | btf));
    }
    else
    {
      CHECK(r.target->acks() == std::vector<std::uint8_t>{0});
      CHECK(r.read(i2c1 + sr1) == (rxne | btf));
    }
  }
}

TEST_CASE("RM0038 26.3.3 closing: a byte after the NACKed one reads 0xFF and reaches no target")
{
  rig r{"parked"};
  r.target->out = {0xC1, 0xC2};
  auto const t0 = r.receiving(pe);
  r.idle_to(t0 + phase(18));
  CHECK(r.target->kinds() == "SR");
  CHECK(r.target->acks() == std::vector<std::uint8_t>{0});
  CHECK(r.read(i2c1 + sr1) == (rxne | btf));
  auto const t1 = r.now_ps();
  r.cpu_write(i2c1 + cr1, pe | stop);
  r.idle_to(t1 + phase(1));
  CHECK(r.target->kinds() == "SRP");
  CHECK(r.target->log.back().at == t1 + phase(1));
  CHECK(r.read(i2c1 + sr2) == 0);
  CHECK(r.read(i2c1 + sr1) == (rxne | btf));
  CHECK(r.cpu_read(i2c1 + dr) == 0xC1);
  CHECK(r.read(i2c1 + sr1) == rxne);
  CHECK(r.cpu_read(i2c1 + dr) == 0xFF);
  CHECK(r.read(i2c1 + sr1) == 0);
  r.idle_to(r.now_ps() + phase(20));
  CHECK(r.target->kinds() == "SRP");
}

TEST_CASE("RM0038 26.6.1 STOP in reception: after the byte that shifts, the byte goes to DR")
{
  rig r{"parked"};
  r.target->out = {0xC1, 0xC2};
  auto const t0 = r.receiving(pe);
  r.idle_to(t0 + phase(4));
  r.cpu_write(i2c1 + cr1, pe | stop);
  r.idle_to(t0 + phase(10) - margin);
  CHECK(r.target->kinds() == "SR");
  r.idle_to(t0 + phase(10));
  CHECK(r.target->kinds() == "SRP");
  CHECK(r.target->log.back().at == t0 + phase(10));
  CHECK(r.read(i2c1 + sr1) == rxne);
  CHECK(r.read(i2c1 + sr2) == 0);
  CHECK(r.read(i2c1 + dr) == 0xC1);
  r.idle_to(t0 + phase(40));
  CHECK(r.target->kinds() == "SRP");
}

TEST_CASE("RM0038 26.6.6 BTF: a repeated START while SCL is held keeps BTF, no byte follows")
{
  rig r{"parked"};
  r.target->out = {0xC1, 0xC2};
  auto const t0 = r.receiving();
  r.idle_to(t0 + phase(18));
  auto const t1 = r.now_ps();
  r.cpu_write(i2c1 + cr1, pe | ack_bit | start);
  r.idle_to(t1 + phase(1));
  CHECK(r.read(i2c1 + sr1) == (sb | rxne | btf));
  CHECK(r.read(i2c1 + sr2) == (msl | busy));
  CHECK(r.cpu_read(i2c1 + dr) == 0xC1);
  CHECK(r.read(i2c1 + sr1) == (sb | rxne));
  r.idle_to(r.now_ps() + phase(20));
  CHECK(r.target->kinds() == "SRR");
  CHECK(r.cpu_read(i2c1 + dr) == 0xC2);
  CHECK(r.read(i2c1 + sr1) == sb);
}

TEST_CASE("RM0038 26.3.3: an ADDR clear into a read starts no byte while BTF holds the last one")
{
  rig r{"parked"};
  r.target->out = {0xC1, 0xC2, 0xD1};
  auto const t0 = r.receiving();
  r.idle_to(t0 + phase(18));
  auto const t1 = r.now_ps();
  r.cpu_write(i2c1 + cr1, pe | ack_bit | start);
  r.idle_to(t1 + phase(1));
  static_cast<void>(r.cpu_read(i2c1 + sr1));
  auto const t2 = r.now_ps();
  r.cpu_write(i2c1 + dr, device << 1 | 1u);
  r.idle_to(t2 + phase(9));
  CHECK(r.read(i2c1 + sr1) == (addr | rxne | btf));
  static_cast<void>(r.cpu_read(i2c1 + sr1));
  static_cast<void>(r.cpu_read(i2c1 + sr2));
  r.idle_to(r.now_ps() + phase(20));
  CHECK(r.target->kinds() == "SRRS");
  CHECK(r.read(i2c1 + sr1) == (rxne | btf));
  auto const t3 = r.now_ps();
  static_cast<void>(r.cpu_read(i2c1 + dr));
  CHECK(r.read(i2c1 + sr1) == rxne);
  CHECK(r.cpu_read(i2c1 + dr) == 0xC2);
  r.idle_to(t3 + phase(9));
  REQUIRE(r.target->kinds() == "SRRSR");
  CHECK(r.target->log[4].at == t3 + phase(9));
  CHECK(r.cpu_read(i2c1 + dr) == 0xD1);
}

TEST_CASE(
    "RM0038 26.6.6 RxNE: a DR write clears RxNE, with BTF the held byte replaces it, inferred")
{
  rig r{"parked"};
  r.target->out = {0xC1, 0xC2, 0xC3};
  auto const t0 = r.receiving();
  r.idle_to(t0 + phase(9));
  r.cpu_write(i2c1 + dr, 0x5A);
  CHECK(r.read(i2c1 + sr1) == 0);
  r.idle_to(t0 + phase(18));
  CHECK(r.read(i2c1 + sr1) == rxne);
  CHECK(r.read(i2c1 + dr) == 0xC2);
  r.idle_to(t0 + phase(27));
  CHECK(r.read(i2c1 + sr1) == (rxne | btf));
  r.cpu_write(i2c1 + dr, 0x5A);
  CHECK(r.read(i2c1 + sr1) == rxne);
  CHECK(r.read(i2c1 + dr) == 0xC3);
}

TEST_CASE("RM0038 Table 139: RxNE raises the event line only with ITBUFEN, BTF with ITEVTEN")
{
  constexpr address nvic_icpr0 = 0xE000'E280;
  rig               r{"parked"};
  r.write(i2c1 + cr2, freq2 | itevten);
  auto const t0 = r.receiving();
  r.write(nvic_icpr0, event_irq);
  r.idle_to(t0 + phase(9));
  CHECK(r.read(i2c1 + sr1) == rxne);
  CHECK((r.read(nvic_ispr0) & event_irq) == 0);
  r.write(i2c1 + cr2, freq2 | itevten | itbufen);
  CHECK((r.read(nvic_ispr0) & event_irq) == event_irq);
  r.write(i2c1 + cr2, freq2 | itevten);
  r.write(nvic_icpr0, event_irq);
  r.idle_to(t0 + phase(18));
  CHECK(r.read(i2c1 + sr1) == (rxne | btf));
  CHECK((r.read(nvic_ispr0) & event_irq) == event_irq);
}

TEST_CASE("the bytes of two targets on one address combine by AND, a read stretch holds SCL")
{
  auto other = std::make_shared<probe_target>();
  rig  r{"parked", {other}};
  r.target->out     = {0x3C, 0xFF};
  other->out        = {0x0F, 0xFF};
  auto const t0     = r.receiving();
  other->stretch_ps = 10'000'000;
  r.idle_to(t0 + phase(9));
  CHECK(r.read(i2c1 + dr) == 0x0C);
  CHECK(r.target->kinds() == "SR");
  CHECK(other->kinds() == "SR");
  r.idle_to(t0 + phase(18) + 10'000'000 - margin);
  CHECK(r.target->kinds() == "SR");
  r.idle_to(t0 + phase(18) + 10'000'000);
  REQUIRE(r.target->kinds() == "SRR");
  CHECK(r.target->log[2].at == t0 + phase(18) + 10'000'000);
}

TEST_CASE(
    "PE 0 while SCL is held, which RM0038 26.6.1 forbids in master mode: popcorn drops RxNE, BTF "
    "and the held byte")
{
  rig r{"parked"};
  r.target->out = {0xC1, 0xC2, 0xD1};
  auto const t0 = r.receiving();
  r.idle_to(t0 + phase(18));
  r.cpu_write(i2c1 + cr1, 0);
  CHECK(r.read(i2c1 + sr1) == 0);
  CHECK(r.read(i2c1 + sr2) == 0);
  r.cpu_write(i2c1 + cr1, pe);
  auto const t1 = r.receiving();
  r.idle_to(t1 + phase(9));
  CHECK(r.cpu_read(i2c1 + dr) == 0xD1);
  CHECK(r.read(i2c1 + sr1) == 0);
}

TEST_CASE("a snapshot while SCL is held restores the byte in the shift register")
{
  rig r{"parked"};
  r.target->out = {0xC1, 0xC2, 0xC3};
  auto const t0 = r.receiving();
  r.idle_to(t0 + phase(18));
  auto const saved = r.f->target->snapshot();
  rig        other{"parked"};
  other.target->out = {0xC1, 0xC2, 0xC3};
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.target->kinds() == "SRR");
  for(auto const* one : {&r, &other})
  {
    CHECK(one->read(i2c1 + sr1) == (rxne | btf));
    CHECK(one->cpu_read(i2c1 + dr) == 0xC1);
    CHECK(one->cpu_read(i2c1 + dr) == 0xC2);
    one->idle_to(t0 + phase(30));
    CHECK(one->read(i2c1 + sr1) == rxne);
    CHECK(one->target->kinds() == "SRRR");
  }
}

TEST_CASE("a target leaving a read answer pending fails the run, the byte reads 0xFF")
{
  rig        r{"parked"};
  auto const t0        = r.receiving();
  r.target->defer_read = true;
  auto const refused   = refusal([&] {
    static_cast<void>(r.f->target->run_slice(
        {.max_instructions = icount{1'000'000}, .until_time = vtime{t0 + phase(10)}}));
  });
  CHECK(refused.contains("an answer at the phase's end from the target at 0x50"));
  CHECK(r.target->kinds() == "SQ");
  CHECK(r.read(i2c1 + dr) == 0xFF);
  CHECK(r.read(i2c1 + sr1) == rxne);
}

TEST_CASE("under strict_peripherals controller receive runs with no model_error")
{
  rig r{"parked", {}, l152re(), firmware, true};
  r.target->out = {0xC1};
  auto const t0 = r.receiving();
  r.idle_to(t0 + phase(9));
  CHECK(r.read(i2c1 + sr1) == rxne);
  CHECK(r.read(i2c1 + dr) == 0xC1);
  CHECK(r.warnings("not modelled") == 0);
}
}

TEST_CASE(
    "a DR write with TRA 1 after a read left RxNE 1 clears it and sends the written byte, inferred")
{
  rig r{"parked"};
  r.target->out = {0xC1};
  auto const t0 = r.receiving(pe);
  r.idle_to(t0 + phase(9));
  REQUIRE(r.read(i2c1 + sr1) == rxne);
  r.cpu_write(i2c1 + cr1, pe | stop);
  CHECK(r.cpu_read(i2c1 + dr) == 0xC1);
  r.idle_to(t0 + phase(19));
  CHECK(r.read(i2c1 + sr1) == rxne);
  CHECK(r.read(i2c1 + dr) == 0xFF);
  CHECK(r.read(i2c1 + sr2) == 0);
  r.transmitting();
  auto const t1 = r.now_ps();
  r.cpu_write(i2c1 + dr, 0x11);
  CHECK(r.read(i2c1 + sr1) == txe);
  r.cpu_write(i2c1 + dr, 0x22);
  r.idle_to(t1 + phase(18));
  CHECK(r.target->bytes() == std::vector<std::uint8_t>{0x11, 0x22});
  CHECK(r.read(i2c1 + sr1) == (txe | btf));
}

TEST_CASE("a write transfer after a repeated START over a held byte drops it and BTF, inferred")
{
  rig r{"parked"};
  r.target->out = {0xC1, 0xC2};
  auto const t0 = r.receiving();
  r.idle_to(t0 + phase(18));
  REQUIRE(r.read(i2c1 + sr1) == (rxne | btf));
  r.transmitting();
  auto const t1 = r.now_ps();
  r.cpu_write(i2c1 + dr, 0x11);
  CHECK(r.read(i2c1 + sr1) == txe);
  r.idle_to(t1 + phase(9));
  CHECK(r.target->bytes() == std::vector<std::uint8_t>{0x11});
  CHECK(r.read(i2c1 + sr1) == (txe | btf));
  r.cpu_write(i2c1 + cr1, pe | stop);
  r.idle_to(t1 + phase(10));
  CHECK(r.read(i2c1 + sr1) == 0);
  CHECK(r.read(i2c1 + sr2) == 0);
}

TEST_CASE("HAL_I2C_Mem_Read of 1, 2 and 3 bytes after IsDeviceReady polls the 5 ms write cycle")
{
  constexpr std::array                   entries{"hal_read_one", "hal_read_two", "hal_read_three"};
  constexpr std::array<std::uint32_t, 3> pattern{0xA1, 0xB2, 0xC3};
  for(auto const& [desc, elf] : {
          std::pair{&l152re(), firmware  },
          std::pair{&l152rb(), firmware_b}
  })
  {
    for(std::size_t size = 1; size <= entries.size(); ++size)
    {
      CAPTURE(elf);
      CAPTURE(size);
      rig        r{entries[size - 1], {}, *desc, elf};
      auto const logged = r.finish();
      REQUIRE(logged.size() == size + 2);
      CHECK(logged.front() == 5);
      CHECK(std::ranges::equal(std::span{logged}.subspan(1, size), std::span{pattern}.first(size)));
      CHECK(logged.back() == 0);
      auto const contents = r.eeprom->contents();
      for(std::size_t index = 0; index < size; ++index)
      {
        CHECK(contents[0x40 + index] == static_cast<std::byte>(pattern[index]));
      }
      CHECK(contents[0x40 + size] == std::byte{0xFF});
      CHECK(std::ranges::none_of(r.logged, [](core::log_record const& one) {
        return one.peripheral == "I2C1";
      }));
    }
  }
}

TEST_CASE("HAL_I2C_Mem_Read of 0 bytes leaves RxNE 1 at its STOP, a Mem_Write then sends its bytes")
{
  for(auto const& [desc, elf] : {
          std::pair{&l152re(), firmware  },
          std::pair{&l152rb(), firmware_b}
  })
  {
    CAPTURE(elf);
    rig r{"hal_read_none", {}, *desc, elf};
    CHECK(r.finish() == std::vector<std::uint32_t>{rxne, 0xA1, 0xB2, 0});
    auto const contents = r.eeprom->contents();
    CHECK(contents[0x40] == std::byte{0xA1});
    CHECK(contents[0x41] == std::byte{0xB2});
    CHECK(contents[0x42] == std::byte{0xFF});
  }
}

TEST_CASE("HAL_I2C_IsDeviceReady: HAL_OK on 0x50, HAL_ERROR on 0x51, bus left idle")
{
  for(auto const& [desc, elf] : {
          std::pair{&l152re(), firmware  },
          std::pair{&l152rb(), firmware_b}
  })
  {
    CAPTURE(elf);
    rig r{"hal_ready", {}, *desc, elf};
    CHECK(r.finish() == std::vector<std::uint32_t>{0, 1, 0, 0, 0, 0, 0});
  }
}
