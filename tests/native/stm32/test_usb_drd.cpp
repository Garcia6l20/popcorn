#include "usb_drd_rig.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace
{
namespace core = popcorn::core;
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
  CHECK(lax.pma_word(0) == 0xbeef'0000u);
  CHECK(lax.read_at(pma + 3, 1) == 0xbe);
}

TEST_CASE("H573 board: a narrow packet memory write zeroes the other lanes, CPU and debugger")
{
  rig const r{false};
  for(auto const by : {popcorn::initiator::cpu, popcorn::initiator::debugger})
  {
    auto&       regs = r.f->target->peripheral("USB");
    auto* const usb  = r.f->target->model("USB");
    auto const  ctx  = popcorn::periph::access_ctx{.pc = 0, .now = popcorn::icount{0}, .by = by};
    r.set_pma_word(0, 0x1122'3344u);
    REQUIRE(usb->on_write(regs, pma + 2, 2, 0, 0xbeef, ctx).has_value());
    CHECK(r.pma_word(0) == 0xbeef'0000u);
    r.set_pma_word(0, 0x1122'3344u);
    REQUIRE(usb->on_write(regs, pma + 1, 1, 0, 0xaa, ctx).has_value());
    CHECK(r.pma_word(0) == 0x0000'aa00u);
    r.set_pma_word(0, 0x1122'3344u);
    CHECK(r.read_at(pma + 2, 2) == 0x1122);
    CHECK(r.read_at(pma + 1, 1) == 0x33);
  }
}

TEST_CASE("H573 board: a narrow CHEPnR or DADDR write zeroes the other lanes")
{
  rig const r;
  r.write("CHEP2R", r.mask("CHEP2R", "EA", 5) | r.mask("CHEP2R", "UTYPE", 1));
  REQUIRE(r.get("CHEP2R", "EA") == 5);
  r.write_at(r.reg("CHEP2R").offset + 1, r.mask("CHEP2R", "DTOGRX") >> 8, 1);
  CHECK(r.get("CHEP2R", "DTOGRX") == 1);
  CHECK(r.get("CHEP2R", "EA") == 0);
  CHECK(r.get("CHEP2R", "UTYPE") == 0);
  r.write("CHEP2R", r.mask("CHEP2R", "EA", 5));
  r.write_at(r.reg("CHEP2R").offset + 2, 0, 2);
  CHECK(r.get("CHEP2R", "EA") == 0);
  r.write("DADDR", r.mask("DADDR", "EF") | r.mask("DADDR", "ADD", 0x05));
  r.write_at(r.reg("DADDR").offset + 1, 0, 1);
  CHECK(r.read("DADDR") == 0);
}

TEST_CASE("a byte write of CHEP0R lane 0 clears VTRX in lane 1, as the zero-filled word does")
{
  rig const r;
  r.connect();
  r.open_control();
  REQUIRE(
      r.port().setup(
          0, {.request_type = 0x80, .request = 6, .value = 0x0100, .length = 18}, r.now())
      == usb::handshake::ack);
  REQUIRE(r.get("CHEP0R", "VTRX") == 1);
  r.write_at(r.reg("CHEP0R").offset, r.read("CHEP0R") & 0xffu, 1);
  CHECK(r.get("CHEP0R", "VTRX") == 0);
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

TEST_CASE(
    "H573 board, PC host: RST_DCON at SE0 end, every CHEPnR but VTRX and VTTX and DADDR "
    "cleared at SE0 start, inferred")
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
  CHECK(r.get("ISTR", "RST_DCON") == 0);
  r.run_for(12'000 * us);
  CHECK(r.get("ISTR", "RST_DCON") == 0);
  r.port().bus_reset_end(r.now());
  CHECK(r.get("ISTR", "RST_DCON") == 1);
}

TEST_CASE("H573 board: CNTR.USBRST set clears every CHEPnR but VTRX and VTTX, keeps DADDR")
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
  CHECK(r.read("DADDR") == (r.mask("DADDR", "EF") | r.mask("DADDR", "ADD", 0x55)));
  CHECK(r.get("ISTR", "RST_DCON") == 0);
  fill();
  r.write("CNTR", r.mask("CNTR", "USBRST") | r.mask("CNTR", "SOFM"));
  CHECK(regs.peek(r.reg("CHEP1R").offset) == 0xffff'ffffu);
  r.write("CNTR", 0);
  CHECK(regs.peek(r.reg("CHEP1R").offset) == 0xffff'ffffu);
  CHECK(r.get("DADDR", "ADD") == 0x55);
}

TEST_CASE("USBFSRST drops the pull-up, keeps the packet memory, and a stale reset end sets nothing")
{
  rig const r;
  r.connect();
  r.set_pma_word(0x100, 0xcafe'f00du);
  r.port().bus_reset(r.now());
  r.run_for(2 * us);
  rcc_reset_pulse(*r.f, "RCC_APB2RSTR", "USBFSRST");
  CHECK_FALSE(r.port().pulled_up());
  REQUIRE(r.host->changes.size() == 2);
  CHECK_FALSE(r.host->changes[1].first);
  CHECK(r.pma_word(0x100) == 0xcafe'f00du);
  r.connect();
  r.run_for(3 * us);
  r.port().bus_reset_end(r.now());
  CHECK(r.get("ISTR", "RST_DCON") == 0);
  r.port().bus_reset(r.now());
  r.run_for(7 * us);
  CHECK(r.get("ISTR", "RST_DCON") == 0);
  r.port().bus_reset_end(r.now());
  CHECK(r.get("ISTR", "RST_DCON") == 1);
}

TEST_CASE(
    "a reset end after a USBFSRST pulse inside SE0 raises no RST_DCON and the frame timer "
    "runs from the pull-up")
{
  constexpr std::uint64_t ms = 1000 * us;
  rig const               r;
  r.connect();
  r.port().bus_reset(r.now());
  r.run_for(2 * us);
  rcc_reset_pulse(*r.f, "RCC_APB2RSTR", "USBFSRST");
  r.connect();
  auto const pulled = r.now().ps;
  r.run_for(20 * us);
  r.port().bus_reset_end(r.now());
  CHECK(r.get("ISTR", "RST_DCON") == 0);
  r.run_for(pulled + ms - us - r.now().ps);
  CHECK(r.get("ISTR", "ESOF") == 0);
  r.run_for(2 * us);
  CHECK(r.get("ISTR", "ESOF") == 1);
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

TEST_CASE("H573 board: SUSPRDY reads SUSPEN, a written 0 keeps SUSPEN, a bus reset clears it")
{
  rig const r;
  r.connect();
  r.write("CNTR", r.mask("CNTR", "SUSPEN"));
  CHECK(r.get("CNTR", "SUSPRDY") == 1);
  r.write("CNTR", 0);
  CHECK(r.get("CNTR", "SUSPEN") == 1);
  CHECK(r.get("CNTR", "SUSPRDY") == 1);
  r.port().bus_reset(r.now());
  CHECK(r.get("CNTR", "SUSPEN") == 0);
  CHECK(r.get("CNTR", "SUSPRDY") == 0);
}

TEST_CASE(
    "H573 board: SUSPEN stops the frame timer and clears LSOF, CNTR reads 0xC, a written 0 keeps "
    "SUSPEN")
{
  constexpr std::uint64_t ms = 1000 * us;
  rig const               r;
  r.power_up();
  r.write("BCDR", r.mask("BCDR", "DPPU_DPD"));
  r.run_for(3 * ms + us);
  REQUIRE(r.get("ISTR", "SUSP") == 1);
  REQUIRE(r.get("FNR", "LSOF") == 3);
  r.write("ISTR", 0);
  r.write("CNTR", r.mask("CNTR", "SUSPEN"));
  CHECK(r.read("CNTR") == 0xC);
  CHECK(r.get("FNR", "LSOF") == 0);
  r.write("CNTR", 0);
  CHECK(r.read("CNTR") == 0xC);
  r.run_for(20 * ms);
  CHECK(r.read("ISTR") == 0);
  CHECK(r.get("FNR", "LSOF") == 0);
}

TEST_CASE(
    "a bus reset or a SOF while SUSPEN is set raises WKUP, clears SUSPEN and SUSPRDY and runs the "
    "frame timer again")
{
  constexpr std::uint64_t ms = 1000 * us;
  for(auto const sof : {false, true})
  {
    for(auto const suspended : {false, true})
    {
      CAPTURE(sof);
      CAPTURE(suspended);
      rig const r;
      r.connect();
      r.port().start_of_frame(1, r.now());
      r.run_for(4 * ms);
      REQUIRE(r.get("ISTR", "SUSP") == 1);
      r.write("ISTR", 0);
      if(suspended)
      {
        r.write("CNTR", r.mask("CNTR", "SUSPEN"));
        REQUIRE(r.get("CNTR", "SUSPRDY") == 1);
      }
      auto const woken = r.now().ps;
      if(sof)
      {
        r.port().start_of_frame(2, r.now());
      }
      else
      {
        r.port().bus_reset(r.now());
      }
      CHECK(r.get("ISTR", "WKUP") == (suspended ? 1u : 0u));
      CHECK(r.get("CNTR", "SUSPEN") == 0);
      CHECK(r.get("CNTR", "SUSPRDY") == 0);
      if(not sof)
      {
        r.run_for(12 * ms);
        CHECK(r.get("ISTR", "ESOF") == 0);
        r.port().bus_reset_end(r.now());
      }
      auto const next = woken + (sof ? ms + us : 13 * ms);
      r.run_for(next - us - r.now().ps);
      CHECK(r.get("ISTR", "ESOF") == 0);
      r.run_for(2 * us);
      CHECK(r.get("ISTR", "ESOF") == 1);
    }
  }
}

TEST_CASE(
    "H573 board, PC host: LCK sets at the second SOF after SE0 ends with RST_DCON pending or "
    "cleared and with WKUP cleared during SE0, and reads 0 while WKUP is set")
{
  constexpr std::uint64_t ms = 1000 * us;
  enum class leg : std::uint8_t
  {
    flags_cleared,
    reset_pending,
    wake_kept,
    wake_cleared,
  };
  for(auto const l : {leg::flags_cleared, leg::reset_pending, leg::wake_kept, leg::wake_cleared})
  {
    CAPTURE(static_cast<int>(l));
    auto const suspended = l == leg::wake_kept or l == leg::wake_cleared;
    rig const  r;
    r.connect();
    r.run_for(4 * ms);
    REQUIRE(r.get("ISTR", "SUSP") == 1);
    if(suspended)
    {
      r.write("CNTR", r.mask("CNTR", "SUSPEN"));
    }
    r.write("ISTR", 0);
    r.port().bus_reset(r.now());
    REQUIRE(r.get("ISTR", "WKUP") == (suspended ? 1u : 0u));
    r.run_for(ms);
    if(l == leg::wake_cleared)
    {
      r.write("ISTR", ~r.mask("ISTR", "WKUP"));
    }
    r.run_for(11 * ms);
    r.port().bus_reset_end(r.now());
    REQUIRE(r.get("ISTR", "RST_DCON") == 1);
    if(l == leg::flags_cleared or l == leg::wake_cleared)
    {
      r.write("ISTR", ~r.mask("ISTR", "RST_DCON"));
    }
    r.port().start_of_frame(10, r.now());
    CHECK(r.get("FNR", "LCK") == 0);
    r.run_for(ms);
    r.port().start_of_frame(11, r.now());
    if(l != leg::wake_kept)
    {
      CHECK(r.get("FNR", "LCK") == 1);
      CHECK(r.get("ISTR", "RST_DCON") == (l == leg::reset_pending ? 1u : 0u));
      continue;
    }
    for(std::uint16_t frame = 12; frame < 312; ++frame)
    {
      r.run_for(ms);
      r.port().start_of_frame(frame, r.now());
    }
    CHECK(r.get("FNR", "FN") == 311);
    CHECK(r.get("FNR", "LCK") == 0);
    CHECK(r.get("ISTR", "WKUP") == 1);
    CHECK(r.get("ISTR", "RST_DCON") == 1);
  }
}

TEST_CASE(
    "a SOF while WKUP is set starts the LCK count again, inferred: LCK sets at the second SOF "
    "after WKUP is cleared, and a SOF under SUSPEN clears a set LCK")
{
  constexpr std::uint64_t ms = 1000 * us;
  rig const               r;
  r.connect();
  r.run_for(4 * ms);
  r.write("CNTR", r.mask("CNTR", "SUSPEN"));
  r.write("ISTR", 0);
  r.port().bus_reset(r.now());
  r.run_for(12 * ms);
  r.port().bus_reset_end(r.now());
  r.write("ISTR", ~r.mask("ISTR", "RST_DCON"));
  REQUIRE(r.get("ISTR", "WKUP") == 1);
  for(std::uint16_t frame = 1; frame <= 5; ++frame)
  {
    r.port().start_of_frame(frame, r.now());
    r.run_for(ms);
  }
  CHECK(r.get("FNR", "LCK") == 0);
  r.write("ISTR", ~r.mask("ISTR", "WKUP"));
  r.port().start_of_frame(6, r.now());
  CHECK(r.get("FNR", "LCK") == 0);
  r.run_for(ms);
  r.port().start_of_frame(7, r.now());
  CHECK(r.get("FNR", "LCK") == 1);

  r.write("CNTR", r.mask("CNTR", "SUSPEN"));
  r.run_for(ms);
  r.port().start_of_frame(8, r.now());
  CHECK(r.get("ISTR", "WKUP") == 1);
  CHECK(r.get("FNR", "LCK") == 0);
}

TEST_CASE(
    "a snapshot with the frame timer running restores the count, a CNTR write after the restore "
    "keeps it, SUSP at the third ESOF")
{
  constexpr std::uint64_t ms = 1000 * us;
  rig const               r;
  r.power_up();
  r.write("BCDR", r.mask("BCDR", "DPPU_DPD"));
  auto const pulled = r.now().ps;
  r.run_for(2 * ms + us);
  REQUIRE(r.get("FNR", "LSOF") == 2);
  REQUIRE(r.get("ISTR", "SUSP") == 0);
  auto const saved = r.f->target->snapshot();

  rig const other;
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  auto const at = [&](std::uint64_t ps) { other.run_for(pulled + ps - other.now().ps); };
  CHECK(other.get("FNR", "LSOF") == 2);
  at(5 * ms / 2);
  other.write("CNTR", other.read("CNTR"));
  at(3 * ms - us);
  CHECK(other.get("ISTR", "SUSP") == 0);
  at(3 * ms + us);
  CHECK(other.get("ISTR", "SUSP") == 1);
  CHECK(other.get("FNR", "LSOF") == 3);
}

TEST_CASE(
    "a snapshot inside SE0 restores the bus reset: RST_DCON at its end, "
    "the next ESOF 1 ms later")
{
  constexpr std::uint64_t ms = 1000 * us;
  rig const               r;
  r.connect();
  r.port().bus_reset(r.now());
  auto const start = r.now().ps;
  r.run_for(5 * ms);
  auto const saved = r.f->target->snapshot();

  rig const other;
  other.port().attach(other.host);
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  REQUIRE(other.port().pulled_up());
  auto const at = [&](std::uint64_t ps) { other.run_for(start + ps - other.now().ps); };
  at(10 * ms);
  CHECK(other.get("ISTR", "RST_DCON") == 0);
  other.port().bus_reset_end(other.now());
  CHECK(other.get("ISTR", "RST_DCON") == 1);
  at(11 * ms - us);
  CHECK(other.get("ISTR", "ESOF") == 0);
  at(11 * ms + us);
  CHECK(other.get("ISTR", "ESOF") == 1);
}

TEST_CASE(
    "H573 board: with no host, DPPU_DPD 1 raises ESOF and counts LSOF every 1 ms and raises SUSP "
    "at the third and every 4 ms after, DPPU_DPD 0 stops both")
{
  constexpr std::uint64_t ms = 1000 * us;
  rig const               r;
  r.power_up();
  r.run_for(5 * ms);
  CHECK(r.read("ISTR") == 0);
  CHECK(r.get("FNR", "LSOF") == 0);
  r.write("BCDR", r.mask("BCDR", "DPPU_DPD"));
  auto const pulled = r.now().ps;
  auto const at     = [&](std::uint64_t ps) { r.run_for(pulled + ps - r.now().ps); };
  at(ms - us);
  CHECK(r.get("ISTR", "ESOF") == 0);
  at(ms + us);
  CHECK(r.get("ISTR", "ESOF") == 1);
  CHECK(r.get("FNR", "LSOF") == 1);
  CHECK(r.get("ISTR", "SUSP") == 0);
  at(2 * ms + us);
  CHECK(r.get("FNR", "LSOF") == 2);
  CHECK(r.get("ISTR", "SUSP") == 0);
  at(3 * ms - us);
  CHECK(r.get("ISTR", "SUSP") == 0);
  at(3 * ms + us);
  CHECK(r.get("FNR", "LSOF") == 3);
  CHECK(r.get("ISTR", "SUSP") == 1);
  at(4 * ms + us);
  CHECK(r.get("FNR", "LSOF") == 0);
  CHECK(r.get("FNR", "RXDP") == 1);
  CHECK(r.get("FNR", "RXDM") == 1);
  r.write("ISTR", 0);
  at(7 * ms - us);
  CHECK(r.get("ISTR", "SUSP") == 0);
  at(7 * ms + us);
  CHECK(r.get("FNR", "LSOF") == 3);
  CHECK(r.get("ISTR", "SUSP") == 1);
  r.write("BCDR", 0);
  r.write("ISTR", 0);
  r.run_for(5 * ms);
  CHECK(r.read("ISTR") == 0);
  CHECK(r.get("FNR", "LSOF") == 3);
}

TEST_CASE(
    "FNR.RXDP and RXDM read 1 and 1 with no host as measured on an H573, SE0 with a host and no "
    "pull-up, J with the pull-up, USB 2.0 7.1.5.1 and 7.1.7.1")
{
  rig const r;
  CHECK(r.get("FNR", "RXDP") == 1);
  CHECK(r.get("FNR", "RXDM") == 1);
  r.power_up();
  r.port().attach(r.host);
  CHECK(r.get("FNR", "RXDP") == 0);
  CHECK(r.get("FNR", "RXDM") == 0);
  r.connect();
  CHECK(r.get("FNR", "RXDP") == 1);
  CHECK(r.get("FNR", "RXDM") == 0);
  r.port().attach(nullptr);
  CHECK(r.get("FNR", "RXDP") == 1);
  CHECK(r.get("FNR", "RXDM") == 1);
}
