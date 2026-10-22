#include "stm32_fixture.hpp"

#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/usb.hpp>
#include <popcorn/periph/usb_host.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
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

constexpr std::uint32_t bulk        = 2;
constexpr std::uint32_t isochronous = 1;

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

  void open_in(std::size_t n, std::uint32_t type, std::uint32_t mps, std::uint32_t depth) const
  {
    auto const fifo = std::format("DIEPTXF{}", n);
    auto const ctl  = std::format("DIEPCTL{}", n);
    write(fifo, mask(fifo, "INEPTXFD", depth));
    write(
        ctl, mask(ctl, "MPSIZ", mps)
                 | mask(ctl, "USBAEP")
                 | mask(ctl, "EPTYP", type)
                 | mask(ctl, "TXFNUM", static_cast<std::uint32_t>(n))
                 | (type == isochronous ? 0u : mask(ctl, "SD0PID_SEVNFRM")));
  }

  void open_out(std::size_t n, std::uint32_t type, std::uint32_t mps) const
  {
    auto const ctl = std::format("DOEPCTL{}", n);
    write(
        ctl, mask(ctl, "MPSIZ", mps)
                 | mask(ctl, "USBAEP")
                 | mask(ctl, "EPTYP", type)
                 | (type == isochronous ? 0u : mask(ctl, "SD0PID_SEVNFRM")));
  }

  [[nodiscard]] auto next_parity_bit(std::string_view ctl, std::string_view odd) const
      -> std::uint32_t
  {
    return (get("DSTS", "FNSOF") & 1u) != 0 ? mask(ctl, "SD0PID_SEVNFRM") : mask(ctl, odd);
  }

  void arm_in(std::size_t n, std::uint32_t bytes, std::uint32_t packets, bool iso = false) const
  {
    auto const size = std::format("DIEPTSIZ{}", n);
    auto const ctl  = std::format("DIEPCTL{}", n);
    write(size, mask(size, "XFRSIZ", bytes) | mask(size, "PKTCNT", packets));
    write(
        ctl, read(ctl)
                 | mask(ctl, "EPENA")
                 | mask(ctl, "CNAK")
                 | (iso ? next_parity_bit(ctl, "SODDFRM") : 0u));
  }

  void arm_out(std::size_t n, std::uint32_t bytes, std::uint32_t packets, bool iso = false) const
  {
    auto const size = std::format("DOEPTSIZ{}", n);
    auto const ctl  = std::format("DOEPCTL{}", n);
    write(size, mask(size, "XFRSIZ", bytes) | mask(size, "PKTCNT", packets));
    write(
        ctl, read(ctl)
                 | mask(ctl, "EPENA")
                 | mask(ctl, "CNAK")
                 | (iso ? next_parity_bit(ctl, "SD1PID_SODDFRM") : 0u));
  }

  void frame(std::uint16_t number) const
  {
    port().start_of_frame(number, now());
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

TEST_CASE("a bulk OUT of 64 then a short 10 with XFRSIZ 128 completes on the short packet")
{
  rig const r;
  r.enumerate();
  r.open_out(2, bulk, 64);
  r.arm_out(2, 128, 2);
  std::vector<std::uint8_t> full(64, 0xa5);
  std::vector<std::uint8_t> tail(10, 0x5a);
  CHECK(r.port().out(0, 2, full, usb::data_pid::data0, r.now()) == usb::handshake::ack);
  CHECK(r.port().out(0, 2, tail, usb::data_pid::data1, r.now()) == usb::handshake::ack);
  CHECK(r.get("DOEPTSIZ2", "XFRSIZ") == 54);
  CHECK(r.get("DOEPCTL2", "EPENA") == 0);
  auto const first = r.pop();
  CHECK(first.pktsts == pktsts_out_data);
  CHECK(first.bytes == 64);
  CHECK(first.dpid == 0);
  static_cast<void>(r.pop_words(16));
  auto const second = r.pop();
  CHECK(second.pktsts == pktsts_out_data);
  CHECK(second.bytes == 10);
  CHECK(second.dpid == 2);
  CHECK(r.pop_words(3).front() == 0x5a5a5a5au);
  CHECK(r.get("DOEPINT2", "XFRC") == 0);
  auto const done = r.pop();
  CHECK(done.pktsts == pktsts_out_done);
  CHECK(done.endpoint == 2);
  CHECK(r.get("DOEPINT2", "XFRC") == 1);
}

TEST_CASE("a STALLed bulk IN answers STALL, clear stall with SD0PID restarts at DATA0")
{
  rig const r;
  r.enumerate();
  r.open_in(1, bulk, 64, 32);
  std::array<std::uint8_t, 8> bytes{1, 2, 3, 4, 5, 6, 7, 8};
  r.arm_in(1, 8, 1);
  r.push("DFIFO1", bytes);
  REQUIRE(r.port().in(0, 1, r.now()).pid == usb::data_pid::data0);
  r.write("DIEPCTL1", r.read("DIEPCTL1") | r.mask("DIEPCTL1", "STALL"));
  CHECK(r.port().in(0, 1, r.now()).hs == usb::handshake::stall);
  r.write(
      "DIEPCTL1",
      (r.read("DIEPCTL1") & ~r.mask("DIEPCTL1", "STALL")) | r.mask("DIEPCTL1", "SD0PID_SEVNFRM"));
  CHECK(r.get("DIEPCTL1", "STALL") == 0);
  CHECK(r.port().in(0, 1, r.now()).hs == usb::handshake::nak);
  r.arm_in(1, 8, 1);
  r.push("DFIFO1", bytes);
  auto const packet = r.port().in(0, 1, r.now());
  CHECK(packet.hs == usb::handshake::ack);
  CHECK(packet.pid == usb::data_pid::data0);
}

TEST_CASE("SNAK and EPDIS on a bulk IN endpoint take effect on their write")
{
  rig const r;
  r.enumerate();
  r.open_in(1, bulk, 64, 32);
  r.arm_in(1, 64, 1);
  r.write("DIEPCTL1", r.read("DIEPCTL1") | r.mask("DIEPCTL1", "SNAK"));
  CHECK(r.get("DIEPINT1", "INEPNE") == 1);
  CHECK(r.port().in(0, 1, r.now()).hs == usb::handshake::nak);
  r.write("DIEPCTL1", r.read("DIEPCTL1") | r.mask("DIEPCTL1", "EPDIS"));
  CHECK(r.get("DIEPINT1", "EPDISD") == 1);
  CHECK(r.get("DIEPCTL1", "EPENA") == 0);
  r.set("DIEPMSK", "EPDM", 1);
  r.set("DAINTMSK", "IEPM", 1u << 1);
  CHECK(r.get("DAINT", "IEPINT") == 1u << 1);
  CHECK(r.get("GINTSTS", "IEPINT") == 1);
}

TEST_CASE("a snapshot during a bulk IN with 64 of 130 bytes taken resumes with the second packet")
{
  rig const first;
  first.enumerate();
  first.open_in(1, bulk, 64, 32);
  first.arm_in(1, 130, 3);
  std::vector<std::uint8_t> payload(130);
  for(std::size_t index = 0; index < payload.size(); ++index)
  {
    payload[index] = static_cast<std::uint8_t>(3 * index);
  }
  first.push("DFIFO1", std::span{payload}.first(128));
  REQUIRE(first.port().in(0, 1, first.now()).data.size() == 64);
  auto const saved = first.f->target->snapshot();

  rig const other;
  other.port().attach(other.host);
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  auto const second = other.port().in(0, 1, other.now());
  CHECK(second.hs == usb::handshake::ack);
  CHECK(second.pid == usb::data_pid::data1);
  CHECK(second.data == std::vector<std::uint8_t>(payload.begin() + 64, payload.begin() + 128));
  other.push("DFIFO1", std::span{payload}.subspan(128));
  auto const third = other.port().in(0, 1, other.now());
  CHECK(third.data == std::vector<std::uint8_t>(payload.begin() + 128, payload.end()));
  CHECK(other.get("DIEPINT1", "XFRC") == 1);
}

TEST_CASE("an iso IN armed for an odd frame answers only in odd frames, IISOIXFR when missed")
{
  rig const r;
  r.enumerate();
  r.open_in(3, isochronous, 34, 16);
  r.frame(10);
  r.arm_in(3, 32, 1, true);
  CHECK(r.get("DIEPCTL3", "EONUM_DPIP") == 1);
  std::array<std::uint8_t, 32> pcm{};
  r.push("DFIFO3", pcm);
  CHECK(r.port().in(0, 3, r.now()).hs == usb::handshake::none);
  r.run_for(1000 * us);
  CHECK(r.get("GINTSTS", "EOPF") == 1);
  CHECK(r.get("GINTSTS", "IISOIXFR") == 0);
  r.frame(11);
  r.run_for(1000 * us);
  CHECK(r.get("GINTSTS", "IISOIXFR") == 1);
  r.write("GINTSTS", r.mask("GINTSTS", "IISOIXFR"));
  r.frame(13);
  auto const packet = r.port().in(0, 3, r.now());
  CHECK(packet.hs == usb::handshake::ack);
  CHECK(packet.pid == usb::data_pid::data0);
  CHECK(packet.data.size() == 32);
  CHECK(r.get("DIEPINT3", "XFRC") == 1);
  r.run_for(1000 * us);
  CHECK(r.get("GINTSTS", "IISOIXFR") == 0);
}

TEST_CASE("an iso IN re-armed each frame the way TinyUSB does sends 32 bytes every frame")
{
  rig const r;
  r.enumerate();
  r.open_in(3, isochronous, 34, 16);
  r.set("GINTMSK", "IISOIXFRM", 1);
  r.frame(100);
  std::array<std::uint8_t, 32> pcm{};
  std::size_t                  sent  = 0;
  auto const                   queue = [&] {
    for(auto& sample : pcm)
    {
      sample = static_cast<std::uint8_t>(sent);
    }
    r.arm_in(3, 32, 1, true);
    r.push("DFIFO3", pcm);
  };
  queue();
  for(std::uint16_t frame = 101; frame < 111; ++frame)
  {
    r.frame(frame);
    auto const packet = r.port().in(0, 3, r.now());
    REQUIRE(packet.hs == usb::handshake::ack);
    CHECK(packet.pid == usb::data_pid::data0);
    CHECK(packet.data == std::vector<std::uint8_t>(32, static_cast<std::uint8_t>(sent)));
    REQUIRE(r.get("DIEPINT3", "XFRC") == 1);
    r.write("DIEPINT3", r.mask("DIEPINT3", "XFRC"));
    ++sent;
    queue();
    r.run_for(1000 * us);
    CHECK(r.get("GINTSTS", "IISOIXFR") == 0);
  }
  CHECK(sent == 10);
}

TEST_CASE("an iso OUT of 32 bytes per frame for 10 frames gives 10 PKTSTS 2 and 3 pairs")
{
  rig const r;
  r.enumerate();
  r.open_out(3, isochronous, 34);
  r.frame(200);
  r.arm_out(3, 34, 1, true);
  std::size_t pairs = 0;
  for(std::uint16_t frame = 201; frame < 211; ++frame)
  {
    r.frame(frame);
    std::vector<std::uint8_t> pcm(32, static_cast<std::uint8_t>(frame));
    auto const                pid = (frame % 2) != 0 ? usb::data_pid::data1 : usb::data_pid::data0;
    REQUIRE(r.port().out(0, 3, pcm, pid, r.now()) == usb::handshake::ack);
    auto const data = r.pop();
    CHECK(data.pktsts == pktsts_out_data);
    CHECK(data.endpoint == 3);
    CHECK(data.bytes == 32);
    CHECK(data.dpid == 0);
    CHECK(r.pop_words(8).front() == 0x01010101u * static_cast<std::uint8_t>(frame));
    if(r.pop().pktsts == pktsts_out_done)
    {
      ++pairs;
    }
    r.write("DOEPINT3", r.mask("DOEPINT3", "XFRC"));
    r.arm_out(3, 34, 1, true);
    r.run_for(1000 * us);
    CHECK(r.get("GINTSTS", "IPXFR") == 0);
  }
  CHECK(pairs == 10);
}

TEST_CASE("an iso OUT outside its frame is dropped and INCOMPISOOUT rises at EOPF")
{
  rig const r;
  r.enumerate();
  r.open_out(3, isochronous, 34);
  r.frame(300);
  r.arm_out(3, 34, 1, true);
  CHECK(r.get("DOEPCTL3", "EONUM_DPIP") == 1);
  std::vector<std::uint8_t> pcm(32);
  CHECK(r.port().out(0, 3, pcm, usb::data_pid::data0, r.now()) == usb::handshake::none);
  CHECK(r.get("GINTSTS", "RXFLVL") == 0);
  r.run_for(1000 * us);
  CHECK(r.get("GINTSTS", "IPXFR") == 0);
  r.frame(301);
  r.run_for(1000 * us);
  CHECK(r.get("GINTSTS", "IPXFR") == 1);
}

TEST_CASE("FIFOs above the 320 words of FIFO RAM warn when an IN endpoint is activated")
{
  rig const                     r;
  std::vector<core::log_record> logged;
  auto const                    sink =
      r.f->target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  r.write("GRXFSIZ", r.mask("GRXFSIZ", "RXFD", 128));
  r.write("HNPTXFSIZ", r.mask("HNPTXFSIZ", "NPTXFD", 16));
  r.open_in(1, bulk, 64, 64);
  CHECK(logged.empty());
  r.open_in(2, bulk, 64, 128);
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].kind == core::log_kind::model_warning);
  CHECK(logged[0].message.contains("336 words"));
}

