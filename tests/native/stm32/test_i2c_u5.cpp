#include "../support/inert_model.hpp"
#include "dma.hpp"
#include "i2c_link_port.hpp"
#include "i2c_rows.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
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
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
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
  std::shared_ptr<std::vector<std::tuple<periph::i2c_target const*, vtime, std::size_t>>> cuts;
  bool          throw_at_cut{};
  char          throw_on{};
  std::uint32_t throw_nth{};
  std::uint32_t seen{};

  void maybe_throw(char kind)
  {
    if(kind == throw_on and ++seen == throw_nth)
    {
      throw std::runtime_error{"hook"};
    }
  }

  [[nodiscard]] auto address() const -> std::uint8_t override
  {
    return inner->address();
  }

  auto matches(std::uint8_t sent, vtime at) -> bool override
  {
    maybe_throw('m');
    return inner->matches(sent, at);
  }

  auto start(bool read, vtime at) -> periph::i2c_reply override
  {
    maybe_throw('s');
    auto const reply = inner->start(read, at);
    events.push_back({reply.ack ? (read ? 'R' : 'W') : 'N', 0, at.ps});
    return reply;
  }

  auto write(std::uint8_t byte, vtime at) -> periph::i2c_reply override
  {
    maybe_throw('w');
    auto const reply = inner->write(byte, at);
    events.push_back({reply.ack ? 'w' : 'n', byte, at.ps});
    return reply;
  }

  auto read(vtime at) -> periph::i2c_read_reply override
  {
    maybe_throw('r');
    auto const reply = inner->read(at);
    events.push_back({'r', reply.byte, at.ps});
    return reply;
  }

  void stop(vtime at) override
  {
    maybe_throw('p');
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

  void power_cut(vtime at) override
  {
    inner->power_cut(at);
    if(cuts)
    {
      cuts->emplace_back(this, at, resets ? resets->size() : 0);
    }
    if(throw_at_cut)
    {
      throw std::runtime_error{"cut"};
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

class request_probe final : public popcorn::testing::inert_model, public stm32::dma_controller
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
  std::shared_ptr<std::vector<std::tuple<periph::i2c_target const*, vtime, std::size_t>>> cuts =
      std::make_shared<std::vector<std::tuple<periph::i2c_target const*, vtime, std::size_t>>>();
  stm32::board                   board{};
  std::unique_ptr<stm32_fixture> f;

  explicit rig(
      std::string_view                  entry,
      bool                              probe   = false,
      popcorn::part_description         desc    = u575(),
      std::optional<stm32::i2c_variant> variant = std::nullopt)
  {
    memory->resets    = resets;
    table->resets     = resets;
    picky->resets     = resets;
    memory->cuts      = cuts;
    table->cuts       = cuts;
    picky->cuts       = cuts;
    board.i2c["I2C1"] = {memory, table, picky, faulty};
    auto options      = popcorn::testing::stm32_options();
    options.models    = stm32::factories(board, desc);
    if(variant)
    {
      options.models.insert_or_assign(
          "I2C1", stm32::i2c_factory(board.i2c, board.i2c_controllers, *variant));
    }
    if(probe)
    {
      options.models.insert_or_assign(
          "GPDMA1",
          [](popcorn::periph::model_context&,
             popcorn::peripheral_desc const&) -> std::shared_ptr<popcorn::periph::model> {
            return std::make_shared<request_probe>();
          });
    }
    f = std::make_unique<stm32_fixture>(std::move(desc), "i2c_ll", std::move(options));
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
  CHECK(r.cuts->empty());
}

TEST_CASE("I2C1RST resets the controller and leaves its board targets alone")
{
  rig r{"eeprom_rw"};
  r.resets->clear();
  auto&       regs = r.f->target->peripheral("I2C1");
  auto const* at   = popcorn::find_register(regs.description(), "TIMINGR");
  REQUIRE(at != nullptr);
  regs.poke(at->offset, 0x1234);
  rcc_reset_pulse(*r.f, "RCC_APB1RSTR1", "I2C1RST");
  CHECK(regs.peek(at->offset) == 0);
  CHECK(r.resets->empty());
  CHECK(r.cuts->empty());
  r.f->target->reset(popcorn::reset_kind::software);
  CHECK(r.resets->size() == 3);
}

TEST_CASE("a power cycle cuts each target in attachment order, then resets each at power-on")
{
  using popcorn::reset_kind;
  rig r{"eeprom_rw"};
  r.f->target->reset(reset_kind::software);
  auto const at = r.f->target->virtual_clock().since_power_on();
  REQUIRE(at > r.f->target->virtual_clock().time());
  r.resets->clear();
  r.f->target->power_cycle();
  CHECK(
      *r.cuts
      == std::vector<std::tuple<periph::i2c_target const*, vtime, std::size_t>>{
          {r.memory.get(), at, 0},
          {r.table.get(),  at, 0},
          {r.picky.get(),  at, 0}
  });
  CHECK(
      *r.resets
      == std::vector<std::pair<periph::i2c_target const*, reset_kind>>{
          {r.memory.get(), reset_kind::power_on},
          {r.table.get(),  reset_kind::power_on},
          {r.picky.get(),  reset_kind::power_on}
  });
}

TEST_CASE("a target that throws at the cut does not stop the cut of the later ones")
{
  using popcorn::reset_kind;
  rig r{"eeprom_rw"};
  r.memory->throw_at_cut = true;
  r.resets->clear();
  CHECK_THROWS_AS(r.f->target->power_cycle(), std::runtime_error);
  REQUIRE(r.cuts->size() == 3);
  CHECK(std::get<0>((*r.cuts)[2]) == r.picky.get());
  CHECK(r.resets->size() == 3);
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

TEST_CASE("without AUTOCR, TC and TCR leave the I2C1 event DMA request low")
{
  constexpr std::uint32_t evc_request = 14;
  auto                    desc        = u575();
  auto const              i2c1        = std::ranges::find(
      desc.peripherals, std::string_view{"I2C1"}, &popcorn::peripheral_desc::name);
  REQUIRE(i2c1 != desc.peripherals.end());
  REQUIRE(
      std::erase_if(
          i2c1->registers, [](popcorn::register_desc const& reg) { return reg.name == "AUTOCR"; })
      == 1);
  rig r{"idle", true, std::move(desc), stm32::h5_i2c};
  bus b{*r.f};
  b.controller();
  b.command(0x38, 2, false, false);
  b.run_until(b.now() + address_ps + period_ps / 2);
  REQUIRE(b.get("ISR", "TXIS") == 1);
  b.send(0x40);
  b.send(0x02);
  b.run_until(b.now() + 2 * byte_ps + period_ps / 2);
  REQUIRE(b.get("ISR", "TC") == 1);
  CHECK_FALSE(r.level(evc_request));
  b.command(0x38, 1, true, false, true);
  b.run_until(b.now() + address_ps + byte_ps + period_ps / 2);
  REQUIRE(b.get("ISR", "TCR") == 1);
  CHECK_FALSE(r.level(evc_request));
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

TEST_CASE("I2C1RST lowers the TX request on the controller and ignores CR1 while held")
{
  constexpr std::uint32_t tx_request = 13;
  rig                     r{"idle", true};
  bus                     b{*r.f};
  b.controller();
  b.command(0x38, 2, false, false);
  b.run_until(b.now() + address_ps + period_ps / 2);
  b.set("CR1", "TXDMAEN", 1);
  REQUIRE(r.level(tx_request));
  rcc_reset_pulse(*r.f, "RCC_APB1RSTR1", "I2C1RST");
  CHECK_FALSE(r.level(tx_request));
  auto const& desc  = r.f->target->peripheral("RCC").description();
  auto const* reset = popcorn::find_register(desc, "RCC_APB1RSTR1");
  REQUIRE(reset != nullptr);
  auto const held = std::uint32_t{1} << 21;
  REQUIRE(r.f->target->mem().write(desc.base + reset->offset, 4, held).has_value());
  b.set("CR1", "PE", 1);
  REQUIRE(r.f->target->mem().write(desc.base + reset->offset, 4, 0).has_value());
  CHECK(b.get("CR1", "PE") == 0);
}

TEST_CASE("I2C1RST moves the phase generation past every event queued before it, twice")
{
  rig                       r{"idle", true};
  bus                       b{*r.f};
  std::vector<std::int64_t> seen;
  for(int pulse = 0; pulse < 2; ++pulse)
  {
    b.controller();
    b.command(0x38, 2, false, false);
    b.run_until(b.now() + period_ps);
    auto const before = popcorn::testing::saved_integer(*r.f, "I2C1", "generation");
    REQUIRE(before.has_value());
    REQUIRE(popcorn::testing::queued_event(*r.f, "I2C1", 0, static_cast<std::uint64_t>(*before)));
    seen.push_back(*before);
    rcc_reset_pulse(*r.f, "RCC_APB1RSTR1", "I2C1RST");
    auto const after = popcorn::testing::saved_integer(*r.f, "I2C1", "generation");
    REQUIRE(after.has_value());
    CHECK(*after > *before);
  }
  CHECK(seen[1] > seen[0]);
}

namespace
{
constexpr std::uint32_t wire       = 1;
constexpr std::uint64_t target_ps  = 9 * period_ps;
constexpr std::uint8_t  own        = 0x42;
constexpr std::uint32_t port_event = 3;
constexpr std::uint32_t addr_log   = 0xADD0'0000;
constexpr std::uint32_t stop_log   = 0x5709;

struct target_part
{
  std::string_view name;
  auto (*desc)() -> popcorn::part_description const&;
  std::string_view firmware;
  std::string_view enable;
  std::string_view reset;
  std::uint32_t    event_irq;
  bool             autocr;
};

constexpr std::array<target_part, 2> target_parts{
    {
     {"STM32U575", &u575, "i2c_ll", "RCC_APB1ENR1", "RCC_APB1RSTR1", 55, true},
     {"STM32H573", &popcorn::testing::h573, "h573_i2c_ll", "RCC_APB1LENR", "RCC_APB1LRSTR", 51,
         false},
     }
};

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

  std::vector<heard>              answers;
  std::vector<lost>               drops;
  std::vector<std::string>        log;
  std::move_only_function<void()> on_answer;

  void answered(std::uint64_t unit, periph::i2c_phase phase, periph::i2c_answer answer, vtime at)
      override
  {
    answers.push_back({unit, phase, answer.ack, answer.byte, at.ps});
    log.push_back(std::format("answer {} {}", unit, answer.ack ? "ack" : "nack"));
    if(on_answer)
    {
      on_answer();
    }
  }

  void dropped(std::uint64_t unit, periph::i2c_phase phase, vtime at) override
  {
    drops.push_back({unit, phase, at.ps});
    log.push_back(std::format("dropped {}", unit));
  }
};

using heard = phase_probe::heard;
using lost  = phase_probe::lost;

struct target_rig
{
  target_part                    part;
  std::shared_ptr<phase_probe>   sink = std::make_shared<phase_probe>();
  stm32::board                   board{};
  std::unique_ptr<stm32_fixture> f;
  std::vector<core::log_record>  logged;
  core::hook_handle              logs;

  explicit target_rig(
      target_part const& which      = target_parts[0],
      bool               strict     = false,
      bool               controlled = true,
      double             cpi        = 1.0)
      : part{which}
  {
    if(controlled)
    {
      board.i2c_controllers["I2C1"] = sink;
    }
    auto options               = popcorn::testing::stm32_options();
    options.cpi                = cpi;
    options.strict_peripherals = strict;
    options.models             = stm32::factories(board, part.desc());
    f    = std::make_unique<stm32_fixture>(part.desc(), part.firmware, std::move(options));
    logs = f->target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
    idle();
  }

  void idle() const
  {
    f->set("pc", sym(part.firmware, "idle"));
  }

  [[nodiscard]] auto b() const -> bus
  {
    return bus{*f};
  }

  [[nodiscard]] auto port() const -> periph::i2c_controller_port&
  {
    auto* const found = dynamic_cast<periph::i2c_controller_port*>(f->target->model("I2C1"));
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto period() const -> std::uint64_t
  {
    auto const hz = f->target->virtual_clock().instruction_hz();
    REQUIRE(1'000'000'000'000 % hz == 0);
    return 1'000'000'000'000 / hz;
  }

  [[nodiscard]] auto now() const -> std::uint64_t
  {
    return f->target->virtual_clock().time().ps;
  }

  void run_until(std::uint64_t ps) const
  {
    b().run_until(ps);
  }

  void listen(std::uint8_t address = own, bool stretch = false) const
  {
    auto const on = b();
    on.set("RCC", part.enable, "I2C1EN", 1);
    on.set("OAR1", "OA1EN", 0);
    on.set("OAR1", "OA1", std::uint32_t{address} << 1);
    on.set("OAR1", "OA1EN", 1);
    on.set("CR1", "ADDRACLR", stretch ? 0 : 1);
    on.set("CR1", "PE", 1);
  }

  [[nodiscard]] auto get(std::string_view reg, std::string_view field) const -> std::uint32_t
  {
    return f->field("I2C1", reg, field);
  }

  void start(std::uint8_t address, bool read, std::uint64_t at, std::uint64_t unit) const
  {
    port().start(address, read, target_ps, vtime{at}, wire, unit);
  }

  void write(std::uint8_t byte, std::uint64_t at, std::uint64_t unit) const
  {
    port().write(byte, target_ps, vtime{at}, wire, unit);
  }

  void read(bool ack, std::uint64_t at, std::uint64_t unit) const
  {
    port().read(ack, target_ps, vtime{at}, wire, unit);
  }

  void stop(std::uint64_t at, std::uint64_t unit) const
  {
    port().stop(vtime{at}, wire, unit);
  }

  void drop(periph::i2c_phase phase, std::uint64_t at, std::uint64_t unit) const
  {
    port().drop(phase, vtime{at}, wire, unit);
  }

  void reset(popcorn::reset_kind kind) const
  {
    f->target->reset(kind);
    idle();
  }

  void power_cycle() const
  {
    f->target->power_cycle();
    idle();
  }

  void rcc_pulse() const
  {
    popcorn::testing::rcc_reset_pulse(*f, part.reset, "I2C1RST");
  }

  [[nodiscard]] auto warnings(std::string_view reg) const -> std::size_t
  {
    return static_cast<std::size_t>(std::ranges::count_if(logged, [&](auto const& record) {
      return record.kind == core::log_kind::model_warning
         and record.peripheral == "I2C1"
         and record.reg == reg;
    }));
  }

  [[nodiscard]] auto model_bytes() const -> std::vector<std::byte>
  {
    std::vector<std::byte> out;
    f->target->model("I2C1")->save(out);
    return out;
  }

  void after_system_reset() const
  {
    run_until(now() + 30 * us);
    reset(popcorn::reset_kind::software);
    REQUIRE(
        f->target->virtual_clock().since_power_on().ps - f->target->virtual_clock().time().ps
        >= 30 * us);
  }
};

void write_timing(bus const& b)
{
  b.write(b.at("I2C1", "TIMINGR"), 0x0010'0305);
}

auto port_due(stm32_fixture const& f) -> std::vector<core::event>
{
  auto const& units = f.target->description().peripherals;
  auto const  found =
      std::ranges::find(units, std::string_view{"I2C1"}, &popcorn::peripheral_desc::name);
  REQUIRE(found != units.end());
  auto const               owner = static_cast<std::uint32_t>(found - units.begin());
  std::vector<core::event> out;
  for(auto const& event : f.target->snapshot().data().events.pending)
  {
    if(event.id.owner == owner and event.id.kind == port_event)
    {
      out.push_back(event);
    }
  }
  return out;
}

auto due_of_kind(stm32_fixture const& f, std::string_view instance, std::uint32_t kind)
    -> std::optional<std::uint64_t>
{
  auto const& units = f.target->description().peripherals;
  auto const  found = std::ranges::find(units, instance, &popcorn::peripheral_desc::name);
  REQUIRE(found != units.end());
  auto const owner = static_cast<std::uint32_t>(found - units.begin());
  for(auto const& event : f.target->snapshot().data().events.pending)
  {
    if(event.id.owner == owner and event.id.kind == kind)
    {
      return event.due_ps;
    }
  }
  return std::nullopt;
}

constexpr auto on_address = periph::i2c_phase::address;
constexpr auto on_write   = periph::i2c_phase::write;
constexpr auto on_read    = periph::i2c_phase::read;
}

TEST_CASE(
    "the target firmware answers OAR1, OAR2 under its mask and the general call, NACKs others")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    r.f->set("pc", sym(part.firmware, "target"));
    r.run_until(r.now() + 200 * us);
    REQUIRE(r.get("CR1", "PE") == 1);
    REQUIRE(r.get("OAR2", "OA2MSK") == 2);
    auto const    base = r.now() + us;
    auto          at   = base;
    std::uint64_t unit = 0;
    for(auto const& [address, read] : std::array<std::pair<std::uint8_t, bool>, 5>{
            {{own, false}, {0x61, true}, {0x64, false}, {0x00, false}, {0x7F, true}}
    })
    {
      r.start(address, read, at, ++unit);
      r.stop(at + target_ps, ++unit);
      at += 2 * target_ps;
    }
    r.run_until(at + 50 * us);
    std::vector<bool> acks;
    for(auto const& one : r.sink->answers)
    {
      acks.push_back(one.ack);
    }
    CHECK(acks == std::vector<bool>{true, true, false, true, false});
    CHECK(r.sink->answers[0].at > base + target_ps);
    CHECK(r.sink->answers[0].at < base + target_ps + 10 * us);
    CHECK(r.sink->drops.empty());
    CHECK(
        r.f->log()
        == std::vector<std::uint32_t>{
            addr_log | 0x84, stop_log, addr_log | 0xC3, stop_log, addr_log, stop_log});
    CHECK(r.get("ISR", "BUSY") == 0);
  }
}

TEST_CASE("the address match reads OAR1, OAR2 and GCEN at the phase end, masks and reserved")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    auto const b = r.b();
    r.listen();
    auto          at   = r.now() + us;
    std::uint64_t unit = 0;
    auto const    ask  = [&](std::uint8_t address) {
      r.start(address, false, at, ++unit);
      r.stop(at + target_ps, ++unit);
      at += 2 * target_ps;
      r.run_until(at);
      return r.sink->answers.back().ack;
    };
    CHECK(ask(own));
    CHECK_FALSE(ask(0x43));
    CHECK_FALSE(ask(0x00));
    b.set("CR1", "GCEN", 1);
    CHECK(ask(0x00));
    b.set("OAR2", "OA2", 0x10);
    b.set("OAR2", "OA2EN", 1);
    CHECK(ask(0x10));
    CHECK_FALSE(ask(0x11));
    b.set("OAR2", "OA2EN", 0);
    b.set("OAR2", "OA2MSK", 1);
    b.set("OAR2", "OA2EN", 1);
    CHECK(ask(0x11));
    CHECK_FALSE(ask(0x12));
    b.set("OAR2", "OA2EN", 0);
    b.set("OAR2", "OA2MSK", 7);
    b.set("OAR2", "OA2EN", 1);
    CHECK(ask(0x55));
    CHECK_FALSE(ask(0x78));
    CHECK_FALSE(ask(0x07));
    b.set("OAR2", "OA2EN", 0);
    b.set("OAR2", "OA2MSK", 0);
    b.set("OAR2", "OA2", 0x07);
    b.set("OAR2", "OA2EN", 1);
    CHECK(ask(0x07));
    r.listen(0x50);
    CHECK_FALSE(ask(own));
    CHECK(ask(0x50));
    b.set("OAR1", "OA1EN", 0);
    CHECK_FALSE(ask(0x50));
    CHECK(r.get("ISR", "ADDCODE") == 0x50);
  }
}

TEST_CASE("ADDR, DIR and ADDCODE are set at the address phase end, SCL held until ADDRCF")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    auto const b = r.b();
    r.listen(own, true);
    b.set("CR1", "ADDRIE", 1);
    auto const base = r.now() + us;
    auto const end  = base + target_ps;
    r.start(own, false, base, 1);
    r.write(0x11, base, 2);
    r.run_until(base + r.period());
    auto const queued = port_due(*r.f);
    REQUIRE(queued.size() == 2);
    CHECK(queued[0].due_ps == end);
    CHECK(queued[0].lane == wire);
    CHECK(r.get("ISR", "BUSY") == 1);
    r.run_until(end - r.period());
    CHECK_FALSE(pending(*r.f, part.event_irq));
    r.run_until(end + r.period());
    CHECK(pending(*r.f, part.event_irq));
    CHECK(r.get("ISR", "ADDR") == 1);
    CHECK(r.get("ISR", "DIR") == 0);
    CHECK(r.get("ISR", "ADDCODE") == own);
    CHECK(r.sink->answers.empty());
    r.run_until(end + 30 * us);
    CHECK(r.sink->answers.empty());
    CHECK(port_due(*r.f).empty());
    auto const release = r.now();
    b.set("ICR", "ADDRCF", 1);
    CHECK(r.get("ISR", "ADDR") == 0);
    auto const after = port_due(*r.f);
    REQUIRE(after.size() == 2);
    CHECK(after[0].due_ps == release);
    CHECK(after[1].due_ps == release);
    r.run_until(release + target_ps + us);
    CHECK(
        r.sink->answers
        == std::vector<heard>{
            {1, on_address, true, 0xFF, release            },
            {2, on_write,   true, 0xFF, release + target_ps}
    });
    CHECK(r.get("ISR", "RXNE") == 1);
  }
}

TEST_CASE("ADDRACLR keeps ADDR clear and STOPFACLR keeps STOPF clear, ADDCODE still set")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    auto const b = r.b();
    r.listen();
    b.set("CR1", "ADDRACLR", 1);
    b.set("CR1", "STOPFACLR", 1);
    auto const base = r.now() + us;
    r.start(own, false, base, 1);
    r.stop(base + target_ps, 2);
    r.run_until(base + 2 * target_ps);
    CHECK(r.sink->answers.at(0).ack);
    CHECK(r.get("ISR", "ADDR") == 0);
    CHECK(r.get("ISR", "ADDCODE") == own);
    CHECK(r.get("ISR", "STOPF") == 0);
    b.set("CR1", "STOPFACLR", 0);
    r.start(own, false, base + 2 * target_ps, 3);
    r.stop(base + 3 * target_ps, 4);
    r.run_until(base + 4 * target_ps);
    CHECK(r.get("ISR", "STOPF") == 1);
  }
}

TEST_CASE("PE clear, a transfer of the I2C as controller and the I2C held in reset answer NACK")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    auto const b = r.b();
    r.listen(own, true);
    b.set("CR1", "PE", 0);
    auto at = r.now() + us;
    r.start(own, false, at, 1);
    r.run_until(at + target_ps + us);
    CHECK_FALSE(r.sink->answers.back().ack);
    CHECK(r.get("ISR", "BUSY") == 0);
    write_timing(b);
    b.set("CR1", "PE", 1);
    b.command(0x38, 1, false, true);
    at = r.now();
    r.port().start(own, false, us, vtime{at}, wire, 2);
    r.run_until(at + 2 * us);
    REQUIRE(r.sink->answers.size() == 2);
    CHECK_FALSE(r.sink->answers.back().ack);
    CHECK(r.get("ISR", "ADDR") == 0);
    r.run_until(at + 200 * us);
    popcorn::testing::rcc_reset_write(*r.f, part.reset, "I2C1RST", 1);
    r.listen();
    at = r.now() + us;
    r.start(own, false, at, 3);
    r.run_until(at + target_ps + us);
    CHECK_FALSE(r.sink->answers.back().ack);
    popcorn::testing::rcc_reset_write(*r.f, part.reset, "I2C1RST", 0);
    CHECK(r.sink->answers.size() == 3);
  }
}

TEST_CASE("a port entry in the past keeps its own count, a bad argument or lane throws")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    r.listen();
    r.run_until(r.now() + 10 * us);
    auto const now  = r.now();
    auto const past = now - 3 * r.period();
    auto&      p    = r.port();
    CHECK_THROWS_AS(p.start(own, false, target_ps, vtime{now}, 0, 1), std::invalid_argument);
    CHECK_THROWS_AS(p.start(0x80, false, target_ps, vtime{now}, wire, 1), std::invalid_argument);
    CHECK_THROWS_AS(p.start(own, false, 0, vtime{now}, wire, 1), std::invalid_argument);
    CHECK_THROWS_AS(
        p.drop(static_cast<periph::i2c_phase>(3), vtime{now}, wire, 1), std::invalid_argument);
    CHECK(popcorn::testing::saved_integer(*r.f, "I2C1", "port.last_unit") == 0);
    p.start(own, false, target_ps, vtime{past}, wire, 1);
    p.drop(on_write, vtime{past}, wire, 2);
    CHECK_THROWS_AS(p.stop(vtime{now}, wire, 2), std::invalid_argument);
    auto const queued = port_due(*r.f);
    auto const keyed  = std::ranges::count_if(queued, [&](core::event const& event) {
      return event.due_ps == past
         and event.at.value + 3 == r.f->target->virtual_clock().now().value;
    });
    CHECK(keyed == 2);
    r.run_until(now + r.period());
    CHECK(
        r.sink->drops
        == std::vector<lost>{
            {2, on_write, past}
    });
    r.run_until(past + target_ps + us);
    CHECK(
        r.sink->answers
        == std::vector<heard>{
            {1, on_address, true, 0xFF, past + target_ps}
    });
  }
}

TEST_CASE("port primitives need a controller, and a controller next to targets is refused")
{
  rig         none{"idle"};
  auto* const p = dynamic_cast<periph::i2c_controller_port*>(none.f->target->model("I2C1"));
  REQUIRE(p != nullptr);
  CHECK_THROWS_AS(p->start(own, false, target_ps, vtime{0}, wire, 1), popcorn::config_error);
  CHECK_THROWS_AS(p->stop(vtime{0}, wire, 1), popcorn::config_error);
  stm32::board board{};
  board.i2c["I2C1"] = {std::make_shared<recorded_target>(std::make_shared<picky_target>())};
  board.i2c_controllers["I2C1"] = std::make_shared<phase_probe>();
  auto options                  = popcorn::testing::stm32_options();
  options.models                = stm32::factories(board, u575());
  CHECK_THROWS_AS(
      std::ignore = std::make_unique<stm32_fixture>(u575(), "i2c_ll", std::move(options)),
      popcorn::config_error);
}

TEST_CASE("a port phase end fires after a local event of the same count, posted either side")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    std::vector<std::vector<std::string>> orders;
    for(bool const port_first : {true, false})
    {
      CAPTURE(port_first);
      target_rig r{part};
      auto const b = r.b();
      r.listen();
      b.set("RCC", part.enable, "I2C2EN", 1);
      b.write(b.at("I2C2", "TIMINGR"), 0x0010'0305);
      b.set("I2C2", "CR1", "PE", 1);
      auto const command = [&] {
        b.write(b.at("I2C2", "CR2"), (0x10u << 1) | (1u << 13) | (1u << 25));
      };
      command();
      auto const due = due_of_kind(*r.f, "I2C2", 0);
      REQUIRE(due.has_value());
      auto const span = *due - r.now();
      r.run_until(r.now() + 4 * span);
      b.set("I2C2", "ICR", "NACKCF", 1);
      b.set("I2C2", "ICR", "STOPCF", 1);
      REQUIRE(r.f->field("I2C2", "ISR", "BUSY") == 0);
      std::vector<std::string> log;
      r.sink->on_answer = [&] {
        log.push_back(r.f->field("I2C2", "ISR", "NACKF") != 0 ? "local first" : "port first");
      };
      auto const base = r.now();
      if(port_first)
      {
        r.port().start(own, false, span + us, vtime{base}, wire, 1);
        r.run_until(base + us);
        REQUIRE(port_due(*r.f).size() == 1);
        command();
      }
      else
      {
        r.run_until(base + us);
        command();
        r.port().start(own, false, span + us, vtime{base}, wire, 1);
      }
      REQUIRE(due_of_kind(*r.f, "I2C2", 0) == base + us + span);
      r.run_until(base + 2 * us + span);
      REQUIRE(r.sink->answers.size() == 1);
      CHECK(r.sink->answers[0].at == base + us + span);
      orders.push_back(log);
    }
    CHECK(orders[0] == orders[1]);
    CHECK(orders[0] == std::vector<std::string>{"local first"});
  }
}

