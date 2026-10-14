#include "dma.hpp"
#include "dma_signal.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace periph = popcorn::periph;
namespace stm32  = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;
using popcorn::testing::u575;

constexpr popcorn::address source      = 0x2800'0000;
constexpr popcorn::address destination = 0x2800'1000;
constexpr popcorn::address node        = 0x2800'2000;
constexpr popcorn::address sram1       = 0x2000'0000;
constexpr popcorn::address lpuart1_tdr = 0x4600'2428;
constexpr std::uint32_t    nvic_ispr3  = 0xE000'E20C;
constexpr std::uint32_t    lpdma_irq   = 114;
constexpr std::uint64_t    ps_per_us   = 1'000'000;

struct call
{
  stm32::dma_controller const* by{};
  std::uint32_t                request{};
  bool                         carried{};

  friend auto operator==(call const&, call const&) -> bool = default;
};

class lpuart final : public periph::model, public stm32::dma_requester
{
public:
  explicit lpuart(periph::model_context& ctx) : tx{ctx.peers, "lpuart1_tx_dma"}
  {}

  stm32::dma_signal tx;
  std::vector<call> acknowledges;
  std::vector<call> completions;

  void transfer_completed(stm32::dma_controller const& by, std::uint32_t request) override
  {
    completions.push_back({.by = &by, .request = request, .carried = tx.carries(by, request)});
  }

  void acknowledged(stm32::dma_controller const& by, std::uint32_t request) override
  {
    acknowledges.push_back({.by = &by, .request = request, .carried = tx.carries(by, request)});
  }

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }
  void reset(periph::register_file&, popcorn::reset_kind) override
  {
    tx.resolve();
  }
  auto on_read(periph::register_file&, std::uint32_t, std::uint8_t, periph::access_ctx)
      -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    return 0u;
  }
  auto on_write(
      periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t,
      periph::access_ctx) -> std::expected<void, popcorn::bus_error> override
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

auto pattern(std::uint32_t index) -> std::uint32_t
{
  return (index * 11u + 1u) & 0xffu;
}

auto machine(bool strict = false) -> stm32_fixture
{
  auto options               = popcorn::testing::stm32_options();
  options.strict_peripherals = strict;
  options.models.emplace(
      "LPUART1",
      [](periph::model_context& ctx, popcorn::peripheral_desc const&)
          -> std::shared_ptr<periph::model> { return std::make_shared<lpuart>(ctx); });
  stm32_fixture f{u575(), "rcc_ll", std::move(options)};
  f.set("pc", sym("rcc_ll", "idle"));
  for(std::uint32_t index = 0; index < 256; ++index)
  {
    REQUIRE(f.target->mem().write(source + index, 1, pattern(index)));
    REQUIRE(f.target->mem().write(destination + index, 1, 0));
  }
  return f;
}

auto device(stm32_fixture const& f) -> lpuart&
{
  auto* const found = f.model_of<lpuart>("LPUART1");
  REQUIRE(found != nullptr);
  return *found;
}

auto controller(stm32_fixture const& f, std::string_view name) -> stm32::dma_controller&
{
  auto* const found = dynamic_cast<stm32::dma_controller*>(f.target->model(name));
  REQUIRE(found != nullptr);
  return *found;
}

auto lsb(std::string_view peripheral, std::string_view reg, std::string_view field) -> std::uint32_t
{
  auto const& unit =
      *std::ranges::find(u575().peripherals, peripheral, &popcorn::peripheral_desc::name);
  auto const found = popcorn::find_register(unit, reg);
  REQUIRE(found != nullptr);
  auto const bits = std::ranges::find(found->fields, field, &popcorn::field_desc::name);
  REQUIRE(bits != found->fields.end());
  return bits->lsb;
}

auto bit(std::size_t ch, std::string_view reg, std::string_view field) -> std::uint32_t
{
  return std::uint32_t{1} << lsb("LPDMA1", std::format("LPDMA_C{}{}", ch, reg), field);
}

