#include "dma.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/stm32/audio.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <doctest/doctest.h>
#include <reflex/serde/bson.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
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

constexpr std::uint64_t us       = 1'000'000;
constexpr std::uint64_t frame_ps = 64 * us;
constexpr std::uint32_t sai_irq  = 90;

struct received_call
{
  std::size_t   slots{};
  std::uint64_t at{};

  friend auto operator==(received_call const&, received_call const&) -> bool = default;
};

class recorder final : public stm32::audio_endpoint
{
public:
  explicit recorder(std::vector<std::uint32_t> words = {}) : words_{std::move(words)}
  {}

  std::vector<stm32::captured_audio_frame> frames;
  std::vector<received_call>               calls;

  void
      transmitted(std::span<std::uint32_t const> slots, stm32::audio_format format, vtime at, vtime)
          override
  {
    frames.push_back({
        .slots = {slots.begin(), slots.end()},
          .format = format, .at = at
    });
  }

  void receive(std::span<std::uint32_t> slots, stm32::audio_format, vtime at) override
  {
    calls.push_back({.slots = slots.size(), .at = at.ps});
    for(auto& slot : slots)
    {
      slot = next_ < words_.size() ? words_[next_++] : 0;
    }
  }

private:
  std::vector<std::uint32_t> words_;
  std::size_t                next_{};
};

auto ramp(std::uint32_t index) -> std::uint32_t
{
  return 0x1000u + index;
}

auto source_words(std::size_t count) -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(std::size_t index = 0; index < count; ++index)
  {
    out.push_back(0xA0000u + static_cast<std::uint32_t>(index) * 0x111u);
  }
  return out;
}

constexpr std::uint32_t a_request = 36;
constexpr std::uint32_t b_request = 37;

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
    if(signal == "sai1_a_dma")
    {
      return a_request;
    }
    if(signal == "sai1_b_dma")
    {
      return b_request;
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
  std::shared_ptr<recorder>      a = std::make_shared<recorder>();
  std::shared_ptr<recorder>      b;
  stm32::board                   board{};
  std::unique_ptr<stm32_fixture> f;

  explicit rig(
      std::string_view           entry,
      std::vector<std::uint32_t> words    = source_words(256),
      stm32::audio_attachment    a_wiring = {},
      bool                       probe    = false)
      : b{std::make_shared<recorder>(std::move(words))}
  {
    a_wiring.endpoint     = a;
    board.audio["SAI1_A"] = a_wiring;
    board.audio["SAI1_B"] = {.endpoint = b};
    auto options          = popcorn::testing::stm32_options();
    options.models        = stm32::factories(board, u575());
    if(probe)
    {
      options.models.insert_or_assign(
          "GPDMA1",
          [](popcorn::periph::model_context&,
             popcorn::peripheral_desc const&) -> std::shared_ptr<popcorn::periph::model> {
            return std::make_shared<request_probe>();
          });
    }
    f = std::make_unique<stm32_fixture>(u575(), "sai_ll", std::move(options));
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("sai_ll", "main")})
            .reason
        == stop_reason::address);
    f->set("pc", sym("sai_ll", entry));
  }

  [[nodiscard]] auto level(std::uint32_t request) const -> bool
  {
    auto* const found = f->model_of<request_probe>("GPDMA1");
    REQUIRE(found != nullptr);
    auto const at = found->levels.find(request);
    return at != found->levels.end() and at->second;
  }

  auto finish() const -> std::vector<std::uint32_t>
  {
    auto const stopped = f->run_to_stop();
    CAPTURE(stopped.detail);
    REQUIRE(stopped.reason == stop_reason::user);
    return f->log();
  }

  [[nodiscard]] auto word(std::string_view name, std::uint32_t index = 0) const -> std::uint32_t
  {
    auto const value = f->target->mem().read(sym("sai_ll", name) + 4 * index, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }
};

struct bus
{
  stm32_fixture const& f;