TEST_CASE("TinyUSB-sized FIFOs within the 320 words of FIFO RAM do not warn")
{
  rig const                     r;
  std::vector<core::log_record> logged;
  auto const                    sink =
      r.f->target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  r.write("GRXFSIZ", r.mask("GRXFSIZ", "RXFD", 60));
  r.write("HNPTXFSIZ", r.mask("HNPTXFSIZ", "NPTXFD", 16));
  r.open_in(1, 3, 8, 2);
  r.open_in(2, bulk, 64, 16);
  r.open_in(3, isochronous, 34, 9);
  r.open_in(4, 3, 64, 16);
  CHECK(logged.empty());
}

TEST_CASE("a zero-length iso IN completes in its frame without IISOIXFR")
{
  rig const r;
  r.enumerate();
  r.open_in(3, isochronous, 34, 16);
  r.frame(20);
  r.arm_in(3, 0, 1, true);
  r.frame(21);
  auto const packet = r.port().in(0, 3, r.now());
  CHECK(packet.hs == usb::handshake::ack);
  CHECK(packet.data.empty());
  CHECK(r.get("DIEPINT3", "XFRC") == 1);
  CHECK(r.get("DIEPCTL3", "EPENA") == 0);
  CHECK(r.get("DIEPTSIZ3", "PKTCNT") == 0);
  r.run_for(1000 * us);
  CHECK(r.get("GINTSTS", "IISOIXFR") == 0);
}