auto offset_of(stm32_fixture const& f, std::string_view peripheral, std::string_view reg)
    -> std::uint32_t
{
  auto const& regs  = f.target->peripheral(peripheral);
  auto const  found = popcorn::find_register(regs.description(), reg);
  REQUIRE(found != nullptr);
  return found->offset;
}

void write(
    stm32_fixture const& f,
    std::string_view     peripheral,
    std::string_view     reg,
    std::uint32_t        value)
{
  auto&      regs = f.target->peripheral(peripheral);
  auto const at   = offset_of(f, peripheral, reg);
  REQUIRE(f.target->model(peripheral)
              ->on_write(regs, at, 4, regs.peek(at), value, {.pc = 0, .now = icount{0}})
              .has_value());
}

void write(stm32_fixture const& f, std::size_t ch, std::string_view reg, std::uint32_t value)
{
  write(f, "LPDMA1", std::format("LPDMA_C{}{}", ch, reg), value);
}

auto read(stm32_fixture const& f, std::size_t ch, std::string_view reg) -> std::uint32_t
{
  auto&      regs  = f.target->peripheral("LPDMA1");
  auto const value = f.target->model("LPDMA1")->on_read(
      regs, offset_of(f, "LPDMA1", std::format("LPDMA_C{}{}", ch, reg)), 4,
      {.pc = 0, .now = icount{0}});
  REQUIRE(value.has_value());
  return *value;
}

auto field(stm32_fixture const& f, std::size_t ch, std::string_view reg, std::string_view name)
    -> std::uint32_t
{
  return f.field("LPDMA1", std::format("LPDMA_C{}{}", ch, reg), name);
}

void idle_for(stm32_fixture const& f, std::chrono::nanoseconds span)
{
  auto const now = f.target->virtual_clock().time();
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{1'000'000'000},
                     .until_time = vtime{now.ps + static_cast<std::uint64_t>(span.count()) * 1000}})
          .reason
      == stop_reason::time);
}

void later(stm32_fixture const& f, std::uint64_t ps)
{
  auto const until = vtime{f.target->virtual_clock().time().ps + ps};
  for(;;)
  {
    auto const result = f.run(
        {.max_instructions = icount{1'000'000'000},
         .wall_budget      = std::chrono::seconds{60},
         .until_time       = until});
    if(result.stopped)
    {
      REQUIRE(result.stopped->reason == stop_reason::time);
      return;
    }
  }
}

auto bytes_at(stm32_fixture const& f, popcorn::address at, std::uint32_t length)
    -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(std::uint32_t index = 0; index < length; ++index)
  {
    auto const byte = f.target->mem().read(at + index, 1);
    REQUIRE(byte.has_value());
    out.push_back(static_cast<std::uint32_t>(*byte));
  }
  return out;
}

auto expected(std::uint32_t from, std::uint32_t length) -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(std::uint32_t index = from; index < from + length; ++index)
  {
    out.push_back(pattern(index));
  }
  return out;
}

auto pending(stm32_fixture const& f, std::size_t ch) -> bool
{
  auto const word = f.target->mem().read(nvic_ispr3, 4);
  REQUIRE(word.has_value());
  return ((*word >> (lpdma_irq - 96 + ch)) & 1u) != 0;
}

void copy(
    stm32_fixture const& f,
    std::size_t          ch,
    std::uint32_t        tr1,
    std::uint32_t        tr2,
    std::uint32_t        length,
    std::uint32_t        cr   = 0,
    popcorn::address     from = source,
    popcorn::address     to   = destination)
{
  write(f, ch, "TR1", tr1);
  write(f, ch, "TR2", tr2);
  write(f, ch, "BR1", length);
  write(f, ch, "SAR", from);
  write(f, ch, "DAR", to);
  write(f, ch, "CR", cr | bit(ch, "CR", "EN"));
}