  [[nodiscard]] auto reg(std::string_view peripheral, std::string_view name) const
      -> popcorn::register_desc const&
  {
    auto const& desc  = f.target->peripheral(peripheral).description();
    auto const  found = std::ranges::find(desc.registers, name, &popcorn::register_desc::name);
    REQUIRE(found != desc.registers.end());
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

  [[nodiscard]] auto bits(
      std::string_view                                               name,
      std::vector<std::pair<std::string_view, std::uint32_t>> const& fields) const -> std::uint32_t
  {
    auto const&   described = reg("SAI1", name);
    std::uint32_t value     = 0;
    for(auto const& [field, bits] : fields)
    {
      auto const found = std::ranges::find(described.fields, field, &popcorn::field_desc::name);
      REQUIRE(found != described.fields.end());
      value |= bits << found->lsb;
    }
    return value;
  }

  void
      put(std::string_view                                               name,
          std::vector<std::pair<std::string_view, std::uint32_t>> const& fields) const
  {
    write(at("SAI1", name), bits(name, fields));
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
    set("SAI1", name, field, value);
  }

  [[nodiscard]] auto get(std::string_view name, std::string_view field) const -> std::uint32_t
  {
    return f.field("SAI1", name, field);
  }

  void push(char block, std::uint32_t word) const
  {
    write(at("SAI1", block == 'A' ? "ADR" : "BDR"), word);
  }

  [[nodiscard]] auto pop(char block) const -> std::uint32_t
  {
    auto&      regs  = f.target->peripheral("SAI1");
    auto const value = f.target->model("SAI1")->on_read(
        regs, reg("SAI1", block == 'A' ? "ADR" : "BDR").offset, 4, {.pc = 0, .now = icount{0}});
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

  void clocks(bool enable = true) const
  {
    set("RCC", "RCC_CR", "HSION", 1);
    run_until(now() + 200 * us);
    REQUIRE(f.field("RCC", "RCC_CR", "HSIRDY") == 1);
    set("RCC", "RCC_CCIPR2", "SAI1SEL", 4);
    if(enable)
    {
      set("RCC", "RCC_APB2ENR", "SAI1EN", 1);
    }
  }

  void stereo16(std::string_view prefix, std::uint32_t frame_length, bool fs_definition) const
  {
    put(
        std::string{
            prefix
    }
            + "FRCR",
        {{"FRL", frame_length - 1},
         {"FSALL", frame_length / 2 - 1},
         {"FSDEF", fs_definition ? 1u : 0u}});
    put(
        std::string{
            prefix
    }
            + "SLOTR",
        {{"NBSLOT", 1}, {"SLOTEN", 3}});
  }
};

auto pending(stm32_fixture const& f, std::uint32_t irq) -> bool
{
  auto const word = f.target->mem().read(0xe000'e200u + 4 * (irq / 32), 4);
  REQUIRE(word.has_value());
  return (*word >> (irq % 32) & 1u) != 0;
}

auto starts(std::vector<stm32::captured_audio_frame> const& frames) -> std::vector<std::uint64_t>
{
  std::vector<std::uint64_t> out;
  for(auto const& frame : frames)
  {
    out.push_back(frame.at.ps);
  }
  return out;
}
}

TEST_CASE("a polled master transmitter sends the ramp one 64 us frame apart, twice identical")
{
  std::vector<stm32::captured_audio_frame> first;
  std::vector<std::byte>                   first_state;
  for(int run = 0; run < 2; ++run)
  {
    rig        r{"tx_poll"};
    auto const log = r.finish();
    CHECK(log == std::vector<std::uint32_t>{0});
    auto const& frames = r.a->frames;
    REQUIRE(frames.size() == 64);
    for(std::uint32_t index = 0; index < frames.size(); ++index)
    {
      CAPTURE(index);
      CHECK(
          frames[index].slots == std::vector<std::uint32_t>{ramp(2 * index), ramp(2 * index + 1)});
      if(index > 0)
      {
        CHECK(frames[index].at.ps - frames[index - 1].at.ps == frame_ps);
      }
    }
    CHECK(r.f->field("SAI1", "ACR1", "SAIAEN") == 0);
    auto const state = core::encode_snapshot(r.f->target->snapshot().data());
    if(run == 0)
    {
      first       = frames;
      first_state = state;
    }
    else
    {
      CHECK(frames == first);
      CHECK(state == first_state);
    }
  }
}

TEST_CASE("block B synchronous with block A receives its endpoint's words through the IRQ")
{
  auto const words = source_words(64);
  rig        r{"duplex_irq", words};
  auto const log = r.finish();
  CHECK(log == std::vector<std::uint32_t>{64, 64});
  for(std::uint32_t index = 0; index < 64; ++index)
  {
    CAPTURE(index);
    CHECK(r.word("received", index) == (words[index] & 0xFFFFu));
  }
  auto const& sent = r.a->frames;
  REQUIRE(sent.size() >= 32);
  for(std::uint32_t index = 0; index < 32; ++index)
  {
    CHECK(sent[index].slots == std::vector<std::uint32_t>{ramp(2 * index), ramp(2 * index + 1)});
  }
  REQUIRE(r.b->calls.size() >= 32);
  for(std::uint32_t index = 0; index < 32; ++index)
  {
    CAPTURE(index);
    CHECK(r.b->calls[index].slots == 2);
    CHECK(r.b->calls[index].at == sent[index].at.ps);
  }
}

TEST_CASE("an underrun sends 0 and resumes at the slot it happened in")
{
  rig        r{"underrun"};
  auto const log = r.finish();
  CHECK(log == std::vector<std::uint32_t>{1, 0, 2});
  auto const& frames = r.a->frames;
  REQUIRE(frames.size() == 4);
  CHECK(frames[0].slots == std::vector<std::uint32_t>{ramp(0), ramp(1)});
  CHECK(frames[1].slots == std::vector<std::uint32_t>{ramp(2), 0});
  CHECK(frames[2].slots == std::vector<std::uint32_t>{0, ramp(3)});
  CHECK(frames[3].slots == std::vector<std::uint32_t>{ramp(4), 0});
  for(std::size_t index = 1; index < frames.size(); ++index)
  {
    CHECK(frames[index].at.ps - frames[index - 1].at.ps == frame_ps);
  }
}

TEST_CASE(
    "NODIV = 1 frames last MCKDIV x (FRL + 1) kernel cycles, slots take words when they start")
{
  rig r{"idle"};
  bus b{*r.f};
  b.clocks();
  b.stereo16("A", 64, false);
  b.put(
      "ACR1", {
                  {"DS",     4},
                  {"NODIV",  1},
                  {"MCKDIV", 5}
  });
  for(std::uint32_t index = 0; index < 3; ++index)
  {
    b.push('A', ramp(index));
  }
  CHECK(b.get("ASR", "FLVL") == 2);
  b.set("ACR1", "SAIAEN", 1);
  auto const start = b.now();
  CHECK(b.get("ASR", "FLVL") == 1);
  b.run_until(start + 21 * us);
  CHECK(b.get("ASR", "FLVL") == 0);
  b.run_until(start + 50 * us);
  auto const& frames = r.a->frames;
  REQUIRE(frames.size() == 2);
  CHECK(starts(frames) == std::vector<std::uint64_t>{start, start + 20 * us});
  CHECK(frames[0].slots == std::vector<std::uint32_t>{ramp(0), ramp(1)});
  CHECK(frames[1].slots == std::vector<std::uint32_t>{ramp(2), 0});
  CHECK(b.get("ASR", "OVRUDR") == 1);
  b.put(
      "ACLRFR", {
                    {"COVRUDR", 1}
  });
  CHECK(b.get("ASR", "OVRUDR") == 0);
}

TEST_CASE("a master with NODIV = 0 and a frame length not a power of 2 sets WCKCFG and stays off")
{
  rig r{"idle"};
  bus b{*r.f};
  b.clocks();
  b.stereo16("A", 24, false);
  b.put(
      "AIM", {
                 {"WCKCFGIE", 1}
  });
  b.put(
      "ACR1", {
                  {"MODE",   1},
                  {"DS",     4},
                  {"SAIAEN", 1}
  });
  CHECK(b.get("ACR1", "SAIAEN") == 0);
  CHECK(b.get("ASR", "WCKCFG") == 1);
  CHECK(pending(*r.f, sai_irq));
  b.put(
      "ACLRFR", {
                    {"CWCKCFG", 1}
  });
  CHECK(b.get("ASR", "WCKCFG") == 0);
  b.run_until(b.now() + 500 * us);
  CHECK(r.a->calls.empty());
  b.stereo16("A", 4, false);
  b.set("ACR1", "SAIAEN", 1);
  CHECK(b.get("ACR1", "SAIAEN") == 0);
  CHECK(b.get("ASR", "WCKCFG") == 1);
  b.put(
      "ACLRFR", {
                    {"CWCKCFG", 1}
  });
  b.stereo16("A", 32, false);
  b.put(
      "ACR1", {
                  {"MODE",   1},
                  {"DS",     4},
                  {"NODIV",  1},
                  {"SAIAEN", 1}
  });
  auto const start = b.now();
  CHECK(b.get("ACR1", "SAIAEN") == 1);
  CHECK(b.get("ASR", "WCKCFG") == 0);
  b.run_until(start + 4 * us);
  REQUIRE(r.a->calls.size() == 2);
  CHECK(r.a->calls[1].at - r.a->calls[0].at == 2 * us);
}

TEST_CASE("FLVL and FREQ follow the FIFO level and FTH while the transmitter is off")
{
  rig b_rig{"idle"};
  bus b{*b_rig.f};
  CHECK(b.get("ASR", "FREQ") == 1);
  constexpr std::array<std::uint32_t, 9> levels{0, 1, 1, 2, 2, 3, 3, 4, 5};
  for(std::uint32_t threshold = 0; threshold <= 4; ++threshold)
  {
    b.put(
        "ACR2", {
                    {"FTH",    threshold},
                    {"FFLUSH", 1        }
    });
    CHECK(b.get("ACR2", "FFLUSH") == 0);
    for(std::uint32_t count = 0; count <= 9; ++count)
    {
      CAPTURE(threshold);
      CAPTURE(count);
      auto const level = levels[std::min<std::uint32_t>(count, 8)];
      CHECK(b.get("ASR", "FLVL") == level);
      CHECK(b.get("ASR", "FREQ") == (level <= threshold ? 1u : 0u));
      b.push('A', count);
    }
  }
}

TEST_CASE("a master receiver fills the FIFO, overruns and resumes at the overrun slot")
{
  auto const words = source_words(64);
  rig        r{"idle", words};
  bus        b{*r.f};
  b.clocks();
  b.stereo16("B", 32, true);
  b.put(
      "BCR2", {
                  {"FTH", 1}
  });
  b.put(
      "BCR1", {
                  {"MODE",   1},
                  {"DS",     4},
                  {"MCKDIV", 4},
                  {"SAIAEN", 1}
  });
  auto const start = b.now();
  CHECK(b.get("BSR", "FREQ") == 0);
  b.run_until(start + 20 * us);
  CHECK(r.b->calls.empty());
  b.run_until(start + 33 * us);
  CHECK(b.get("BSR", "FLVL") == 1);
  CHECK(b.get("BSR", "FREQ") == 0);
  REQUIRE(r.b->calls.size() == 1);
  CHECK(r.b->calls[0] == received_call{.slots = 2, .at = start});
  b.run_until(start + frame_ps + 1);
  CHECK(b.get("BSR", "FLVL") == 2);
  CHECK(b.get("BSR", "FREQ") == 1);
  b.run_until(start + 4 * frame_ps + 1);
  CHECK(b.get("BSR", "FLVL") == 5);
  CHECK(b.get("BSR", "OVRUDR") == 0);
  b.run_until(start + 4 * frame_ps + 33 * us);
  CHECK(b.get("BSR", "OVRUDR") == 1);
  CHECK(b.pop('B') == (words[0] & 0xFFFFu));
  b.run_until(start + 5 * frame_ps + 1);
  CHECK(b.get("BSR", "FLVL") == 4);
  for(std::uint32_t index = 1; index < 8; ++index)
  {
    CHECK(b.pop('B') == (words[index] & 0xFFFFu));
  }
  CHECK(b.get("BSR", "FLVL") == 0);
  b.run_until(start + 6 * frame_ps + 1);
  CHECK(b.read(b.at("SAI1", "BDR")) == (words[10] & 0xFFFFu));
  b.push('B', 0x55);
  CHECK(b.get("BSR", "FLVL") == 2);
  CHECK(b.pop('B') == (words[10] & 0xFFFFu));
  CHECK(b.pop('B') == (words[11] & 0xFFFFu));
  CHECK(b.pop('B') == 0);
}

TEST_CASE("a slave block runs at the board's external frame rate, none without it")
{
  SUBCASE("external clock")
  {
    rig r{
        "idle", {},
         {.decimation = 1, .external_frame_hz = 16'000}
    };
    bus b{*r.f};
    b.stereo16("A", 32, false);
    b.push('A', ramp(0));
    b.push('A', ramp(1));
    b.put(
        "ACR1", {
                    {"MODE",   2},
                    {"DS",     4},
                    {"SAIAEN", 1}
    });
    auto const start = b.now();
    b.run_until(start + 130 * us);
    REQUIRE(r.a->frames.size() == 2);
    CHECK(starts(r.a->frames) == std::vector<std::uint64_t>{start, start + 62'500'000});
    CHECK(r.a->frames[0].slots == std::vector<std::uint32_t>{ramp(0), ramp(1)});
  }
  SUBCASE("no external clock")
  {
    rig                           r{"idle"};
    bus                           b{*r.f};
    std::vector<core::log_record> logged;
    auto const                    sink = r.f->target->add_log_sink(
        [&](core::log_record const& record) { logged.push_back(record); });
    b.stereo16("A", 32, false);
    b.push('A', ramp(0));
    b.put(
        "ACR1", {
                    {"MODE",   2},
                    {"DS",     4},
                    {"SAIAEN", 1}
    });
    b.run_until(b.now() + 500 * us);
    CHECK(r.a->frames.empty());
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].kind == core::log_kind::model_warning);
    CHECK(logged[0].peripheral == "SAI1");
    CHECK(logged[0].message.contains("external frame rate"));
  }
}

namespace
{
constexpr std::uint64_t slave_frame = 62'500'000;
constexpr auto          clock_a     = stm32::audio_block::a;

auto port_of(rig const& r) -> stm32::audio_port&
{
  auto* const port = dynamic_cast<stm32::audio_port*>(r.f->target->model("SAI1"));
  REQUIRE(port != nullptr);
  return *port;
}

void slave_a(bus const& b)
{
  b.stereo16("A", 32, false);
  for(std::uint32_t index = 0; index < 8; ++index)
  {
    b.push('A', ramp(index));
  }
  b.put(
      "ACR1", {
                  {"MODE",   2},
                  {"DS",     4},
                  {"SAIAEN", 1}
  });
}

void sync_b(bus const& b)
{
  b.stereo16("B", 32, false);
  b.put(
      "BCR1", {
                  {"MODE",   3},
                  {"DS",     4},
                  {"SYNCEN", 1},
                  {"SAIAEN", 1}
  });
}

auto since(std::vector<stm32::captured_audio_frame> const& frames, std::uint64_t origin)
    -> std::vector<std::int64_t>
{
  std::vector<std::int64_t> out;
  for(auto const& frame : frames)
  {
    out.push_back(static_cast<std::int64_t>(frame.at.ps) - static_cast<std::int64_t>(origin));
  }
  return out;
}

auto since(std::vector<received_call> const& calls, std::uint64_t origin)
    -> std::vector<std::int64_t>
{
  std::vector<std::int64_t> out;
  for(auto const& call : calls)
  {
    out.push_back(static_cast<std::int64_t>(call.at) - static_cast<std::int64_t>(origin));
  }
  return out;
}

auto tail(std::vector<stm32::captured_audio_frame> const& frames, std::size_t from)
    -> std::vector<stm32::captured_audio_frame>
{
  return {frames.begin() + static_cast<std::ptrdiff_t>(from), frames.end()};
}

auto restored(core::snapshot const& saved) -> core::snapshot
{
  return core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))};
}
}

