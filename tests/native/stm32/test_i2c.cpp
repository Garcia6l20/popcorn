#include "stm32_fixture.hpp"

#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/devices/i2c_eeprom.hpp>
#include <popcorn/periph/devices/i2c_table.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/i2c.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core    = popcorn::core;
namespace devices = popcorn::periph::devices;
namespace stm32   = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;
using popcorn::testing::u575;

constexpr std::uint64_t us        = 1'000'000;
constexpr std::uint64_t period_ps = 2'500'000;
constexpr std::uint64_t byte_ps   = 9 * period_ps;
constexpr std::uint64_t tWR_ps    = 5'000 * us;

struct bus_event
{
  char          kind{};
  std::uint32_t value{};
  std::uint64_t at{};

  friend auto operator==(bus_event const&, bus_event const&) -> bool = default;
};

class recorded_target final : public stm32::i2c_target
{
public:
  explicit recorded_target(std::shared_ptr<stm32::i2c_target> target) : inner{std::move(target)}
  {}

  std::shared_ptr<stm32::i2c_target> inner;
  std::vector<bus_event>             events;

  [[nodiscard]] auto address() const -> std::uint8_t override
  {
    return inner->address();
  }

  auto start(bool read, vtime at) -> bool override
  {
    auto const ack = inner->start(read, at);
    events.push_back({ack ? (read ? 'R' : 'W') : 'N', 0, at.ps});
    return ack;
  }

  auto write(std::uint8_t byte, vtime at) -> bool override
  {
    auto const ack = inner->write(byte, at);
    events.push_back({ack ? 'w' : 'n', byte, at.ps});
    return ack;
  }

  auto read(vtime at) -> std::uint8_t override
  {
    auto const byte = inner->read(at);
    events.push_back({'r', byte, at.ps});
    return byte;
  }

  void stop(vtime at) override
  {
    inner->stop(at);
    events.push_back({'P', 0, at.ps});
  }

  void save(std::vector<std::byte>& out) const override
  {
    inner->save(out);
  }

  void load(std::span<std::byte const> in) override
  {
    inner->load(in);
  }
};

class picky_target final : public stm32::i2c_target
{
public:
  std::uint32_t written{};

  [[nodiscard]] auto address() const -> std::uint8_t override
  {
    return 0x21;
  }

  auto start(bool, vtime) -> bool override
  {
    written = 0;
    return true;
  }

  auto write(std::uint8_t, vtime) -> bool override
  {
    return written++ == 0;
  }

  auto read(vtime) -> std::uint8_t override
  {
    return 0;
  }

  void stop(vtime) override
  {}

  void save(std::vector<std::byte>& out) const override
  {
    out.push_back(static_cast<std::byte>(written));
  }

  void load(std::span<std::byte const> in) override
  {
    written = in.empty() ? 0 : std::to_integer<std::uint32_t>(in.front());
  }
};

auto eeprom_image() -> std::filesystem::path const&
{
  static auto const path = [] {
    auto const    at = std::filesystem::temp_directory_path() / "popcorn-test-i2c-image.bin";
    std::ofstream file{at, std::ios::binary | std::ios::trunc};
    for(int index = 0; index < 300; ++index)
    {
      file.put(static_cast<char>(index & 0xFF));
    }
    return at;
  }();
  return path;
}

auto eeprom_options() -> devices::eeprom_24xx_options
{
  return {
      .address = 0x50, .size = 4096, .address_bytes = 2, .page_size = 16, .image = eeprom_image()};
}

struct rig
{
  std::shared_ptr<devices::eeprom_24xx> eeprom =
      std::make_shared<devices::eeprom_24xx>(eeprom_options());
  std::shared_ptr<devices::table_target> script =
      std::make_shared<devices::table_target>(devices::table_target_options{
          .address        = 0x38,
          .register_bytes = 2,
          .replies        = {{0x4002, {0x12, 0x34}}}});
  std::shared_ptr<recorded_target> memory = std::make_shared<recorded_target>(eeprom);
  std::shared_ptr<recorded_target> table  = std::make_shared<recorded_target>(script);
  std::shared_ptr<recorded_target> picky =
      std::make_shared<recorded_target>(std::make_shared<picky_target>());
  stm32::board                   board{};
  std::unique_ptr<stm32_fixture> f;