auto incrementing(std::size_t ch) -> std::uint32_t
{
  return bit(ch, "TR1", "SINC") | bit(ch, "TR1", "DINC");
}

auto software(std::size_t ch) -> std::uint32_t
{
  return bit(ch, "TR2", "SWREQ");
}

auto reach_warnings(std::vector<core::log_record> const& logged) -> std::vector<core::log_record>
{
  std::vector<core::log_record> out;
  std::ranges::copy_if(logged, std::back_inserter(out), [](core::log_record const& record) {
    return record.kind == core::log_kind::model_warning and record.message.contains("outside");
  });
  return out;
}
}

TEST_CASE("the LPDMA1 request table gives RM0456 Table 147 numbers")
{
  auto                                       f     = machine();
  auto const&                                lpdma = controller(f, "LPDMA1");
  constexpr std::array<std::string_view, 17> signals{
      "lpuart1_rx_dma", "lpuart1_tx_dma", "spi3_rx_dma",    "spi3_tx_dma",   "i2c3_rx_dma",
      "i2c3_tx_dma",    "i2c3_evc_dma",   "adc4_dma",       "dac1_ch1_dma",  "dac1_ch2_dma",
      "adf1_flt0_dma",  "lptim1_ic1_dma", "lptim1_ic2_dma", "lptim1_ue_dma", "lptim3_ic1_dma",
      "lptim3_ic2_dma", "lptim3_ue_dma"};
  for(std::uint32_t number = 0; number < signals.size(); ++number)
  {
    CAPTURE(signals[number]);
    CHECK(lpdma.request_of(signals[number]) == number);
  }
  CHECK_FALSE(lpdma.request_of("usart1_tx_dma").has_value());
  CHECK_FALSE(lpdma.request_of("hash_in_dma").has_value());
  CHECK(controller(f, "GPDMA1").request_of("lpuart1_tx_dma") == 35u);
  CHECK(f.target->model("SEC_LPDMA1") == f.target->model("LPDMA1"));
}

TEST_CASE("a software-requested LPDMA1 copy moves its block and raises TCF on its own IRQ")
{
  for(std::size_t ch = 0; ch < 4; ++ch)
  {
    CAPTURE(ch);
    auto f = machine();
    copy(f, ch, incrementing(ch), software(ch), 16, bit(ch, "CR", "TCIE"));
    idle_for(f, std::chrono::microseconds{100});
    CHECK(bytes_at(f, destination, 16) == expected(0, 16));
    CHECK(bytes_at(f, destination + 16, 1) == std::vector<std::uint32_t>{0});
    CHECK(field(f, ch, "BR1", "BNDT") == 0);
    CHECK(field(f, ch, "SR", "TCF") == 1);
    CHECK(field(f, ch, "SR", "IDLEF") == 1);
    CHECK(field(f, ch, "CR", "EN") == 0);
    CHECK(f.field("LPDMA1", "LPDMA_MISR", std::format("MIS{}", ch)) == 1);
    for(std::size_t other = 0; other < 4; ++other)
    {
      CAPTURE(other);
      CHECK(pending(f, other) == (other == ch));
    }
    write(f, ch, "FCR", bit(ch, "FCR", "TCF"));
    CHECK(field(f, ch, "SR", "TCF") == 0);
    CHECK(f.field("LPDMA1", "LPDMA_MISR", std::format("MIS{}", ch)) == 0);
  }
}

