#include "factories.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/usb.hpp>
#include <popcorn/periph/usb.hpp>
#include <popcorn/periph/usb_host.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core  = popcorn::core;
namespace stm32 = popcorn::periph::stm32;
namespace usb   = popcorn::periph::usb;
using popcorn::icount;
using popcorn::part_description;
using popcorn::reset_kind;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::l152rb;
using popcorn::testing::l152re;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr std::uint64_t us = 1'000'000;
constexpr std::uint64_t ms = 1'000 * us;

constexpr std::uint32_t lp_irq     = 20;
constexpr std::uint32_t hp_irq     = 19;
constexpr std::uint32_t wakeup_irq = 42;
constexpr std::uint32_t exti_base  = 0x4001'0400;

constexpr std::uint32_t disabled = 0;
constexpr std::uint32_t stall    = 1;
constexpr std::uint32_t nak      = 2;
constexpr std::uint32_t valid    = 3;

constexpr std::uint32_t bulk    = 0;
constexpr std::uint32_t control = 1;

constexpr usb::setup_packet
    get_device{.request_type = 0x80, .request = 6, .value = 0x0100, .index = 0, .length = 18};

struct recording_host final : usb::host_side
{
  std::vector<std::pair<bool, std::uint64_t>> changes;

  void pull_up_changed(bool on, vtime at) override
  {
    changes.emplace_back(on, at.ps);
  }

  void on_time(vtime) override
  {}

  void save(std::vector<std::byte>&) const override
  {}

  void load(std::span<std::byte const>) override
  {}
};

struct endpoint_setup
{
  std::uint32_t address{};
  std::uint32_t type{bulk};
  std::uint32_t stat_tx{disabled};
  std::uint32_t stat_rx{disabled};
  std::uint32_t kind{};
  std::uint32_t dtog_tx{};
  std::uint32_t dtog_rx{};
};

struct rig
{
  std::string                     elf;
  std::unique_ptr<stm32_fixture>  f;
  std::shared_ptr<recording_host> host = std::make_shared<recording_host>();
  std::vector<core::log_record>   logged;
  core::hook_handle               sink;

  explicit rig(bool strict = true, part_description const& part = l152re())
      : elf{&part == &l152rb() ? "l152xb_l1_exti_ll" : "l152xe_l1_exti_ll"}
  {
    auto options               = popcorn::testing::stm32_options();
    options.strict_peripherals = strict;
    start(part, std::move(options));
  }

  explicit rig(stm32::board const& board) : elf{"l152xe_l1_exti_ll"}
  {
    auto options               = popcorn::testing::stm32_options();
    options.strict_peripherals = true;
    auto factories             = stm32::factories(board, l152re());
    options.models.emplace("USB", std::move(factories.at("USB")));
    start(l152re(), std::move(options));
  }

  void start(part_description const& part, core::machine_options options)
  {
    f = std::make_unique<stm32_fixture>(part, elf, std::move(options));
    f->set("pc", sym(elf, "idle"));
    sink = f->target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
  }

  [[nodiscard]] auto warnings() const -> std::size_t
  {
    return static_cast<std::size_t>(
        std::ranges::count(logged, core::log_kind::model_warning, &core::log_record::kind));
  }

