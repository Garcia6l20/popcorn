#include "dma.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/devices/i2c_eeprom.hpp>
#include <popcorn/periph/devices/i2c_table.hpp>
#include <popcorn/periph/i2c.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/i2c.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

namespace
{
namespace core    = popcorn::core;
namespace devices = popcorn::periph::devices;
namespace periph  = popcorn::periph;
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

class recorded_target final : public periph::i2c_target
{
public:
  explicit recorded_target(std::shared_ptr<periph::i2c_target> target) : inner{std::move(target)}
  {}

  std::shared_ptr<periph::i2c_target>                                                     inner;
  std::vector<bus_event>                                                                  events;
  std::shared_ptr<std::vector<std::pair<periph::i2c_target const*, popcorn::reset_kind>>> resets;

  [[nodiscard]] auto address() const -> std::uint8_t override
  {
    return inner->address();
  }

  auto start(bool read, vtime at) -> periph::i2c_reply override
  {
    auto const reply = inner->start(read, at);
    events.push_back({reply.ack ? (read ? 'R' : 'W') : 'N', 0, at.ps});
    return reply;
  }

  auto write(std::uint8_t byte, vtime at) -> periph::i2c_reply override
  {
    auto const reply = inner->write(byte, at);
    events.push_back({reply.ack ? 'w' : 'n', byte, at.ps});
    return reply;
  }

  auto read(vtime at) -> periph::i2c_read_reply override
  {
    auto const reply = inner->read(at);
    events.push_back({'r', reply.byte, at.ps});
    return reply;
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

  void reset(popcorn::reset_kind kind) override
  {
    inner->reset(kind);
    if(resets)
    {
      resets->emplace_back(this, kind);
    }
  }
};

class picky_target final : public periph::i2c_target
{
public:
  std::uint32_t written{};

  [[nodiscard]] auto address() const -> std::uint8_t override
  {
    return 0x21;
  }

  auto start(bool, vtime) -> periph::i2c_reply override
  {
    written = 0;
    return {.ack = true};
  }

  auto write(std::uint8_t, vtime) -> periph::i2c_reply override
  {
    return {.ack = written++ == 0};
  }

  auto read(vtime) -> periph::i2c_read_reply override
  {
    return {.byte = 0};
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

class faulty_target final : public periph::i2c_target
{
public:
  periph::i2c_error      error{};
  std::uint32_t          at_call{};
  std::uint32_t          shots{1};
  std::uint32_t          calls{};
  std::uint32_t          stretch_call{~0u};
  std::uint64_t          stretch_ps{};
  std::vector<bus_event> events;

  [[nodiscard]] auto address() const -> std::uint8_t override
  {
    return 0x33;
  }

  auto start(bool read, vtime at) -> periph::i2c_reply override
  {
    calls = 0;
    events.push_back({read ? 'R' : 'W', 0, at.ps});
    return {.ack = true, .error = pick(), .stretch_ps = stretch()};
  }

  auto write(std::uint8_t byte, vtime at) -> periph::i2c_reply override
  {
    ++calls;
    events.push_back({'w', byte, at.ps});
    return {.ack = true, .error = pick(), .stretch_ps = stretch()};
  }

  auto read(vtime at) -> periph::i2c_read_reply override
  {
    ++calls;
    events.push_back({'r', 0x5A, at.ps});
    return {.byte = 0x5A, .error = pick(), .stretch_ps = stretch()};
  }

  void stop(vtime at) override
  {
    events.push_back({'P', 0, at.ps});
  }

  void save(std::vector<std::byte>& out) const override
  {
    for(auto const value : {calls, shots})
    {
      out.push_back(static_cast<std::byte>(value));
    }
  }

  void load(std::span<std::byte const> in) override
  {
    REQUIRE(in.size() == 2);
    calls = std::to_integer<std::uint32_t>(in[0]);
    shots = std::to_integer<std::uint32_t>(in[1]);
  }

private:
  [[nodiscard]] auto stretch() const -> std::uint64_t
  {
    return calls == stretch_call ? stretch_ps : 0;
  }

  auto pick() -> periph::i2c_error
  {
    if(calls != at_call or shots == 0)
    {
      return periph::i2c_error::none;
    }
    --shots;
    return error;
  }
};

class image_file
{
public:
  image_file()
  {
    std::ofstream file{path_, std::ios::binary | std::ios::trunc};
    for(int index = 0; index < 300; ++index)
    {
      file.put(static_cast<char>(index & 0xFF));
    }
  }

  image_file(image_file const&)                    = delete;
  auto operator=(image_file const&) -> image_file& = delete;

  ~image_file()
  {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  [[nodiscard]] auto path() const noexcept -> std::filesystem::path const&
  {
    return path_;
  }

private:
  std::filesystem::path path_ = std::filesystem::temp_directory_path()
                              / std::format("popcorn-test-i2c-image-{}.bin", ::getpid());
};

auto eeprom_image() -> std::filesystem::path const&
{
  static image_file const image;
  return image.path();
}

auto eeprom_options() -> devices::eeprom_24xx_options
{
  return {
      .address = 0x50, .size = 4096, .address_bytes = 2, .page_size = 16, .image = eeprom_image()};
}

class request_probe final : public popcorn::periph::model, public stm32::dma_controller
{
public:
  std::map<std::uint32_t, bool> levels;

  void on_request(std::uint32_t request, bool level) override
  {
    levels[request] = level;
  }

  [[nodiscard]] auto request_of(std::string_view signal) const
      -> std::optional<std::uint32_t> override
  {
    if(signal == "i2c1_rx_dma")
    {
      return 12;
    }
    if(signal == "i2c1_tx_dma")
    {
      return 13;
    }
    if(signal == "i2c1_evc_dma")
    {
      return 14;
    }
    return std::nullopt;
  }

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }
  void reset(popcorn::periph::register_file&, popcorn::reset_kind) override
  {}
  auto on_read(
      popcorn::periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      popcorn::periph::access_ctx) -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    return 0u;
  }
  auto on_write(
      popcorn::periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t,
      popcorn::periph::access_ctx) -> std::expected<void, popcorn::bus_error> override
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
  std::shared_ptr<faulty_target> faulty = std::make_shared<faulty_target>();
  std::shared_ptr<std::vector<std::pair<periph::i2c_target const*, popcorn::reset_kind>>> resets =
      std::make_shared<std::vector<std::pair<periph::i2c_target const*, popcorn::reset_kind>>>();
  stm32::board                   board{};
  std::unique_ptr<stm32_fixture> f;

  explicit rig(std::string_view entry, bool probe = false)
  {
    memory->resets    = resets;
    table->resets     = resets;
    picky->resets     = resets;
    board.i2c["I2C1"] = {memory, table, picky, faulty};
    auto options      = popcorn::testing::stm32_options();
    options.models    = stm32::factories(board, u575());
    if(probe)
    {
      options.models.insert_or_assign(
          "GPDMA1",
          [](popcorn::periph::model_context&,
             popcorn::peripheral_desc const&) -> std::shared_ptr<popcorn::periph::model> {
            return std::make_shared<request_probe>();
          });
    }
    f = std::make_unique<stm32_fixture>(u575(), "i2c_ll", std::move(options));
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("i2c_ll", "main")})
            .reason
        == stop_reason::address);
    f->set("pc", sym("i2c_ll", entry));
  }