TEST_CASE("an iso IN re-armed for the other parity after IISOIXFR goes out in the next frame")
{
  rig const r;
  r.enumerate();
  r.open_in(3, isochronous, 34, 16);
  r.frame(30);
  r.arm_in(3, 32, 1, true);
  r.frame(31);
  r.run_for(1000 * us);
  REQUIRE(r.get("GINTSTS", "IISOIXFR") == 1);
  r.write("GINTSTS", r.mask("GINTSTS", "IISOIXFR"));
  REQUIRE(r.get("DIEPCTL3", "EPENA") == 1);
  r.write("DIEPTSIZ3", r.mask("DIEPTSIZ3", "XFRSIZ", 32) | r.mask("DIEPTSIZ3", "PKTCNT", 1));
  r.write("DIEPCTL3", r.read("DIEPCTL3") | r.mask("DIEPCTL3", "SD0PID_SEVNFRM"));
  CHECK(r.get("DIEPCTL3", "EONUM_DPIP") == 0);
  std::array<std::uint8_t, 32> pcm{};
  r.push("DFIFO3", pcm);
  CHECK(r.port().in(0, 3, r.now()).hs == usb::handshake::none);
  r.frame(32);
  auto const packet = r.port().in(0, 3, r.now());
  CHECK(packet.hs == usb::handshake::ack);
  CHECK(packet.data.size() == 32);
  r.run_for(1000 * us);
  CHECK(r.get("GINTSTS", "IISOIXFR") == 0);
}

