#include "dma_signal.hpp"
#include "factories.hpp"
#include "model_base.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/scheduler.hpp>
#include <popcorn/periph/i2c.hpp>
#include <popcorn/periph/reset_listener.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/i2c.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
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
constexpr std::uint32_t port_due       = 3;
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

constexpr std::uint8_t entry_phase   = 0;
constexpr std::uint8_t entry_ending  = 1;
constexpr std::uint8_t entry_stop    = 2;
constexpr std::uint8_t entry_dropped = 3;

struct i2c_port_entry
{
  std::uint64_t                due_ps{};
  std::uint64_t                unit{};
  std::uint64_t                phase_ps{};
  std::uint64_t                posted_ps{};
  std::optional<std::uint64_t> wire_end_ps{};
  std::uint32_t                lane{};
  std::uint8_t                 kind{};
  std::uint8_t                 phase{};
  std::uint8_t                 data{};

  friend auto operator==(i2c_port_entry const&, i2c_port_entry const&) -> bool = default;
};

struct i2c_port
{
  std::vector<i2c_port_entry>  entries;
  std::uint64_t                last_unit{};
  std::uint32_t                generation{};
  std::optional<std::uint64_t> reset_ps;

  friend auto operator==(i2c_port const&, i2c_port const&) -> bool = default;
};

auto phase_start(std::span<i2c_port_entry const> entries, std::uint64_t posted, std::uint64_t unit)
    -> std::uint64_t
{
  auto const earlier = [unit](i2c_port_entry const& entry) { return entry.unit < unit; };
  auto       ahead   = entries | std::views::filter(earlier);
  auto const below   = std::ranges::fold_left(
      ahead | std::views::filter([](auto const& entry) {
        return entry.kind == entry_stop;
      }) | std::views::transform([](auto const& entry) { return entry.unit; }),
      std::uint64_t{}, [](std::uint64_t one, std::uint64_t other) { return std::max(one, other); });
  auto start = posted;
  for(auto const& entry : ahead)
  {
    if(entry.kind == entry_stop)
    {
      start = std::max(start, entry.posted_ps);
    }
    else if(entry.unit < below)
    {
      continue;
    }
    else if(entry.kind == entry_phase)
    {
      start = std::max(start, entry.due_ps + entry.phase_ps);
    }
    else if(entry.kind == entry_ending)
    {
      start = std::max(start, entry.due_ps);
    }
    else if(entry.wire_end_ps)
    {
      start = std::max(start, *entry.wire_end_ps);
    }
  }
  return start;
}

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
  std::uint8_t                        addressed{};
  std::uint8_t                        involved{};
  std::uint8_t                        wire_busy{};
  i2c_port                            port;
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
 *        the model.
 *
 *        Target mode, address phase, RM0456 65.4.8 and RM0481 53.4.8: the controller of
 *        board::i2c_controllers queues i2c_controller_port entries on the lane it names, each with
 *        a unit above the last one. The port runs the entries of one instruction count in unit
 *        order. A phase starts at the latest of its time, the end of the phases queued before it
 *        and the time of the STOPs queued before it. A START with PE set sets BUSY until the STOP.
 *        At the end of an address phase the address matches OAR1 with OA1EN and OA1MODE clear,
 *        OAR2 with OA2EN and its OA2MSK low bits ignored, no reserved address 0b0000xxx or
 *        0b1111xxx once OA2MSK is not 0, and the address byte 0b00000000 with GCEN, each read at
 *        that time, RM0456 65.9.3, 65.9.4 and 65.9.1, RM0481 53.9.3, 53.9.4 and 53.9.1. A match
 *        while PE is set, set already at the START, and no transfer of the I2C runs as controller
 *        is acknowledged: ADDCODE takes the address, DIR the direction and ADDR sets unless
 *        ADDRACLR is set, RM0456 65.9.7, RM0481 53.9.7.
 *        Any other address phase, PE clear included, answers NACK, as does the I2C held in
 *        reset. The answer reaches the controller at the end of the phase. A write or a read
 *        phase answers NACK or 0xFF. After an acknowledged address it logs once that target data
 *        phases are not modelled, and raises model_error under strict_peripherals. A STOP sets
 *        STOPF when an address of the transfer was acknowledged, unless STOPFACLR is set. It ends
 *        the phases of lower units still queued or running, reported as dropped at its time and
 *        not received, and BERR sets for a data byte of an acknowledged address it ends, RM0456
 *        65.4.17, RM0481 53.4.17. A START the firmware requests during the controller's
 *        transfer waits for its STOP. While OA1EN is set, OAR1 keeps OA1 and OA1MODE, and while
 *        OA2EN is set OAR2 keeps OA2 and OA2MSK, RM0456 65.9.3 and 65.9.4, RM0481 53.9.3 and
 *        53.9.4: one warning per instance, model_error under strict_peripherals. An RCC reset
 *        drops the phases started at or before it, each reported at the later of its end and the
 *        reset, and keeps the later phases and STOPs, which run in the reset model. A phase
 *        queued after it with a start at or before it is dropped the same way, and a STOP or a
 *        drop queued after it is due no earlier than it. A machine reset of any kind, the model
 *        a reset_listener, drops every queued phase at its end rebased by the reset's local
 *        time and removes every queued STOP. A phase a reset drops holds the wire to its end, in
 *        the new base after a machine reset, and a later phase starts no earlier. Dropped units
 *        survive every reset and are reported once, through i2c_controller_sink::dropped(). The
 *        port entries, the last unit, the generation and the time of the last RCC reset are
 *        saved with the model. Not modelled, logged once: 10-bit addressing, target byte
 *        control, NOSTRETCH, SMBus and PEC, the autonomous trigger, target data phases.
 */
