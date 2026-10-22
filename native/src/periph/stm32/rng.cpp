#include "factories.hpp"
#include "model_base.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/periph/stm32/rng.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
namespace
{
using wide = unsigned __int128;

constexpr std::uint64_t ps_per_second = 1'000'000'000'000;
constexpr std::uint32_t work_done     = 0;
constexpr std::uint32_t condrst_done  = 1;
constexpr std::size_t   event_kinds   = 2;
constexpr std::size_t   fifo_words    = 4;
constexpr std::uint64_t ahb_round     = 203;
constexpr std::uint64_t rng_round     = 128;
constexpr std::uint64_t startup_ahb   = 13 * ahb_round;
constexpr std::uint32_t reset_config1 = 0x08;
constexpr std::uint32_t reset_config3 = 0xD;
constexpr std::uint64_t reset_wait    = 16;
constexpr std::uint64_t custom_wait   = 192;

enum phase : std::uint8_t
{
  idle,
  init,
  round,
};

struct rng_source
{
  std::uint64_t              drawn{};
  std::vector<std::uint32_t> pending{};
  std::uint64_t              pending_at{};
  std::uint32_t              stuck_word{};
  std::uint8_t               stuck_set{};

  friend auto operator==(rng_source const&, rng_source const&) -> bool = default;
};

struct rng_state
{
  rng_source                             source{};
  std::array<std::uint32_t, fifo_words>  fifo{};
  std::array<std::uint32_t, fifo_words>  staged{};
  std::uint64_t                          remaining_ps{};
  std::uint64_t                          due_ps{};
  std::uint64_t                          clear_due_ps{};
  std::array<std::uint64_t, event_kinds> generation{};
  std::array<std::uint64_t, event_kinds> queued{};
  std::uint8_t                           fifo_count{};
  std::uint8_t                           staged_full{};
  std::uint8_t                           phase{};
  std::uint8_t                           initialised{};
  std::uint8_t                           clearing{};