TEST_CASE("a STOP due at a phase end runs in unit order, posted before or after the start")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    std::vector<std::vector<std::string>> logs;
    for(bool const early : {true, false})
    {
      CAPTURE(early);
      target_rig r{part};
      r.listen();
      auto const base = r.now() + us;
      r.start(own, false, base, 1);
      if(not early)
      {
        r.run_until(base + target_ps / 2);
      }
      r.stop(base + target_ps, 2);
      r.run_until(base + 2 * target_ps);
      CHECK(r.get("ISR", "STOPF") == 1);
      logs.push_back(r.sink->log);
    }
    CHECK(logs[0] == logs[1]);
    CHECK(logs[0] == std::vector<std::string>{"answer 1 ack"});
  }
}

TEST_CASE("an RCC reset drops the started phase, the repeated START and STOP run in the reset I2C")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    r.listen();
    auto const base = r.now() + us;
    r.start(own, false, base, 1);
    r.start(own, true, base, 2);
    r.stop(base + 2 * target_ps, 3);
    r.run_until(base + target_ps / 2);
    auto const generation = popcorn::testing::saved_integer(*r.f, "I2C1", "port.generation");
    REQUIRE(generation.has_value());
    r.rcc_pulse();
    CHECK(popcorn::testing::saved_integer(*r.f, "I2C1", "port.generation") == *generation + 1);
    r.listen();
    r.run_until(base + 3 * target_ps);
    CHECK(r.sink->log == std::vector<std::string>{"dropped 1", "answer 2 ack"});
    CHECK(
        r.sink->drops
        == std::vector<lost>{
            {1, on_address, base + target_ps}
    });
    CHECK(r.sink->answers.at(0).at == base + 2 * target_ps);
    CHECK(r.get("ISR", "DIR") == 1);
    CHECK(r.get("ISR", "STOPF") == 1);
    CHECK(r.get("ISR", "BUSY") == 0);
  }
}

TEST_CASE("a phase started before an RCC reset is dropped, queued before or after the reset")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    std::vector<std::vector<std::string>> logs;
    std::vector<std::vector<lost>>        drops;
    std::vector<std::vector<std::byte>>   bytes;
    std::vector<std::vector<heard>>       answers;
    for(bool const after : {false, true})
    {
      CAPTURE(after);
      target_rig r{part};
      r.listen();
      auto const base  = r.now() + us;
      auto const queue = [&] {
        r.stop(base - us, 1);
        r.start(own, false, base, 2);
      };
      if(not after)
      {
        queue();
      }
      r.run_until(base + 3 * us);
      r.rcc_pulse();
      if(after)
      {
        queue();
      }
      r.start(own, false, base + 3 * us, 3);
      r.run_until(base + 3 * target_ps);
      logs.push_back(r.sink->log);
      drops.push_back(r.sink->drops);
      bytes.push_back(r.model_bytes());
      REQUIRE(r.sink->answers.size() == 1);
      CHECK(r.sink->answers[0].at == base + 2 * target_ps);
      answers.push_back(r.sink->answers);
    }
    CHECK(logs[0] == logs[1]);
    CHECK(drops[0] == drops[1]);
    CHECK(bytes[0] == bytes[1]);
    CHECK(logs[0] == std::vector<std::string>{"dropped 2", "answer 3 nack"});
    CHECK(answers[0] == answers[1]);
  }
}

TEST_CASE("a STOP or a drop queued after an RCC reset with a time before it is due at the reset")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    r.listen();
    auto const base = r.now() + us;
    r.run_until(base + 3 * us);
    auto const reset = r.now();
    r.rcc_pulse();
    r.drop(on_write, base, 1);
    r.stop(base, 2);
    auto const stops = port_due(*r.f);
    REQUIRE(stops.size() == 2);
    CHECK(stops[0].due_ps == reset);
    CHECK(stops[1].due_ps == reset);
    r.run_until(reset + r.period());
    CHECK(
        r.sink->drops
        == std::vector<lost>{
            {1, on_write, reset}
    });
    r.after_system_reset();
    r.drop(on_write, 0, 3);
    r.run_until(r.now() + r.period());
    CHECK(r.sink->drops.back() == lost{3, on_write, 0});
  }
}

TEST_CASE("every machine reset kind and a power cycle drop the queued phases at rebased times")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(auto const kind :
        {popcorn::reset_kind::software, popcorn::reset_kind::pin, popcorn::reset_kind::watchdog,
         popcorn::reset_kind::power_on, popcorn::reset_kind::brown_out})
    {
      for(bool const cycle : {false, true})
      {
        if(cycle and kind != popcorn::reset_kind::power_on)
        {
          continue;
        }
        CAPTURE(kind);
        CAPTURE(cycle);
        target_rig r{part};
        r.after_system_reset();
        r.listen();
        auto const base = r.now() + us;
        r.start(own, false, base, 1);
        r.write(0x11, base, 2);
        r.stop(base + 2 * target_ps, 3);
        r.run_until(base + target_ps / 2);
        auto const at_reset = r.now();
        if(cycle)
        {
          r.power_cycle();
        }
        else
        {
          r.reset(kind);
        }
        CHECK(r.sink->drops.empty());
        CHECK(r.sink->answers.empty());
        r.listen();
        r.run_until(base + 3 * target_ps - at_reset);
        CHECK(
            r.sink->drops
            == std::vector<lost>{
                {1, on_address, base + target_ps - at_reset    },
                {2, on_write,   base + 2 * target_ps - at_reset}
        });
        CHECK(r.sink->answers.empty());
        CHECK(r.get("ISR", "STOPF") == 0);
        CHECK(r.get("ISR", "BUSY") == 0);
      }
    }
  }
}

TEST_CASE("a machine reset removes a queued STOP, the next START starts at its own time")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    r.listen();
    auto const base = r.now() + us;
    r.start(own, false, base, 1);
    r.stop(base + target_ps + 80 * us, 2);
    r.run_until(base + target_ps + us);
    REQUIRE(r.sink->answers.size() == 1);
    r.reset(popcorn::reset_kind::software);
    r.listen();
    auto const next = r.now() + us;
    r.start(own, false, next, 3);
    r.run_until(next + target_ps + 100 * us);
    CHECK(r.sink->answers.back() == heard{3, on_address, true, 0xFF, next + target_ps});
    CHECK(r.get("ISR", "STOPF") == 0);
    CHECK(r.get("ISR", "BUSY") == 1);
  }
}

TEST_CASE("a STOP inside a written byte drops it at the STOP, sets STOPF and BERR")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    r.listen();
    auto const base = r.now() + us;
    r.start(own, false, base, 1);
    r.write(0x5A, base, 2);
    r.write(0x5B, base, 3);
    r.stop(base + target_ps + target_ps / 2, 4);
    r.run_until(base + 4 * target_ps);
    CHECK(r.sink->log == std::vector<std::string>{"answer 1 ack", "dropped 2", "dropped 3"});
    CHECK(
        r.sink->drops
        == std::vector<lost>{
            {2, on_write, base + target_ps + target_ps / 2},
            {3, on_write, base + target_ps + target_ps / 2}
    });
    CHECK(r.get("ISR", "STOPF") == 1);
    CHECK(r.get("ISR", "BERR") == 1);
    CHECK(r.get("ISR", "BUSY") == 0);
  }
}

TEST_CASE("a STOP ending the address phase drops it with no BERR, a drop reports at its time")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    r.listen();
    auto const base = r.now() + us;
    r.start(own, false, base, 1);
    r.stop(base + target_ps / 2, 2);
    r.port().drop(on_read, vtime{base + 3 * target_ps}, wire, 3);
    r.run_until(base + 4 * target_ps);
    CHECK(
        r.sink->drops
        == std::vector<lost>{
            {1, on_address, base + target_ps / 2},
            {3, on_read,    base + 3 * target_ps}
    });
    CHECK(r.get("ISR", "BERR") == 0);
    CHECK(r.get("ISR", "STOPF") == 0);
  }
}

TEST_CASE("a second reset before the re-armed entries fire reports each unit once")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    r.after_system_reset();
    r.listen();
    auto const base = r.now() + us;
    r.start(own, false, base, 1);
    r.write(0x11, base, 2);
    r.run_until(base + target_ps / 2);
    auto const at_reset = r.now();
    r.reset(popcorn::reset_kind::software);
    r.run_until(2 * us);
    r.rcc_pulse();
    r.run_until(base + 3 * target_ps);
    CHECK(
        r.sink->drops
        == std::vector<lost>{
            {1, on_address, base + target_ps - at_reset    },
            {2, on_write,   base + 2 * target_ps - at_reset}
    });
  }
}

TEST_CASE("a STOP leaves dropped and drop entries at their own report times")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    r.listen();
    auto const base = r.now() + us;
    r.start(own, false, base, 1);
    r.write(0x11, base, 2);
    r.run_until(base + target_ps + target_ps / 2);
    auto const at_reset = r.now();
    r.reset(popcorn::reset_kind::software);
    auto const rearmed = base + 2 * target_ps - at_reset;
    r.stop(rearmed / 2, 3);
    r.port().drop(on_write, vtime{rearmed + us}, wire, 4);
    r.run_until(rearmed + 2 * us);
    CHECK(
        r.sink->drops
        == std::vector<lost>{
            {2, on_write, rearmed     },
            {4, on_write, rearmed + us}
    });
    CHECK(r.sink->answers.size() == 1);
  }
}

TEST_CASE("an RCC store at the count of an address phase end reports that phase exactly once")
{
  constexpr std::uint32_t stub_at = 0x2002'0000;
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    std::vector<std::vector<std::string>> logs;
    for(bool const store_second : {false, true})
    {
      CAPTURE(store_second);
      target_rig r{part};
      r.listen();
      auto const period = r.period();
      auto const base   = r.now();
      REQUIRE(base % period == 0);
      auto const end = base + 40 * us;
      r.start(own, false, end - target_ps, 1);
      r.stop(end, 2);
      auto const first = store_second ? std::uint32_t{0x6001'BF00} : std::uint32_t{0x6001'E7FF};
      REQUIRE(r.f->target->mem().write(stub_at, 4, first).has_value());
      REQUIRE(r.f->target->mem().write(stub_at + 4, 4, 0xE7FE'6002).has_value());
      r.run_until(end - period);
      REQUIRE(r.sink->log.empty());
      auto const& desc  = r.f->target->peripheral("RCC").description();
      auto const* found = popcorn::find_register(desc, part.reset);
      REQUIRE(found != nullptr);
      auto const rst =
          std::ranges::find(found->fields, std::string_view{"I2C1RST"}, &popcorn::field_desc::name);
      REQUIRE(rst != found->fields.end());
      r.f->set("r0", desc.base + found->offset);
      r.f->set("r1", std::uint64_t{1} << rst->lsb);
      r.f->set("r2", 0);
      r.f->set("pc", stub_at | 1u);
      r.run_until(end + 10 * period);
      CHECK(r.sink->answers.size() + r.sink->drops.size() == 1);
      for(auto const& one : r.sink->answers)
      {
        CHECK(one.at == end);
      }
      for(auto const& one : r.sink->drops)
      {
        CHECK(one.unit == 1);
        CHECK(one.at == end + 3 * period);
      }
      CHECK(popcorn::testing::saved_integer(*r.f, "I2C1", "port.entries.0.kind") == std::nullopt);
      logs.push_back(r.sink->log);
    }
    CHECK(logs[0] == std::vector<std::string>{"answer 1 ack"});
    CHECK(logs[1] == std::vector<std::string>{"dropped 1"});
  }
}

TEST_CASE("a data phase with no acknowledged address answers NACK or 0xFF and logs nothing")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    r.listen();
    auto const base = r.now() + us;
    r.start(0x43, false, base, 1);
    r.write(0x11, base, 2);
    r.read(true, base, 3);
    r.stop(base + 3 * target_ps, 4);
    r.start(own, false, base + 3 * target_ps, 5);
    r.run_until(base + 4 * target_ps + us);
    r.rcc_pulse();
    r.listen();
    auto const later = r.now() + us;
    r.write(0x22, later, 6);
    r.read(false, later, 7);
    r.run_until(later + 2 * target_ps + us);
    CHECK(
        r.sink->answers
        == std::vector<heard>{
            {1, on_address, false, 0xFF, base + target_ps     },
            {2, on_write,   false, 0xFF, base + 2 * target_ps },
            {3, on_read,    true,  0xFF, base + 3 * target_ps },
            {5, on_address, true,  0xFF, base + 4 * target_ps },
            {6, on_write,   false, 0xFF, later + target_ps    },
            {7, on_read,    false, 0xFF, later + 2 * target_ps},
    });
    CHECK(r.warnings("TXDR") == 0);
    CHECK(r.get("ISR", "RXNE") == 0);
  }
}

TEST_CASE("a 10-bit own address logs once as not modelled and is never matched, strict refuses")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    auto const b = r.b();
    r.listen();
    b.set("OAR1", "OA1EN", 0);
    b.set("OAR1", "OA1MODE", 1);
    b.set("OAR1", "OA1EN", 1);
    auto const base = r.now() + us;
    r.start(own, false, base, 1);
    r.stop(base + target_ps, 2);
    r.start(own, false, base + target_ps, 3);
    r.run_until(base + 2 * target_ps + us);
    CHECK(r.sink->log == std::vector<std::string>{"answer 1 nack", "answer 3 nack"});
    CHECK(r.warnings("OAR1") == 1);
    target_rig strict{part, true};
    strict.listen();
    strict.b().set("OAR1", "OA1EN", 0);
    strict.b().set("OAR1", "OA1MODE", 1);
    strict.b().set("OAR1", "OA1EN", 1);
    auto const at = strict.now() + us;
    strict.start(own, false, at, 1);
    CHECK_THROWS_AS(
        std::ignore = strict.f->run(
            {.max_instructions = icount{1'000'000'000}, .until_time = vtime{at + 2 * target_ps}}),
        popcorn::model_error);
  }
}

TEST_CASE("OAR1 and OAR2 keep their address bits while enabled, one warning, strict refuses")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(bool const strict : {false, true})
    {
      CAPTURE(strict);
      target_rig r{part, strict};
      auto const b = r.b();
      r.listen();
      b.set("OAR2", "OA2", 0x30);
      b.set("OAR2", "OA2EN", 1);
      auto&      regs           = r.f->target->peripheral("I2C1");
      auto*      model          = r.f->target->model("I2C1");
      auto const firmware_write = [&](std::string_view name, std::uint32_t value) {
        auto const offset = b.reg("I2C1", name).offset;
        return model->on_write(
            regs, offset, 4, regs.peek(offset), value, {.pc = 0, .now = icount{0}});
      };
      auto const oar1 = (1u << 15) | (0x50u << 1);
      auto const oar2 = (1u << 15) | (3u << 8) | (0x31u << 1);
      if(strict)
      {
        CHECK_THROWS_AS(std::ignore = firmware_write("OAR1", oar1), popcorn::model_error);
        CHECK(r.get("OAR1", "OA1") == own << 1);
        continue;
      }
      REQUIRE(firmware_write("OAR1", oar1).has_value());
      REQUIRE(firmware_write("OAR2", oar2).has_value());
      CHECK(r.get("OAR1", "OA1") == own << 1);
      CHECK(r.get("OAR2", "OA2") == 0x30);
      CHECK(r.get("OAR2", "OA2MSK") == 0);
      CHECK(r.warnings("OAR1") + r.warnings("OAR2") == 1);
      b.write(b.at("I2C1", "OAR1"), oar1);
      CHECK(r.get("OAR1", "OA1") == own << 1);
      REQUIRE(firmware_write("OAR1", 0).has_value());
      REQUIRE(firmware_write("OAR1", oar1).has_value());
      CHECK(r.get("OAR1", "OA1") == 0x50u << 1);
      CHECK(r.warnings("OAR1") + r.warnings("OAR2") == 1);
    }
  }
}

TEST_CASE("a START the firmware requests during the controller's transfer is sent at its STOP")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    auto const b = r.b();
    r.listen();
    write_timing(b);
    auto const base = r.now() + us;
    r.start(own, false, base, 1);
    r.stop(base + target_ps + 20 * us, 2);
    r.run_until(base + target_ps + 5 * us);
    b.command(0x38, 0, false, true);
    CHECK(due_of_kind(*r.f, "I2C1", 0) == std::nullopt);
    r.run_until(base + target_ps + 10 * us);
    CHECK(r.get("ISR", "BUSY") == 1);
    CHECK(due_of_kind(*r.f, "I2C1", 0) == std::nullopt);
    r.run_until(base + target_ps + 20 * us + r.period());
    auto const due = due_of_kind(*r.f, "I2C1", 0);
    REQUIRE(due.has_value());
    CHECK(*due > base + target_ps + 20 * us);
    CHECK(r.get("ISR", "BUSY") == 1);
  }
}

TEST_CASE("RM0456 65.9.2 and RM0481 53.9.2: a target address match clears a pending START")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    auto const b = r.b();
    r.listen(own, true);
    write_timing(b);
    auto const base = r.now() + us;
    r.start(own, false, base, 1);
    r.run_until(base + target_ps / 2);
    b.command(0x38, 0, false, true);
    CHECK(r.get("CR2", "START") == 1);
    r.run_until(base + target_ps + r.period());
    CHECK(r.get("ISR", "ADDR") == 1);
    CHECK(r.get("CR2", "START") == 0);
  }
}

TEST_CASE("the last port unit survives a reset")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    r.port().drop(on_write, vtime{r.now()}, wire, 5);
    r.reset(popcorn::reset_kind::software);
    CHECK_THROWS_AS(r.port().drop(on_write, vtime{0}, wire, 5), std::invalid_argument);
    r.port().drop(on_write, vtime{0}, wire, 6);
    CHECK(popcorn::testing::saved_integer(*r.f, "I2C1", "port.last_unit") == 6);
  }
}

TEST_CASE("1000 port drops leave no entry behind, the port holds at most max_entries entries")
{
  target_rig r;
  auto const base = r.now() + us;
  for(std::uint64_t unit = 1; unit <= 1000; ++unit)
  {
    r.port().drop(on_read, vtime{base}, wire, unit);
  }
  r.run_until(base + us);
  CHECK(r.sink->drops.size() == 1000);
  CHECK(port_due(*r.f).empty());
  CHECK(popcorn::testing::saved_integer(*r.f, "I2C1", "port.entries.0.unit") == std::nullopt);
  auto const later = r.now() + us;
  for(std::uint64_t unit = 1001; unit < 1001 + periph::i2c_controller_port::max_entries; ++unit)
  {
    r.port().drop(on_read, vtime{later}, wire, unit);
  }
  CHECK_THROWS_AS(r.port().drop(on_read, vtime{later}, wire, 9999), popcorn::config_error);
}

TEST_CASE("a snapshot mid address phase with a drop pending restores the same answers")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    r.rcc_pulse();
    r.listen();
    auto const base = r.now() + us;
    r.start(own, false, base, 1);
    r.drop(on_read, base + 200 * us, 2);
    r.stop(base + target_ps, 3);
    r.run_until(base + target_ps / 2);
    REQUIRE(r.sink->log.empty());
    auto const saved = core::encode_snapshot(r.f->target->snapshot().data());
    auto const bytes = r.model_bytes();
    r.run_until(base + 300 * us);
    auto const log = r.sink->log;
    REQUIRE(log == std::vector<std::string>{"answer 1 ack", "dropped 2"});

    target_rig other{part};
    other.f->target->restore(core::snapshot{core::decode_snapshot(saved)});
    CHECK(other.model_bytes() == bytes);
    other.run_until(base + 300 * us);
    CHECK(other.sink->log == log);
    CHECK(other.sink->drops == r.sink->drops);
    CHECK(other.get("ISR", "STOPF") == 1);
  }
}

TEST_CASE("a snapshot holding port entries is refused by an I2C with no controller, unchanged")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    r.listen();
    auto const base = r.now() + us;
    r.start(own, false, base, 1);
    r.run_until(base + target_ps / 2);
    auto const saved = core::encode_snapshot(r.f->target->snapshot().data());
    target_rig none{part, false, false};
    auto const before = none.model_bytes();
    auto const own    = core::encode_snapshot(none.f->target->snapshot().data());
    CHECK_THROWS_AS(
        none.f->target->restore(core::snapshot{core::decode_snapshot(saved)}),
        popcorn::config_error);
    CHECK(none.model_bytes() == before);
    none.f->target->restore(core::snapshot{core::decode_snapshot(own)});
    CHECK(none.model_bytes() == before);
    none.run_until(none.now() + 2 * target_ps);
    CHECK(none.sink->log.empty());
  }
}

TEST_CASE("an RCC reset after a phase end keeps its wire end, a later phase queued either side")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    std::vector<std::vector<lost>>      drops;
    std::vector<std::vector<std::byte>> bytes;
    std::uint64_t                       reset_at = 0;
    std::uint64_t                       end_at   = 0;
    for(bool const after : {false, true})
    {
      CAPTURE(after);
      target_rig r{part};
      r.listen();
      r.run_until(r.now() + 100 * us);
      auto const now = r.now();
      auto const end = now - 3 * r.period();
      reset_at       = now;
      end_at         = end;
      r.start(own, false, end - target_ps, 1);
      if(not after)
      {
        r.write(0x11, end - target_ps, 2);
      }
      r.rcc_pulse();
      if(after)
      {
        r.write(0x11, end - target_ps, 2);
      }
      bytes.push_back(r.model_bytes());
      r.run_until(end + 3 * target_ps);
      drops.push_back(r.sink->drops);
    }
    CHECK(drops[0] == drops[1]);
    CHECK(bytes[0] == bytes[1]);
    CHECK(
        drops[0]
        == std::vector<lost>{
            {1, on_address, reset_at          },
            {2, on_write,   end_at + target_ps}
    });
  }
}

TEST_CASE("the general call acknowledges the address byte 0b00000000 only, never 0b00000001")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    auto const b = r.b();
    r.listen(own, true);
    b.set("CR1", "GCEN", 1);
    auto const base = r.now() + us;
    r.start(0x00, true, base, 1);
    r.stop(base + target_ps, 2);
    r.start(0x00, false, base + 2 * target_ps, 3);
    r.run_until(base + 3 * target_ps + us);
    CHECK(r.sink->log == std::vector<std::string>{"answer 1 nack"});
    CHECK(r.get("ISR", "ADDR") == 1);
    CHECK(r.get("ISR", "DIR") == 0);
    b.set("ICR", "ADDRCF", 1);
    r.stop(r.now(), 4);
    r.run_until(r.now() + us);
    CHECK(r.sink->log == std::vector<std::string>{"answer 1 nack", "answer 3 ack"});
    r.start(0x00, true, r.now() + us, 5);
    r.run_until(r.now() + target_ps + 2 * us);
    CHECK(r.sink->log.back() == "answer 5 nack");
    CHECK(r.get("ISR", "ADDR") == 0);
  }
}

TEST_CASE("an address phase whose START came while PE was clear answers NACK")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    auto const b = r.b();
    r.listen();
    b.set("CR1", "ADDRACLR", 0);
    b.set("CR1", "PE", 0);
    auto const base = r.now() + us;
    r.start(own, false, base, 1);
    r.run_until(base + target_ps / 2);
    b.set("CR1", "PE", 1);
    r.run_until(base + target_ps + us);
    CHECK(
        r.sink->answers
        == std::vector<heard>{
            {1, on_address, false, 0xFF, base + target_ps}
    });
    CHECK(r.get("ISR", "ADDR") == 0);
    CHECK(r.get("ISR", "BUSY") == 0);
  }
}

TEST_CASE("a phase a machine reset drops holds the wire to its end in the new base")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    r.after_system_reset();
    r.listen();
    auto const base = r.now() + us;
    r.start(own, false, base, 1);
    r.run_until(base + target_ps / 2);
    auto const at_reset = r.now();
    r.reset(popcorn::reset_kind::software);
    r.listen();
    r.write(0x11, 0, 2);
    auto const rebased = base + target_ps - at_reset;
    r.run_until(rebased + target_ps + us);
    CHECK(
        r.sink->drops
        == std::vector<lost>{
            {1, on_address, rebased}
    });
    CHECK(
        r.sink->answers
        == std::vector<heard>{
            {2, on_write, false, 0xFF, rebased + target_ps}
    });
  }
}

TEST_CASE("each RCC reset leaves one stale I2C port event, gone once the phase's due passes")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    r.listen();
    auto const base = r.now() + 1'000 * us;
    r.start(own, false, base, 1);
    REQUIRE(port_due(*r.f).size() == 1);
    for(int pulse = 0; pulse < 1000; ++pulse)
    {
      r.rcc_pulse();
    }
    CHECK(port_due(*r.f).size() == 1 + 1000);
    r.listen();
    r.run_until(base + target_ps + us);
    CHECK(port_due(*r.f).empty());
    CHECK(r.sink->answers.size() == 1);
  }
}

TEST_CASE("a written byte lands in RXDR and sets RXNE at its phase end, the event IRQ with no read")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    auto const b = r.b();
    r.listen();
    b.set("CR1", "RXIE", 1);
    auto const base = r.now() + us;
    auto const end  = base + 2 * target_ps;
    r.start(own, false, base, 1);
    r.write(0x5A, base, 2);
    r.run_until(base + target_ps + r.period());
    auto const queued = port_due(*r.f);
    REQUIRE(queued.size() == 1);
    CHECK(queued[0].due_ps == end);
    r.run_until(end - r.period());
    CHECK_FALSE(pending(*r.f, part.event_irq));
    CHECK(r.get("ISR", "RXNE") == 0);
    r.run_until(end + r.period());
    CHECK(pending(*r.f, part.event_irq));
    CHECK(r.get("ISR", "RXNE") == 1);
    CHECK(
        r.sink->answers
        == std::vector<heard>{
            {1, on_address, true, 0xFF, base + target_ps},
            {2, on_write,   true, 0xFF, end             }
    });
    CHECK(b.receive() == 0x5A);
    CHECK(r.get("ISR", "RXNE") == 0);
    r.stop(r.now(), 3);
    r.run_until(r.now() + us);
    CHECK(r.get("ISR", "STOPF") == 1);
    CHECK(r.get("ISR", "BUSY") == 0);
  }
}