class i2c final : public dispatch_model<i2c, i2c_state>,
                  public i2c_bus,
                  public i2c_controller_port,
                  public reset_listener,
                  public bus_reset_listener
{
public:
  i2c(model_context&                           ctx,
      peripheral_desc const&                   desc,
      std::vector<std::shared_ptr<i2c_target>> targets,
      std::shared_ptr<i2c_controller_sink>     controller,
      i2c_variant const&                       variant);

  void before_reset(reset_kind kind) override;

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
  void start(
      std::uint8_t  address,
      bool          read,
      std::uint64_t phase_ps,
      vtime         at,
      std::uint32_t lane,
      std::uint64_t unit) override;
  void write(
      std::uint8_t  byte,
      std::uint64_t phase_ps,
      vtime         at,
      std::uint32_t lane,
      std::uint64_t unit) override;
  void read(bool ack, std::uint64_t phase_ps, vtime at, std::uint32_t lane, std::uint64_t unit)
      override;
  void stop(vtime at, std::uint32_t lane, std::uint64_t unit) override;
  void drop(i2c_phase phase, vtime at, std::uint32_t lane, std::uint64_t unit) override;

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
  [[nodiscard]] auto bus_taken() const -> bool;
  void               stop_flag();

  void port_check(std::uint32_t lane, std::uint64_t unit) const;
  void queue_phase(
      i2c_phase     phase,
      std::uint8_t  data,
      std::uint64_t phase_ps,
      vtime         at,
      std::uint32_t lane,
      std::uint64_t unit);
  [[nodiscard]] auto after_model_reset(std::uint64_t due_ps) const -> std::uint64_t;
  [[nodiscard]] auto count_of(std::uint64_t due_ps) const -> icount;
  void               queue_port(i2c_port_entry entry);
  void               schedule_port(i2c_port_entry const& entry);
  void               rearm_port(i2c_port port, std::optional<reset_kind> kind);
  void               run_port(std::uint32_t lane);
  void               phase_begun(i2c_port_entry const& entry);
  [[nodiscard]] auto phase_ended(i2c_port_entry const& entry) -> i2c_answer;
  void               port_stopped(i2c_port_entry const& entry);
  [[nodiscard]] auto matches(std::uint8_t address, bool read) const -> bool;
  void               warn_target_unsupported();
  void               unmodelled(std::string_view reg_name, bool& logged, std::string message);
  void               rethrow_strict();

  register_file&                               regs_;
  std::vector<std::shared_ptr<i2c_target>>     targets_;
  std::shared_ptr<i2c_controller_sink>         controller_;
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
  field_ref                                    address_matched_;
  field_ref                                    target_direction_;
  field_ref                                    address_code_;
  field_ref                                    own1_;
  field_ref                                    own1_mode_;
  field_ref                                    own1_enable_;
  field_ref                                    own2_;
  field_ref                                    own2_mask_;
  field_ref                                    own2_enable_;
  field_ref                                    general_call_;
  field_ref                                    address_auto_clear_;
  field_ref                                    stop_auto_clear_;
  reg_ref                                      own1_reg_;
  reg_ref                                      own2_reg_;
  std::vector<field_ref>                       target_unsupported_;
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
  bool                                         target_logged_{};
  bool                                         data_logged_{};
  bool                                         protect_logged_{};
  std::optional<std::uint64_t>                 reset_at_;
  std::optional<std::string>                   strict_refused_;
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

auto i2c_table(i2c_variant const& variant) -> std::span<write_entry<i2c> const>
{
  auto const all = std::span{i2c_writes};
  return variant.autocr ? all : all.first(all.size() - 1);
}

i2c::i2c(
    model_context&                           ctx,
    peripheral_desc const&                   desc,
    std::vector<std::shared_ptr<i2c_target>> targets,
    std::shared_ptr<i2c_controller_sink>     controller,
    i2c_variant const&                       variant)
    : dispatch_model{ctx, desc, "I2C", i2c_table(variant)}, regs_{ctx.regs},
      targets_{std::move(targets)}, controller_{std::move(controller)}, command_{reg("CR2")},
      status_{reg("ISR")}, clear_{reg("ICR")}, transmit_{reg("TXDR")}, receive_{reg("RXDR")},
      enable_{field("CR1", "PE")}, start_{field("CR2", "START")}, stop_{field("CR2", "STOP")},
      count_{field("CR2", "NBYTES")}, reload_{field("CR2", "RELOAD")},
      autoend_{field("CR2", "AUTOEND")}, direction_{field("CR2", "RD_WRN")},
      address_{field("CR2", "SADD")}, ten_bit_{field("CR2", "ADD10")},
      scl_low_{field("TIMINGR", "SCLL")}, scl_high_{field("TIMINGR", "SCLH")},
      prescaler_{field("TIMINGR", "PRESC")}, tx_empty_{field("ISR", "TXE")},
      tx_interrupt_{field("ISR", "TXIS")}, rx_full_{field("ISR", "RXNE")},
      transfer_complete_{field("ISR", "TC")}, reload_complete_{field("ISR", "TCR")},
      stop_detected_{field("ISR", "STOPF")}, nack_received_{field("ISR", "NACKF")},
      busy_{field("ISR", "BUSY")}, bus_error_{field("ISR", "BERR")},
      arbitration_lost_{field("ISR", "ARLO")}, overrun_{field("ISR", "OVR")},
      address_matched_{field("ISR", "ADDR")}, target_direction_{field("ISR", "DIR")},
      address_code_{field("ISR", "ADDCODE")}, own1_{field("OAR1", "OA1")},
      own1_mode_{field("OAR1", "OA1MODE")}, own1_enable_{field("OAR1", "OA1EN")},
      own2_{field("OAR2", "OA2")}, own2_mask_{field("OAR2", "OA2MSK")},
      own2_enable_{field("OAR2", "OA2EN")}, general_call_{field("CR1", "GCEN")},
      address_auto_clear_{field("CR1", "ADDRACLR")}, stop_auto_clear_{field("CR1", "STOPFACLR")},
      own1_reg_{reg("OAR1")}, own2_reg_{reg("OAR2")}, error_enable_{field("CR1", "ERRIE")},
      kernel_{ctx.clock.domain(lower(desc.name))}, event_line_{irq(desc.name + "_EV_IRQn")},
      error_line_{irq(desc.name + "_ER_IRQn")}, tx_dma_enable_{field("CR1", "TXDMAEN")},
      rx_dma_enable_{field("CR1", "RXDMAEN")},
      complete_dma_enable_{
          variant.autocr ? std::optional{field("I2C_AUTOCR", "TCDMAEN")} : std::nullopt},
      reload_dma_enable_{
          variant.autocr ? std::optional{field("I2C_AUTOCR", "TCRDMAEN")} : std::nullopt},
      tx_dma_{ctx.peers, lower(desc.name) + "_tx_dma"},
      rx_dma_{ctx.peers, lower(desc.name) + "_rx_dma"},
      event_dma_{ctx.peers, lower(desc.name) + "_evc_dma"}
{
  if(controller_ and not targets_.empty())
  {
    throw config_error(
        ctx.part.part, 0, std::format("board.i2c.{}", desc.name),
        "an I2C controller alone on the instance, no target next to it");
  }
  expect_register("I2C_AUTOCR", variant.autocr);
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
  for(auto const name : {"SBC", "NOSTRETCH", "SMBHEN", "SMBDEN", "ALERTEN", "PECEN"})
  {
    unsupported_.push_back(field("CR1", name));
    target_unsupported_.push_back(field("CR1", name));
  }
  if(variant.autocr)
  {
    unsupported_.push_back(field("I2C_AUTOCR", "TRIGEN"));
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
  stop_flag();
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
  if(get(regs_, start_) != 0 and not bus_taken())
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
  if(get(regs_, start_) != 0 and not bus_taken())
  {
    begin_start(state_.end_ps);
  }
}

auto i2c::held() const -> bool
{
  return state_.active.id != 0;
}

auto i2c::bus_taken() const -> bool
{
  return held() or state_.wire_busy != 0;
}

void i2c::stop_flag()
{
  if(get(regs_, stop_auto_clear_) == 0)
  {
    update(regs_, stop_detected_, 1);
  }
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
  if(state_.wire_busy != 0)
  {
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
  state_.target    = no_target;
  state_.addressed = 0;
  state_.involved  = 0;
  state_.wire_busy = 0;
  regs_.poke(command_.offset, regs_.peek(command_.offset) & ~command_set_only_);
  regs_.poke(status_.offset, (regs_.peek(status_.offset) & ~status_reset_mask_) | tx_empty_.mask());
  if(state_.pending_waits != 0)
  {
    take_bus();
  }
}

void i2c::before_reset(reset_kind)
{
  reset_at_ = ctx_.clock.time().ps;
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
  auto       port       = std::move(state_.port);
  state_                = {};
  state_.generation     = generation;
  state_.holds          = holds;
  rearm_port(std::move(port), kind);
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
        "10-bit addressing, target byte control, NOSTRETCH, SMBus, PEC or the autonomous trigger "
        "is configured: not modelled, the I2C runs as a plain 7-bit controller and target");
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
      if(started and not bus_taken())
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

void i2c::port_check(std::uint32_t lane, std::uint64_t unit) const
{
  if(not controller_)
  {
    throw config_error(
        ctx_.part.part, 0, std::format("devices.{}", desc_.name),
        "an I2C controller attached to the instance");
  }
  if(state_.port.entries.size() >= max_entries)
  {
    throw config_error(
        ctx_.part.part, 0, std::format("devices.{}", desc_.name),
        std::format("at most {} I2C port entries queued", max_entries));
  }
  if(lane == 0)
  {
    throw std::invalid_argument("an I2C port entry goes on a lane of 1 and up");
  }
  if(unit <= state_.port.last_unit)
  {
    throw std::invalid_argument(
        std::format(
            "an I2C port unit {} is not above the last unit {}", unit, state_.port.last_unit));
  }
}

auto i2c::after_model_reset(std::uint64_t due_ps) const -> std::uint64_t
{
  return std::max(due_ps, state_.port.reset_ps.value_or(0));
}

auto i2c::count_of(std::uint64_t due_ps) const -> icount
{
  auto const& time  = ctx_.clock;
  auto const  count = time.to_icount(vtime{std::max(due_ps, time.epoch_time().ps)});
  return count.value < time.now().value ? count : time.now();
}

void i2c::schedule_port(i2c_port_entry const& entry)
{
  ctx_.events.schedule_at(
      vtime{entry.due_ps}, port_due, std::uint64_t{state_.port.generation} << 32 | entry.lane,
      entry.lane, ctx_.clock);
}

void i2c::queue_port(i2c_port_entry entry)
{
  state_.port.last_unit = entry.unit;
  state_.port.entries.push_back(entry);
  schedule_port(entry);
}

void i2c::queue_phase(
    i2c_phase     phase,
    std::uint8_t  data,
    std::uint64_t phase_ps,
    vtime         at,
    std::uint32_t lane,
    std::uint64_t unit)
{
  port_check(lane, unit);
  if(phase_ps == 0)
  {
    throw std::invalid_argument("an I2C port phase lasts at least 1 ps");
  }
  auto const start = phase_start(state_.port.entries, at.ps, unit);
  if(phase_ps > std::numeric_limits<std::uint64_t>::max() - start)
  {
    throw std::invalid_argument("an I2C port phase ends within the 64-bit picosecond range");
  }
  auto const end        = start + phase_ps;
  auto const last_reset = state_.port.reset_ps;
  auto const dropped    = last_reset and start <= *last_reset;
  queue_port(
      {.due_ps      = dropped ? std::max(*last_reset, end) : start,
       .unit        = unit,
       .phase_ps    = phase_ps,
       .posted_ps   = at.ps,
       .wire_end_ps = dropped ? std::optional{end} : std::nullopt,
       .lane        = lane,
       .kind        = dropped ? entry_dropped : entry_phase,
       .phase       = static_cast<std::uint8_t>(phase),
       .data        = data});
}

void i2c::start(
    std::uint8_t  address,
    bool          read,
    std::uint64_t phase_ps,
    vtime         at,
    std::uint32_t lane,
    std::uint64_t unit)
{
  if(address > 0x7Fu)
  {
    throw std::invalid_argument("an I2C port address has 7 bits");
  }
  queue_phase(
      i2c_phase::address, static_cast<std::uint8_t>(address << 1 | (read ? 1u : 0u)), phase_ps, at,
      lane, unit);
}

void i2c::write(
    std::uint8_t  byte,
    std::uint64_t phase_ps,
    vtime         at,
    std::uint32_t lane,
    std::uint64_t unit)
{
  queue_phase(i2c_phase::write, byte, phase_ps, at, lane, unit);
}

void i2c::read(bool ack, std::uint64_t phase_ps, vtime at, std::uint32_t lane, std::uint64_t unit)
{
  queue_phase(i2c_phase::read, ack ? 1 : 0, phase_ps, at, lane, unit);
}

void i2c::stop(vtime at, std::uint32_t lane, std::uint64_t unit)
{
  port_check(lane, unit);
  queue_port(
      {.due_ps    = after_model_reset(at.ps),
       .unit      = unit,
       .posted_ps = at.ps,
       .lane      = lane,
       .kind      = entry_stop});
}

void i2c::drop(i2c_phase phase, vtime at, std::uint32_t lane, std::uint64_t unit)
{
  if(phase != i2c_phase::address and phase != i2c_phase::write and phase != i2c_phase::read)
  {
    throw std::invalid_argument("an I2C port drop names an address, write or read phase");
  }
  port_check(lane, unit);
  queue_port(
      {.due_ps    = after_model_reset(at.ps),
       .unit      = unit,
       .posted_ps = at.ps,
       .lane      = lane,
       .kind      = entry_dropped,
       .phase     = static_cast<std::uint8_t>(phase)});
}

void i2c::run_port(std::uint32_t lane)
{
  auto const now = ctx_.clock.time().ps;
  for(;;)
  {
    auto&                      entries = state_.port.entries;
    std::optional<std::size_t> pick;
    auto                       pick_count = icount{};
    for(std::size_t index = 0; index < entries.size(); ++index)
    {
      auto const& entry = entries[index];
      if(entry.lane > lane or entry.due_ps > now)
      {
        continue;
      }
      auto const count = count_of(entry.due_ps);
      if(not pick
         or count.value < pick_count.value
         or (count.value == pick_count.value and entry.unit < entries[*pick].unit))
      {
        pick       = index;
        pick_count = count;
      }
    }
    if(not pick)
    {
      return;
    }
    auto const entry = entries[*pick];
    auto const phase = static_cast<i2c_phase>(entry.phase);
    switch(entry.kind)
    {
      case entry_phase:
        entries[*pick].kind   = entry_ending;
        entries[*pick].due_ps = entry.due_ps + entry.phase_ps;
        schedule_port(entries[*pick]);
        phase_begun(entry);
        flags();
        break;
      case entry_ending:
      {
        entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(*pick));
        auto const answer = phase_ended(entry);
        flags();
        controller_->answered(entry.unit, phase, answer, vtime{entry.due_ps});
        break;
      }
      case entry_stop:
        entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(*pick));
        port_stopped(entry);
        break;
      default:
        entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(*pick));
        flags();
        controller_->dropped(entry.unit, phase, vtime{entry.due_ps});
        break;
    }
  }
}