  explicit rig(std::string_view entry)
  {
    board.i2c["I2C1"] = {memory, table, picky};
    auto options      = popcorn::testing::stm32_options();
    options.models    = stm32::factories(board, u575());
    f                 = std::make_unique<stm32_fixture>(u575(), "i2c_ll", std::move(options));
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("i2c_ll", "main")})
            .reason
        == stop_reason::address);
    f->set("pc", sym("i2c_ll", entry));
  }

  auto finish() const -> std::vector<std::uint32_t>
  {
    auto const stopped = f->run_to_stop();
    CAPTURE(stopped.detail);
    REQUIRE(stopped.reason == stop_reason::user);
    return f->log();
  }
};

auto of_kind(std::vector<bus_event> const& events, char kind) -> std::vector<bus_event>
{
  std::vector<bus_event> out;
  std::ranges::copy_if(
      events, std::back_inserter(out), [&](bus_event const& e) { return e.kind == kind; });
  return out;
}

auto stop_after(std::vector<bus_event> const& events, std::size_t from) -> std::uint64_t
{
  auto const found = std::find_if(
      events.begin() + static_cast<std::ptrdiff_t>(from), events.end(),
      [](bus_event const& e) { return e.kind == 'P'; });
  REQUIRE(found != events.end());
  return found->at;
}
}

TEST_CASE("two page writes with acknowledge polling, then a random read of 20 bytes, twice")
{
  std::vector<bus_event> first;
  std::vector<std::byte> first_state;
  for(int run = 0; run < 2; ++run)
  {
    rig        r{"eeprom_rw"};
    auto const log = r.finish();
    REQUIRE(log.size() == 3);
    CHECK(log[0] == 20);
    auto const& events = r.memory->events;
    auto const  nacks  = of_kind(events, 'N');
    CHECK(nacks.size() == log[1] + log[2]);
    CHECK(log[1] > 100);
    CHECK(log[1] <= tWR_ps / (11 * period_ps));
    auto const state = core::encode_snapshot(r.f->target->snapshot().data());
    if(run == 0)
    {
      first       = events;
      first_state = state;
    }
    else
    {
      CHECK(events == first);
      CHECK(state == first_state);
    }
  }
}

TEST_CASE("the EEPROM NACKs its address during tWR and ACKs the first poll after it")
{
  rig r{"eeprom_rw"};
  r.finish();
  auto const& events           = r.memory->events;
  auto const  first_write_stop = stop_after(events, 0);
  auto const  cycle_end        = first_write_stop + tWR_ps;
  std::size_t polls            = 0;
  auto        acked_at         = std::uint64_t{};
  for(auto const& e : events)
  {
    if(e.at <= first_write_stop or acked_at != 0)
    {
      continue;
    }
    if(e.kind == 'N')
    {
      CHECK(e.at < cycle_end);
      ++polls;
    }
    if(e.kind == 'W')
    {
      acked_at = e.at;
    }
  }
  CHECK(polls > 0);
  CHECK(acked_at >= cycle_end);
  CHECK(r.eeprom->contents()[0x10] == std::byte{3});
  CHECK(r.eeprom->contents()[0x23] == std::byte{(7 * 19 + 3) & 0xFF});
}

TEST_CASE("bytes of a write transfer are 9 SCL periods apart at 400 kHz")
{
  rig r{"eeprom_rw"};
  r.finish();
  auto const& events = r.memory->events;
  REQUIRE(events.size() > 19);
  REQUIRE(events[0].kind == 'W');
  for(std::size_t index = 2; index < 19; ++index)
  {
    CAPTURE(index);
    REQUIRE(events[index].kind == 'w');
    CHECK(events[index].at - events[index - 1].at == byte_ps);
  }
  CHECK(events[19].kind == 'P');
  CHECK(events[19].at - events[18].at == period_ps);
  CHECK(events[1].at - events[0].at >= byte_ps);
}

TEST_CASE("an address nobody answers sets NACKF and STOPF, which IRQ 55 serves")
{
  rig        r{"nack"};
  auto const log = r.finish();
  CHECK(log == std::vector<std::uint32_t>{1, 1, 1, 0});
  CHECK(r.memory->events.empty());
  CHECK(r.table->events.empty());
}

