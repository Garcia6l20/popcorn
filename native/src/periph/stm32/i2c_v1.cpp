#include "factories.hpp"
#include "model_base.hpp"
#include "variants.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/scheduler.hpp>
#include <popcorn/periph/i2c.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
namespace
{
constexpr std::uint64_t ps_per_second = 1'000'000'000'000;
constexpr std::uint64_t hz_per_mhz    = 1'000'000;
constexpr std::uint32_t phase_done    = 0;
constexpr std::uint32_t byte_periods  = 9;

enum class wire : std::uint8_t
{
  idle,
  start,
  address,
  data,
  stop,
};

enum timing_warning : std::uint8_t
{
  freq_mismatch = 1u << 0,
  clock_too_low = 1u << 1,
  ccr_too_low   = 1u << 2,
  trise_off     = 1u << 3,
  clock_stopped = 1u << 4,
};

struct i2c_v1_state
{
  std::uint32_t                       generation{};
  std::uint8_t                        phase{};
  std::uint8_t                        status_read{};
  std::uint8_t                        tx_full{};
  std::uint8_t                        shift{};
  std::uint8_t                        address{};
  std::uint64_t                       phase_end_ps{};
  std::uint64_t                       released_ps{};
  std::vector<std::uint8_t>           acked;
  std::vector<std::uint8_t>           in_transfer;
  std::vector<std::vector<std::byte>> targets;