void i2c::phase_begun(i2c_port_entry const& entry)
{
  if(static_cast<i2c_phase>(entry.phase) != i2c_phase::address
     or not enabled()
     or step() != phase::idle)
  {
    return;
  }
  state_.wire_busy = 1;
  update(regs_, busy_, 1);
}

auto i2c::phase_ended(i2c_port_entry const& entry) -> i2c_answer
{
  switch(static_cast<i2c_phase>(entry.phase))
  {
    case i2c_phase::address:
    {
      auto const address = static_cast<std::uint8_t>(entry.data >> 1);
      auto const read    = entry.data & 1u;
      warn_target_unsupported();
      if(not enabled()
         or step() != phase::idle
         or state_.wire_busy == 0
         or not matches(address, read != 0))
      {
        state_.addressed = 0;
        return {.ack = false};
      }
      update(regs_, address_code_, address);
      update(regs_, target_direction_, read);
      if(get(regs_, address_auto_clear_) == 0)
      {
        update(regs_, address_matched_, 1);
      }
      state_.addressed = 1;
      state_.involved  = 1;
      return {.ack = true};
    }
    case i2c_phase::write:
      if(state_.addressed != 0)
      {
        unmodelled(
            "RXDR", data_logged_,
            "a target write or read phase after an acknowledged address: not modelled, the I2C "
            "answers NACK or 0xFF");
      }
      return {.ack = false};
    case i2c_phase::read:
      if(state_.addressed != 0)
      {
        unmodelled(
            "RXDR", data_logged_,
            "a target write or read phase after an acknowledged address: not modelled, the I2C "
            "answers NACK or 0xFF");
      }
      return {.ack = entry.data != 0};
  }
  return {};
}