TEST_CASE("a snapshot mid iso frame after the packet went out raises no IISOIXFR at EOPF")
{
  rig const first;
  first.enumerate();
  first.open_in(3, isochronous, 34, 16);
  first.frame(40);
  first.arm_in(3, 32, 2, true);
  std::array<std::uint8_t, 32> pcm{};
  first.push("DFIFO3", pcm);
  first.frame(41);
  REQUIRE(first.port().in(0, 3, first.now()).data.size() == 32);
  REQUIRE(first.get("DIEPCTL3", "EPENA") == 1);
  auto const saved = first.f->target->snapshot();

  rig const other;
  other.port().attach(other.host);
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  other.run_for(1000 * us);
  CHECK(other.get("GINTSTS", "EOPF") == 1);
  CHECK(other.get("GINTSTS", "IISOIXFR") == 0);
}

TEST_CASE("an interrupt IN endpoint NAKs until armed, then sends with toggles like bulk")
{
  rig const r;
  r.enumerate();
  r.open_in(1, 3, 8, 4);
  CHECK(r.port().in(0, 1, r.now()).hs == usb::handshake::nak);
  std::array<std::uint8_t, 8> report{0x01, 0x02};
  for(auto const pid : {usb::data_pid::data0, usb::data_pid::data1})
  {
    r.arm_in(1, 8, 1);
    r.push("DFIFO1", report);
    auto const packet = r.port().in(0, 1, r.now());
    CHECK(packet.hs == usb::handshake::ack);
    CHECK(packet.pid == pid);
    CHECK(packet.data == std::vector<std::uint8_t>(report.begin(), report.end()));
    CHECK(r.get("DIEPINT1", "XFRC") == 1);
    r.write("DIEPINT1", r.mask("DIEPINT1", "XFRC"));
  }
  CHECK(r.port().in(0, 1, r.now()).hs == usb::handshake::nak);
}

