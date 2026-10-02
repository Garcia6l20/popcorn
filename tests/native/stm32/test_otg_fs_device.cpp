#include "stm32_fixture.hpp"

#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/usb.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core = popcorn::core;
namespace usb  = popcorn::periph::usb;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;
using popcorn::testing::u575;

constexpr std::uint64_t us      = 1'000'000;
constexpr std::uint32_t otg_irq = 73;

constexpr std::uint32_t pktsts_global_out_nak = 1;
constexpr std::uint32_t pktsts_out_data       = 2;
constexpr std::uint32_t pktsts_out_done       = 3;
constexpr std::uint32_t pktsts_setup_done     = 4;
constexpr std::uint32_t pktsts_setup_data     = 6;

constexpr usb::setup_packet get_device_descriptor{
    .request_type = 0x80,
    .request      = 6,
    .value        = 0x0100,
    .index        = 0,
    .length       = 64};
constexpr usb::setup_packet
    set_address{.request_type = 0x00, .request = 5, .value = 1, .index = 0, .length = 0};

struct recording_host final : usb::host_side
{
  std::vector<std::pair<bool, std::uint64_t>> changes;
  std::vector<std::uint64_t>                  wakes;
  std::vector<std::byte>                      memory;
  std::size_t                                 loads{};

  void pull_up_changed(bool on, vtime at) override
  {
    changes.emplace_back(on, at.ps);
  }

  void on_time(vtime at) override
  {
    wakes.push_back(at.ps);
  }

  void save(std::vector<std::byte>& out) const override
  {
    out.insert(out.end(), memory.begin(), memory.end());
  }

  void load(std::span<std::byte const> in) override
  {
    memory.assign(in.begin(), in.end());
    ++loads;
  }
};

struct status
{
  std::uint32_t endpoint{};
  std::uint32_t bytes{};
  std::uint32_t dpid{};
  std::uint32_t pktsts{};
};

struct rig
{
  std::unique_ptr<stm32_fixture>  f    = std::make_unique<stm32_fixture>(u575(), "rcc_ll");
  std::shared_ptr<recording_host> host = std::make_shared<recording_host>();

  rig()
  {
    f->set("pc", sym("rcc_ll", "idle"));
  }