TEST_CASE("lpuart1_tx_dma paces LPDMA1 on REQSEL 1 and GPDMA1 on REQSEL 35, each acknowledging")
{
  auto        f     = machine();
  auto const& lpdma = controller(f, "LPDMA1");
  auto const& gpdma = controller(f, "GPDMA1");
  copy(f, 2, incrementing(2), 1u << lsb("LPDMA1", "LPDMA_C2TR2", "REQSEL"), 16);

  SUBCASE("LPDMA1 alone")
  {
    idle_for(f, std::chrono::microseconds{50});
    CHECK(field(f, 2, "BR1", "BNDT") == 16);
    device(f).tx.set(true);
    idle_for(f, std::chrono::microseconds{100});
    CHECK(bytes_at(f, destination, 16) == expected(0, 16));
    CHECK(field(f, 2, "SR", "TCF") == 1);
    CHECK(
        device(f).acknowledges
        == std::vector<call>(16, call{.by = &lpdma, .request = 1, .carried = true}));
    CHECK(
        device(f).completions
        == std::vector<call>{
            {.by = &lpdma, .request = 1, .carried = true}
    });
  }
  SUBCASE("both controllers on the same request")
  {
    constexpr popcorn::address other = destination + 0x100;
    write(
        f, "GPDMA1", "GPDMA_C0TR1",
        1u << lsb("GPDMA1", "GPDMA_C0TR1", "SINC") | 1u << lsb("GPDMA1", "GPDMA_C0TR1", "DINC"));
    write(f, "GPDMA1", "GPDMA_C0TR2", 35);
    write(f, "GPDMA1", "GPDMA_C0BR1", 16);
    write(f, "GPDMA1", "GPDMA_C0SAR", source);
    write(f, "GPDMA1", "GPDMA_C0DAR", other);
    write(f, "GPDMA1", "GPDMA_C0CR", 1u << lsb("GPDMA1", "GPDMA_C0CR", "EN"));
    device(f).tx.set(true);
    idle_for(f, std::chrono::microseconds{100});
    CHECK(bytes_at(f, destination, 16) == expected(0, 16));
    CHECK(bytes_at(f, other, 16) == expected(0, 16));
    CHECK(field(f, 2, "SR", "TCF") == 1);
    CHECK(f.field("GPDMA1", "GPDMA_C0SR", "TCF") == 1);
    auto const& seen = device(f).acknowledges;
    CHECK(seen.size() == 32);
    CHECK(std::ranges::count(seen, call{.by = &lpdma, .request = 1, .carried = true}) == 16);
    CHECK(std::ranges::count(seen, call{.by = &gpdma, .request = 35, .carried = true}) == 16);
    auto const& done = device(f).completions;
    CHECK(done.size() == 2);
    CHECK(std::ranges::count(done, call{.by = &lpdma, .request = 1, .carried = true}) == 1);
    CHECK(std::ranges::count(done, call{.by = &gpdma, .request = 35, .carried = true}) == 1);
  }
}

