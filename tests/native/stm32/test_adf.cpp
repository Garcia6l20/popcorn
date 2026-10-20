#include "dfilter_chain.hpp"
#include "dma.hpp"
#include "gpdma_requests.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/pdm.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <map>
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
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;
using popcorn::testing::u575;

constexpr std::uint64_t us              = 1'000'000;
constexpr std::uint32_t adf_irq         = 112;
constexpr std::uint32_t sample_bits     = 16;
constexpr std::uint64_t sample_ps       = 7'812'500;
constexpr std::uint32_t gpdma_adf       = 98;
constexpr std::uint32_t lpdma_adf       = 10;
constexpr std::uint32_t sad_level_flag  = 1u << 13;
constexpr std::uint32_t sad_detect_flag = 1u << 12;

class request_probe final : public popcorn::periph::model, public stm32::dma_controller
{
public:
  explicit request_probe(std::span<stm32::gpdma_request const> table) : table_{table}
  {}

  std::map<std::uint32_t, std::vector<bool>> levels;

  void on_request(std::uint32_t request, bool level) override
  {
    auto& seen = levels[request];
    if(seen.empty() or seen.back() != level)
    {
      seen.push_back(level);
    }
  }

  [[nodiscard]] auto request_of(std::string_view signal) const
      -> std::optional<std::uint32_t> override
  {
    auto const found = std::ranges::find(table_, signal, &stm32::gpdma_request::signal);
    if(found == table_.end())
    {
      return std::nullopt;
    }
    return found->request;
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

private:
  std::span<stm32::gpdma_request const> table_;
};

auto options_for(bool strict, bool probe) -> core::machine_options
{
  stm32::board board{};
  auto         options       = popcorn::testing::stm32_options();
  options.models             = stm32::factories(board, u575());
  options.strict_peripherals = strict;
  if(probe)
  {
    for(auto const& [name, table] : {
            std::pair{"GPDMA1", std::span<stm32::gpdma_request const>{stm32::gpdma_requests}},
            std::pair{"LPDMA1", std::span<stm32::gpdma_request const>{stm32::lpdma_requests}}
    })
    {
      options.models.insert_or_assign(
          name,
          [table](popcorn::periph::model_context&, popcorn::peripheral_desc const&)
              -> std::shared_ptr<popcorn::periph::model> {
            return std::make_shared<request_probe>(table);
          });
    }
  }
  return options;
}

class scripted final : public stm32::pdm_source
{
public:
  std::vector<std::uint8_t> bits;
  std::size_t               next{};

  void clock(std::span<std::uint8_t> out, stm32::bit_rate) override
  {
    for(auto& bit : out)
    {
      bit = next < bits.size() ? bits[next] : static_cast<std::uint8_t>(next % 2);
      ++next;
    }
  }
};

struct tapped
{
  std::int32_t  pcm;
  std::uint64_t at;
  bool          dropped;
  bool          captured;
};

class recorder final : public stm32::pdm_tap
{
public:
  std::vector<tapped> samples;

  void wrote(std::int32_t pcm, vtime at, bool dropped, bool captured) override
  {
    samples.push_back({.pcm = pcm, .at = at.ps, .dropped = dropped, .captured = captured});
  }
};

struct rig : stm32_fixture
{
  std::vector<core::log_record> logged;
  core::hook_handle             sink;

  explicit rig(bool strict = false, bool probe = false)
      : stm32_fixture{u575(), "rcc_ll", options_for(strict, probe)},
        sink{target->add_log_sink(
            [this](core::log_record const& record) { logged.push_back(record); })}
  {
    set("pc", sym("rcc_ll", "idle"));
    put("RCC", "RCC_AHB3ENR", "ADF1EN", 1);
  }

  [[nodiscard]] auto at(std::string_view peripheral, std::string_view reg) const -> popcorn::address
  {
    auto const& desc  = target->peripheral(peripheral).description();
    auto const  found = popcorn::find_register(desc, reg);
    REQUIRE(found != nullptr);
    return desc.base + found->offset;
  }

  void write(std::string_view peripheral, std::string_view reg, std::uint32_t value) const
  {
    REQUIRE(target->mem().write(at(peripheral, reg), 4, value).has_value());
  }