TEST_CASE("a bus reset clears USBAEP on EP1-5 and keeps EP0")
{
  rig const r;
  r.enumerate();
  r.open_in(1, bulk, 64, 16);
  r.open_out(2, bulk, 64);
  r.port().bus_reset(r.now());
  CHECK(r.get("DIEPCTL1", "USBAEP") == 0);
  CHECK(r.get("DOEPCTL2", "USBAEP") == 0);
  CHECK(r.get("DOEPCTL0", "USBAEP") == 1);
}

TEST_CASE("an iso OUT is not taken under a global OUT NAK")
{
  rig const r;
  r.enumerate();
  r.open_out(3, isochronous, 34);
  r.frame(50);
  r.arm_out(3, 34, 1, true);
  r.frame(51);
  r.write("DCTL", r.read("DCTL") | r.mask("DCTL", "SGONAK"));
  REQUIRE(r.pop().pktsts == pktsts_global_out_nak);
  std::vector<std::uint8_t> pcm(32);
  CHECK(r.port().out(0, 3, pcm, usb::data_pid::data0, r.now()) == usb::handshake::none);
  CHECK(r.get("GINTSTS", "RXFLVL") == 0);
}

namespace
{
struct scripted_firmware
{
  std::optional<usb::setup_packet> setup;
  std::vector<usb::setup_packet>   requests;
};

auto firmware_device() -> std::vector<std::uint8_t>
{
  return {18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x09, 0x12, 0x02, 0x00, 0x00, 0x01, 1, 2, 0, 1};
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
      reply = {8, 3, 'p', 0, 'o', 0, 'p', 0};
      break;
    case 0x0302:
      reply = {6, 3, 'f', 0, 'w', 0};
      break;
    default:
      return std::nullopt;
  }
  reply.resize(std::min<std::size_t>(reply.size(), setup.length));
  return reply;
}