  [[nodiscard]] auto level(std::uint32_t request) const -> bool
  {
    auto* const probe = f->model_of<request_probe>("GPDMA1");
    REQUIRE(probe != nullptr);
    auto const found = probe->levels.find(request);
    return found != probe->levels.end() and found->second;
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

TEST_CASE("every machine reset reaches each target once, in attachment order, with its kind")
{
  using popcorn::reset_kind;
  rig        r{"eeprom_rw"};
  auto const expect = [&](reset_kind kind) {
    return std::vector<std::pair<periph::i2c_target const*, reset_kind>>{
        {r.memory.get(), kind},
        {r.table.get(),  kind},
        {r.picky.get(),  kind}
    };
  };
  CHECK(*r.resets == expect(reset_kind::power_on));
  r.resets->clear();
  r.f->target->reset(reset_kind::watchdog);
  CHECK(*r.resets == expect(reset_kind::watchdog));
}

TEST_CASE("a power-on reset during tWR lets the EEPROM answer at once in the new timeline")
{
  auto const  fresh = rig{"eeprom_rw"}.finish();
  rig         r{"eeprom_rw"};
  auto const& events = r.memory->events;
  auto const stopped = r.f->run_to_stop({.max_instructions = icount{100'000'000}, .stop_when = [&] {
                                           return not of_kind(events, 'N').empty();
                                         }});
  REQUIRE(stopped.reason == stop_reason::user);
  r.f->target->reset(popcorn::reset_kind::power_on);
  REQUIRE(
      r.f->run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("i2c_ll", "main")})
          .reason
      == stop_reason::address);
  r.f->set("pc", sym("i2c_ll", "eeprom_rw"));
  CHECK(r.finish() == fresh);
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
    auto const  found = popcorn::find_register(desc, name);
    REQUIRE(found != nullptr);
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

TEST_CASE("GPDMA reads 32 EEPROM bytes through the I2C1 RX request, twice the same")
{
  std::vector<bus_event> first;
  std::vector<std::byte> first_state;
  for(int run = 0; run < 2; ++run)
  {
    rig r{"dma"};
    CHECK(r.finish() == std::vector<std::uint32_t>{32, 1});
    auto const reads = of_kind(r.memory->events, 'r');
    REQUIRE(reads.size() == 32);
    for(std::size_t index = 1; index < reads.size(); ++index)
    {
      CAPTURE(index);
      CHECK(reads[index].at - reads[index - 1].at == byte_ps);
    }
    auto const state = core::encode_snapshot(r.f->target->snapshot().data());
    if(run == 0)
    {
      first       = r.memory->events;
      first_state = state;
    }
    else
    {
      CHECK(r.memory->events == first);
      CHECK(state == first_state);
    }
  }
}

TEST_CASE("a snapshot mid-DMA read restores to the same bytes and the same bus times")
{
  rig r{"dma"};
  REQUIRE(
      r.f->run_to_stop({.max_instructions = icount{100'000'000},
                        .stop_when =
                            [&] {
                              auto const reads = of_kind(r.memory->events, 'r').size();
                              return reads > 4 and reads < 28;
                            }})
          .reason
      == stop_reason::user);
  auto const saved = r.f->target->snapshot();
  auto const mark  = r.memory->events.size();
  auto const log   = r.finish();
  CHECK(log == std::vector<std::uint32_t>{32, 1});
  auto const tail = std::vector(
      r.memory->events.begin() + static_cast<std::ptrdiff_t>(mark), r.memory->events.end());

  rig other{"idle"};
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.finish() == log);
  CHECK(other.memory->events == tail);
}