  void write(std::string_view reg, std::uint32_t value) const
  {
    write("ADF1", reg, value);
  }

  void raw(std::string_view reg, std::uint32_t value) const
  {
    static_cast<void>(target->mem().write(at("ADF1", reg), 4, value));
  }

  [[nodiscard]] auto read(std::string_view peripheral, std::string_view reg) const -> std::uint32_t
  {
    auto const value = target->mem().read(at(peripheral, reg), 4);
    REQUIRE(value.has_value());
    return *value;
  }

  [[nodiscard]] auto read(std::string_view reg) const -> std::uint32_t
  {
    return read("ADF1", reg);
  }

  [[nodiscard]] auto bits(std::string_view reg, std::string_view name) const -> std::uint32_t
  {
    return field("ADF1", reg, name);
  }

  [[nodiscard]] auto shifted(std::string_view reg, std::string_view name, std::uint32_t value) const
      -> std::uint32_t
  {
    return shifted("ADF1", reg, name, value);
  }

  [[nodiscard]] auto shifted(
      std::string_view peripheral,
      std::string_view reg,
      std::string_view name,
      std::uint32_t    value) const -> std::uint32_t
  {
    auto const& desc  = target->peripheral(peripheral).description();
    auto const  found = popcorn::find_register(desc, reg);
    REQUIRE(found != nullptr);
    auto const field = std::ranges::find(found->fields, name, &popcorn::field_desc::name);
    REQUIRE(field != found->fields.end());
    auto const mask = ((std::uint64_t{1} << field->width) - 1u) << field->lsb;
    return static_cast<std::uint32_t>((std::uint64_t{value} << field->lsb) & mask);
  }

  void
      put(std::string_view peripheral,
          std::string_view reg,
          std::string_view name,
          std::uint32_t    value) const
  {
    auto const mask = shifted(peripheral, reg, name, ~std::uint32_t{});
    write(peripheral, reg, (read(peripheral, reg) & ~mask) | shifted(peripheral, reg, name, value));
  }

  [[nodiscard]] auto now() const -> std::uint64_t
  {
    return target->virtual_clock().time().ps;
  }

  void run_until(std::uint64_t ps) const
  {
    REQUIRE(
        run_to_stop({.max_instructions = icount{100'000'000}, .until_time = vtime{ps}}).reason
        == stop_reason::time);
  }

  [[nodiscard]] auto pending(std::uint32_t irq) const -> bool
  {
    auto const word = target->mem().read(0xe000'e200u + 4 * (irq / 32), 4);
    REQUIRE(word.has_value());
    return (*word >> (irq % 32) & 1u) != 0;
  }

  [[nodiscard]] auto warned(std::string_view text) const -> std::size_t
  {
    return static_cast<std::size_t>(std::ranges::count_if(logged, [text](auto const& record) {
      return record.kind == core::log_kind::model_warning and record.message.contains(text);
    }));
  }
};

template <typename F>
void check_unmodelled(F&& call, std::string_view reg, std::string_view field, std::uint32_t value)
{
  try
  {
    call();
    FAIL("no unmodelled_config");
  }
  catch(popcorn::unmodelled_config const& error)
  {
    CHECK(error.peripheral() == "ADF1");
    CHECK(error.reg() == reg);
    CHECK(error.field() == field);
    CHECK(error.value() == value);
  }
}

struct sad_setup
{
  std::uint32_t sadcr;
  std::uint32_t sadcfgr;

  [[nodiscard]] auto config() const -> stm32::sad_config
  {
    return {
        .mode       = static_cast<std::uint8_t>((sadcr >> 12) & 3u),
        .frame      = static_cast<std::uint8_t>((sadcr >> 8) & 7u),
        .learn      = static_cast<std::uint8_t>((sadcfgr >> 8) & 7u),
        .hangover   = static_cast<std::uint8_t>((sadcfgr >> 12) & 7u),
        .slope      = static_cast<std::uint8_t>((sadcfgr >> 4) & 7u),
        .threshold  = static_cast<std::uint8_t>(sadcfgr & 15u),
        .hysteresis = (sadcr & (1u << 7)) != 0,
        .floor      = static_cast<std::uint16_t>((sadcfgr >> 16) & 0x1FFFu)};
  }
};

struct stream_rig : rig
{
  std::shared_ptr<scripted> mic{std::make_shared<scripted>()};
  std::shared_ptr<recorder> tap{std::make_shared<recorder>()};