TEST_CASE("CR2.NACK refuses the next written byte, which still lands, and clears once sent")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    auto const b = r.b();
    r.listen();
    auto const base = r.now() + us;
    r.start(own, false, base, 1);
    r.write(0x11, base, 2);
    r.write(0x22, base, 3);
    r.write(0x33, base, 4);
    r.run_until(base + 2 * target_ps + us);
    CHECK(b.receive() == 0x11);
    b.set("CR2", "NACK", 1);
    REQUIRE(r.get("CR2", "NACK") == 1);
    r.run_until(base + 3 * target_ps + us);
    CHECK(r.sink->log.back() == "answer 3 nack");
    CHECK(r.get("CR2", "NACK") == 0);
    CHECK(b.receive() == 0x22);
    r.run_until(base + 4 * target_ps + us);
    CHECK(r.sink->log.back() == "answer 4 ack");
    CHECK(b.receive() == 0x33);
    b.set("CR2", "NACK", 1);
    r.stop(r.now(), 5);
    r.run_until(r.now() + us);
    CHECK(r.get("CR2", "NACK") == 0);
    b.set("CR2", "NACK", 1);
    auto const again = r.now() + us;
    r.start(own, false, again, 6);
    r.run_until(again + target_ps + us);
    CHECK(r.sink->log.back() == "answer 6 ack");
    CHECK(r.get("CR2", "NACK") == 0);
  }
}

TEST_CASE("a byte received while RXDR is full holds SCL until the RXDR read, then answers there")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    std::vector<std::vector<heard>>     answers;
    std::vector<std::vector<std::byte>> bytes;
    for(int const variant : {0, 1, 2})
    {
      CAPTURE(variant);
      target_rig r{part};
      auto const b = r.b();
      r.listen();
      auto const base = r.now() + us;
      r.start(own, false, base, 1);
      r.write(0x11, base, 2);
      r.write(0x22, base, 3);
      if(variant != 1)
      {
        r.write(0x33, base, 4);
      }
      r.run_until(base + 3 * target_ps + us);
      if(variant == 1)
      {
        r.write(0x33, base, 4);
      }
      CHECK(r.sink->answers.size() == 2);
      CHECK(r.get("ISR", "RXNE") == 1);
      CHECK(port_due(*r.f).size() == (variant == 1 ? 1u : 0u));
      auto const read_at = base + 3 * target_ps + 40 * us;
      r.run_until(read_at);
      CHECK(port_due(*r.f).empty());
      if(variant == 2)
      {
        b.set("CR2", "NACK", 1);
      }
      CHECK(b.receive() == 0x11);
      CHECK(r.get("ISR", "RXNE") == 1);
      auto const after = port_due(*r.f);
      REQUIRE(after.size() == 2);
      CHECK(after[0].due_ps == read_at);
      CHECK(after[1].due_ps == read_at);
      r.run_until(read_at + us);
      CHECK(b.receive() == 0x22);
      r.run_until(read_at + target_ps + us);
      CHECK(
          r.sink->answers
          == std::vector<heard>{
              {1, on_address, true,         0xFF, base + target_ps    },
              {2, on_write,   true,         0xFF, base + 2 * target_ps},
              {3, on_write,   variant != 2, 0xFF, read_at             },
              {4, on_write,   true,         0xFF, read_at + target_ps }
      });
      CHECK(r.get("CR2", "NACK") == 0);
      if(variant != 2)
      {
        answers.push_back(r.sink->answers);
        bytes.push_back(r.model_bytes());
      }
    }
    CHECK(answers[0] == answers[1]);
    CHECK(bytes[0] == bytes[1]);
  }
}

TEST_CASE("a STOP queued during a byte stretch runs at the release, after the byte's answer")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    auto const b = r.b();
    r.listen();
    auto const base = r.now() + us;
    r.start(own, false, base, 1);
    r.write(0x11, base, 2);
    r.write(0x22, base, 3);
    r.stop(base + 3 * target_ps + 5 * us, 4);
    r.run_until(base + 3 * target_ps + 20 * us);
    CHECK(r.get("ISR", "STOPF") == 0);
    CHECK(r.get("ISR", "BUSY") == 1);
    auto const read_at = r.now();
    CHECK(b.receive() == 0x11);
    r.run_until(read_at + r.period());
    CHECK(r.sink->answers.back() == heard{3, on_write, true, 0xFF, read_at});
    CHECK(r.sink->drops.empty());
    CHECK(r.get("ISR", "STOPF") == 1);
    CHECK(r.get("ISR", "BERR") == 0);
    CHECK(r.get("ISR", "BUSY") == 0);
    CHECK(b.receive() == 0x22);
  }
}

auto last_port_due(stm32_fixture const& f) -> std::uint64_t
{
  auto const events = port_due(f);
  REQUIRE(not events.empty());
  return std::ranges::max(events, {}, &core::event::seq).due_ps;
}

TEST_CASE("a phase posted in the past behind an ending, held or released phase starts at its end")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    {
      target_rig r{part};
      r.listen();
      auto const base = r.now() + us;
      r.start(own, false, base, 1);
      r.run_until(base + target_ps / 2);
      r.write(0x11, base, 2);
      CHECK(last_port_due(*r.f) == base + target_ps);
      r.run_until(base + 2 * target_ps + us);
      CHECK(
          r.sink->answers
          == std::vector<heard>{
              {1, on_address, true, 0xFF, base + target_ps    },
              {2, on_write,   true, 0xFF, base + 2 * target_ps}
      });
    }
    for(bool const released : {false, true})
    {
      CAPTURE(released);
      target_rig r{part};
      auto const b = r.b();
      r.listen();
      auto const base = r.now() + us;
      r.start(own, false, base, 1);
      r.write(0x11, base, 2);
      r.write(0x22, base, 3);
      r.run_until(base + 3 * target_ps + 20 * us);
      REQUIRE(r.sink->answers.size() == 2);
      auto const read_at = r.now();
      if(released)
      {
        CHECK(b.receive() == 0x11);
      }
      r.write(0x33, base, 4);
      CHECK(last_port_due(*r.f) == (released ? read_at : base + 3 * target_ps));
    }
  }
}

TEST_CASE("a phase posted after an RCC reset that ended before it is due at the reset")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    r.listen();
    auto const t = r.now() + us;
    r.run_until(t + 3 * target_ps);
    auto const reset_ps = r.now();
    r.rcc_pulse();
    r.start(own, false, t, 1);
    CHECK(last_port_due(*r.f) == reset_ps);
  }
}

TEST_CASE("PE clear releases an ADDR stretch with ACK and a byte stretch with NACK")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    {
      target_rig r{part};
      auto const b = r.b();
      r.listen(own, true);
      auto const base = r.now() + us;
      r.start(own, false, base, 1);
      r.write(0x11, base, 2);
      r.run_until(base + target_ps + 10 * us);
      REQUIRE(r.sink->answers.empty());
      auto const clear_at = r.now();
      b.set("CR1", "PE", 0);
      r.run_until(clear_at + target_ps + us);
      CHECK(
          r.sink->answers
          == std::vector<heard>{
              {1, on_address, true,  0xFF, clear_at            },
              {2, on_write,   false, 0xFF, clear_at + target_ps}
      });
      CHECK(r.get("ISR", "RXNE") == 0);
    }
    {
      target_rig r{part};
      auto const b = r.b();
      r.listen();
      auto const base = r.now() + us;
      r.start(own, false, base, 1);
      r.write(0x11, base, 2);
      r.write(0x22, base, 3);
      r.run_until(base + 3 * target_ps + 10 * us);
      auto const clear_at = r.now();
      b.set("CR1", "PE", 0);
      r.run_until(clear_at + us);
      CHECK(r.sink->answers.back() == heard{3, on_write, false, 0xFF, clear_at});
      CHECK(r.get("ISR", "RXNE") == 0);
    }
  }
}

TEST_CASE("a target reset between the address and the second byte answers NACK, no RXNE")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(bool const machine : {false, true})
    {
      CAPTURE(machine);
      target_rig r{part};
      auto const b = r.b();
      r.listen();
      auto const base = r.now() + us;
      r.start(own, false, base, 1);
      r.write(0x11, base, 2);
      r.run_until(base + 2 * target_ps + us);
      REQUIRE(r.sink->log == std::vector<std::string>{"answer 1 ack", "answer 2 ack"});
      CHECK(b.receive() == 0x11);
      if(machine)
      {
        r.reset(popcorn::reset_kind::software);
      }
      else
      {
        r.rcc_pulse();
      }
      r.listen();
      auto const at = r.now() + us;
      r.write(0x22, at, 3);
      r.run_until(at + target_ps + us);
      CHECK(r.sink->answers.back() == heard{3, on_write, false, 0xFF, at + target_ps});
      CHECK(r.get("ISR", "RXNE") == 0);
      CHECK(r.sink->drops.empty());
    }
  }
}

TEST_CASE(
    "an RCC reset during a stretch drops the held phase at the reset, later phases queued "
    "before or after it")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(bool const address : {false, true})
    {
      CAPTURE(address);
      std::vector<std::vector<lost>>      drops;
      std::vector<std::vector<std::byte>> bytes;
      for(bool const after : {false, true})
      {
        CAPTURE(after);
        target_rig r{part};
        r.listen(own, address);
        auto const    base    = r.now() + us;
        auto const    blocked = address ? 2u : 4u;
        std::uint64_t unit    = 0;
        r.start(own, false, base, ++unit);
        if(not address)
        {
          r.write(0x11, base, ++unit);
          r.write(0x22, base, ++unit);
        }
        if(not after)
        {
          r.write(0x33, base, blocked);
        }
        auto const held_end = base + (address ? 1 : 3) * target_ps;
        r.run_until(held_end + 10 * us);
        REQUIRE(r.sink->answers.size() == (address ? 0u : 2u));
        auto const reset_at = r.now();
        r.rcc_pulse();
        if(after)
        {
          r.write(0x33, base, blocked);
        }
        r.run_until(reset_at + target_ps + us);
        CHECK(
            r.sink->drops
            == std::vector<lost>{
                {blocked - 1, address ? on_address : on_write, reset_at            },
                {blocked,     on_write,                        reset_at + target_ps}
        });
        CHECK(r.get("ISR", "RXNE") == 0);
        drops.push_back(r.sink->drops);
        bytes.push_back(r.model_bytes());
        r.listen();
        auto const again = r.now() + us;
        r.start(own, false, again, blocked + 1);
        r.run_until(again + target_ps + us);
        CHECK(
            r.sink->answers.back()
            == heard{blocked + 1, on_address, true, 0xFF, again + target_ps});
      }
      CHECK(drops[0] == drops[1]);
      CHECK(bytes[0] == bytes[1]);
    }
  }
}

TEST_CASE("a machine reset during an ADDR stretch drops the address at the reset, rebased")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    r.after_system_reset();
    r.listen(own, true);
    auto const base = r.now() + us;
    r.start(own, false, base, 1);
    r.write(0x11, base, 2);
    r.run_until(base + target_ps + 10 * us);
    REQUIRE(r.sink->answers.empty());
    r.reset(popcorn::reset_kind::software);
    r.run_until(target_ps + us);
    CHECK(
        r.sink->drops
        == std::vector<lost>{
            {1, on_address, 0        },
            {2, on_write,   target_ps}
    });
    CHECK(r.sink->answers.empty());
  }
}

TEST_CASE("a snapshot inside a byte stretch restores the held byte and its release")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    r.listen();
    auto const base = r.now() + us;
    r.start(own, false, base, 1);
    r.write(0x11, base, 2);
    r.write(0x22, base, 3);
    r.run_until(base + 3 * target_ps + 10 * us);
    auto const saved   = core::encode_snapshot(r.f->target->snapshot().data());
    auto const bytes   = r.model_bytes();
    auto const read_at = base + 3 * target_ps + 30 * us;
    auto const finish  = [&](target_rig const& one) {
      one.run_until(read_at);
      auto const first = one.b().receive();
      one.run_until(read_at + us);
      auto const second = one.b().receive();
      return std::pair{first, second};
    };
    CHECK(finish(r) == std::pair{std::uint32_t{0x11}, std::uint32_t{0x22}});
    target_rig other{part};
    other.f->target->restore(core::snapshot{core::decode_snapshot(saved)});
    CHECK(other.model_bytes() == bytes);
    CHECK(finish(other) == std::pair{std::uint32_t{0x11}, std::uint32_t{0x22}});
    CHECK(
        other.sink->answers
        == std::vector<heard>{
            {3, on_write, true, 0xFF, read_at}
    });
    REQUIRE(r.sink->answers.size() == 3);
    CHECK(r.sink->answers[2] == other.sink->answers.at(0));
  }
}

TEST_CASE("the target firmware receives written bytes and NACKs the byte after its marker")
{
  constexpr std::uint32_t data_log = 0xDA7A'0000;
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    r.f->set("pc", sym(part.firmware, "target"));
    r.run_until(r.now() + 200 * us);
    REQUIRE(r.get("CR1", "RXIE") == 1);
    auto const base = r.now() + us;
    r.start(own, false, base, 1);
    std::uint64_t unit = 1;
    for(std::uint8_t const byte : {0x11, 0xEE, 0x22, 0x33})
    {
      r.write(byte, base, ++unit);
    }
    r.stop(base + 5 * target_ps + 50 * us, ++unit);
    r.run_until(base + 5 * target_ps + 100 * us);
    CHECK(
        r.sink->log
        == std::vector<std::string>{
            "answer 1 ack", "answer 2 ack", "answer 3 ack", "answer 4 nack", "answer 5 ack"});
    CHECK(r.sink->drops.empty());
    CHECK(
        r.f->log()
        == std::vector<std::uint32_t>{
            addr_log | 0x84, data_log | 0x11, data_log | 0xEE, data_log | 0x22, data_log | 0x33,
            stop_log});
  }
}

TEST_CASE("a written byte after an acknowledged read address answers NACK and lands nowhere")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    r.listen();
    auto const base = r.now() + us;
    r.start(own, true, base, 1);
    r.write(0x11, base, 2);
    r.run_until(base + 2 * target_ps + us);
    CHECK(r.sink->log == std::vector<std::string>{"answer 1 ack", "answer 2 nack"});
    CHECK(r.get("ISR", "RXNE") == 0);
  }
}

TEST_CASE("a STOP on another lane due at the ADDRCF store that releases an ADDR stretch runs")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    auto const b = r.b();
    r.listen(own, true);
    auto const base = r.now() + us;
    r.start(own, false, base, 1);
    auto const release_at = base + target_ps + 4'000 * r.period();
    r.port().stop(vtime{release_at}, wire + 1, 2);
    r.run_until(release_at);
    REQUIRE(r.now() == release_at);
    REQUIRE(r.get("ISR", "ADDR") == 1);
    REQUIRE(port_due(*r.f).empty());
    b.set("ICR", "ADDRCF", 1);
    r.run_until(release_at + r.period());
    CHECK(
        r.sink->answers
        == std::vector<heard>{
            {1, on_address, true, 0xFF, release_at}
    });
    CHECK(r.get("ISR", "STOPF") == 1);
    CHECK(r.get("ISR", "BUSY") == 0);
    CHECK(port_due(*r.f).empty());
  }
}

namespace
{
constexpr std::uint32_t txe_bit  = 1u << 0;
constexpr std::uint32_t txis_bit = 1u << 1;

void enable_with(target_rig const& r, std::string_view field)
{
  auto const b = r.b();
  b.set("CR1", "PE", 0);
  b.set("CR1", field, 1);
  b.set("CR1", "PE", 1);
}

void write_isr(target_rig const& r, std::uint32_t value)
{
  auto const b = r.b();
  b.write(b.at("I2C1", "ISR"), value);
}
}

TEST_CASE("a read sends TXDR bytes, TXIS asks at the address and after each copy, NACK stops it")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    auto const b = r.b();
    r.listen();
    b.set("CR1", "TXIE", 1);
    b.set("CR1", "NACKIE", 1);
    b.send(0xC1);
    REQUIRE(r.get("ISR", "TXE") == 0);
    auto const base = r.now() + us;
    r.start(own, true, base, 1);
    r.read(true, base, 2);
    r.read(true, base, 3);
    r.read(false, base, 4);
    r.stop(base + 4 * target_ps, 5);
    auto const asks = [&](std::uint64_t end, std::uint8_t next) {
      r.run_until(end - r.period());
      CHECK_FALSE(pending(*r.f, part.event_irq));
      CHECK(r.get("ISR", "TXIS") == 0);
      r.run_until(end + r.period());
      CHECK(pending(*r.f, part.event_irq));
      CHECK(r.get("ISR", "TXIS") == 1);
      CHECK(r.get("ISR", "TXE") == 1);
      b.send(next);
      CHECK(r.get("ISR", "TXIS") == 0);
      unpend(*r.f, part.event_irq);
    };
    asks(base + target_ps, 0xC2);
    asks(base + 2 * target_ps, 0xC3);
    asks(base + 3 * target_ps, 0xC4);
    r.run_until(base + 4 * target_ps - r.period());
    CHECK_FALSE(pending(*r.f, part.event_irq));
    CHECK(r.get("ISR", "NACKF") == 0);
    r.run_until(base + 4 * target_ps + r.period());
    CHECK(pending(*r.f, part.event_irq));
    CHECK(r.get("ISR", "NACKF") == 1);
    CHECK(r.get("ISR", "TXIS") == 0);
    CHECK(r.get("ISR", "TXE") == 0);
    CHECK(r.get("ISR", "STOPF") == 1);
    CHECK(r.get("ISR", "BUSY") == 0);
    CHECK(
        r.sink->answers
        == std::vector<heard>{
            {1, on_address, true,  0xFF, base + target_ps    },
            {2, on_read,    true,  0xC1, base + 2 * target_ps},
            {3, on_read,    true,  0xC2, base + 3 * target_ps},
            {4, on_read,    false, 0xC3, base + 4 * target_ps},
    });
    CHECK(r.warnings("TXDR") == 0);
    CHECK(r.sink->drops.empty());
  }
}

TEST_CASE("an empty TXDR holds a read byte to the TXDR write, a later read queued before or during")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    std::vector<std::vector<heard>>     answers;
    std::vector<std::vector<std::byte>> bytes;
    for(bool const during : {false, true})
    {
      CAPTURE(during);
      target_rig r{part};
      auto const b = r.b();
      r.listen();
      auto const base = r.now() + us;
      r.start(own, true, base, 1);
      r.read(true, base, 2);
      if(not during)
      {
        r.read(false, base, 3);
      }
      r.run_until(base + target_ps + r.period());
      CHECK(r.get("ISR", "TXIS") == 1);
      CHECK(r.get("ISR", "TXE") == 1);
      if(during)
      {
        r.read(false, base, 3);
      }
      auto const write_at = base + target_ps + 40 * us;
      r.run_until(write_at);
      CHECK(r.sink->answers.size() == 1);
      CHECK(port_due(*r.f).empty());
      b.send(0xD1);
      CHECK(r.get("ISR", "TXIS") == 1);
      auto const after = port_due(*r.f);
      REQUIRE(after.size() == 2);
      CHECK(after[0].due_ps == write_at + target_ps);
      CHECK(after[1].due_ps == write_at + target_ps);
      b.send(0xD2);
      r.run_until(write_at + 2 * target_ps + us);
      CHECK(
          r.sink->answers
          == std::vector<heard>{
              {1, on_address, true,  0xFF, base + target_ps        },
              {2, on_read,    true,  0xD1, write_at + target_ps    },
              {3, on_read,    false, 0xD2, write_at + 2 * target_ps},
      });
      CHECK(r.get("ISR", "NACKF") == 1);
      answers.push_back(r.sink->answers);
      bytes.push_back(r.model_bytes());
    }
    CHECK(answers[0] == answers[1]);
    CHECK(bytes[0] == bytes[1]);
  }
}

TEST_CASE("a STOP queued during a read stretch runs after the held byte's answer, not cutting it")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    auto const b = r.b();
    r.listen();
    auto const base = r.now() + us;
    r.start(own, true, base, 1);
    r.read(false, base, 2);
    r.stop(base + 2 * target_ps + 5 * us, 3);
    r.run_until(base + target_ps + r.period());
    REQUIRE(r.get("ISR", "TXIS") == 1);
    REQUIRE(r.get("ISR", "TXE") == 1);
    auto const write_at = base + 2 * target_ps + 40 * us;
    r.run_until(write_at);
    CHECK(r.sink->answers.size() == 1);
    CHECK(r.get("ISR", "STOPF") == 0);
    b.send(0xD1);
    r.run_until(write_at + target_ps + us);
    CHECK(
        r.sink->answers
        == std::vector<heard>{
            {1, on_address, true,  0xFF, base + target_ps    },
            {2, on_read,    false, 0xD1, write_at + target_ps},
    });
    CHECK(r.sink->drops.empty());
    CHECK(r.get("ISR", "NACKF") == 1);
    CHECK(r.get("ISR", "BERR") == 0);
    CHECK(r.get("ISR", "STOPF") == 1);
  }
}

TEST_CASE("with NOSTRETCH, ADDR holds no SCL, TXIS follows a copy and an empty TXDR sends 0xFF")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(bool const preload : {false, true})
    {
      CAPTURE(preload);
      target_rig r{part};
      auto const b = r.b();
      r.listen(own, true);
      enable_with(r, "NOSTRETCH");
      if(preload)
      {
        b.send(0xE1);
      }
      auto const base = r.now() + us;
      r.start(own, true, base, 1);
      r.read(true, base, 2);
      r.read(false, base, 3);
      r.run_until(base + target_ps + r.period());
      CHECK(
          r.sink->answers
          == std::vector<heard>{
              {1, on_address, true, 0xFF, base + target_ps}
      });
      CHECK(r.get("ISR", "ADDR") == 1);
      CHECK(r.get("ISR", "TXIS") == (preload ? 1u : 0u));
      r.run_until(base + 2 * target_ps - r.period());
      CHECK(r.get("ISR", "OVR") == (preload ? 0u : 1u));
      r.run_until(base + 3 * target_ps + us);
      REQUIRE(r.sink->answers.size() == 3);
      CHECK(
          r.sink->answers[1]
          == heard{
              2, on_read, true, preload ? std::uint8_t{0xE1} : std::uint8_t{0xFF},
              base + 2 * target_ps});
      CHECK(r.sink->answers[2] == heard{3, on_read, false, 0xFF, base + 3 * target_ps});
      CHECK(r.get("ISR", "OVR") == 1);
    }
  }
}

TEST_CASE("with NOSTRETCH, STOPF at the first byte or a full RXDR sets OVR, firmware sets TXIS")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(bool const cleared : {false, true})
    {
      CAPTURE(cleared);
      target_rig r{part};
      auto const b = r.b();
      r.listen();
      enable_with(r, "NOSTRETCH");
      auto const base = r.now() + us;
      r.start(own, false, base, 1);
      r.stop(base + target_ps, 2);
      r.run_until(base + target_ps + us);
      REQUIRE(r.get("ISR", "STOPF") == 1);
      if(cleared)
      {
        b.set("ICR", "STOPCF", 1);
      }
      b.send(0xE2);
      auto const at = r.now() + us;
      r.start(own, true, at, 3);
      r.read(false, at, 4);
      r.run_until(at + 2 * target_ps + us);
      CHECK(r.sink->answers.back() == heard{4, on_read, false, 0xE2, at + 2 * target_ps});
      CHECK(r.get("ISR", "OVR") == (cleared ? 0u : 1u));
    }
    {
      target_rig r{part};
      auto const b = r.b();
      r.listen();
      enable_with(r, "NOSTRETCH");
      auto const base = r.now() + us;
      r.start(own, false, base, 1);
      r.write(0x11, base, 2);
      r.write(0x22, base, 3);
      r.run_until(base + 3 * target_ps + us);
      CHECK(
          r.sink->answers
          == std::vector<heard>{
              {1, on_address, true,  0xFF, base + target_ps    },
              {2, on_write,   true,  0xFF, base + 2 * target_ps},
              {3, on_write,   false, 0xFF, base + 3 * target_ps},
      });
      CHECK(r.get("ISR", "OVR") == 1);
      CHECK(b.receive() == 0x11);
      CHECK(r.get("ISR", "RXNE") == 0);
    }
    for(bool const stretch : {false, true})
    {
      CAPTURE(stretch);
      target_rig r{part};
      r.listen();
      if(not stretch)
      {
        enable_with(r, "NOSTRETCH");
      }
      write_isr(r, txis_bit);
      CHECK(r.get("ISR", "TXIS") == (stretch ? 0u : 1u));
      r.b().send(0x55);
      CHECK(r.get("ISR", "TXIS") == 0);
    }
    {
      target_rig r{part};
      r.listen();
      enable_with(r, "NOSTRETCH");
      r.b().set("CR1", "PE", 0);
      write_isr(r, txis_bit);
      CHECK(r.get("ISR", "TXIS") == 0);
      r.b().set("CR1", "PE", 1);
      CHECK(r.get("ISR", "TXIS") == 0);
    }
  }
}

TEST_CASE("SBC with RELOAD stretches each received byte at TCR, the NBYTES write sends its ACK")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    auto const b = r.b();
    r.listen(own, true);
    enable_with(r, "SBC");
    b.set("CR1", "TCIE", 1);
    auto const base = r.now() + us;
    r.start(own, false, base, 1);
    r.write(0x11, base, 2);
    r.write(0x22, base, 3);
    r.run_until(base + target_ps + r.period());
    REQUIRE(r.get("ISR", "ADDR") == 1);
    b.set("CR2", "RELOAD", 1);
    b.set("CR2", "NBYTES", 1);
    auto const released = r.now();
    b.set("ICR", "ADDRCF", 1);
    r.run_until(released + target_ps - r.period());
    CHECK_FALSE(pending(*r.f, part.event_irq));
    r.run_until(released + target_ps + r.period());
    CHECK(pending(*r.f, part.event_irq));
    CHECK(r.get("ISR", "TCR") == 1);
    CHECK(r.get("ISR", "RXNE") == 1);
    r.run_until(released + target_ps + 10 * us);
    CHECK(r.sink->answers.size() == 1);
    CHECK(b.receive() == 0x11);
    CHECK(r.sink->answers.size() == 1);
    b.set("CR2", "NBYTES", 0);
    CHECK(r.get("ISR", "TCR") == 1);
    r.run_until(r.now() + 10 * us);
    CHECK(r.sink->answers.size() == 1);
    b.set("CR2", "NACK", 1);
    auto const first = r.now();
    b.set("CR2", "NBYTES", 1);
    CHECK(r.get("ISR", "TCR") == 0);
    r.run_until(first + target_ps + 10 * us);
    CHECK(r.get("ISR", "TCR") == 1);
    CHECK(b.receive() == 0x22);
    auto const second = r.now();
    b.set("CR2", "NBYTES", 1);
    r.run_until(second + us);
    CHECK(
        r.sink->answers
        == std::vector<heard>{
            {1, on_address, true,  0xFF, released},
            {2, on_write,   false, 0xFF, first   },
            {3, on_write,   true,  0xFF, second  },
    });
    CHECK(r.get("CR2", "NACK") == 0);
    CHECK(r.get("ISR", "TCR") == 0);
  }
}