TEST_CASE("the I2C1 DMA requests follow TXIS, RXNE, TC and TCR with their enables and PE")
{
  constexpr std::uint32_t rx_request  = 12;
  constexpr std::uint32_t tx_request  = 13;
  constexpr std::uint32_t evc_request = 14;
  rig                     r{"idle", true};
  bus                     b{*r.f};
  b.controller();
  b.command(0x38, 2, false, false);
  b.run_until(b.now() + address_ps + period_ps / 2);
  REQUIRE(b.get("ISR", "TXIS") == 1);
  CHECK_FALSE(r.level(tx_request));
  b.set("CR1", "TXDMAEN", 1);
  CHECK(r.level(tx_request));
  b.send(0x40);
  CHECK(r.level(tx_request));
  b.send(0x02);
  CHECK_FALSE(r.level(tx_request));
  b.run_until(b.now() + 2 * byte_ps + period_ps / 2);
  REQUIRE(b.get("ISR", "TC") == 1);
  REQUIRE(b.get("ISR", "TXE") == 1);
  CHECK_FALSE(r.level(tx_request));
  CHECK_FALSE(r.level(evc_request));
  b.set("I2C_AUTOCR", "TCDMAEN", 1);
  CHECK(r.level(evc_request));
  b.command(0x38, 1, true, false, true);
  CHECK_FALSE(r.level(evc_request));
  b.run_until(b.now() + address_ps + byte_ps + period_ps / 2);
  REQUIRE(b.get("ISR", "RXNE") == 1);
  REQUIRE(b.get("ISR", "TCR") == 1);
  CHECK_FALSE(r.level(rx_request));
  CHECK_FALSE(r.level(evc_request));
  b.set("I2C_AUTOCR", "TCRDMAEN", 1);
  CHECK(r.level(evc_request));
  b.set("CR1", "RXDMAEN", 1);
  CHECK(r.level(rx_request));
  b.set("CR1", "PE", 0);
  CHECK_FALSE(r.level(evc_request));
  CHECK_FALSE(r.level(rx_request));
}

TEST_CASE("clearing PE drops the TX request, reading RXDR drops the RX request")
{
  constexpr std::uint32_t rx_request = 12;
  constexpr std::uint32_t tx_request = 13;
  rig                     r{"idle", true};
  bus                     b{*r.f};
  b.controller();
  b.set("CR1", "TXDMAEN", 1);
  b.set("CR1", "RXDMAEN", 1);
  b.command(0x38, 1, false, true);
  b.run_until(b.now() + address_ps + period_ps / 2);
  REQUIRE(b.get("ISR", "TXIS") == 1);
  CHECK(r.level(tx_request));
  b.set("CR1", "PE", 0);
  CHECK_FALSE(r.level(tx_request));
  b.set("CR1", "PE", 1);
  b.command(0x38, 1, true, true);
  b.run_until(b.now() + address_ps + byte_ps + period_ps / 2);
  REQUIRE(b.get("ISR", "RXNE") == 1);
  CHECK(r.level(rx_request));
  CHECK(b.receive() == 0xFF);
  CHECK_FALSE(r.level(rx_request));
}

TEST_CASE("setting AUTOCR.TRIGEN logs the unmodelled trigger once, at the write")
{
  rig                           r{"idle"};
  bus                           b{*r.f};
  std::vector<core::log_record> logged;
  auto const                    sink =
      r.f->target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  b.controller();
  b.set("I2C_AUTOCR", "TRIGEN", 1);
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].kind == core::log_kind::model_warning);
  CHECK(logged[0].peripheral == "I2C1");
  CHECK(logged[0].message.contains("trigger"));
  b.set("I2C_AUTOCR", "TRIGPOL", 1);
  CHECK(logged.size() == 1);
}