  explicit stream_rig(bool probe = false) : rig{false, probe}
  {
    put("RCC", "RCC_CR", "HSION", 1);
    run_until(now() + 200 * us);
    write("RCC", "RCC_PLL3CFGR", (2u << 0) | (3u << 2) | (4u << 8) | (1u << 17));
    write("RCC", "RCC_PLL3DIVR", 63u | (9u << 16));
    put("RCC", "RCC_CR", "PLL3ON", 1);
    run_until(now() + 200 * us);
    REQUIRE(field("RCC", "RCC_CR", "PLL3RDY") == 1);
    put("RCC", "RCC_CCIPR3", "ADF1SEL", 2);
    port().attach(0, stm32::pdm_edge::rising, mic);
    port().attach_tap(0, tap);
    write("CKGCR", (4u << 24) | (1u << 16) | (1u << 5) | (1u << 1));
    put("ADF1", "CKGCR", "CKGDEN", 1);
    write("SITF0CR", 0x1F01);
  }

  [[nodiscard]] auto port() const -> stm32::pdm_port&
  {
    auto* const found = dynamic_cast<stm32::pdm_port*>(target->model("ADF1"));
    REQUIRE(found != nullptr);
    return *found;
  }

  void loud(std::vector<bool> const& samples) const
  {
    mic->bits.clear();
    for(auto const on : samples)
    {
      for(std::uint32_t b = 0; b < sample_bits; ++b)
      {
        mic->bits.push_back(on ? 1 : static_cast<std::uint8_t>(b % 2));
      }
    }
  }

  void sinc4(std::uint32_t cr = 0) const
  {
    write("BSMX0CR", 0);
    write("DFLT0CICR", (0x0Cu << 20) | ((sample_bits - 1) << 8) | (4u << 4));
    write("DFLT0RSFR", 1u | (1u << 7));
    write("DFLT0CR", cr | 1u);
  }

  void sad(sad_setup const& setup) const
  {
    write("SADCFGR", setup.sadcfgr);
    write("SADCR", setup.sadcr);
    write("SADCR", setup.sadcr | 1u);
  }

  [[nodiscard]] auto phase() const -> std::uint32_t
  {
    return (read("SADCR") >> 4) & 3u;
  }