  friend auto operator==(i2c_v1_state const&, i2c_v1_state const&) -> bool = default;
};

auto kernel_name(peripheral_desc const& desc, i2c_v1_variant const& variant) -> std::string
{
  auto const* const bus = window_of(variant.kernel_buses, static_cast<std::uint32_t>(desc.base));
  if(bus == nullptr)
  {
    throw config_error(
        desc.name, 0, "base", "an address on one of the buses that clock the I2C model");
  }
  return std::string{bus->clock};
}

/**
 * @brief I2C1 and I2C2 of the STM32L1, RM0038 section 26 "Inter-integrated circuit (I2C)
 *        interface", controller transmitter with 7-bit addresses on the board's i2c_target
 *        targets.
 *
 *        Timing, RM0038 26.6.8: the SCL period is Thigh + Tlow of the APB clock of the
 *        instance's bus, PCLK1, 2 x CCR cycles in Sm mode, 3 x CCR with F/S 1 and DUTY 0 and
 *        25 x CCR with DUTY 1, a CCR of 0 counting as 1, rounded down to a whole picosecond. A
 *        START takes 1 period, the address and each data byte 9 with the acknowledge, a STOP 1.
 *        The first START of each PE period with PCLK1 running checks CR2.FREQ against the PCLK1
 *        rate in MHz, the PCLK1 rate against 2 MHz in Sm mode and 4 MHz in Fm mode, RM0038
 *        26.3.3, CCR against its minimum of 4, 1 with DUTY 1, and TRISE against at least the rise
 *        time RM0038 26.6.9 asks, 1000 ns in Sm mode and 300 ns in Fm mode times FREQ plus 1, the
 *        filter value on top allowed, and logs one model_warning per rule and instance. The SCL
 *        period follows CCR and PCLK1 whatever FREQ and TRISE hold.
 *
 *        Controller transmitter, RM0038 26.3.3 and Figure 212: CR1.START with PE 1 sends a
 *        START, BUSY 1 from its start. At its end SB, MSL and BUSY are 1 and START is cleared.
 *        A DR write after an SR1 read clears SB and sends DR as the address byte, EV5. At the end
 *        of the address byte every target is asked i2c_target::matches(), in attachment order,
 *        and those it matches answer through i2c_target::start_deferred(): one acknowledge sets
 *        ADDR, TRA follows the R/W bit, and with none AF is set. The I2C keeps SCL low until an
 *        SR2 read after an SR1 read clears ADDR, EV6, as RM0038 26.6.7 says, an SR1 read before
 *        ADDR is set included. TxE then sets. The first DR write goes to the shift register with
 *        TxE kept 1, EV8_1, and each byte reaches the targets that acknowledged the address
 *        through i2c_target::write_deferred() at its end. A DR write while a byte shifts fills
 *        DR and clears TxE, EV8, and at the acknowledge DR moves to the shift register with TxE
 *        1. A byte acknowledged with DR empty sets BTF and SCL stays low until a DR write, which
 *        clears BTF and starts the byte with TxE kept 1. A DR read clears BTF too. A NACK sets
 *        AF, neither TxE nor BTF. CR1.STOP sends a STOP at once while SB is set or SCL is held
 *        after a byte or a NACK, after the current byte while one shifts, and after the ADDR
 *        clear while ADDR is set. CR1.START with MSL 1 sends a repeated START by the same rule.
 *        The STOP's end clears STOP, BUSY, MSL, TRA, TxE and BTF and reaches every target an
 *        address of the transfer matched through i2c_target::stop(). A repeated START clears TRA,
 *        TxE and BTF. A target's stretch_ps holds SCL after its byte: the next phase starts at
 *        the byte's end plus the longest stretch. Targets share the open-drain bus: one
 *        acknowledge acknowledges. The answer of a target that carries an i2c_error logs one
 *        model_warning per instance and the byte completes as its acknowledge says. An SR1 read
 *        arms both clear sequences until the SR2 read that clears ADDR or the DR write that
 *        clears SB, an SR2 read with ADDR clear leaving it armed. Inferred: a DR write with SB set
 *        and no SR1 read since the last SB or ADDR clear, or with ADDR set, stores DR and sends
 *        nothing. The DR write that clears SB takes the SR1 read, so the ADDR clear needs an SR1
 *        read after it. A DR write during the address phase or after a data NACK stores DR and
 *        sends nothing.
 *
 *        An address with the R/W bit set completes its address phase as above. Controller
 *        receive, SMBus, PEC, ALERT, ARP and CR2.DMAEN log one model_warning per instance when
 *        the firmware uses them, raise model_error under strict_peripherals at the START or the
 *        ADDR clear that uses them, and the bus stays idle for them. OAR1, OAR2, ENGC and
 *        NOSTRETCH are stored: no controller reaches the instance as a target, so it answers no
 *        address. ADD10 stays 0: no i2c_target answers a 10-bit header, so a header address
 *        byte takes the NACK path and reaches no i2c_target::matches().
 *
 *        Interrupts, RM0038 Table 139 and Figure 214: the event line follows SB, ADDR, ADD10,
 *        STOPF and BTF with CR2.ITEVTEN, TxE and RxNE with ITEVTEN and ITBUFEN, the error line
 *        BERR, ARLO, AF, OVR, PECERR, TIMEOUT and SMBALERT with ITERREN. SR1's error flags clear
 *        by a write of 0. PE 0 ends the transfer at once and clears START, STOP, ACK, POS, PEC
 *        and ALERT, every SR1 flag and SR2's MSL, TRA and BUSY. PE 0 during a controller
 *        transfer, which RM0038 26.6.1 forbids, logs one model_warning per instance. CR1.SWRST 1
 *        resets the I2C as an RCC reset does and keeps SWRST 1. While SWRST is 1 a CR1 write
 *        changes nothing but the SWRST clear, which leaves CR1 0. Neither tells the targets.
 *        Debugger accesses to SR1, SR2 and DR have no side effect and a debugger DR write sends
 *        nothing.
 *
 *        Targets, the STM32L1 I2C on the generic interfaces: each target is attached to the model
 *        at its construction and detached at its destruction, and reset with the machine. A
 *        target that leaves an answer pending is taken as NACK, and the phase's event fails the
 *        run with config_error: i2c_target_port::complete() and complete_read() throw
 *        config_error. The model takes no board::i2c_controllers entry, offers no
 *        i2c_controller_port, i2c_link_port nor i2c_bus, so an STM32L1 I2C is a link end of no
 *        linked system and takes no host hold. The targets, the phase in flight and the set of
 *        targets of the transfer are saved with the model, and a snapshot with another number of
 *        targets than the machine's is refused with config_error. An RCC reset moves the event
 *        generation, so the event of a phase in flight acts on nothing.
 */
class i2c_v1 final : public dispatch_model<i2c_v1, i2c_v1_state>,
                     public i2c_target_port,
                     public bus_reset_listener
{
public:
  i2c_v1(
      model_context&                           ctx,
      peripheral_desc const&                   desc,
      std::vector<std::shared_ptr<i2c_target>> targets,
      bool                                     controlled,
      i2c_v1_variant const&                    variant);
  i2c_v1(i2c_v1 const&)                    = delete;
  auto operator=(i2c_v1 const&) -> i2c_v1& = delete;
  i2c_v1(i2c_v1&&)                         = delete;
  auto operator=(i2c_v1&&) -> i2c_v1&      = delete;
  ~i2c_v1() override;

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
  void complete(std::uint64_t ticket, i2c_reply reply, vtime at, std::uint32_t lane) override;
  void complete_read(std::uint64_t ticket, i2c_read_reply reply, vtime at, std::uint32_t lane)
      override;

  void control_written(std::uint32_t old, std::uint32_t now);
  void control2_written(std::uint32_t old, std::uint32_t now);
  void status_written(std::uint32_t old, std::uint32_t now);

private:
  void               restore_reset_values(std::optional<reset_kind> kind);
  [[nodiscard]] auto step() const -> wire;
  [[nodiscard]] auto controlling() const -> bool;
  [[nodiscard]] auto length_ps(std::uint32_t periods) -> std::optional<std::uint64_t>;
  [[nodiscard]] auto power_on_offset() const -> std::uint64_t;
  [[nodiscard]] auto ticket_of(std::size_t index) const -> std::uint64_t;
  void               begin(wire next, std::uint32_t periods);
  void               begin_start();
  void               check_timing();
  void               check_unmodelled();
  void               note(std::uint8_t warning, std::string_view reg, std::string message);
  void               unmodelled(bool& logged, std::string_view name, std::string_view what);
  void               phase_ended();
  void               start_ended();
  void               address_ended();
  void               data_ended();
  void               stop_ended();
  void               address_cleared();
  void               data_written();
  void               shift_out(std::uint8_t byte);
  [[nodiscard]] auto pending_condition() -> bool;
  void               take(std::size_t index, std::optional<i2c_reply> const& reply, bool& ack);
  void               abort();
  void               flags();
  void               rethrow_refused();
  [[noreturn]] void  refuse_completion() const;

  register_file&                           regs_;
  std::vector<std::shared_ptr<i2c_target>> targets_;
  reg_ref                                  control_;
  reg_ref                                  data_;
  reg_ref                                  status1_;
  reg_ref                                  status2_;
  field_ref                                enable_;
  field_ref                                start_;
  field_ref                                stop_;
  field_ref                                reset_bit_;
  field_ref                                frequency_;
  field_ref                                event_ie_;
  field_ref                                buffer_ie_;
  field_ref                                error_ie_;
  field_ref                                dma_enable_;
  field_ref                                fast_;
  field_ref                                duty_;
  field_ref                                divider_;
  field_ref                                rise_;
  field_ref                                start_bit_;
  field_ref                                address_sent_;
  field_ref                                byte_finished_;
  field_ref                                tx_empty_;
  field_ref                                rx_full_;
  field_ref                                ack_failure_;
  field_ref                                master_;
  field_ref                                busy_;
  field_ref                                transmitter_;
  std::uint32_t                            cleared_by_disable_{};
  std::uint32_t                            status1_events_{};
  std::uint32_t                            status1_errors_{};
  std::uint32_t                            unmodelled_control_{};
  core::clock_domain                       kernel_;
  irq_line&                                event_line_;
  irq_line&                                error_line_;
  std::vector<std::string>                 deferred_;
  std::uint8_t                             timing_logged_{};
  bool                                     timing_checked_{};
  bool                                     error_logged_{};
  bool                                     receive_logged_{};
  bool                                     control_logged_{};
  bool                                     dma_logged_{};
  bool                                     disable_logged_{};
  bool                                     ending_{};
};

constexpr std::array<write_entry<i2c_v1>, 9> i2c_v1_writes{
    {
     {"CR1", &i2c_v1::control_written},
     {"CR2", &i2c_v1::control2_written},
     {"OAR1", nullptr},
     {"OAR2", nullptr},
     {"DR", nullptr},
     {"SR1", &i2c_v1::status_written},
     {"SR2", nullptr},
     {"CCR", nullptr},
     {"TRISE", nullptr},
     }
};

i2c_v1::i2c_v1(
    model_context&                           ctx,
    peripheral_desc const&                   desc,
    std::vector<std::shared_ptr<i2c_target>> targets,
    bool                                     controlled,
    i2c_v1_variant const&                    variant)
    : dispatch_model{ctx, desc, "I2C", i2c_v1_writes}, regs_{ctx.regs},
      targets_{std::move(targets)}, control_{reg("CR1")}, data_{reg("DR")}, status1_{reg("SR1")},
      status2_{reg("SR2")}, enable_{field("CR1", "PE")}, start_{field("CR1", "START")},
      stop_{field("CR1", "STOP")}, reset_bit_{field("CR1", "SWRST")},
      frequency_{field("CR2", "FREQ")}, event_ie_{field("CR2", "ITEVTEN")},
      buffer_ie_{field("CR2", "ITBUFEN")}, error_ie_{field("CR2", "ITERREN")},
      dma_enable_{field("CR2", "DMAEN")}, fast_{field("CCR", "F_S")}, duty_{field("CCR", "DUTY")},
      divider_{field("CCR", "CCR")}, rise_{field("TRISE", "TRISE")}, start_bit_{field("SR1", "SB")},
      address_sent_{field("SR1", "ADDR")}, byte_finished_{field("SR1", "BTF")},
      tx_empty_{field("SR1", "TxE")}, rx_full_{field("SR1", "RxNE")},
      ack_failure_{field("SR1", "AF")}, master_{field("SR2", "MSL")}, busy_{field("SR2", "BUSY")},
      transmitter_{field("SR2", "TRA")}, kernel_{ctx.clock.domain(kernel_name(desc, variant))},
      event_line_{irq(desc.name + "_EV_IRQn")}, error_line_{irq(desc.name + "_ER_IRQn")}
{
  if(controlled)
  {
    throw config_error(
        ctx.part.part, 0, std::format("board.i2c_controllers.{}", desc.name),
        "no controller entry, the STM32L1 I2C model runs as a controller on board targets");
  }
  for(auto const name : {"START", "STOP", "ACK", "POS", "PEC", "ALERT"})
  {
    cleared_by_disable_ |= field("CR1", name).mask();
  }
  for(auto const name : {"SMBUS", "ENPEC", "PEC", "ENARP", "ALERT"})
  {
    unmodelled_control_ |= field("CR1", name).mask();
  }
  for(auto const name : {"SB", "ADDR", "ADD10", "STOPF", "BTF"})
  {
    status1_events_ |= field("SR1", name).mask();
  }
  for(auto const name : {"BERR", "ARLO", "AF", "OVR", "PECERR", "TIMEOUT", "SMBALERT"})
  {
    status1_errors_ |= field("SR1", name).mask();
  }
  std::size_t attached = 0;
  try
  {
    for(; attached < targets_.size(); ++attached)
    {
      targets_[attached]->attached(*this);
    }
  }
  catch(...)
  {
    while(attached > 0)
    {
      targets_[--attached]->detached();
    }
    throw;
  }
}

i2c_v1::~i2c_v1()
{
  for(auto const& target : targets_)
  {
    target->detached();
  }
}

auto i2c_v1::step() const -> wire
{
  return static_cast<wire>(state_.phase);
}

auto i2c_v1::controlling() const -> bool
{
  return get(regs_, master_) != 0;
}

auto i2c_v1::power_on_offset() const -> std::uint64_t
{
  return ctx_.clock.since_power_on().ps - ctx_.clock.time().ps;
}

auto i2c_v1::ticket_of(std::size_t index) const -> std::uint64_t
{
  return std::uint64_t{state_.generation} << 16 | index;
}

auto i2c_v1::length_ps(std::uint32_t periods) -> std::optional<std::uint64_t>
{
  auto const hz = ctx_.clock.hz(kernel_);
  if(hz == 0)
  {
    note(clock_stopped, "CR1", "the I2C bus clock PCLK1 is stopped: no bus phase starts");
    return std::nullopt;
  }
  auto const ccr = std::max<std::uint64_t>(get(regs_, divider_), 1);
  auto const per = get(regs_, fast_) == 0 ? 2 * ccr : (get(regs_, duty_) == 0 ? 3 : 25) * ccr;
  return std::uint64_t{periods} * per * ps_per_second / hz;
}

void i2c_v1::note(std::uint8_t warning, std::string_view reg, std::string message)
{
  if((timing_logged_ & warning) == 0)
  {
    timing_logged_ |= warning;
    warn(reg, std::move(message));
  }
}

void i2c_v1::unmodelled(bool& logged, std::string_view name, std::string_view what)
{
  if(not std::exchange(logged, true))
  {
    warn(name, std::format("{}: not modelled, nothing is clocked for it", what));
  }
  if(ctx_.strict_peripherals)
  {
    throw model_error(std::string{desc_.name}, std::string{name}, reg(name).offset);
  }
}

void i2c_v1::check_timing()
{
  auto const hz = ctx_.clock.hz(kernel_);
  if(hz == 0 or std::exchange(timing_checked_, true))
  {
    return;
  }
  auto const freq = get(regs_, frequency_);
  auto const fast = get(regs_, fast_) != 0;
  if(freq != hz / hz_per_mhz)
  {
    note(
        freq_mismatch, "CR2",
        std::format(
            "FREQ {} for a PCLK1 of {} Hz, RM0038 26.6.2 asks {}", freq, hz, hz / hz_per_mhz));
  }
  if(hz < (fast ? 4 : 2) * hz_per_mhz)
  {
    note(
        clock_too_low, "CR2",
        std::format(
            "a PCLK1 of {} Hz, below the {} MHz RM0038 26.3.3 asks in {} mode", hz, fast ? 4 : 2,
            fast ? "Fm" : "Sm"));
  }
  auto const minimum = fast and get(regs_, duty_) != 0 ? 1u : 4u;
  if(get(regs_, divider_) < minimum)
  {
    note(
        ccr_too_low, "CCR",
        std::format("CCR {} below the minimum {} of RM0038 26.6.8", get(regs_, divider_), minimum));
  }
  auto const rise = fast ? freq * 300 / 1000 + 1 : freq + 1;
  if(get(regs_, rise_) < rise)
  {
    note(
        trise_off, "TRISE",
        std::format(
            "TRISE {} below the {} RM0038 26.6.9 asks for FREQ {} in {} mode", get(regs_, rise_),
            rise, freq, fast ? "Fm" : "Sm"));
  }
}

void i2c_v1::check_unmodelled()
{
  if((regs_.peek(control_.offset) & unmodelled_control_) != 0)
  {
    unmodelled(control_logged_, "CR1", "SMBus, PEC, ALERT or ARP");
  }
  if(get(regs_, dma_enable_) != 0)
  {
    unmodelled(dma_logged_, "CR2", "the DMA requests of CR2.DMAEN");
  }
}

void i2c_v1::begin(wire next, std::uint32_t periods)
{
  auto const ps = length_ps(periods);
  if(not ps)
  {
    return;
  }
  auto const now      = ending_ ? state_.phase_end_ps : ctx_.clock.time().ps;
  auto const from     = std::max(now, state_.released_ps);
  state_.phase        = static_cast<std::uint8_t>(next);
  state_.phase_end_ps = from + *ps;
  ctx_.events.schedule_at(vtime{state_.phase_end_ps}, phase_done, ++state_.generation, ctx_.clock);
}

void i2c_v1::begin_start()
{
  check_unmodelled();
  check_timing();
  update(regs_, busy_, 1);
  begin(wire::start, 1);
}

auto i2c_v1::pending_condition() -> bool
{
  if(get(regs_, stop_) != 0)
  {
    begin(wire::stop, 1);
    return true;
  }
  if(get(regs_, start_) != 0)
  {
    begin_start();
    return true;
  }
  return false;
}

void i2c_v1::control_written(std::uint32_t old, std::uint32_t now)
{
  if(reset_bit_.of(old) != 0)
  {
    regs_.poke(control_.offset, reset_bit_.of(now) != 0 ? old : 0);
    return;
  }
  if(reset_bit_.of(now) != 0)
  {
    restore_reset_values(std::nullopt);
    update(regs_, reset_bit_, 1);
    return;
  }
  if(enable_.of(now) == 0)
  {
    if(enable_.of(old) != 0)
    {
      if(controlling() and not std::exchange(disable_logged_, true))
      {
        warn(
            "CR1", "PE 0 during a controller transfer, which RM0038 26.6.1 forbids: the transfer "
                   "ends at once");
      }
      abort();
    }
    regs_.poke(control_.offset, regs_.peek(control_.offset) & ~start_.mask());
    return;
  }
  if(enable_.of(old) == 0)
  {
    timing_checked_ = false;
  }
  if(step() != wire::idle)
  {
    return;
  }
  if(get(regs_, start_bit_) != 0)
  {
    if(get(regs_, stop_) != 0)
    {
      begin(wire::stop, 1);
    }
    return;
  }
  if(not controlling())
  {
    if(get(regs_, start_) != 0)
    {
      begin_start();
    }
    return;
  }
  if(get(regs_, address_sent_) == 0)
  {
    static_cast<void>(pending_condition());
  }
}

void i2c_v1::control2_written(std::uint32_t, std::uint32_t)
{}

void i2c_v1::status_written(std::uint32_t old, std::uint32_t now)
{
  regs_.poke(status1_.offset, (now & ~status1_errors_) | (old & now & status1_errors_));
}

void i2c_v1::abort()
{
  ++state_.generation;
  state_.phase       = static_cast<std::uint8_t>(wire::idle);
  state_.status_read = 0;
  state_.tx_full     = 0;
  state_.released_ps = 0;
  std::ranges::fill(state_.acked, 0);
  std::ranges::fill(state_.in_transfer, 0);
  regs_.poke(control_.offset, regs_.peek(control_.offset) & ~cleared_by_disable_);
  regs_.poke(status1_.offset, 0);
  for(auto const& bit : {master_, transmitter_, busy_})
  {
    update(regs_, bit, 0);
  }
}

void i2c_v1::shift_out(std::uint8_t byte)
{
  state_.shift = byte;
  update(regs_, tx_empty_, 1);
  update(regs_, byte_finished_, 0);
  begin(wire::data, byte_periods);
}

void i2c_v1::data_written()
{
  auto const byte = static_cast<std::uint8_t>(regs_.peek(data_.offset));
  if(get(regs_, start_bit_) != 0)
  {
    if(std::exchange(state_.status_read, 0) != 0 and step() == wire::idle)
    {
      update(regs_, start_bit_, 0);
      state_.address = byte;
      begin(wire::address, byte_periods);
    }
    return;
  }
  if(not controlling() or get(regs_, transmitter_) == 0 or get(regs_, address_sent_) != 0)
  {
    return;
  }
  if(step() == wire::idle and get(regs_, ack_failure_) == 0)
  {
    shift_out(byte);
    return;
  }
  if(step() == wire::data)
  {
    state_.tx_full = 1;
    update(regs_, tx_empty_, 0);
  }
}

void i2c_v1::address_cleared()
{
  if(get(regs_, transmitter_) == 0)
  {
    unmodelled(receive_logged_, "SR2", "controller receive");
  }
  update(regs_, address_sent_, 0);
  if(get(regs_, transmitter_) != 0)
  {
    update(regs_, tx_empty_, 1);
  }
  static_cast<void>(pending_condition());
}

void i2c_v1::take(std::size_t index, std::optional<i2c_reply> const& reply, bool& ack)
{
  if(not reply)
  {
    deferred_.push_back(std::format("the target at {:#04x}", targets_[index]->address()));
    return;
  }
  if(reply->error != i2c_error::none and not std::exchange(error_logged_, true))
  {
    warn(
        "SR1", std::format(
                   "the {} a target injects: not modelled, the byte completes as its acknowledge "
                   "says",
                   reply->error));
  }
  ack                = ack or reply->ack;
  state_.released_ps = std::max(state_.released_ps, state_.phase_end_ps + reply->stretch_ps);
}

void i2c_v1::start_ended()
{
  regs_.poke(control_.offset, regs_.peek(control_.offset) & ~start_.mask());
  update(regs_, start_bit_, 1);
  update(regs_, master_, 1);
  update(regs_, transmitter_, 0);
  update(regs_, tx_empty_, 0);
  update(regs_, byte_finished_, 0);
  state_.tx_full = 0;
  std::ranges::fill(state_.acked, 0);
  if(get(regs_, stop_) != 0)
  {
    begin(wire::stop, 1);
  }
}

void i2c_v1::address_ended()
{
  auto const at      = vtime{state_.phase_end_ps + power_on_offset()};
  auto const address = static_cast<std::uint8_t>(state_.address >> 1);
  auto const read    = (state_.address & 1u) != 0;
  auto const header  = (state_.address & 0xF8u) == 0xF0u;
  auto       ack     = false;
  for(std::size_t index = 0; index < targets_.size() and not header; ++index)
  {
    if(not targets_[index]->matches(address, at))
    {
      continue;
    }
    state_.in_transfer[index] = 1;
    auto       acked          = false;
    auto const reply = targets_[index]->start_deferred(address, read, at, ticket_of(index));
    take(index, reply, acked);
    state_.acked[index] = acked ? 1 : 0;
    ack                 = ack or acked;
  }
  if(ack)
  {
    update(regs_, address_sent_, 1);
    update(regs_, transmitter_, read ? 0 : 1);
    return;
  }
  update(regs_, ack_failure_, 1);
  static_cast<void>(pending_condition());
}

void i2c_v1::data_ended()
{
  auto const at  = vtime{state_.phase_end_ps + power_on_offset()};
  auto       ack = false;
  for(std::size_t index = 0; index < targets_.size(); ++index)
  {
    if(state_.acked[index] != 0)
    {
      take(index, targets_[index]->write_deferred(state_.shift, at, ticket_of(index)), ack);
    }
  }
  if(not ack)
  {
    update(regs_, ack_failure_, 1);
    static_cast<void>(pending_condition());
    return;
  }
  if(state_.tx_full == 0)
  {
    update(regs_, byte_finished_, 1);
  }
  if(pending_condition())
  {
    return;
  }
  if(state_.tx_full != 0)
  {
    state_.tx_full = 0;
    shift_out(static_cast<std::uint8_t>(regs_.peek(data_.offset)));
  }
}

void i2c_v1::stop_ended()
{
  auto const at = vtime{state_.phase_end_ps + power_on_offset()};
  regs_.poke(control_.offset, regs_.peek(control_.offset) & ~stop_.mask());
  for(auto const& bit : {master_, transmitter_, busy_})
  {
    update(regs_, bit, 0);
  }
  update(regs_, tx_empty_, 0);
  update(regs_, byte_finished_, 0);
  state_.tx_full = 0;
  std::ranges::fill(state_.acked, 0);
  for(std::size_t index = 0; index < targets_.size(); ++index)
  {
    if(std::exchange(state_.in_transfer[index], 0) != 0)
    {
      targets_[index]->stop(at);
    }
  }
  if(get(regs_, enable_) != 0 and get(regs_, start_) != 0)
  {
    begin_start();
  }
}

void i2c_v1::phase_ended()
{
  struct ending
  {
    bool& flag;
    ~ending()
    {
      flag = false;
    }
  } const scope{ending_ = true};
  auto const ended   = step();
  state_.phase       = static_cast<std::uint8_t>(wire::idle);
  state_.released_ps = std::max(state_.released_ps, state_.phase_end_ps);
  switch(ended)
  {
    case wire::start:
      start_ended();
      break;
    case wire::address:
      address_ended();
      break;
    case wire::data:
      data_ended();
      break;
    case wire::stop:
      stop_ended();
      break;
    case wire::idle:
      break;
  }
}

void i2c_v1::flags()
{
  auto const sr1    = regs_.peek(status1_.offset);
  auto const buffer = get(regs_, buffer_ie_) != 0 and (tx_empty_.of(sr1) | rx_full_.of(sr1)) != 0;
  auto const event  = get(regs_, event_ie_) != 0 and ((sr1 & status1_events_) != 0 or buffer);
  auto const error  = get(regs_, error_ie_) != 0 and (sr1 & status1_errors_) != 0;
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
}

void i2c_v1::rethrow_refused()
{
  if(deferred_.empty())
  {
    return;
  }
  auto devices = std::string{};
  for(auto const& one : std::exchange(deferred_, {}))
  {
    devices += devices.empty() ? one : " and " + one;
  }
  throw config_error(
      ctx_.part.part, 0, std::format("devices.{}", desc_.name),
      std::format(
          "an answer at the phase's end from {}, the STM32L1 I2C completes no deferred phase",
          devices));
}

void i2c_v1::refuse_completion() const
{
  throw config_error(
      ctx_.part.part, 0, std::format("devices.{}", desc_.name),
      "an answer at the phase's end, the STM32L1 I2C completes no deferred phase");
}

void i2c_v1::complete(std::uint64_t, i2c_reply, vtime, std::uint32_t)
{
  refuse_completion();
}

void i2c_v1::complete_read(std::uint64_t, i2c_read_reply, vtime, std::uint32_t)
{
  refuse_completion();
}

void i2c_v1::reset(register_file&, reset_kind kind)
{
  restore_reset_values(kind);
}

void i2c_v1::on_bus_reset()
{
  restore_reset_values(std::nullopt);
}

void i2c_v1::restore_reset_values(std::optional<reset_kind> kind)
{
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
  auto const generation = state_.generation + 1;
  state_                = {};
  state_.generation     = generation;
  state_.acked.resize(targets_.size());
  state_.in_transfer.resize(targets_.size());
  deferred_.clear();
  timing_checked_ = false;
  if(kind)
  {
    for(auto const& target : targets_)
    {
      target->reset(*kind);
    }
  }
  flags();
}

void i2c_v1::power_cut()
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

auto i2c_v1::on_read(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    access_ctx     access) -> std::expected<std::uint32_t, bus_error>
{
  auto const value = dispatch_model::on_read(regs, offset, size, access);
  auto const word  = offset & ~3u;
  if(access.by == initiator::debugger)
  {
    return value;
  }
  if(word == status1_.offset)
  {
    state_.status_read = 1;
  }
  else if(word == status2_.offset)
  {
    if(get(regs_, address_sent_) != 0 and std::exchange(state_.status_read, 0) != 0)
    {
      address_cleared();
    }
  }
  else if(word == data_.offset)
  {
    update(regs_, byte_finished_, 0);
  }
  flags();
  return value;
}

auto i2c_v1::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     access) -> std::expected<void, bus_error>
{
  if((offset & ~3u) == data_.offset)
  {
    if(access.by == initiator::debugger)
    {
      return {};
    }
    auto const merged = merge_write(regs_.peek(data_.offset), offset, size, value);
    regs_.poke(data_.offset, merged & 0xFFu);
    if(get(regs_, enable_) != 0)
    {
      data_written();
    }
    flags();
    return {};
  }
  auto const result = dispatch_model::on_write(regs, offset, size, old, value, access);
  flags();
  return result;
}