  [[nodiscard]] auto port() const -> usb::device_port&
  {
    auto* const found = f->model_of<usb::device_port>("USB");
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto reg_of(std::string_view unit, std::string_view name) const
      -> popcorn::register_desc const&
  {
    auto const* found = popcorn::find_register(f->target->peripheral(unit).description(), name);
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto at(std::string_view unit, std::string_view name) const -> popcorn::address
  {
    return f->target->peripheral(unit).description().base + reg_of(unit, name).offset;
  }

  [[nodiscard]] auto
      mask_of(std::string_view unit, std::string_view name, std::string_view field) const
      -> std::uint32_t
  {
    auto const& fields = reg_of(unit, name).fields;
    auto const  found  = std::ranges::find(fields, field, &popcorn::field_desc::name);
    REQUIRE(found != fields.end());
    return 1u << found->lsb;
  }

  [[nodiscard]] auto
      mask(std::string_view name, std::string_view field, std::uint32_t value = 1) const
      -> std::uint32_t
  {
    auto const& fields = reg_of("USB", name).fields;
    auto const  found  = std::ranges::find(fields, field, &popcorn::field_desc::name);
    REQUIRE(found != fields.end());
    return value << found->lsb;
  }

  [[nodiscard]] auto read(std::string_view name) const -> std::uint32_t
  {
    auto&      regs  = f->target->peripheral("USB");
    auto const value = f->target->model("USB")->on_read(
        regs, reg_of("USB", name).offset, 4, {.pc = 0, .now = icount{0}});
    REQUIRE(value.has_value());
    return *value;
  }

  void write(std::string_view name, std::uint32_t value) const
  {
    auto&      regs   = f->target->peripheral("USB");
    auto const offset = reg_of("USB", name).offset;
    REQUIRE(f->target->model("USB")
                ->on_write(regs, offset, 4, regs.peek(offset), value, {.pc = 0, .now = icount{0}})
                .has_value());
  }

  [[nodiscard]] auto get(std::string_view name, std::string_view field) const -> std::uint32_t
  {
    return f->field("USB", name, field);
  }

  [[nodiscard]] auto sram_read(
      std::uint32_t      offset,
      std::uint8_t       size = 4,
      popcorn::initiator by   = popcorn::initiator::cpu) const -> std::uint32_t
  {
    auto&      regs  = f->target->peripheral("USB_SRAM");
    auto const value = f->target->model("USB_SRAM")
                           ->on_read(regs, offset, size, {.pc = 0, .now = icount{0}, .by = by});
    REQUIRE(value.has_value());
    return *value;
  }

  void sram_write(
      std::uint32_t      offset,
      std::uint32_t      value,
      std::uint8_t       size = 2,
      popcorn::initiator by   = popcorn::initiator::cpu) const
  {
    auto& regs = f->target->peripheral("USB_SRAM");
    REQUIRE(f->target->model("USB_SRAM")
                ->on_write(regs, offset, size, 0, value, {.pc = 0, .now = icount{0}, .by = by})
                .has_value());
  }

  [[nodiscard]] auto local16(std::uint32_t local) const -> std::uint32_t
  {
    return sram_read(local / 2 * 4, 2);
  }

  void set_local16(std::uint32_t local, std::uint32_t value) const
  {
    sram_write(local / 2 * 4, value);
  }

  [[nodiscard]] auto btable() const -> std::uint32_t
  {
    return read("BTABLE") & 0xFFF8u;
  }

  void tx_buffer(std::size_t n, std::uint32_t address, std::uint32_t count, std::uint32_t which = 0)
      const
  {
    auto const entry = btable() + static_cast<std::uint32_t>(8 * n + 4 * which);
    set_local16(entry, address);
    set_local16(entry + 2, count);
  }

  void rx_buffer(std::size_t n, std::uint32_t address, std::uint32_t bytes, std::uint32_t which = 1)
      const
  {
    auto const entry  = btable() + static_cast<std::uint32_t>(8 * n + 4 * which);
    auto const large  = bytes > 62;
    auto const blocks = large ? bytes / 32 - 1 : bytes / 2;
    set_local16(entry, address);
    set_local16(entry + 2, (large ? 1u << 15 : 0u) | (blocks << 10));
  }

  [[nodiscard]] auto rx_count(std::size_t n, std::uint32_t which = 1) const -> std::uint32_t
  {
    return local16(btable() + static_cast<std::uint32_t>(8 * n + 4 * which) + 2) & 0x3FFu;
  }

  void put_bytes(std::uint32_t local, std::span<std::uint8_t const> bytes) const
  {
    for(std::size_t index = 0; index < bytes.size(); index += 2)
    {
      std::uint32_t half = bytes[index];
      if(index + 1 < bytes.size())
      {
        half |= std::uint32_t{bytes[index + 1]} << 8;
      }
      set_local16(local + static_cast<std::uint32_t>(index), half);
    }
  }

  [[nodiscard]] auto bytes_at(std::uint32_t local, std::size_t count) const
      -> std::vector<std::uint8_t>
  {
    std::vector<std::uint8_t> out;
    for(std::size_t index = 0; index < count; ++index)
    {
      auto const half = local16(local + static_cast<std::uint32_t>(index & ~std::size_t{1}));
      out.push_back(static_cast<std::uint8_t>(half >> (8 * (index % 2))));
    }
    return out;
  }

  void endpoint(std::size_t n, endpoint_setup const& want) const
  {
    auto const name    = std::format("EP{}R", n);
    auto const now     = read(name);
    auto const toggled = [&](std::string_view field, std::uint32_t value) {
      auto const width_mask = mask(name, field, field.starts_with("STAT") ? 3u : 1u);
      return (now & width_mask) ^ mask(name, field, value);
    };
    write(
        name, mask(name, "EA", want.address)
                  | mask(name, "EP_TYPE", want.type)
                  | mask(name, "EP_KIND", want.kind)
                  | mask(name, "CTR_RX")
                  | mask(name, "CTR_TX")
                  | toggled("STAT_TX", want.stat_tx)
                  | toggled("STAT_RX", want.stat_rx)
                  | toggled("DTOG_TX", want.dtog_tx)
                  | toggled("DTOG_RX", want.dtog_rx));
  }

  void clear_ep(std::size_t n, std::string_view flag) const
  {
    auto const name = std::format("EP{}R", n);
    auto const word = read(name);
    write(
        name, (word
               & ~(mask(name, "STAT_TX", 3)
                   | mask(name, "STAT_RX", 3)
                   | mask(name, "DTOG_TX")
                   | mask(name, "DTOG_RX")
                   | mask(name, flag)))
                  | (flag == "CTR_RX" ? mask(name, "CTR_TX") : mask(name, "CTR_RX")));
  }

  [[nodiscard]] auto now() const -> vtime
  {
    return f->now();
  }

  void run_for(std::uint64_t ps) const
  {
    f->run_for(ps);
  }

  [[nodiscard]] auto pending(std::uint32_t irq) const -> bool
  {
    return f->pending(irq);
  }

  void unpend(std::uint32_t irq) const
  {
    REQUIRE(f->target->mem().write(0xE000'E280u + 4 * (irq / 32), 4, 1u << (irq % 32)));
  }

  void bus_write(popcorn::address address, std::uint32_t value) const
  {
    REQUIRE(f->target->mem().write(address, 4, value));
  }

  [[nodiscard]] auto bus_read(popcorn::address address) const -> std::uint32_t
  {
    auto const value = f->target->mem().read(address, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void pull_up(bool on) const
  {
    bus_write(at("SYSCFG", "PMC"), on ? 1u : 0u);
  }

  void power_up() const
  {
    write("CNTR", 0);
    write("DADDR", mask("DADDR", "EF"));
  }

  void connect() const
  {
    power_up();
    port().attach(host);
    pull_up(true);
    REQUIRE(port().pulled_up());
  }

  void open_control() const
  {
    tx_buffer(0, 0x40, 0);
    rx_buffer(0, 0x80, 64);
    endpoint(0, {.address = 0, .type = control, .stat_tx = nak, .stat_rx = valid});
  }

  void reset_bus() const
  {
    port().bus_reset(now());
    run_for(10 * ms);
    port().bus_reset_end(now());
  }

  void enumerate() const
  {
    connect();
    reset_bus();
    run_for(20 * us);
    write("ISTR", 0);
    write("DADDR", mask("DADDR", "EF"));
    open_control();
  }
};

auto sequence(std::size_t count, std::uint8_t first) -> std::vector<std::uint8_t>
{
  std::vector<std::uint8_t> bytes(count);
  for(std::size_t index = 0; index < count; ++index)
  {
    bytes[index] = static_cast<std::uint8_t>(first + index);
  }
  return bytes;
}
}

TEST_CASE("one USB model per L1 part, the USB_SRAM window served by it, on Cat.5 and Cat.1")
{
  for(auto const* part : {&l152re(), &l152rb()})
  {
    CAPTURE(part->part);
    rig const   r{true, *part};
    std::size_t controllers = 0;
    for(auto const& peripheral : part->peripherals)
    {
      if(r.f->model_of<usb::device_port>(peripheral.name) != nullptr)
      {
        ++controllers;
      }
    }
    CHECK(controllers == 1);
    REQUIRE(r.f->target->model("USB_SRAM") != nullptr);
    CHECK(r.f->target->model("USB_SRAM") != r.f->target->model("USB"));
    r.sram_write(0x3FC, 0xBEEF);
    CHECK(r.bus_read(r.f->target->peripheral("USB_SRAM").description().base + 0x3FC) == 0xBEEF);
  }
}

TEST_CASE(
    "RM0038 24.5.3: half-word k of the packet memory sits in the low half of the USB_SRAM word at "
    "offset 4k, the upper half reads 0, board check 152 inferred")
{
  rig const r;
  r.sram_write(8, 0xAAAA'1234u, 4);
  CHECK(r.sram_read(8) == 0x1234u);
  CHECK(r.sram_read(8, 2) == 0x1234u);
  r.sram_write(0xC, 0x5678);
  CHECK(r.sram_read(0xC) == 0x5678u);
  r.set_local16(0x1FE, 0xCAFE);
  CHECK(r.sram_read(0x3FC) == 0xCAFEu);
  r.write("BTABLE", 0x40);
  r.rx_buffer(0, 0x80, 8);
  r.tx_buffer(0, 4, 4);
  r.endpoint(0, {.address = 0, .type = control, .stat_tx = valid, .stat_rx = nak});
  r.connect();
  r.write("DADDR", r.mask("DADDR", "EF"));
  auto const sent = r.port().in(0, 0, r.now());
  REQUIRE(sent.hs == usb::handshake::ack);
  CHECK(sent.data == std::vector<std::uint8_t>{0x34, 0x12, 0x78, 0x56});
}

TEST_CASE(
    "board check 160 inferred: a CPU byte access or an upper half-word access of the packet memory "
    "logs once and raises model_error under strict, a debugger access passes")
{
  rig const r;
  auto&     regs  = r.f->target->peripheral("USB_SRAM");
  auto*     model = r.f->target->model("USB_SRAM");
  r.sram_write(0, 0x1122);
  CHECK_THROWS_AS(
      static_cast<void>(model->on_read(regs, 1, 1, {.pc = 0, .now = icount{0}})),
      popcorn::model_error);
  CHECK_THROWS_AS(
      static_cast<void>(model->on_write(regs, 2, 2, 0, 0x3344, {.pc = 0, .now = icount{0}})),
      popcorn::model_error);
  CHECK(r.sram_read(1, 1, popcorn::initiator::debugger) == 0x11);
  r.sram_write(0, 0x55, 1, popcorn::initiator::debugger);
  CHECK(r.sram_read(0) == 0x55u);

  rig const lax{false};
  lax.sram_write(0, 0x1122);
  lax.sram_write(2, 0x4455, 2);
  CHECK(lax.sram_read(0) == 0x1122u);
  lax.sram_write(1, 0x33, 1);
  CHECK(lax.sram_read(0) == 0x3300u);
  CHECK(lax.warnings() == 1);
}

TEST_CASE("the L1 packet memory reads 0 after a power-on, a software reset and USBRST keep it")
{
  rig const  r;
  auto const apb1rstr = r.at("RCC", "APB1RSTR");
  auto const usbrst   = r.mask_of("RCC", "APB1RSTR", "USBRST");
  r.sram_write(0x10, 0x4321);
  r.f->target->reset(reset_kind::software);
  CHECK(r.sram_read(0x10) == 0x4321u);
  r.bus_write(apb1rstr, usbrst);
  r.bus_write(apb1rstr, 0);
  CHECK(r.sram_read(0x10) == 0x4321u);
  CHECK(r.read("CNTR") == 3);
  r.f->target->reset(reset_kind::power_on);
  CHECK(r.sram_read(0x10) == 0u);
}

TEST_CASE(
    "RM0038 24.5.3: BTABLE moves the descriptor table, an EP0 IN sends COUNT0_TX bytes from "
    "ADDR0_TX with bit 0 ignored, board check 153 inferred")
{
  rig const r;
  r.tx_buffer(0, 0x100, 2);
  r.write("BTABLE", 0x40);
  CHECK(r.read("BTABLE") == 0x40u);
  r.write("BTABLE", 0x47);
  CHECK(r.read("BTABLE") == 0x40u);
  r.tx_buffer(0, 0x81, 4);
  r.rx_buffer(0, 0xC0, 8);
  auto const bytes = sequence(4, 0xA0);
  r.put_bytes(0x80, bytes);
  r.put_bytes(0x100, sequence(2, 0x10));
  r.endpoint(0, {.address = 0, .type = control, .stat_tx = valid, .stat_rx = nak});
  r.connect();
  r.write("DADDR", r.mask("DADDR", "EF"));
  auto const sent = r.port().in(0, 0, r.now());
  REQUIRE(sent.hs == usb::handshake::ack);
  CHECK(sent.pid == usb::data_pid::data0);
  CHECK(sent.data == bytes);
  CHECK(r.get("EP0R", "CTR_TX") == 1);
  CHECK(r.get("EP0R", "STAT_TX") == nak);
}

TEST_CASE(
    "RM0038 24.5.2: a SETUP lands in the EP0 receive buffer, COUNT0_RX 8 at BTABLE + 6, CTR_RX "
    "and SETUP set, USB_LP pends with CTRM, USB_HP stays low")
{
  rig const r;
  r.enumerate();
  r.write("CNTR", r.mask("CNTR", "CTRM"));
  CHECK(r.port().setup(0, get_device, r.now()) == usb::handshake::ack);
  CHECK(r.rx_count(0) == 8);
  auto const expected = get_device.bytes();
  CHECK(std::ranges::equal(r.bytes_at(0x80, 8), expected));
  CHECK(r.get("EP0R", "CTR_RX") == 1);
  CHECK(r.get("EP0R", "SETUP") == 1);
  CHECK(r.get("EP0R", "STAT_RX") == nak);
  CHECK(r.get("ISTR", "CTR") == 1);
  CHECK(r.get("ISTR", "DIR") == 1);
  CHECK(r.get("ISTR", "EP_ID") == 0);
  CHECK(r.pending(lp_irq));
  CHECK_FALSE(r.pending(hp_irq));
  r.clear_ep(0, "CTR_RX");
  CHECK(r.get("ISTR", "CTR") == 0);
  CHECK(r.get("EP0R", "STAT_RX") == nak);
}

TEST_CASE(
    "RM0038 24.3.1, board check 154 inferred: a double-buffered bulk OUT pends USB_HP whatever "
    "CTRM, USB_LP only with CTRM")
{
  for(auto const ctrm : {false, true})
  {
    CAPTURE(ctrm);
    rig const r;
    r.enumerate();
    r.write("CNTR", ctrm ? r.mask("CNTR", "CTRM") : 0u);
    r.rx_buffer(1, 0x100, 64, 0);
    r.rx_buffer(1, 0x140, 64, 1);
    r.endpoint(
        1, {.address = 1, .type = bulk, .stat_rx = valid, .kind = 1, .dtog_tx = 1, .dtog_rx = 0});
    auto const data = sequence(6, 0x30);
    CHECK(r.port().out(0, 1, data, usb::data_pid::data0, r.now()) == usb::handshake::ack);
    CHECK(r.bytes_at(0x100, 6) == data);
    CHECK(r.get("EP1R", "CTR_RX") == 1);
    CHECK(r.get("ISTR", "EP_ID") == 1);
    CHECK(r.pending(hp_irq));
    CHECK(r.pending(lp_irq) == ctrm);
    r.clear_ep(1, "CTR_RX");
    r.unpend(hp_irq);
    r.run_for(us);
    CHECK_FALSE(r.pending(hp_irq));
  }
}

TEST_CASE(
    "RM0038 24.4.2: SYSCFG_PMC.USB_PU with a host attached tells the host once per change, never "
    "without a host, and SYSCFGRST with USB_PU set detaches the device")
{
  rig const r;
  r.power_up();
  r.pull_up(true);
  CHECK_FALSE(r.port().pulled_up());
  r.port().attach(r.host);
  CHECK(r.port().pulled_up());
  r.pull_up(true);
  REQUIRE(r.host->changes.size() == 1);
  CHECK(r.host->changes[0].first);
  r.pull_up(false);
  REQUIRE(r.host->changes.size() == 2);
  CHECK_FALSE(r.host->changes[1].first);
  r.pull_up(true);
  REQUIRE(r.host->changes.size() == 3);
  auto const apb2rstr  = r.at("RCC", "APB2RSTR");
  auto const syscfgrst = r.mask_of("RCC", "APB2RSTR", "SYSCFGRST");
  r.bus_write(apb2rstr, syscfgrst);
  r.bus_write(apb2rstr, 0);
  CHECK_FALSE(r.port().pulled_up());
  REQUIRE(r.host->changes.size() == 4);
  CHECK_FALSE(r.host->changes[3].first);
  CHECK(r.get("FNR", "RXDP") == 0);
}

TEST_CASE("a software reset with USB_PU set and a host attached tells the host one detach")
{
  rig const r;
  r.power_up();
  r.port().attach(r.host);
  r.pull_up(true);
  REQUIRE(r.port().pulled_up());
  REQUIRE(r.host->changes.size() == 1);
  r.f->target->reset(reset_kind::software);
  CHECK_FALSE(r.port().pulled_up());
  REQUIRE(r.host->changes.size() == 2);
  CHECK_FALSE(r.host->changes[1].first);
  CHECK(r.get("FNR", "RXDP") == 0);
}

TEST_CASE(
    "board check 155 inferred: the L1 suspend timer runs on USB_PU, ESOF 1 ms after USB_PU is set, "
    "none while it is 0")
{
  rig const r;
  r.power_up();
  r.run_for(3 * ms);
  CHECK(r.get("ISTR", "ESOF") == 0);
  r.pull_up(true);
  r.run_for(ms - us);
  CHECK(r.get("ISTR", "ESOF") == 0);
  r.run_for(2 * us);
  CHECK(r.get("ISTR", "ESOF") == 1);
  CHECK(r.get("FNR", "LSOF") == 1);
}

TEST_CASE(
    "RM0038 24.5.1 and 24.5.2: a bus reset clears EPnR but CTR_RX and CTR_TX and DADDR at the SE0 "
    "start, board check 156 inferred, sets RESET at its end, no answer until RESET is cleared")
{
  rig const r;
  r.enumerate();
  r.write("DADDR", r.mask("DADDR", "EF") | r.mask("DADDR", "ADD", 5));
  REQUIRE(r.port().setup(5, get_device, r.now()) == usb::handshake::ack);
  r.port().bus_reset(r.now());
  CHECK(r.read("EP0R") == r.mask("EP0R", "CTR_RX"));
  CHECK(r.read("DADDR") == 0);
  CHECK(r.get("ISTR", "RESET") == 0);
  r.run_for(10 * ms);
  r.port().bus_reset_end(r.now());
  CHECK(r.get("ISTR", "RESET") == 1);
  r.write("DADDR", r.mask("DADDR", "EF"));
  r.clear_ep(0, "CTR_RX");
  r.open_control();
  CHECK(r.port().setup(0, get_device, r.now()) == usb::handshake::none);
  r.write("ISTR", ~r.mask("ISTR", "RESET"));
  CHECK(r.port().setup(0, get_device, r.now()) == usb::handshake::ack);
}

TEST_CASE(
    "RM0038 24.5.1 CNTR.FRES, board check 157 inferred: FRES written from 0 to 1 clears EPnR but "
    "CTR_RX and CTR_TX and DADDR and sets RESET")
{
  rig const r;
  r.enumerate();
  r.write("DADDR", r.mask("DADDR", "EF") | r.mask("DADDR", "ADD", 3));
  REQUIRE(r.port().setup(3, get_device, r.now()) == usb::handshake::ack);
  r.write("ISTR", 0);
  r.write("CNTR", r.mask("CNTR", "FRES"));
  CHECK(r.read("EP0R") == r.mask("EP0R", "CTR_RX"));
  CHECK(r.read("DADDR") == 0);
  CHECK(r.get("ISTR", "RESET") == 1);
  r.write("CNTR", 0);
  r.write("ISTR", 0);
  r.write("CNTR", r.mask("CNTR", "FRES"));
  CHECK(r.get("ISTR", "RESET") == 1);
}

TEST_CASE(
    "RM0038 24.5.1: FSUSP and LPMODE are the firmware's, a written 0 clears FSUSP, the suspend "
    "check stops while FSUSP is set")
{
  rig const r;
  r.connect();
  r.write("CNTR", r.mask("CNTR", "FSUSP") | r.mask("CNTR", "LPMODE"));
  CHECK(r.get("CNTR", "FSUSP") == 1);
  CHECK(r.get("CNTR", "LPMODE") == 1);
  r.write("ISTR", 0);
  r.run_for(5 * ms);
  CHECK(r.get("ISTR", "ESOF") == 0);
  CHECK(r.get("ISTR", "SUSP") == 0);
  r.write("CNTR", r.mask("CNTR", "LPMODE"));
  CHECK(r.get("CNTR", "FSUSP") == 0);
  CHECK(r.get("CNTR", "LPMODE") == 1);
  r.run_for(ms + us);
  CHECK(r.get("ISTR", "ESOF") == 1);
  r.write("CNTR", 0);
  CHECK(r.get("CNTR", "LPMODE") == 0);
}

TEST_CASE(
    "RM0038 24.4.5 and Table 51, board check 158 inferred: a SOF while FSUSP is set raises WKUP, "
    "clears LPMODE, keeps FSUSP and pends USB_FS_WKUP through EXTI 18 with IMR18 1 only")
{
  for(auto const unmasked : {false, true})
  {
    CAPTURE(unmasked);
    rig const r;
    r.enumerate();
    r.bus_write(exti_base + 0x08, 1u << 18);
    if(unmasked)
    {
      r.bus_write(exti_base + 0x00, 1u << 18);
    }
    for(int round = 0; round < 2; ++round)
    {
      CAPTURE(round);
      r.write("CNTR", r.mask("CNTR", "FSUSP") | r.mask("CNTR", "LPMODE"));
      CHECK(r.get("ISTR", "WKUP") == 0);
      r.port().start_of_frame(static_cast<std::uint16_t>(7 + round), r.now());
      CHECK(r.get("ISTR", "WKUP") == 1);
      CHECK(r.get("CNTR", "FSUSP") == 1);
      CHECK(r.get("CNTR", "LPMODE") == 0);
      CHECK(r.pending(wakeup_irq) == unmasked);
      CHECK((r.bus_read(exti_base + 0x14) & (1u << 18)) == (unmasked ? 1u << 18 : 0u));
      r.bus_write(exti_base + 0x14, 1u << 18);
      r.unpend(wakeup_irq);
      r.write("CNTR", 0);
      r.write("ISTR", ~r.mask("ISTR", "WKUP"));
    }
  }
}

TEST_CASE("RM0038 24.5.1: CNTR.RESUME logs once without strict and raises model_error under strict")
{
  {
    rig const r{false};
    r.power_up();
    r.write("CNTR", r.mask("CNTR", "RESUME"));
    r.write("CNTR", 0);
    r.write("CNTR", r.mask("CNTR", "RESUME"));
    CHECK(r.warnings() == 1);
  }
  rig const r;
  r.power_up();
  auto& regs = r.f->target->peripheral("USB");
  CHECK_THROWS_AS(
      static_cast<void>(r.f->target->model("USB")->on_write(
          regs, r.reg_of("USB", "CNTR").offset, 4, 0, r.mask("CNTR", "RESUME"),
          {.pc = 0, .now = icount{0}})),
      popcorn::model_error);
}

TEST_CASE(
    "checklist 39: the L1 USB variant refuses a description without BTABLE, or with a "
    "USB_SRAM window of 512 bytes")
{
  stm32::register_builtin_models();
  auto const build = [](part_description desc) {
    return std::make_unique<core::machine>(
        std::move(desc), popcorn::arch::armv8m::factory(), popcorn::testing::stm32_options());
  };
  auto  without  = l152re();
  auto& usb_unit = *std::ranges::find(
      without.peripherals, std::string_view{"USB"}, &popcorn::peripheral_desc::name);
  std::erase_if(
      usb_unit.registers, [](popcorn::register_desc const& r) { return r.name == "BTABLE"; });
  CHECK_THROWS_AS(static_cast<void>(build(without)), popcorn::config_error);
  auto small = l152re();
  std::ranges::find(
      small.peripherals, std::string_view{"USB_SRAM"}, &popcorn::peripheral_desc::name)
      ->size = 0x200;
  CHECK_THROWS_AS(static_cast<void>(build(small)), popcorn::config_error);
  CHECK_NOTHROW(static_cast<void>(build(l152re())));
}

TEST_CASE("an L1 USB description without EP3R throws a config_error that names EP3R")
{
  stm32::register_builtin_models();
  auto  without  = l152re();
  auto& usb_unit = *std::ranges::find(
      without.peripherals, std::string_view{"USB"}, &popcorn::peripheral_desc::name);
  std::erase_if(
      usb_unit.registers, [](popcorn::register_desc const& r) { return r.name == "EP3R"; });
  try
  {
    static_cast<void>(std::make_unique<core::machine>(
        std::move(without), popcorn::arch::armv8m::factory(), popcorn::testing::stm32_options()));
    FAIL("no config_error");
  }
  catch(popcorn::config_error const& error)
  {
    CHECK(std::string_view{error.what()}.contains("EP3R"));
  }
}

TEST_CASE("an L1 snapshot after a SETUP restored into a fresh machine gives the same IN")
{
  rig const r;
  r.enumerate();
  REQUIRE(r.port().setup(0, get_device, r.now()) == usb::handshake::ack);
  r.put_bytes(0x40, sequence(18, 1));
  r.tx_buffer(0, 0x40, 18);
  r.clear_ep(0, "CTR_RX");
  r.endpoint(0, {.address = 0, .type = control, .stat_tx = valid, .stat_rx = nak, .dtog_tx = 1});
  auto const saved = r.f->target->snapshot();

  rig const other;
  other.port().attach(other.host);
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.port().pulled_up());
  CHECK(other.rx_count(0) == 8);
  auto const restored = other.port().in(0, 0, other.now());
  auto const direct   = r.port().in(0, 0, r.now());
  CHECK(restored.hs == usb::handshake::ack);
  CHECK(restored.data == sequence(18, 1));
  CHECK(restored.data == direct.data);
  CHECK(restored.pid == direct.pid);
}

TEST_CASE("an L1 board host plugged at power-on sees the pull-up the firmware sets in PMC")
{
  stm32::board board;
  board.usb_hosts.emplace("USB", stm32::usb_host_options{.attached = true});
  rig const r{board};
  auto*     cable = r.f->model_of<stm32::usb_host_port>("USB");
  REQUIRE(cable != nullptr);
  CHECK(cable->plugged());
  r.power_up();
  CHECK_FALSE(r.port().pulled_up());
  r.pull_up(true);
  CHECK(r.port().pulled_up());
}

TEST_CASE(
    "RM0038 10.2.4 and 24.5.1, board check 158 inferred: clearing ISTR.WKUP is a falling edge "
    "on EXTI 18, a reset with WKUP set too")
{
  for(auto const reset : {false, true})
  {
    CAPTURE(reset);
    rig const r;
    r.enumerate();
    r.bus_write(exti_base + 0x00, 1u << 18);
    r.bus_write(exti_base + 0x0C, 1u << 18);
    r.write("CNTR", r.mask("CNTR", "FSUSP"));
    r.port().start_of_frame(9, r.now());
    REQUIRE(r.get("ISTR", "WKUP") == 1);
    CHECK((r.bus_read(exti_base + 0x14) & (1u << 18)) == 0);
    if(reset)
    {
      auto const apb1rstr = r.at("RCC", "APB1RSTR");
      r.bus_write(apb1rstr, r.mask_of("RCC", "APB1RSTR", "USBRST"));
      r.bus_write(apb1rstr, 0);
    }
    else
    {
      r.write("ISTR", ~r.mask("ISTR", "WKUP"));
    }
    CHECK(r.get("ISTR", "WKUP") == 0);
    CHECK((r.bus_read(exti_base + 0x14) & (1u << 18)) == 1u << 18);
    CHECK(r.pending(wakeup_irq));
  }
}

namespace
{
struct scripted_firmware
{
  std::vector<usb::setup_packet> requests;
  std::optional<std::uint8_t>    address;
  std::vector<std::uint8_t>      pending;
  std::size_t                    sent{};
};

auto firmware_device() -> std::vector<std::uint8_t>
{
  return {18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x83, 0x04, 0x40, 0x57, 0x00, 0x01, 1, 2, 0, 1};
}

auto firmware_configuration() -> std::vector<std::uint8_t>
{
  return {9, 2, 32, 0, 1,    1, 0,  0x80, 50, 9, 4, 0,    0, 2,  0xff, 0,
          0, 0, 7,  5, 0x01, 2, 64, 0,    0,  7, 5, 0x81, 2, 64, 0,    0};
}

auto firmware_reply(usb::setup_packet const& setup) -> std::optional<std::vector<std::uint8_t>>
{
  if(setup.request_type != 0x80 or setup.request != 6)
  {
    return std::nullopt;
  }
  std::vector<std::uint8_t> reply;
  switch(setup.value)
  {
    case 0x0100:
      reply = firmware_device();
      break;
    case 0x0200:
      reply = firmware_configuration();
      break;
    case 0x0300:
      reply = {4, 3, 0x09, 0x04};
      break;
    case 0x0301:
      reply = {6, 3, 'l', 0, '1', 0};
      break;
    case 0x0302:
      reply = {6, 3, 'f', 0, 's', 0};
      break;
    default:
      return std::nullopt;
  }
  reply.resize(std::min<std::size_t>(reply.size(), setup.length));
  return reply;
}

void send_packet(rig const& r, scripted_firmware& fw)
{
  auto const size = std::min<std::size_t>(64, fw.pending.size() - fw.sent);
  r.put_bytes(0x40, std::span{fw.pending}.subspan(fw.sent, size));
  r.tx_buffer(0, 0x40, static_cast<std::uint32_t>(size));
  fw.sent += size;
  auto const word = r.read("EP0R");
  r.write(
      "EP0R", (word
               & ~(r.mask("EP0R", "STAT_RX", 3)
                   | r.mask("EP0R", "STAT_TX", 3)
                   | r.mask("EP0R", "DTOG_TX")
                   | r.mask("EP0R", "DTOG_RX")))
                  | r.mask("EP0R", "STAT_TX", valid ^ r.get("EP0R", "STAT_TX"))
                  | r.mask("EP0R", "CTR_RX")
                  | r.mask("EP0R", "CTR_TX"));
}

void arm_rx(rig const& r)
{
  auto const word = r.read("EP0R");
  r.write(
      "EP0R", (word
               & ~(r.mask("EP0R", "STAT_TX", 3)
                   | r.mask("EP0R", "STAT_RX", 3)
                   | r.mask("EP0R", "DTOG_TX")
                   | r.mask("EP0R", "DTOG_RX")))
                  | r.mask("EP0R", "STAT_RX", valid ^ r.get("EP0R", "STAT_RX"))
                  | r.mask("EP0R", "CTR_RX")
                  | r.mask("EP0R", "CTR_TX"));
}

void service(rig const& r, scripted_firmware& fw)
{
  if(r.get("ISTR", "RESET") != 0)
  {
    r.write("ISTR", ~r.mask("ISTR", "RESET"));
    r.write("DADDR", r.mask("DADDR", "EF"));
    r.open_control();
  }
  while(r.get("ISTR", "CTR") != 0)
  {
    auto const n    = r.get("ISTR", "EP_ID");
    auto const name = std::format("EP{}R", n);
    if(r.get(name, "CTR_RX") != 0)
    {
      auto const setup = r.get(name, "SETUP") != 0;
      auto const count = r.rx_count(n);
      auto const bytes = r.bytes_at(0x80, count);
      r.clear_ep(n, "CTR_RX");
      if(setup)
      {
        usb::setup_packet const packet{
            .request_type = bytes[0],
            .request      = bytes[1],
            .value        = static_cast<std::uint16_t>(bytes[2] | (bytes[3] << 8)),
            .index        = static_cast<std::uint16_t>(bytes[4] | (bytes[5] << 8)),
            .length       = static_cast<std::uint16_t>(bytes[6] | (bytes[7] << 8))};
        fw.requests.push_back(packet);
        fw.pending.clear();
        fw.sent = 0;
        if(auto const reply = firmware_reply(packet))
        {
          fw.pending = *reply;
          send_packet(r, fw);
          arm_rx(r);
        }
        else
        {
          if(packet.request_type == 0 and packet.request == 5)
          {
            fw.address = static_cast<std::uint8_t>(packet.value);
          }
          send_packet(r, fw);
        }
      }
      else
      {
        arm_rx(r);
      }
    }
    if(r.get(name, "CTR_TX") != 0)
    {
      r.clear_ep(n, "CTR_TX");
      if(n == 0 and fw.address)
      {
        r.write("DADDR", r.mask("DADDR", "EF") | r.mask("DADDR", "ADD", *fw.address));
        fw.address.reset();
      }
      else if(n == 0 and fw.sent < fw.pending.size())
      {
        send_packet(r, fw);
      }
    }
  }
}
}

TEST_CASE("the virtual host enumerates the L1 USB model run by a register-level firmware, strict")
{
  rig const                r;
  std::vector<std::string> warnings;
  usb::host_options        options;
  options.warn           = [&](std::string const& message) { warnings.push_back(message); };
  auto const        host = std::make_shared<usb::host>(r.port(), std::move(options));
  scripted_firmware fw;
  r.power_up();
  r.port().attach(host);
  r.pull_up(true);
  CHECK(host->state() == usb::device_state::powered);
  for(std::size_t slice = 0; slice < 20'000 and host->state() != usb::device_state::configured;
      ++slice)
  {
    r.run_for(25 * us);
    service(r, fw);
  }
  REQUIRE(host->state() == usb::device_state::configured);
  CHECK(warnings.empty());
  CHECK(host->address() == 1);
  CHECK(r.get("DADDR", "ADD") == 1);
  CHECK(std::ranges::equal(host->device_bytes(), firmware_device()));
  CHECK(std::ranges::equal(host->configuration_bytes(), firmware_configuration()));
  CHECK(host->strings().size() == 3);
  REQUIRE_FALSE(fw.requests.empty());
  CHECK(fw.requests.front().value == 0x0100);
  CHECK(fw.requests.back().request == 9);
  CHECK(r.warnings() == 0);
}