TEST_CASE("SBC with RELOAD asks NBYTES TXIS, stretches at TCR and resumes at the NBYTES write")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    auto const b = r.b();
    r.listen(own, true);
    enable_with(r, "SBC");
    auto const base = r.now() + us;
    r.start(own, true, base, 1);
    r.read(true, base, 2);
    r.read(true, base, 3);
    r.read(false, base, 4);
    r.run_until(base + target_ps + r.period());
    REQUIRE(r.get("ISR", "ADDR") == 1);
    b.set("CR2", "RELOAD", 1);
    b.set("CR2", "NBYTES", 2);
    auto const released = r.now();
    b.set("ICR", "ADDRCF", 1);
    CHECK(r.get("ISR", "TXIS") == 1);
    b.send(0xA1);
    CHECK(r.get("ISR", "TXIS") == 1);
    b.send(0xA2);
    CHECK(r.get("ISR", "TXIS") == 0);
    r.run_until(released + 2 * target_ps + r.period());
    CHECK(r.get("ISR", "TCR") == 1);
    CHECK(r.get("ISR", "TXIS") == 0);
    r.run_until(released + 2 * target_ps + 20 * us);
    CHECK(r.sink->answers.size() == 3);
    auto const reload = r.now();
    b.set("CR2", "NBYTES", 1);
    CHECK(r.get("ISR", "TCR") == 0);
    CHECK(r.get("ISR", "TXIS") == 1);
    b.send(0xA3);
    CHECK(r.get("ISR", "TXIS") == 0);
    r.run_until(reload + target_ps + us);
    CHECK(
        r.sink->answers
        == std::vector<heard>{
            {1, on_address, true,  0xFF, released                },
            {2, on_read,    true,  0xA1, released + target_ps    },
            {3, on_read,    true,  0xA2, released + 2 * target_ps},
            {4, on_read,    false, 0xA3, reload + target_ps      },
    });
    CHECK(r.get("ISR", "NACKF") == 1);
  }
}

TEST_CASE("SBC with RELOAD clear stops the count at 0 and acknowledges bytes as CR2.NACK says")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    auto const b = r.b();
    r.listen(own, true);
    enable_with(r, "SBC");
    auto const base = r.now() + us;
    r.start(own, false, base, 1);
    r.write(0x11, base, 2);
    r.write(0x22, base, 3);
    r.run_until(base + target_ps + r.period());
    REQUIRE(r.get("ISR", "ADDR") == 1);
    b.set("CR2", "NBYTES", 1);
    auto const released = r.now();
    b.set("ICR", "ADDRCF", 1);
    r.run_until(released + target_ps + us);
    CHECK(r.get("ISR", "TCR") == 0);
    CHECK(b.receive() == 0x11);
    r.run_until(released + 2 * target_ps + us);
    CHECK(r.get("ISR", "TCR") == 0);
    CHECK(b.receive() == 0x22);
    CHECK(
        r.sink->answers
        == std::vector<heard>{
            {1, on_address, true, 0xFF, released                },
            {2, on_write,   true, 0xFF, released + target_ps    },
            {3, on_write,   true, 0xFF, released + 2 * target_ps},
    });
  }
}

TEST_CASE("a STOP after a transmitter's TCR byte leaves TCR to the NBYTES write, which clears it")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    auto const b = r.b();
    r.listen(own, true);
    enable_with(r, "SBC");
    b.set("CR1", "TCIE", 1);
    auto const base = r.now() + us;
    r.start(own, true, base, 1);
    r.read(true, base, 2);
    r.run_until(base + target_ps + r.period());
    REQUIRE(r.get("ISR", "ADDR") == 1);
    b.set("CR2", "RELOAD", 1);
    b.set("CR2", "NBYTES", 1);
    auto const released = r.now();
    b.set("ICR", "ADDRCF", 1);
    b.send(0xA1);
    r.stop(released + target_ps + 5 * us, 3);
    r.run_until(released + target_ps + 10 * us);
    CHECK(r.sink->answers.back() == heard{2, on_read, true, 0xA1, released + target_ps});
    CHECK(r.sink->drops.empty());
    REQUIRE(r.get("ISR", "STOPF") == 1);
    CHECK(r.get("ISR", "TCR") == 1);
    b.set("ICR", "STOPCF", 1);
    unpend(*r.f, part.event_irq);
    r.run_until(r.now() + r.period());
    CHECK(pending(*r.f, part.event_irq));
    b.set("CR2", "NBYTES", 1);
    CHECK(r.get("ISR", "TCR") == 0);
    CHECK(r.get("ISR", "TXIS") == 0);
    unpend(*r.f, part.event_irq);
    r.run_until(r.now() + 10 * r.period());
    CHECK_FALSE(pending(*r.f, part.event_irq));
  }
}

TEST_CASE("a target reset between the read address and the second read byte sends 0xFF")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(bool const machine : {false, true})
    {
      CAPTURE(machine);
      target_rig r{part};
      auto const b = r.b();
      r.listen();
      b.send(0xC1);
      auto const base = r.now() + us;
      r.start(own, true, base, 1);
      r.read(true, base, 2);
      r.run_until(base + 2 * target_ps + us);
      REQUIRE(r.sink->answers.size() == 2);
      CHECK(r.sink->answers[1].byte == 0xC1);
      if(machine)
      {
        r.reset(popcorn::reset_kind::software);
      }
      else
      {
        r.rcc_pulse();
      }
      r.listen();
      b.send(0x99);
      auto const at = r.now() + us;
      r.read(true, at, 3);
      r.run_until(at + target_ps + us);
      CHECK(r.sink->answers.back() == heard{3, on_read, true, 0xFF, at + target_ps});
      CHECK(r.get("ISR", "TXE") == 0);
      CHECK(r.sink->drops.empty());
    }
  }
}

TEST_CASE("a snapshot inside a read stretch restores the held byte and its release")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    r.listen();
    auto const base = r.now() + us;
    r.start(own, true, base, 1);
    r.read(true, base, 2);
    r.read(false, base, 3);
    r.run_until(base + target_ps + 10 * us);
    REQUIRE(r.sink->answers.size() == 1);
    auto const saved    = core::encode_snapshot(r.f->target->snapshot().data());
    auto const bytes    = r.model_bytes();
    auto const write_at = base + target_ps + 30 * us;
    auto const finish   = [&](target_rig const& one) {
      one.run_until(write_at);
      one.b().send(0xD1);
      one.b().send(0xD2);
      one.run_until(write_at + 2 * target_ps + us);
    };
    finish(r);
    target_rig other{part};
    other.f->target->restore(core::snapshot{core::decode_snapshot(saved)});
    CHECK(other.model_bytes() == bytes);
    finish(other);
    auto const expected = std::vector<heard>{
        {2, on_read, true,  0xD1, write_at + target_ps    },
        {3, on_read, false, 0xD2, write_at + 2 * target_ps},
    };
    CHECK(other.sink->answers == expected);
    REQUIRE(r.sink->answers.size() == 3);
    CHECK(std::vector<heard>{r.sink->answers.begin() + 1, r.sink->answers.end()} == expected);
  }
}

TEST_CASE("an RCC reset drops a running read at its end and a held read one phase after the reset")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    {
      target_rig r{part};
      r.listen();
      r.b().send(0xC1);
      auto const base = r.now() + us;
      r.start(own, true, base, 1);
      r.read(true, base, 2);
      r.read(true, base, 3);
      r.run_until(base + target_ps + target_ps / 2);
      REQUIRE(port_due(*r.f).size() == 2);
      r.rcc_pulse();
      r.run_until(base + 3 * target_ps + us);
      CHECK(
          r.sink->drops
          == std::vector<lost>{
              {2, on_read, base + 2 * target_ps}
      });
      CHECK(r.sink->answers.back() == heard{3, on_read, true, 0xFF, base + 3 * target_ps});
    }
    std::vector<std::vector<heard>>     answers;
    std::vector<std::vector<std::byte>> bytes;
    for(bool const after : {false, true})
    {
      CAPTURE(after);
      target_rig r{part};
      r.listen();
      auto const base = r.now() + us;
      r.start(own, true, base, 1);
      r.read(true, base, 2);
      if(not after)
      {
        r.read(true, base, 3);
      }
      r.run_until(base + target_ps + 10 * us);
      REQUIRE(r.sink->answers.size() == 1);
      auto const reset_at = r.now();
      r.rcc_pulse();
      if(after)
      {
        r.read(true, base, 3);
      }
      r.run_until(reset_at + 2 * target_ps + us);
      CHECK(
          r.sink->drops
          == std::vector<lost>{
              {2, on_read, reset_at + target_ps}
      });
      CHECK(r.sink->answers.back() == heard{3, on_read, true, 0xFF, reset_at + 2 * target_ps});
      answers.push_back(r.sink->answers);
      bytes.push_back(r.model_bytes());
      r.listen();
      auto const again = r.now() + us;
      r.start(own, true, again, 4);
      r.run_until(again + target_ps + us);
      CHECK(r.sink->answers.back() == heard{4, on_address, true, 0xFF, again + target_ps});
    }
    CHECK(answers[0] == answers[1]);
    CHECK(bytes[0] == bytes[1]);
  }
}

TEST_CASE("PE clear releases a held read byte, which reads 0xFF")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    auto const b = r.b();
    r.listen();
    auto const base = r.now() + us;
    r.start(own, true, base, 1);
    r.read(true, base, 2);
    r.read(false, base, 3);
    r.run_until(base + target_ps + 10 * us);
    REQUIRE(r.get("ISR", "TXIS") == 1);
    auto const clear_at = r.now();
    b.set("CR1", "PE", 0);
    CHECK(r.get("ISR", "TXIS") == 0);
    r.run_until(clear_at + 2 * target_ps + us);
    CHECK(
        r.sink->answers
        == std::vector<heard>{
            {1, on_address, true,  0xFF, base + target_ps        },
            {2, on_read,    true,  0xFF, clear_at + target_ps    },
            {3, on_read,    false, 0xFF, clear_at + 2 * target_ps},
    });
    CHECK(r.get("ISR", "NACKF") == 0);
  }
}

TEST_CASE("a machine reset during a read stretch drops the held byte one phase after it, rebased")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    r.after_system_reset();
    r.listen();
    auto const base = r.now() + us;
    r.start(own, true, base, 1);
    r.read(true, base, 2);
    r.run_until(base + target_ps + 10 * us);
    REQUIRE(r.sink->answers.size() == 1);
    r.reset(popcorn::reset_kind::software);
    r.run_until(target_ps + us);
    CHECK(
        r.sink->drops
        == std::vector<lost>{
            {2, on_read, target_ps}
    });
  }
}

TEST_CASE("TXIS asked before a STOP stays set until TXDR is written")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    r.listen();
    auto const base = r.now() + us;
    r.start(own, true, base, 1);
    r.stop(base + target_ps, 2);
    r.run_until(base + target_ps + 10 * us);
    CHECK(r.get("ISR", "STOPF") == 1);
    CHECK(r.get("ISR", "TXIS") == 1);
    r.b().send(0x55);
    CHECK(r.get("ISR", "TXIS") == 0);
    CHECK(r.get("ISR", "TXE") == 0);
  }
}

TEST_CASE("the target firmware sends bytes on TXIS, logs the NACK of the last and the STOP")
{
  constexpr std::uint32_t nacked_log = 0x4AC0;
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    r.f->set("pc", sym(part.firmware, "target"));
    r.run_until(r.now() + 200 * us);
    REQUIRE(r.get("CR1", "TXIE") == 1);
    auto const base = r.now() + us;
    r.start(own, true, base, 1);
    r.read(true, base, 2);
    r.read(true, base, 3);
    r.read(false, base, 4);
    r.stop(base + 4 * target_ps + 50 * us, 5);
    r.run_until(base + 4 * target_ps + 100 * us);
    std::vector<std::uint8_t> sent;
    for(auto const& one : r.sink->answers | std::views::drop(1))
    {
      sent.push_back(one.byte);
    }
    CHECK(sent == std::vector<std::uint8_t>{0xB0, 0xB1, 0xB2});
    CHECK(r.sink->log.front() == "answer 1 ack");
    CHECK(r.sink->drops.empty());
    CHECK(r.f->log() == std::vector<std::uint32_t>{addr_log | 0x85, nacked_log, stop_log});
  }
}

namespace
{
constexpr std::uint32_t timeout_kind = 4;
constexpr std::uint32_t timoutens    = 1u << 15;
constexpr std::uint32_t tidle        = 1u << 12;
constexpr std::uint32_t textens      = 1u << 31;

auto timeout_b(std::uint32_t value) -> std::uint32_t
{
  return value << 16;
}

auto kernel_ps(stm32_fixture const& f, std::uint64_t cycles) -> std::uint64_t
{
  auto&      clock = const_cast<core::clock&>(f.target->virtual_clock());
  auto const hz    = clock.hz(clock.domain("i2c1"));
  REQUIRE(hz != 0);
  return cycles * 1'000'000'000'000 / hz;
}

auto timeout_events(stm32_fixture const& f) -> std::vector<std::uint64_t>
{
  auto const& units = f.target->description().peripherals;
  auto const  found =
      std::ranges::find(units, std::string_view{"I2C1"}, &popcorn::peripheral_desc::name);
  REQUIRE(found != units.end());
  auto const                 owner = static_cast<std::uint32_t>(found - units.begin());
  std::vector<std::uint64_t> out;
  for(auto const& event : f.target->snapshot().data().events.pending)
  {
    if(event.id.owner == owner and event.id.kind == timeout_kind)
    {
      out.push_back(event.due_ps);
    }
  }
  return out;
}

struct timer_rig
{
  target_part                    part;
  std::shared_ptr<faulty_target> faulty = std::make_shared<faulty_target>();
  stm32::board                   board{};
  std::unique_ptr<stm32_fixture> f;
  std::vector<core::log_record>  logged;
  core::hook_handle              logs;

  explicit timer_rig(target_part const& which, bool strict = false, double cpi = 1.0) : part{which}
  {
    board.i2c["I2C1"]          = {faulty};
    auto options               = popcorn::testing::stm32_options();
    options.cpi                = cpi;
    options.strict_peripherals = strict;
    options.models             = stm32::factories(board, part.desc());
    f    = std::make_unique<stm32_fixture>(part.desc(), part.firmware, std::move(options));
    logs = f->target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
    f->set("pc", sym(part.firmware, "idle"));
    auto const on = b();
    on.set("RCC", part.enable, "I2C1EN", 1);
    write_timing(on);
    on.set("CR1", "ERRIE", 1);
    on.set("CR1", "PE", 1);
  }

  [[nodiscard]] auto b() const -> bus
  {
    return bus{*f};
  }

  [[nodiscard]] auto cycles(std::uint64_t count) const -> std::uint64_t
  {
    return kernel_ps(*f, count);
  }

  [[nodiscard]] auto period() const -> std::uint64_t
  {
    return cycles(10);
  }

  [[nodiscard]] auto now() const -> std::uint64_t
  {
    return b().now();
  }

  void run_until(std::uint64_t ps) const
  {
    b().run_until(ps);
  }

  void timeout(std::uint32_t value) const
  {
    b().write(b().at("I2C1", "TIMEOUTR"), value);
  }

  [[nodiscard]] auto flag() const -> std::uint32_t
  {
    return b().get("ISR", "TIMEOUT");
  }

  [[nodiscard]] auto error_pending() const -> bool
  {
    return pending(*f, part.event_irq + 1);
  }

  [[nodiscard]] auto warnings(std::string_view reg) const -> std::size_t
  {
    return static_cast<std::size_t>(std::ranges::count_if(logged, [&](auto const& record) {
      return record.kind == core::log_kind::model_warning
         and record.peripheral == "I2C1"
         and record.reg == reg;
    }));
  }

  [[nodiscard]] auto model_bytes() const -> std::vector<std::byte>
  {
    std::vector<std::byte> out;
    f->target->model("I2C1")->save(out);
    return out;
  }
};

auto target_cycles(target_rig const& r, std::uint64_t count) -> std::uint64_t
{
  return kernel_ps(*r.f, count);
}

void target_timeout(target_rig const& r, std::uint32_t value)
{
  auto const b = r.b();
  b.write(b.at("I2C1", "TIMEOUTR"), value);
  b.set("CR1", "ERRIE", 1);
}
}

TEST_CASE("TIMEOUT sets (TIMEOUTA + 1) x 2048 kernel periods after SCL falls into a TXDR wait")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(std::uint32_t const units : {1u, 2u})
    {
      CAPTURE(units);
      timer_rig  r{part};
      auto const b = r.b();
      r.timeout(timoutens | (units - 1));
      b.command(0x33, 2, false, true);
      auto const fall  = r.now() + 10 * r.period();
      auto const limit = r.cycles(2048 * units);
      r.run_until(fall + limit - r.period());
      CHECK(timeout_events(*r.f) == std::vector<std::uint64_t>{fall + limit});
      CHECK(r.flag() == 0);
      CHECK_FALSE(r.error_pending());
      CHECK(b.get("ISR", "BUSY") == 1);
      r.run_until(fall + limit + r.period() / 2);
      CHECK(r.flag() == 1);
      CHECK(r.error_pending());
      CHECK(timeout_events(*r.f).empty());
      r.run_until(fall + limit + 3 * r.period());
      CHECK(b.get("ISR", "STOPF") == 1);
      CHECK(b.get("ISR", "BUSY") == 0);
      REQUIRE_FALSE(r.faulty->events.empty());
      CHECK(r.faulty->events.back().kind == 'P');
      CHECK(of_kind(r.faulty->events, 'w').empty());
      b.set("ICR", "TIMOUTCF", 1);
      CHECK(r.flag() == 0);
    }
  }
}

TEST_CASE("TIMOUTEN set during a wait counts SCL low from the set, TIMOUTEN clear stops it")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    timer_rig  r{part};
    auto const b = r.b();
    b.command(0x33, 2, false, true);
    auto const fall  = r.now() + 10 * r.period();
    auto const limit = r.cycles(2048);
    r.run_until(fall + limit / 2);
    auto const set = r.now();
    r.timeout(timoutens);
    CHECK(timeout_events(*r.f) == std::vector<std::uint64_t>{set + limit});
    r.run_until(set + limit - r.period());
    r.timeout(0);
    CHECK(timeout_events(*r.f).empty());
    r.run_until(set + 2 * limit);
    CHECK(r.flag() == 0);
    CHECK(b.get("ISR", "BUSY") == 1);
    auto const again = r.now();
    r.timeout(timoutens);
    r.run_until(again + limit + r.period() / 2);
    CHECK(r.flag() == 1);
  }
}

TEST_CASE("a target's stretch counts as SCL low: below the limit no flag, above it a STOP after it")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(bool const over : {false, true})
    {
      CAPTURE(over);
      timer_rig  r{part};
      auto const b           = r.b();
      auto const limit       = r.cycles(2048);
      auto const extend      = over ? limit + 40 * r.period() : limit - 40 * r.period();
      r.faulty->stretch_call = 0;
      r.faulty->stretch_ps   = extend;
      r.timeout(timoutens);
      b.command(0x33, 1, false, true);
      b.send(0x5A);
      auto const fall = r.now() + 10 * r.period();
      r.run_until(fall + extend + 20 * r.period());
      if(not over)
      {
        CHECK(r.flag() == 0);
        CHECK(of_kind(r.faulty->events, 'w').size() == 1);
        CHECK(b.get("ISR", "STOPF") == 1);
        CHECK(timeout_events(*r.f).empty());
        continue;
      }
      CHECK(r.flag() == 1);
      CHECK(of_kind(r.faulty->events, 'w').empty());
      CHECK(b.get("ISR", "STOPF") == 1);
      REQUIRE(r.faulty->events.size() == 2);
      auto const offset = r.f->target->virtual_clock().since_power_on().ps - r.now();
      CHECK(r.faulty->events[1].at == offset + fall + extend + r.period());
    }
  }
}

TEST_CASE("the existing 10 us stretch with the RM0456 25 ms and 8 ms settings trips nothing")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    timer_rig  r{part};
    auto const b       = r.b();
    auto const unit    = r.cycles(2048);
    auto const a_units = (25'000 * us + unit - 1) / unit;
    auto const b_units = (8'000 * us) / unit;
    REQUIRE(b_units >= 1);
    r.timeout(
        timoutens
        | static_cast<std::uint32_t>(a_units - 1)
        | textens
        | timeout_b(static_cast<std::uint32_t>(b_units - 1)));
    r.faulty->stretch_call = 1;
    r.faulty->stretch_ps   = stretch_ps;
    b.command(0x33, 3, false, true);
    for(int index = 0; index < 3; ++index)
    {
      r.run_until(r.now() + 10 * r.period());
      b.send(static_cast<std::uint8_t>(0x10 + index));
    }
    r.run_until(r.now() + 40 * r.period() + stretch_ps);
    CHECK(b.get("ISR", "STOPF") == 1);
    CHECK(of_kind(r.faulty->events, 'w').size() == 3);
    CHECK(r.flag() == 0);
    CHECK(timeout_events(*r.f).empty());
  }
}

TEST_CASE("TEXTEN sums the controller's own waits of one transfer, not the target's stretch")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    SUBCASE("two waits of one transfer add up")
    {
      timer_rig  r{part};
      auto const b     = r.b();
      auto const limit = r.cycles(2048);
      r.timeout(textens);
      b.command(0x33, 2, false, true);
      auto const fall = r.now() + 10 * r.period();
      auto const one  = limit * 3 / 4;
      r.run_until(fall + one);
      auto const sent = r.now();
      b.send(0x11);
      auto const second = sent + 9 * r.period();
      r.run_until(second + r.period() / 2);
      CHECK(timeout_events(*r.f) == std::vector<std::uint64_t>{second + limit - (sent - fall)});
      r.run_until(second + limit - (sent - fall) - r.period());
      CHECK(r.flag() == 0);
      r.run_until(second + limit - (sent - fall) + r.period() / 2);
      CHECK(r.flag() == 1);
      r.run_until(r.now() + 3 * r.period());
      CHECK(b.get("ISR", "BUSY") == 0);
    }
    SUBCASE("TIMOUTEN alone does not trip on either wait")
    {
      timer_rig  r{part};
      auto const b     = r.b();
      auto const limit = r.cycles(2048);
      r.timeout(timoutens);
      b.command(0x33, 2, false, true);
      auto const fall = r.now() + 10 * r.period();
      r.run_until(fall + limit * 3 / 4);
      b.send(0x11);
      r.run_until(r.now() + 9 * r.period() + limit * 3 / 4);
      b.send(0x22);
      r.run_until(r.now() + 20 * r.period());
      CHECK(r.flag() == 0);
      CHECK(b.get("ISR", "STOPF") == 1);
    }
    SUBCASE("a STOP restarts the sum")
    {
      timer_rig  r{part};
      auto const b     = r.b();
      auto const limit = r.cycles(2048);
      r.timeout(textens);
      for(int round = 0; round < 2; ++round)
      {
        b.command(0x33, 1, false, true);
        r.run_until(r.now() + 10 * r.period() + limit * 3 / 4);
        b.send(0x11);
        r.run_until(r.now() + 20 * r.period());
        CHECK(b.get("ISR", "STOPF") == 1);
        b.set("ICR", "STOPCF", 1);
      }
      CHECK(r.flag() == 0);
    }
    SUBCASE("a target's stretch above the limit is not the I2C's own")
    {
      timer_rig  r{part};
      auto const b     = r.b();
      auto const limit = r.cycles(2048);
      r.timeout(textens);
      r.faulty->stretch_call = 0;
      r.faulty->stretch_ps   = 2 * limit;
      b.command(0x33, 1, false, true);
      b.send(0x5A);
      r.run_until(r.now() + 2 * limit + 30 * r.period());
      CHECK(r.flag() == 0);
      CHECK(of_kind(r.faulty->events, 'w').size() == 1);
    }
    SUBCASE("a TEXTEN clear and set restarts the sum")
    {
      timer_rig  r{part};
      auto const b     = r.b();
      auto const limit = r.cycles(2048);
      r.timeout(textens);
      b.command(0x33, 2, false, true);
      r.run_until(r.now() + 10 * r.period() + limit * 3 / 4);
      b.send(0x11);
      r.run_until(r.now() + 9 * r.period() + r.period() / 2);
      r.timeout(0);
      auto const set = r.now();
      r.timeout(textens);
      CHECK(timeout_events(*r.f) == std::vector<std::uint64_t>{set + limit});
      r.run_until(set + limit / 2);
      CHECK(r.flag() == 0);
      r.run_until(set + limit + r.period() / 2);
      CHECK(r.flag() == 1);
    }
    SUBCASE("a wait after a target's address stretch starts at the stretch end")
    {
      timer_rig  r{part};
      auto const b     = r.b();
      auto const limit = r.cycles(2048);
      r.timeout(textens);
      r.faulty->stretch_call = 0;
      r.faulty->stretch_ps   = 2 * limit;
      b.command(0x33, 2, false, true);
      auto const released = r.now() + 10 * r.period() + 2 * limit;
      r.run_until(released + limit / 4);
      CHECK(r.flag() == 0);
      auto const sent = r.now();
      b.send(0x11);
      auto const second = sent + 9 * r.period();
      r.run_until(second + r.period() / 2);
      auto const due = second + limit - (sent - released);
      CHECK(timeout_events(*r.f) == std::vector<std::uint64_t>{due});
      r.run_until(due - r.period());
      CHECK(r.flag() == 0);
      r.run_until(due + r.period() / 2);
      CHECK(r.flag() == 1);
    }
    SUBCASE("a TC wait after a target's last-byte stretch starts at the stretch end")
    {
      timer_rig  r{part};
      auto const b     = r.b();
      auto const limit = r.cycles(2048);
      r.timeout(textens);
      r.faulty->stretch_call = 1;
      r.faulty->stretch_ps   = 2 * limit;
      b.command(0x33, 1, false, false);
      b.send(0x5A);
      auto const released = r.now() + 10 * r.period() + 9 * r.period() + 2 * limit;
      r.run_until(released - limit / 2);
      CHECK(b.get("ISR", "TC") == 1);
      CHECK(timeout_events(*r.f) == std::vector<std::uint64_t>{released + limit});
      r.run_until(released + limit - r.period());
      CHECK(r.flag() == 0);
      r.run_until(released + limit + r.period() / 2);
      CHECK(r.flag() == 1);
    }
  }
}

TEST_CASE("an unmodelled controller field at a START logs once, strict refuses at the START")
{
  struct unmodelled_field
  {
    std::string_view reg;
    std::string_view field;
  };
  constexpr std::uint32_t start_bit = 1u << 13;
  constexpr std::uint32_t add10_bit = 1u << 11;
  constexpr std::uint32_t autoend   = 1u << 25;
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(auto const [reg, field] : std::array<unmodelled_field, 8>{
            {{"CR1", "PECEN"},
             {"CR1", "ALERTEN"},
             {"CR1", "SMBHEN"},
             {"CR1", "SMBDEN"},
             {"CR1", "SBC"},
             {"CR1", "NOSTRETCH"},
             {"CR2", "ADD10"},
             {"I2C_AUTOCR", "TRIGEN"}}
    })
    {
      CAPTURE(field);
      for(bool const strict : {false, true})
      {
        CAPTURE(strict);
        if(reg == "I2C_AUTOCR" and not part.autocr)
        {
          continue;
        }
        timer_rig  r{part, strict};
        auto const b = r.b();
        REQUIRE(
            popcorn::find_register(r.f->target->peripheral("I2C1").description(), reg) != nullptr);
        auto const extra = reg == "CR2" ? add10_bit : 0u;
        if(reg == "CR1")
        {
          b.set("CR1", "PE", 0);
        }
        if(reg != "CR2")
        {
          b.set(reg, field, 1);
        }
        if(reg == "CR1")
        {
          b.set("CR1", "PE", 1);
        }
        auto const word = (0x33u << 1) | start_bit | autoend | extra;
        auto const cr2  = b.at("I2C1", "CR2");
        if(strict)
        {
          CHECK_THROWS_AS(
              std::ignore = r.f->target->mem().write(cr2, 4, word), popcorn::model_error);
          continue;
        }
        b.write(cr2, word);
        r.run_until(r.now() + 20 * r.period());
        b.write(cr2, word);
        r.run_until(r.now() + 20 * r.period());
        CHECK(b.get("ISR", "STOPF") == 1);
        CHECK(r.warnings("CR1") == 1);
      }
    }
  }
}