TEST_CASE("a table target records register writes and answers a register read")
{
  rig r{"table"};
  CHECK(r.finish() == std::vector<std::uint32_t>{0x12, 0x34});
  REQUIRE(r.script->writes().size() == 2);
  CHECK(r.script->writes()[0] == devices::table_write{.reg = 0x4000, .bytes = {0x01}});
  CHECK(
      r.script->writes()[1]
      == devices::table_write{
          .reg = 0x4001, .bytes = {0x02, 0x03}
  });
  CHECK(r.memory->events.empty());
}

TEST_CASE("a 300-byte read in three reloads of 100 bytes sets TCR twice, then stops")
{
  rig r{"reload"};
  CHECK(r.finish() == std::vector<std::uint32_t>{300, 2});
  auto const reads = of_kind(r.memory->events, 'r');
  CHECK(reads.size() == 300);
  CHECK(of_kind(r.memory->events, 'P').size() == 1);
}

TEST_CASE("a snapshot during tWR restores the NACKs and the ACK at the same virtual times")
{
  rig r{"eeprom_rw"};
  REQUIRE(
      r.f->run_to_stop({.max_instructions = icount{100'000'000},
                        .stop_when = [&] { return not of_kind(r.memory->events, 'N').empty(); }})
          .reason
      == stop_reason::user);
  auto const saved = r.f->target->snapshot();
  auto const mark  = r.memory->events.size();
  auto const log   = r.finish();
  auto const tail  = std::vector(
      r.memory->events.begin() + static_cast<std::ptrdiff_t>(mark), r.memory->events.end());

  rig other{"idle"};
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.finish() == log);
  CHECK(other.memory->events == tail);
}

namespace
{
struct bus
{
  stm32_fixture& f;

  [[nodiscard]] auto reg(std::string_view peripheral, std::string_view name) const
      -> popcorn::register_desc const&
  {
    auto const& desc  = f.target->peripheral(peripheral).description();
    auto const  found = std::ranges::find(desc.registers, name, &popcorn::register_desc::name);
    REQUIRE(found != desc.registers.end());
    return *found;
  }

  [[nodiscard]] auto at(std::string_view peripheral, std::string_view name) const
      -> popcorn::address
  {
    return f.target->peripheral(peripheral).description().base + reg(peripheral, name).offset;
  }