  [[nodiscard]] auto pop() const -> std::uint32_t
  {
    auto&       regs  = target->peripheral("ADF1");
    auto const* found = popcorn::find_register(regs.description(), "DFLT0DR");
    REQUIRE(found != nullptr);
    auto const value = target->model("ADF1")->on_read(
        regs, found->offset, 4, {.pc = 0, .now = icount{0}, .by = popcorn::initiator::cpu});
    REQUIRE(value.has_value());
    return *value;
  }
};

auto bursts(std::size_t frames, std::vector<std::pair<std::size_t, std::size_t>> const& spans)
    -> std::vector<bool>
{
  std::vector<bool> out(frames * 8);
  for(auto const& [from, to] : spans)
  {
    for(auto k = from * 8; k < to * 8; ++k)
    {
      out[k] = true;
    }
  }
  return out;
}

struct expected_run
{
  std::vector<stm32::sad_frame> frames;
  std::vector<std::size_t>      ends;
  std::vector<bool>             captured;
};

auto expect(std::vector<tapped> const& samples, sad_setup const& setup, std::uint32_t datcap)
    -> expected_run
{
  expected_run     out;
  stm32::sad_state state;
  auto const       config = setup.config();
  for(std::size_t k = 0; k < samples.size(); ++k)
  {
    out.captured.push_back(
        datcap >= 2 or (datcap == 1 and state.phase == stm32::sad_phase::detect));
    if(auto const frame = stm32::sad_feed(state, config, samples[k].pcm))
    {
      out.frames.push_back(*frame);
      out.ends.push_back(k);
    }
  }
  return out;
}

constexpr sad_setup fixed_detect{
    .sadcr   = (1u << 12) | (1u << 3) | (1u << 1),
    .sadcfgr = (100u << 16) | (0u << 12) | (0u << 8)};
}

TEST_CASE("ADF1 SCKSRC 2, BSSEL 2 and TRGSRC 0010 raise naming ADF1, a retry works")
{
  rig const r;
  check_unmodelled([&r] { r.raw("SITF0CR", 0x1F05); }, "SITF0CR", "SCKSRC", 2);
  CHECK(r.read("SITF0CR") == 0x1F00);
  r.write("SITF0CR", 0x1F01);
  CHECK(r.bits("SITF0CR", "SITFACTIVE") == 1);

  r.write("BSMX0CR", 2);
  r.write("DFLT0CICR", 5u << 4);
  check_unmodelled([&r] { r.raw("DFLT0CR", 1); }, "BSMX0CR", "BSSEL", 2);
  CHECK(r.bits("DFLT0CR", "DFLTACTIVE") == 0);
  r.write("BSMX0CR", 1);
  r.write("DFLT0CR", 1);
  CHECK(r.bits("DFLT0CR", "DFLTACTIVE") == 1);
  r.write("DFLT0CR", 0);

  check_unmodelled([&r] { r.raw("DFLT0CR", (2u << 12) | (2u << 4) | 1u); }, "DFLT0CR", "TRGSRC", 2);
  CHECK(r.bits("DFLT0CR", "DFLTACTIVE") == 0);
  r.write("DFLT0CR", (2u << 4) | 1u);
  CHECK(r.bits("DFLT0CR", "DFLTACTIVE") == 1);
}

TEST_CASE("ADF1 ACQMOD 101 runs as 000, RM0456 40.8.5, 100 raises the window mode")
{
  stream_rig const r;
  r.sinc4(0);
  r.write("DFLT0CR", 0);
  check_unmodelled([&r] { r.raw("DFLT0CR", (4u << 4) | 1u); }, "DFLT0CR", "ACQMOD", 4);
  CHECK(r.bits("DFLT0CR", "DFLTACTIVE") == 0);
  r.sinc4(5u << 4);
  CHECK(r.bits("DFLT0CR", "DFLTRUN") == 1);
  r.run_until(r.now() + 3 * sample_ps);
  CHECK(r.tap->samples.size() >= 2);
}

TEST_CASE("ADF1's DMA request reaches GPDMA1 request 98 and LPDMA1 request 10")
{
  stream_rig const r{true};
  auto*            gpdma = r.model_of<request_probe>("GPDMA1");
  auto*            lpdma = r.model_of<request_probe>("LPDMA1");
  REQUIRE(gpdma != nullptr);
  REQUIRE(lpdma != nullptr);
  r.sinc4(1u << 1);
  r.run_until(r.now() + sample_ps + sample_ps / 2);
  REQUIRE(r.tap->samples.size() == 1);
  CHECK(gpdma->levels[gpdma_adf] == std::vector<bool>{false, true});
  CHECK(lpdma->levels[lpdma_adf] == std::vector<bool>{false, true});
  static_cast<void>(r.pop());
  CHECK(gpdma->levels[gpdma_adf] == std::vector<bool>{false, true, false});
  CHECK(lpdma->levels[lpdma_adf] == std::vector<bool>{false, true, false});
}

TEST_CASE(
    "SADMOD 01 with DATCAP 01 and DETCFG 1: SDDETF at entry and exit, the RXFIFO gets "
    "popcorn's capture window, the frame after the entry through the frame that leaves")
{
  stream_rig const r;
  r.loud(bursts(
      40, {
              {6,  9 },
              {20, 21}
  }));
  r.sad(fixed_detect);
  r.sinc4();
  std::vector<std::uint32_t> phases;
  std::vector<bool>          detected;
  std::size_t                popped = 0;
  while(r.tap->samples.size() < 40 * 8)
  {
    r.run_until(r.now() + sample_ps / 2);
    auto const isr = r.read("DFLT0ISR");
    for(auto level = isr; (level & (1u << 3)) != 0; level = r.read("DFLT0ISR"))
    {
      static_cast<void>(r.pop());
      ++popped;
    }
    if(r.tap->samples.size() >= 8 * (phases.size() + 1))
    {
      REQUIRE(r.tap->samples.size() == 8 * (phases.size() + 1));
      phases.push_back(r.phase());
      detected.push_back((isr & sad_detect_flag) != 0);
      r.write("DFLT0ISR", sad_detect_flag | sad_level_flag);
    }
  }
  REQUIRE(r.tap->samples.size() == 40 * 8);
  auto const want = expect(r.tap->samples, fixed_detect, 1);
  REQUIRE(want.frames.size() == 40);
  REQUIRE(phases.size() == 40);
  std::size_t changes = 0;
  for(std::size_t i = 0; i < 40; ++i)
  {
    CAPTURE(i);
    CHECK(phases[i] == static_cast<std::uint32_t>(want.frames[i].phase));
    CHECK(detected[i] == want.frames[i].changed);
    changes += want.frames[i].changed ? 1 : 0;
  }
  CHECK(changes == 4);
  CHECK(want.frames[6].phase == stm32::sad_phase::detect);
  CHECK(want.frames[5].phase == stm32::sad_phase::monitor);
  std::size_t captured = 0;
  for(std::size_t k = 0; k < r.tap->samples.size(); ++k)
  {
    CAPTURE(k);
    CHECK(r.tap->samples[k].captured == want.captured[k]);
    CHECK_FALSE(r.tap->samples[k].dropped);
    captured += want.captured[k] ? 1 : 0;
  }
  CHECK(captured > 0);
  CHECK(captured < r.tap->samples.size() / 2);
  CHECK_FALSE(want.captured[7 * 8 - 1]);
  CHECK(want.captured[7 * 8]);
  CHECK(popped == captured);
  CHECK((r.read("DFLT0ISR") & (1u << 1)) == 0);
}

TEST_CASE("DATCAP 00 keeps every sample out with no DOVRF, SADEN 0 lets them through again")
{
  stream_rig const r;
  r.loud(bursts(8, {}));
  r.sad({.sadcr = 1u << 12, .sadcfgr = 100u << 16});
  r.sinc4();
  r.run_until(r.now() + 16 * sample_ps);
  REQUIRE(r.tap->samples.size() >= 15);
  CHECK(std::ranges::none_of(r.tap->samples, &tapped::captured));
  CHECK((r.read("DFLT0ISR") & ((1u << 3) | (1u << 1))) == 0);
  r.write("SADCR", 1u << 12);
  auto const before = r.tap->samples.size();
  r.run_until(r.now() + 2 * sample_ps);
  REQUIRE(r.tap->samples.size() > before);
  CHECK(r.tap->samples.back().captured);
  CHECK((r.read("DFLT0ISR") & (1u << 3)) != 0);
  CHECK(r.phase() == 0);
}

TEST_CASE(
    "with DATCAP 00, SDLVLF and SDDETF raise ADF1_IRQn at the edge of the frame's last sample, "
    "popcorn's zero latency, no read")
{
  sad_setup const setup{.sadcr = 1u << 12, .sadcfgr = 100u << 16};
  auto const      run = [&setup](stream_rig const& r, std::uint32_t enable) {
    r.loud(bursts(
        12, {
                {4, 6}
    }));
    r.write("DFLT0IER", enable);
    r.sad(setup);
    r.sinc4();
  };
  stream_rig const dry;
  run(dry, 0);
  dry.run_until(dry.now() + 12 * 8 * sample_ps);
  auto const want = expect(dry.tap->samples, setup, 0);
  REQUIRE(want.frames.size() >= 6);
  REQUIRE(want.frames[3].phase == stm32::sad_phase::monitor);
  REQUIRE(want.frames[4].phase == stm32::sad_phase::detect);
  auto const first_level = dry.tap->samples[want.ends[0]].at;
  auto const detection   = dry.tap->samples[want.ends[4]].at;
  for(auto const& [enable, at] : {
          std::pair{sad_level_flag,  first_level},
          std::pair{sad_detect_flag, detection  }
  })
  {
    CAPTURE(enable);
    stream_rig const r;
    run(r, enable);
    r.run_until(at - us / 2);
    CHECK_FALSE(r.pending(adf_irq));
    r.run_until(at + us / 2);
    CHECK(r.pending(adf_irq));
  }
}

TEST_CASE("SADSDLVR and SADANLVR keep the frame that set SDLVLF until SDLVLF is cleared")
{
  stream_rig const r;
  r.loud(bursts(
      6, {
             {1, 2}
  }));
  sad_setup const setup{.sadcr = (1u << 12) | (2u << 1), .sadcfgr = 100u << 16};
  r.sad(setup);
  r.sinc4();
  r.run_until(r.now() + 2 * 8 * sample_ps + sample_ps / 2);
  auto const want = expect(r.tap->samples, setup, 2);
  REQUIRE(want.frames.size() == 2);
  REQUIRE(want.frames[0].level != want.frames[1].level);
  CHECK(r.read("SADSDLVR") == want.frames[0].level);
  CHECK(r.read("SADANLVR") == want.frames[0].noise);
  r.write("DFLT0ISR", sad_level_flag);
  CHECK((r.read("DFLT0ISR") & sad_level_flag) == 0);
  r.run_until(r.now() + 8 * sample_ps);
  auto const later = expect(r.tap->samples, setup, 2);
  REQUIRE(later.frames.size() == 3);
  CHECK((r.read("DFLT0ISR") & sad_level_flag) != 0);
  CHECK(r.read("SADSDLVR") == later.frames[2].level);
  CHECK(r.read("SADANLVR") == later.frames[2].noise);
}

TEST_CASE("SADEN 0 then 1 restarts LEARN: frames count from the enable, the old ones are dropped")
{
  stream_rig const r;
  r.loud(bursts(
      64, {
              {2, 64}
  }));
  sad_setup const setup{.sadcr = (1u << 12) | (2u << 1), .sadcfgr = (100u << 16) | (1u << 8)};
  r.sad(setup);
  r.sinc4();
  r.run_until(r.now() + 8 * 8 * sample_ps);
  CHECK(r.phase() == 3);
  r.write("SADCR", setup.sadcr);
  CHECK(r.phase() == 0);
  r.run_until(r.now() + 8 * sample_ps / 2);
  auto const from = r.tap->samples.size();
  r.write("SADCR", setup.sadcr | 1u);
  r.run_until(r.tap->samples[from - 1].at + 8 * 4 * sample_ps - sample_ps / 2);
  CHECK(r.phase() == 0);
  r.write("DFLT0ISR", sad_level_flag);
  r.run_until(r.now() + sample_ps);
  CHECK(r.phase() == 1);
  std::vector<tapped> const fresh{
      r.tap->samples.begin() + static_cast<std::ptrdiff_t>(from), r.tap->samples.end()};
  auto const want = expect(fresh, setup, 2);
  REQUIRE(want.frames.size() == 4);
  CHECK(r.read("SADANLVR") == want.frames[3].noise);
}

TEST_CASE("a snapshot taken in DETECT's hangover restores the SAD: the same frames follow")
{
  stream_rig const r;
  r.loud(bursts(
      30, {
              {4, 6 },
              {9, 10}
  }));
  sad_setup const setup{
      .sadcr = (1u << 12) | (1u << 3) | (1u << 1), .sadcfgr = (100u << 16) | (1u << 12)};
  r.sad(setup);
  r.sinc4();
  r.run_until(r.now() + 7 * 8 * sample_ps + 3 * sample_ps);
  REQUIRE(r.phase() == 3);
  auto const saved = r.target->snapshot();
  auto const next  = r.mic->next;
  auto const from  = r.tap->samples.size();
  auto const end   = r.now() + 20 * 8 * sample_ps;
  r.run_until(end);
  std::vector<tapped> const first{
      r.tap->samples.begin() + static_cast<std::ptrdiff_t>(from), r.tap->samples.end()};
  auto const first_state = std::pair{r.read("SADSDLVR"), r.read("SADCR")};
  r.target->restore(saved);
  r.mic->next = next;
  r.tap->samples.resize(from);
  r.run_until(end);
  std::vector<tapped> const second{
      r.tap->samples.begin() + static_cast<std::ptrdiff_t>(from), r.tap->samples.end()};
  REQUIRE(first.size() == second.size());
  for(std::size_t k = 0; k < first.size(); ++k)
  {
    CAPTURE(k);
    CHECK(first[k].captured == second[k].captured);
  }
  CHECK(std::ranges::any_of(first, &tapped::captured));
  CHECK(std::ranges::any_of(first, [](tapped const& t) { return not t.captured; }));
  CHECK(std::pair{r.read("SADSDLVR"), r.read("SADCR")} == first_state);
}

namespace
{
struct sad_protected
{
  std::string_view reg;
  std::string_view field;
};

constexpr sad_protected sad_fields[]{
    {"SADCR",   "FRSIZE"},
    {"SADCR",   "DETCFG"},
    {"SADCR",   "SADMOD"},
    {"SADCR",   "HYSTEN"},
    {"SADCR",   "DATCAP"},
    {"SADCFGR", "ANMIN" },
    {"SADCFGR", "HGOVR" },
    {"SADCFGR", "LFRNB" },
    {"SADCFGR", "ANSLP" },
    {"SADCFGR", "SNTHR" },
};
}

TEST_CASE("each Table 401 SAD field changed while SADACTIVE is kept, logs once, raises when strict")
{
  for(auto const& entry : sad_fields)
  {
    CAPTURE(entry.reg);
    CAPTURE(entry.field);
    {
      rig const r;
      r.write("SADCR", 1);
      REQUIRE(r.bits("SADCR", "SADACTIVE") == 1);
      auto const before = r.read(entry.reg);
      auto const word   = (entry.reg == "SADCR" ? 1u : 0u) | r.shifted(entry.reg, entry.field, 1);
      r.write(entry.reg, word);
      CHECK(r.read(entry.reg) == before);
      r.write(entry.reg, word);
      CHECK(r.warned(std::format("ADF1 {}", entry.reg)) == 1);
      CHECK(r.warned("Table 401") == 1);
    }
    rig const r{true};
    r.write("SADCR", 1);
    auto const before = r.read(entry.reg);
    auto const word   = (entry.reg == "SADCR" ? 1u : 0u) | r.shifted(entry.reg, entry.field, 1);
    CHECK_THROWS_AS(
        static_cast<void>(r.target->mem().write(r.at("ADF1", entry.reg), 4, word)),
        popcorn::model_error);
    CHECK(r.read(entry.reg) == before);
  }
  rig const r;
  r.write("SADCR", 1);
  r.write("SADCR", 0);
  CHECK(r.bits("SADCR", "SADACTIVE") == 0);
  r.write("SADCFGR", 0x00FF'0779u);
  CHECK(r.read("SADCFGR") == 0x00FF'0779u);
  CHECK(r.warned("Table 401") == 0);
}

TEST_CASE("SNTHR 10 raises at SADEN naming SADCFGR, HYSTEN with SADMOD 10 logs once, strict raises")
{
  {
    rig const r;
    r.write("SADCFGR", 10);
    check_unmodelled([&r] { r.raw("SADCR", 1); }, "SADCFGR", "SNTHR", 10);
    CHECK(r.read("SADCR") == 0);
    r.write("SADCFGR", 9);
    r.write("SADCR", 1);
    CHECK(r.bits("SADCR", "SADACTIVE") == 1);
  }
  {
    rig const r;
    r.write("SADCR", (2u << 12) | (1u << 7) | 1u);
    CHECK(r.bits("SADCR", "SADACTIVE") == 1);
    r.write("SADCR", 0);
    r.write("SADCR", (2u << 12) | (1u << 7) | 1u);
    CHECK(r.warned("HYSTEN") == 1);
  }
  rig const r{true};
  CHECK_THROWS_AS(
      static_cast<void>(
          r.target->mem().write(r.at("ADF1", "SADCR"), 4, (2u << 12) | (1u << 7) | 1u)),
      popcorn::model_error);
  CHECK(r.read("SADCR") == 0);
  r.write("SADCR", (2u << 12) | 1u);
  CHECK(r.bits("SADCR", "SADACTIVE") == 1);
}