auto i2c::matches(std::uint8_t address, bool read) const -> bool
{
  if(get(regs_, own1_enable_) != 0
     and get(regs_, own1_mode_) == 0
     and ((get(regs_, own1_) >> 1) & 0x7Fu) == std::uint32_t{address})
  {
    return true;
  }
  if(get(regs_, own2_enable_) != 0)
  {
    auto const mask     = get(regs_, own2_mask_);
    auto const received = std::uint32_t{address};
    auto const reserved = (received & 0x78u) == 0 or (received & 0x78u) == 0x78u;
    if(mask == 0 ? get(regs_, own2_) == received
                 : not reserved and (received >> mask) == (get(regs_, own2_) >> mask))
    {
      return true;
    }
  }
  return address == 0 and not read and get(regs_, general_call_) != 0;
}

void i2c::port_stopped(i2c_port_entry const& entry)
{
  auto const cut = [&](i2c_port_entry const& queued) {
    return (queued.kind == entry_phase or queued.kind == entry_ending) and queued.unit < entry.unit;
  };
  std::vector<std::pair<std::uint64_t, i2c_phase>> reports;
  auto                                             misplaced = false;
  for(auto const& queued : state_.port.entries)
  {
    if(not cut(queued))
    {
      continue;
    }
    auto const phase = static_cast<i2c_phase>(queued.phase);
    misplaced =
        misplaced
        or (queued.kind == entry_ending and phase != i2c_phase::address and state_.addressed != 0);
    reports.emplace_back(queued.unit, phase);
  }
  std::erase_if(state_.port.entries, cut);
  if(enabled())
  {
    if(misplaced)
    {
      update(regs_, bus_error_, 1);
    }
    if(state_.involved != 0)
    {
      stop_flag();
    }
    if(state_.wire_busy != 0 and not held())
    {
      update(regs_, busy_, 0);
    }
  }
  auto const waited = state_.wire_busy != 0;
  state_.addressed  = 0;
  state_.involved   = 0;
  state_.wire_busy  = 0;
  if(waited and enabled() and step() == phase::idle and get(regs_, start_) != 0 and not held())
  {
    begin_start(ctx_.clock.time().ps);
  }
  std::ranges::sort(reports);
  flags();
  for(auto const& [unit, phase] : reports)
  {
    controller_->dropped(unit, phase, vtime{entry.due_ps});
  }
}