namespace
{
struct injected
{
  periph::i2c_error error;
  std::string_view  flag;
  std::string_view  clear;
  std::uint32_t     bit;
};

constexpr std::array<injected, 3> injections{
    {
     {periph::i2c_error::bus_error, "BERR", "BERRCF", 1u << 8},
     {periph::i2c_error::arbitration_lost, "ARLO", "ARLOCF", 1u << 9},
     {periph::i2c_error::overrun, "OVR", "OVRCF", 1u << 10},
     }
};

constexpr std::uint32_t error_irq = 56;

void unpend(stm32_fixture const& f, std::uint32_t irq)
{
  REQUIRE(f.target->mem().write(0xe000'e280u + 4 * (irq / 32), 4, 1u << (irq % 32)).has_value());
}

auto data_events(std::vector<bus_event> const& events) -> std::size_t
{
  return static_cast<std::size_t>(std::ranges::count_if(
      events, [](bus_event const& e) { return e.kind == 'w' or e.kind == 'r'; }));
}
}

TEST_CASE("a target's BERR, ARLO or OVR sets its ISR flag at the end of the byte it answered")
{
  for(auto const& kind : injections)
  {
    for(auto const read : {false, true})
    {
      for(auto const at_call : {0u, 1u})
      {
        CAPTURE(kind.flag);
        CAPTURE(read);
        CAPTURE(at_call);
        rig r{"idle"};
        bus b{*r.f};
        b.controller();
        r.faulty->error   = kind.error;
        r.faulty->at_call = at_call;
        if(not read)
        {
          b.send(0x11);
        }
        b.command(0x33, 2, read, false);
        auto const start = b.now();
        auto const end   = start + address_ps + at_call * byte_ps;
        b.run_until(end - period_ps / 2);
        CHECK(b.get("ISR", kind.flag) == 0);
        if(not read and at_call == 1)
        {
          b.send(0x22);
        }
        b.run_until(end + period_ps / 2);
        CHECK(b.get("ISR", kind.flag) == 1);
        if(not read and at_call == 0)
        {
          b.send(0x22);
        }
        CHECK_FALSE(pending(*r.f, error_irq));
        b.set("CR1", "ERRIE", 1);
        CHECK(pending(*r.f, error_irq));
        unpend(*r.f, error_irq);
        CHECK(pending(*r.f, error_irq));
        if(kind.error == periph::i2c_error::arbitration_lost)
        {
          CHECK(b.get("CR2", "START") == 0);
          CHECK(b.get("ISR", "BUSY") == 1);
          CHECK(b.get("ISR", "NACKF") == 0);
          CHECK(b.get("ISR", "TXIS") == 0);
          CHECK(b.get("ISR", "RXNE") == 0);
          auto const seen = r.faulty->events.size();
          b.run_until(end + period_ps - period_ps / 4);
          CHECK(b.get("ISR", "BUSY") == 1);
          b.run_until(end + period_ps + period_ps / 4);
          CHECK(b.get("ISR", "BUSY") == 0);
          REQUIRE(r.faulty->events.size() == seen + 1);
          CHECK(r.faulty->events.back().kind == 'P');
          b.run_until(end + 4 * byte_ps);
          CHECK(r.faulty->events.size() == seen + 1);
          CHECK(data_events(r.faulty->events) == at_call);
          CHECK(b.get("ISR", "STOPF") == 0);
          CHECK(b.get("ISR", "TC") == 0);
          CHECK(b.get("ISR", "NACKF") == 0);
        }
        else
        {
          if(read)
          {
            if(at_call == 0)
            {
              b.run_until(start + address_ps + byte_ps + period_ps / 2);
            }
            CHECK(b.receive() == 0x5A);
          }
          b.run_until(start + address_ps + 2 * byte_ps + period_ps / 2);
          CHECK(b.get("ISR", "TC") == 1);
          CHECK(b.get("ISR", "BUSY") == 1);
          CHECK(data_events(r.faulty->events) == 2);
        }
        b.set("ICR", kind.clear, 1);
        CHECK(b.get("ISR", kind.flag) == 0);
        unpend(*r.f, error_irq);
        CHECK_FALSE(pending(*r.f, error_irq));
      }
    }
  }
}

TEST_CASE("a START written while a lost arbitration holds BUSY is sent at the other STOP")
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  r.faulty->error = periph::i2c_error::arbitration_lost;
  b.command(0x33, 1, true, true);
  auto const end = b.now() + address_ps;
  b.run_until(end + period_ps / 2);
  REQUIRE(b.get("ISR", "ARLO") == 1);
  b.command(0x38, 0, false, true);
  CHECK(b.get("CR2", "START") == 1);
  b.run_until(end + period_ps + address_ps + period_ps / 2);
  REQUIRE(r.faulty->events.size() == 2);
  CHECK(r.faulty->events[1] == bus_event{'P', 0, r.faulty->events[0].at + period_ps});
  REQUIRE(r.table->events.size() == 1);
  CHECK(r.table->events[0].kind == 'W');
  CHECK(r.table->events[0].at - r.faulty->events[1].at == address_ps);
  CHECK(b.get("ISR", "ARLO") == 1);
}

TEST_CASE("clearing PE inside a lost arbitration gives the target its STOP")
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  r.faulty->error = periph::i2c_error::arbitration_lost;
  b.command(0x33, 1, true, true);
  auto const end = b.now() + address_ps;
  b.run_until(end + period_ps / 2);
  REQUIRE(b.get("ISR", "BUSY") == 1);
  auto const written = b.now();
  b.set("CR1", "PE", 0);
  CHECK(b.get("ISR", "BUSY") == 0);
  CHECK(b.get("ISR", "ARLO") == 0);
  b.run_until(end + 4 * period_ps);
  REQUIRE(r.faulty->events.size() == 2);
  CHECK(r.faulty->events[1] == bus_event{'P', 0, written});
  b.set("CR1", "PE", 1);
  CHECK(b.get("ISR", "BUSY") == 0);
}