void arm_status_out(rig const& r)
{
  r.write("DOEPTSIZ0", r.mask("DOEPTSIZ0", "PKTCNT", 1) | r.mask("DOEPTSIZ0", "STUPCNT", 3));
  r.write(
      "DOEPCTL0", r.read("DOEPCTL0") | r.mask("DOEPCTL0", "EPENA") | r.mask("DOEPCTL0", "CNAK"));
}

void send_in0(rig const& r, std::span<std::uint8_t const> bytes)
{
  auto const packets =
      std::max<std::uint32_t>(1, static_cast<std::uint32_t>((bytes.size() + 63) / 64));
  r.write(
      "DIEPTSIZ0", r.mask("DIEPTSIZ0", "XFRSIZ", static_cast<std::uint32_t>(bytes.size()))
                       | r.mask("DIEPTSIZ0", "PKTCNT", packets));
  r.write(
      "DIEPCTL0", r.read("DIEPCTL0") | r.mask("DIEPCTL0", "EPENA") | r.mask("DIEPCTL0", "CNAK"));
  r.push("DFIFO0", bytes);
}

void answer(rig const& r, usb::setup_packet const& setup)
{
  if(auto const reply = firmware_reply(setup))
  {
    send_in0(r, *reply);
    arm_status_out(r);
    return;
  }
  if(setup.request_type == 0 and setup.request == 5)
  {
    r.set("DCFG", "DAD", setup.value);
  }
  else if(setup.request_type != 0 or setup.request != 9)
  {
    r.write("DIEPCTL0", r.read("DIEPCTL0") | r.mask("DIEPCTL0", "STALL"));
    return;
  }
  send_in0(r, {});
}

void service(rig const& r, scripted_firmware& fw)
{
  if(r.get("GINTSTS", "USBRST") != 0)
  {
    r.write("GINTSTS", r.mask("GINTSTS", "USBRST"));
    r.set("DCFG", "DAD", 0);
    r.write("DOEPTSIZ0", r.mask("DOEPTSIZ0", "STUPCNT", 3));
  }
  if(r.get("GINTSTS", "ENUMDNE") != 0)
  {
    r.write("GINTSTS", r.mask("GINTSTS", "ENUMDNE"));
  }
  while(r.get("GINTSTS", "RXFLVL") != 0)
  {
    auto const entry = r.pop();
    if(entry.pktsts == pktsts_setup_data)
    {
      auto const words = r.pop_words(2);
      fw.setup         = usb::setup_packet{
          .request_type = static_cast<std::uint8_t>(words[0]),
          .request      = static_cast<std::uint8_t>(words[0] >> 8),
          .value        = static_cast<std::uint16_t>(words[0] >> 16),
          .index        = static_cast<std::uint16_t>(words[1]),
          .length       = static_cast<std::uint16_t>(words[1] >> 16)};
    }
    else if(entry.pktsts == pktsts_setup_done and fw.setup)
    {
      r.write("DOEPINT0", r.mask("DOEPINT0", "STUP"));
      fw.requests.push_back(*fw.setup);
      answer(r, *fw.setup);
    }
    else if(entry.pktsts == pktsts_out_data)
    {
      static_cast<void>(r.pop_words((entry.bytes + 3) / 4));
    }
  }
}

void run_firmware(rig const& r, scripted_firmware& fw, std::function_ref<bool()> done)
{
  for(std::size_t slice = 0; slice < 20'000 and not done(); ++slice)
  {
    r.run_for(25 * us);
    service(r, fw);
  }
}
}

