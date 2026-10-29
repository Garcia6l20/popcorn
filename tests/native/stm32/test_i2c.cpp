#include "dma.hpp"
#include "factories.hpp"
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
  bool throw_at_cut{};

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
      options.models.insert_or_assign("I2C1", stm32::i2c_factory(board, *variant));
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
};

constexpr std::array<target_part, 2> target_parts{
    {
     {"STM32U575", &u575, "i2c_ll", "RCC_APB1ENR1", "RCC_APB1RSTR1", 55},
     {"STM32H573", &popcorn::testing::h573, "h573_i2c_ll", "RCC_APB1LENR", "RCC_APB1LRSTR", 51},
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
      bool               controlled = true)
      : part{which}
  {
    if(controlled)
    {
      board.i2c_controllers["I2C1"] = sink;
    }
    auto options               = popcorn::testing::stm32_options();
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
    r.run_until(base + target_ps / 2);
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