TEST_CASE("CR2.STOP set inside a lost arbitration is cleared at its end")
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  r.faulty->error = periph::i2c_error::arbitration_lost;
  b.command(0x33, 1, true, true);
  auto const end = b.now() + address_ps;
  b.run_until(end + period_ps / 2);
  REQUIRE(b.get("ISR", "ARLO") == 1);
  b.command(0x38, 2, false, true);
  b.set("CR2", "STOP", 1);
  CHECK(b.get("CR2", "STOP") == 1);
  auto const freed = end + period_ps;
  b.run_until(freed + period_ps / 2);
  CHECK(b.get("CR2", "STOP") == 0);
  b.run_until(freed + address_ps + period_ps / 2);
  b.send(0x10);
  b.run_until(freed + address_ps + byte_ps + period_ps / 2);
  b.send(0x20);
  b.run_until(freed + address_ps + 2 * byte_ps + 2 * period_ps);
  CHECK(
      r.table->events
      == std::vector<bus_event>{
          {'W', 0,    r.table->events[0].at},
          {'w', 0x10, r.table->events[1].at},
          {'w', 0x20, r.table->events[2].at},
          {'P', 0,    r.table->events[3].at}
  });
}

TEST_CASE("the DMA requests after an injected error follow TXIS and RXNE as without one")
{
  constexpr std::uint32_t rx_request = 12;
  constexpr std::uint32_t tx_request = 13;
  for(auto const& kind : injections)
  {
    CAPTURE(kind.flag);
    auto const lost = kind.error == periph::i2c_error::arbitration_lost;
    {
      rig r{"idle", true};
      bus b{*r.f};
      b.controller();
      r.faulty->error   = kind.error;
      r.faulty->at_call = 1;
      b.set("CR1", "TXDMAEN", 1);
      b.command(0x33, 3, false, false);
      auto const start = b.now();
      b.run_until(start + address_ps + period_ps / 2);
      REQUIRE(r.level(tx_request));
      b.send(0x11);
      CHECK(r.level(tx_request));
      b.run_until(start + address_ps + byte_ps + period_ps / 2);
      REQUIRE(b.get("ISR", kind.flag) == 1);
      CHECK(r.level(tx_request) == not lost);
      CHECK(b.get("ISR", "TXIS") == (lost ? 0u : 1u));
    }
    {
      rig r{"idle", true};
      bus b{*r.f};
      b.controller();
      r.faulty->error   = kind.error;
      r.faulty->at_call = 1;
      b.set("CR1", "RXDMAEN", 1);
      b.command(0x33, 2, true, false);
      auto const start = b.now();
      b.run_until(start + address_ps + byte_ps + period_ps / 2);
      REQUIRE(b.get("ISR", kind.flag) == 1);
      CHECK(r.level(rx_request) == not lost);
      CHECK(b.get("ISR", "RXNE") == (lost ? 0u : 1u));
    }
  }
}