  friend auto operator==(rng_state const&, rng_state const&) -> bool = default;
};

/**
 * @brief RNG of the STM32U5, RM0456 Rev 7 chapter 48. The output pipeline is a 4-word FIFO in
 *        front of the 128-bit conditioning output register, 48.3.3 "Output buffer": init ends
 *        with the FIFO full and a conditioning round started, a round ends into the FIFO when
 *        it is empty, else into the register, and a new round starts only while the register is
 *        empty. The fourth DR read of the FIFO moves the register in and starts a round. DRDY
 *        is 1 while the FIFO holds a word. A CPU or DMA read of DR pops the FIFO, 0 when it is
 *        empty, a debugger read shows the head and pops nothing. A phase end takes its words
 *        from the entropy_source: the stuck word while one is set, else the queue, else a
 *        counter SplitMix64 generator, rng_next_word(). The source is board state, kept across
 *        every reset kind.
 *        Generation runs while CR.RNGEN is 1, CONDRST reads 0 and the `rng` kernel clock runs,
 *        which RCC stops for RCC_AHB2ENR1.RNGEN 0 or a stopped RNGSEL source. Otherwise init and
 *        rounds pause with their remaining time and resume from it, 48.3.8, and the FIFO and the
 *        register still deliver what they hold.
 *        Timing, 48.5, from the HCLK and kernel rates when a phase starts: init per Table 463,
 *        column "Configuration A or C, reset value", with CLKDIV read as the divider ratio
 *        2^CLKDIV, as the 48.5 example of configuration C at CLKDIV 0 reads it, and wait_noise 16
 * while RNG_CONFIG1, RNG_CONFIG2, RNG_CONFIG3 and NISTC hold their reset values, else 192. A round
 * takes 203 x N AHB cycles when fAHB < 1.6 x fRNG / 2^CLKDIV, else 128 x N divided RNG cycles, N 2
 * for NISTC 0, else RNG_CONFIG3[1:0] + 1, Table 464. CR bits 29:4 are written only with CONDRST 1
 * in the same access and CONFIGLOCK 0, CLKDIV and CED also only with RNGEN 0 before or in the
 * write, as the 48.3.4 software reset sequence writes them. CONFIGLOCK is set-once. NSCR and HTCR
 * are written only while CONDRST reads 1 and CONFIGLOCK is 0, 48.7.4 and 48.7.5. CONDRST written 1
 * empties the FIFO and the register, stops init and the round and clears SR. Written back to 0 it
 * reads 1 for 2 AHB plus 2 RNG cycles, 48.7.1, the time counted from the write, or from the kernel
 * clock start when it is stopped or stops before the end. Then init restarts. SEIS and CEIS are
 * cleared by writing 0. The RNG_IRQn line is IE and RNGEN and any of DRDY, SEIS and CEIS, 48.4.
 * Only 32-bit accesses are served, others are a bus error and a write is ignored, 48.2. Deviations:
 * register accesses are served with RCC_AHB2ENR1.RNGEN 0. RCC_AHB2RSTR1.RNGRST is not modelled, so
 * CONFIGLOCK clears only on a system reset. There is no noise source: NSCR and HTCR are storage.
 * Configuration B timing is not modelled, the configuration A and C column applies to every
 * configuration. A rate change does not rescale a phase already started. The tamper output to TAMP
 * is not modelled.
 */
class rng final : public dispatch_model<rng, rng_state>,
                  public clock_listener,
                  public entropy_source
{
public:
  rng(model_context& ctx, peripheral_desc const& desc, rng_options options);

  void reset(register_file& regs, reset_kind kind) override;
  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
      -> std::expected<std::uint32_t, bus_error> override;
  auto on_write(
      register_file& regs,
      std::uint32_t  offset,
      std::uint8_t   size,
      std::uint32_t  old,
      std::uint32_t  value,
      access_ctx     ctx) -> std::expected<void, bus_error> override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
  void on_clock_change() override;

  [[nodiscard]] auto seed() const -> std::uint64_t override;
  [[nodiscard]] auto drawn() const -> std::uint64_t override;
  [[nodiscard]] auto queued() const -> std::size_t override;
  void               queue(std::span<std::uint32_t const> words) override;
  [[nodiscard]] auto stuck() const -> std::optional<std::uint32_t> override;
  void               set_stuck(std::optional<std::uint32_t> word) override;

  void control_written(std::uint32_t old, std::uint32_t now);
  void status_written(std::uint32_t old, std::uint32_t now);
  void noise_written(std::uint32_t old, std::uint32_t now);
  void health_written(std::uint32_t old, std::uint32_t now);

private:
  [[nodiscard]] auto now_ps() const -> std::uint64_t;
  [[nodiscard]] auto producing() const -> bool;
  [[nodiscard]] auto loops() const -> std::uint64_t;
  [[nodiscard]] auto init_ps() const -> std::uint64_t;
  [[nodiscard]] auto round_ps() const -> std::uint64_t;
  [[nodiscard]] auto clear_ps() const -> std::uint64_t;

  void post(std::uint32_t kind, std::uint64_t due);
  void withdraw(std::uint32_t kind);
  void start(phase which, std::uint64_t at, std::uint64_t duration);
  void refresh(std::uint64_t at);
  void restart_conditioning();
  void schedule_clear();
  void finish_phase();
  auto pop() -> std::uint32_t;
  void expose();
  void interrupt();
  void fresh_state();
  auto next_word() -> std::uint32_t;
  void keep_unless_open(reg_ref target, std::uint32_t old);

  register_file&     regs_;
  rng_options        options_;
  reg_ref            cr_;
  reg_ref            sr_;
  reg_ref            dr_;
  reg_ref            nscr_;
  reg_ref            htcr_;
  field_ref          configlock_;
  field_ref          condrst_;
  field_ref          config1_;
  field_ref          clkdiv_;
  field_ref          config2_;
  field_ref          nistc_;
  field_ref          config3_;
  field_ref          ced_;
  field_ref          ie_;
  field_ref          rngen_;
  field_ref          seis_;
  field_ref          ceis_;
  field_ref          drdy_;
  std::uint32_t      config_bits_;
  std::uint32_t      live_bits_;
  core::clock_domain kernel_;
  core::clock_domain hclk_;
  irq_line&          line_;
};

constexpr std::array<write_entry<rng>, 5> rng_writes{
    {
     {"CR", &rng::control_written},
     {"SR", &rng::status_written},
     {"DR", nullptr},
     {"NSCR", &rng::noise_written},
     {"HTCR", &rng::health_written},
     }
};

rng::rng(model_context& ctx, peripheral_desc const& desc, rng_options options)
    : dispatch_model{ctx, desc, "RNG", rng_writes}, regs_{ctx.regs}, options_{options},
      cr_{reg("CR")}, sr_{reg("SR")}, dr_{reg("DR")}, nscr_{reg("NSCR")}, htcr_{reg("HTCR")},
      configlock_{field("CR", "CONFIGLOCK")}, condrst_{field("CR", "CONDRST")},
      config1_{field("CR", "RNG_CONFIG1")}, clkdiv_{field("CR", "CLKDIV")},
      config2_{field("CR", "RNG_CONFIG2")}, nistc_{field("CR", "NISTC")},
      config3_{field("CR", "RNG_CONFIG3")}, ced_{field("CR", "CED")}, ie_{field("CR", "IE")},
      rngen_{field("CR", "RNGEN")}, seis_{field("SR", "SEIS")}, ceis_{field("SR", "CEIS")},
      drdy_{field("SR", "DRDY")},
      config_bits_{
          config1_.mask()
          | clkdiv_.mask()
          | config2_.mask()
          | nistc_.mask()
          | config3_.mask()
          | field("CR", "ARDIS").mask()
          | ced_.mask()},
      live_bits_{clkdiv_.mask() | ced_.mask()}, kernel_{ctx.clock.domain("rng")},
      hclk_{ctx.clock.domain("hclk")}, line_{irq("RNG_IRQn")}
{}

auto rng::now_ps() const -> std::uint64_t
{
  return ctx_.clock.time().ps;
}

auto rng::producing() const -> bool
{
  return get(regs_, rngen_) != 0 and get(regs_, condrst_) == 0 and ctx_.clock.hz(kernel_) != 0;
}

auto rng::loops() const -> std::uint64_t
{
  return get(regs_, nistc_) == 0 ? 2 : (get(regs_, config3_) & 3u) + 1;
}

auto rng::init_ps() const -> std::uint64_t
{
  auto const fr    = wide{ctx_.clock.hz(kernel_)};
  auto const fa    = wide{std::max<std::uint64_t>(ctx_.clock.hz(hclk_), 1)};
  auto const d     = wide{1} << get(regs_, clkdiv_);
  auto const n     = wide{loops()};
  auto const plain = get(regs_, config1_) == reset_config1
                 and get(regs_, config2_) == 0
                 and get(regs_, config3_) == reset_config3
                 and get(regs_, nistc_) == 0;
  auto const wait   = wide{plain ? reset_wait : custom_wait};
  auto const health = std::max((wait + 11 + 1024 * d) * fa, wide{startup_ahb} * fr);
  auto const keys   = std::max(
      (1 + 2 * n) * rng_round * d * fa + ahb_round * fr,
      rng_round * d * fa + (2 * n + 2) * ahb_round * fr);
  auto const den = fr * fa;
  return static_cast<std::uint64_t>(((health + keys) * ps_per_second + den - 1) / den);
}

auto rng::round_ps() const -> std::uint64_t
{
  auto const fa    = wide{std::max<std::uint64_t>(ctx_.clock.hz(hclk_), 1)};
  auto const fr    = wide{ctx_.clock.hz(kernel_)};
  auto const ratio = wide{1} << get(regs_, clkdiv_);
  auto const n     = wide{loops()};
  if(5 * fa * ratio < 8 * fr)
  {
    return static_cast<std::uint64_t>((n * ahb_round * ps_per_second + fa - 1) / fa);
  }
  return static_cast<std::uint64_t>((n * rng_round * ratio * ps_per_second + fr - 1) / fr);
}

auto rng::clear_ps() const -> std::uint64_t
{
  auto const fr  = wide{ctx_.clock.hz(kernel_)};
  auto const fa  = wide{std::max<std::uint64_t>(ctx_.clock.hz(hclk_), 1)};
  auto const den = fr * fa;
  return static_cast<std::uint64_t>(((2 * fr + 2 * fa) * ps_per_second + den - 1) / den);
}

void rng::post(std::uint32_t kind, std::uint64_t due)
{
  withdraw(kind);
  state_.queued[kind] =
      ctx_.events.schedule_at(vtime{due}, kind, ++state_.generation[kind], ctx_.clock) + 1;
}

void rng::withdraw(std::uint32_t kind)
{
  if(auto const seq = std::exchange(state_.queued[kind], 0); seq != 0)
  {
    ctx_.events.cancel(seq - 1);
  }
}

void rng::start(phase which, std::uint64_t at, std::uint64_t duration)
{
  state_.phase        = which;
  state_.remaining_ps = 0;
  state_.due_ps       = at + duration;
  post(work_done, state_.due_ps);
}

void rng::refresh(std::uint64_t at)
{
  auto const on = producing();
  if(state_.phase != idle)
  {
    auto const running = state_.queued[work_done] != 0;
    if(on and not running)
    {
      state_.due_ps = at + std::exchange(state_.remaining_ps, 0);
      post(work_done, state_.due_ps);
    }
    else if(not on and running)
    {
      state_.remaining_ps = state_.due_ps > now_ps() ? state_.due_ps - now_ps() : 0;
      state_.due_ps       = 0;
      withdraw(work_done);
    }
  }
  else if(on and state_.initialised == 0)
  {
    start(init, at, init_ps());
  }
  else if(on and state_.staged_full == 0)
  {
    start(round, at, round_ps());
  }
  interrupt();
}

void rng::restart_conditioning()
{
  withdraw(work_done);
  withdraw(condrst_done);
  fresh_state();
  regs_.poke(sr_.offset, 0);
  expose();
}

void rng::fresh_state()
{
  auto source       = std::move(state_.source);
  auto generation   = state_.generation;
  state_            = {};
  state_.source     = std::move(source);
  state_.generation = generation;
}

auto rng::next_word() -> std::uint32_t
{
  auto& source = state_.source;
  if(source.stuck_set != 0)
  {
    return source.stuck_word;
  }
  if(source.pending_at < source.pending.size())
  {
    auto const word = source.pending[source.pending_at++];
    if(2 * source.pending_at >= source.pending.size())
    {
      source.pending.erase(
          source.pending.begin(),
          source.pending.begin() + static_cast<std::ptrdiff_t>(source.pending_at));
      source.pending_at = 0;
    }
    return word;
  }
  return rng_next_word(options_.seed, source.drawn);
}

auto rng::seed() const -> std::uint64_t
{
  return options_.seed;
}

auto rng::drawn() const -> std::uint64_t
{
  return state_.source.drawn;
}

auto rng::queued() const -> std::size_t
{
  return state_.source.pending.size() - state_.source.pending_at;
}

void rng::queue(std::span<std::uint32_t const> words)
{
  if(words.size() > rng_max_queued - queued())
  {
    throw config_error(
        desc_.name, 0, "entropy.queue",
        std::format(
            "at most {} queued words, not {} more on {}", rng_max_queued, words.size(), queued()));
  }
  state_.source.pending.insert(state_.source.pending.end(), words.begin(), words.end());
}

auto rng::stuck() const -> std::optional<std::uint32_t>
{
  return state_.source.stuck_set != 0 ? std::optional{state_.source.stuck_word} : std::nullopt;
}

void rng::set_stuck(std::optional<std::uint32_t> word)
{
  state_.source.stuck_set  = word ? 1 : 0;
  state_.source.stuck_word = word.value_or(0);
}

void rng::schedule_clear()
{
  if(ctx_.clock.hz(kernel_) != 0)
  {
    state_.clear_due_ps = now_ps() + clear_ps();
    post(condrst_done, state_.clear_due_ps);
  }
}

void rng::finish_phase()
{
  std::array<std::uint32_t, fifo_words> words{};
  for(auto& word : words)
  {
    word = next_word();
  }
  if(state_.phase == init)
  {
    state_.initialised = 1;
  }
  state_.phase  = idle;
  state_.due_ps = 0;
  if(state_.fifo_count == 0)
  {
    state_.fifo       = words;
    state_.fifo_count = fifo_words;
  }
  else
  {
    state_.staged      = words;
    state_.staged_full = 1;
  }
  expose();
}

auto rng::pop() -> std::uint32_t
{
  auto const word = state_.fifo[0];
  std::ranges::shift_left(state_.fifo, 1);
  state_.fifo.back() = 0;
  --state_.fifo_count;
  if(state_.fifo_count == 0 and state_.staged_full != 0)
  {
    state_.fifo        = std::exchange(state_.staged, {});
    state_.fifo_count  = fifo_words;
    state_.staged_full = 0;
  }
  expose();
  refresh(now_ps());
  return word;
}

void rng::expose()
{
  regs_.poke(dr_.offset, state_.fifo_count != 0 ? state_.fifo[0] : 0);
  update(regs_, drdy_, state_.fifo_count != 0 ? 1 : 0);
  interrupt();
}

void rng::interrupt()
{
  auto const pending = get(regs_, drdy_) != 0 or get(regs_, seis_) != 0 or get(regs_, ceis_) != 0;
  if(get(regs_, ie_) != 0 and get(regs_, rngen_) != 0 and pending)
  {
    line_.raise();
  }
  else
  {
    line_.lower();
  }
}

void rng::reset(register_file& regs, reset_kind)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
  for(auto& value : state_.generation)
  {
    ++value;
  }
  fresh_state();
  line_.lower();
}