TEST_CASE("an unmodelled controller field refuses a START sent from a hold end, a STOP or a read")
{
  constexpr std::uint32_t start_bit = 1u << 13;
  constexpr std::uint32_t stop_bit  = 1u << 14;
  constexpr std::uint32_t read_bit  = 1u << 10;
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    timer_rig  r{part, true};
    auto const b   = r.b();
    auto const p   = r.period();
    auto const cr2 = b.at("I2C1", "CR2");
    b.set("CR1", "PECEN", 1);
    auto const write_cr2 = [&](std::uint32_t word) {
      std::ignore = r.f->target->mem().write(cr2, 4, word);
    };
    auto const run_to = [&](std::uint64_t ps) {
      std::ignore =
          r.f->run_to_stop({.max_instructions = icount{1'000'000'000}, .until_time = vtime{ps}});
    };
    SUBCASE("hold end")
    {
      auto* const held = dynamic_cast<stm32::i2c_bus*>(r.f->target->model("I2C1"));
      REQUIRE(held != nullptr);
      auto const until = r.now() + 20 * p;
      held->hold(vtime{r.now() + p}, vtime{until});
      r.run_until(r.now() + 2 * p);
      REQUIRE(b.get("ISR", "BUSY") == 1);
      CHECK_NOTHROW(write_cr2((0x33u << 1) | start_bit | (1u << 25)));
      CHECK_THROWS_AS(run_to(until + 2 * p), popcorn::model_error);
      CHECK(b.get("ISR", "BUSY") == 1);
    }
    SUBCASE("STOP then START")
    {
      CHECK_THROWS_AS(write_cr2((0x33u << 1) | start_bit | (1u << 16)), popcorn::model_error);
      b.send(0x5A);
      r.run_until(r.now() + 25 * p);
      REQUIRE(b.get("ISR", "TC") == 1);
      CHECK_NOTHROW(write_cr2((0x33u << 1) | start_bit | stop_bit | (1u << 16)));
      CHECK_THROWS_AS(run_to(r.now() + 5 * p), popcorn::model_error);
    }
    SUBCASE("RXDR read")
    {
      CHECK_THROWS_AS(
          write_cr2((0x33u << 1) | read_bit | start_bit | (2u << 16)), popcorn::model_error);
      r.run_until(r.now() + 35 * p);
      REQUIRE(b.get("ISR", "RXNE") == 1);
      CHECK_NOTHROW(write_cr2((0x33u << 1) | read_bit | start_bit | (2u << 16)));
      r.run_until(r.now() + 5 * p);
      CHECK_THROWS_AS(std::ignore = b.receive(), popcorn::model_error);
    }
  }
}

TEST_CASE("TIDLE times the idle bus, once per idle period, from the STOP or the TIMOUTEN set")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    timer_rig  r{part};
    auto const b    = r.b();
    auto const idle = r.cycles(4 * 10);
    auto const set  = r.now();
    r.timeout(timoutens | tidle | 9);
    CHECK(timeout_events(*r.f) == std::vector<std::uint64_t>{set + idle});
    r.run_until(set + idle - r.period() / 4);
    CHECK(r.flag() == 0);
    r.run_until(set + idle + r.period());
    CHECK(r.flag() == 1);
    CHECK(r.error_pending());
    b.set("ICR", "TIMOUTCF", 1);
    r.run_until(r.now() + 4 * idle);
    CHECK(r.flag() == 0);
    CHECK(timeout_events(*r.f).empty());
    b.command(0x33, 0, false, true);
    auto const begun = r.now();
    CHECK(timeout_events(*r.f).empty());
    auto const stop_end = begun + 11 * r.period();
    r.run_until(stop_end + r.period() / 4);
    REQUIRE(b.get("ISR", "BUSY") == 0);
    CHECK(r.flag() == 0);
    REQUIRE(timeout_events(*r.f).size() == 1);
    auto const due = timeout_events(*r.f).front();
    CHECK(due == stop_end + idle);
    r.run_until(due + r.period());
    CHECK(r.flag() == 1);
    b.set("ICR", "TIMOUTCF", 1);
    r.timeout(0);
    auto const reset_at = r.now();
    r.timeout(timoutens | tidle | 9);
    CHECK(timeout_events(*r.f) == std::vector<std::uint64_t>{reset_at + idle});
    r.run_until(reset_at + idle + r.period());
    CHECK(r.flag() == 1);
  }
}

TEST_CASE("the TIDLE period starts at the end of the STOP off the instruction grid too")
{
  timer_rig  r{target_parts[0]};
  auto const b = r.b();
  b.set("RCC", "RCC_CR", "HSION", 1);
  r.run_until(r.now() + 200 * us);
  REQUIRE(r.f->field("RCC", "RCC_CR", "HSIRDY") == 1);
  b.set("RCC", "RCC_CCIPR1", "I2C1SEL", 2);
  auto const idle = r.cycles(4 * 10);
  r.timeout(timoutens | tidle | 9);
  r.run_until(r.now() + idle + r.period());
  REQUIRE(r.flag() == 1);
  b.set("ICR", "TIMOUTCF", 1);
  b.command(0x33, 0, false, true);
  auto const stop_end    = r.now() + 11 * r.period();
  auto const instruction = 1'000'000'000'000 / r.f->target->virtual_clock().instruction_hz();
  REQUIRE(stop_end % instruction != 0);
  r.run_until(stop_end + r.period());
  REQUIRE(b.get("ISR", "BUSY") == 0);
  CHECK(timeout_events(*r.f) == std::vector<std::uint64_t>{stop_end + idle});
}

TEST_CASE("the TIDLE period starts at a host hold's end and a target STOP's time, off the grid")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    SUBCASE("host hold")
    {
      timer_rig  r{part};
      auto const b    = r.b();
      auto const idle = r.cycles(4 * 10);
      r.timeout(timoutens | tidle | 9);
      r.run_until(r.now() + idle + r.period());
      REQUIRE(r.flag() == 1);
      b.set("ICR", "TIMOUTCF", 1);
      auto* const held = dynamic_cast<stm32::i2c_bus*>(r.f->target->model("I2C1"));
      REQUIRE(held != nullptr);
      auto const from  = r.now() + 2 * idle;
      auto const until = from + 4 * idle + 123;
      held->hold(vtime{from}, vtime{until});
      r.run_until(until + r.period());
      REQUIRE(b.get("ISR", "BUSY") == 0);
      CHECK(timeout_events(*r.f) == std::vector<std::uint64_t>{until + idle});
    }
    SUBCASE("target STOP")
    {
      target_rig r{part};
      r.listen();
      auto const idle = target_cycles(r, 4 * 10);
      target_timeout(r, timoutens | tidle | 9);
      auto const base = r.now() + 2 * idle;
      r.start(own, false, base, 1);
      auto const stopped = base + 2 * target_ps + 123;
      r.stop(stopped, 2);
      r.run_until(stopped + r.period());
      REQUIRE(r.b().get("ISR", "BUSY") == 0);
      CHECK(r.b().get("ISR", "STOPF") == 1);
      CHECK(timeout_events(*r.f) == std::vector<std::uint64_t>{stopped + idle});
    }
  }
}

TEST_CASE("TIMEOUTR keeps TIMEOUTA and TIDLE while TIMOUTEN is set, TIMEOUTB while TEXTEN is set")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(bool const strict : {false, true})
    {
      CAPTURE(strict);
      target_rig r{part, strict};
      auto const b = r.b();
      r.listen();
      auto&      regs           = r.f->target->peripheral("I2C1");
      auto*      model          = r.f->target->model("I2C1");
      auto const offset         = b.reg("I2C1", "TIMEOUTR").offset;
      auto const firmware_write = [&](std::uint32_t value) {
        return model->on_write(
            regs, offset, 4, regs.peek(offset), value, {.pc = 0, .now = icount{0}});
      };
      REQUIRE(firmware_write(timoutens | 5 | textens | timeout_b(7)).has_value());
      CHECK(regs.peek(offset) == (timoutens | 5 | textens | timeout_b(7)));
      if(strict)
      {
        CHECK_THROWS_AS(
            std::ignore = firmware_write(timoutens | tidle | 6 | textens | timeout_b(7)),
            popcorn::model_error);
        CHECK(regs.peek(offset) == (timoutens | 5 | textens | timeout_b(7)));
        continue;
      }
      REQUIRE(firmware_write(timoutens | tidle | 6 | textens | timeout_b(8)).has_value());
      CHECK(regs.peek(offset) == (timoutens | 5 | textens | timeout_b(7)));
      CHECK(r.warnings("TIMEOUTR") == 1);
      REQUIRE(firmware_write(textens | timeout_b(8)).has_value());
      CHECK(regs.peek(offset) == (5 | textens | timeout_b(7)));
      REQUIRE(firmware_write(tidle | 6).has_value());
      CHECK(regs.peek(offset) == (tidle | 6 | timeout_b(7)));
      REQUIRE(firmware_write(textens | timeout_b(9) | timoutens | 3).has_value());
      CHECK(regs.peek(offset) == (textens | timeout_b(9) | timoutens | 3));
      CHECK(r.warnings("TIMEOUTR") == 1);
    }
  }
}

TEST_CASE("a target stretch past TIMEOUTA releases SCL as PE clear does and leaves the transfer")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(auto const held : {on_address, on_write, on_read})
    {
      CAPTURE(static_cast<int>(held));
      target_rig r{part};
      r.listen(own, held == on_address);
      target_timeout(r, timoutens);
      auto const limit = target_cycles(r, 2048);
      auto const base  = r.now() + us;
      auto       fall  = base + target_ps;
      r.start(own, held == on_read, base, 1);
      if(held == on_write)
      {
        r.write(0x11, base, 2);
        r.write(0x22, base, 3);
        r.write(0x33, base, 4);
        fall = base + 3 * target_ps;
      }
      else if(held == on_read)
      {
        r.read(true, base, 2);
        r.read(true, base, 3);
        r.read(true, base, 4);
      }
      else
      {
        r.write(0x44, base, 2);
        r.write(0x55, base, 3);
        r.write(0x66, base, 4);
      }
      r.stop(fall + limit + 4 * target_ps + 5 * us, 5);
      r.run_until(fall + limit - us);
      CHECK(timeout_events(*r.f) == std::vector<std::uint64_t>{fall + limit});
      CHECK(r.get("ISR", "TIMEOUT") == 0);
      auto const answered = r.sink->answers.size();
      r.run_until(fall + limit + 4 * target_ps + 10 * us);
      CHECK(r.get("ISR", "TIMEOUT") == 1);
      CHECK(pending(*r.f, part.event_irq + 1));
      REQUIRE(r.sink->answers.size() == 4);
      auto const& released    = r.sink->answers[answered];
      auto const  answered_at = fall + limit + (held == on_read ? target_ps : 0);
      CHECK(released.at >= answered_at);
      CHECK(released.at < answered_at + r.period());
      if(held == on_address)
      {
        CHECK(released == heard{1, on_address, true, 0xFF, released.at});
      }
      else if(held == on_write)
      {
        CHECK(released.unit == 3);
        CHECK_FALSE(released.ack);
      }
      else
      {
        CHECK(released.unit == 2);
        CHECK(released.byte == 0xFF);
      }
      for(auto const& later : r.sink->answers | std::views::drop(answered + 1))
      {
        CAPTURE(later.unit);
        CHECK_FALSE((later.ack and later.phase == on_write));
        if(later.phase == on_read)
        {
          CHECK(later.byte == 0xFF);
        }
      }
      CHECK(r.sink->drops.empty());
      CHECK(timeout_events(*r.f).empty());
      CHECK(r.get("ISR", "STOPF") == 1);
    }
  }
}

TEST_CASE(
    "a target counts SCL low between the controller's phases, a phase due by then releases it")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(auto const late : {false, true})
    {
      CAPTURE(late);
      target_rig r{part};
      r.listen();
      target_timeout(r, timoutens);
      auto const limit = target_cycles(r, 2048);
      auto const base  = r.now() + us;
      auto const fall  = base + target_ps;
      auto const next  = fall + limit + (late ? r.period() : 0);
      r.start(own, false, base, 1);
      r.write(0x11, next, 2);
      r.stop(next + target_ps + us, 3);
      r.run_until(fall + limit - us);
      CHECK(
          timeout_events(*r.f)
          == (late ? std::vector<std::uint64_t>{fall + limit} : std::vector<std::uint64_t>{}));
      r.run_until(next + target_ps + 2 * us);
      CHECK(r.get("ISR", "TIMEOUT") == (late ? 1u : 0u));
      REQUIRE(r.sink->answers.size() == 2);
      CHECK(r.sink->answers[0] == heard{1, on_address, true, 0xFF, fall});
      CHECK(r.sink->answers[1].ack == not late);
      CHECK(r.sink->answers[1].at == next + target_ps);
      CHECK(r.sink->drops.empty());
    }
  }
}

TEST_CASE("TEXTEN sums a target's stretches of one transfer, each released before the limit")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    r.listen();
    target_timeout(r, textens);
    auto const limit = target_cycles(r, 2048);
    auto const base  = r.now() + us;
    r.start(own, false, base, 1);
    r.write(0x11, base, 2);
    r.write(0x22, base, 3);
    r.write(0x33, base, 4);
    auto const fall = base + 3 * target_ps;
    r.run_until(fall + limit * 3 / 4);
    auto const read_at = r.now();
    CHECK(r.b().receive() == 0x11);
    CHECK(r.get("ISR", "TIMEOUT") == 0);
    auto const second = read_at + target_ps;
    r.run_until(second + us);
    auto const due = second + limit - (read_at - fall);
    CHECK(timeout_events(*r.f) == std::vector<std::uint64_t>{due});
    r.run_until(due + target_ps + us);
    CHECK(r.get("ISR", "TIMEOUT") == 1);
    REQUIRE(r.sink->answers.size() == 4);
    CHECK(r.sink->answers[3].unit == 4);
    CHECK_FALSE(r.sink->answers[3].ack);
  }
}

TEST_CASE("a snapshot with the timer running restores the same TIMEOUT time, bytes re-dumped equal")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    timer_rig  r{part};
    auto const b = r.b();
    r.timeout(timoutens | textens | timeout_b(3));
    b.command(0x33, 2, false, true);
    auto const fall  = r.now() + 10 * r.period();
    auto const limit = r.cycles(2048);
    r.run_until(fall + limit / 2);
    auto const saved = core::encode_snapshot(r.f->target->snapshot().data());
    auto const bytes = r.model_bytes();
    auto const due   = timeout_events(*r.f);
    REQUIRE(due == std::vector<std::uint64_t>{fall + limit});
    timer_rig other{part};
    other.f->target->restore(core::snapshot{core::decode_snapshot(saved)});
    CHECK(other.model_bytes() == bytes);
    CHECK(timeout_events(*other.f) == due);
    for(auto const* one : {&r, &other})
    {
      one->run_until(fall + limit - one->period());
      CHECK(one->flag() == 0);
      one->run_until(fall + limit + one->period() / 2);
      CHECK(one->flag() == 1);
    }
    CHECK(other.model_bytes() == r.model_bytes());
  }
}

TEST_CASE("an RCC reset or a machine reset with the timer running leaves no TIMEOUT behind")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(bool const machine : {false, true})
    {
      CAPTURE(machine);
      timer_rig  r{part};
      auto const b = r.b();
      r.timeout(timoutens);
      b.command(0x33, 2, false, true);
      auto const fall  = r.now() + 10 * r.period();
      auto const limit = r.cycles(2048);
      r.run_until(fall + limit / 2);
      REQUIRE(timeout_events(*r.f).size() == 1);
      if(machine)
      {
        r.f->target->reset(popcorn::reset_kind::software);
        r.f->set("pc", sym(part.firmware, "idle"));
      }
      else
      {
        popcorn::testing::rcc_reset_pulse(*r.f, part.reset, "I2C1RST");
      }
      CHECK(timeout_events(*r.f).empty());
      CHECK(b.get("TIMEOUTR", "TIMOUTEN") == 0);
      b.set("RCC", part.enable, "I2C1EN", 1);
      write_timing(b);
      b.set("CR1", "ERRIE", 1);
      b.set("CR1", "PE", 1);
      r.timeout(timoutens);
      b.command(0x33, 2, false, true);
      auto const again = r.now() + 10 * r.period();
      r.run_until(again + limit / 2);
      CHECK(r.flag() == 0);
      CHECK(timeout_events(*r.f) == std::vector<std::uint64_t>{again + limit});
      r.run_until(again + limit - r.period());
      CHECK(r.flag() == 0);
      r.run_until(again + limit + r.period() / 2);
      CHECK(r.flag() == 1);
    }
  }
}

TEST_CASE("PE clear clears TIMEOUT and stops every timer")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    timer_rig  r{part};
    auto const b = r.b();
    r.timeout(timoutens | textens);
    b.command(0x33, 2, false, true);
    auto const limit = r.cycles(2048);
    r.run_until(r.now() + 10 * r.period() + limit + r.period());
    REQUIRE(r.flag() == 1);
    b.command(0x33, 2, false, true);
    r.run_until(r.now() + 10 * r.period() + limit / 2);
    REQUIRE(timeout_events(*r.f).size() == 1);
    b.set("CR1", "PE", 0);
    CHECK(r.flag() == 0);
    CHECK(timeout_events(*r.f).empty());
    r.run_until(r.now() + 2 * limit);
    CHECK(r.flag() == 0);
  }
}

TEST_CASE("200 waits each below the limit leave at most one TIMEOUT event pending")
{
  timer_rig  r{target_parts[0]};
  auto const b     = r.b();
  auto const limit = r.cycles(2048);
  r.timeout(timoutens | textens | timeout_b(4095));
  b.command(0x33, 200, false, true);
  r.run_until(r.now() + 10 * r.period());
  for(int index = 0; index < 200; ++index)
  {
    r.run_until(r.now() + limit / 4);
    CHECK(timeout_events(*r.f).size() == 1);
    b.send(static_cast<std::uint8_t>(index));
    r.run_until(r.now() + 9 * r.period() + r.period() / 2);
  }
  r.run_until(r.now() + 4 * r.period());
  CHECK(r.flag() == 0);
  CHECK(of_kind(r.faulty->events, 'w').size() == 200);
  CHECK(timeout_events(*r.f).empty());
}

TEST_CASE("PEC in target mode still logs once as not modelled and strict refuses")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(bool const strict : {false, true})
    {
      CAPTURE(strict);
      target_rig r{part, strict};
      r.b().set("CR1", "PECEN", 1);
      r.listen();
      auto const base = r.now() + us;
      r.start(own, false, base, 1);
      r.stop(base + target_ps, 2);
      r.start(own, false, base + 2 * target_ps, 3);
      if(strict)
      {
        CHECK_THROWS_AS(
            std::ignore = r.f->run(
                {.max_instructions = icount{1'000'000'000},
                 .until_time       = vtime{base + 4 * target_ps}}),
            popcorn::model_error);
        continue;
      }
      r.run_until(base + 4 * target_ps);
      CHECK(r.warnings("CR1") == 1);
    }
  }
}

TEST_CASE("an RXNE, TCR or TC wait holds SCL low too, TIMEOUT sends the STOP from there")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(std::string_view const wait : {"RXNE", "TCR", "TC"})
    {
      CAPTURE(wait);
      timer_rig  r{part};
      auto const b = r.b();
      r.timeout(timoutens);
      auto const start = r.now();
      auto       fall  = start + 19 * r.period();
      if(wait == "RXNE")
      {
        b.command(0x33, 2, true, true);
        fall = start + 28 * r.period();
      }
      else
      {
        b.command(0x33, 1, false, false, wait == "TCR");
        b.send(0x5A);
      }
      auto const limit = r.cycles(2048);
      r.run_until(fall + limit - r.period());
      CHECK(r.flag() == 0);
      CHECK(b.get("ISR", std::string{wait}) == 1);
      CHECK(timeout_events(*r.f) == std::vector<std::uint64_t>{fall + limit});
      r.run_until(fall + limit + 3 * r.period());
      CHECK(r.flag() == 1);
      CHECK(b.get("ISR", "STOPF") == 1);
      CHECK(b.get("ISR", "BUSY") == 0);
      CHECK(timeout_events(*r.f).empty());
    }
  }
}

TEST_CASE(
    "a TEXTEN timeout restarts the stretch sum, a repeated START stretch gets the whole limit")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part};
    r.listen(own, true);
    target_timeout(r, textens);
    auto const limit = target_cycles(r, 2048);
    auto const base  = r.now() + us;
    r.start(own, false, base, 1);
    auto const fall = base + target_ps;
    r.run_until(fall + limit + 10 * us);
    REQUIRE(r.get("ISR", "TIMEOUT") == 1);
    REQUIRE(r.sink->answers.size() == 1);
    r.b().set("ICR", "TIMOUTCF", 1);
    r.b().set("ICR", "ADDRCF", 1);
    auto const again = r.now() + us;
    r.start(own, false, again, 2);
    auto const second = again + target_ps;
    r.run_until(second + us);
    CHECK(r.get("ISR", "ADDR") == 1);
    CHECK(timeout_events(*r.f) == std::vector<std::uint64_t>{second + limit});
    r.run_until(second + limit - us);
    CHECK(r.get("ISR", "TIMEOUT") == 0);
    r.run_until(second + limit + us);
    CHECK(r.get("ISR", "TIMEOUT") == 1);
  }
}

namespace
{
class voter final : public periph::i2c_target
{
public:
  explicit voter(std::uint8_t own) : own_{own}
  {}

  std::optional<std::uint8_t> live;
  bool                        ack_address{true};
  std::uint8_t                byte{0xFF};
  std::uint64_t               stretch_ps{};
  periph::i2c_error           error{};
  std::uint32_t               error_call{~0u};
  std::uint32_t               calls{};
  std::vector<bus_event>      events;
  std::vector<std::uint64_t>  asked;

  [[nodiscard]] auto address() const -> std::uint8_t override
  {
    return own_;
  }

  [[nodiscard]] auto matches(std::uint8_t sent, vtime at) -> bool override
  {
    asked.push_back(at.ps);
    return live ? sent == *live : i2c_target::matches(sent, at);
  }

  auto start_address(std::uint8_t sent, bool read, vtime at) -> periph::i2c_reply override
  {
    calls = 0;
    events.push_back({read ? 'R' : 'W', sent, at.ps});
    return {.ack = ack_address, .error = pick(), .stretch_ps = stretch_ps};
  }

  auto start(bool, vtime at) -> periph::i2c_reply override
  {
    events.push_back({'S', 0, at.ps});
    return {};
  }

  auto write(std::uint8_t value, vtime at) -> periph::i2c_reply override
  {
    ++calls;
    events.push_back({'w', value, at.ps});
    return {.ack = true, .error = pick(), .stretch_ps = stretch_ps};
  }

  auto read(vtime at) -> periph::i2c_read_reply override
  {
    events.push_back({'?', 0, at.ps});
    return {};
  }

  auto read_acked(vtime at, bool ack) -> periph::i2c_read_reply override
  {
    ++calls;
    events.push_back({ack ? 'a' : 'n', byte, at.ps});
    return {.byte = byte, .error = pick(), .stretch_ps = stretch_ps};
  }

  void stop(vtime at) override
  {
    events.push_back({'P', 0, at.ps});
  }

  void save(std::vector<std::byte>& out) const override
  {
    out.push_back(static_cast<std::byte>(calls));
  }

  void load(std::span<std::byte const> in) override
  {
    REQUIRE(in.size() == 1);
    calls = std::to_integer<std::uint32_t>(in.front());
  }

  [[nodiscard]] auto kinds() const -> std::string
  {
    std::string out;
    for(auto const& event : events)
    {
      out.push_back(event.kind);
    }
    return out;
  }

private:
  [[nodiscard]] auto pick() const -> periph::i2c_error
  {
    return calls == error_call ? error : periph::i2c_error::none;
  }

  std::uint8_t own_;
};

auto voters(std::initializer_list<std::uint8_t> owns) -> std::vector<std::shared_ptr<voter>>
{
  std::vector<std::shared_ptr<voter>> out;
  for(auto const own : owns)
  {
    out.push_back(std::make_shared<voter>(own));
  }
  return out;
}

template <class Target = voter> struct fan_rig
{
  target_part                          part;
  std::vector<std::shared_ptr<Target>> targets;
  stm32::board                         board{};
  std::unique_ptr<stm32_fixture>       f;

  fan_rig(target_part const& which, std::vector<std::shared_ptr<Target>> attached)
      : part{which}, targets{std::move(attached)}
  {
    for(auto const& target : targets)
    {
      board.i2c["I2C1"].push_back(target);
    }
    auto options   = popcorn::testing::stm32_options();
    options.models = stm32::factories(board, part.desc());
    f = std::make_unique<stm32_fixture>(part.desc(), part.firmware, std::move(options));
    f->set("pc", sym(part.firmware, "idle"));
    enable();
  }

  void enable() const
  {
    auto const on = b();
    on.set("RCC", part.enable, "I2C1EN", 1);
    write_timing(on);
    on.set("CR1", "PE", 1);
  }

  [[nodiscard]] auto b() const -> bus
  {
    return bus{*f};
  }

  [[nodiscard]] auto period() const -> std::uint64_t
  {
    return kernel_ps(*f, 10);
  }

  [[nodiscard]] auto drive(std::uint64_t span, std::uint8_t first = 0x10) const
      -> std::vector<std::uint32_t>
  {
    auto const                 on   = b();
    auto                       next = first;
    std::vector<std::uint32_t> received;
    auto const                 until = on.now() + span;
    for(auto at = on.now() + period() / 2; at < until; at += period() / 2)
    {
      on.run_until(at);
      if(on.get("ISR", "TXIS") == 1)
      {
        on.send(next++);
      }
      if(on.get("ISR", "RXNE") == 1)
      {
        received.push_back(on.receive());
      }
    }
    return received;
  }

  [[nodiscard]] auto model_bytes() const -> std::vector<std::byte>
  {
    std::vector<std::byte> out;
    f->target->model("I2C1")->save(out);
    return out;
  }
};
}