  [[nodiscard]] auto read(popcorn::address address) const -> std::uint32_t
  {
    auto const value = f.target->mem().read(address, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void write(popcorn::address address, std::uint32_t value) const
  {
    REQUIRE(f.target->mem().write(address, 4, value).has_value());
  }

  [[nodiscard]] auto get(std::string_view name, std::string_view field) const -> std::uint32_t
  {
    return f.field("I2C1", name, field);
  }

  void
      set(std::string_view peripheral,
          std::string_view name,
          std::string_view field,
          std::uint32_t    value) const
  {
    auto const& fields = reg(peripheral, name).fields;
    auto const  found  = std::ranges::find(fields, field, &popcorn::field_desc::name);
    REQUIRE(found != fields.end());
    auto const mask    = ((std::uint64_t{1} << found->width) - 1u) << found->lsb;
    auto const address = at(peripheral, name);
    write(
        address, static_cast<std::uint32_t>(
                     (read(address) & ~mask) | ((std::uint64_t{value} << found->lsb) & mask)));
  }

  void set(std::string_view name, std::string_view field, std::uint32_t value) const
  {
    set("I2C1", name, field, value);
  }

  void command(
      std::uint32_t address,
      std::uint32_t count,
      bool          read,
      bool          autoend,
      bool          reload = false) const
  {
    auto const    cr2   = reg("I2C1", "CR2");
    std::uint32_t value = 0;
    for(auto const& [name, bits] : std::vector<std::pair<std::string_view, std::uint32_t>>{
            {"SADD",    address << 1     },
            {"NBYTES",  count            },
            {"RD_WRN",  read ? 1u : 0u   },
            {"AUTOEND", autoend ? 1u : 0u},
            {"RELOAD",  reload ? 1u : 0u },
            {"START",   1                }
    })
    {
      auto const found = std::ranges::find(cr2.fields, name, &popcorn::field_desc::name);
      REQUIRE(found != cr2.fields.end());
      value |= bits << found->lsb;
    }
    write(at("I2C1", "CR2"), value);
  }

  void send(std::uint8_t byte) const
  {
    write(at("I2C1", "TXDR"), byte);
  }

  [[nodiscard]] auto receive() const -> std::uint32_t
  {
    auto&      regs  = f.target->peripheral("I2C1");
    auto const value = f.target->model("I2C1")->on_read(
        regs, reg("I2C1", "RXDR").offset, 4, {.pc = 0, .now = icount{0}});
    REQUIRE(value.has_value());
    return *value;
  }

  [[nodiscard]] auto now() const -> std::uint64_t
  {
    return f.target->virtual_clock().time().ps;
  }

  void run_until(std::uint64_t ps) const
  {
    REQUIRE(
        f.run_to_stop({.max_instructions = icount{1'000'000'000}, .until_time = vtime{ps}}).reason
        == stop_reason::time);
  }

  void controller() const
  {
    set("RCC", "RCC_APB1ENR1", "I2C1EN", 1);
    write(at("I2C1", "TIMINGR"), 0x0010'0305);
    set("CR1", "PE", 1);
  }
};

auto pending(stm32_fixture const& f, std::uint32_t irq) -> bool
{
  auto const word = f.target->mem().read(0xe000'e200u + 4 * (irq / 32), 4);
  REQUIRE(word.has_value());
  return (*word >> (irq % 32) & 1u) != 0;
}

constexpr std::uint32_t event_irq  = 55;
constexpr std::uint64_t address_ps = 10 * period_ps;
}

TEST_CASE("TXIS asks for the next byte as soon as TXDR is copied, TXE flushes TXDR")
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  CHECK(b.get("ISR", "TXE") == 1);
  b.command(0x38, 3, false, true);
  auto const start = b.now();
  CHECK(b.get("ISR", "BUSY") == 1);
  CHECK(b.get("ISR", "TXIS") == 0);
  b.run_until(start + address_ps + period_ps / 2);
  CHECK(b.get("CR2", "START") == 0);
  CHECK(b.get("ISR", "TXIS") == 1);
  b.send(0x40);
  CHECK(b.get("ISR", "TXIS") == 1);
  CHECK(b.get("ISR", "TXE") == 1);
  b.send(0x01);
  CHECK(b.get("ISR", "TXIS") == 0);
  CHECK(b.get("ISR", "TXE") == 0);
  b.set("ISR", "TXE", 1);
  CHECK(b.get("ISR", "TXE") == 1);
  CHECK(b.get("ISR", "TXIS") == 1);
  b.send(0x02);
  b.run_until(start + address_ps + byte_ps + period_ps / 2);
  CHECK(b.get("ISR", "TXIS") == 1);
  b.send(0x03);
  CHECK(b.get("ISR", "TXIS") == 0);
  b.run_until(start + address_ps + 3 * byte_ps + period_ps + period_ps / 2);
  CHECK(b.get("ISR", "STOPF") == 1);
  CHECK(b.get("ISR", "BUSY") == 0);
  CHECK(
      r.table->events
      == std::vector<bus_event>{
          {'W', 0,    r.table->events[0].at},
          {'w', 0x40, r.table->events[1].at},
          {'w', 0x02, r.table->events[2].at},
          {'w', 0x03, r.table->events[3].at},
          {'P', 0,    r.table->events[4].at}
  });
  b.set("ICR", "STOPCF", 1);
  CHECK(b.get("ISR", "STOPF") == 0);
  CHECK(b.read(b.at("I2C1", "ICR")) == 0);
}

TEST_CASE("a NACKed data byte sets NACKF without TXIS and sends a STOP")
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  b.command(0x21, 3, false, false);
  auto const start = b.now();
  b.run_until(start + address_ps + period_ps / 2);
  b.send(0x11);
  b.send(0x22);
  b.run_until(start + address_ps + byte_ps + period_ps / 2);
  CHECK(b.get("ISR", "NACKF") == 0);
  b.run_until(start + address_ps + 2 * byte_ps + period_ps / 2);
  CHECK(b.get("ISR", "NACKF") == 1);
  CHECK(b.get("ISR", "TXIS") == 0);
  CHECK(b.get("ISR", "STOPF") == 0);
  b.run_until(start + address_ps + 2 * byte_ps + period_ps + period_ps / 2);
  CHECK(b.get("ISR", "STOPF") == 1);
  CHECK(b.get("ISR", "TC") == 0);
  CHECK(b.get("ISR", "BUSY") == 0);
  CHECK(r.picky->events.size() == 4);
  CHECK(r.picky->events.back().kind == 'P');
}

TEST_CASE(
    "CR2.STOP during a read sends the STOP after the current byte, TC waits for START or STOP")
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  b.command(0x50, 5, true, false);
  auto const start = b.now();
  b.run_until(start + address_ps + byte_ps + period_ps / 2);
  CHECK(b.get("ISR", "RXNE") == 1);
  b.set("CR2", "STOP", 1);
  b.run_until(start + address_ps + 2 * byte_ps + period_ps / 2);
  CHECK(b.get("ISR", "STOPF") == 0);
  CHECK(b.get("ISR", "BUSY") == 1);
  CHECK(b.receive() == 0x00);
  CHECK(b.get("ISR", "RXNE") == 1);
  auto const resumed = b.now();
  b.run_until(resumed + period_ps + period_ps / 2);
  CHECK(b.get("ISR", "STOPF") == 1);
  CHECK(b.get("CR2", "STOP") == 0);
  CHECK(b.receive() == 0x01);
  CHECK(of_kind(r.memory->events, 'r').size() == 2);

  b.set("ICR", "STOPCF", 1);
  b.command(0x50, 1, true, false);
  auto const again = b.now();
  b.run_until(again + address_ps + byte_ps + period_ps / 2);
  CHECK(b.get("ISR", "TC") == 1);
  CHECK(b.get("ISR", "BUSY") == 1);
  b.run_until(again + address_ps + 4 * byte_ps);
  CHECK(b.get("ISR", "STOPF") == 0);
  b.set("CR2", "STOP", 1);
  CHECK(b.get("ISR", "TC") == 0);
  b.run_until(b.now() + period_ps + period_ps / 2);
  CHECK(b.get("ISR", "STOPF") == 1);
}

TEST_CASE("TCR holds the bus until NBYTES is written again")
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  b.command(0x50, 1, true, false, true);
  auto const start = b.now();
  b.run_until(start + address_ps + byte_ps + period_ps / 2);
  CHECK(b.get("ISR", "TCR") == 1);
  CHECK(b.receive() == 0x00);
  b.run_until(start + address_ps + 3 * byte_ps);
  CHECK(of_kind(r.memory->events, 'r').size() == 1);
  b.set("CR2", "RELOAD", 0);
  CHECK(b.get("ISR", "TCR") == 0);
  auto const resumed = b.now();
  b.run_until(resumed + byte_ps + period_ps / 2);
  CHECK(b.get("ISR", "TC") == 1);
  CHECK(b.receive() == 0x01);
}

TEST_CASE("the event IRQ follows the flags and their enables, CR1 writes re-evaluate it")
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  b.command(0x38, 1, false, true);
  b.run_until(b.now() + address_ps + period_ps / 2);
  REQUIRE(b.get("ISR", "TXIS") == 1);
  CHECK_FALSE(pending(*r.f, event_irq));
  b.set("CR1", "TXIE", 1);
  CHECK(pending(*r.f, event_irq));
}

TEST_CASE("clearing PE ends the transfer and resets the flags and CR2's command bits")
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  b.command(0x2A, 1, false, false);
  b.run_until(b.now() + address_ps + period_ps / 2);
  REQUIRE(b.get("ISR", "NACKF") == 1);
  b.command(0x38, 2, false, false);
  b.run_until(b.now() + address_ps / 2);
  b.set("CR2", "STOP", 1);
  b.set("CR1", "PE", 0);
  CHECK(b.get("ISR", "NACKF") == 0);
  CHECK(b.get("ISR", "BUSY") == 0);
  CHECK(b.get("ISR", "TXE") == 1);
  CHECK(b.get("CR2", "START") == 0);
  CHECK(b.get("CR2", "STOP") == 0);
  auto const events = r.table->events.size();
  b.run_until(b.now() + 4 * address_ps);
  CHECK(r.table->events.size() == events);
  b.set("CR2", "START", 1);
  CHECK(b.get("CR2", "START") == 0);
}

TEST_CASE("a stopped I2C kernel clock moves nothing")
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  b.set("RCC", "RCC_APB1ENR1", "I2C1EN", 0);
  b.command(0x38, 1, false, true);
  b.run_until(b.now() + 4 * address_ps);
  CHECK(b.get("ISR", "BUSY") == 1);
  CHECK(r.table->events.empty());
}