auto rng::on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
    -> std::expected<std::uint32_t, bus_error>
{
  if(size != 4 or offset % 4 != 0)
  {
    return std::unexpected{bus_error::denied};
  }
  if(offset == dr_.offset and ctx.by != initiator::debugger)
  {
    return state_.fifo_count != 0 ? pop() : 0u;
  }
  return dispatch_model::on_read(regs, offset, size, ctx);
}

auto rng::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     ctx) -> std::expected<void, bus_error>
{
  if(size != 4 or offset % 4 != 0)
  {
    return std::unexpected{bus_error::denied};
  }
  return dispatch_model::on_write(regs, offset, size, old, value, ctx);
}

void rng::control_written(std::uint32_t old, std::uint32_t now)
{
  auto taken = 0u;
  if(condrst_.of(now) != 0 and configlock_.of(old) == 0)
  {
    taken = rngen_.of(old) == 0 or rngen_.of(now) == 0 ? config_bits_ : config_bits_ & ~live_bits_;
  }
  auto word       = (now & ~config_bits_) | (now & taken) | (old & config_bits_ & ~taken);
  word            = configlock_.with(word, configlock_.of(old) | configlock_.of(now));
  auto const held = condrst_.of(old) != 0;
  if(condrst_.of(now) == 0 and held)
  {
    word = condrst_.with(word, 1);
  }
  regs_.poke(cr_.offset, word);
  if(condrst_.of(now) != 0)
  {
    if(not held or state_.clearing != 0)
    {
      restart_conditioning();
    }
  }
  else if(held and state_.clearing == 0)
  {
    state_.clearing = 1;
    schedule_clear();
  }
  refresh(now_ps());
}