TEST_CASE(
    "an address one target acknowledges and one refuses is acknowledged, bytes reach the first")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(bool const refuser_first : {false, true})
    {
      CAPTURE(refuser_first);
      fan_rig     r{part, voters({0x50, 0x50, 0x51})};
      auto const& acker    = r.targets[refuser_first ? 1 : 0];
      auto const& refuser  = r.targets[refuser_first ? 0 : 1];
      refuser->ack_address = false;
      r.b().command(0x50, 2, false, true);
      std::ignore = r.drive(40 * r.period());
      CHECK(r.b().get("ISR", "NACKF") == 0);
      CHECK(r.b().get("ISR", "STOPF") == 1);
      REQUIRE(acker->kinds() == "WwwP");
      CHECK(acker->events[1].value == 0x10);
      CHECK(acker->events[2].value == 0x11);
      CHECK(refuser->kinds() == "WP");
      CHECK(r.targets[2]->events.empty());
    }
    for(bool const refuser_first : {false, true})
    {
      CAPTURE(refuser_first);
      fan_rig     r{part, voters({0x50, 0x50})};
      auto const& acker    = r.targets[refuser_first ? 1 : 0];
      auto const& refuser  = r.targets[refuser_first ? 0 : 1];
      refuser->ack_address = false;
      acker->byte          = 0xF0;
      refuser->byte        = 0x0F;
      r.b().command(0x50, 2, true, true);
      CHECK(r.drive(40 * r.period()) == std::vector<std::uint32_t>{0xF0, 0xF0});
      CHECK(acker->kinds() == "RanP");
      CHECK(refuser->kinds() == "RP");
    }
    {
      fan_rig r{part, voters({0x50, 0x50})};
      r.targets[0]->ack_address = false;
      r.targets[1]->ack_address = false;
      r.b().command(0x50, 2, false, true);
      std::ignore = r.drive(40 * r.period());
      CHECK(r.b().get("ISR", "NACKF") == 1);
      CHECK(r.targets[0]->kinds() == "WP");
      CHECK(r.targets[1]->kinds() == "WP");
    }
  }
}

TEST_CASE("targets on one address read as the AND of their bytes, SCL held for the longer stretch")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(auto const& [read, longer_first] : {
            std::pair{true,  false},
            std::pair{true,  true },
            std::pair{false, false}
    })
    {
      CAPTURE(read);
      CAPTURE(longer_first);
      fan_rig    r{part, voters({0x50, 0x50})};
      auto const p             = r.period();
      r.targets[0]->byte       = 0xF0;
      r.targets[0]->stretch_ps = (longer_first ? 5 : 3) * p;
      r.targets[1]->byte       = 0x3C;
      r.targets[1]->stretch_ps = (longer_first ? 3 : 5) * p;
      r.b().command(0x50, 2, read, true);
      auto const begun    = r.b().now();
      auto const received = r.drive(80 * p);
      if(read)
      {
        CHECK(received == std::vector<std::uint32_t>{0x30, 0x30});
      }
      for(auto const& target : r.targets)
      {
        REQUIRE(target->events.size() == 4);
        CHECK(target->events[0].at == begun + 10 * p);
        CHECK(target->events[1].at - target->events[0].at == 5 * p + 9 * p);
        CHECK(target->events[2].at - target->events[1].at == 5 * p + 9 * p);
      }
      CHECK(r.targets[0]->kinds() == (read ? "RanP" : "WwwP"));
      CHECK(r.targets[1]->kinds() == r.targets[0]->kinds());
    }
  }
}

TEST_CASE("over several targets a byte ends with the first error in attachment order")
{
  using periph::i2c_error;
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(bool const swapped : {false, true})
    {
      CAPTURE(swapped);
      fan_rig r{part, voters({0x50, 0x50})};
      r.targets[0]->error      = swapped ? i2c_error::overrun : i2c_error::bus_error;
      r.targets[0]->error_call = 1;
      r.targets[1]->error      = swapped ? i2c_error::bus_error : i2c_error::overrun;
      r.targets[1]->error_call = 1;
      r.b().command(0x50, 2, false, true);
      std::ignore = r.drive(40 * r.period());
      CHECK(r.b().get("ISR", "BERR") == (swapped ? 0 : 1));
      CHECK(r.b().get("ISR", "OVR") == (swapped ? 1 : 0));
      CHECK(r.targets[0]->kinds() == "WwwP");
    }
    {
      fan_rig r{part, voters({0x50, 0x50})};
      r.targets[1]->error      = i2c_error::arbitration_lost;
      r.targets[1]->error_call = 0;
      r.b().command(0x50, 2, false, true);
      std::ignore = r.drive(40 * r.period());
      CHECK(r.b().get("ISR", "ARLO") == 1);
      CHECK(r.b().get("ISR", "NACKF") == 0);
      CHECK(r.b().get("ISR", "STOPF") == 0);
      CHECK(r.targets[0]->kinds() == "WP");
      CHECK(r.targets[1]->kinds() == "WP");
    }
    {
      fan_rig r{part, voters({0x50, 0x50})};
      r.targets[0]->error      = i2c_error::bus_error;
      r.targets[0]->error_call = 0;
      r.targets[1]->error      = i2c_error::arbitration_lost;
      r.targets[1]->error_call = 0;
      r.b().command(0x50, 1, false, true);
      std::ignore = r.drive(40 * r.period());
      CHECK(r.b().get("ISR", "BERR") == 1);
      CHECK(r.b().get("ISR", "ARLO") == 0);
      CHECK(r.targets[1]->kinds() == "WwP");
    }
  }
}

TEST_CASE("a target matching a live address is selected with the address sent, its address() not")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    fan_rig r{part, voters({0x50, 0x50})};
    r.targets[0]->live = 0x61;
    r.b().command(0x61, 1, false, true);
    std::ignore = r.drive(30 * r.period());
    REQUIRE(r.targets[0]->kinds() == "WwP");
    CHECK(r.targets[0]->events[0].value == 0x61);
    REQUIRE(r.targets[0]->asked.size() == 1);
    CHECK(r.targets[0]->asked.front() == r.targets[0]->events[0].at);
    CHECK(r.targets[1]->asked == r.targets[0]->asked);
    CHECK(r.targets[1]->events.empty());
    r.targets[0]->events.clear();
    r.b().command(0x50, 1, false, true);
    std::ignore = r.drive(30 * r.period());
    CHECK(r.targets[0]->events.empty());
    REQUIRE(r.targets[1]->kinds() == "WwP");
    CHECK(r.targets[1]->events[0].value == 0x50);
  }
}

TEST_CASE("the controller NACKs the byte that ends NBYTES with RELOAD clear, read_acked hears it")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    {
      fan_rig r{part, voters({0x50})};
      r.b().command(0x50, 3, true, true);
      CHECK(r.drive(60 * r.period()).size() == 3);
      CHECK(r.targets[0]->kinds() == "RaanP");
    }
    {
      fan_rig    r{part, voters({0x50})};
      auto const b = r.b();
      b.command(0x50, 2, true, false, true);
      CHECK(r.drive(40 * r.period()).size() == 2);
      REQUIRE(b.get("ISR", "TCR") == 1);
      CHECK(r.targets[0]->kinds() == "Raa");
      auto const cr2 = b.at("I2C1", "CR2");
      b.write(cr2, (b.read(cr2) & ~(0xFFu << 16 | 1u << 24)) | 1u << 16 | 1u << 25);
      CHECK(r.drive(30 * r.period()).size() == 1);
      CHECK(r.targets[0]->kinds() == "RaanP");
    }
  }
}

TEST_CASE("a reset ends the fan-out, the next address reaches every matching target again")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(bool const machine : {false, true})
    {
      CAPTURE(machine);
      fan_rig r{part, voters({0x50, 0x50})};
      r.b().command(0x50, 3, false, true);
      r.b().send(0x10);
      r.b().run_until(r.b().now() + 25 * r.period());
      REQUIRE(r.targets[1]->kinds() == "Ww");
      if(machine)
      {
        r.f->target->reset(popcorn::reset_kind::software);
        r.f->set("pc", sym(part.firmware, "idle"));
      }
      else
      {
        rcc_reset_pulse(*r.f, part.reset, "I2C1RST");
      }
      r.enable();
      r.b().run_until(r.b().now() + 30 * r.period());
      for(auto const& target : r.targets)
      {
        CHECK(target->kinds() == "Ww");
      }
      r.b().command(0x50, 1, false, true);
      std::ignore = r.drive(30 * r.period());
      for(auto const& target : r.targets)
      {
        CHECK(target->kinds() == "WwWwP");
      }
    }
  }
}

TEST_CASE(
    "a snapshot inside a fan-out transfer restores every acknowledging target, bytes re-dumped")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    auto const setup = [](fan_rig<>& r) {
      r.targets[1]->ack_address = false;
      r.targets[3]->ack_address = true;
    };
    fan_rig r{part, voters({0x50, 0x50, 0x51, 0x50})};
    setup(r);
    r.b().command(0x50, 2, false, true);
    r.b().send(0x10);
    r.b().run_until(r.b().now() + 25 * r.period());
    REQUIRE(r.targets[3]->kinds() == "Ww");
    auto const saved = r.f->target->snapshot();
    auto const bytes = r.model_bytes();

    fan_rig other{part, voters({0x50, 0x50, 0x51, 0x50})};
    setup(other);
    other.f->target->restore(
        core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
    CHECK(other.model_bytes() == bytes);
    std::ignore = other.drive(30 * other.period(), 0x11);
    CHECK(other.targets[0]->kinds() == "wP");
    CHECK(other.targets[1]->kinds() == "P");
    CHECK(other.targets[2]->events.empty());
    REQUIRE(other.targets[3]->kinds() == "wP");
    CHECK(other.targets[3]->events[0].value == 0x11);

    fan_rig    fewer{part, voters({0x50, 0x50})};
    auto const before = fewer.model_bytes();
    CHECK_THROWS_AS(
        fewer.f->target->restore(
            core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))}),
        popcorn::config_error);
    CHECK(fewer.model_bytes() == before);
  }
}

TEST_CASE("a STOP after a repeated START to another address reaches every target of the transfer")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    {
      fan_rig    r{part, voters({0x50, 0x51})};
      auto const b = r.b();
      b.command(0x50, 2, false, false);
      std::ignore = r.drive(30 * r.period());
      REQUIRE(b.get("ISR", "TC") == 1);
      CHECK(r.targets[0]->kinds() == "Www");
      b.command(0x51, 2, true, true);
      b.run_until(b.now() + 10 * r.period() + r.period() / 2);
      REQUIRE(r.targets[1]->kinds() == "R");
      auto const saved = r.f->target->snapshot();
      auto const bytes = r.model_bytes();
      CHECK(r.drive(40 * r.period()).size() == 2);
      CHECK(b.get("ISR", "STOPF") == 1);
      CHECK(r.targets[0]->kinds() == "WwwP");
      CHECK(r.targets[1]->kinds() == "RanP");
      CHECK(r.targets[0]->events.back().at == r.targets[1]->events.back().at);

      fan_rig other{part, voters({0x50, 0x51})};
      other.f->target->restore(
          core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
      CHECK(other.model_bytes() == bytes);
      CHECK(other.drive(40 * other.period()).size() == 2);
      CHECK(other.b().get("ISR", "STOPF") == 1);
      CHECK(other.targets[0]->kinds() == "P");
      CHECK(other.targets[1]->kinds() == "anP");
    }
    {
      auto const table = std::make_shared<devices::table_target>(
          devices::table_target_options{.address = 0x38, .register_bytes = 2});
      auto const other = std::make_shared<voter>(0x50);
      fan_rig    r{
          part, std::vector<std::shared_ptr<periph::i2c_target>>{table, other}
      };
      auto const b = r.b();
      b.command(0x38, 3, false, false);
      std::ignore = r.drive(40 * r.period(), 0x00);
      REQUIRE(b.get("ISR", "TC") == 1);
      b.command(0x50, 1, true, false);
      std::ignore = r.drive(20 * r.period());
      REQUIRE(b.get("ISR", "TC") == 1);
      CHECK(other->kinds() == "Rn");
      CHECK(table->writes().empty());
      b.set("CR2", "STOP", 1);
      b.run_until(b.now() + 5 * r.period());
      CHECK(b.get("ISR", "STOPF") == 1);
      REQUIRE(table->writes().size() == 1);
      CHECK(table->writes()[0] == devices::table_write{.reg = 0x0001, .bytes = {0x02}});
    }
  }
}

TEST_CASE("an EEPROM write cut by a repeated START to another address commits nothing at the STOP")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(bool const cut : {false, true})
    {
      CAPTURE(cut);
      auto const eeprom = std::make_shared<devices::eeprom_24xx>(eeprom_options());
      auto const other  = std::make_shared<voter>(0x51);
      fan_rig    r{
          part, std::vector<std::shared_ptr<periph::i2c_target>>{eeprom, other}
      };
      auto const b      = r.b();
      auto const before = std::to_integer<std::uint8_t>(eeprom->contents()[0x0102]);
      REQUIRE(before != 0x03);
      b.command(0x50, 4, false, not cut);
      std::ignore = r.drive(60 * r.period(), 0x01);
      if(cut)
      {
        REQUIRE(b.get("ISR", "TC") == 1);
        b.command(0x51, 1, true, true);
        std::ignore = r.drive(30 * r.period());
        CHECK(other->kinds() == "RnP");
      }
      REQUIRE(b.get("ISR", "STOPF") == 1);
      CHECK(
          std::to_integer<std::uint8_t>(eeprom->contents()[0x0102])
          == (cut ? before : std::uint8_t{0x03}));
      b.set("ICR", "STOPCF", 1);
      b.command(0x50, 0, false, true);
      b.run_until(b.now() + 20 * r.period());
      CHECK(b.get("ISR", "NACKF") == (cut ? 0u : 1u));
    }
  }
}

TEST_CASE("a target an address of the transfer adds keeps its attachment order in the transfer")
{
  using periph::i2c_error;
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    fan_rig    r{part, voters({0x51, 0x52, 0x50})};
    auto const b             = r.b();
    r.targets[0]->error      = i2c_error::bus_error;
    r.targets[0]->error_call = 1;
    r.targets[2]->error      = i2c_error::overrun;
    r.targets[2]->error_call = 1;
    b.command(0x50, 1, false, false);
    r.targets[2]->error_call = ~0u;
    std::ignore              = r.drive(30 * r.period());
    REQUIRE(b.get("ISR", "TC") == 1);
    REQUIRE(r.targets[2]->kinds() == "Ww");
    r.targets[0]->live       = 0x50;
    r.targets[2]->error_call = 1;
    b.command(0x50, 1, false, true);
    std::ignore = r.drive(30 * r.period());
    CHECK(b.get("ISR", "STOPF") == 1);
    CHECK(b.get("ISR", "BERR") == 1);
    CHECK(b.get("ISR", "OVR") == 0);
    CHECK(r.targets[0]->kinds() == "WwP");
    CHECK(r.targets[1]->events.empty());
    CHECK(r.targets[2]->kinds() == "WwWwP");
  }
}

namespace
{
constexpr std::uint32_t completed_kind = 5;

struct asked_phase
{
  char          kind{};
  std::uint8_t  value{};
  std::uint64_t at{};
  std::uint64_t ticket{};
};

class deferring final : public periph::i2c_target
{
public:
  explicit deferring(std::uint8_t own = 0x50) : own_{own}
  {}

  periph::i2c_target_port*     port{};
  int                          attaches{};
  int                          detaches{};
  std::optional<std::uint64_t> delay;
  bool                         hold{};
  bool                         ack_address{true};
  std::uint8_t                 byte{0x5A};
  periph::i2c_error            error{};
  std::uint32_t                error_call{~0u};
  std::uint32_t                calls{};
  std::uint64_t                stretch_ps{};
  std::uint32_t                lane{1};
  bool                         refuse_attach{};
  std::vector<asked_phase>     asked;
  std::vector<std::uint64_t>   stops;

  [[nodiscard]] auto address() const -> std::uint8_t override
  {
    return own_;
  }

  auto start(bool, vtime) -> periph::i2c_reply override
  {
    return {.ack = false};
  }

  auto write(std::uint8_t, vtime) -> periph::i2c_reply override
  {
    return {.ack = false};
  }

  auto read(vtime) -> periph::i2c_read_reply override
  {
    return {};
  }

  auto start_deferred(std::uint8_t sent, bool read, vtime at, std::uint64_t ticket)
      -> std::optional<periph::i2c_reply> override
  {
    calls = 0;
    asked.push_back({read ? 'R' : 'W', sent, at.ps, ticket});
    return given(
        periph::i2c_reply{.ack = ack_address, .error = pick(), .stretch_ps = stretch_ps}, at,
        ticket);
  }

  auto write_deferred(std::uint8_t value, vtime at, std::uint64_t ticket)
      -> std::optional<periph::i2c_reply> override
  {
    ++calls;
    asked.push_back({'w', value, at.ps, ticket});
    return given(
        periph::i2c_reply{.ack = true, .error = pick(), .stretch_ps = stretch_ps}, at, ticket);
  }

  auto read_deferred(vtime at, bool ack, std::uint64_t ticket)
      -> std::optional<periph::i2c_read_reply> override
  {
    ++calls;
    asked.push_back({ack ? 'a' : 'n', byte, at.ps, ticket});
    return given(
        periph::i2c_read_reply{.byte = byte, .error = pick(), .stretch_ps = stretch_ps}, at,
        ticket);
  }

  void stop(vtime at) override
  {
    stops.push_back(at.ps);
  }

  void attached(periph::i2c_target_port& to) override
  {
    if(refuse_attach)
    {
      throw std::runtime_error("the target refuses its port");
    }
    port = &to;
    ++attaches;
  }

  void detached() noexcept override
  {
    port = nullptr;
    ++detaches;
  }

  void save(std::vector<std::byte>& out) const override
  {
    out.push_back(static_cast<std::byte>(calls));
  }

  void load(std::span<std::byte const> in) override
  {
    REQUIRE(in.size() == 1);
    calls = std::to_integer<std::uint32_t>(in.front());
  }

  void send(std::uint64_t ticket, periph::i2c_reply reply, vtime at) const
  {
    port->complete(ticket, reply, at, lane);
  }

  void send(std::uint64_t ticket, periph::i2c_read_reply reply, vtime at) const
  {
    port->complete_read(ticket, reply, at, lane);
  }

  [[nodiscard]] auto kinds() const -> std::string
  {
    std::string out;
    for(auto const& one : asked)
    {
      out.push_back(one.kind);
    }
    return out;
  }

private:
  [[nodiscard]] auto pick() const -> periph::i2c_error
  {
    return calls == error_call ? error : periph::i2c_error::none;
  }

  template <class Reply>
  auto given(Reply reply, vtime at, std::uint64_t ticket) -> std::optional<Reply>
  {
    if(delay)
    {
      send(ticket, reply, vtime{at.ps + *delay});
      return std::nullopt;
    }
    if(hold)
    {
      return std::nullopt;
    }
    return reply;
  }

  std::uint8_t own_;
};

auto offset_of(stm32_fixture const& f) -> std::uint64_t
{
  auto const& clock = f.target->virtual_clock();
  return clock.since_power_on().ps - clock.time().ps;
}

auto completion_due(stm32_fixture const& f, std::optional<std::uint64_t> ticket = std::nullopt)
    -> std::optional<std::uint64_t>
{
  auto const& units = f.target->description().peripherals;
  auto const  found =
      std::ranges::find(units, std::string_view{"I2C1"}, &popcorn::peripheral_desc::name);
  REQUIRE(found != units.end());
  auto const owner = static_cast<std::uint32_t>(found - units.begin());
  for(auto const& event : f.target->snapshot().data().events.pending)
  {
    if(event.id.owner == owner
       and event.id.kind == completed_kind
       and (not ticket or event.payload == *ticket))
    {
      return event.due_ps;
    }
  }
  return std::nullopt;
}

auto refused_of(stm32_fixture const& f) -> std::int64_t
{
  return popcorn::testing::saved_integer(f, "I2C1", "refused").value_or(-1);
}

auto slow(std::optional<std::uint64_t> delay = std::nullopt) -> std::shared_ptr<deferring>
{
  auto made   = std::make_shared<deferring>();
  made->delay = delay;
  made->hold  = not delay;
  return made;
}
}

TEST_CASE("a deferred answer holds SCL from the phase end to its completion, the next phase after")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    auto const target = slow();
    fan_rig    r{part, std::vector{target}};
    auto const b  = r.b();
    auto const p  = r.period();
    auto const d  = 20 * p;
    target->delay = d;
    auto const t0 = b.now();
    b.command(0x50, 1, false, true);
    b.send(0x10);
    auto const a = t0 + 10 * p;
    b.run_until(a + p / 2);
    REQUIRE(target->kinds() == "W");
    CHECK(target->asked[0].at == a + offset_of(*r.f));
    CHECK(completion_due(*r.f, target->asked[0].ticket) == a + d);
    CHECK_FALSE(due_of_kind(*r.f, "I2C1", 0).has_value());
    CHECK(b.get("ISR", "BUSY") == 1);
    b.run_until(a + d + p / 2);
    CHECK(due_of_kind(*r.f, "I2C1", 0) == a + d + 9 * p);
    b.run_until(a + d + 9 * p + p / 2);
    REQUIRE(target->kinds() == "Ww");
    CHECK(target->asked[1].value == 0x10);
    CHECK(target->asked[1].at == a + d + 9 * p + offset_of(*r.f));
    CHECK(target->asked[1].ticket != target->asked[0].ticket);
    CHECK(completion_due(*r.f, target->asked[1].ticket) == a + 2 * d + 9 * p);
    b.run_until(a + 2 * d + 10 * p - p / 2);
    CHECK(b.get("ISR", "STOPF") == 0);
    b.run_until(a + 2 * d + 10 * p + p / 2);
    CHECK(b.get("ISR", "STOPF") == 1);
    REQUIRE(target->stops.size() == 1);
    CHECK(target->stops[0] == a + 2 * d + 10 * p + offset_of(*r.f));
    CHECK(refused_of(*r.f) == 0);
  }
}

TEST_CASE("a deferred read byte lands in RXDR at its completion, with the stretch after it")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    auto const target = slow();
    fan_rig    r{part, std::vector{target}};
    auto const b       = r.b();
    auto const period  = r.period();
    auto const d       = 20 * period;
    target->delay      = d;
    target->byte       = 0xC3;
    target->stretch_ps = 3 * period;
    auto const t0      = b.now();
    b.command(0x50, 1, true, true);
    auto const a = t0 + 10 * period;
    b.run_until(a + d + 3 * period + 9 * period + d - period / 2);
    CHECK(target->kinds() == "Rn");
    CHECK(target->asked[1].at == a + d + 3 * period + 9 * period + offset_of(*r.f));
    CHECK(b.get("ISR", "RXNE") == 0);
    b.run_until(a + 2 * d + 12 * period + period / 2);
    CHECK(b.get("ISR", "RXNE") == 1);
    CHECK(b.receive() == 0xC3);
    b.run_until(a + 2 * d + 16 * period - period / 2);
    CHECK(b.get("ISR", "STOPF") == 0);
    b.run_until(a + 2 * d + 16 * period + period / 2);
    CHECK(b.get("ISR", "STOPF") == 1);
  }
}

TEST_CASE("a deferred NACK, BERR or ARLO takes effect at the completion")
{
  using periph::i2c_error;
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    {
      auto const target = slow();
      fan_rig    r{part, std::vector{target}};
      auto const b        = r.b();
      auto const p        = r.period();
      auto const d        = 20 * p;
      target->delay       = d;
      target->ack_address = false;
      auto const a        = b.now() + 10 * p;
      b.command(0x50, 1, false, true);
      b.run_until(a + d - p / 2);
      CHECK(b.get("ISR", "NACKF") == 0);
      CHECK(b.get("ISR", "BUSY") == 1);
      b.run_until(a + d + p / 2);
      CHECK(b.get("ISR", "NACKF") == 1);
      b.run_until(a + d + p + p / 2);
      CHECK(b.get("ISR", "STOPF") == 1);
      REQUIRE(target->stops.size() == 1);
      CHECK(target->stops[0] == a + d + p + offset_of(*r.f));
    }
    {
      auto const target = slow();
      fan_rig    r{part, std::vector{target}};
      auto const b       = r.b();
      auto const p       = r.period();
      auto const d       = 20 * p;
      target->delay      = d;
      target->error      = i2c_error::bus_error;
      target->error_call = 1;
      auto const a       = b.now() + 10 * p;
      b.command(0x50, 1, false, true);
      b.send(0x10);
      b.run_until(a + 2 * d + 9 * p - p / 2);
      CHECK(b.get("ISR", "BERR") == 0);
      b.run_until(a + 2 * d + 9 * p + p / 2);
      CHECK(b.get("ISR", "BERR") == 1);
    }
    {
      auto const target = slow();
      fan_rig    r{part, std::vector{target}};
      auto const b       = r.b();
      auto const p       = r.period();
      auto const d       = 20 * p;
      target->delay      = d;
      target->error      = i2c_error::arbitration_lost;
      target->error_call = 0;
      auto const a       = b.now() + 10 * p;
      b.command(0x50, 1, false, true);
      b.send(0x10);
      b.run_until(a + d - p / 2);
      CHECK(b.get("ISR", "ARLO") == 0);
      b.run_until(a + d + p / 2);
      CHECK(b.get("ISR", "ARLO") == 1);
      CHECK(b.get("ISR", "BUSY") == 1);
      b.run_until(a + d + p + p / 2);
      CHECK(b.get("ISR", "BUSY") == 0);
      CHECK(b.get("ISR", "NACKF") == 0);
      CHECK(b.get("ISR", "STOPF") == 0);
      CHECK(target->kinds() == "W");
      REQUIRE(target->stops.size() == 1);
      CHECK(target->stops[0] == a + d + p + offset_of(*r.f));
    }
  }
}

TEST_CASE("deferred and synchronous answers of one phase join in attachment order at the latest")
{
  using periph::i2c_error;
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    {
      auto const now   = slow();
      auto const later = slow();
      now->hold        = false;
      now->byte        = 0xF0;
      later->byte      = 0x3C;
      fan_rig r{
          part, std::vector{now, later}
      };
      auto const b = r.b();
      auto const p = r.period();
      auto const d = 20 * p;
      later->delay = d;
      auto const a = b.now() + 10 * p;
      b.command(0x50, 1, true, true);
      b.run_until(a + 2 * d + 9 * p - p / 2);
      CHECK(b.get("ISR", "RXNE") == 0);
      REQUIRE(later->kinds() == "Rn");
      CHECK(now->asked[1].at == later->asked[1].at);
      CHECK(now->asked[0].ticket != later->asked[0].ticket);
      b.run_until(a + 2 * d + 9 * p + p / 2);
      CHECK(b.get("ISR", "RXNE") == 1);
      CHECK(b.receive() == 0x30);
    }
    {
      auto const first  = slow();
      auto const second = slow();
      fan_rig    r{
          part, std::vector{first, second}
      };
      auto const b       = r.b();
      auto const p       = r.period();
      first->delay       = 30 * p;
      second->delay      = 10 * p;
      first->error       = i2c_error::overrun;
      first->error_call  = 1;
      second->error      = i2c_error::bus_error;
      second->error_call = 1;
      auto const a       = b.now() + 10 * p;
      b.command(0x50, 1, false, true);
      b.send(0x10);
      auto const end = a + 30 * p + 9 * p + 30 * p;
      b.run_until(end - p / 2);
      CHECK(b.get("ISR", "OVR") == 0);
      CHECK(b.get("ISR", "BERR") == 0);
      b.run_until(end + p / 2);
      CHECK(b.get("ISR", "OVR") == 1);
      CHECK(b.get("ISR", "BERR") == 0);
    }
  }
}