void i2c::rearm_port(i2c_port port, std::optional<reset_kind> kind)
{
  auto const now   = ctx_.clock.time().ps;
  auto const shift = kind ? std::exchange(reset_at_, std::nullopt).value_or(0) : 0;
  auto const moved = [&](std::uint64_t due) {
    return std::max(now, due > shift ? due - shift : 0);
  };
  if(kind)
  {
    std::erase_if(port.entries, [](auto const& entry) { return entry.kind == entry_stop; });
  }
  for(auto& entry : port.entries)
  {
    if(entry.kind == entry_phase and (kind or entry.due_ps <= now))
    {
      entry.due_ps += entry.phase_ps;
      entry.kind = entry_ending;
    }
    if(entry.kind == entry_ending)
    {
      entry.wire_end_ps = kind ? moved(entry.due_ps) : entry.due_ps;
      entry.due_ps      = moved(entry.due_ps);
      entry.kind        = entry_dropped;
    }
    else if(entry.kind != entry_phase)
    {
      entry.due_ps = moved(entry.due_ps);
      if(entry.wire_end_ps and kind)
      {
        entry.wire_end_ps = moved(*entry.wire_end_ps);
      }
      if(kind)
      {
        entry.posted_ps = entry.due_ps;
      }
    }
  }
  port.reset_ps = kind ? std::nullopt : std::optional{now};
  ++port.generation;
  state_.port = std::move(port);
  for(auto const& entry : state_.port.entries)
  {
    schedule_port(entry);
  }
}