TEST_CASE("a snapshot before the erring byte or inside the lost BUSY restores the same flags")
{
  struct probe
  {
    std::uint32_t flag;
    std::uint32_t busy;
  };
  for(auto const& kind : injections)
  {
    for(auto const inside : {false, true})
    {
      CAPTURE(kind.flag);
      CAPTURE(inside);
      if(inside and kind.error != periph::i2c_error::arbitration_lost)
      {
        continue;
      }
      rig r{"idle"};
      bus b{*r.f};
      b.controller();
      r.faulty->error   = kind.error;
      r.faulty->at_call = 1;
      b.command(0x33, 2, true, true);
      auto const start = b.now();
      auto const end   = start + address_ps + byte_ps;
      b.run_until(inside ? end + period_ps / 2 : end - period_ps / 2);
      auto const saved = r.f->target->snapshot();
      auto const mark  = r.faulty->events.size();
      auto const look  = [](bus const& on) {
        return probe{
            .flag = on.get("ISR", "BERR") | on.get("ISR", "ARLO") << 1 | on.get("ISR", "OVR") << 2,
            .busy = on.get("ISR", "BUSY")};
      };
      std::vector<std::uint32_t> trace;
      auto const                 sample = [&](bus const& on, std::vector<std::uint32_t>& out) {
        for(auto const at :
            {end + 3 * period_ps / 4, end + period_ps + period_ps / 4,
             end + byte_ps + period_ps / 4, end + 3 * byte_ps})
        {
          on.run_until(at);
          auto const seen = look(on);
          out.push_back(seen.flag);
          out.push_back(seen.busy);
        }
      };
      sample(b, trace);
      auto const tail = std::vector(
          r.faulty->events.begin() + static_cast<std::ptrdiff_t>(mark), r.faulty->events.end());

      rig other{"idle"};
      other.faulty->error   = kind.error;
      other.faulty->at_call = 1;
      other.f->target->restore(
          core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
      bus                        o{*other.f};
      std::vector<std::uint32_t> again;
      sample(o, again);
      CHECK(again == trace);
      CHECK(other.faulty->events == tail);
      CHECK(trace[0] == kind.bit >> 8);
    }
  }
}

TEST_CASE("firmware serves each injected error in I2C1_ER_IRQHandler, clears it and retries")
{
  for(auto const& kind : injections)
  {
    for(auto const read : {false, true})
    {
      CAPTURE(kind.flag);
      CAPTURE(read);
      rig r{read ? "read_errors" : "errors"};
      r.faulty->error   = kind.error;
      r.faulty->at_call = 1;
      auto const log    = r.finish();
      REQUIRE(log.size() == (read ? 8u : 6u));
      CHECK(log[0] == 2);
      CHECK(log[1] == 1);
      CHECK(log[2] == kind.bit);
      CHECK(log[3] == (kind.error == periph::i2c_error::arbitration_lost ? 0u : 1u));
      CHECK(log[4] == 0);
      CHECK(log[5] == 0);
      auto const& events = r.faulty->events;
      REQUIRE(events.size() >= 4);
      auto const retry = std::vector(events.end() - 4, events.end());
      CHECK(retry[0].kind == (read ? 'R' : 'W'));
      CHECK(retry[3].kind == 'P');
      if(read)
      {
        CHECK(retry[1].kind == 'r');
        CHECK(retry[2].kind == 'r');
        CHECK(log[6] == 0x5A);
        CHECK(log[7] == 0x5A);
      }
      else
      {
        CHECK(retry[1] == bus_event{'w', 0xA5, retry[1].at});
        CHECK(retry[2] == bus_event{'w', 0x5A, retry[2].at});
      }
    }
  }
}

namespace
{
constexpr std::uint64_t stretch_ps = 10 * us;

auto served(bus const& b, rig const& r, std::uint64_t until) -> std::vector<bus_event>
{
  std::uint8_t next = 0x10;
  for(auto at = b.now() + period_ps / 2; at < until; at += period_ps / 2)
  {
    b.run_until(at);
    if(b.get("ISR", "TXIS") == 1)
    {
      b.send(next++);
    }
    if(b.get("ISR", "RXNE") == 1)
    {
      std::ignore = b.receive();
    }
  }
  return r.faulty->events;
}

auto stretched_transfer(bool read, std::uint32_t call) -> std::vector<bus_event>
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  r.faulty->stretch_call = call;
  r.faulty->stretch_ps   = stretch_ps;
  if(not read)
  {
    b.send(0x0F);
  }
  b.command(0x33, 3, read, true);
  return served(b, r, b.now() + address_ps + 3 * byte_ps + period_ps + 2 * stretch_ps);
}
}

TEST_CASE("a target stretching SCL after a byte delays every later phase by the stretch")
{
  for(auto const read : {false, true})
  {
    auto const plain = stretched_transfer(read, ~0u);
    REQUIRE(plain.size() == 5);
    CHECK(plain[4].kind == 'P');
    for(std::uint32_t call = 0; call < 4; ++call)
    {
      CAPTURE(read);
      CAPTURE(call);
      auto const events = stretched_transfer(read, call);
      REQUIRE(events.size() == plain.size());
      for(std::size_t index = 0; index < events.size(); ++index)
      {
        CAPTURE(index);
        CHECK(events[index].kind == plain[index].kind);
        CHECK(events[index].at == plain[index].at + (index > call ? stretch_ps : 0));
      }
    }
  }
}

TEST_CASE("SCL is released once both the stretching target and the waiting controller let go")
{
  for(auto const late : {false, true})
  {
    CAPTURE(late);
    auto const release = late ? stretch_ps + 3 * period_ps : stretch_ps / 2;
    {
      rig r{"idle"};
      bus b{*r.f};
      b.controller();
      r.faulty->stretch_call = 1;
      r.faulty->stretch_ps   = stretch_ps;
      b.send(0x10);
      b.command(0x33, 2, false, true);
      auto const end = b.now() + address_ps + byte_ps;
      b.run_until(end + release);
      b.send(0x20);
      b.run_until(end + release + stretch_ps + byte_ps + 2 * period_ps);
      REQUIRE(r.faulty->events.size() == 4);
      CHECK(
          r.faulty->events[2].at - r.faulty->events[1].at
          == std::max(release, stretch_ps) + byte_ps);
    }
    {
      rig r{"idle"};
      bus b{*r.f};
      b.controller();
      r.faulty->stretch_call = 2;
      r.faulty->stretch_ps   = stretch_ps;
      b.command(0x33, 3, true, true);
      auto const end = b.now() + address_ps + 2 * byte_ps;
      b.run_until(end + release);
      CHECK(b.receive() == 0x5A);
      CHECK(b.receive() == 0x5A);
      b.run_until(end + release + stretch_ps + byte_ps + 2 * period_ps);
      REQUIRE(r.faulty->events.size() == 5);
      CHECK(
          r.faulty->events[3].at - r.faulty->events[2].at
          == std::max(release, stretch_ps) + byte_ps);
    }
    {
      rig r{"idle"};
      bus b{*r.f};
      b.controller();
      r.faulty->stretch_call = 1;
      r.faulty->stretch_ps   = stretch_ps;
      b.command(0x33, 1, true, false);
      auto const end = b.now() + address_ps + byte_ps;
      b.run_until(end + release);
      REQUIRE(b.get("ISR", "TC") == 1);
      b.set("CR2", "STOP", 1);
      b.run_until(end + release + stretch_ps + 2 * period_ps);
      REQUIRE(r.faulty->events.size() == 3);
      CHECK(
          r.faulty->events[2].at - r.faulty->events[1].at
          == std::max(release, stretch_ps) + period_ps);
    }
  }
}