TEST_CASE("an external clock started at run time frames a waiting slave pair from that instant")
{
  rig   r{"idle"};
  bus   b{*r.f};
  auto& port = port_of(r);
  sync_b(b);
  slave_a(b);
  b.run_until(b.now() + 100 * us);
  CHECK(r.a->frames.empty());
  CHECK(r.b->calls.empty());
  CHECK(port.external_clock(clock_a) == 0);
  auto const t = b.now();
  port.set_external_clock(clock_a, 16'000);
  CHECK(port.external_clock(clock_a) == 16'000);
  CHECK(port.external_clock(stm32::audio_block::b) == 0);
  b.run_until(t + 130 * us);
  CHECK(since(r.a->frames, t) == std::vector<std::int64_t>{0, slave_frame});
  REQUIRE_FALSE(r.a->frames.empty());
  CHECK(r.a->frames[0].slots == std::vector<std::uint32_t>{ramp(0), ramp(1)});
  CHECK(since(r.b->calls, t) == std::vector<std::int64_t>{0, slave_frame});
}

TEST_CASE("a stopped external clock ends the frame in progress, a restart frames at once")
{
  rig   r{"idle"};
  bus   b{*r.f};
  auto& port = port_of(r);
  slave_a(b);
  auto const t = b.now();
  port.set_external_clock(clock_a, 16'000);
  b.run_until(t + 100 * us);
  port.set_external_clock(clock_a, 0);
  CHECK(port.external_clock(clock_a) == 0);
  b.run_until(t + 300 * us);
  CHECK(since(r.a->frames, t) == std::vector<std::int64_t>{0, slave_frame});
  CHECK(b.get("ACR1", "SAIAEN") == 1);
  auto const again = b.now();
  port.set_external_clock(clock_a, 16'000);
  b.run_until(again + 70 * us);
  REQUIRE(r.a->frames.size() == 3);
  CHECK(r.a->frames[2].at.ps == again);
}

TEST_CASE("an external rate change takes effect at the end of the frame in progress")
{
  rig   r{"idle"};
  bus   b{*r.f};
  auto& port = port_of(r);
  slave_a(b);
  auto const t = b.now();
  port.set_external_clock(clock_a, 16'000);
  b.run_until(t + 100 * us);
  port.set_external_clock(clock_a, 8'000);
  CHECK(port.external_clock(clock_a) == 8'000);
  b.run_until(t + 400 * us);
  CHECK(
      since(r.a->frames, t)
      == std::vector<std::int64_t>{0, slave_frame, 2 * slave_frame, 4 * slave_frame});
}

TEST_CASE("a slave pair enabled after the external clock started waits for its next frame")
{
  rig        r{"idle"};
  bus        b{*r.f};
  auto&      port = port_of(r);
  auto const t    = b.now();
  port.set_external_clock(clock_a, 16'000);
  b.run_until(t + 70 * us);
  slave_a(b);
  b.run_until(t + 100 * us);
  sync_b(b);
  CHECK(r.a->frames.empty());
  CHECK(b.get("ASR", "FLVL") == 5);
  b.run_until(t + 200 * us);
  CHECK(since(r.a->frames, t) == std::vector<std::int64_t>{2 * slave_frame});
  CHECK(since(r.b->calls, t) == std::vector<std::int64_t>{2 * slave_frame});
}

TEST_CASE("a build-time external clock takes its phase at the first enable, re-enables align")
{
  rig   r{"idle", {}, {.external_frame_hz = 16'000}};
  bus   b{*r.f};
  auto& port = port_of(r);
  CHECK(port.external_clock(clock_a) == 16'000);
  slave_a(b);
  auto const start = b.now();
  b.run_until(start + 100 * us);
  b.set("ACR1", "SAIAEN", 0);
  b.run_until(start + 200 * us);
  CHECK(b.get("ACR1", "SAIAEN") == 0);
  b.set("ACR1", "SAIAEN", 1);
  b.run_until(start + 380 * us);
  CHECK(
      since(r.a->frames, start)
      == std::vector<std::int64_t>{0, slave_frame, 4 * slave_frame, 5 * slave_frame});
}

TEST_CASE("a power-on reset powers the codec down: its clock returns to the wiring, no phase")
{
  rig   r{"idle", {}, {.external_frame_hz = 8'000}};
  bus   b{*r.f};
  auto& port = port_of(r);
  slave_a(b);
  port.set_external_clock(clock_a, 16'000);
  b.run_until(b.now() + 100 * us);
  r.f->target->reset(popcorn::reset_kind::power_on);
  CHECK(port.external_clock(clock_a) == 8'000);
  REQUIRE(
      r.f->run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("sai_ll", "main")})
          .reason
      == stop_reason::address);
  r.f->set("pc", sym("sai_ll", "idle"));
  b.run_until(b.now() + 30 * us);
  auto const before = r.a->frames.size();
  slave_a(b);
  auto const enabled_at = b.now();
  b.run_until(enabled_at + 130 * us);
  REQUIRE(r.a->frames.size() == before + 1);
  CHECK(r.a->frames[before].at.ps == enabled_at);
}

TEST_CASE("a pin reset keeps the codec clock running in phase, the next enable joins it")
{
  rig   r{"idle"};
  bus   b{*r.f};
  auto& port  = port_of(r);
  auto& clock = r.f->target->virtual_clock();
  slave_a(b);
  auto const started = clock.since_power_on().ps;
  port.set_external_clock(clock_a, 16'000);
  b.run_until(b.now() + 100 * us);
  r.f->target->reset(popcorn::reset_kind::pin);
  CHECK(port.external_clock(clock_a) == 16'000);
  REQUIRE(
      r.f->run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("sai_ll", "main")})
          .reason
      == stop_reason::address);
  r.f->set("pc", sym("sai_ll", "idle"));
  b.run_until(b.now() + 30 * us);
  auto const before = r.a->frames.size();
  slave_a(b);
  auto const offset     = clock.since_power_on().ps - b.now();
  auto const enabled_at = clock.since_power_on().ps;
  auto const due = started + (enabled_at - started + slave_frame - 1) / slave_frame * slave_frame;
  CHECK(due > enabled_at);
  b.run_until(due - offset + 70 * us);
  REQUIRE(r.a->frames.size() == before + 1);
  CHECK(r.a->frames[before].at.ps + offset == due);
}

TEST_CASE("a rate change on a block waiting for its first frame holds after the block is disabled")
{
  rig        r{"idle"};
  bus        b{*r.f};
  auto&      port = port_of(r);
  auto const t    = b.now();
  port.set_external_clock(clock_a, 16'000);
  b.run_until(t + 130 * us);
  slave_a(b);
  b.run_until(t + 140 * us);
  port.set_external_clock(clock_a, 8'000);
  b.run_until(t + 150 * us);
  b.set("ACR1", "SAIAEN", 0);
  CHECK(b.get("ACR1", "SAIAEN") == 0);
  b.run_until(t + 200 * us);
  b.set("ACR1", "SAIAEN", 1);
  b.run_until(t + 450 * us);
  CHECK(since(r.a->frames, t) == std::vector<std::int64_t>{5 * slave_frame});
}

TEST_CASE("a stop and a start within one frame keep the phase of a clock no block uses")
{
  rig        r{"idle"};
  bus        b{*r.f};
  auto&      port = port_of(r);
  auto const t    = b.now();
  port.set_external_clock(clock_a, 16'000);
  b.run_until(t + 70 * us);
  port.set_external_clock(clock_a, 0);
  b.run_until(t + 100 * us);
  port.set_external_clock(clock_a, 16'000);
  b.run_until(t + 150 * us);
  slave_a(b);
  b.run_until(t + 260 * us);
  CHECK(since(r.a->frames, t) == std::vector<std::int64_t>{3 * slave_frame});

  SUBCASE("a start after the stop's frame end takes a new phase")
  {
    port.set_external_clock(clock_a, 0);
    b.set("ACR1", "SAIAEN", 0);
    b.run_until(t + 400 * us);
    port.set_external_clock(clock_a, 16'000);
    auto const again = b.now();
    b.run_until(again + 20 * us);
    b.set("ACR1", "SAIAEN", 1);
    b.run_until(again + 130 * us);
    REQUIRE_FALSE(r.a->frames.empty());
    CHECK(r.a->frames.back().at.ps == again + slave_frame);
  }
}

TEST_CASE("a stop and a start within one frame keep a running block framing in phase")
{
  rig   r{"idle"};
  bus   b{*r.f};
  auto& port = port_of(r);
  slave_a(b);
  auto const t = b.now();
  port.set_external_clock(clock_a, 16'000);
  b.run_until(t + 100 * us);
  port.set_external_clock(clock_a, 0);
  b.run_until(t + 110 * us);
  port.set_external_clock(clock_a, 16'000);
  b.run_until(t + 260 * us);
  CHECK(
      since(r.a->frames, t)
      == std::vector<std::int64_t>{0, slave_frame, 2 * slave_frame, 3 * slave_frame});
}

TEST_CASE("the external clock is snapshot state, in place and into a machine built without it")
{
  rig   first{"idle"};
  bus   b{*first.f};
  auto& port = port_of(first);
  slave_a(b);
  auto const stopped = first.f->target->snapshot();
  auto const t       = b.now();
  port.set_external_clock(clock_a, 16'000);
  b.run_until(t + 100 * us);
  auto const saved = first.f->target->snapshot();
  auto const done  = first.a->frames.size();
  b.run_until(t + 400 * us);
  auto const rest = tail(first.a->frames, done);
  REQUIRE(rest.size() == 5);

  first.f->target->restore(saved);
  first.a->frames.clear();
  CHECK(port.external_clock(clock_a) == 16'000);
  b.run_until(t + 400 * us);
  CHECK(first.a->frames == rest);

  first.f->target->restore(stopped);
  first.a->frames.clear();
  CHECK(port.external_clock(clock_a) == 0);
  b.run_until(b.now() + 300 * us);
  CHECK(first.a->frames.empty());

  rig other{"idle"};
  bus o{*other.f};
  other.f->target->restore(restored(saved));
  CHECK(port_of(other).external_clock(clock_a) == 16'000);
  o.run_until(t + 400 * us);
  CHECK(other.a->frames == rest);
}

TEST_CASE("a snapshot keeps the phase of a running external clock no block uses yet")
{
  rig        first{"idle"};
  bus        b{*first.f};
  auto const t = b.now();
  port_of(first).set_external_clock(clock_a, 16'000);
  b.run_until(t + 70 * us);
  auto const saved = first.f->target->snapshot();
  slave_a(b);
  b.run_until(t + 200 * us);
  CHECK(since(first.a->frames, t) == std::vector<std::int64_t>{2 * slave_frame});

  rig other{"idle"};
  bus o{*other.f};
  other.f->target->restore(restored(saved));
  slave_a(o);
  o.run_until(t + 200 * us);
  CHECK(since(other.a->frames, t) == std::vector<std::int64_t>{2 * slave_frame});
}

namespace
{
struct saved_block
{
  std::uint32_t              generation{};
  std::vector<std::uint32_t> fifo;
  std::vector<std::uint32_t> frame;
  std::uint64_t              anchor_ps{};
  std::uint64_t              frames{};
  std::uint64_t              next_ps{};
  std::uint64_t              hz{};
  std::uint64_t              cycles{};
  std::uint32_t              bits{};
  std::uint32_t              position{};
  std::int8_t                resume_slot{-1};
  bool                       ticking{};
  bool                       stalled{};
  bool                       active{};
  bool                       stopping{};
  bool                       waiting{};
};

struct saved_clock
{
  std::uint32_t hz{};
  std::uint64_t anchor_ps{};
  bool          anchored{};
};

struct new_sai_state
{
  std::array<saved_block, 2>                block{};
  std::optional<std::array<saved_clock, 2>> clock{};
};

struct old_sai_state
{
  std::array<saved_block, 2> block{};
};

auto without_clock(core::snapshot const& saved) -> core::snapshot
{
  auto data = core::decode_snapshot(core::encode_snapshot(saved.data()));
  auto part = std::ranges::find(data.models, std::string{"SAI1"}, &core::snapshot_part::owner);
  REQUIRE(part != data.models.end());
  auto const now = reflex::serde::bson::deserializer{
      std::span<std::byte const>{
          part->data}}.load<new_sai_state>();
  REQUIRE(now.clock.has_value());
  REQUIRE(now.block[0].ticking);
  part->data.clear();
  reflex::serde::bson::serializer{part->data}.dump(old_sai_state{.block = now.block});
  return core::snapshot{std::move(data)};
}
}

TEST_CASE("an SAI state saved before the external clock loads and keeps the machine's clock")
{
  rig first{"idle", {}, {.external_frame_hz = 16'000}};
  bus b{*first.f};
  slave_a(b);
  auto const start = b.now();
  b.run_until(start + 100 * us);
  auto const old  = without_clock(first.f->target->snapshot());
  auto const done = first.a->frames.size();
  b.run_until(start + 400 * us);
  auto const rest = tail(first.a->frames, done);
  REQUIRE(rest.size() == 5);

  SUBCASE("built with the rate: the block runs on, its next enable takes a new phase")
  {
    rig other{"idle", {}, {.external_frame_hz = 16'000}};
    bus o{*other.f};
    other.f->target->restore(old);
    CHECK(port_of(other).external_clock(clock_a) == 16'000);
    o.run_until(start + 400 * us);
    CHECK(other.a->frames == rest);
    o.set("ACR1", "SAIAEN", 0);
    o.run_until(start + 470 * us);
    CHECK(o.get("ACR1", "SAIAEN") == 0);
    o.set("ACR1", "SAIAEN", 1);
    auto const enabled_at = o.now();
    o.run_until(enabled_at + 70 * us);
    REQUIRE_FALSE(other.a->frames.empty());
    CHECK(other.a->frames.back().at.ps == enabled_at);
  }
  SUBCASE("built without a rate: the block ends its frame and waits")
  {
    rig other{"idle"};
    bus o{*other.f};
    other.f->target->restore(old);
    CHECK(port_of(other).external_clock(clock_a) == 0);
    o.run_until(start + 400 * us);
    CHECK(other.a->frames == std::vector<stm32::captured_audio_frame>{rest[0]});
    CHECK(o.get("ACR1", "SAIAEN") == 1);
  }
}

TEST_CASE("frame sync errors never rise on an external clock, their enables log nothing")
{
  rig                           r{"idle", {}, {.external_frame_hz = 16'000}};
  bus                           b{*r.f};
  std::vector<core::log_record> logged;
  auto const                    sink =
      r.f->target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  b.put(
      "AIM", {
                 {"AFSDETIE", 1},
                 {"LFSDETIE", 1}
  });
  slave_a(b);
  b.run_until(b.now() + 100 * slave_frame);
  CHECK(r.a->frames.size() == 100);
  CHECK(b.get("ASR", "AFSDET") == 0);
  CHECK(b.get("ASR", "LFSDET") == 0);
  CHECK_FALSE(pending(*r.f, sai_irq));
  CHECK(logged.empty());
}

TEST_CASE("an attachment with an external rate starts a slave block waiting for a clock")
{
  rig   r{"idle"};
  bus   b{*r.f};
  auto& port = port_of(r);
  slave_a(b);
  b.run_until(b.now() + 100 * us);
  CHECK(r.a->frames.empty());
  auto const t = b.now();
  port.attach(clock_a, {.endpoint = r.a, .external_frame_hz = 16'000});
  CHECK(port.external_clock(clock_a) == 16'000);
  b.run_until(t + 130 * us);
  CHECK(since(r.a->frames, t) == std::vector<std::int64_t>{0, slave_frame});
}

TEST_CASE("a slave block whose clock a device drives waits for it without a warning")
{
  rig                           r{"idle", source_words(256), {.clock_wired = true}};
  bus                           b{*r.f};
  std::vector<core::log_record> logged;
  auto const                    sink =
      r.f->target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  auto& port = port_of(r);
  slave_a(b);
  b.run_until(b.now() + 100 * us);
  CHECK(r.a->frames.empty());
  CHECK(logged.empty());
  auto const t = b.now();
  port.set_external_clock(clock_a, 16'000);
  b.run_until(t + 130 * us);
  CHECK(since(r.a->frames, t) == std::vector<std::int64_t>{0, slave_frame});
  CHECK(logged.empty());
}

TEST_CASE("decimation stretches the frame time and is logged once")
{
  rig                           r{"idle", {}, {.decimation = 8}};
  bus                           b{*r.f};
  std::vector<core::log_record> logged;
  auto const                    sink =
      r.f->target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  b.clocks();
  b.stereo16("A", 64, false);
  b.put(
      "ACR1", {
                  {"DS",     4},
                  {"NODIV",  1},
                  {"MCKDIV", 5}
  });
  for(std::uint32_t index = 0; index < 4; ++index)
  {
    b.push('A', ramp(index));
  }
  b.set("ACR1", "SAIAEN", 1);
  auto const start = b.now();
  b.run_until(start + 330 * us);
  CHECK(starts(r.a->frames) == std::vector<std::uint64_t>{start, start + 160 * us});
  CHECK(
      std::ranges::count_if(
          logged,
          [](core::log_record const& record) { return record.message.contains("decimated by 8"); })
      == 1);
}

TEST_CASE("clearing SAIEN takes effect at the end of the frame in progress")
{
  rig r{"idle"};
  bus b{*r.f};
  b.clocks();
  b.stereo16("B", 32, false);
  b.put(
      "BCR1", {
                  {"MODE",   1},
                  {"DS",     4},
                  {"MCKDIV", 4},
                  {"SAIAEN", 1}
  });
  auto const start = b.now();
  b.run_until(start + 10 * us);
  b.set("BCR1", "SAIAEN", 0);
  CHECK(b.get("BCR1", "SAIAEN") == 1);
  b.run_until(start + frame_ps - us);
  CHECK(b.get("BCR1", "SAIAEN") == 1);
  b.run_until(start + frame_ps + 1);
  CHECK(b.get("BCR1", "SAIAEN") == 0);
  CHECK(b.get("BSR", "FLVL") == 2);
  b.run_until(start + 3 * frame_ps);
  CHECK(r.b->calls.size() == 1);
}

TEST_CASE("mono mode sends slot 0's word in both slots, the data size masks the words")
{
  rig r{"idle"};
  bus b{*r.f};
  b.clocks();
  b.stereo16("A", 32, false);
  b.put(
      "ACR1", {
                  {"DS",     2},
                  {"NODIV",  1},
                  {"MCKDIV", 5},
                  {"MONO",   1}
  });
  b.push('A', 0x1234);
  b.push('A', 0x56);
  b.push('A', 0x78);
  b.set("ACR1", "SAIAEN", 1);
  auto const start = b.now();
  b.run_until(start + 21 * us);
  REQUIRE(r.a->frames.size() == 2);
  CHECK(r.a->frames[0].slots == std::vector<std::uint32_t>{0x34, 0x34});
  CHECK(r.a->frames[1].slots == std::vector<std::uint32_t>{0x56, 0x56});
  CHECK(
      r.a->frames[0].format
      == stm32::audio_format{.data_bits = 8, .active_slots = 3, .mono = true});
  CHECK(b.get("ASR", "OVRUDR") == 0);
}

TEST_CASE("without a kernel clock the block waits, it starts when the clock comes")
{
  rig                           r{"idle"};
  bus                           b{*r.f};
  std::vector<core::log_record> logged;
  auto const                    sink =
      r.f->target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  b.clocks(false);
  b.stereo16("B", 32, false);
  b.put(
      "BCR1", {
                  {"MODE",   1},
                  {"DS",     4},
                  {"MCKDIV", 4},
                  {"SAIAEN", 1}
  });
  b.run_until(b.now() + 200 * us);
  CHECK(r.b->calls.empty());
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].message.contains("no kernel clock"));
  b.set("RCC", "RCC_APB2ENR", "SAI1EN", 1);
  auto const start = b.now();
  b.run_until(start + 2 * frame_ps + 1);
  REQUIRE(r.b->calls.size() == 2);
  CHECK(r.b->calls[0].at == start);
  CHECK(r.b->calls[1].at - r.b->calls[0].at == frame_ps);
  b.set("RCC", "RCC_APB2ENR", "SAI1EN", 0);
  b.run_until(start + 5 * frame_ps + 1);
  CHECK(r.b->calls.size() == 3);
  b.set("RCC", "RCC_APB2ENR", "SAI1EN", 1);
  auto const again = b.now();
  b.run_until(again + frame_ps);
  REQUIRE(r.b->calls.size() == 4);
  CHECK(r.b->calls[3].at == again);
}

TEST_CASE("a snapshot mid-stream restored into a fresh machine sends the same rest")
{
  rig first{"tx_poll"};
  bus b{*first.f};
  b.run_until(b.now() + 20 * frame_ps);
  auto const done  = first.a->frames.size();
  auto const saved = first.f->target->snapshot();
  first.finish();
  std::vector<stm32::captured_audio_frame> rest{
      first.a->frames.begin() + static_cast<std::ptrdiff_t>(done), first.a->frames.end()};
  REQUIRE(rest.size() > 20);
  rig other{"idle"};
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  auto const log = other.finish();
  CHECK(log == std::vector<std::uint32_t>{0});
  CHECK(other.a->frames == rest);
}

TEST_CASE("a machine reset stops the stream and clears the FIFO")
{
  rig r{"idle"};
  bus b{*r.f};
  b.clocks();
  b.stereo16("B", 32, false);
  b.put(
      "BCR1", {
                  {"MODE",   1},
                  {"DS",     4},
                  {"MCKDIV", 4},
                  {"SAIAEN", 1}
  });
  b.run_until(b.now() + 3 * frame_ps);
  auto const calls = r.b->calls.size();
  r.f->target->reset();
  CHECK(b.get("BCR1", "SAIAEN") == 0);
  CHECK(b.get("BSR", "FLVL") == 0);
  CHECK(b.get("ASR", "FREQ") == 1);
  REQUIRE(
      r.f->run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("sai_ll", "main")})
          .reason
      == stop_reason::address);
  r.f->set("pc", sym("sai_ll", "idle"));
  b.run_until(b.now() + 3 * frame_ps);
  CHECK(r.b->calls.size() == calls);
}

TEST_CASE("FSDEF is writable and starts the second half of the slots mid-frame")
{
  rig r{"idle"};
  bus b{*r.f};
  b.clocks();
  b.stereo16("A", 64, true);
  CHECK(b.get("AFRCR", "FSDEF") == 1);
  b.put(
      "ACR1", {
                  {"DS",     4},
                  {"NODIV",  1},
                  {"MCKDIV", 5}
  });
  b.push('A', ramp(0));
  b.push('A', ramp(1));
  b.set("ACR1", "SAIAEN", 1);
  auto const start = b.now();
  b.run_until(start + 7 * us);
  CHECK(b.get("ASR", "FLVL") == 1);
  b.run_until(start + 11 * us);
  CHECK(b.get("ASR", "FLVL") == 0);
  b.put(
      "AFRCR", {
                   {"FRL",   63},
                   {"FSALL", 31}
  });
  CHECK(b.get("AFRCR", "FSDEF") == 0);
}

TEST_CASE("endpoints attached through the audio port replace the board's, on SAI1 and SAI2")
{
  rig   r{"idle"};
  bus   b{*r.f};
  auto* port = dynamic_cast<stm32::audio_port*>(r.f->target->model("SAI1"));
  REQUIRE(port != nullptr);
  CHECK(dynamic_cast<stm32::audio_port*>(r.f->target->model("SAI2")) != nullptr);
  auto const capture = std::make_shared<stm32::audio_capture_endpoint>();
  auto const source =
      std::make_shared<stm32::audio_source_endpoint>(std::vector<std::uint32_t>{7, 8, 9});
  port->attach(stm32::audio_block::a, {.endpoint = capture});
  port->attach(stm32::audio_block::b, {.endpoint = source});
  b.clocks();
  b.stereo16("A", 32, false);
  b.put(
      "ASLOTR", {
                    {"NBSLOT", 1},
                    {"SLOTEN", 5}
  });
  b.stereo16("B", 32, false);
  b.put(
      "BCR1", {
                  {"MODE",   3},
                  {"DS",     4},
                  {"SYNCEN", 1},
                  {"SAIAEN", 1}
  });
  b.put(
      "ACR1", {
                  {"DS",     4},
                  {"NODIV",  1},
                  {"MCKDIV", 5}
  });
  b.push('A', ramp(0));
  b.push('A', ramp(1));
  b.set("ACR1", "SAIAEN", 1);
  auto const start = b.now();
  b.run_until(start + 21 * us);
  REQUIRE(capture->frames.size() == 2);
  CHECK(
      capture->frames[0]
      == stm32::captured_audio_frame{
          .slots  = {ramp(0), 0},
          .format = {.data_bits = 16, .active_slots = 1, .mono = false},
          .at     = vtime{start}
  });
  CHECK(source->served() == 3);
  CHECK(b.pop('B') == 7);
  CHECK(b.pop('B') == 8);
  CHECK(b.pop('B') == 9);
  CHECK(b.pop('B') == 0);
  CHECK(r.a->frames.empty());
  CHECK(r.b->calls.empty());
  port->attach(stm32::audio_block::b, {});
  b.run_until(start + 31 * us);
  CHECK(b.pop('B') == 0);
  CHECK(b.get("BSR", "FLVL") == 1);
}

TEST_CASE("a frame keeps the timing it started with, the next frames take the new one")
{
  rig r{"idle"};
  bus b{*r.f};
  b.clocks();
  b.stereo16("B", 32, false);
  b.put(
      "BCR1", {
                  {"MODE",   1},
                  {"DS",     4},
                  {"NODIV",  1},
                  {"MCKDIV", 5},
                  {"SAIAEN", 1}
  });
  auto const start = b.now();
  b.run_until(start + 15 * us);
  b.set("BCR1", "MCKDIV", 10);
  b.run_until(start + 51 * us);
  std::vector<std::uint64_t> at;
  for(auto const& call : r.b->calls)
  {
    at.push_back(call.at - start);
  }
  CHECK(at == std::vector<std::uint64_t>{0, 10 * us, 20 * us, 40 * us});
}

TEST_CASE("a write to a full FIFO is dropped, a mono receiver keeps slot 0 only")
{
  rig r{
      "idle", {1, 2, 3, 4, 5, 6}
  };
  bus b{*r.f};
  b.clocks();
  b.stereo16("A", 32, false);
  b.stereo16("B", 32, false);
  b.put(
      "ACR1", {
                  {"DS",     4},
                  {"NODIV",  1},
                  {"MCKDIV", 5}
  });
  for(std::uint32_t index = 0; index < 9; ++index)
  {
    b.push('A', ramp(index));
  }
  b.put(
      "BCR1", {
                  {"MODE",   3},
                  {"DS",     4},
                  {"SYNCEN", 1},
                  {"MONO",   1},
                  {"SAIAEN", 1}
  });
  b.set("ACR1", "SAIAEN", 1);
  auto const start = b.now();
  b.run_until(start + 51 * us);
  REQUIRE(r.a->frames.size() == 5);
  CHECK(r.a->frames[3].slots == std::vector<std::uint32_t>{ramp(6), ramp(7)});
  CHECK(r.a->frames[4].slots == std::vector<std::uint32_t>{0, 0});
  CHECK(b.pop('B') == 1);
  CHECK(b.pop('B') == 3);
  CHECK(b.pop('B') == 5);
}

TEST_CASE("the IRQ-driven firmware runs give the same frames, log and state twice")
{
  for(std::string_view const entry : {"duplex_irq", "underrun", "dma"})
  {
    CAPTURE(entry);
    std::vector<stm32::captured_audio_frame> frames;
    std::vector<received_call>               calls;
    std::vector<std::uint32_t>               log;
    std::vector<std::byte>                   state;
    for(int run = 0; run < 2; ++run)
    {
      rig        r{entry};
      auto const now   = r.finish();
      auto const saved = core::encode_snapshot(r.f->target->snapshot().data());
      if(run == 0)
      {
        frames = r.a->frames;
        calls  = r.b->calls;
        log    = now;
        state  = saved;
        continue;
      }
      CHECK(r.a->frames == frames);
      CHECK(r.b->calls == calls);
      CHECK(now == log);
      CHECK(saved == state);
    }
  }
}

TEST_CASE("a frame shorter than 8 bits never runs and is logged, WCKCFG stays for NODIV = 0")
{
  rig                           r{"idle"};
  bus                           b{*r.f};
  std::vector<core::log_record> logged;
  auto const                    sink =
      r.f->target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  b.clocks();
  b.stereo16("B", 4, false);
  b.put(
      "BCR1", {
                  {"MODE",   1},
                  {"DS",     2},
                  {"NODIV",  1},
                  {"MCKDIV", 5},
                  {"SAIAEN", 1}
  });
  CHECK(b.get("BCR1", "SAIAEN") == 1);
  CHECK(b.get("BSR", "WCKCFG") == 0);
  b.run_until(b.now() + 100 * us);
  CHECK(r.b->calls.empty());
  CHECK(
      std::ranges::count_if(
          logged,
          [](core::log_record const& record) {
            return record.message.contains("shorter than 8 bits");
          })
      == 1);
}

TEST_CASE("an attachment set through the port changes the decimation from the next frame")
{
  rig   r{"idle"};
  bus   b{*r.f};
  auto* port = dynamic_cast<stm32::audio_port*>(r.f->target->model("SAI1"));
  REQUIRE(port != nullptr);
  b.clocks();
  b.stereo16("B", 32, false);
  b.put(
      "BCR1", {
                  {"MODE",   1},
                  {"DS",     4},
                  {"NODIV",  1},
                  {"MCKDIV", 5},
                  {"SAIAEN", 1}
  });
  auto const start = b.now();
  b.run_until(start + 15 * us);
  port->attach(stm32::audio_block::b, {.endpoint = r.b, .decimation = 2});
  b.run_until(start + 51 * us);
  std::vector<std::uint64_t> at;
  for(auto const& call : r.b->calls)
  {
    at.push_back(call.at - start);
  }
  CHECK(at == std::vector<std::uint64_t>{0, 10 * us, 20 * us, 40 * us});
}

TEST_CASE(
    "GPDMA feeds block A from a circular ring for 1024 frames, block B fills one buffer then "
    "overruns")
{
  auto const words = source_words(64);
  rig        r{"dma", words};
  auto const log = r.finish();
  CHECK(log == std::vector<std::uint32_t>{32, 32, 1, 0});
  auto const& frames = r.a->frames;
  REQUIRE(frames.size() >= 1024);
  for(std::uint32_t index = 0; index < 1024; ++index)
  {
    CAPTURE(index);
    auto const first = (2 * index) % 64;
    CHECK(frames[index].slots == std::vector<std::uint32_t>{ramp(first), ramp(first + 1)});
    if(index > 0)
    {
      CHECK(frames[index].at.ps - frames[index - 1].at.ps == frame_ps);
    }
  }
  for(std::uint32_t index = 0; index < 64; ++index)
  {
    CAPTURE(index);
    CHECK(r.word("received", index) == (words[index] & 0xFFFFu));
  }
}

TEST_CASE("a snapshot mid-DMA restored into a fresh machine sends the same rest")
{
  rig first{"dma"};
  bus b{*first.f};
  b.run_until(b.now() + 300 * frame_ps + 20 * us);
  auto const                               done  = first.a->frames.size();
  auto const                               saved = first.f->target->snapshot();
  auto const                               log   = first.finish();
  std::vector<stm32::captured_audio_frame> rest{
      first.a->frames.begin() + static_cast<std::ptrdiff_t>(done), first.a->frames.end()};
  REQUIRE(rest.size() > 500);
  rig other{"idle"};
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.finish() == log);
  CHECK(other.a->frames == rest);
}

TEST_CASE("the DMA requests follow DMAEN and FREQ, also while the block is off")
{
  rig r{"idle", source_words(64), {}, true};
  bus b{*r.f};
  CHECK_FALSE(r.level(a_request));
  b.set("ACR1", "DMAEN", 1);
  CHECK(r.level(a_request));
  b.push('A', 1);
  CHECK_FALSE(r.level(a_request));
  b.put(
      "ACR2", {
                  {"FTH", 1}
  });
  CHECK(r.level(a_request));
  b.push('A', 2);
  b.push('A', 3);
  CHECK_FALSE(r.level(a_request));
  b.set("ACR1", "DMAEN", 0);
  b.put(
      "ACR2", {
                  {"FFLUSH", 1}
  });
  CHECK_FALSE(r.level(a_request));
  b.clocks();
  b.stereo16("B", 32, false);
  b.put(
      "BCR1", {
                  {"MODE",   1},
                  {"DS",     4},
                  {"MCKDIV", 4},
                  {"DMAEN",  1},
                  {"SAIAEN", 1}
  });
  auto const start = b.now();
  CHECK_FALSE(r.level(b_request));
  b.run_until(start + 33 * us);
  CHECK(r.level(b_request));
  CHECK(b.pop('B') == (source_words(1)[0] & 0xFFFFu));
  CHECK_FALSE(r.level(b_request));
  b.run_until(start + frame_ps + 1);
  CHECK(r.level(b_request));
  b.set("BCR1", "DMAEN", 0);
  CHECK_FALSE(r.level(b_request));
  b.set("BCR1", "DMAEN", 1);
  CHECK(r.level(b_request));
  r.f->target->reset();
  CHECK_FALSE(r.level(b_request));
  CHECK_FALSE(r.level(a_request));
}

TEST_CASE("GPDMA widens 16-bit samples to the 32-bit data register, 0-padded or sign-extended")
{
  constexpr std::array<std::int16_t, 16> samples{-3,  2,    -1,     0x7fff,  -32768, 0,  1, -2,
                                                 100, -100, 0x1234, -0x1234, 7,      -7, 8, -8};
  for(bool const sign : {false, true})
  {
    CAPTURE(sign);
    rig        r{sign ? "dma16_sign" : "dma16_zero"};
    auto const log = r.finish();
    CHECK(log == std::vector<std::uint32_t>{1, 0});
    REQUIRE(r.a->frames.size() == 8);
    for(std::size_t index = 0; index < samples.size(); ++index)
    {
      CAPTURE(index);
      auto const half = static_cast<std::uint16_t>(samples[index]);
      auto const expected =
          sign ? static_cast<std::uint32_t>(std::int32_t{samples[index]}) : std::uint32_t{half};
      CHECK(r.a->frames[index / 2].slots[index % 2] == expected);
      CHECK(r.a->frames[index / 2].format.data_bits == 32);
    }
  }
}

namespace
{
constexpr std::uint32_t stereo_frames = 512;
constexpr std::uint32_t stereo_halves = 1280;

auto stereo_words(std::uint32_t frames) -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(std::uint32_t frame = 0; frame < frames; ++frame)
  {
    out.push_back(0x1000u | frame);
    out.push_back(0x2000u | frame);
  }
  return out;
}

struct stereo_rig
{
  std::shared_ptr<recorder>      tx = std::make_shared<recorder>();
  std::shared_ptr<recorder>      rx = std::make_shared<recorder>(stereo_words(1400));
  stm32::board                   board{};
  std::unique_ptr<stm32_fixture> f;

  stereo_rig(std::string_view entry, std::string const& instance, std::uint32_t external_hz)
  {
    board.audio[instance + "_A"] = {.endpoint = tx, .external_frame_hz = external_hz};
    board.audio[instance + "_B"] = {.endpoint = rx};
    auto options                 = popcorn::testing::stm32_options();
    options.models               = stm32::factories(board, u575());
    f = std::make_unique<stm32_fixture>(u575(), "sai_ll", std::move(options));
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("sai_ll", "main")})
            .reason
        == stop_reason::address);
    f->set("pc", sym("sai_ll", entry));
  }

  [[nodiscard]] auto read(std::string_view name, std::uint32_t offset, std::uint8_t size) const
      -> std::uint32_t
  {
    auto const value = f->target->mem().read(sym("sai_ll", name) + offset, size);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  [[nodiscard]] auto halfword(std::string_view name, std::uint32_t half, std::uint32_t frame) const
      -> std::uint32_t
  {
    return read(name, 2 * (half * stereo_frames + frame), 2);
  }

  [[nodiscard]] auto event_ticks(std::string_view name) const -> std::vector<std::int64_t>
  {
    std::vector<std::int64_t> out;
    auto const                start = read("stereo_start", 0, 4);
    for(std::uint32_t index = 0; index < read(name, 8, 4); ++index)
    {
      out.push_back(static_cast<std::int64_t>(read(name, 12 + 4 * index, 4) - start));
    }
    return out;
  }
};

auto stereo_checksum(stereo_rig const& r, std::string_view name) -> std::uint32_t
{
  std::uint32_t sum = 0;
  for(std::uint32_t frame = 0; frame < stereo_frames; ++frame)
  {
    sum = sum * 31u + r.halfword(name, 0, frame);
    sum = sum * 31u + r.halfword(name, 1, frame);
  }
  return sum;
}

auto stereo_log(std::uint32_t tag) -> std::vector<std::uint32_t>
{
  return {
      tag | 0x0048'0001u, tag | 0x0054'0001u, tag | 0x0048'0002u, tag | 0x0054'0002u,
      tag | 0x0048'0003u};
}

auto stereo_blocks() -> std::array<std::uint64_t, 5>
{
  return {255, 511, 767, 1023, 1279};
}

void check_stereo(
    stereo_rig const& r,
    std::uint32_t     tx_tag,
    std::uint32_t     rx_tag,
    std::string_view  rx_channel,
    std::uint64_t     frame)
{
  constexpr std::uint64_t latency = us;
  auto const              log     = r.f->log();
  REQUIRE(log.size() == 12);
  std::vector<std::uint32_t> tx_log;
  std::vector<std::uint32_t> rx_log;
  for(auto const word : std::span{log}.first(10))
  {
    (word >> 24 == tx_tag >> 24 ? tx_log : rx_log).push_back(word);
  }
  CHECK(tx_log == stereo_log(tx_tag));
  CHECK(rx_log == stereo_log(rx_tag));
  CHECK(log[10] == stereo_checksum(r, "tx_stereo"));
  CHECK(log[11] == stereo_checksum(r, "rx_stereo"));

  auto const& sent = r.tx->frames;
  REQUIRE(sent.size() >= stereo_halves);
  auto const                 t0 = sent[0].at.ps;
  std::vector<std::uint32_t> wrong_frames;
  for(std::uint32_t index = 0; index < stereo_halves; ++index)
  {
    auto const k = index % stereo_frames;
    if(sent[index].slots != std::vector<std::uint32_t>{k, 0x8000u | k}
       or sent[index].at.ps != t0 + index * frame)
    {
      wrong_frames.push_back(index);
    }
  }
  CHECK(wrong_frames.empty());

  auto const repeat = r.f->field("GPDMA1", std::format("GPDMA_C{}BR1", rx_channel), "BRC");
  auto const bytes  = r.f->field("GPDMA1", std::format("GPDMA_C{}BR1", rx_channel), "BNDT");
  auto const done   = stereo_frames - 1 - repeat;
  CAPTURE(repeat);
  CAPTURE(bytes);
  std::vector<std::uint32_t> wrong_slots;
  for(std::uint32_t k = 0; k < stereo_frames; ++k)
  {
    auto const left  = k < done or (k == done and bytes == 4) ? 1024 + k : 512 + k;
    auto const right = k < done ? 1024 + k : 512 + k;
    if(r.halfword("rx_stereo", 0, k) != (0x1000u | left)
       or r.halfword("rx_stereo", 1, k) != (0x2000u | right))
    {
      wrong_slots.push_back(k);
    }
  }
  CHECK(wrong_slots.empty());

  auto const bit = frame / 64;
  auto const due = [&](std::uint64_t at) { return static_cast<std::int64_t>((at - t0) / us); };
  std::vector<std::int64_t> tx_due;
  std::vector<std::int64_t> rx_due;
  for(auto const block : stereo_blocks())
  {
    tx_due.push_back(due(t0 + block * frame + latency));
    rx_due.push_back(due(t0 + (block + 1) * frame + 48 * bit + latency));
  }
  auto const tx_seen = r.event_ticks("stereo_tx_events");
  auto const rx_seen = r.event_ticks("stereo_rx_events");
  CAPTURE(tx_due);
  CAPTURE(rx_due);
  CAPTURE(tx_seen);
  CAPTURE(rx_seen);
  REQUIRE(tx_seen.size() == tx_due.size());
  REQUIRE(rx_seen.size() == rx_due.size());
  std::vector<std::int64_t> late;
  for(std::size_t index = 0; index < tx_due.size(); ++index)
  {
    late.push_back(tx_seen[index] - tx_due[index]);
    late.push_back(rx_seen[index] - rx_due[index]);
  }
  CAPTURE(late);
  CHECK(std::ranges::min(late) >= 0);
  CHECK(std::ranges::max(late) <= 5);
  CHECK(std::ranges::max(late) - std::ranges::min(late) <= 1);
}
}

TEST_CASE("a 2D circular GPDMA list runs a slave SAI1 pair from and to two stereo buffers")
{
  stereo_rig r{"codec_dma", "SAI1", 16'000};
  auto const stopped = r.f->run_to_stop({.max_instructions = icount{1'000'000'000}});
  CAPTURE(stopped.detail);
  REQUIRE(stopped.reason == stop_reason::user);
  check_stereo(r, 0x0C00'0000, 0x0D00'0000, "13", 62'500'000);
}

TEST_CASE("a 2D circular GPDMA list runs an SAI2 master pair on channels 14 and 15")
{
  stereo_rig r{"pair_dma", "SAI2", 0};
  auto const stopped = r.f->run_to_stop({.max_instructions = icount{1'000'000'000}});
  CAPTURE(stopped.detail);
  REQUIRE(stopped.reason == stop_reason::user);
  check_stereo(r, 0x0E00'0000, 0x0F00'0000, "15", frame_ps);
}