void i2c::warn_target_unsupported()
{
  auto const ten_bit = get(regs_, own1_enable_) != 0 and get(regs_, own1_mode_) != 0;
  if(ten_bit or std::ranges::any_of(target_unsupported_, [&](field_ref bits) {
       return get(regs_, bits) != 0;
     }))
  {
    unmodelled(
        ten_bit ? "OAR1" : "CR1", target_logged_,
        "a 10-bit own address, target byte control, NOSTRETCH, SMBus or PEC in target mode: not "
        "modelled, the I2C matches 7-bit addresses only");
  }
}

void i2c::unmodelled(std::string_view reg_name, bool& logged, std::string message)
{
  if(not std::exchange(logged, true))
  {
    warn(reg_name, std::move(message));
  }
  if(ctx_.strict_peripherals and not strict_refused_)
  {
    strict_refused_ = std::string{reg_name};
  }
}

void i2c::rethrow_strict()
{
  if(auto const name = std::exchange(strict_refused_, std::nullopt))
  {
    throw model_error(desc_.name, *name, reg(*name).offset);
  }
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
  auto const word   = offset & ~3u;
  auto const kept   = word == own1_reg_.offset and get(regs_, own1_enable_) != 0
                        ? own1_.mask() | own1_mode_.mask()
                    : word == own2_reg_.offset and get(regs_, own2_enable_) != 0
                        ? own2_.mask() | own2_mask_.mask()
                        : 0u;
  auto const before = regs_.peek(word);
  auto const result = dispatch_model::on_write(regs, offset, size, old, value, access);
  auto const now    = regs_.peek(word);
  if(((now ^ before) & kept) != 0)
  {
    regs_.poke(word, (now & ~kept) | (before & kept));
    if(access.by != initiator::debugger)
    {
      auto const name = word == own1_reg_.offset ? "OAR1" : "OAR2";
      if(not std::exchange(protect_logged_, true))
      {
        warn(
            name, std::format(
                      "{} {} written while its enable bit is set, the old own address and mode "
                      "are kept, RM0456 65.9.3 and 65.9.4, RM0481 53.9.3 and 53.9.4",
                      desc_.name, name));
      }
      if(ctx_.strict_peripherals)
      {
        throw model_error(desc_.name, name, word);
      }
    }
  }
  flags();
  return result;
}

void i2c::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind == port_due)
  {
    if(payload >> 32 == state_.port.generation)
    {
      run_port(static_cast<std::uint32_t>(payload));
      rethrow_strict();
    }
    return;
  }
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
  auto loaded = reflex::serde::bson::deserializer{in}.template load<i2c_state>();
  if(not controller_ and not loaded.port.entries.empty())
  {
    throw config_error(
        ctx_.part.part, 0, std::format("devices.{}", desc_.name),
        "an I2C controller attached to the instance, the snapshot holds port entries");
  }
  state_ = std::move(loaded);
  for(std::size_t index = 0; index < targets_.size() and index < state_.devices.size(); ++index)
  {
    targets_[index]->load(state_.devices[index]);
  }
  state_.devices.clear();
}
}

auto i2c_factory(board const& b, i2c_variant variant) -> model_factory
{
  return [targets = b.i2c, controllers = b.i2c_controllers,
          variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    auto const found      = targets.find(desc.name);
    auto const controller = controllers.find(desc.name);
    return std::make_shared<i2c>(
        ctx, desc,
        found != targets.end() ? found->second : std::vector<std::shared_ptr<i2c_target>>{},
        controller != controllers.end() ? controller->second : nullptr, variant);
  };
}
}