TEST_CASE("PRESC scales the SCL period and targets see the time since power-on")
{
  rig r{"idle"};
  r.f->run_to_stop({.max_instructions = icount{100'000'000}, .until_time = vtime{1'000 * us}});
  r.f->target->reset(popcorn::reset_kind::software);
  r.f->set("pc", sym("i2c_ll", "idle"));
  bus b{*r.f};
  b.controller();
  b.write(b.at("I2C1", "TIMINGR"), 0x1010'0305);
  b.command(0x38, 0, false, true);
  auto const start = b.now();
  b.run_until(start + 2 * address_ps + 4 * period_ps);
  REQUIRE(r.table->events.size() == 2);
  auto const offset = r.f->target->virtual_clock().since_power_on().ps - b.now();
  CHECK(offset >= 1'000 * us);
  CHECK(r.table->events[0].at == offset + start + 2 * address_ps);
  CHECK(r.table->events[1].at == offset + start + 2 * address_ps + 2 * period_ps);
}

TEST_CASE("TCR raises the event IRQ through TCIE")
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  b.set("CR1", "TCIE", 1);
  b.command(0x50, 1, true, false, true);
  b.run_until(b.now() + address_ps + byte_ps + period_ps / 2);
  REQUIRE(b.get("ISR", "TCR") == 1);
  CHECK(pending(*r.f, event_irq));
}

TEST_CASE("TXIS stays low once the last byte of NBYTES is in the shift register")
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  b.command(0x38, 1, false, true);
  b.run_until(b.now() + address_ps + period_ps / 2);
  REQUIRE(b.get("ISR", "TXIS") == 1);
  b.send(0x40);
  CHECK(b.get("ISR", "TXE") == 1);
  CHECK(b.get("ISR", "TXIS") == 0);
}

TEST_CASE("START set during a transfer sends a repeated START after NBYTES, without TC")
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  b.command(0x38, 1, false, false);
  auto const start = b.now();
  b.run_until(start + address_ps + period_ps / 2);
  b.send(0x40);
  b.command(0x38, 1, true, false);
  b.run_until(start + 2 * address_ps + byte_ps + period_ps / 2);
  CHECK(b.get("ISR", "TC") == 0);
  REQUIRE(r.table->events.size() == 3);
  CHECK(r.table->events[2].kind == 'R');
  CHECK(r.table->events[2].at - r.table->events[1].at == address_ps);
}

TEST_CASE("START set while a STOP is sent starts the next transfer after it")
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  b.command(0x2A, 1, false, true);
  auto const start = b.now();
  b.run_until(start + address_ps + period_ps / 2);
  REQUIRE(b.get("ISR", "NACKF") == 1);
  REQUIRE(b.get("ISR", "STOPF") == 0);
  b.command(0x38, 0, false, true);
  b.run_until(start + 2 * address_ps + 2 * period_ps + period_ps / 2);
  REQUIRE(r.table->events.size() == 2);
  CHECK(r.table->events[0].kind == 'W');
}

TEST_CASE("writing 0 to START or STOP has no effect, TXDR is ignored while PE is clear")
{
  rig r{"idle"};
  bus b{*r.f};
  b.set("RCC", "RCC_APB1ENR1", "I2C1EN", 1);
  b.send(0x55);
  CHECK(b.get("ISR", "TXE") == 1);
  b.controller();
  b.command(0x38, 2, false, false);
  auto const& fields = b.reg("I2C1", "CR2").fields;
  auto const  start =
      std::ranges::find(fields, std::string_view{"START"}, &popcorn::field_desc::name);
  REQUIRE(start != fields.end());
  auto const cr2 = b.read(b.at("I2C1", "CR2"));
  b.write(b.at("I2C1", "CR2"), cr2 & ~(1u << start->lsb));
  CHECK(b.get("CR2", "START") == 1);
}

TEST_CASE("a STOP requested while TXDR is awaited is sent at once")
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  b.command(0x38, 2, false, false);
  b.run_until(b.now() + address_ps + period_ps / 2);
  REQUIRE(b.get("ISR", "TXIS") == 1);
  b.set("CR2", "STOP", 1);
  b.run_until(b.now() + period_ps + period_ps / 2);
  CHECK(b.get("ISR", "STOPF") == 1);
  CHECK(b.get("ISR", "BUSY") == 0);
}

