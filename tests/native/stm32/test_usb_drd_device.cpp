#include "usb_drd_rig.hpp"

#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
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
#include <vector>

namespace
{
namespace core = popcorn::core;
using namespace popcorn::testing::usb_drd;

constexpr usb::setup_packet
    get_device{.request_type = 0x80, .request = 6, .value = 0x0100, .index = 0, .length = 18};

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

TEST_CASE("DPPU_DPD with a host attached tells the host once, and never without a host")
{
  rig const r;
  r.power_up();
  r.write("BCDR", r.mask("BCDR", "DPPU_DPD"));
  CHECK_FALSE(r.port().pulled_up());
  r.port().attach(r.host);
  CHECK(r.port().pulled_up());
  r.write("BCDR", r.mask("BCDR", "DPPU_DPD"));
  REQUIRE(r.host->changes.size() == 1);
  CHECK(r.host->changes[0].first);
  r.write("BCDR", 0);
  REQUIRE(r.host->changes.size() == 2);
  CHECK_FALSE(r.host->changes[1].first);
}

TEST_CASE(
    "a SETUP lands in the EP0 receive buffer with COUNT_RX 8, both toggles 1 and both STATs NAK")
{
  rig const r;
  r.enumerate();
  CHECK(r.port().setup(0, get_device, r.now()) == usb::handshake::ack);
  auto const bytes = get_device.bytes();
  CHECK(r.bytes_at(0x80, 8) == std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
  CHECK(r.rx_count(0) == 8);
  CHECK(r.get("CHEP0R", "DTOGRX") == 1);
  CHECK(r.get("CHEP0R", "DTOGTX") == 1);
  CHECK(r.get("CHEP0R", "STATRX") == nak);
  CHECK(r.get("CHEP0R", "STATTX") == nak);
  CHECK(r.get("CHEP0R", "SETUP") == 1);
  CHECK(r.get("CHEP0R", "VTRX") == 1);
  CHECK(r.get("ISTR", "CTR") == 1);
  CHECK(r.get("ISTR", "DIR") == 1);
  CHECK(r.get("ISTR", "IDN") == 0);
  CHECK(r.port().setup(0, get_device, r.now()) == usb::handshake::none);
}

TEST_CASE("a SETUP is taken with STATRX NAK or STALL, and ignored with STATRX DISABLED")
{
  rig const r;
  r.enumerate();
  r.endpoint(0, {.address = 0, .type = control, .stat_tx = nak, .stat_rx = stall});
  CHECK(r.port().setup(0, get_device, r.now()) == usb::handshake::ack);
  r.clear_flags(0);
  r.endpoint(0, {.address = 0, .type = control, .stat_tx = nak, .stat_rx = disabled});
  CHECK(r.port().setup(0, get_device, r.now()) == usb::handshake::none);
}

TEST_CASE("tokens to another address or with DADDR.EF clear get no answer")
{
  rig const r;
  r.enumerate();
  CHECK(r.port().setup(3, get_device, r.now()) == usb::handshake::none);
  r.write("DADDR", r.mask("DADDR", "ADD", 3));
  CHECK(r.port().setup(3, get_device, r.now()) == usb::handshake::none);
  r.write("DADDR", r.mask("DADDR", "ADD", 3) | r.mask("DADDR", "EF"));
  CHECK(r.port().setup(3, get_device, r.now()) == usb::handshake::ack);
}

TEST_CASE("an EP0 IN of 18 bytes NAKs until STATTX is VALID, then sends DATA1 and sets VTTX")
{
  rig const r;
  r.enumerate();
  REQUIRE(r.port().setup(0, get_device, r.now()) == usb::handshake::ack);
  CHECK(r.port().in(0, 0, r.now()).hs == usb::handshake::nak);
  r.clear_flags(0);
  auto const reply = sequence(18, 1);
  r.put_bytes(0x40, reply);
  r.tx_buffer(0, 0x40, 18);
  r.endpoint(
      0, {.address = 0,
          .type    = control,
          .stat_tx = valid,
          .stat_rx = nak,
          .dtog_tx = 1,
          .dtog_rx = 1});
  auto const answer = r.port().in(0, 0, r.now());
  CHECK(answer.hs == usb::handshake::ack);
  CHECK(answer.pid == usb::data_pid::data1);
  CHECK(answer.data == reply);
  CHECK(r.get("CHEP0R", "STATTX") == nak);
  CHECK(r.get("CHEP0R", "DTOGTX") == 0);
  CHECK(r.get("CHEP0R", "VTTX") == 1);
  CHECK(r.get("ISTR", "DIR") == 0);
}

TEST_CASE(
    "an OUT with VALID and the expected toggle lands, an OUT with the other toggle is dropped")
{
  rig const r;
  r.enumerate();
  r.rx_buffer(2, 0x100, 64);
  r.endpoint(2, {.address = 2, .type = bulk, .stat_rx = valid});
  auto const data = sequence(10, 0x20);
  CHECK(r.port().out(0, 2, data, usb::data_pid::data1, r.now()) == usb::handshake::ack);
  CHECK(r.get("CHEP2R", "VTRX") == 0);
  CHECK(r.port().out(0, 2, data, usb::data_pid::data0, r.now()) == usb::handshake::ack);
  CHECK(r.get("CHEP2R", "VTRX") == 1);
  CHECK(r.rx_count(2) == 10);
  CHECK(r.bytes_at(0x100, 10) == data);
  CHECK(r.get("CHEP2R", "DTOGRX") == 1);
  CHECK(r.get("CHEP2R", "STATRX") == nak);
  CHECK(r.port().out(0, 2, data, usb::data_pid::data1, r.now()) == usb::handshake::nak);
}

TEST_CASE("an OUT longer than its buffer is STALLed with the bytes that fit and no VTRX")
{
  rig const r;
  r.enumerate();
  r.set_pma_word(0x110, 0xffff'ffffu);
  r.rx_buffer(2, 0x100, 16);
  r.endpoint(2, {.address = 2, .type = bulk, .stat_rx = valid});
  auto const data = sequence(20, 0x40);
  CHECK(r.port().out(0, 2, data, usb::data_pid::data0, r.now()) == usb::handshake::stall);
  CHECK(r.bytes_at(0x100, 16) == std::vector<std::uint8_t>(data.begin(), data.begin() + 16));
  CHECK(r.pma_word(0x110) == 0xffff'ffffu);
  CHECK(r.get("CHEP2R", "VTRX") == 0);
  CHECK(r.get("CHEP2R", "STATRX") == valid);
}

TEST_CASE("STATUS_OUT STALLs an EP0 OUT with data and takes a zero-length one")
{
  rig const r;
  r.enumerate();
  r.endpoint(
      0,
      {.address = 0, .type = control, .stat_tx = nak, .stat_rx = valid, .kind = 1, .dtog_rx = 1});
  std::vector<std::uint8_t> const one{7};
  CHECK(r.port().out(0, 0, one, usb::data_pid::data1, r.now()) == usb::handshake::stall);
  CHECK(r.port().out(0, 0, {}, usb::data_pid::data1, r.now()) == usb::handshake::ack);
  CHECK(r.get("CHEP0R", "VTRX") == 1);
  CHECK(r.get("CHEP0R", "SETUP") == 0);
}

TEST_CASE("a STALLed endpoint answers STALL both ways")
{
  rig const r;
  r.enumerate();
  r.endpoint(1, {.address = 1, .type = bulk, .stat_tx = stall, .stat_rx = stall});
  CHECK(r.port().in(0, 1, r.now()).hs == usb::handshake::stall);
  CHECK(r.port().out(0, 1, {}, usb::data_pid::data0, r.now()) == usb::handshake::stall);
}

TEST_CASE("a double-buffered bulk OUT fills the two buffers by DTOGRX and NAKs on SW_BUF")
{
  rig const r;
  r.enumerate();
  r.rx_buffer(3, 0x200, 64, 0);
  r.rx_buffer(3, 0x240, 64, 1);
  r.endpoint(3, {.address = 3, .type = bulk, .stat_rx = valid, .kind = 1, .dtog_tx = 1});
  auto const first  = sequence(8, 0x10);
  auto const second = sequence(8, 0x80);
  CHECK(r.port().out(0, 3, first, usb::data_pid::data0, r.now()) == usb::handshake::ack);
  CHECK(r.get("CHEP3R", "STATRX") == nak);
  r.write(
      "CHEP3R",
      (r.read("CHEP3R")
       & ~(r.mask("CHEP3R", "STATRX", 3) | r.mask("CHEP3R", "DTOGRX") | r.mask("CHEP3R", "DTOGTX")))
          | r.mask("CHEP3R", "STATRX", nak ^ valid));
  CHECK(r.port().out(0, 3, second, usb::data_pid::data1, r.now()) == usb::handshake::nak);
  r.write(
      "CHEP3R", (r.read("CHEP3R") & ~r.mask("CHEP3R", "STATRX", 3) & ~r.mask("CHEP3R", "DTOGRX"))
                    | r.mask("CHEP3R", "DTOGTX"));
  CHECK(r.port().out(0, 3, second, usb::data_pid::data1, r.now()) == usb::handshake::ack);
  CHECK(r.bytes_at(0x200, 8) == first);
  CHECK(r.bytes_at(0x240, 8) == second);
  CHECK(r.rx_count(3, 0) == 8);
  CHECK(r.rx_count(3, 1) == 8);
  CHECK(r.get("CHEP3R", "STATRX") == valid);
}

TEST_CASE("a double-buffered bulk IN sends the two buffers by DTOGTX")
{
  rig const r;
  r.enumerate();
  auto const first  = sequence(4, 1);
  auto const second = sequence(6, 9);
  r.put_bytes(0x300, first);
  r.put_bytes(0x340, second);
  r.tx_buffer(4, 0x300, 4, 0);
  r.tx_buffer(4, 0x340, 6, 1);
  r.endpoint(4, {.address = 4, .type = bulk, .stat_tx = valid, .kind = 1, .dtog_rx = 1});
  auto const a = r.port().in(0, 4, r.now());
  CHECK(a.data == first);
  CHECK(a.pid == usb::data_pid::data0);
  CHECK(r.get("CHEP4R", "STATTX") == nak);
  r.write(
      "CHEP4R",
      (r.read("CHEP4R")
       & ~(r.mask("CHEP4R", "STATTX", 3) | r.mask("CHEP4R", "DTOGTX") | r.mask("CHEP4R", "DTOGRX")))
          | r.mask("CHEP4R", "STATTX", nak ^ valid));
  CHECK(r.port().in(0, 4, r.now()).hs == usb::handshake::nak);
  r.write(
      "CHEP4R", (r.read("CHEP4R") & ~r.mask("CHEP4R", "STATTX", 3) & ~r.mask("CHEP4R", "DTOGTX"))
                    | r.mask("CHEP4R", "DTOGRX"));
  auto const b = r.port().in(0, 4, r.now());
  CHECK(b.data == second);
  CHECK(b.pid == usb::data_pid::data1);
  CHECK(r.get("CHEP4R", "STATTX") == valid);
}

TEST_CASE(
    "the first double-buffered transaction after EPKIND or UTYPE changes NAKs, later ones keep "
    "VALID")
{
  rig const  r;
  auto const plain = [&] {
    return r.read("CHEP3R")
         & ~(r.mask("CHEP3R", "STATRX", 3)
             | r.mask("CHEP3R", "STATTX", 3)
             | r.mask("CHEP3R", "DTOGRX")
             | r.mask("CHEP3R", "DTOGTX"));
  };
  auto const release = [&] { r.write("CHEP3R", plain() | r.mask("CHEP3R", "DTOGTX")); };
  auto const rearm = [&] { r.write("CHEP3R", plain() | r.mask("CHEP3R", "STATRX", nak ^ valid)); };
  r.enumerate();
  r.rx_buffer(3, 0x200, 64, 0);
  r.rx_buffer(3, 0x240, 64, 1);
  r.endpoint(3, {.address = 3, .type = bulk, .stat_rx = valid, .kind = 1, .dtog_tx = 1});
  auto const data = sequence(8, 0x10);
  REQUIRE(r.port().out(0, 3, data, usb::data_pid::data0, r.now()) == usb::handshake::ack);
  CHECK(r.get("CHEP3R", "STATRX") == nak);
  rearm();
  release();
  REQUIRE(r.port().out(0, 3, data, usb::data_pid::data1, r.now()) == usb::handshake::ack);
  CHECK(r.get("CHEP3R", "STATRX") == valid);
  release();
  REQUIRE(r.port().out(0, 3, data, usb::data_pid::data0, r.now()) == usb::handshake::ack);
  CHECK(r.get("CHEP3R", "STATRX") == valid);

  r.write("CHEP3R", plain() & ~r.mask("CHEP3R", "EPKIND"));
  r.write("CHEP3R", plain() | r.mask("CHEP3R", "EPKIND"));
  release();
  REQUIRE(r.port().out(0, 3, data, usb::data_pid::data1, r.now()) == usb::handshake::ack);
  CHECK(r.get("CHEP3R", "STATRX") == nak);
  rearm();
  release();
  REQUIRE(r.port().out(0, 3, data, usb::data_pid::data0, r.now()) == usb::handshake::ack);
  CHECK(r.get("CHEP3R", "STATRX") == valid);

  r.write("CHEP3R", plain() | r.mask("CHEP3R", "UTYPE", 3));
  r.write("CHEP3R", plain() & ~r.mask("CHEP3R", "UTYPE", 3));
  release();
  REQUIRE(r.port().out(0, 3, data, usb::data_pid::data1, r.now()) == usb::handshake::ack);
  CHECK(r.get("CHEP3R", "STATRX") == nak);
}

TEST_CASE("ISTR.RST_DCON set holds every token unanswered until the firmware clears it")
{
  rig const r;
  r.enumerate();
  r.port().bus_reset(r.now());
  r.run_for(20 * us);
  REQUIRE(r.get("ISTR", "RST_DCON") == 1);
  r.write("DADDR", r.mask("DADDR", "EF"));
  r.open_control();
  r.tx_buffer(1, 0x300, 0);
  r.endpoint(1, {.address = 1, .type = bulk, .stat_tx = valid});
  CHECK(r.port().setup(0, get_device, r.now()) == usb::handshake::none);
  CHECK(r.port().in(0, 1, r.now()).hs == usb::handshake::none);
  CHECK(r.port().out(0, 1, {}, usb::data_pid::data0, r.now()) == usb::handshake::none);
  r.write("ISTR", ~r.mask("ISTR", "RST_DCON"));
  CHECK(r.port().in(0, 1, r.now()).hs == usb::handshake::ack);
  CHECK(r.port().setup(0, get_device, r.now()) == usb::handshake::ack);
}

TEST_CASE("an isochronous IN and OUT swap buffers by DTOG, send DATA0 and keep STAT")
{
  rig const r;
  r.enumerate();
  auto const even = sequence(3, 0x30);
  auto const odd  = sequence(5, 0x50);
  r.put_bytes(0x400, even);
  r.put_bytes(0x440, odd);
  r.tx_buffer(5, 0x400, 3, 0);
  r.tx_buffer(5, 0x440, 5, 1);
  r.endpoint(5, {.address = 5, .type = iso, .stat_tx = valid});
  CHECK(r.port().in(0, 5, r.now()).data == even);
  auto const next = r.port().in(0, 5, r.now());
  CHECK(next.data == odd);
  CHECK(next.pid == usb::data_pid::data0);
  CHECK(r.get("CHEP5R", "STATTX") == valid);
  CHECK(r.get("CHEP5R", "VTTX") == 1);

  r.rx_buffer(6, 0x500, 4, 0);
  r.rx_buffer(6, 0x540, 32, 1);
  r.endpoint(6, {.address = 6, .type = iso, .stat_rx = valid});
  auto const in = sequence(6, 0x60);
  CHECK(r.port().out(0, 6, in, usb::data_pid::data0, r.now()) == usb::handshake::ack);
  CHECK(r.rx_count(6, 0) == 4);
  CHECK(r.port().out(0, 6, in, usb::data_pid::data0, r.now()) == usb::handshake::ack);
  CHECK(r.rx_count(6, 1) == 6);
  CHECK(r.bytes_at(0x540, 6) == in);
  CHECK(r.get("CHEP6R", "STATRX") == valid);
}

TEST_CASE("an isochronous endpoint with SBUF_ISO uses its single buffer")
{
  rig const r;
  r.enumerate();
  auto const data = sequence(4, 0x70);
  r.put_bytes(0x400, data);
  r.tx_buffer(5, 0x400, 4, 0);
  r.endpoint(5, {.address = 5, .type = iso, .stat_tx = valid, .kind = 1});
  CHECK(r.port().in(0, 5, r.now()).data == data);
  CHECK(r.port().in(0, 5, r.now()).data == data);
  CHECK(r.get("CHEP5R", "DTOGTX") == 0);
}

TEST_CASE("IDN names a double-buffered or isochronous endpoint before a lower plain one")
{
  rig const r;
  r.enumerate();
  r.tx_buffer(1, 0x300, 0);
  r.endpoint(1, {.address = 1, .type = bulk, .stat_tx = valid});
  r.tx_buffer(5, 0x400, 0);
  r.endpoint(5, {.address = 5, .type = iso, .stat_tx = valid});
  REQUIRE(r.port().in(0, 1, r.now()).hs == usb::handshake::ack);
  CHECK(r.get("ISTR", "IDN") == 1);
  REQUIRE(r.port().in(0, 5, r.now()).hs == usb::handshake::ack);
  CHECK(r.get("ISTR", "IDN") == 5);
  r.clear_flags(5);
  CHECK(r.get("ISTR", "IDN") == 1);
}

TEST_CASE(
    "SOF sets FN and LCK from the second one, suspend raises SUSP, resume WKUP and clears SUSPEN")
{
  rig const r;
  r.enumerate();
  r.port().start_of_frame(0x123, r.now());
  CHECK(r.get("FNR", "FN") == 0x123);
  CHECK(r.get("FNR", "LCK") == 0);
  r.port().start_of_frame(0x124, r.now());
  CHECK(r.get("FNR", "LCK") == 1);
  r.port().suspend(r.now());
  CHECK(r.get("ISTR", "SUSP") == 1);
  CHECK(r.get("FNR", "LCK") == 0);
  r.write("CNTR", r.mask("CNTR", "SUSPEN"));
  r.port().resume(r.now());
  CHECK(r.get("ISTR", "WKUP") == 1);
  CHECK(r.get("CNTR", "SUSPEN") == 0);
  CHECK(r.get("CNTR", "SUSPRDY") == 0);
}

TEST_CASE("a cable plugged back, a pull-up drop and an unplug within 3 ms never suspend")
{
  popcorn::periph::stm32::board board;
  board.usb_hosts.emplace("USB", popcorn::periph::stm32::usb_host_options{.attached = true});
  rig const r{board};
  r.power_up();
  r.write("BCDR", r.mask("BCDR", "DPPU_DPD"));
  REQUIRE(r.port().pulled_up());
  r.cable().plug(false);
  r.run_for(1000 * us);
  r.cable().plug(true);
  r.run_for(500 * us);
  r.write("BCDR", 0);
  REQUIRE_FALSE(r.port().pulled_up());
  r.cable().plug(false);
  r.run_for(4000 * us);
  CHECK(r.get("ISTR", "SUSP") == 0);
}

TEST_CASE("a snapshot after a SETUP restored into a fresh machine gives the same IN and host bytes")
{
  rig const r;
  r.enumerate();
  r.host->memory = {std::byte{1}, std::byte{2}, std::byte{3}};
  REQUIRE(r.port().setup(0, get_device, r.now()) == usb::handshake::ack);
  r.clear_flags(0);
  auto const reply = sequence(18, 3);
  r.put_bytes(0x40, reply);
  r.tx_buffer(0, 0x40, 18);
  r.endpoint(
      0, {.address = 0,
          .type    = control,
          .stat_tx = valid,
          .stat_rx = nak,
          .dtog_tx = 1,
          .dtog_rx = 1});
  auto const saved = r.f->target->snapshot();

  rig const other;
  other.port().attach(other.host);
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.host->memory == r.host->memory);
  CHECK(other.port().pulled_up());
  auto const restored = other.port().in(0, 0, other.now());
  auto const direct   = r.port().in(0, 0, r.now());
  CHECK(restored.hs == usb::handshake::ack);
  CHECK(restored.data == direct.data);
  CHECK(restored.pid == direct.pid);
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

void send_packet(rig const& r, scripted_firmware& fw)
{
  auto const size = std::min<std::size_t>(64, fw.pending.size() - fw.sent);
  r.put_bytes(0x40, std::span{fw.pending}.subspan(fw.sent, size));
  r.tx_buffer(0, 0x40, static_cast<std::uint32_t>(size));
  fw.sent += size;
  auto const word = r.read("CHEP0R");
  r.write(
      "CHEP0R", ((word
                  & ~(r.mask("CHEP0R", "STATRX", 3)
                      | r.mask("CHEP0R", "DTOGTX")
                      | r.mask("CHEP0R", "DTOGRX")))
                 ^ r.mask("CHEP0R", "STATTX", valid ^ r.get("CHEP0R", "STATTX")))
                    | r.mask("CHEP0R", "VTRX")
                    | r.mask("CHEP0R", "VTTX"));
}

void arm_rx(rig const& r)
{
  auto const word = r.read("CHEP0R");
  r.write(
      "CHEP0R", ((word
                  & ~(r.mask("CHEP0R", "STATTX", 3)
                      | r.mask("CHEP0R", "DTOGTX")
                      | r.mask("CHEP0R", "DTOGRX")))
                 ^ r.mask("CHEP0R", "STATRX", valid ^ r.get("CHEP0R", "STATRX")))
                    | r.mask("CHEP0R", "VTRX")
                    | r.mask("CHEP0R", "VTTX"));
}

void clear_ep(rig const& r, std::size_t n, std::string_view flag)
{
  auto const name = std::format("CHEP{}R", n);
  auto const word = r.read(name);
  r.write(
      name, (word
             & ~(r.mask(name, "STATTX", 3)
                 | r.mask(name, "STATRX", 3)
                 | r.mask(name, "DTOGTX")
                 | r.mask(name, "DTOGRX")
                 | r.mask(name, flag)))
                | (flag == "VTRX" ? r.mask(name, "VTTX") : r.mask(name, "VTRX")));
}

void service(rig const& r, scripted_firmware& fw)
{
  if(r.get("ISTR", "RST_DCON") != 0)
  {
    r.write("ISTR", ~r.mask("ISTR", "RST_DCON"));
    r.write("DADDR", r.mask("DADDR", "EF"));
    r.open_control();
  }
  while(r.get("ISTR", "CTR") != 0)
  {
    auto const n    = r.get("ISTR", "IDN");
    auto const name = std::format("CHEP{}R", n);
    if(r.get(name, "VTRX") != 0)
    {
      auto const setup = r.get(name, "SETUP") != 0;
      auto const count = r.rx_count(n);
      auto const bytes = r.bytes_at(0x80, count);
      clear_ep(r, n, "VTRX");
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
        else if(packet.request_type == 0 and packet.request == 5)
        {
          fw.address = static_cast<std::uint8_t>(packet.value);
          send_packet(r, fw);
        }
        else
        {
          send_packet(r, fw);
        }
      }
      else
      {
        arm_rx(r);
      }
    }
    if(r.get(name, "VTTX") != 0)
    {
      clear_ep(r, n, "VTTX");
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

void run_firmware(rig const& r, scripted_firmware& fw, std::function_ref<bool()> done)
{
  for(std::size_t slice = 0; slice < 20'000 and not done(); ++slice)
  {
    r.run_for(25 * us);
    service(r, fw);
  }
}
}

TEST_CASE("the virtual host enumerates the USB model run by a register-level firmware ")
{
  rig const                r;
  std::vector<std::string> warnings;
  usb::host_options        options;
  options.warn           = [&](std::string const& message) { warnings.push_back(message); };
  auto const        host = std::make_shared<usb::host>(r.port(), std::move(options));
  scripted_firmware fw;
  r.power_up();
  r.port().attach(host);
  r.write("BCDR", r.mask("BCDR", "DPPU_DPD"));
  CHECK(host->state() == usb::device_state::powered);
  run_firmware(r, fw, [&] { return host->state() == usb::device_state::configured; });
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
  r.f->target->model("USB")->save(model_state);
  CHECK_FALSE(std::ranges::search(model_state, host_state).empty());
}
