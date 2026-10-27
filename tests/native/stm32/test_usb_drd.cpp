#include "usb_drd_rig.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace
{
using namespace popcorn::testing::usb_drd;
using popcorn::testing::h573;
using popcorn::testing::queued_event;
using popcorn::testing::rcc_reset_pulse;
using popcorn::testing::saved_integer;

constexpr std::uint32_t usb_base        = 0x4001'6000;
constexpr std::uint32_t usb_secure_base = 0x5001'6000;
}

TEST_CASE(
    "the packet memory at 0x400 to 0xBFF is served through the bus and kept by a software reset")
{
  rig const r;
  REQUIRE(r.f->target->mem().write(usb_base + 0x400, 4, 0x1122'3344u));
  REQUIRE(r.f->target->mem().write(usb_base + 0xbfc, 4, 0xa5a5'5a5au));
  CHECK(r.pma_word(0) == 0x1122'3344u);
  CHECK(r.f->target->mem().read(usb_base + 0xbfc, 4) == 0xa5a5'5a5au);
  r.f->target->reset(popcorn::reset_kind::software);
  CHECK(r.pma_word(0) == 0x1122'3344u);
  r.f->target->reset(popcorn::reset_kind::power_on);
  CHECK(r.pma_word(0) == 0);
  CHECK(r.pma_word(0x7fc) == 0);
}

TEST_CASE("the packet memory written through the SEC_USB alias reads back through USB")
{
  rig const r;
  REQUIRE(r.f->target->mem().write(usb_secure_base + 0x500, 4, 0x5eca'11a5u));
  CHECK(r.f->target->mem().read(usb_base + 0x500, 4) == 0x5eca'11a5u);
  CHECK(r.pma_word(0x100) == 0x5eca'11a5u);
  REQUIRE(r.f->target->mem().write(usb_base + 0x504, 4, 0x0bad'cafeu));
  CHECK(r.f->target->mem().read(usb_secure_base + 0x504, 4) == 0x0bad'cafeu);
}

TEST_CASE("a CPU byte access of the packet memory is a model error under strict peripherals")
{
  rig const r;
  r.set_pma_word(0, 0x1122'3344u);
  auto&       regs = r.f->target->peripheral("USB");
  auto* const usb  = r.f->target->model("USB");
  CHECK_THROWS_AS(
      static_cast<void>(usb->on_read(regs, pma + 1, 1, {.pc = 0, .now = popcorn::icount{0}})),
      popcorn::model_error);
  auto const debugger = usb->on_read(
      regs, pma + 1, 1, {.pc = 0, .now = popcorn::icount{0}, .by = popcorn::initiator::debugger});
  REQUIRE(debugger.has_value());
  CHECK(*debugger == 0x33);

  rig const lax{false};
  lax.set_pma_word(0, 0x1122'3344u);
  lax.write_at(pma + 2, 0xbeef, 2);
  CHECK(lax.pma_word(0) == 0xbeef'3344u);
  CHECK(lax.read_at(pma + 3, 1) == 0xbe);
}

TEST_CASE("VTRX, VTTX and NAK clear on a written 0 and keep on a written 1")
{
  rig const r;
  r.connect();
  r.open_control();
  REQUIRE(
      r.port().setup(
          0, {.request_type = 0x80, .request = 6, .value = 0x0100, .length = 18}, r.now())
      == usb::handshake::ack);
  REQUIRE(r.get("CHEP0R", "VTRX") == 1);
  r.write("CHEP0R", r.read("CHEP0R"));
  CHECK(r.get("CHEP0R", "VTRX") == 1);
  CHECK(r.get("CHEP0R", "SETUP") == 1);
  r.write("CHEP0R", r.read("CHEP0R") & ~r.mask("CHEP0R", "VTRX"));
  CHECK(r.get("CHEP0R", "VTRX") == 0);
  r.write("CHEP0R", r.read("CHEP0R") | r.mask("CHEP0R", "VTRX"));
  CHECK(r.get("CHEP0R", "VTRX") == 0);
}

TEST_CASE("DTOGRX and DTOGTX toggle on a written 1 and keep on a written 0")
{
  rig const r;
  r.write("CHEP1R", r.mask("CHEP1R", "DTOGRX"));
  CHECK(r.get("CHEP1R", "DTOGRX") == 1);
  r.write("CHEP1R", 0);
  CHECK(r.get("CHEP1R", "DTOGRX") == 1);
  r.write("CHEP1R", r.mask("CHEP1R", "DTOGRX") | r.mask("CHEP1R", "DTOGTX"));
  CHECK(r.get("CHEP1R", "DTOGRX") == 0);
  CHECK(r.get("CHEP1R", "DTOGTX") == 1);
}

TEST_CASE("STATTX and STATRX toggle per written bit and read back")
{
  rig const r;
  r.write("CHEP2R", r.mask("CHEP2R", "STATTX", 3));
  CHECK(r.get("CHEP2R", "STATTX") == valid);
  r.write("CHEP2R", r.mask("CHEP2R", "STATTX", 1));
  CHECK(r.get("CHEP2R", "STATTX") == nak);
  r.write("CHEP2R", r.mask("CHEP2R", "STATRX", 2));
  CHECK(r.get("CHEP2R", "STATRX") == nak);
  CHECK((r.read("CHEP2R") & r.mask("CHEP2R", "STATRX", 3)) == r.mask("CHEP2R", "STATRX", nak));
}

TEST_CASE("SETUP ignores writes and the other fields take the written value")
{
  rig const r;
  r.write(
      "CHEP3R", r.mask("CHEP3R", "SETUP")
                    | r.mask("CHEP3R", "EA", 5)
                    | r.mask("CHEP3R", "UTYPE", 3)
                    | r.mask("CHEP3R", "EPKIND"));
  CHECK(r.get("CHEP3R", "SETUP") == 0);
  CHECK(r.get("CHEP3R", "EA") == 5);
  CHECK(r.get("CHEP3R", "UTYPE") == 3);
  CHECK(r.get("CHEP3R", "EPKIND") == 1);
}

TEST_CASE("a byte write to CHEPnR toggles only the bits of its lane")
{
  rig const r;
  r.write("CHEP4R", r.mask("CHEP4R", "DTOGTX"));
  REQUIRE(r.get("CHEP4R", "DTOGTX") == 1);
  r.write_at(r.reg("CHEP4R").offset + 1, r.mask("CHEP4R", "DTOGRX") >> 8, 1);
  CHECK(r.get("CHEP4R", "DTOGRX") == 1);
  CHECK(r.get("CHEP4R", "DTOGTX") == 1);
}

TEST_CASE("ISTR flags clear on a written 0, CTR, DIR and IDN ignore writes")
{
  rig const r;
  r.connect();
  r.port().start_of_frame(7, r.now());
  r.port().suspend(r.now());
  REQUIRE(r.get("ISTR", "SOF") == 1);
  REQUIRE(r.get("ISTR", "SUSP") == 1);
  r.write("ISTR", ~r.mask("ISTR", "SOF"));
  CHECK(r.get("ISTR", "SOF") == 0);
  CHECK(r.get("ISTR", "SUSP") == 1);
  CHECK(r.get("ISTR", "CTR") == 0);
  CHECK(r.get("ISTR", "IDN") == 0);
}

TEST_CASE("IRQ 74 follows an ISTR flag and its CNTR mask")
{
  rig const r;
  r.connect();
  r.port().start_of_frame(1, r.now());
  REQUIRE(r.get("ISTR", "SOF") == 1);
  CHECK_FALSE(r.pending());
  r.write("CNTR", r.mask("CNTR", "SOFM"));
  CHECK(r.pending());
}

TEST_CASE("a description without the CHEPnR bit kinds fails the construction")
{
  auto part = h573();
  for(auto& unit : part.peripherals)
  {
    if(unit.name != "USB")
    {
      continue;
    }
    for(auto& reg : unit.registers)
    {
      if(reg.name == "CHEP5R")
      {
        for(auto& field : reg.fields)
        {
          if(field.name == "DTOGRX")
          {
            field.on_write = popcorn::write_effect::modify;
          }
        }
      }
    }
  }
  CHECK_THROWS_AS(rig(true, part), popcorn::config_error);
}

TEST_CASE("a USB peripheral too small for the packet memory fails the construction")
{
  auto part = h573();
  for(auto& unit : part.peripherals)
  {
    if(unit.name == "USB")
    {
      unit.size = 0x400;
    }
  }
  CHECK_THROWS_AS(rig(true, part), popcorn::config_error);
}

TEST_CASE("a bus reset clears every CHEPnR but VTRX and VTTX and DADDR, RST_DCON 10 us later")
{
  rig const r;
  r.connect();
  r.open_control();
  REQUIRE(
      r.port().setup(
          0, {.request_type = 0x80, .request = 6, .value = 0x0100, .length = 18}, r.now())
      == usb::handshake::ack);
  r.write("DADDR", r.mask("DADDR", "EF") | r.mask("DADDR", "ADD", 9));
  r.endpoint(1, {.address = 1, .type = bulk, .stat_tx = valid, .dtog_tx = 1});
  r.write("CHEP1R", r.read("CHEP1R") | r.mask("CHEP1R", "VTTX"));
  r.port().bus_reset(r.now());
  CHECK(r.read("CHEP0R") == r.mask("CHEP0R", "VTRX"));
  CHECK(r.read("CHEP1R") == 0);
  CHECK(r.read("DADDR") == 0);
  r.run_for(9 * us);
  CHECK(r.get("ISTR", "RST_DCON") == 0);
  r.run_for(2 * us);
  CHECK(r.get("ISTR", "RST_DCON") == 1);
}

TEST_CASE("CNTR.USBRST set clears every CHEPnR but VTRX and VTTX and DADDR, with no RST_DCON")
{
  rig const r;
  r.connect();
  auto&      regs = r.f->target->peripheral("USB");
  auto const fill = [&] {
    for(std::size_t n = 0; n < 8; ++n)
    {
      regs.poke(r.reg(std::format("CHEP{}R", n)).offset, 0xffff'ffffu);
    }
    regs.poke(r.reg("DADDR").offset, r.mask("DADDR", "EF") | r.mask("DADDR", "ADD", 0x55));
  };
  fill();
  r.write("CNTR", r.mask("CNTR", "USBRST"));
  for(std::size_t n = 0; n < 8; ++n)
  {
    auto const name = std::format("CHEP{}R", n);
    CAPTURE(name);
    CHECK(regs.peek(r.reg(name).offset) == (r.mask(name, "VTRX") | r.mask(name, "VTTX")));
  }
  CHECK(r.read("DADDR") == 0);
  CHECK(r.get("ISTR", "RST_DCON") == 0);
  fill();
  r.write("CNTR", r.mask("CNTR", "USBRST") | r.mask("CNTR", "SOFM"));
  CHECK(regs.peek(r.reg("CHEP1R").offset) == 0xffff'ffffu);
  r.write("CNTR", 0);
  CHECK(regs.peek(r.reg("CHEP1R").offset) == 0xffff'ffffu);
  CHECK(r.get("DADDR", "ADD") == 0x55);
}

TEST_CASE("USBFSRST drops the pull-up, keeps the packet memory, and a stale reset end never fires")
{
  rig const r;
  r.connect();
  r.set_pma_word(0x100, 0xcafe'f00du);
  r.port().bus_reset(r.now());
  auto const stale = saved_integer(*r.f, "USB", "generation.0");
  REQUIRE(stale.has_value());
  REQUIRE(queued_event(*r.f, "USB", 0, static_cast<std::uint64_t>(*stale)));
  r.run_for(2 * us);
  rcc_reset_pulse(*r.f, "RCC_APB2RSTR", "USBFSRST");
  CHECK_FALSE(r.port().pulled_up());
  REQUIRE(r.host->changes.size() == 2);
  CHECK_FALSE(r.host->changes[1].first);
  CHECK(r.pma_word(0x100) == 0xcafe'f00du);
  r.connect();
  r.run_for(3 * us);
  r.port().bus_reset(r.now());
  r.run_for(7 * us);
  CHECK(r.get("ISTR", "RST_DCON") == 0);
  r.run_for(4 * us);
  CHECK(r.get("ISTR", "RST_DCON") == 1);
}

TEST_CASE("a reset end pending at USBFSRST never raises RST_DCON after the pulse")
{
  rig const r;
  r.connect();
  r.port().bus_reset(r.now());
  r.run_for(2 * us);
  rcc_reset_pulse(*r.f, "RCC_APB2RSTR", "USBFSRST");
  r.run_for(20 * us);
  CHECK(r.get("ISTR", "RST_DCON") == 0);
}

TEST_CASE("host mode, LPM and battery charging detection are model errors under strict peripherals")
{
  rig const r;
  CHECK_THROWS_AS(r.write_unchecked("CNTR", r.mask("CNTR", "HOST")), popcorn::model_error);
  CHECK_THROWS_AS(r.write_unchecked("LPMCSR", r.mask("LPMCSR", "LPMEN")), popcorn::model_error);
  CHECK_THROWS_AS(r.write_unchecked("BCDR", r.mask("BCDR", "BCDEN")), popcorn::model_error);
  CHECK_THROWS_AS(r.write_unchecked("CNTR", r.mask("CNTR", "L2RES")), popcorn::model_error);
  rig const lax{false};
  lax.write("CNTR", lax.mask("CNTR", "HOST"));
  CHECK(lax.get("CNTR", "HOST") == 1);
}

TEST_CASE("SUSPRDY reads SUSPEN")
{
  rig const r;
  r.write("CNTR", r.mask("CNTR", "SUSPEN"));
  CHECK(r.get("CNTR", "SUSPRDY") == 1);
  r.write("CNTR", 0);
  CHECK(r.get("CNTR", "SUSPRDY") == 0);
}