TEST_CASE("an event of a transfer PE ended does not complete the next one")
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  b.command(0x38, 0, false, true);
  b.run_until(b.now() + address_ps / 2);
  b.set("CR1", "PE", 0);
  b.set("CR1", "PE", 1);
  b.command(0x38, 0, false, true);
  auto const restart = b.now();
  b.run_until(restart + address_ps - period_ps / 2);
  CHECK(r.table->events.empty());
  b.run_until(restart + address_ps + period_ps / 2);
  CHECK(r.table->events.size() == 1);
}

TEST_CASE("an event of a transfer PE ended does not complete one started on a stopped clock")
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  b.command(0x38, 0, false, true);
  auto const start = b.now();
  b.run_until(start + address_ps / 2);
  b.set("CR1", "PE", 0);
  b.set("RCC", "RCC_APB1ENR1", "I2C1EN", 0);
  b.set("CR1", "PE", 1);
  b.command(0x38, 0, false, true);
  b.run_until(start + 4 * address_ps);
  CHECK(r.table->events.empty());
  CHECK(b.get("ISR", "BUSY") == 1);
}

TEST_CASE("a repeated START to another target leaves the table write pending until its next STOP")
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  b.command(0x38, 3, false, false);
  b.run_until(b.now() + address_ps + period_ps / 2);
  b.send(0x00);
  b.send(0x07);
  b.run_until(b.now() + byte_ps);
  b.send(0x99);
  b.run_until(b.now() + 2 * byte_ps + period_ps / 2);
  REQUIRE(b.get("ISR", "TC") == 1);
  b.command(0x50, 1, true, true);
  b.run_until(b.now() + address_ps + byte_ps + period_ps + period_ps / 2);
  REQUIRE(b.get("ISR", "STOPF") == 1);
  CHECK(r.memory->events.front().kind == 'R');
  CHECK(r.memory->events.back().kind == 'P');
  CHECK(r.table->events.back().kind == 'w');
  CHECK(r.script->writes().empty());
  b.set("ICR", "STOPCF", 1);
  b.command(0x38, 0, false, true);
  b.run_until(b.now() + address_ps + period_ps + period_ps / 2);
  REQUIRE(r.script->writes().size() == 1);
  CHECK(r.script->writes()[0] == devices::table_write{.reg = 0x07, .bytes = {0x99}});
}

TEST_CASE("TXIS stays low during TCR and comes back once NBYTES is written again")
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  b.command(0x38, 1, false, false, true);
  b.run_until(b.now() + address_ps + period_ps / 2);
  REQUIRE(b.get("ISR", "TXIS") == 1);
  b.send(0x07);
  CHECK(b.get("ISR", "TXIS") == 0);
  b.run_until(b.now() + byte_ps + period_ps / 2);
  CHECK(b.get("ISR", "TCR") == 1);
  CHECK(b.get("ISR", "TXIS") == 0);
  b.run_until(b.now() + 2 * byte_ps);
  CHECK(b.get("ISR", "TXIS") == 0);
  b.set("CR2", "RELOAD", 0);
  CHECK(b.get("ISR", "TCR") == 0);
  CHECK(b.get("ISR", "TXIS") == 1);
  b.send(0x42);
  b.run_until(b.now() + byte_ps + period_ps / 2);
  CHECK(b.get("ISR", "TC") == 1);
  REQUIRE(r.table->events.size() == 3);
  CHECK(r.table->events[2] == bus_event{'w', 0x42, r.table->events[2].at});
}