namespace
{
constexpr std::uint64_t hold_ps = 50 * us;

auto bus_of(rig const& r) -> stm32::i2c_bus&
{
  auto* const held = dynamic_cast<stm32::i2c_bus*>(r.f->target->model("I2C1"));
  REQUIRE(held != nullptr);
  return *held;
}

void hold(rig const& r, std::uint64_t from, std::uint64_t until)
{
  bus_of(r).hold(vtime{from}, vtime{until});
}
}

TEST_CASE("a hold on an idle bus reads BUSY until its end and delays a START to it")
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  auto const t1 = b.now() + 10 * us;
  auto const t2 = t1 + hold_ps;
  hold(r, t1, t2);
  b.run_until(t1 - period_ps);
  CHECK(b.get("ISR", "BUSY") == 0);
  b.run_until(t1 + period_ps);
  CHECK(b.get("ISR", "BUSY") == 1);
  b.command(0x38, 0, false, true);
  CHECK(b.get("CR2", "START") == 1);
  b.run_until(t2 - period_ps);
  CHECK(b.get("ISR", "BUSY") == 1);
  CHECK(b.get("CR2", "START") == 1);
  CHECK(r.table->events.empty());
  b.run_until(t2 + address_ps + 2 * period_ps);
  REQUIRE(r.table->events.size() == 2);
  CHECK(r.table->events[0] == bus_event{'W', 0, t2 + address_ps});
  CHECK(b.get("ISR", "BUSY") == 0);
  CHECK(b.get("ISR", "STOPF") == 1);
}

TEST_CASE("a hold without a START releases BUSY at its end")
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  auto const t1 = b.now();
  hold(r, t1, t1 + hold_ps);
  CHECK(b.get("ISR", "BUSY") == 0);
  b.run_until(t1 + period_ps);
  CHECK(b.get("ISR", "BUSY") == 1);
  b.run_until(t1 + hold_ps + period_ps);
  CHECK(b.get("ISR", "BUSY") == 0);
  CHECK(r.f->target->pending_events() == 0);
}

TEST_CASE("a hold due during a transfer starts at its STOP and keeps its end")
{
  for(auto const over : {false, true})
  {
    CAPTURE(over);
    rig r{"idle"};
    bus b{*r.f};
    b.controller();
    b.command(0x38, 0, false, true);
    auto const stop  = b.now() + address_ps + period_ps;
    auto const until = over ? b.now() + address_ps : stop + hold_ps;
    hold(r, b.now() + period_ps, until);
    b.run_until(stop - period_ps / 2);
    CHECK(b.get("ISR", "BUSY") == 1);
    b.run_until(stop + period_ps / 2);
    CHECK(b.get("ISR", "STOPF") == 1);
    CHECK(b.get("ISR", "BUSY") == (over ? 0u : 1u));
    b.set("ICR", "STOPCF", 1);
    b.command(0x38, 0, false, true);
    b.run_until(stop + hold_ps + address_ps + 2 * period_ps);
    REQUIRE(r.table->events.size() == 4);
    CHECK(r.table->events[2].at == (over ? stop + period_ps / 2 : stop + hold_ps) + address_ps);
  }
}

TEST_CASE("clearing PE inside a hold reads BUSY 0 and setting it again reads BUSY 1")
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  auto const t1 = b.now();
  hold(r, t1, t1 + hold_ps);
  b.run_until(t1 + period_ps);
  REQUIRE(b.get("ISR", "BUSY") == 1);
  b.set("CR1", "PE", 0);
  CHECK(b.get("ISR", "BUSY") == 0);
  b.run_until(t1 + 2 * period_ps);
  b.set("CR1", "PE", 1);
  CHECK(b.get("ISR", "BUSY") == 1);
  b.command(0x38, 0, false, true);
  b.run_until(t1 + hold_ps + address_ps + 2 * period_ps);
  REQUIRE(r.table->events.size() == 2);
  CHECK(r.table->events[0].at == t1 + hold_ps + address_ps);
}

TEST_CASE("a hold due during a transfer that PE clears starts at the clear")
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  b.command(0x38, 2, false, false);
  auto const t1 = b.now();
  hold(r, t1 + period_ps, t1 + hold_ps);
  b.run_until(t1 + 2 * period_ps);
  CHECK(b.get("ISR", "BUSY") == 1);
  b.set("CR1", "PE", 0);
  CHECK(b.get("ISR", "BUSY") == 0);
  b.set("CR1", "PE", 1);
  CHECK(b.get("ISR", "BUSY") == 1);
  b.run_until(t1 + hold_ps + period_ps);
  CHECK(b.get("ISR", "BUSY") == 0);
}

