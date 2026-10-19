#include "dma_signal.hpp"
#include "factories.hpp"
#include "model_base.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/scheduler.hpp>
#include <popcorn/periph/i2c.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/i2c.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
i2c_bus::~i2c_bus() = default;

namespace
{
constexpr std::uint64_t ps_per_second  = 1'000'000'000'000;
constexpr std::uint32_t phase_done     = 0;
constexpr std::uint32_t hold_begin     = 1;
constexpr std::uint32_t hold_end       = 2;
constexpr std::uint32_t no_target      = ~std::uint32_t{};
constexpr std::uint32_t address_length = 10;
constexpr std::uint32_t byte_length    = 9;
constexpr std::uint32_t stop_length    = 1;

enum class phase : std::uint8_t
{
  idle,
  address,
  byte,
  stop,
  wait_tx,
  wait_rx,
  wait_reload,
  wait_end,
  lost,
};

struct i2c_hold
{
  std::uint32_t id{};
  std::uint64_t from_ps{};
  std::uint64_t until_ps{};
  std::uint64_t begin_seq{};
  std::uint64_t end_seq{};

  friend auto operator==(i2c_hold const&, i2c_hold const&) -> bool = default;
};

struct i2c_state
{
  std::uint32_t                       generation{};
  std::uint8_t                        step{};
  std::uint8_t                        read{};
  std::uint32_t                       target{no_target};
  std::uint32_t                       left{};
  std::uint8_t                        tx_full{};
  std::uint8_t                        tx_data{};
  std::uint8_t                        shift{};
  std::uint8_t                        rx_held{};
  std::uint8_t                        rx_data{};
  std::uint64_t                       end_ps{};
  std::uint32_t                       holds{};
  i2c_hold                            pending{};
  std::uint8_t                        pending_waits{};
  i2c_hold                            active{};
  std::vector<std::vector<std::byte>> devices;