TEST_CASE("an LPDMA1 linked list of two items copies both and completes at LLR 0")
{
  auto       f = machine();
  auto const updates =
      bit(0, "LLR", "UB1") | bit(0, "LLR", "USA") | bit(0, "LLR", "UDA") | bit(0, "LLR", "ULL");
  for(auto const [index, word] : std::array<std::pair<std::uint32_t, std::uint32_t>, 4>{
          {{0, 24}, {1, source + 32}, {2, destination + 32}, {3, 0}}
  })
  {
    REQUIRE(f.target->mem().write(node + 4 * index, 4, word));
  }
  write(f, 0, "LBAR", node & 0xffff'0000u);
  write(f, 0, "LLR", updates | (node & 0xfffcu));
  copy(f, 0, incrementing(0), software(0), 16);
  idle_for(f, std::chrono::microseconds{200});
  CHECK(bytes_at(f, destination, 16) == expected(0, 16));
  CHECK(bytes_at(f, destination + 16, 16) == std::vector<std::uint32_t>(16, 0));
  CHECK(bytes_at(f, destination + 32, 24) == expected(32, 24));
  CHECK(field(f, 0, "SR", "TCF") == 1);
  CHECK(field(f, 0, "SR", "ULEF") == 0);
  CHECK(field(f, 0, "SR", "USEF") == 0);
  CHECK(field(f, 0, "CR", "EN") == 0);
  CHECK(read(f, 0, "LLR") == 0);
}

TEST_CASE("an LPDMA1 half transfer rises when exactly half the block is left")
{
  auto f = machine();
  copy(f, 1, incrementing(1), software(1), 32);
  std::uint32_t left_at_half{};
  std::uint32_t complete_at_half{1};
  auto const    stopped = f.run_to_stop({.max_instructions = icount{100'000'000}, .stop_when = [&] {
                                        if(field(f, 1, "SR", "HTF") == 0)
                                        {
                                          return false;
                                        }
                                        left_at_half     = field(f, 1, "BR1", "BNDT");
                                        complete_at_half = field(f, 1, "SR", "TCF");
                                        return true;
                                         }});
  REQUIRE(stopped.reason == stop_reason::user);
  CHECK(left_at_half == 16);
  CHECK(complete_at_half == 0);
}

TEST_CASE("LPDMA1 PAM pads, sign-extends or truncates by its one bit and never packs")
{
  auto const run = [](std::uint32_t sdw, std::uint32_t ddw, std::uint32_t pam, std::uint32_t length,
                      std::uint32_t out) {
    auto f = machine();
    REQUIRE(f.target->mem().write(source, 4, 0x8483'0281));
    REQUIRE(f.target->mem().write(source + 4, 4, 0x0000'0000));
    auto const tr1 = incrementing(0)
                   | sdw << lsb("LPDMA1", "LPDMA_C0TR1", "SDW_LOG2")
                   | ddw << lsb("LPDMA1", "LPDMA_C0TR1", "DDW_LOG2")
                   | pam << lsb("LPDMA1", "LPDMA_C0TR1", "PAM");
    copy(f, 0, tr1, software(0), length);
    idle_for(f, std::chrono::microseconds{50});
    REQUIRE(field(f, 0, "SR", "TCF") == 1);
    REQUIRE(field(f, 0, "SR", "USEF") == 0);
    return bytes_at(f, destination, out);
  };
  CHECK(run(1, 0, 0, 4, 2) == std::vector<std::uint32_t>{0x81, 0x83});
  CHECK(run(1, 0, 1, 4, 2) == std::vector<std::uint32_t>{0x02, 0x84});
  CHECK(run(0, 1, 0, 2, 4) == std::vector<std::uint32_t>{0x81, 0x00, 0x02, 0x00});
  CHECK(run(0, 1, 1, 2, 4) == std::vector<std::uint32_t>{0x81, 0xff, 0x02, 0x00});
  CHECK(run(0, 2, 1, 2, 8) == std::vector<std::uint32_t>{0x81, 0xff, 0xff, 0xff, 0x02, 0, 0, 0});
  CHECK(run(2, 0, 1, 4, 1) == std::vector<std::uint32_t>{0x84});

  auto f = machine();
  write(f, 0, "TR1", 1u << 12 | 1u << lsb("LPDMA1", "LPDMA_C0TR1", "PAM"));
  CHECK(read(f, 0, "TR1") == 1u << lsb("LPDMA1", "LPDMA_C0TR1", "PAM"));
}

TEST_CASE("GPDMA burst, exchange, 2D and port bits written to LPDMA1 read 0 and change nothing")
{
  auto       f     = machine();
  auto const gpdma = [](std::string_view reg, std::string_view field) {
    return std::uint32_t{1} << lsb("GPDMA1", std::format("GPDMA_C0{}", reg), field);
  };
  auto const extra_1 = 3u << lsb("GPDMA1", "GPDMA_C0TR1", "SBL_1")
                     | 3u << lsb("GPDMA1", "GPDMA_C0TR1", "DBL_1")
                     | gpdma("TR1", "SBX")
                     | gpdma("TR1", "DBX")
                     | gpdma("TR1", "DHX")
                     | gpdma("TR1", "SAP")
                     | gpdma("TR1", "DAP");
  write(f, 0, "TR1", incrementing(0) | extra_1);
  CHECK(read(f, 0, "TR1") == incrementing(0));
  write(f, 0, "TR2", 1u | gpdma("TR2", "DREQ") | 1u << 5);
  CHECK(read(f, 0, "TR2") == 1u);
  write(f, 0, "BR1", 4u | 1u << 16 | 1u << 28);
  CHECK(read(f, 0, "BR1") == 4u);
  write(f, 0, "SAR", source);
  write(f, 0, "DAR", destination);
  write(f, 0, "CR", 1u << 17);
  CHECK(read(f, 0, "CR") == 0);
  write(f, 0, "CR", bit(0, "CR", "EN"));
  device(f).tx.set(true);
  idle_for(f, std::chrono::microseconds{100});
  CHECK(
      bytes_at(f, destination, 5)
      == std::vector<std::uint32_t>{pattern(0), pattern(1), pattern(2), pattern(3), 0});
  CHECK(device(f).acknowledges.size() == 4);
  CHECK(device(f).completions.size() == 1);
  CHECK(field(f, 0, "SR", "TCF") == 1);
  CHECK(field(f, 0, "CR", "EN") == 0);
}

TEST_CASE("an LPDMA1 channel with an 8-byte width or a misaligned address is a user error")
{
  SUBCASE("DDW_LOG2 3")
  {
    auto f = machine();
    copy(
        f, 3, incrementing(3) | 3u << lsb("LPDMA1", "LPDMA_C3TR1", "DDW_LOG2"), software(3), 16,
        bit(3, "CR", "USEIE"));
    idle_for(f, std::chrono::microseconds{50});
    CHECK(field(f, 3, "SR", "USEF") == 1);
    CHECK(field(f, 3, "CR", "EN") == 0);
    CHECK(pending(f, 3));
    CHECK(bytes_at(f, destination, 16) == std::vector<std::uint32_t>(16, 0));
  }
  SUBCASE("half-words from an odd address")
  {
    auto f = machine();
    write(f, 3, "TR1", incrementing(3) | 1u << lsb("LPDMA1", "LPDMA_C3TR1", "SDW_LOG2"));
    write(f, 3, "TR2", software(3));
    write(f, 3, "BR1", 16);
    write(f, 3, "SAR", source + 1);
    write(f, 3, "DAR", destination);
    write(f, 3, "CR", bit(3, "CR", "EN"));
    idle_for(f, std::chrono::microseconds{50});
    CHECK(field(f, 3, "SR", "USEF") == 1);
    CHECK(bytes_at(f, destination, 16) == std::vector<std::uint32_t>(16, 0));
  }
}

TEST_CASE("an LPDMA1 snapshot before the half transfer restores its block size")
{
  auto f = machine();
  copy(f, 0, incrementing(0), software(0), 128);
  while(field(f, 0, "BR1", "BNDT") == 128)
  {
    idle_for(f, std::chrono::nanoseconds{500});
  }
  REQUIRE(field(f, 0, "BR1", "BNDT") > 64);
  REQUIRE(field(f, 0, "SR", "HTF") == 0);
  auto const bytes = core::encode_snapshot(f.target->snapshot().data());
  idle_for(f, std::chrono::microseconds{500});
  REQUIRE(field(f, 0, "SR", "TCF") == 1);
  auto const copied = bytes_at(f, destination, 129);
  CHECK(copied == [] {
    auto out = expected(0, 128);
    out.push_back(0);
    return out;
  }());

  auto other = machine();
  other.target->restore(core::snapshot{core::decode_snapshot(bytes)});
  REQUIRE(field(other, 0, "BR1", "BNDT") > 64);
  std::uint32_t left_at_half{};
  auto const    stopped =
      other.run_to_stop({.max_instructions = icount{100'000'000}, .stop_when = [&] {
                           if(field(other, 0, "SR", "HTF") == 0)
                           {
                             return false;
                           }
                           left_at_half = field(other, 0, "BR1", "BNDT");
                           return true;
                         }});
  REQUIRE(stopped.reason == stop_reason::user);
  CHECK(left_at_half == 64);
  idle_for(other, std::chrono::microseconds{500});
  CHECK(field(other, 0, "SR", "TCF") == 1);
  CHECK(bytes_at(other, destination, 129) == copied);
}

TEST_CASE("DBG_LPDMA0_STOP holds an LPDMA1 channel while the core is held, clear it runs on")
{
  for(auto const frozen : {true, false})
  {
    CAPTURE(frozen);
    auto f = machine();
    popcorn::testing::set_debug_freeze(f, "DBG_LPDMA0_STOP", frozen);
    copy(f, 0, incrementing(0), software(0), 128);
    while(field(f, 0, "BR1", "BNDT") == 128)
    {
      idle_for(f, std::chrono::nanoseconds{500});
    }
    auto const left = field(f, 0, "BR1", "BNDT");
    f.target->hold_core(true);
    later(f, 500 * ps_per_us);
    CHECK(field(f, 0, "BR1", "BNDT") == (frozen ? left : 0));
    f.target->hold_core(false);
    later(f, 500 * ps_per_us);
    CHECK(field(f, 0, "BR1", "BNDT") == 0);
    CHECK(field(f, 0, "SR", "TCF") == 1);
  }
}

TEST_CASE("an LPDMA1 address outside SRAM4 and the SRD peripherals warns once per channel and kind")
{
  auto                          f = machine();
  std::vector<core::log_record> logged;
  auto const sink = f.target->add_log_sink([&](core::log_record const& r) { logged.push_back(r); });
  for(std::uint32_t index = 0; index < 16; ++index)
  {
    REQUIRE(f.target->mem().write(sram1 + index, 1, pattern(index)));
  }
  for(auto round = 0; round < 2; ++round)
  {
    copy(f, 0, incrementing(0), software(0), 16, 0, sram1);
    idle_for(f, std::chrono::microseconds{100});
    CHECK(bytes_at(f, destination, 16) == expected(0, 16));
    CHECK(field(f, 0, "SR", "TCF") == 1);
    CHECK(field(f, 0, "SR", "DTEF") == 0);
    CHECK(field(f, 0, "SR", "ULEF") == 0);
    write(f, 0, "FCR", bit(0, "FCR", "TCF"));
    REQUIRE(reach_warnings(logged).size() == 1);
  }
  auto const first = reach_warnings(logged)[0];
  CHECK(first.peripheral == "LPDMA1");
  CHECK(first.reg == "LPDMA_C0CR");
  CHECK(first.message.contains("source"));
  CHECK(first.message.contains("0x20000000"));
  copy(f, 1, incrementing(1), software(1), 16, 0, sram1);
  idle_for(f, std::chrono::microseconds{100});
  CHECK(reach_warnings(logged).size() == 2);
  CHECK(reach_warnings(logged)[1].reg == "LPDMA_C1CR");

  copy(f, 0, incrementing(0), software(0), 16, 0, source, sram1 + 0x100);
  idle_for(f, std::chrono::microseconds{100});
  CHECK(bytes_at(f, sram1 + 0x100, 16) == expected(0, 16));
  REQUIRE(reach_warnings(logged).size() == 3);
  CHECK(reach_warnings(logged)[2].message.contains("destination"));
  copy(f, 0, incrementing(0), software(0), 16, 0, source, sram1 + 0x100);
  idle_for(f, std::chrono::microseconds{100});
  CHECK(reach_warnings(logged).size() == 3);

  auto const updates = bit(2, "LLR", "UB1") | bit(2, "LLR", "USA") | bit(2, "LLR", "UDA");
  for(auto const [index, word] : std::array<std::pair<std::uint32_t, std::uint32_t>, 3>{
          {{0, 8}, {1, source + 32}, {2, destination + 32}}
  })
  {
    REQUIRE(f.target->mem().write(sram1 + 0x200 + 4 * index, 4, word));
  }
  write(f, 2, "LBAR", sram1 & 0xffff'0000u);
  write(f, 2, "LLR", updates | ((sram1 + 0x200) & 0xfffcu));
  copy(f, 2, incrementing(2), software(2), 16);
  idle_for(f, std::chrono::microseconds{200});
  CHECK(bytes_at(f, destination + 32, 8) == expected(32, 8));
  CHECK(field(f, 2, "SR", "ULEF") == 0);
  REQUIRE(reach_warnings(logged).size() == 4);
  CHECK(reach_warnings(logged)[3].message.contains("linked-list"));
}

TEST_CASE("an LPDMA1 address outside its reach is a model_error under strict peripherals")
{
  auto f = machine(true);
  copy(f, 0, incrementing(0), software(0), 16, 0, sram1);
  CHECK_THROWS_AS(
      static_cast<void>(f.run(
          {.max_instructions = icount{1'000'000'000},
           .wall_budget      = std::chrono::seconds{60},
           .until_time       = vtime{f.target->virtual_clock().time().ps + 100 * ps_per_us}})),
      popcorn::model_error);
  CHECK(bytes_at(f, destination, 16) == std::vector<std::uint32_t>(16, 0));
}

TEST_CASE(
    "LPDMA1 reaches SRAM4 and the SRD peripherals, secure aliases included, without a warning")
{
  auto                          f = machine(true);
  std::vector<core::log_record> logged;
  auto const sink = f.target->add_log_sink([&](core::log_record const& r) { logged.push_back(r); });
  constexpr std::array<popcorn::address, 4> sources{
      source, source + 0x1000'0000, 0x4600'2400, 0x5600'2400};
  constexpr std::array<popcorn::address, 4> targets{
      destination, destination + 0x1000'0000, lpuart1_tdr, lpuart1_tdr + 0x1000'0000};
  for(auto index = 0u; index < sources.size(); ++index)
  {
    CAPTURE(index);
    copy(f, 0, 0, software(0), 4, 0, sources[index], targets[index]);
    idle_for(f, std::chrono::microseconds{100});
    CHECK(field(f, 0, "SR", "TCF") == 1);
    write(f, 0, "FCR", bit(0, "FCR", "TCF"));
  }
  CHECK(logged.empty());
}

TEST_CASE("GPDMA1 copies from SRAM1 without a reach warning")
{
  auto                          f = machine(true);
  std::vector<core::log_record> logged;
  auto const sink = f.target->add_log_sink([&](core::log_record const& r) { logged.push_back(r); });
  for(std::uint32_t index = 0; index < 16; ++index)
  {
    REQUIRE(f.target->mem().write(sram1 + index, 1, pattern(index)));
  }
  auto const gp = [&](std::string_view reg) { return std::format("GPDMA_C0{}", reg); };
  write(
      f, "GPDMA1", gp("TR1"),
      1u << lsb("GPDMA1", gp("TR1"), "SINC") | 1u << lsb("GPDMA1", gp("TR1"), "DINC"));
  write(f, "GPDMA1", gp("TR2"), 1u << lsb("GPDMA1", gp("TR2"), "SWREQ"));
  write(f, "GPDMA1", gp("BR1"), 16);
  write(f, "GPDMA1", gp("SAR"), sram1);
  write(f, "GPDMA1", gp("DAR"), sram1 + 0x100);
  write(f, "GPDMA1", gp("CR"), 1u << lsb("GPDMA1", gp("CR"), "EN"));
  idle_for(f, std::chrono::microseconds{100});
  CHECK(bytes_at(f, sram1 + 0x100, 16) == expected(0, 16));
  CHECK(f.field("GPDMA1", gp("SR"), "TCF") == 1);
  CHECK(logged.empty());
}