TEST_CASE("the virtual host enumerates the OTG_FS model run by a register-level firmware")
{
  rig const                r;
  std::vector<std::string> warnings;
  usb::host_options        options;
  options.warn           = [&](std::string const& message) { warnings.push_back(message); };
  auto const        host = std::make_shared<usb::host>(r.port(), std::move(options));
  scripted_firmware fw;
  r.power_up();
  r.port().attach(host);
  r.set("DCTL", "SDIS", 0);
  CHECK(host->state() == usb::device_state::powered);
  run_firmware(r, fw, [&] { return host->state() == usb::device_state::configured; });
  REQUIRE(host->state() == usb::device_state::configured);
  CHECK(warnings.empty());
  CHECK(host->address() == 1);
  CHECK(r.get("DCFG", "DAD") == 1);
  CHECK(std::ranges::equal(host->device_bytes(), firmware_device()));
  CHECK(std::ranges::equal(host->configuration_bytes(), firmware_configuration()));
  CHECK(host->strings().size() == 3);
  REQUIRE(fw.requests.size() == 9);
  CHECK(fw.requests.front().value == 0x0100);
  CHECK(fw.requests.front().length == 64);
  CHECK(fw.requests.back().request == 9);
  std::vector<usb::setup_packet> logged;
  for(auto const& entry : host->log())
  {
    if(entry.kind == usb::log_kind::request)
    {
      CHECK(entry.status == usb::transfer_status::completed);
      logged.push_back(entry.setup);
    }
  }
  CHECK(logged == fw.requests);

  std::vector<std::byte> host_state;
  host->save(host_state);
  std::vector<std::byte> model_state;
  r.f->target->model("OTG_FS")->save(model_state);
  MESSAGE("OTG_FS state ", model_state.size(), " B, host blob ", host_state.size(), " B");
  CHECK_FALSE(std::ranges::search(model_state, host_state).empty());
  CHECK(model_state.size() < host_state.size() + 4096);

  auto const saved = r.f->target->snapshot();
  rig const  other;
  auto const restored = std::make_shared<usb::host>(other.port(), usb::host_options{});
  other.port().attach(restored);
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(restored->state() == usb::device_state::configured);
  CHECK(restored->address() == 1);
  auto const id =
      restored->control({.request_type = 0x80, .request = 6, .value = 0x0100, .length = 18});
  scripted_firmware                 again;
  std::vector<usb::transfer_result> results;
  run_firmware(other, again, [&] {
    auto taken = restored->take_results();
    results.insert(results.end(), taken.begin(), taken.end());
    return not results.empty();
  });
  REQUIRE(results.size() == 1);
  CHECK(results[0].id == id);
  CHECK(results[0].status == usb::transfer_status::completed);
  CHECK(results[0].data == firmware_device());
  CHECK(again.requests.size() == 1);
}

TEST_CASE("OTGRST releases the pull-up, the host sees the detach, and the device enumerates again")
{
  rig const r;
  r.enumerate();
  REQUIRE(r.port().setup(0, get_device_descriptor, r.now()) == usb::handshake::ack);
  rcc_reset_pulse(*r.f, "RCC_AHB2RSTR1", "OTGRST");
  CHECK_FALSE(r.port().pulled_up());
  REQUIRE(r.host->changes.size() == 2);
  CHECK_FALSE(r.host->changes[1].first);
  CHECK(r.get("GINTSTS", "RXFLVL") == 0);
  r.enumerate();
  CHECK(r.port().pulled_up());
  CHECK(r.port().setup(0, get_device_descriptor, r.now()) == usb::handshake::ack);
}

TEST_CASE("OTGRST moves every event generation past the events queued before it, twice")
{
  rig const                 r;
  std::vector<std::int64_t> seen;
  for(int pulse = 0; pulse < 2; ++pulse)
  {
    r.connect();
    r.port().bus_reset(r.now());
    std::array<std::int64_t, 4> before{};
    bool                        queued = false;
    for(std::size_t kind = 0; kind < before.size(); ++kind)
    {
      auto const value =
          popcorn::testing::saved_integer(*r.f, "OTG_FS", std::format("generation.{}", kind));
      REQUIRE(value.has_value());
      before[kind] = *value;
      queued       = queued
                  or popcorn::testing::queued_event(
                         *r.f, static_cast<std::uint32_t>(kind), static_cast<std::uint64_t>(*value));
    }
    REQUIRE(queued);
    seen.push_back(before[0]);
    rcc_reset_pulse(*r.f, "RCC_AHB2RSTR1", "OTGRST");
    for(std::size_t kind = 0; kind < before.size(); ++kind)
    {
      CAPTURE(kind);
      auto const after =
          popcorn::testing::saved_integer(*r.f, "OTG_FS", std::format("generation.{}", kind));
      REQUIRE(after.has_value());
      CHECK(*after > before[kind]);
    }
  }
  CHECK(seen[1] > seen[0]);
}