TEST_CASE("a hold past TIMEOUTA sets TIMEOUT and the STOP leaves at the completion")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(bool const over : {false, true})
    {
      CAPTURE(over);
      auto const target = slow();
      fan_rig    r{part, std::vector{target}};
      auto const b     = r.b();
      auto const p     = r.period();
      auto const limit = kernel_ps(*r.f, 2048);
      auto const d     = over ? limit + 40 * p : limit - 40 * p;
      target->delay    = d;
      b.set("CR1", "ERRIE", 1);
      b.write(b.at("I2C1", "TIMEOUTR"), timoutens);
      auto const a = b.now() + 10 * p;
      b.command(0x50, 1, false, true);
      b.send(0x10);
      b.run_until(a + p / 2);
      CHECK(timeout_events(*r.f) == std::vector<std::uint64_t>{a + limit});
      b.run_until(a + limit + p / 2);
      CHECK(b.get("ISR", "TIMEOUT") == (over ? 1u : 0u));
      CHECK(target->stops.empty());
      b.run_until(a + (over ? d : 2 * d + 9 * p) + 30 * p);
      CHECK(b.get("ISR", "STOPF") == 1);
      REQUIRE(target->stops.size() == 1);
      if(over)
      {
        CHECK(target->kinds() == "W");
        CHECK(target->stops[0] == a + d + p + offset_of(*r.f));
        continue;
      }
      CHECK(b.get("ISR", "TIMEOUT") == 0);
      CHECK(target->kinds() == "Ww");
    }
  }
}

TEST_CASE("a hold is the target's stretch for TEXTEN, the wait after it counts from its end")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    auto const target = slow();
    fan_rig    r{part, std::vector{target}};
    auto const b     = r.b();
    auto const p     = r.period();
    auto const limit = kernel_ps(*r.f, 2048);
    auto const d     = 2 * limit;
    target->delay    = d;
    b.set("CR1", "ERRIE", 1);
    b.write(b.at("I2C1", "TIMEOUTR"), textens | timeout_b(0));
    auto const a = b.now() + 10 * p;
    b.command(0x50, 2, false, true);
    b.run_until(a + d - p);
    CHECK(timeout_events(*r.f).empty());
    CHECK(b.get("ISR", "TIMEOUT") == 0);
    b.run_until(a + d + p / 2);
    CHECK(b.get("ISR", "TXIS") == 1);
    CHECK(timeout_events(*r.f) == std::vector<std::uint64_t>{a + d + limit});
    b.run_until(a + d + limit - p / 2);
    CHECK(b.get("ISR", "TIMEOUT") == 0);
    b.run_until(a + d + limit + p / 2);
    CHECK(b.get("ISR", "TIMEOUT") == 1);
  }
}

TEST_CASE("a completion fires after a TIMEOUT due at the same count, posted before or after it")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    std::vector<std::vector<std::uint64_t>> outcomes;
    for(bool const inside : {true, false})
    {
      CAPTURE(inside);
      auto const target = slow();
      fan_rig    r{part, std::vector{target}};
      auto const b     = r.b();
      auto const p     = r.period();
      auto const limit = kernel_ps(*r.f, 2048);
      if(inside)
      {
        target->delay = limit;
      }
      b.write(b.at("I2C1", "TIMEOUTR"), timoutens);
      auto const a = b.now() + 10 * p;
      b.command(0x50, 1, false, true);
      b.send(0x10);
      b.run_until(a + p / 2);
      REQUIRE(target->kinds() == "W");
      if(not inside)
      {
        REQUIRE(timeout_events(*r.f) == std::vector<std::uint64_t>{a + limit});
        target->send(target->asked[0].ticket, {.ack = true}, vtime{a + limit + offset_of(*r.f)});
      }
      REQUIRE(completion_due(*r.f) == a + limit);
      b.run_until(a + limit + 20 * p);
      std::vector<std::uint64_t> outcome{b.get("ISR", "TIMEOUT"), target->asked.size()};
      outcome.insert(outcome.end(), target->stops.begin(), target->stops.end());
      outcomes.push_back(outcome);
    }
    REQUIRE(outcomes.size() == 2);
    CHECK(outcomes[0] == outcomes[1]);
    CHECK(outcomes[0][0] == 1);
    CHECK(outcomes[0][1] == 1);
  }
}

TEST_CASE("a deferred completion is due in the time base of the last reset, after a system reset")
{
  using popcorn::reset_kind;
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    auto const target = slow();
    fan_rig    r{part, std::vector{target}};
    auto const b  = r.b();
    auto const p  = r.period();
    auto const d  = 20 * p;
    target->delay = d;
    b.run_until(50 * us);
    for(auto const kind : {reset_kind::software, reset_kind::power_on})
    {
      CAPTURE(kind);
      r.f->target->reset(kind);
      r.f->set("pc", sym(part.firmware, "idle"));
      r.enable();
      auto const offset = offset_of(*r.f);
      if(kind == reset_kind::software)
      {
        REQUIRE(offset >= 50 * us);
      }
      else
      {
        REQUIRE(offset == 0);
      }
      target->asked.clear();
      target->stops.clear();
      auto const a = b.now() + 10 * p;
      b.command(0x50, 0, false, true);
      b.run_until(a + p / 2);
      REQUIRE(target->asked.size() == 1);
      CHECK(target->asked[0].at == a + offset);
      CHECK(completion_due(*r.f, target->asked[0].ticket) == a + d);
      b.run_until(a + d + p + p / 2);
      CHECK(b.get("ISR", "STOPF") == 1);
      REQUIRE(target->stops.size() == 1);
      CHECK(target->stops[0] == a + d + p + offset);
      b.set("ICR", "STOPCF", 1);
    }
  }
}

TEST_CASE("a STOP a TIMEOUT sends during a hold starts at a past-due completion's own time")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    auto const target = slow();
    fan_rig    r{part, std::vector{target}};
    auto const b     = r.b();
    auto const p     = r.period();
    auto const limit = kernel_ps(*r.f, 2048);
    auto const hz    = r.f->target->virtual_clock().instruction_hz();
    REQUIRE(1'000'000'000'000 % hz == 0);
    auto const instruction = 1'000'000'000'000 / hz;
    REQUIRE(3 * instruction < p);
    b.write(b.at("I2C1", "TIMEOUTR"), timoutens);
    auto const a = b.now() + 10 * p;
    b.command(0x50, 1, false, true);
    b.send(0x10);
    b.run_until(a + limit + p / 2);
    CHECK(b.get("ISR", "TIMEOUT") == 1);
    REQUIRE(target->kinds() == "W");
    auto const x = a + limit + 50 * p;
    b.run_until(x);
    auto const at = x - 3 * instruction;
    target->send(
        target->asked[0].ticket, periph::i2c_reply{.ack = true}, vtime{at + offset_of(*r.f)});
    b.run_until(x + 30 * p);
    CHECK(b.get("ISR", "STOPF") == 1);
    REQUIRE(target->stops.size() == 1);
    CHECK(target->stops[0] == at + p + offset_of(*r.f));
    CHECK(refused_of(*r.f) == 0);
  }
}

TEST_CASE("a target whose attached throws detaches the targets attached before it")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    auto const first      = slow();
    auto const second     = slow();
    auto const third      = slow();
    second->refuse_attach = true;
    CHECK_THROWS(
        fan_rig{
            part, std::vector{first, second, third}
    });
    CHECK(first->attaches == 1);
    CHECK(first->detaches == 1);
    CHECK(first->port == nullptr);
    CHECK(second->attaches == 0);
    CHECK(second->detaches == 0);
    CHECK(third->attaches == 0);
    CHECK(third->detaches == 0);
  }
}

TEST_CASE("a target is attached at the I2C's construction and detached once at its destruction")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    auto const target = slow();
    {
      fan_rig r{part, std::vector{target}};
      CHECK(target->attaches == 1);
      CHECK(target->port != nullptr);
      CHECK(target->detaches == 0);
    }
    CHECK(target->attaches == 1);
    CHECK(target->detaches == 1);
    CHECK(target->port == nullptr);
  }
}

TEST_CASE("every reset kind, a power cycle and PE clear drop a held phase, its completion refused")
{
  using popcorn::reset_kind;
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    auto const target = slow();
    fan_rig    r{part, std::vector{target}};
    auto const b        = r.b();
    auto const p        = r.period();
    auto       expected = std::int64_t{0};
    auto const leg      = [&](auto&& drop) {
      target->asked.clear();
      auto const a = b.now() + 10 * p;
      b.command(0x50, 0, false, true);
      b.run_until(a + p / 2);
      REQUIRE(target->asked.size() == 1);
      auto const stale = target->asked[0].ticket;
      target->send(stale, {.ack = false}, vtime{b.now() + 40 * p + offset_of(*r.f)});
      REQUIRE(completion_due(*r.f, stale).has_value());
      drop();
      CHECK(b.get("ISR", "BUSY") == 0);
      auto const kept = completion_due(*r.f).has_value();
      auto const t1   = b.now() + 10 * p;
      b.command(0x50, 0, false, true);
      b.run_until(t1 + p / 2);
      REQUIRE(target->asked.size() == 2);
      auto const now = b.now();
      if(not kept)
      {
        CHECK_FALSE(
            popcorn::testing::saved_integer(*r.f, "I2C1", "completions.0.ticket").has_value());
        target->send(stale, {.ack = false}, vtime{now + 2 * p + offset_of(*r.f)});
      }
      target->send(target->asked[1].ticket, {.ack = true}, vtime{now + 50 * p + offset_of(*r.f)});
      b.run_until(now + 45 * p);
      CHECK(refused_of(*r.f) == ++expected);
      CHECK(b.get("ISR", "NACKF") == 0);
      CHECK(b.get("ISR", "STOPF") == 0);
      b.run_until(now + 51 * p + p / 2);
      CHECK(b.get("ISR", "STOPF") == 1);
      CHECK(b.get("ISR", "NACKF") == 0);
      b.set("ICR", "STOPCF", 1);
    };
    auto const machine = [&](auto&& reset) {
      return [&, reset] {
        reset();
        CHECK_FALSE(completion_due(*r.f).has_value());
        r.f->set("pc", sym(part.firmware, "idle"));
        r.enable();
      };
    };
    for(auto const kind :
        {reset_kind::power_on, reset_kind::brown_out, reset_kind::pin, reset_kind::software,
         reset_kind::watchdog, reset_kind::window_watchdog, reset_kind::low_power,
         reset_kind::option_load})
    {
      CAPTURE(kind);
      leg(machine([&, kind] { r.f->target->reset(kind); }));
    }
    leg(machine([&] { r.f->target->power_cycle(); }));
    leg([&] {
      b.set("CR1", "PE", 0);
      b.set("CR1", "PE", 1);
    });
  }
}

TEST_CASE("after I2C1RST a completion is refused once, handed before or after the reset")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    std::vector<std::int64_t> counts;
    for(bool const before : {true, false})
    {
      CAPTURE(before);
      auto const target = slow();
      fan_rig    r{part, std::vector{target}};
      auto const b = r.b();
      auto const p = r.period();
      auto const a = b.now() + 10 * p;
      b.command(0x50, 0, false, true);
      b.run_until(a + p / 2);
      REQUIRE(target->asked.size() == 1);
      auto const stale = target->asked[0].ticket;
      auto const now   = b.now();
      auto const post  = [&] {
        target->send(stale, {.ack = false}, vtime{now + 15 * p + offset_of(*r.f)});
      };
      if(before)
      {
        post();
      }
      rcc_reset_pulse(*r.f, part.reset, "I2C1RST");
      r.enable();
      if(not before)
      {
        post();
      }
      CHECK(completion_due(*r.f, stale) == now + 15 * p);
      b.command(0x50, 0, false, true);
      b.run_until(now + 10 * p + p / 2);
      REQUIRE(target->asked.size() == 2);
      target->send(target->asked[1].ticket, {.ack = true}, vtime{now + 20 * p + offset_of(*r.f)});
      b.run_until(now + 17 * p);
      counts.push_back(refused_of(*r.f));
      CHECK(b.get("ISR", "NACKF") == 0);
      b.run_until(now + 21 * p + p / 2);
      CHECK(b.get("ISR", "STOPF") == 1);
      CHECK(b.get("ISR", "NACKF") == 0);
    }
    CHECK(counts == std::vector<std::int64_t>{1, 1});
  }
}

TEST_CASE("a snapshot inside a hold restores the same completion and phases, bytes re-dumped")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    auto const target = slow();
    fan_rig    r{part, std::vector{target}};
    auto const b  = r.b();
    auto const p  = r.period();
    auto const d  = 20 * p;
    target->delay = d;
    auto const a  = b.now() + 10 * p;
    b.command(0x50, 1, false, true);
    b.send(0x10);
    b.run_until(a + p / 2);
    REQUIRE(target->kinds() == "W");
    auto const saved = r.f->target->snapshot();
    auto const first = r.model_bytes();
    auto const run   = [&] {
      b.run_until(a + 2 * d + 10 * p + p / 2);
      std::vector<std::uint64_t> out{b.get("ISR", "STOPF")};
      for(auto const& one : target->asked)
      {
        out.push_back(one.at);
      }
      out.insert(out.end(), target->stops.begin(), target->stops.end());
      return out;
    };
    auto const before = run();
    r.f->target->restore(saved);
    CHECK(r.model_bytes() == first);
    target->asked.resize(1);
    target->stops.clear();
    CHECK(run() == before);
    CHECK(before[0] == 1);
    CHECK(before.size() == 4);
    CHECK(refused_of(*r.f) == 0);
  }
}

TEST_CASE("a completion of the wrong kind or a second one is refused, a bad call throws")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    auto const target = slow();
    fan_rig    r{part, std::vector{target}};
    auto const b = r.b();
    auto const p = r.period();
    b.run_until(30 * us);
    r.f->target->reset(popcorn::reset_kind::software);
    r.f->set("pc", sym(part.firmware, "idle"));
    r.enable();
    auto const offset = offset_of(*r.f);
    REQUIRE(offset >= 30 * us);
    auto const a = b.now() + 10 * p;
    b.command(0x50, 0, false, true);
    b.run_until(a + p / 2);
    REQUIRE(target->asked.size() == 1);
    auto const ticket = target->asked[0].ticket;
    auto const now    = b.now();
    CHECK_THROWS_AS(
        target->port->complete(ticket, {}, vtime{now + offset}, 0), std::invalid_argument);
    CHECK_THROWS_AS(
        target->send(ticket, periph::i2c_reply{}, vtime{offset - 1}), std::invalid_argument);
    CHECK_FALSE(completion_due(*r.f).has_value());
    target->send(ticket, periph::i2c_read_reply{.byte = 0}, vtime{now + p + offset});
    b.run_until(now + p + p / 2);
    CHECK(refused_of(*r.f) == 1);
    CHECK(b.get("ISR", "STOPF") == 0);
    auto const hz = r.f->target->virtual_clock().instruction_hz();
    REQUIRE(1'000'000'000'000 % hz == 0);
    auto const period = 1'000'000'000'000 / hz;
    auto const late   = b.now();
    target->send(ticket, {.ack = true}, vtime{late - 3 * period + offset});
    auto const pending = r.f->target->snapshot().data().events.pending;
    auto const kept    = std::ranges::find_if(pending, [&](core::event const& event) {
      return event.id.kind == completed_kind and event.due_ps == late - 3 * period;
    });
    REQUIRE(kept != pending.end());
    CHECK(kept->at.value + 3 == r.f->target->virtual_clock().now().value);
    CHECK(kept->lane == 1);
    target->send(ticket, {.ack = false}, vtime{late + 2 * p + offset});
    b.run_until(late + 3 * p);
    CHECK(b.get("ISR", "STOPF") == 1);
    CHECK(b.get("ISR", "NACKF") == 0);
    CHECK(refused_of(*r.f) == 2);
    b.set("ICR", "STOPCF", 1);

    auto const c = b.now() + 10 * p;
    b.command(0x50, 0, false, true);
    b.run_until(c + p / 2);
    REQUIRE(target->asked.size() == 2);
    b.set("CR1", "PE", 0);
    b.set("CR1", "PE", 1);
    target->send(target->asked[1].ticket, {.ack = false}, vtime{b.now() + p + offset});
    b.run_until(b.now() + 2 * p);
    CHECK(refused_of(*r.f) == 3);
    CHECK(b.get("ISR", "NACKF") == 0);
    CHECK(b.get("ISR", "BUSY") == 0);
  }
}

TEST_CASE("1000 refused I2C completions leave no record behind")
{
  auto const target = slow();
  fan_rig    r{target_parts[0], std::vector{target}};
  auto const b   = r.b();
  auto const now = b.now();
  for(std::uint64_t index = 0; index < 1000; ++index)
  {
    target->send(index + 1'000'000, periph::i2c_reply{}, vtime{now + us + index + offset_of(*r.f)});
  }
  CHECK(popcorn::testing::saved_integer(*r.f, "I2C1", "completions.999.ticket").has_value());
  b.run_until(now + 2 * us);
  CHECK(refused_of(*r.f) == 1000);
  CHECK_FALSE(popcorn::testing::saved_integer(*r.f, "I2C1", "completions.0.ticket").has_value());
  CHECK_FALSE(completion_due(*r.f).has_value());
}

namespace
{
class i2c_proxy final : public stm32::i2c_link_target
{
public:
  struct asked
  {
    char          kind{};
    std::uint8_t  value{};
    std::uint64_t at{};
    std::uint64_t local_ps{};
    std::uint64_t ticket{};
  };

  stm32::i2c_link_port*            link{};
  core::clock const*               clock{};
  periph::i2c_target_port*         port{};
  int                              links{};
  int                              attaches{};
  int                              detaches{};
  bool                             ack{true};
  bool                             hold{};
  bool                             refuse_attach{};
  bool                             acked{};
  std::uint8_t                     byte{0xFF};
  std::vector<asked>               heard;
  std::vector<std::string>         posts;
  std::vector<popcorn::reset_kind> resets;
  std::vector<std::uint64_t>       cuts;
  std::vector<std::uint64_t>       stops;
  std::vector<bool>                muted_in_hooks;

  void linked(stm32::i2c_link_port& to, core::clock const& machine) override
  {
    link  = &to;
    clock = &machine;
    ++links;
  }

  void attached(periph::i2c_target_port& to) override
  {
    muted_in_hooks.push_back(link->muted(*this));
    if(refuse_attach)
    {
      link  = nullptr;
      clock = nullptr;
      throw std::runtime_error("the proxy refuses its port");
    }
    port = &to;
    ++attaches;
  }

  void detached() noexcept override
  {
    muted_in_hooks.push_back(link->muted(*this));
    link  = nullptr;
    clock = nullptr;
    port  = nullptr;
    ++detaches;
  }

  [[nodiscard]] auto address() const -> std::uint8_t override
  {
    return 0x7F;
  }

  [[nodiscard]] auto matches(std::uint8_t, vtime) -> bool override
  {
    return true;
  }

  auto start(bool, vtime) -> periph::i2c_reply override
  {
    return {.ack = false};
  }

  auto write(std::uint8_t, vtime) -> periph::i2c_reply override
  {
    return {.ack = false};
  }

  auto read(vtime) -> periph::i2c_read_reply override
  {
    return {};
  }

  auto start_deferred(std::uint8_t sent, bool read, vtime at, std::uint64_t ticket)
      -> std::optional<periph::i2c_reply> override
  {
    record(read ? 'R' : 'W', sent, at, ticket);
    post("start");
    acked = ack;
    if(hold)
    {
      return std::nullopt;
    }
    return periph::i2c_reply{.ack = ack};
  }

  auto write_deferred(std::uint8_t value, vtime at, std::uint64_t ticket)
      -> std::optional<periph::i2c_reply> override
  {
    if(not acked)
    {
      record('x', value, at, ticket);
      return periph::i2c_reply{.ack = false};
    }
    record('w', value, at, ticket);
    post("write");
    return periph::i2c_reply{};
  }

  auto read_deferred(vtime at, bool sent_ack, std::uint64_t ticket)
      -> std::optional<periph::i2c_read_reply> override
  {
    if(not acked)
    {
      record('y', 0xFF, at, ticket);
      return periph::i2c_read_reply{};
    }
    record(sent_ack ? 'a' : 'n', byte, at, ticket);
    post("read");
    return periph::i2c_read_reply{.byte = byte};
  }

  void stop(vtime at) override
  {
    stops.push_back(at.ps);
    acked = false;
    post("stop");
  }

  void reset(popcorn::reset_kind kind) override
  {
    resets.push_back(kind);
    acked = false;
    post("reset");
  }

  void power_cut(vtime at) override
  {
    cuts.push_back(at.ps);
  }

  void save(std::vector<std::byte>&) const override
  {}

  void load(std::span<std::byte const>) override
  {}

  [[nodiscard]] auto kinds() const -> std::string
  {
    std::string out;
    for(auto const& one : heard)
    {
      out.push_back(one.kind);
    }
    return out;
  }

private:
  void record(char kind, std::uint8_t value, vtime at, std::uint64_t ticket)
  {
    auto const offset = clock->since_power_on().ps - clock->time().ps;
    heard.push_back(
        {.kind = kind, .value = value, .at = at.ps, .local_ps = at.ps - offset, .ticket = ticket});
  }

  void post(std::string const& what)
  {
    if(link != nullptr and link->muted(*this))
    {
      return;
    }
    posts.push_back(what);
  }
};

class i2c_link_sink final : public stm32::i2c_link_controller
{
public:
  periph::i2c_controller_port* port{};
  core::clock const*           clock{};
  int                          links{};
  int                          unlinks{};
  std::vector<heard>           answers;
  std::vector<lost>            drops;
  bool                         refuse{};

  void linked(periph::i2c_controller_port& to, core::clock const& machine) override
  {
    if(refuse)
    {
      throw std::runtime_error("the link controller refuses its port");
    }
    port  = &to;
    clock = &machine;
    ++links;
  }

  void unlinked() noexcept override
  {
    port  = nullptr;
    clock = nullptr;
    ++unlinks;
  }

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

auto link_port(stm32_fixture const& f) -> stm32::i2c_link_port&
{
  auto* const found = dynamic_cast<stm32::i2c_link_port*>(f.target->model("I2C1"));
  REQUIRE(found != nullptr);
  return *found;
}

auto encoded(stm32_fixture const& f) -> std::vector<std::byte>
{
  return core::encode_snapshot(f.target->snapshot().data());
}

auto no_targets() -> std::vector<std::shared_ptr<voter>>
{
  return {};
}
}

TEST_CASE("a link target next to a board target joins the fan-out and the AND, a detach restores")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    fan_rig     r{part, voters({0x50})};
    auto const& board = r.targets[0];
    auto const  proxy = std::make_shared<i2c_proxy>();
    auto const  p     = r.period();
    board->byte       = 0xF0;
    proxy->byte       = 0x3C;
    link_port(*r.f).attach_target(proxy);
    CHECK(proxy->links == 1);
    CHECK(proxy->attaches == 1);
    CHECK(proxy->clock == &r.f->target->virtual_clock());
    r.b().command(0x50, 2, true, true);
    CHECK(r.drive(40 * p) == std::vector<std::uint32_t>{0x30, 0x30});
    CHECK(board->kinds() == "RanP");
    CHECK(proxy->kinds() == "Ran");
    CHECK(proxy->posts == std::vector<std::string>{"start", "read", "read", "stop"});
    CHECK((proxy->heard[0].ticket & 0xFFFF'FFFFu) == 1);
    proxy->ack = false;
    r.b().command(0x50, 2, true, true);
    CHECK(r.drive(40 * p) == std::vector<std::uint32_t>{0xF0, 0xF0});
    CHECK(proxy->kinds() == "RanRyy");
    proxy->ack         = true;
    board->ack_address = false;
    r.b().command(0x50, 1, false, true);
    std::ignore = r.drive(40 * p);
    CHECK(r.b().get("ISR", "NACKF") == 0);
    CHECK(board->kinds() == "RanPRanPWP");
    CHECK(proxy->kinds() == "RanRyyWw");
    board->ack_address = true;
    link_port(*r.f).detach_target(*proxy);
    CHECK(proxy->detaches == 1);
    CHECK(proxy->link == nullptr);
    CHECK(proxy->clock == nullptr);
    CHECK(proxy->port == nullptr);
    r.b().command(0x50, 1, true, true);
    CHECK(r.drive(40 * p) == std::vector<std::uint32_t>{0xF0});
    CHECK(proxy->kinds() == "RanRyyWw");
  }
}

TEST_CASE(
    "a member snapshot has the same bytes with an I2C link target or link controller attached")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    std::vector<std::vector<std::byte>> saved;
    for(bool const linked : {false, true})
    {
      CAPTURE(linked);
      fan_rig    r{part, voters({0x50})};
      auto const proxy = std::make_shared<i2c_proxy>();
      auto const p     = r.period();
      if(linked)
      {
        link_port(*r.f).attach_target(proxy);
      }
      r.b().command(0x50, 3, false, true);
      std::ignore = r.drive(15 * p);
      saved.push_back(encoded(*r.f));
      std::ignore = r.drive(40 * p);
      saved.push_back(encoded(*r.f));
      CHECK(r.targets[0]->kinds() == "WwwwP");
      CHECK(proxy->kinds() == (linked ? "Wwww" : ""));
    }
    CHECK(saved[0] == saved[2]);
    CHECK(saved[1] == saved[3]);
    saved.clear();
    for(bool const linked : {false, true})
    {
      CAPTURE(linked);
      target_rig r{part, false, not linked};
      auto const sink = std::make_shared<i2c_link_sink>();
      if(linked)
      {
        link_port(*r.f).attach_controller(sink);
      }
      r.listen();
      auto const t = r.now() + us;
      r.start(own, false, t, 1);
      r.write(0x11, t + target_ps, 2);
      r.stop(t + 2 * target_ps, 3);
      r.run_until(t + target_ps + target_ps / 2);
      saved.push_back(encoded(*r.f));
      r.run_until(t + 3 * target_ps);
      saved.push_back(encoded(*r.f));
      CHECK((linked ? sink->answers.size() : r.sink->answers.size()) == 2);
    }
    CHECK(saved[0] == saved[2]);
    CHECK(saved[1] == saved[3]);
  }
}

TEST_CASE("attaching and detaching I2C link proxies twice leaves no reference behind")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    std::weak_ptr<i2c_proxy>     target_gone;
    std::weak_ptr<i2c_link_sink> sink_gone;
    {
      fan_rig    r{part, no_targets()};
      auto const proxy = std::make_shared<i2c_proxy>();
      target_gone      = proxy;
      for(int round = 1; round <= 2; ++round)
      {
        CAPTURE(round);
        link_port(*r.f).attach_target(proxy);
        CHECK(proxy->links == round);
        CHECK(proxy->attaches == round);
        CHECK(proxy->port != nullptr);
        link_port(*r.f).detach_target(*proxy);
        CHECK(proxy->detaches == round);
        CHECK(proxy->link == nullptr);
        CHECK(proxy->clock == nullptr);
        CHECK(proxy->port == nullptr);
      }
      link_port(*r.f).detach_target(*proxy);
      CHECK(proxy->detaches == 2);
    }
    CHECK(target_gone.expired());
    {
      target_rig r{part, false, false};
      auto const sink = std::make_shared<i2c_link_sink>();
      sink_gone       = sink;
      for(int round = 1; round <= 2; ++round)
      {
        CAPTURE(round);
        link_port(*r.f).attach_controller(sink);
        CHECK(sink->port != nullptr);
        CHECK(sink->clock == &r.f->target->virtual_clock());
        link_port(*r.f).detach_controller(*sink);
        CHECK(sink->unlinks == round);
        CHECK(sink->port == nullptr);
        CHECK(sink->clock == nullptr);
      }
    }
    CHECK(sink_gone.expired());
  }
}