TEST_CASE("a second hold replaces a pending one and leaves no stale event")
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  auto const base = r.f->target->pending_events();
  auto const t1   = b.now() + 10 * us;
  for(std::uint64_t index = 0; index < 1000; ++index)
  {
    hold(r, t1 + index, t1 + hold_ps + index);
  }
  CHECK(r.f->target->pending_events() == base + 2);
  hold(r, t1 + 2 * hold_ps, t1 + 3 * hold_ps);
  b.run_until(t1 + period_ps);
  CHECK(b.get("ISR", "BUSY") == 0);
  b.run_until(t1 + 2 * hold_ps + period_ps);
  CHECK(b.get("ISR", "BUSY") == 1);
  b.run_until(t1 + 3 * hold_ps + period_ps);
  CHECK(b.get("ISR", "BUSY") == 0);
  CHECK(r.f->target->pending_events() == base);
}

TEST_CASE("a hold replacing one that owns the bus takes over at its start or after the old end")
{
  for(auto const gap : {false, true})
  {
    CAPTURE(gap);
    rig r{"idle"};
    bus b{*r.f};
    b.controller();
    auto const t1 = b.now();
    hold(r, t1, t1 + hold_ps);
    b.run_until(t1 + period_ps);
    REQUIRE(b.get("ISR", "BUSY") == 1);
    auto const from = gap ? t1 + 2 * hold_ps : t1 + hold_ps / 2;
    hold(r, from, from + hold_ps);
    b.run_until(t1 + hold_ps + period_ps);
    CHECK(b.get("ISR", "BUSY") == (gap ? 0u : 1u));
    b.run_until(from + period_ps);
    CHECK(b.get("ISR", "BUSY") == 1);
    b.run_until(from + hold_ps + period_ps);
    CHECK(b.get("ISR", "BUSY") == 0);
    CHECK(r.f->target->pending_events() == 0);
  }
}

TEST_CASE("a replacement starting at the end of the owning hold keeps a queued START waiting")
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  auto const t1 = b.now();
  auto const t2 = t1 + hold_ps;
  hold(r, t1, t2);
  b.run_until(t1 + period_ps);
  b.command(0x38, 0, false, true);
  hold(r, t2, t2 + hold_ps);
  b.run_until(t2 + hold_ps - period_ps);
  CHECK(b.get("ISR", "BUSY") == 1);
  CHECK(r.table->events.empty());
  b.run_until(t2 + hold_ps + address_ps + 2 * period_ps);
  CHECK(b.get("ISR", "BUSY") == 0);
  REQUIRE(r.table->events.size() == 2);
  CHECK(r.table->events[0].at == t2 + hold_ps + address_ps);
  CHECK(r.f->target->pending_events() == 0);
}

TEST_CASE("a snapshot inside a hold restores BUSY and the delayed START")
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  auto const t1 = b.now();
  auto const t2 = t1 + hold_ps;
  hold(r, t1 + period_ps, t2);
  b.run_until(t1 + 2 * period_ps);
  b.command(0x38, 0, false, true);
  auto const saved = r.f->target->snapshot();

  rig other{"idle"};
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  bus o{*other.f};
  CHECK(o.get("ISR", "BUSY") == 1);
  o.run_until(t2 - period_ps);
  CHECK(o.get("ISR", "BUSY") == 1);
  o.run_until(t2 + address_ps + 2 * period_ps);
  REQUIRE(other.table->events.size() == 2);
  CHECK(other.table->events[0].at == t2 + address_ps);
}

TEST_CASE("a hold due at the instant of a STOP comes after it however early it was posted")
{
  std::vector<std::vector<bus_event>> seen;
  for(auto const early : {true, false})
  {
    CAPTURE(early);
    rig r{"idle"};
    bus b{*r.f};
    b.controller();
    b.command(0x38, 0, false, true);
    auto const stop = b.now() + address_ps + period_ps;
    if(early)
    {
      hold(r, stop, stop + hold_ps);
    }
    b.run_until(stop - period_ps / 2);
    b.command(0x50, 0, false, true);
    if(not early)
    {
      hold(r, stop, stop + hold_ps);
    }
    b.run_until(stop + hold_ps + 2 * address_ps);
    REQUIRE_FALSE(r.memory->events.empty());
    CHECK(r.memory->events[0] == bus_event{'W', 0, stop + address_ps});
    seen.push_back(r.memory->events);
  }
  CHECK(seen[0] == seen[1]);
}

TEST_CASE("a hold due inside a lost arbitration starts at the other controller's STOP")
{
  rig r{"idle"};
  bus b{*r.f};
  b.controller();
  r.faulty->error = periph::i2c_error::arbitration_lost;
  b.command(0x33, 1, true, true);
  auto const end = b.now() + address_ps;
  hold(r, end + period_ps / 2, end + hold_ps);
  b.run_until(end + period_ps / 4);
  REQUIRE(b.get("ISR", "ARLO") == 1);
  b.command(0x38, 0, false, true);
  b.run_until(end + period_ps + period_ps / 2);
  CHECK(b.get("ISR", "BUSY") == 1);
  CHECK(r.table->events.empty());
  b.run_until(end + hold_ps + address_ps + 2 * period_ps);
  REQUIRE(r.table->events.size() == 2);
  CHECK(r.table->events[0].at == end + hold_ps + address_ps);
}