  friend auto operator==(i2c_state const&, i2c_state const&) -> bool = default;
};

auto lower(std::string_view name) -> std::string
{
  std::string out{name};
  std::ranges::transform(
      out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

/**
 * @brief I2C1 to I2C4 of the STM32U5, RM0456 section 65 "Inter-integrated circuit interface
 *        (I2C)", controller mode with 7-bit addresses. The SCL period is
 *        ((SCLH + 1) + (SCLL + 1)) x (PRESC + 1) kernel clock cycles, TIMINGR, without the
 *        tSYNC delays of the SCL edges, which depend on the board. A START and the address byte
 *        take 10 periods, a data byte 9, a STOP 1. A phase keeps the duration it started with
 *        across a clock change. CR2.START sends a START, or a repeated START after the NBYTES
 *        transfer, and the address: the target answering SADD acknowledges or not, and a NACK
 *        sets NACKF and sends a STOP. Transmit: TXIS while TXDR is empty and a byte of NBYTES is
 *        still to be written, TXDR copied into the shift register as soon as the bus is free for
 *        it, SCL stretched while TXDR is empty, a NACKed byte sets NACKF and sends a STOP.
 *        Receive: each byte lands in RXDR and sets RXNE, SCL stretched while RXDR is full. After
 *        NBYTES bytes: TCR with RELOAD until NBYTES is written again, a STOP with AUTOEND, a
 *        repeated START when START is already set, otherwise TC until START or STOP. CR2.STOP
 *        sends a STOP after the current byte. STOPF and BUSY clear at the end of the STOP. A
 *        target's reply stretches SCL after its byte by the reply's stretch_ps: the next phase
 *        starts once both the target and the controller release SCL, the phase lengths
 *        unchanged. A host hold, i2c_bus::hold, is another controller owning the bus: BUSY 1 while
 *        PE is set, a START requested meanwhile sent at the hold's end, a hold due while the
 *        firmware's transfer runs taken at that transfer's STOP, at the end of a lost
 *        arbitration or at PE clear, its end unchanged. Deviation from RM0456: setting PE
 *        during a hold reads BUSY 1 at once, where silicon waits for the next START it detects. Its
 *        events go on core::host_lane, after the model's own events of the same instant. Deviation:
 *        on silicon the firmware's transfer would lose arbitration instead, which a target's ARLO
 *        reply models. ICR clears its flags, PE = 0 resets the transfer, CR2's START, STOP, NACK
 *        and PECBYTE and
 *        the ISR flags. The event IRQ follows TXIS, RXNE, ADDR, NACKF, STOPF, TC and TCR with
 *        their enables, the error IRQ BERR, ARLO, OVR, PECERR, TIMEOUT and ALERT with ERRIE. BERR,
 *        ARLO and OVR are set only when a target's reply to an address or a byte carries that
 *        error, at the end of that byte, RM0456 section 65.4.17: BERR and OVR leave the byte to
 *        complete as the reply's acknowledge says, a read byte landing in RXDR. OVR exists on
 *        silicon only in target mode with NOSTRETCH, the injection exercises firmware handlers.
 *        ARLO releases the bus: CR2.START cleared, the byte read dropped, no NACKF, TC or STOPF,
 *        BUSY kept 1 for one STOP length, the other controller's STOP, which clears BUSY and
 *        CR2.STOP, ends the target's transfer and sends a START requested meanwhile. PE = 0 in
 *        that window ends it at once, the target's STOP at the time of the write. PECERR, TIMEOUT
 *        and ALERT are never set. DMA requests, RM0456 section 65.7, through the GPDMA
 *        peer, low while PE is clear since PE = 0 clears their flags: `<instance>_tx_dma` follows
 *        TXIS with CR1.TXDMAEN, `<instance>_rx_dma` RXNE with CR1.RXDMAEN, `<instance>_evc_dma` TC
 *        with AUTOCR.TCDMAEN or TCR with AUTOCR.TCRDMAEN. The targets are saved and loaded with
 *        the model. Not modelled, logged once: target mode, 10-bit addressing, SMBus and PEC, the
 *        autonomous trigger.
 */
class i2c final : public dispatch_model<i2c, i2c_state>, public i2c_bus, public bus_reset_listener
{
public:
  i2c(model_context&                           ctx,
      peripheral_desc const&                   desc,
      std::vector<std::shared_ptr<i2c_target>> targets);

  void reset(register_file& regs, reset_kind kind) override;
  void on_bus_reset() override;
  void power_cut() override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
      -> std::expected<std::uint32_t, bus_error> override;
  auto on_write(
      register_file& regs,
      std::uint32_t  offset,
      std::uint8_t   size,
      std::uint32_t  old,
      std::uint32_t  value,
      access_ctx     ctx) -> std::expected<void, bus_error> override;
  void save(std::vector<std::byte>& out) const override;
  void load(std::span<std::byte const> in) override;
  void hold(vtime from, vtime until) override;

  void enable_written(std::uint32_t old, std::uint32_t now);
  void command_written(std::uint32_t old, std::uint32_t now);
  void status_written(std::uint32_t old, std::uint32_t now);
  void clear_written(std::uint32_t old, std::uint32_t now);
  void plain_written(std::uint32_t old, std::uint32_t now);
  void trigger_written(std::uint32_t old, std::uint32_t now);

private:
  void               restore_reset_values(std::optional<reset_kind> kind);
  [[nodiscard]] auto step() const -> phase;
  [[nodiscard]] auto enabled() const -> bool;
  [[nodiscard]] auto to_load() const -> std::uint32_t;
  [[nodiscard]] auto length_ps(std::uint32_t periods) -> std::uint64_t;
  [[nodiscard]] auto target_at() const -> vtime;
  [[nodiscard]] auto released() const -> std::uint64_t;

  void               enter(phase next);
  void               begin(phase next, std::uint32_t periods, std::uint64_t from_ps);
  void               begin_start(std::uint64_t from_ps);
  void               begin_stop(std::uint64_t from_ps);
  void               next_byte(std::uint64_t from_ps);
  void               after_byte(std::uint64_t from_ps);
  void               transfer_done(std::uint64_t from_ps);
  void               address_done();
  void               byte_done();
  void               stop_done();
  void               lost_done();
  void               hold_begun();
  void               hold_ended(std::uint32_t id);
  void               take_bus();
  [[nodiscard]] auto held() const -> bool;
  void               lose();
  [[nodiscard]] auto failed(i2c_error error) -> bool;
  void               abort();
  void               flags();
  void               warn_unsupported();

  register_file&                               regs_;
  std::vector<std::shared_ptr<i2c_target>>     targets_;
  reg_ref                                      command_;
  reg_ref                                      status_;
  reg_ref                                      clear_;
  reg_ref                                      transmit_;
  reg_ref                                      receive_;
  field_ref                                    enable_;
  field_ref                                    start_;
  field_ref                                    stop_;
  field_ref                                    count_;
  field_ref                                    reload_;
  field_ref                                    autoend_;
  field_ref                                    direction_;
  field_ref                                    address_;
  field_ref                                    ten_bit_;
  field_ref                                    scl_low_;
  field_ref                                    scl_high_;
  field_ref                                    prescaler_;
  field_ref                                    tx_empty_;
  field_ref                                    tx_interrupt_;
  field_ref                                    rx_full_;
  field_ref                                    transfer_complete_;
  field_ref                                    reload_complete_;
  field_ref                                    stop_detected_;
  field_ref                                    nack_received_;
  field_ref                                    busy_;
  field_ref                                    bus_error_;
  field_ref                                    arbitration_lost_;
  field_ref                                    overrun_;
  std::uint32_t                                command_set_only_{};
  std::uint32_t                                status_reset_mask_{};
  std::vector<std::pair<field_ref, field_ref>> clears_;
  std::vector<std::pair<field_ref, field_ref>> events_;
  std::vector<field_ref>                       errors_;
  field_ref                                    error_enable_;
  std::vector<field_ref>                       unsupported_;
  core::clock_domain                           kernel_;
  irq_line&                                    event_line_;
  irq_line&                                    error_line_;
  field_ref                                    tx_dma_enable_;
  field_ref                                    rx_dma_enable_;
  std::optional<field_ref>                     complete_dma_enable_;
  std::optional<field_ref>                     reload_dma_enable_;
  dma_signal                                   tx_dma_;
  dma_signal                                   rx_dma_;
  dma_signal                                   event_dma_;
  bool                                         unsupported_logged_{};
  bool                                         no_clock_logged_{};
};

constexpr std::array<write_entry<i2c>, 11> i2c_writes{
    {
     {"CR1", &i2c::enable_written},
     {"CR2", &i2c::command_written},
     {"OAR1", &i2c::plain_written},
     {"OAR2", &i2c::plain_written},
     {"TIMINGR", &i2c::plain_written},
     {"TIMEOUTR", &i2c::plain_written},
     {"ISR", &i2c::status_written},
     {"ICR", &i2c::clear_written},
     {"RXDR", &i2c::plain_written},
     {"TXDR", &i2c::plain_written},
     {"I2C_AUTOCR", &i2c::trigger_written},
     }
};

static_assert(i2c_writes.back().reg == "I2C_AUTOCR");

auto i2c_table(peripheral_desc const& desc) -> std::span<write_entry<i2c> const>
{
  auto const all = std::span{i2c_writes};
  return find_register(desc, "I2C_AUTOCR") != nullptr ? all : all.first(all.size() - 1);
}

i2c::i2c(
    model_context&                           ctx,
    peripheral_desc const&                   desc,
    std::vector<std::shared_ptr<i2c_target>> targets)
    : dispatch_model{ctx, desc, "I2C", i2c_table(desc)}, regs_{ctx.regs},
      targets_{std::move(targets)}, command_{reg("CR2")}, status_{reg("ISR")}, clear_{reg("ICR")},
      transmit_{reg("TXDR")}, receive_{reg("RXDR")}, enable_{field("CR1", "PE")},
      start_{field("CR2", "START")}, stop_{field("CR2", "STOP")}, count_{field("CR2", "NBYTES")},
      reload_{field("CR2", "RELOAD")}, autoend_{field("CR2", "AUTOEND")},
      direction_{field("CR2", "RD_WRN")}, address_{field("CR2", "SADD")},
      ten_bit_{field("CR2", "ADD10")}, scl_low_{field("TIMINGR", "SCLL")},
      scl_high_{field("TIMINGR", "SCLH")}, prescaler_{field("TIMINGR", "PRESC")},
      tx_empty_{field("ISR", "TXE")}, tx_interrupt_{field("ISR", "TXIS")},
      rx_full_{field("ISR", "RXNE")}, transfer_complete_{field("ISR", "TC")},
      reload_complete_{field("ISR", "TCR")}, stop_detected_{field("ISR", "STOPF")},
      nack_received_{field("ISR", "NACKF")}, busy_{field("ISR", "BUSY")},
      bus_error_{field("ISR", "BERR")}, arbitration_lost_{field("ISR", "ARLO")},
      overrun_{field("ISR", "OVR")}, error_enable_{field("CR1", "ERRIE")},
      kernel_{ctx.clock.domain(lower(desc.name))}, event_line_{irq(desc.name + "_EV_IRQn")},
      error_line_{irq(desc.name + "_ER_IRQn")}, tx_dma_enable_{field("CR1", "TXDMAEN")},
      rx_dma_enable_{field("CR1", "RXDMAEN")},
      complete_dma_enable_{
          has_register("I2C_AUTOCR") ? std::optional{field("I2C_AUTOCR", "TCDMAEN")}
                                     : std::nullopt},
      reload_dma_enable_{
          has_register("I2C_AUTOCR") ? std::optional{field("I2C_AUTOCR", "TCRDMAEN")}
                                     : std::nullopt},
      tx_dma_{ctx.peers, lower(desc.name) + "_tx_dma"},
      rx_dma_{ctx.peers, lower(desc.name) + "_rx_dma"},
      event_dma_{ctx.peers, lower(desc.name) + "_evc_dma"}
{
  for(auto const name : {"START", "STOP", "NACK", "PECBYTE"})
  {
    command_set_only_ |= field("CR2", name).mask();
  }
  for(auto const name :
      {"BUSY", "TXIS", "RXNE", "ADDR", "NACKF", "TCR", "TC", "STOPF", "BERR", "ARLO", "PECERR",
       "TIMEOUT", "ALERT", "OVR"})
  {
    status_reset_mask_ |= field("ISR", name).mask();
  }
  for(auto const& rule : desc.cross_rules)
  {
    if(rule.effect == cross_effect::clear and rule.reg == "ICR" and rule.target_reg == "ISR")
    {
      clears_.emplace_back(field("ICR", rule.field), field("ISR", rule.target_field));
    }
  }
  for(auto const& [enable, flag] : std::array<std::pair<std::string_view, std::string_view>, 7>{
          {
           {"TXIE", "TXIS"},
           {"RXIE", "RXNE"},
           {"ADDRIE", "ADDR"},
           {"NACKIE", "NACKF"},
           {"STOPIE", "STOPF"},
           {"TCIE", "TC"},
           {"TCIE", "TCR"},
           }
  })
  {
    events_.emplace_back(field("CR1", enable), field("ISR", flag));
  }
  for(auto const name : {"BERR", "ARLO", "OVR", "PECERR", "TIMEOUT", "ALERT"})
  {
    errors_.push_back(field("ISR", name));
  }
  for(auto const& [reg_name, name] : std::array<std::pair<std::string_view, std::string_view>, 10>{
          {
           {"CR1", "SBC"},
           {"CR1", "NOSTRETCH"},
           {"CR1", "GCEN"},
           {"CR1", "SMBHEN"},
           {"CR1", "SMBDEN"},
           {"CR1", "ALERTEN"},
           {"CR1", "PECEN"},
           {"OAR1", "OA1EN"},
           {"OAR2", "OA2EN"},
           {"I2C_AUTOCR", "TRIGEN"},
           }
  })
  {
    if(has_register(reg_name))
    {
      unsupported_.push_back(field(reg_name, name));
    }
  }
}

auto i2c::step() const -> phase
{
  return static_cast<phase>(state_.step);
}

void i2c::enter(phase next)
{
  state_.step = static_cast<std::uint8_t>(next);
}

auto i2c::enabled() const -> bool
{
  return get(regs_, enable_) != 0;
}

auto i2c::to_load() const -> std::uint32_t
{
  auto const shifting = step() == phase::byte and state_.read == 0 ? 1u : 0u;
  auto const taken    = shifting + state_.tx_full;
  return state_.left > taken ? state_.left - taken : 0;
}

auto i2c::length_ps(std::uint32_t periods) -> std::uint64_t
{
  auto const hz = ctx_.clock.hz(kernel_);
  if(hz == 0)
  {
    if(not std::exchange(no_clock_logged_, true))
    {
      warn("TIMINGR", "the I2C kernel clock is stopped: the bus does not move");
    }
    return 0;
  }
  auto const cycles = std::uint64_t{get(regs_, scl_low_) + 1u + get(regs_, scl_high_) + 1u}
                    * (get(regs_, prescaler_) + 1u);
  return std::uint64_t{periods} * cycles * ps_per_second / hz;
}

auto i2c::target_at() const -> vtime
{
  return vtime{state_.end_ps + ctx_.clock.since_power_on().ps - ctx_.clock.time().ps};
}

auto i2c::released() const -> std::uint64_t
{
  return std::max(ctx_.clock.time().ps, state_.end_ps);
}

void i2c::begin(phase next, std::uint32_t periods, std::uint64_t from_ps)
{
  enter(next);
  ++state_.generation;
  auto const ps = length_ps(periods);
  if(ps == 0)
  {
    return;
  }
  state_.end_ps = std::max(from_ps + ps, ctx_.clock.time().ps);
  ctx_.events.schedule_at(vtime{state_.end_ps}, phase_done, state_.generation, ctx_.clock);
}

void i2c::begin_start(std::uint64_t from_ps)
{
  warn_unsupported();
  update(regs_, busy_, 1);
  begin(phase::address, address_length, from_ps);
}

void i2c::begin_stop(std::uint64_t from_ps)
{
  begin(phase::stop, stop_length, from_ps);
}

void i2c::next_byte(std::uint64_t from_ps)
{
  if(state_.left == 0)
  {
    transfer_done(from_ps);
    return;
  }
  if(state_.read != 0)
  {
    begin(phase::byte, byte_length, from_ps);
    return;
  }
  if(state_.tx_full == 0)
  {
    enter(phase::wait_tx);
    return;
  }
  state_.shift   = state_.tx_data;
  state_.tx_full = 0;
  begin(phase::byte, byte_length, from_ps);
}

void i2c::after_byte(std::uint64_t from_ps)
{
  if(get(regs_, stop_) != 0)
  {
    begin_stop(from_ps);
    return;
  }
  next_byte(from_ps);
}

void i2c::transfer_done(std::uint64_t from_ps)
{
  if(get(regs_, reload_) != 0)
  {
    update(regs_, reload_complete_, 1);
    enter(phase::wait_reload);
    return;
  }
  if(get(regs_, autoend_) != 0 or get(regs_, stop_) != 0)
  {
    begin_stop(from_ps);
    return;
  }
  if(get(regs_, start_) != 0)
  {
    begin_start(from_ps);
    return;
  }
  update(regs_, transfer_complete_, 1);
  enter(phase::wait_end);
}

void i2c::address_done()
{
  update(regs_, start_, 0);
  state_.read        = static_cast<std::uint8_t>(get(regs_, direction_));
  state_.left        = get(regs_, count_);
  state_.target      = no_target;
  auto const address = static_cast<std::uint8_t>((get(regs_, address_) >> 1) & 0x7Fu);
  auto       reply   = i2c_reply{.ack = false};
  if(get(regs_, ten_bit_) == 0)
  {
    for(std::size_t index = 0; index < targets_.size(); ++index)
    {
      if(targets_[index]->address() == address)
      {
        state_.target = static_cast<std::uint32_t>(index);
        reply         = targets_[index]->start(state_.read != 0, target_at());
        break;
      }
    }
  }
  if(failed(reply.error))
  {
    return;
  }
  state_.end_ps += reply.stretch_ps;
  if(not reply.ack)
  {
    update(regs_, nack_received_, 1);
    begin_stop(state_.end_ps);
    return;
  }
  next_byte(state_.end_ps);
}

void i2c::byte_done()
{
  --state_.left;
  auto* const target = state_.target != no_target ? targets_[state_.target].get() : nullptr;
  if(state_.read == 0)
  {
    auto const reply =
        target != nullptr ? target->write(state_.shift, target_at()) : i2c_reply{.ack = false};
    if(failed(reply.error))
    {
      return;
    }
    state_.end_ps += reply.stretch_ps;
    if(not reply.ack)
    {
      update(regs_, nack_received_, 1);
      begin_stop(state_.end_ps);
      return;
    }
    after_byte(state_.end_ps);
    return;
  }
  auto const reply = target != nullptr ? target->read(target_at()) : i2c_read_reply{};
  if(failed(reply.error))
  {
    return;
  }
  state_.end_ps += reply.stretch_ps;
  auto const data = reply.byte;
  if(get(regs_, rx_full_) != 0)
  {
    state_.rx_held = 1;
    state_.rx_data = data;
    enter(phase::wait_rx);
    return;
  }
  regs_.poke(receive_.offset, data);
  update(regs_, rx_full_, 1);
  after_byte(state_.end_ps);
}

void i2c::stop_done()
{
  enter(phase::idle);
  update(regs_, stop_, 0);
  update(regs_, busy_, 0);
  update(regs_, stop_detected_, 1);
  state_.left = 0;
  if(state_.target != no_target)
  {
    auto const target = std::exchange(state_.target, no_target);
    targets_[target]->stop(target_at());
  }
  if(state_.pending_waits != 0)
  {
    take_bus();
  }
  if(get(regs_, start_) != 0 and not held())
  {
    begin_start(state_.end_ps);
  }
}

auto i2c::failed(i2c_error error) -> bool
{
  switch(error)
  {
    case i2c_error::none:
      return false;
    case i2c_error::bus_error:
      update(regs_, bus_error_, 1);
      return false;
    case i2c_error::overrun:
      update(regs_, overrun_, 1);
      return false;
    case i2c_error::arbitration_lost:
      lose();
      return true;
  }
  return false;
}

void i2c::lose()
{
  update(regs_, arbitration_lost_, 1);
  update(regs_, start_, 0);
  begin(phase::lost, stop_length, state_.end_ps);
}

void i2c::lost_done()
{
  enter(phase::idle);
  update(regs_, stop_, 0);
  update(regs_, busy_, 0);
  if(state_.target != no_target)
  {
    auto const target = std::exchange(state_.target, no_target);
    targets_[target]->stop(target_at());
  }
  if(state_.pending_waits != 0)
  {
    take_bus();
  }
  if(get(regs_, start_) != 0 and not held())
  {
    begin_start(state_.end_ps);
  }
}

auto i2c::held() const -> bool
{
  return state_.active.id != 0;
}

void i2c::hold(vtime from, vtime until)
{
  if(state_.pending.id != 0)
  {
    ctx_.events.cancel(state_.pending.begin_seq);
    ctx_.events.cancel(state_.pending.end_seq);
  }
  auto const id        = ++state_.holds;
  state_.pending_waits = 0;
  state_.pending       = {
      .id        = id,
      .from_ps   = from.ps,
      .until_ps  = until.ps,
      .begin_seq = ctx_.events.schedule_at(from, hold_begin, id, core::host_lane, ctx_.clock),
      .end_seq   = ctx_.events.schedule_at(until, hold_end, id, core::host_lane, ctx_.clock)};
}

void i2c::hold_begun()
{
  if(step() != phase::idle)
  {
    state_.pending_waits = 1;
    return;
  }
  take_bus();
}

void i2c::take_bus()
{
  if(held())
  {
    ctx_.events.cancel(state_.active.end_seq);
  }
  state_.active        = std::exchange(state_.pending, {});
  state_.pending_waits = 0;
  update(regs_, busy_, enabled() ? 1 : 0);
}

void i2c::hold_ended(std::uint32_t id)
{
  if(id == state_.pending.id)
  {
    state_.pending       = {};
    state_.pending_waits = 0;
    return;
  }
  if(id != state_.active.id or not held())
  {
    return;
  }
  auto const until = state_.active.until_ps;
  state_.active    = {};
  if(state_.pending.id != 0 and state_.pending.from_ps <= until)
  {
    ctx_.events.cancel(state_.pending.begin_seq);
    take_bus();
    return;
  }
  update(regs_, busy_, 0);
  if(enabled() and get(regs_, start_) != 0)
  {
    begin_start(until);
  }
}

void i2c::abort()
{
  ++state_.generation;
  auto const lost = step() == phase::lost;
  enter(phase::idle);
  state_.left    = 0;
  state_.tx_full = 0;
  state_.rx_held = 0;
  if(lost && state_.target != no_target)
  {
    targets_[state_.target]->stop(vtime{ctx_.clock.since_power_on().ps});
  }
  state_.target = no_target;
  regs_.poke(command_.offset, regs_.peek(command_.offset) & ~command_set_only_);
  regs_.poke(status_.offset, (regs_.peek(status_.offset) & ~status_reset_mask_) | tx_empty_.mask());
  if(state_.pending_waits != 0)
  {
    take_bus();
  }
}

void i2c::reset(register_file&, reset_kind kind)
{
  restore_reset_values(kind);
}

void i2c::on_bus_reset()
{
  restore_reset_values(std::nullopt);
}

void i2c::restore_reset_values(std::optional<reset_kind> kind)
{
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
  auto const generation = state_.generation + 1;
  auto const holds      = state_.holds;
  state_                = {};
  state_.generation     = generation;
  state_.holds          = holds;
  tx_dma_.resolve();
  rx_dma_.resolve();
  event_dma_.resolve();
  if(kind)
  {
    for(auto const& target : targets_)
    {
      target->reset(*kind);
    }
  }
  flags();
}

void i2c::power_cut()
{
  auto const         at = ctx_.clock.since_power_on();
  std::exception_ptr first;
  for(auto const& target : targets_)
  {
    try
    {
      target->power_cut(at);
    }
    catch(...)
    {
      if(not first)
      {
        first = std::current_exception();
      }
    }
  }
  if(first)
  {
    std::rethrow_exception(first);
  }
}

void i2c::flags()
{
  update(regs_, tx_empty_, state_.tx_full != 0 ? 0 : 1);
  auto const sending = state_.read == 0 and (step() == phase::byte or step() == phase::wait_tx);
  update(regs_, tx_interrupt_, sending and state_.tx_full == 0 and to_load() != 0 ? 1 : 0);
  auto event = false;
  for(auto const& [enable, flag] : events_)
  {
    event = event or (get(regs_, enable) != 0 and get(regs_, flag) != 0);
  }
  auto error = false;
  for(auto const flag : errors_)
  {
    error = error or get(regs_, flag) != 0;
  }
  error = error and get(regs_, error_enable_) != 0;
  if(event)
  {
    event_line_.raise();
  }
  else
  {
    event_line_.lower();
  }
  if(error)
  {
    error_line_.raise();
  }
  else
  {
    error_line_.lower();
  }
  auto const enabled = [this](std::optional<field_ref> const& enable) {
    return enable and get(regs_, *enable) != 0;
  };
  auto const complete = enabled(complete_dma_enable_) and get(regs_, transfer_complete_) != 0;
  auto const reload   = enabled(reload_dma_enable_) and get(regs_, reload_complete_) != 0;
  tx_dma_.set(get(regs_, tx_dma_enable_) != 0 and get(regs_, tx_interrupt_) != 0);
  rx_dma_.set(get(regs_, rx_dma_enable_) != 0 and get(regs_, rx_full_) != 0);
  event_dma_.set(complete or reload);
}

void i2c::warn_unsupported()
{
  auto const ten_bit = get(regs_, ten_bit_) != 0;
  auto const other =
      std::ranges::any_of(unsupported_, [&](field_ref bits) { return get(regs_, bits) != 0; });
  auto const used = ten_bit or other;
  if(used and not std::exchange(unsupported_logged_, true))
  {
    warn(
        "CR1",
        "target mode, 10-bit addressing, SMBus, PEC or the autonomous trigger is configured: not "
        "modelled, the I2C runs as a plain 7-bit controller");
  }
}

void i2c::enable_written(std::uint32_t old, std::uint32_t now)
{
  if(enable_.of(old) != 0 and enable_.of(now) == 0)
  {
    abort();
  }
  if(enable_.of(old) == 0 and enable_.of(now) != 0 and held())
  {
    update(regs_, busy_, 1);
  }
}

void i2c::command_written(std::uint32_t old, std::uint32_t now)
{
  auto word = now | (old & command_set_only_);
  if(not enabled())
  {
    regs_.poke(command_.offset, word & ~command_set_only_);
    return;
  }
  regs_.poke(command_.offset, word);
  auto const started = start_.of(old) == 0 and start_.of(word) != 0;
  auto const stopped = stop_.of(old) == 0 and stop_.of(word) != 0;
  auto const time    = released();
  switch(step())
  {
    case phase::idle:
      if(stopped)
      {
        update(regs_, stop_, 0);
      }
      if(started and not held())
      {
        begin_start(ctx_.clock.time().ps);
      }
      return;
    case phase::wait_end:
      if(stopped)
      {
        update(regs_, transfer_complete_, 0);
        begin_stop(time);
      }
      else if(started)
      {
        update(regs_, transfer_complete_, 0);
        begin_start(time);
      }
      return;
    case phase::wait_reload:
      if(get(regs_, count_) != 0)
      {
        update(regs_, reload_complete_, 0);
        state_.left = get(regs_, count_);
        next_byte(time);
      }
      return;
    case phase::wait_tx:
      if(stopped)
      {
        begin_stop(time);
      }
      return;
    case phase::address:
    case phase::byte:
    case phase::stop:
    case phase::wait_rx:
    case phase::lost:
      return;
  }
}

void i2c::status_written(std::uint32_t old, std::uint32_t now)
{
  regs_.poke(status_.offset, old);
  if(tx_empty_.of(now) != 0 and tx_empty_.of(old) == 0)
  {
    state_.tx_full = 0;
  }
}

void i2c::clear_written(std::uint32_t, std::uint32_t now)
{
  for(auto const& [clear, flag] : clears_)
  {
    if(clear.of(now) != 0)
    {
      update(regs_, flag, 0);
    }
  }
  regs_.poke(clear_.offset, 0);
}

void i2c::plain_written(std::uint32_t, std::uint32_t)
{}

void i2c::trigger_written(std::uint32_t, std::uint32_t)
{
  warn_unsupported();
}

auto i2c::on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx access)
    -> std::expected<std::uint32_t, bus_error>
{
  if((offset & ~3u) != receive_.offset or access.by == initiator::debugger)
  {
    return dispatch_model::on_read(regs, offset, size, access);
  }
  auto const value = dispatch_model::on_read(regs, offset, size, access);
  if(get(regs_, rx_full_) != 0)
  {
    update(regs_, rx_full_, 0);
    if(std::exchange(state_.rx_held, 0) != 0)
    {
      regs_.poke(receive_.offset, state_.rx_data);
      update(regs_, rx_full_, 1);
      after_byte(released());
    }
  }
  flags();
  return value;
}

auto i2c::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     access) -> std::expected<void, bus_error>
{
  if((offset & ~3u) == transmit_.offset)
  {
    auto const data = static_cast<std::uint8_t>(merge_write(0, offset, size, value) & 0xFFu);
    if(enabled() and state_.tx_full == 0)
    {
      state_.tx_full = 1;
      state_.tx_data = data;
      regs_.poke(transmit_.offset, data);
      if(step() == phase::wait_tx)
      {
        next_byte(released());
      }
    }
    flags();
    return {};
  }
  auto const result = dispatch_model::on_write(regs, offset, size, old, value, access);
  flags();
  return result;
}

void i2c::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind == hold_begin and payload == state_.pending.id)
  {
    hold_begun();
    flags();
    return;
  }
  if(kind == hold_end)
  {
    hold_ended(static_cast<std::uint32_t>(payload));
    flags();
    return;
  }
  if(kind != phase_done or payload != state_.generation)
  {
    return;
  }
  switch(step())
  {
    case phase::address:
      address_done();
      break;
    case phase::byte:
      byte_done();
      break;
    case phase::stop:
      stop_done();
      break;
    case phase::lost:
      lost_done();
      break;
    case phase::idle:
    case phase::wait_tx:
    case phase::wait_rx:
    case phase::wait_reload:
    case phase::wait_end:
      return;
  }
  flags();
}

void i2c::save(std::vector<std::byte>& out) const
{
  auto copy = state_;
  copy.devices.clear();
  for(auto const& target : targets_)
  {
    target->save(copy.devices.emplace_back());
  }
  reflex::serde::bson::serializer{out}.dump(copy);
}

void i2c::load(std::span<std::byte const> in)
{
  state_ = reflex::serde::bson::deserializer{in}.template load<i2c_state>();
  for(std::size_t index = 0; index < targets_.size() and index < state_.devices.size(); ++index)
  {
    targets_[index]->load(state_.devices[index]);
  }
  state_.devices.clear();
}
}

auto i2c_factory(board const& b) -> model_factory
{
  return
      [targets = b.i2c](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
        auto const found = targets.find(desc.name);
        return std::make_shared<i2c>(
            ctx, desc,
            found != targets.end() ? found->second : std::vector<std::shared_ptr<i2c_target>>{});
      };
}
}