TEST_CASE("an I2C link controller drives the target, and after a detach its units start from 1")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part, false, false};
    r.listen();
    for(int round = 1; round <= 2; ++round)
    {
      CAPTURE(round);
      auto const sink = std::make_shared<i2c_link_sink>();
      link_port(*r.f).attach_controller(sink);
      auto const t = r.now() + us;
      sink->port->start(own, false, target_ps, vtime{t}, wire, 1);
      sink->port->write(0x11, target_ps, vtime{t + target_ps}, wire, 2);
      sink->port->stop(vtime{t + 2 * target_ps}, wire, 3);
      r.run_until(t + 3 * target_ps);
      CHECK(
          sink->answers
          == std::vector<heard>{
              {1, on_address, true, 0xFF, t + target_ps    },
              {2, on_write,   true, 0xFF, t + 2 * target_ps}
      });
      CHECK(popcorn::testing::saved_integer(*r.f, "I2C1", "port.last_unit") == 3);
      link_port(*r.f).detach_controller(*sink);
      CHECK(popcorn::testing::saved_integer(*r.f, "I2C1", "port.last_unit") == 0);
      std::ignore = r.b().receive();
    }
  }
}

TEST_CASE("a bus release ends a link target's SCL low count until the transfer ends")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part, false, false};
    r.listen();
    target_timeout(r, timoutens);
    auto const limit = target_cycles(r, 2048);
    for(auto const released : {true, false})
    {
      CAPTURE(released);
      auto const sink = std::make_shared<i2c_link_sink>();
      link_port(*r.f).attach_controller(sink);
      auto const t    = r.now() + us;
      auto const fall = t + target_ps;
      sink->port->start(own, false, target_ps, vtime{t}, wire, 1);
      if(released)
      {
        link_port(*r.f).bus_released(vtime{fall + us}, 1);
      }
      r.run_until(fall + limit - us);
      CHECK(
          timeout_events(*r.f)
          == (released ? std::vector<std::uint64_t>{} : std::vector<std::uint64_t>{fall + limit}));
      r.run_until(fall + limit + us);
      CHECK(r.get("ISR", "TIMEOUT") == (released ? 0u : 1u));
      REQUIRE(sink->answers.size() == 1);
      CHECK(sink->answers[0].ack);
      sink->port->stop(vtime{r.now()}, wire, 2);
      r.run_until(r.now() + us);
      link_port(*r.f).detach_controller(*sink);
    }
  }
}

TEST_CASE("a link target's RCC reset keeps a bus release stored before or after it")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(auto const release_first : {true, false})
    {
      CAPTURE(release_first);
      target_rig r{part, false, false};
      r.listen();
      auto const sink = std::make_shared<i2c_link_sink>();
      link_port(*r.f).attach_controller(sink);
      auto const t     = r.now() + 10 * us;
      auto const fall  = t + target_ps;
      auto const limit = target_cycles(r, 2048);
      sink->port->start(own, false, target_ps, vtime{t}, wire, 1);
      if(release_first)
      {
        link_port(*r.f).bus_released(vtime{fall + us}, 1);
      }
      r.rcc_pulse();
      if(not release_first)
      {
        link_port(*r.f).bus_released(vtime{fall + us}, 1);
      }
      r.listen();
      target_timeout(r, timoutens);
      r.run_until(fall + limit + us);
      CHECK(timeout_events(*r.f).empty());
      CHECK(r.get("ISR", "TIMEOUT") == 0);
      REQUIRE(sink->answers.size() == 1);
      CHECK(sink->answers[0].ack);
      link_port(*r.f).detach_controller(*sink);
    }
  }
}

TEST_CASE("a link controller whose linked() throws leaves the instance as it was")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part, false, false};
    r.listen();
    auto const first = std::make_shared<i2c_link_sink>();
    link_port(*r.f).attach_controller(first);
    auto const t = r.now() + us;
    first->port->start(own, false, target_ps, vtime{t}, wire, 1);
    first->port->write(0x11, target_ps, vtime{t + target_ps}, wire, 2);
    first->port->stop(vtime{t + 2 * target_ps}, wire, 3);
    r.run_until(t + 3 * target_ps);
    std::ignore       = r.b().receive();
    auto const inside = r.f->target->snapshot();
    link_port(*r.f).detach_controller(*first);
    r.f->target->restore(inside);
    REQUIRE(popcorn::testing::saved_integer(*r.f, "I2C1", "port.last_unit") == 3);
    auto const refusing = std::make_shared<i2c_link_sink>();
    refusing->refuse    = true;
    CHECK_THROWS_AS(link_port(*r.f).attach_controller(refusing), std::runtime_error);
    CHECK(refusing->port == nullptr);
    CHECK(popcorn::testing::saved_integer(*r.f, "I2C1", "port.last_unit") == 3);
    auto const probe = std::make_shared<i2c_proxy>();
    link_port(*r.f).attach_target(probe);
    link_port(*r.f).detach_target(*probe);
    auto const sink = std::make_shared<i2c_link_sink>();
    link_port(*r.f).attach_controller(sink);
    CHECK(popcorn::testing::saved_integer(*r.f, "I2C1", "port.last_unit") == 0);
    auto const u = r.now() + us;
    sink->port->start(own, false, target_ps, vtime{u}, wire, 1);
    sink->port->stop(vtime{u + target_ps}, wire, 2);
    r.run_until(u + 2 * target_ps);
    CHECK(
        sink->answers
        == std::vector<heard>{
            {1, on_address, true, 0xFF, u + target_ps}
    });
    link_port(*r.f).detach_controller(*sink);
  }
}

TEST_CASE("an I2C link controller detach mid-transfer drops its entries unreported, frees the bus")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    target_rig r{part, false, false};
    auto const sink = std::make_shared<i2c_link_sink>();
    link_port(*r.f).attach_controller(sink);
    r.listen();
    auto const t = r.now() + us;
    sink->port->start(own, false, target_ps, vtime{t}, wire, 1);
    sink->port->write(0x11, target_ps, vtime{t + target_ps}, wire, 2);
    sink->port->stop(vtime{t + 2 * target_ps}, wire, 3);
    r.run_until(t + target_ps + target_ps / 2);
    CHECK(r.get("ISR", "BUSY") == 1);
    REQUIRE(sink->answers.size() == 1);
    link_port(*r.f).detach_controller(*sink);
    CHECK(r.get("ISR", "BUSY") == 0);
    r.run_until(t + 3 * target_ps);
    CHECK(sink->answers.size() == 1);
    CHECK(sink->drops.empty());
    CHECK(r.get("ISR", "STOPF") == 0);
    CHECK(r.get("ISR", "RXNE") == 0);
    auto const again = std::make_shared<i2c_link_sink>();
    link_port(*r.f).attach_controller(again);
    auto const later = r.now() + us;
    again->port->start(own, false, target_ps, vtime{later}, wire, 1);
    r.run_until(later + target_ps + us);
    CHECK(
        again->answers
        == std::vector<heard>{
            {1, on_address, true, 0xFF, later + target_ps}
    });
    CHECK(r.get("ISR", "BUSY") == 1);
  }
}

TEST_CASE("I2C link attaches are refused before any change, the instance keeps working")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    SUBCASE("a link proxy next to a board controller")
    {
      target_rig r{part};
      auto const sink  = std::make_shared<i2c_link_sink>();
      auto const proxy = std::make_shared<i2c_proxy>();
      CHECK_THROWS_AS(link_port(*r.f).attach_controller(sink), popcorn::config_error);
      CHECK_THROWS_AS(link_port(*r.f).attach_target(proxy), popcorn::config_error);
      CHECK(sink->links == 0);
      CHECK(proxy->links == 0);
      r.listen();
      auto const t = r.now() + us;
      r.start(own, false, t, 1);
      r.run_until(t + target_ps + us);
      CHECK(
          r.sink->answers
          == std::vector<heard>{
              {1, on_address, true, 0xFF, t + target_ps}
      });
    }
    SUBCASE("a link controller next to board targets or a link target")
    {
      fan_rig    r{part, voters({0x50})};
      auto const sink = std::make_shared<i2c_link_sink>();
      CHECK_THROWS_AS(link_port(*r.f).attach_controller(sink), popcorn::config_error);
      fan_rig    bare{part, no_targets()};
      auto const proxy = std::make_shared<i2c_proxy>();
      link_port(*bare.f).attach_target(proxy);
      CHECK_THROWS_AS(link_port(*bare.f).attach_controller(sink), popcorn::config_error);
      CHECK(sink->links == 0);
      r.b().command(0x50, 1, false, true);
      std::ignore = r.drive(40 * r.period());
      CHECK(r.targets[0]->kinds() == "WwP");
    }
    SUBCASE("an empty, repeated or throwing proxy")
    {
      fan_rig    r{part, no_targets()};
      auto&      link  = link_port(*r.f);
      auto const proxy = std::make_shared<i2c_proxy>();
      link.attach_target(proxy);
      CHECK_THROWS_AS(link.attach_target(proxy), std::invalid_argument);
      CHECK_THROWS_AS(link.attach_target(nullptr), std::invalid_argument);
      CHECK(proxy->links == 1);
      auto const other     = std::make_shared<i2c_proxy>();
      other->refuse_attach = true;
      CHECK_THROWS_AS(link.attach_target(other), std::runtime_error);
      CHECK(other->links == 1);
      CHECK(other->detaches == 0);
      CHECK_THROWS_AS(std::ignore = link.muted(*other), std::invalid_argument);
      CHECK_THROWS_AS(link.mute(*other), std::invalid_argument);
      CHECK_THROWS_AS(link.unmute(*other), std::invalid_argument);
      r.b().command(0x50, 1, false, true);
      std::ignore = r.drive(40 * r.period());
      CHECK(proxy->kinds() == "Ww");
      CHECK(other->heard.empty());
    }
    SUBCASE("an empty or second link controller")
    {
      target_rig r{part, false, false};
      auto&      link = link_port(*r.f);
      auto const sink = std::make_shared<i2c_link_sink>();
      CHECK_THROWS_AS(link.attach_controller(nullptr), std::invalid_argument);
      link.attach_controller(sink);
      auto const second = std::make_shared<i2c_link_sink>();
      CHECK_THROWS_AS(link.attach_controller(second), std::invalid_argument);
      CHECK_THROWS_AS(link.attach_target(std::make_shared<i2c_proxy>()), popcorn::config_error);
      CHECK(second->links == 0);
      r.listen();
      auto const t = r.now() + us;
      sink->port->start(own, false, target_ps, vtime{t}, wire, 1);
      r.run_until(t + target_ps + us);
      CHECK(sink->answers.size() == 1);
    }
    SUBCASE("a snapshot holding a phase a link target owes, restored with no link target")
    {
      fan_rig    r{part, no_targets()};
      auto const proxy = std::make_shared<i2c_proxy>();
      proxy->hold      = true;
      link_port(*r.f).attach_target(proxy);
      r.b().command(0x50, 1, false, true);
      r.b().run_until(r.b().now() + 11 * r.period());
      REQUIRE(proxy->kinds() == "W");
      auto const bytes = r.model_bytes();
      fan_rig    none{part, no_targets()};
      auto const before = none.model_bytes();
      CHECK_THROWS_AS(none.f->target->model("I2C1")->load(bytes), popcorn::config_error);
      CHECK(none.model_bytes() == before);
    }
  }
}

TEST_CASE("a muted I2C link target posts nothing at a machine reset, unmuted the next phase posts")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    fan_rig    r{part, no_targets()};
    auto&      link  = link_port(*r.f);
    auto const proxy = std::make_shared<i2c_proxy>();
    link.attach_target(proxy);
    CHECK_FALSE(link.muted(*proxy));
    r.b().run_until(r.b().now() + 30 * us);
    link.mute(*proxy);
    CHECK(link.muted(*proxy));
    r.f->target->reset(popcorn::reset_kind::software);
    r.f->set("pc", sym(part.firmware, "idle"));
    r.enable();
    CHECK(proxy->resets == std::vector{popcorn::reset_kind::software});
    CHECK(proxy->posts.empty());
    link.unmute(*proxy);
    CHECK_FALSE(link.muted(*proxy));
    auto const t0 = r.b().now();
    r.b().command(0x50, 1, false, true);
    std::ignore = r.drive(40 * r.period());
    CHECK(proxy->posts == std::vector<std::string>{"start", "write", "stop"});
    REQUIRE_FALSE(proxy->heard.empty());
    CHECK(offset_of(*r.f) >= 30 * us);
    CHECK(proxy->heard[0].local_ps == t0 + 10 * r.period());
    CHECK(proxy->heard[0].at == t0 + 10 * r.period() + offset_of(*r.f));
    link.mute(*proxy);
    link.detach_target(*proxy);
    link.attach_target(proxy);
    CHECK_FALSE(link.muted(*proxy));
    CHECK(proxy->muted_in_hooks == std::vector<bool>{false, true, false});
  }
}

TEST_CASE("a power cycle reaches an I2C link target")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    fan_rig    r{part, voters({0x50})};
    auto const proxy = std::make_shared<i2c_proxy>();
    link_port(*r.f).attach_target(proxy);
    r.b().run_until(r.b().now() + 10 * us);
    r.f->target->power_cycle();
    CHECK(proxy->cuts.size() == 1);
    CHECK(proxy->resets == std::vector{popcorn::reset_kind::power_on});
  }
}

TEST_CASE("an I2C link target owing an answer completes it before its detach, else the phase holds")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(bool const completes : {false, true})
    {
      CAPTURE(completes);
      fan_rig    r{part, no_targets()};
      auto const proxy = std::make_shared<i2c_proxy>();
      proxy->hold      = true;
      link_port(*r.f).attach_target(proxy);
      auto const b  = r.b();
      auto const p  = r.period();
      auto const t0 = b.now();
      b.command(0x50, 1, false, true);
      b.send(0x10);
      auto const a = t0 + 10 * p;
      b.run_until(a + p / 2);
      REQUIRE(proxy->kinds() == "W");
      if(completes)
      {
        proxy->port->complete(
            proxy->heard[0].ticket, periph::i2c_reply{}, vtime{a + 5 * p + offset_of(*r.f)}, 1);
      }
      link_port(*r.f).detach_target(*proxy);
      b.run_until(a + 60 * p);
      CHECK(b.get("ISR", "STOPF") == (completes ? 1u : 0u));
      CHECK(b.get("ISR", "BUSY") == (completes ? 0u : 1u));
      CHECK(b.get("ISR", "NACKF") == (completes ? 1u : 0u));
      CHECK(refused_of(*r.f) == 0);
      b.set("CR1", "PE", 0);
      CHECK(b.get("ISR", "BUSY") == 0);
    }
  }
}

TEST_CASE("an I2C link target attached during a transfer hears its later bytes unacknowledged")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    fan_rig    r{part, voters({0x50})};
    auto const proxy = std::make_shared<i2c_proxy>();
    auto const p     = r.period();
    r.b().command(0x50, 3, false, true);
    std::ignore = r.drive(15 * p);
    link_port(*r.f).attach_target(proxy);
    std::ignore = r.drive(60 * p);
    CHECK(r.targets[0]->kinds() == "WwwwP");
    CHECK(proxy->kinds() == "xxx");
    CHECK(proxy->posts == std::vector<std::string>{"stop"});
    CHECK(r.b().get("ISR", "NACKF") == 0);
    CHECK(r.b().get("ISR", "STOPF") == 1);
  }
}

TEST_CASE("an RCC reset restarts the phases a link controller queued from their posted time")
{
  auto const p = target_ps;
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(bool const linked : {false, true})
    {
      CAPTURE(linked);
      for(auto const posted : {p / 4, 9 * p / 10})
      {
        CAPTURE(posted);
        std::vector<std::vector<heard>>     answers;
        std::vector<std::vector<lost>>      drops;
        std::vector<std::vector<std::byte>> bytes;
        std::uint64_t                       t{};
        for(bool const after : {false, true})
        {
          CAPTURE(after);
          target_rig r{part, false, not linked};
          auto const sink = std::make_shared<i2c_link_sink>();
          if(linked)
          {
            link_port(*r.f).attach_controller(sink);
          }
          r.listen();
          t = r.now() + us;
          r.start(own, false, t, 1);
          auto const later = [&] {
            r.write(0x11, t + posted, 2);
            r.start(own, false, t + 11 * p / 10, 3);
          };
          if(not after)
          {
            later();
          }
          r.run_until(t + p / 2);
          r.rcc_pulse();
          if(after)
          {
            later();
          }
          r.run_until(t + 4 * p);
          answers.push_back(linked ? sink->answers : r.sink->answers);
          drops.push_back(linked ? sink->drops : r.sink->drops);
          bytes.push_back(r.model_bytes());
        }
        CHECK(answers[0] == answers[1]);
        CHECK(drops[0] == drops[1]);
        CHECK(bytes[0] == bytes[1]);
        auto const        early = posted == p / 4;
        std::vector<lost> expected_drops{
            {1, on_address, t + p}
        };
        std::vector<heard> expected;
        if(not linked)
        {
          expected = {
              {2, on_write,   false, 0xFF, t + 2 * p},
              {3, on_address, false, 0xFF, t + 3 * p}
          };
        }
        else if(early)
        {
          expected_drops.push_back({2, on_write, t + posted + p});
          expected = {
              {3, on_address, false, 0xFF, t + 11 * p / 10 + p}
          };
        }
        else
        {
          expected = {
              {2, on_write,   false, 0xFF, t + posted + p    },
              {3, on_address, false, 0xFF, t + posted + 2 * p}
          };
        }
        CHECK(drops[0] == expected_drops);
        CHECK(answers[0] == expected);
      }
    }
  }
}

TEST_CASE("a read byte held at an RCC reset runs from the reset under a link or a board controller")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(bool const linked : {false, true})
    {
      CAPTURE(linked);
      target_rig r{part, false, not linked};
      auto const sink = std::make_shared<i2c_link_sink>();
      if(linked)
      {
        link_port(*r.f).attach_controller(sink);
      }
      r.listen();
      auto const t = r.now() + us;
      r.start(own, true, t, 1);
      r.read(true, t + target_ps, 2);
      r.run_until(t + 3 * target_ps);
      CHECK(r.get("ISR", "TXIS") == 1);
      auto const reset_at = r.now();
      r.rcc_pulse();
      r.run_until(reset_at + 2 * target_ps);
      auto const& heard_drops = linked ? sink->drops : r.sink->drops;
      CHECK(
          heard_drops
          == std::vector<lost>{
              {2, on_read, reset_at + target_ps}
      });
    }
  }
}

TEST_CASE("CR1 keeps NOSTRETCH, ANFOFF and DNF while PE is set, one warning, strict refuses")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(bool const strict : {false, true})
    {
      CAPTURE(strict);
      target_rig r{part, strict};
      auto const b = r.b();
      r.listen();
      auto&      regs           = r.f->target->peripheral("I2C1");
      auto*      model          = r.f->target->model("I2C1");
      auto const offset         = b.reg("I2C1", "CR1").offset;
      auto const firmware_write = [&](std::uint32_t value) {
        return model->on_write(
            regs, offset, 4, regs.peek(offset), value, {.pc = 0, .now = icount{0}});
      };
      auto const mask = [&](std::string_view name) {
        auto const& fields = b.reg("I2C1", "CR1").fields;
        auto const  found  = std::ranges::find(fields, name, &popcorn::field_desc::name);
        REQUIRE(found != fields.end());
        return static_cast<std::uint32_t>(((std::uint64_t{1} << found->width) - 1u) << found->lsb);
      };
      auto const fixed = mask("NOSTRETCH") | mask("ANFOFF") | mask("DNF");
      if(strict)
      {
        CHECK_THROWS_AS(
            std::ignore = firmware_write(regs.peek(offset) | fixed), popcorn::model_error);
        CHECK(r.get("CR1", "NOSTRETCH") == 0);
        continue;
      }
      REQUIRE(firmware_write(regs.peek(offset) | fixed).has_value());
      CHECK(r.get("CR1", "NOSTRETCH") == 0);
      CHECK(r.get("CR1", "ANFOFF") == 0);
      CHECK(r.get("CR1", "DNF") == 0);
      CHECK(r.get("CR1", "PE") == 1);
      REQUIRE(firmware_write(regs.peek(offset) | mask("DNF") | mask("ERRIE")).has_value());
      CHECK(r.get("CR1", "DNF") == 0);
      CHECK(r.get("CR1", "ERRIE") == 1);
      CHECK(r.warnings("CR1") == 1);
      b.set("CR1", "NOSTRETCH", 1);
      CHECK(r.get("CR1", "NOSTRETCH") == 0);
      REQUIRE(firmware_write(regs.peek(offset) & ~mask("PE")).has_value());
      REQUIRE(firmware_write(regs.peek(offset) | mask("NOSTRETCH")).has_value());
      CHECK(r.get("CR1", "NOSTRETCH") == 1);
      CHECK(r.warnings("CR1") == 1);
    }
  }
}

TEST_CASE("a target that throws from start or read leaves the controller going, the firmware ends")
{
  for(auto const& [kind, nth] : {
          std::pair{'s', 3u},
          std::pair{'r', 3u}
  })
  {
    CAPTURE(kind);
    rig r{"eeprom_rw"};
    r.memory->throw_on  = kind;
    r.memory->throw_nth = nth;
    CHECK_THROWS_AS(std::ignore = r.f->run_to_stop(), std::runtime_error);
    CHECK(r.finish().size() == 3);
    CHECK(r.memory->seen > nth);
  }
}

TEST_CASE("a target that throws from read gives a byte of ones")
{
  rig r{"eeprom_rw"};
  r.memory->throw_on  = 'r';
  r.memory->throw_nth = 3;
  CHECK_THROWS_AS(std::ignore = r.f->run_to_stop(), std::runtime_error);
  CHECK(r.finish().size() == 3);
  auto const byte = r.f->target->mem().read(sym("i2c_ll", "buffer") + 2, 1);
  REQUIRE(byte.has_value());
  CHECK(*byte == 0xFF);
}

TEST_CASE("a target that throws from stop or matches leaves the controller going")
{
  for(auto const kind : {'p', 'm'})
  {
    CAPTURE(kind);
    rig r{"eeprom_rw"};
    r.memory->throw_on  = kind;
    r.memory->throw_nth = 2;
    CHECK_THROWS_AS(std::ignore = r.f->run_to_stop(), std::runtime_error);
    CHECK(r.finish().size() == 3);
    CHECK(r.memory->seen > 2);
  }
}

TEST_CASE("a target that throws from write ends the transfer on the wire as a NACK")
{
  for(auto const nth : {1u, 10u})
  {
    CAPTURE(nth);
    rig r{"idle"};
    bus b{*r.f};
    b.controller();
    r.memory->throw_on  = 'w';
    r.memory->throw_nth = nth;
    b.command(0x50, 20, false, true);
    auto thrown = false;
    for(std::uint32_t index = 0; index < 20 and not thrown; ++index)
    {
      b.send(static_cast<std::uint8_t>(index));
      try
      {
        std::ignore = r.f->run_to_stop(
            {.max_instructions = icount{1'000'000'000}, .until_time = vtime{b.now() + 100 * us}});
      }
      catch(std::runtime_error const&)
      {
        thrown = true;
      }
    }
    REQUIRE(thrown);
    b.run_until(b.now() + 200 * us);
    CHECK(b.get("ISR", "NACKF") == 1);
    CHECK(b.get("ISR", "STOPF") == 1);
    CHECK(b.get("ISR", "BUSY") == 0);
  }
}

TEST_CASE("a target that throws from stop still starts the START queued behind the STOP")
{
  rig r{"idle"};
  bus b{*r.f};
  b.set("RCC", "RCC_APB1ENR1", "I2C1EN", 1);
  b.write(b.at("I2C1", "TIMINGR"), 0x0010'0305);
  b.set("I2C1", "CR1", "PE", 1);
  b.command(0x50, 1, false, false);
  b.run_until(b.now() + 100 * us);
  b.send(0x00);
  b.run_until(b.now() + 100 * us);
  CHECK(b.f.field("I2C1", "ISR", "TC") == 1);
  r.memory->throw_on  = 'p';
  r.memory->throw_nth = 1;
  b.set("I2C1", "CR2", "STOP", 1);
  b.set("I2C1", "CR2", "START", 1);
  auto const until = b.now() + 300 * us;
  CHECK_THROWS_AS(
      std::ignore =
          r.f->run_to_stop({.max_instructions = icount{1'000'000'000}, .until_time = vtime{until}}),
      std::runtime_error);
  b.run_until(b.now() + 300 * us);
  CHECK(b.f.field("I2C1", "ISR", "BUSY") == 1);
}

TEST_CASE(
    "the STOP after a timeout leaves one kernel period after the expiry instant, not after the end "
    "of the instruction that holds it, at cpi 1 to 4")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(double const cpi : {1.0, 2.0, 3.0, 4.0})
    {
      CAPTURE(cpi);
      timer_rig  r{part, false, cpi};
      auto const b = r.b();
      r.timeout(timoutens);
      b.command(0x33, 2, false, true);
      r.run_until(r.now() + 20 * r.period());
      auto const due = timeout_events(*r.f);
      REQUIRE(due.size() == 1);
      r.run_until(due[0] + 10 * r.period());
      auto const& events = r.faulty->events;
      REQUIRE_FALSE(events.empty());
      REQUIRE(events.back().kind == 'P');
      CHECK(events.back().at == due[0] + r.period());
    }
  }
}

TEST_CASE(
    "a START the firmware requested during a peer transfer begins at the instant of the peer STOP, "
    "not at the end of the instruction that holds it, at cpi 1 to 4")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(double const cpi : {1.0, 2.0, 3.0, 4.0})
    {
      CAPTURE(cpi);
      target_rig r{part, false, true, cpi};
      auto const b = r.b();
      r.listen();
      write_timing(b);
      auto const base = r.now() + us;
      auto const stop = base + target_ps + 20 * us + 137;
      r.start(own, false, base, 1);
      r.stop(stop, 2);
      r.run_until(base + target_ps + 5 * us);
      b.command(0x38, 0, false, true);
      r.run_until(stop + 3 * us);
      auto const due = due_of_kind(*r.f, "I2C1", 0);
      REQUIRE(due.has_value());
      CHECK(
          *due - stop == (part.name == std::string_view{"STM32U575"} ? 25 * us : 3125 * us / 1000));
    }
  }
}

TEST_CASE(
    "a target stretching the address phase past the SCL low timeout answers its peer at the expiry "
    "instant, not at the end of the instruction that holds it, at cpi 1 to 4")
{
  for(auto const& part : target_parts)
  {
    CAPTURE(part.name);
    for(double const cpi : {1.0, 2.0, 3.0, 4.0})
    {
      CAPTURE(cpi);
      target_rig r{part, false, true, cpi};
      auto const b = r.b();
      r.listen(own, true);
      write_timing(b);
      b.write(b.at("I2C1", "TIMEOUTR"), timoutens);
      auto const base = r.now() + us;
      r.start(own, false, base, 1);
      r.run_until(base + target_ps + 5 * us);
      auto const due = timeout_events(*r.f);
      REQUIRE(due.size() == 1);
      r.run_until(due[0] + 20 * us);
      REQUIRE(r.sink->answers.size() == 1);
      CHECK(r.sink->answers[0].at == due[0]);
    }
  }
}