  [[nodiscard]] auto port() const -> usb::device_port&
  {
    auto* const found = f->model_of<usb::device_port>("OTG_FS");
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto reg(std::string_view name) const -> popcorn::register_desc const&
  {
    auto const& registers = f->target->peripheral("OTG_FS").description().registers;
    auto const  found     = std::ranges::find(registers, name, &popcorn::register_desc::name);
    REQUIRE(found != registers.end());
    return *found;
  }

  [[nodiscard]] auto bit(std::string_view name, std::string_view field) const
      -> popcorn::field_desc const&
  {
    auto const& fields = reg(name).fields;
    auto const  found  = std::ranges::find(fields, field, &popcorn::field_desc::name);
    REQUIRE(found != fields.end());
    return *found;
  }

  [[nodiscard]] auto
      mask(std::string_view name, std::string_view field, std::uint32_t value = 1) const
      -> std::uint32_t
  {
    return value << bit(name, field).lsb;
  }

  [[nodiscard]] auto read(std::string_view name) const -> std::uint32_t
  {
    auto&      regs = f->target->peripheral("OTG_FS");
    auto const value =
        f->target->model("OTG_FS")->on_read(regs, reg(name).offset, 4, {.pc = 0, .now = icount{0}});
    REQUIRE(value.has_value());
    return *value;
  }

  void write(std::string_view name, std::uint32_t value) const
  {
    auto&      regs   = f->target->peripheral("OTG_FS");
    auto const offset = reg(name).offset;
    REQUIRE(f->target->model("OTG_FS")
                ->on_write(regs, offset, 4, regs.peek(offset), value, {.pc = 0, .now = icount{0}})
                .has_value());
  }

  void set(std::string_view name, std::string_view field, std::uint32_t value) const
  {
    auto const& found = bit(name, field);
    auto const  bits  = ((std::uint64_t{1} << found.width) - 1u) << found.lsb;
    auto const  now   = (read(name) & ~bits) | ((std::uint64_t{value} << found.lsb) & bits);
    write(name, static_cast<std::uint32_t>(now));
  }

  [[nodiscard]] auto get(std::string_view name, std::string_view field) const -> std::uint32_t
  {
    return f->field("OTG_FS", name, field);
  }

  [[nodiscard]] auto now() const -> vtime
  {
    return f->target->virtual_clock().time();
  }

  void run_for(std::uint64_t ps) const
  {
    REQUIRE(
        f->run_to_stop(
             {.max_instructions = icount{1'000'000'000}, .until_time = vtime{now().ps + ps}})
            .reason
        == stop_reason::time);
  }

  [[nodiscard]] auto pending() const -> bool
  {
    auto const word = f->target->mem().read(0xe000'e200u + 4 * (otg_irq / 32), 4);
    REQUIRE(word.has_value());
    return ((*word >> (otg_irq % 32)) & 1u) != 0;
  }

  void unpend() const
  {
    REQUIRE(f->target->mem().write(0xe000'e280u + 4 * (otg_irq / 32), 4, 1u << (otg_irq % 32)));
  }

  void power_up(bool forced_b_valid = true) const
  {
    set("GCCFG", "PWRDWN", 1);
    if(forced_b_valid)
    {
      write("GOTGCTL", read("GOTGCTL") | mask("GOTGCTL", "BVALOEN") | mask("GOTGCTL", "BVALOVAL"));
    }
    else
    {
      set("GCCFG", "VBDEN", 1);
    }
  }

  void connect() const
  {
    power_up();
    port().attach(host);
    set("DCTL", "SDIS", 0);
    REQUIRE(port().pulled_up());
  }

  void enumerate() const
  {
    connect();
    port().bus_reset(now());
    run_for(20 * us);
    write("GINTSTS", ~std::uint32_t{});
    write("DOEPTSIZ0", mask("DOEPTSIZ0", "STUPCNT", 3));
  }

  [[nodiscard]] auto pop() const -> status
  {
    auto const word = read("GRXSTSP_DEVICE");
    return {
        .endpoint = word & 0xfu,
        .bytes    = (word >> 4) & 0x7ffu,
        .dpid     = (word >> 15) & 3u,
        .pktsts   = (word >> 17) & 0xfu};
  }

  [[nodiscard]] auto pop_words(std::size_t count) const -> std::vector<std::uint32_t>
  {
    std::vector<std::uint32_t> words;
    for(std::size_t index = 0; index < count; ++index)
    {
      words.push_back(read("DFIFO0"));
    }
    return words;
  }

  void push(std::string_view fifo, std::span<std::uint8_t const> bytes) const
  {
    for(std::size_t index = 0; index < bytes.size(); index += 4)
    {
      std::uint32_t word = 0;
      for(std::size_t lane = 0; lane < 4 and index + lane < bytes.size(); ++lane)
      {
        word |= std::uint32_t{bytes[index + lane]} << (8 * lane);
      }
      write(fifo, word);
    }
  }

  void start_in0(std::uint32_t bytes) const
  {
    write("DIEPTSIZ0", mask("DIEPTSIZ0", "XFRSIZ", bytes) | mask("DIEPTSIZ0", "PKTCNT", 1));
    write("DIEPCTL0", read("DIEPCTL0") | mask("DIEPCTL0", "EPENA") | mask("DIEPCTL0", "CNAK"));
  }

  void start_out0(std::uint32_t bytes) const
  {
    write(
        "DOEPTSIZ0", mask("DOEPTSIZ0", "XFRSIZ", bytes)
                         | mask("DOEPTSIZ0", "PKTCNT", 1)
                         | mask("DOEPTSIZ0", "STUPCNT", get("DOEPTSIZ0", "STUPCNT")));
    write("DOEPCTL0", read("DOEPCTL0") | mask("DOEPCTL0", "EPENA") | mask("DOEPCTL0", "CNAK"));
  }
};

auto setup_words(usb::setup_packet const& packet) -> std::vector<std::uint32_t>
{
  auto const bytes = packet.bytes();
  return {
      std::uint32_t{bytes[0]}
          | (std::uint32_t{bytes[1]} << 8)
          | (std::uint32_t{bytes[2]} << 16)
          | (std::uint32_t{bytes[3]} << 24),
      std::uint32_t{bytes[4]}
          | (std::uint32_t{bytes[5]} << 8)
          | (std::uint32_t{bytes[6]} << 16)
          | (std::uint32_t{bytes[7]} << 24)};
}
}

TEST_CASE("clearing SDIS with VBUS sensed tells the host once")
{
  rig const r;
  r.power_up(false);
  r.port().attach(r.host);
  CHECK(r.get("GOTGCTL", "BSVLD") == 1);
  CHECK(r.host->changes.empty());
  r.set("DCTL", "SDIS", 0);
  REQUIRE(r.host->changes.size() == 1);
  CHECK(r.host->changes[0].first);
  CHECK(r.host->changes[0].second == r.now().ps);
  r.set("DCTL", "SDIS", 0);
  CHECK(r.host->changes.size() == 1);
  r.set("DCTL", "SDIS", 1);
  REQUIRE(r.host->changes.size() == 2);
  CHECK_FALSE(r.host->changes[1].first);
}

TEST_CASE("without VBUS clearing SDIS never attaches")
{
  SUBCASE("no host")
  {
    rig const r;
    r.power_up(false);
    r.set("DCTL", "SDIS", 0);
    CHECK(r.get("GOTGCTL", "BSVLD") == 0);
    CHECK_FALSE(r.port().pulled_up());
    CHECK(r.port().setup(0, get_device_descriptor, r.now()) == usb::handshake::none);
  }
  SUBCASE("a host but no VBUS sensing and no B-valid override")
  {
    rig const r;
    r.set("GCCFG", "PWRDWN", 1);
    r.port().attach(r.host);
    r.set("DCTL", "SDIS", 0);
    CHECK(r.get("GOTGCTL", "BSVLD") == 0);
    CHECK_FALSE(r.port().pulled_up());
    CHECK(r.host->changes.empty());
  }
}

TEST_CASE("B-valid forced with VBDEN clear reads BSVLD and attaches on SDIS clear")
{
  rig const r;
  r.set("GCCFG", "PWRDWN", 1);
  r.set("GCCFG", "VBDEN", 0);
  r.write(
      "GOTGCTL", r.read("GOTGCTL") | r.mask("GOTGCTL", "BVALOEN") | r.mask("GOTGCTL", "BVALOVAL"));
  CHECK(r.get("GOTGCTL", "BSVLD") == 1);
  r.port().attach(r.host);
  CHECK(r.host->changes.empty());
  r.set("DCTL", "SDIS", 0);
  REQUIRE(r.host->changes.size() == 1);
  CHECK(r.host->changes[0].first);
  r.set("GOTGCTL", "BVALOVAL", 0);
  CHECK(r.get("GOTGCTL", "BSVLD") == 0);
  CHECK(r.get("GOTGINT", "SEDET") == 1);
  CHECK(r.get("GINTSTS", "OTGINT") == 1);
  REQUIRE(r.host->changes.size() == 2);
  CHECK_FALSE(r.host->changes[1].first);
}

TEST_CASE("attaching a host to a connected device tells it at once")
{
  rig const r;
  r.power_up();
  r.set("DCTL", "SDIS", 0);
  r.port().attach(r.host);
  REQUIRE(r.host->changes.size() == 1);
  CHECK(r.host->changes[0].first);
}

TEST_CASE("a bus reset raises USBRST, then ENUMDNE at full speed, the IRQ following the masks")
{
  rig const r;
  r.connect();
  r.port().bus_reset(r.now());
  CHECK(r.get("GINTSTS", "USBRST") == 1);
  CHECK(r.get("GINTSTS", "ENUMDNE") == 0);
  CHECK_FALSE(r.pending());
  r.set("GINTMSK", "USBRST", 1);
  CHECK_FALSE(r.pending());
  r.set("GAHBCFG", "GINTMSK", 1);
  CHECK(r.pending());
  r.write("GINTSTS", r.mask("GINTSTS", "USBRST"));
  CHECK(r.get("GINTSTS", "USBRST") == 0);
  r.unpend();
  CHECK_FALSE(r.pending());
  r.set("GINTMSK", "ENUMDNEM", 1);
  r.run_for(20 * us);
  CHECK(r.get("GINTSTS", "ENUMDNE") == 1);
  CHECK(r.get("DSTS", "ENUMSPD") == 3);
  CHECK(r.pending());
}

TEST_CASE("writing all ones to GINTSTS clears its events and sets nothing")
{
  rig const r;
  r.connect();
  r.port().bus_reset(r.now());
  r.run_for(20 * us);
  REQUIRE(r.get("GINTSTS", "ENUMDNE") == 1);
  r.write("GINTSTS", ~std::uint32_t{});
  CHECK(r.get("GINTSTS", "USBRST") == 0);
  CHECK(r.get("GINTSTS", "ENUMDNE") == 0);
  CHECK(r.get("GINTSTS", "SOF") == 0);
  CHECK(r.get("GINTSTS", "WKUPINT") == 0);
  CHECK(r.get("GINTSTS", "RXFLVL") == 0);
}

TEST_CASE("a SETUP pops as PKTSTS 6 with its 8 bytes, then 4, which raises STUP")
{
  rig const r;
  r.enumerate();
  REQUIRE(r.get("DOEPTSIZ0", "STUPCNT") == 3);
  CHECK(r.port().setup(0, get_device_descriptor, r.now()) == usb::handshake::ack);
  CHECK(r.get("DOEPTSIZ0", "STUPCNT") == 2);
  CHECK(r.get("DIEPCTL0", "NAKSTS") == 1);
  CHECK(r.get("DIEPINT0", "INEPNE") == 1);
  CHECK(r.get("GINTSTS", "RXFLVL") == 1);
  auto const data = r.pop();
  CHECK(data.pktsts == pktsts_setup_data);
  CHECK(data.endpoint == 0);
  CHECK(data.bytes == 8);
  CHECK(data.dpid == 0);
  CHECK(r.pop_words(2) == setup_words(get_device_descriptor));
  CHECK(r.get("DOEPINT0", "STUP") == 0);
  CHECK(r.get("GINTSTS", "RXFLVL") == 1);
  CHECK(r.pop().pktsts == pktsts_setup_done);
  CHECK(r.get("DOEPINT0", "STUP") == 1);
  CHECK(r.get("GINTSTS", "RXFLVL") == 0);
  r.set("DOEPMSK", "STUPM", 1);
  CHECK(r.get("DAINT", "OEPINT") == 1);
  r.write("DOEPINT0", r.mask("DOEPINT0", "STUP"));
  CHECK(r.get("DOEPINT0", "STUP") == 0);
  CHECK(r.get("DAINT", "OEPINT") == 0);
}

TEST_CASE("GRXSTSR peeks and a debugger read of GRXSTSP pops nothing")
{
  rig const r;
  r.enumerate();
  REQUIRE(r.port().setup(0, get_device_descriptor, r.now()) == usb::handshake::ack);
  auto const peek = r.read("GRXSTSR_DEVICE");
  CHECK(r.read("GRXSTSR_DEVICE") == peek);
  auto const debugger = r.f->target->mem().read(
      r.f->target->peripheral("OTG_FS").description().base + r.reg("GRXSTSP_DEVICE").offset, 4);
  REQUIRE(debugger.has_value());
  CHECK(*debugger == peek);
  CHECK(r.read("GRXSTSP_DEVICE") == peek);
  CHECK(r.read("GRXSTSR_DEVICE") != peek);
}

TEST_CASE("an EP0 IN of 18 bytes NAKs until the FIFO holds them, then completes")
{
  rig const r;
  r.enumerate();
  REQUIRE(r.port().setup(0, get_device_descriptor, r.now()) == usb::handshake::ack);
  r.start_in0(18);
  CHECK(r.port().in(0, 0, r.now()).hs == usb::handshake::nak);
  std::array<std::uint8_t, 18> descriptor{};
  for(std::size_t index = 0; index < descriptor.size(); ++index)
  {
    descriptor[index] = static_cast<std::uint8_t>(0x10 + index);
  }
  r.push("DFIFO0", std::span{descriptor}.first(16));
  CHECK(r.port().in(0, 0, r.now()).hs == usb::handshake::nak);
  r.push("DFIFO0", std::span{descriptor}.subspan(16));
  CHECK(r.get("DIEPINT0", "XFRC") == 0);
  auto const packet = r.port().in(0, 0, r.now());
  CHECK(packet.hs == usb::handshake::ack);
  CHECK(packet.pid == usb::data_pid::data1);
  CHECK(packet.data == std::vector<std::uint8_t>(descriptor.begin(), descriptor.end()));
  CHECK(r.get("DIEPINT0", "XFRC") == 1);
  CHECK(r.get("DIEPCTL0", "EPENA") == 0);
  CHECK(r.get("DIEPTSIZ0", "PKTCNT") == 0);
  CHECK(r.get("DIEPTSIZ0", "XFRSIZ") == 0);
  CHECK(r.port().in(0, 0, r.now()).hs == usb::handshake::nak);
}

TEST_CASE("TXFE follows the FIFO level and reaches DAINT through DIEPEMPMSK")
{
  rig const r;
  r.enumerate();
  r.write("HNPTXFSIZ", r.mask("HNPTXFSIZ", "NPTXFD", 16));
  CHECK(r.get("DTXFSTS0", "INEPTFSAV") == 16);
  CHECK(r.get("DIEPINT0", "TXFE") == 1);
  std::array<std::uint8_t, 40> bytes{};
  r.push("DFIFO0", bytes);
  CHECK(r.get("DTXFSTS0", "INEPTFSAV") == 6);
  CHECK(r.get("DIEPINT0", "TXFE") == 0);
  r.set("GAHBCFG", "TXFELVL", 1);
  r.write("GRSTCTL", r.mask("GRSTCTL", "TXFFLSH"));
  CHECK(r.get("DIEPINT0", "TXFE") == 1);
  CHECK(r.get("DAINT", "IEPINT") == 0);
  r.set("DIEPEMPMSK", "INEPTXFEM", 1);
  CHECK(r.get("DAINT", "IEPINT") == 1);
}

TEST_CASE("an EP0 OUT status stage pops PKTSTS 2 with no data, then 3, which raises XFRC")
{
  rig const r;
  r.enumerate();
  REQUIRE(r.port().setup(0, set_address, r.now()) == usb::handshake::ack);
  REQUIRE(r.pop().pktsts == pktsts_setup_data);
  static_cast<void>(r.pop_words(2));
  REQUIRE(r.pop().pktsts == pktsts_setup_done);
  CHECK(r.port().out(0, 0, {}, usb::data_pid::data1, r.now()) == usb::handshake::nak);
  r.start_out0(0);
  CHECK(r.get("DOEPCTL0", "EPENA") == 1);
  CHECK((r.read("DOEPCTL0") & r.mask("DOEPCTL0", "EPENA")) != 0);
  CHECK(r.port().out(0, 0, {}, usb::data_pid::data1, r.now()) == usb::handshake::ack);
  auto const data = r.pop();
  CHECK(data.pktsts == pktsts_out_data);
  CHECK(data.bytes == 0);
  CHECK(data.dpid == 2);
  CHECK(r.get("DOEPINT0", "XFRC") == 0);
  CHECK(r.pop().pktsts == pktsts_out_done);
  CHECK(r.get("DOEPINT0", "XFRC") == 1);
  CHECK(r.get("DOEPCTL0", "EPENA") == 0);
  CHECK(r.get("GINTSTS", "RXFLVL") == 0);
}

TEST_CASE("an OUT packet with the wrong toggle is acknowledged and dropped")
{
  rig const r;
  r.enumerate();
  REQUIRE(r.port().setup(0, set_address, r.now()) == usb::handshake::ack);
  REQUIRE(r.pop().pktsts == pktsts_setup_data);
  static_cast<void>(r.pop_words(2));
  REQUIRE(r.pop().pktsts == pktsts_setup_done);
  r.start_out0(0);
  CHECK(r.port().out(0, 0, {}, usb::data_pid::data0, r.now()) == usb::handshake::ack);
  CHECK(r.get("GINTSTS", "RXFLVL") == 0);
  CHECK(r.get("DOEPCTL0", "EPENA") == 1);
}

TEST_CASE("an OUT packet the receive FIFO cannot hold is NAKed and nothing is pushed")
{
  rig const r;
  r.enumerate();
  r.write("GRXFSIZ", r.mask("GRXFSIZ", "RXFD", 16));
  r.start_out0(64);
  std::array<std::uint8_t, 64> payload{};
  CHECK(r.port().out(0, 0, payload, usb::data_pid::data0, r.now()) == usb::handshake::nak);
  CHECK(r.get("GINTSTS", "RXFLVL") == 0);
  CHECK(r.get("DOEPTSIZ0", "PKTCNT") == 1);
  r.write("GRXFSIZ", r.mask("GRXFSIZ", "RXFD", 18));
  CHECK(r.port().out(0, 0, payload, usb::data_pid::data0, r.now()) == usb::handshake::ack);
  CHECK(r.get("GINTSTS", "RXFLVL") == 1);
}

TEST_CASE("TXFFLSH with TXFNUM 0x10 empties every TX FIFO, RXFFLSH the receive FIFO")
{
  rig const r;
  r.enumerate();
  std::array<std::uint8_t, 8> bytes{};
  for(auto const* fifo : {"DFIFO0", "DFIFO1", "DFIFO3", "DFIFO5"})
  {
    r.push(fifo, bytes);
  }
  CHECK(r.get("DTXFSTS0", "INEPTFSAV") == r.get("HNPTXFSIZ", "NPTXFD") - 2);
  CHECK(r.get("DTXFSTS3", "INEPTFSAV") == r.get("DIEPTXF3", "INEPTXFD") - 2);
  r.write("GRSTCTL", r.mask("GRSTCTL", "TXFFLSH") | r.mask("GRSTCTL", "TXFNUM", 3));
  CHECK(r.get("DTXFSTS3", "INEPTFSAV") == r.get("DIEPTXF3", "INEPTXFD"));
  CHECK(r.get("DTXFSTS5", "INEPTFSAV") == r.get("DIEPTXF5", "INEPTXFD") - 2);
  r.write("GRSTCTL", r.mask("GRSTCTL", "TXFFLSH") | r.mask("GRSTCTL", "TXFNUM", 0x10));
  CHECK(r.get("GRSTCTL", "TXFFLSH") == 0);
  CHECK(r.get("DTXFSTS0", "INEPTFSAV") == r.get("HNPTXFSIZ", "NPTXFD"));
  CHECK(r.get("DTXFSTS1", "INEPTFSAV") == r.get("DIEPTXF1", "INEPTXFD"));
  CHECK(r.get("DTXFSTS5", "INEPTFSAV") == r.get("DIEPTXF5", "INEPTXFD"));
  REQUIRE(r.port().setup(0, get_device_descriptor, r.now()) == usb::handshake::ack);
  REQUIRE(r.get("GINTSTS", "RXFLVL") == 1);
  r.write("GRSTCTL", r.mask("GRSTCTL", "RXFFLSH"));
  CHECK(r.get("GINTSTS", "RXFLVL") == 0);
}

TEST_CASE("the device takes its new address after the SET_ADDRESS status stage")
{
  rig const r;
  r.enumerate();
  REQUIRE(r.port().setup(0, set_address, r.now()) == usb::handshake::ack);
  r.set("DCFG", "DAD", 1);
  r.start_in0(0);
  CHECK(r.port().in(1, 0, r.now()).hs == usb::handshake::none);
  auto const status_stage = r.port().in(0, 0, r.now());
  CHECK(status_stage.hs == usb::handshake::ack);
  CHECK(status_stage.data.empty());
  CHECK(r.port().setup(0, get_device_descriptor, r.now()) == usb::handshake::none);
  CHECK(r.port().setup(1, get_device_descriptor, r.now()) == usb::handshake::ack);
  r.port().bus_reset(r.now());
  r.set("DCFG", "DAD", 0);
  CHECK(r.port().setup(0, get_device_descriptor, r.now()) == usb::handshake::ack);
}

TEST_CASE("a DAD write outside a SET_ADDRESS takes effect at once")
{
  rig const r;
  r.enumerate();
  REQUIRE(r.port().setup(0, set_address, r.now()) == usb::handshake::ack);
  r.start_in0(0);
  REQUIRE(r.port().in(0, 0, r.now()).hs == usb::handshake::ack);
  r.set("DCFG", "DAD", 2);
  CHECK(r.port().setup(0, get_device_descriptor, r.now()) == usb::handshake::none);
  CHECK(r.port().setup(2, get_device_descriptor, r.now()) == usb::handshake::ack);
  r.set("DCFG", "DAD", 3);
  CHECK(r.port().setup(3, get_device_descriptor, r.now()) == usb::handshake::ack);
}

TEST_CASE("a bulk IN of 130 bytes, MPS 64, goes through a TX FIFO of 32 words in three packets")
{
  rig const r;
  r.enumerate();
  r.write("DIEPTXF1", r.mask("DIEPTXF1", "INEPTXFD", 32));
  r.write(
      "DIEPCTL1", r.mask("DIEPCTL1", "MPSIZ", 64)
                      | r.mask("DIEPCTL1", "USBAEP")
                      | r.mask("DIEPCTL1", "EPTYP", 2)
                      | r.mask("DIEPCTL1", "TXFNUM", 1)
                      | r.mask("DIEPCTL1", "SD0PID_SEVNFRM"));
  r.write("DIEPTSIZ1", r.mask("DIEPTSIZ1", "XFRSIZ", 130) | r.mask("DIEPTSIZ1", "PKTCNT", 3));
  r.write(
      "DIEPCTL1", r.read("DIEPCTL1") | r.mask("DIEPCTL1", "EPENA") | r.mask("DIEPCTL1", "CNAK"));
  std::vector<std::uint8_t> payload(130);
  for(std::size_t index = 0; index < payload.size(); ++index)
  {
    payload[index] = static_cast<std::uint8_t>(index);
  }
  std::size_t sent = 0;
  auto const  fill = [&] {
    for(auto packets = r.get("DIEPTSIZ1", "PKTCNT"); packets > 0; --packets)
    {
      auto const bytes = std::min<std::uint32_t>(r.get("DIEPTSIZ1", "XFRSIZ"), 64);
      if(bytes == 0 or bytes > 4 * r.get("DTXFSTS1", "INEPTFSAV"))
      {
        break;
      }
      r.push("DFIFO1", std::span{payload}.subspan(sent, bytes));
      sent += bytes;
    }
  };
  fill();
  CHECK(sent == 128);
  CHECK(r.get("DIEPTSIZ1", "XFRSIZ") == 2);
  auto const first = r.port().in(0, 1, r.now());
  CHECK(first.hs == usb::handshake::ack);
  CHECK(first.pid == usb::data_pid::data0);
  CHECK(first.data == std::vector<std::uint8_t>(payload.begin(), payload.begin() + 64));
  CHECK(r.get("DIEPTSIZ1", "PKTCNT") == 2);
  fill();
  CHECK(sent == 130);
  CHECK(r.get("DIEPTSIZ1", "XFRSIZ") == 0);
  auto const second = r.port().in(0, 1, r.now());
  CHECK(second.pid == usb::data_pid::data1);
  CHECK(second.data == std::vector<std::uint8_t>(payload.begin() + 64, payload.begin() + 128));
  CHECK(r.get("DIEPINT1", "XFRC") == 0);
  auto const third = r.port().in(0, 1, r.now());
  CHECK(third.pid == usb::data_pid::data0);
  CHECK(third.data == std::vector<std::uint8_t>(payload.begin() + 128, payload.end()));
  CHECK(r.get("DIEPINT1", "XFRC") == 1);
  CHECK(r.get("DIEPCTL1", "EPENA") == 0);
}

TEST_CASE("an OUT endpoint takes its maximum packet size from DOEPCTL, not DIEPCTL")
{
  rig const r;
  r.enumerate();
  r.write(
      "DOEPCTL2", r.mask("DOEPCTL2", "MPSIZ", 32)
                      | r.mask("DOEPCTL2", "USBAEP")
                      | r.mask("DOEPCTL2", "EPTYP", 2)
                      | r.mask("DOEPCTL2", "SD0PID_SEVNFRM"));
  r.write("DOEPTSIZ2", r.mask("DOEPTSIZ2", "XFRSIZ", 64) | r.mask("DOEPTSIZ2", "PKTCNT", 2));
  r.write(
      "DOEPCTL2", r.read("DOEPCTL2") | r.mask("DOEPCTL2", "EPENA") | r.mask("DOEPCTL2", "CNAK"));
  std::array<std::uint8_t, 32> full{};
  std::array<std::uint8_t, 40> oversized{};
  CHECK(r.port().out(0, 2, full, usb::data_pid::data0, r.now()) == usb::handshake::ack);
  CHECK(r.get("DOEPCTL2", "EPENA") == 1);
  r.write("DIEPCTL2", r.mask("DIEPCTL2", "MPSIZ", 8) | r.mask("DIEPCTL2", "USBAEP"));
  CHECK(r.port().out(0, 2, oversized, usb::data_pid::data1, r.now()) == usb::handshake::none);
  CHECK(r.port().out(0, 2, full, usb::data_pid::data1, r.now()) == usb::handshake::ack);
  CHECK(r.get("DOEPCTL2", "EPENA") == 0);
  CHECK(r.get("DOEPTSIZ2", "XFRSIZ") == 0);
  CHECK(r.pop().pktsts == pktsts_out_data);
  CHECK(r.pop_words(8).size() == 8);
  CHECK(r.pop().pktsts == pktsts_out_data);
  static_cast<void>(r.pop_words(8));
  auto const done = r.pop();
  CHECK(done.pktsts == pktsts_out_done);
  CHECK(done.endpoint == 2);
}

TEST_CASE("a host wake-up fires at its time, a new request replacing the pending one")
{
  rig const r;
  r.connect();
  auto const start = r.now().ps;
  r.port().wake_at(vtime{start + 5 * us});
  r.port().wake_at(vtime{start + 8 * us});
  r.run_for(20 * us);
  REQUIRE(r.host->wakes.size() == 1);
  CHECK(r.host->wakes[0] == start + 8 * us);
  r.port().wake_at(vtime{0});
  r.run_for(1 * us);
  CHECK(r.host->wakes.size() == 2);
}

TEST_CASE("the host state and a pending wake-up travel in the device snapshot")
{
  rig const first;
  first.connect();
  first.host->memory = {std::byte{1}, std::byte{2}, std::byte{3}};
  first.port().wake_at(vtime{first.now().ps + 5 * us});
  auto const saved = first.f->target->snapshot();

  rig const other;
  other.port().attach(other.host);
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.host->loads == 1);
  CHECK(other.host->memory == first.host->memory);
  CHECK(other.host->changes.empty());
  other.run_for(10 * us);
  CHECK(other.host->wakes.size() == 1);
}

TEST_CASE("a snapshot taken without a host gives a new host empty bytes and the pull-up")
{
  rig const first;
  first.power_up();
  first.set("DCTL", "SDIS", 0);
  REQUIRE_FALSE(first.port().pulled_up());
  auto const saved = first.f->target->snapshot();

  rig const other;
  other.port().attach(other.host);
  other.host->memory = {std::byte{9}};
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.host->loads == 1);
  CHECK(other.host->memory.empty());
  REQUIRE(other.host->changes.size() == 1);
  CHECK(other.host->changes[0].first);
}

TEST_CASE("a snapshot with 16 of 18 IN bytes in the FIFO resumes the transfer")
{
  rig const first;
  first.enumerate();
  first.set("DCFG", "DAD", 1);
  first.start_in0(18);
  std::array<std::uint8_t, 18> descriptor{};
  for(std::size_t index = 0; index < descriptor.size(); ++index)
  {
    descriptor[index] = static_cast<std::uint8_t>(0x40 + index);
  }
  first.push("DFIFO0", std::span{descriptor}.first(16));
  auto const saved = first.f->target->snapshot();

  rig const other;
  other.port().attach(other.host);
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.port().in(1, 0, other.now()).hs == usb::handshake::nak);
  other.push("DFIFO0", std::span{descriptor}.subspan(16));
  CHECK(other.port().in(0, 0, other.now()).hs == usb::handshake::none);
  auto const packet = other.port().in(1, 0, other.now());
  CHECK(packet.hs == usb::handshake::ack);
  CHECK(packet.pid == usb::data_pid::data0);
  CHECK(packet.data == std::vector<std::uint8_t>(descriptor.begin(), descriptor.end()));
}

TEST_CASE("a snapshot between a bus reset and ENUMDNE still raises ENUMDNE")
{
  rig const first;
  first.connect();
  first.port().bus_reset(first.now());
  auto const saved = first.f->target->snapshot();

  rig const other;
  other.port().attach(other.host);
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.get("GINTSTS", "USBRST") == 1);
  CHECK(other.get("GINTSTS", "ENUMDNE") == 0);
  other.run_for(20 * us);
  CHECK(other.get("GINTSTS", "ENUMDNE") == 1);
  CHECK(other.get("DSTS", "ENUMSPD") == 3);
}

TEST_CASE("the endpoint disable polls of the drivers end on the write that starts them")
{
  rig const r;
  r.enumerate();
  r.start_in0(8);
  r.write("DIEPCTL0", r.read("DIEPCTL0") | r.mask("DIEPCTL0", "SNAK"));
  CHECK(r.get("DIEPINT0", "INEPNE") == 1);
  CHECK(r.get("DIEPCTL0", "NAKSTS") == 1);
  CHECK(r.get("DIEPCTL0", "SNAK") == 0);
  r.write("DIEPCTL0", r.read("DIEPCTL0") | r.mask("DIEPCTL0", "EPDIS"));
  CHECK(r.get("DIEPINT0", "EPDISD") == 1);
  CHECK(r.get("DIEPCTL0", "EPENA") == 0);
  CHECK(r.get("DIEPCTL0", "EPDIS") == 0);
  r.write("DIEPINT0", r.mask("DIEPINT0", "EPDISD"));
  CHECK(r.get("DIEPINT0", "EPDISD") == 0);
  r.write("DIEPCTL0", r.read("DIEPCTL0") | r.mask("DIEPCTL0", "CNAK"));
  CHECK(r.get("DIEPINT0", "INEPNE") == 0);

  r.set("DOEPCTL1", "USBAEP", 1);
  r.write("DOEPCTL1", r.read("DOEPCTL1") | r.mask("DOEPCTL1", "EPENA"));
  r.write("DCTL", r.read("DCTL") | r.mask("DCTL", "SGONAK"));
  CHECK(r.get("GINTSTS", "GONAKEFF") == 1);
  CHECK(r.get("DCTL", "GONSTS") == 1);
  CHECK(r.pop().pktsts == pktsts_global_out_nak);
  r.write("DOEPCTL1", r.read("DOEPCTL1") | r.mask("DOEPCTL1", "EPDIS"));
  CHECK(r.get("DOEPINT1", "EPDISD") == 1);
  CHECK(r.get("DOEPCTL1", "EPENA") == 0);
  r.write("DCTL", r.read("DCTL") | r.mask("DCTL", "CGONAK"));
  CHECK(r.get("GINTSTS", "GONAKEFF") == 0);
  r.write("DCTL", r.read("DCTL") | r.mask("DCTL", "SGINAK"));
  CHECK(r.get("GINTSTS", "GINAKEFF") == 1);
  r.write("DCTL", r.read("DCTL") | r.mask("DCTL", "CGINAK"));
  CHECK(r.get("GINTSTS", "GINAKEFF") == 0);
}

TEST_CASE("a STALLed EP0 answers STALL until the next SETUP")
{
  rig const r;
  r.enumerate();
  r.write("DIEPCTL0", r.read("DIEPCTL0") | r.mask("DIEPCTL0", "STALL"));
  r.write("DOEPCTL0", r.read("DOEPCTL0") | r.mask("DOEPCTL0", "STALL"));
  CHECK(r.port().in(0, 0, r.now()).hs == usb::handshake::stall);
  CHECK(r.port().out(0, 0, {}, usb::data_pid::data1, r.now()) == usb::handshake::stall);
  r.write("DIEPCTL0", r.read("DIEPCTL0") & ~r.mask("DIEPCTL0", "STALL"));
  CHECK(r.get("DIEPCTL0", "STALL") == 1);
  CHECK(r.port().setup(0, get_device_descriptor, r.now()) == usb::handshake::ack);
  CHECK(r.get("DIEPCTL0", "STALL") == 0);
  CHECK(r.get("DOEPCTL0", "STALL") == 0);
  CHECK(r.port().in(0, 0, r.now()).hs == usb::handshake::nak);
}

TEST_CASE("SOF sets FNSOF and EOPF follows within the frame, suspend and resume raise theirs")
{
  rig const r;
  r.enumerate();
  r.port().start_of_frame(0x123, r.now());
  CHECK(r.get("GINTSTS", "SOF") == 1);
  CHECK(r.get("DSTS", "FNSOF") == 0x123);
  CHECK(r.get("GINTSTS", "EOPF") == 0);
  r.run_for(1000 * us);
  CHECK(r.get("GINTSTS", "EOPF") == 1);
  r.port().suspend(r.now());
  CHECK(r.get("GINTSTS", "USBSUSP") == 1);
  CHECK(r.get("GINTSTS", "ESUSP") == 1);
  CHECK(r.get("DSTS", "SUSPSTS") == 1);
  r.port().resume(r.now());
  CHECK(r.get("GINTSTS", "WKUPINT") == 1);
  CHECK(r.get("DSTS", "SUSPSTS") == 0);
}

TEST_CASE("a snapshot after a SETUP restored into a fresh machine pops the same words")
{
  rig const first;
  first.enumerate();
  REQUIRE(first.port().setup(0, get_device_descriptor, first.now()) == usb::handshake::ack);
  auto const saved = first.f->target->snapshot();
  auto const head  = first.pop();
  auto const words = first.pop_words(2);
  auto const done  = first.pop().pktsts;

  rig const other;
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.get("GINTSTS", "RXFLVL") == 1);
  CHECK(other.get("DOEPTSIZ0", "STUPCNT") == 2);
  auto const again = other.pop();
  CHECK(again.pktsts == head.pktsts);
  CHECK(again.bytes == head.bytes);
  CHECK(other.pop_words(2) == words);
  CHECK(other.pop().pktsts == done);
  CHECK(other.get("DOEPINT0", "STUP") == 1);
}

TEST_CASE("a machine reset releases the pull-up and empties the FIFOs")
{
  rig const r;
  r.enumerate();
  REQUIRE(r.port().setup(0, get_device_descriptor, r.now()) == usb::handshake::ack);
  r.f->target->reset();
  CHECK_FALSE(r.port().pulled_up());
  REQUIRE(r.host->changes.size() == 2);
  CHECK_FALSE(r.host->changes[1].first);
  CHECK(r.get("GINTSTS", "RXFLVL") == 0);
  CHECK(r.read("GRXSTSR_DEVICE") == 0);
}