void rng::status_written(std::uint32_t old, std::uint32_t now)
{
  auto const flags = seis_.mask() | ceis_.mask();
  regs_.poke(sr_.offset, (old & ~flags) | (old & now & flags));
  interrupt();
}

void rng::keep_unless_open(reg_ref target, std::uint32_t old)
{
  if(get(regs_, condrst_) == 0 or get(regs_, configlock_) != 0)
  {
    regs_.poke(target.offset, old);
  }
}

void rng::noise_written(std::uint32_t old, std::uint32_t)
{
  keep_unless_open(nscr_, old);
}

void rng::health_written(std::uint32_t old, std::uint32_t)
{
  keep_unless_open(htcr_, old);
}

void rng::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind >= event_kinds or payload != state_.generation[kind])
  {
    return;
  }
  state_.queued[kind] = 0;
  auto at             = state_.due_ps;
  if(kind == condrst_done)
  {
    at              = std::exchange(state_.clear_due_ps, 0);
    state_.clearing = 0;
    update(regs_, condrst_, 0);
  }
  else
  {
    finish_phase();
  }
  refresh(at);
}

void rng::on_clock_change()
{
  if(state_.clearing != 0 and ctx_.clock.hz(kernel_) == 0)
  {
    withdraw(condrst_done);
  }
  if(state_.clearing != 0 and state_.queued[condrst_done] == 0)
  {
    schedule_clear();
  }
  refresh(now_ps());
}
}

entropy_source::~entropy_source() = default;

auto rng_factory(rng_options options) -> model_factory
{
  return [options](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<rng>(ctx, desc, options);
  };
}
}