void i2c_v1::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind != phase_done or payload != state_.generation or step() == wire::idle)
  {
    return;
  }
  phase_ended();
  flags();
  rethrow_refused();
}

void i2c_v1::save(std::vector<std::byte>& out) const
{
  auto copy = state_;
  copy.targets.clear();
  for(auto const& target : targets_)
  {
    target->save(copy.targets.emplace_back());
  }
  reflex::serde::bson::serializer{out}.dump(copy);
}

void i2c_v1::load(std::span<std::byte const> in)
{
  auto loaded = reflex::serde::bson::deserializer{in}.template load<i2c_v1_state>();
  if(loaded.targets.size() != targets_.size())
  {
    throw config_error(
        ctx_.part.part, 0, std::format("devices.{}", desc_.name),
        std::format(
            "a snapshot with {} targets, the machine has {}", loaded.targets.size(),
            targets_.size()));
  }
  state_ = std::move(loaded);
  for(std::size_t index = 0; index < targets_.size(); ++index)
  {
    targets_[index]->load(state_.targets[index]);
  }
  state_.targets.clear();
  state_.acked.resize(targets_.size());
  state_.in_transfer.resize(targets_.size());
}
}

auto i2c_v1_factory(board const& b, i2c_v1_variant variant) -> model_factory
{
  return [targets = b.i2c, controllers = b.i2c_controllers,
          variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    auto const found = targets.find(desc.name);
    return std::make_shared<i2c_v1>(
        ctx, desc,
        found != targets.end() ? found->second : std::vector<std::shared_ptr<i2c_target>>{},
        controllers.contains(desc.name), variant);
  };
}
}
