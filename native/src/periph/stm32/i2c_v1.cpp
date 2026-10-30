#include "factories.hpp"
#include "i2c_port_queue.hpp"
#include "model_base.hpp"
#include "variants.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/scheduler.hpp>
#include <popcorn/periph/i2c.hpp>
#include <popcorn/periph/reset_listener.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <algorithm>
#include <array>
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
#include <tuple>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
namespace
{
constexpr std::uint64_t ps_per_second = 1'000'000'000'000;
constexpr std::uint64_t hz_per_mhz    = 1'000'000;
constexpr std::uint32_t phase_done    = 0;
constexpr std::uint32_t port_due      = 1;
constexpr std::uint32_t byte_periods  = 9;
constexpr std::uint32_t data_periods  = 8;

enum class wire : std::uint8_t
{
  idle,
  start,
  address,
  data,
  stop,
  receive,
  acknowledge,
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
  std::uint8_t                        ack_next{};
  std::uint8_t                        rx_held{};
  std::uint8_t                        ack_pulse{};
  std::uint8_t                        addressed{};
  std::uint8_t                        last_ack{};
  std::uint8_t                        wire_busy{};
  std::uint64_t                       phase_end_ps{};
  std::uint64_t                       released_ps{};
  std::vector<std::uint8_t>           acked;
  std::vector<std::uint8_t>           in_transfer;
  std::vector<std::vector<std::byte>> targets;
  i2c_port                            port;

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
 *        interface", controller transmitter and receiver with 7-bit addresses on the board's
 *        i2c_target targets.
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
 *        Controller receiver, RM0038 26.3.3 and Figure 213: an address with the R/W bit set sets
 *        ADDR with TRA 0, and the ADDR clear starts the first byte. Each byte takes 8 data periods
 *        and 1 acknowledge period and reaches the targets that acknowledged the address through
 *        i2c_target::read_deferred() at the end of the acknowledge period, their bytes combined by
 *        AND as the open-drain bus does. The I2C decides the acknowledge at the end of the eighth
 *        period, before the acknowledge pulse, as RM0038 26.3.3 and Figure 213 note 4 order it:
 *        with POS 0 as CR1.ACK is then, an ACK write inside the eighth period counting, as Figure
 *        213 note 4 states. An ACK write during the acknowledge pulse counts for the next byte,
 *        inferred. With POS 1 as ACK was at the decision of the previous byte or at the end of the
 *        address, which the RM0038 26.3.3 2-byte procedure settles for its EV6 write, inferred for
 *        an ACK write during a byte. After a NACKed byte the targets take no part: a byte clocked
 *        after it reads 0xFF and reaches no target. A byte that ends with RxNE 0 goes to DR, sets
 *        RxNE and the next byte starts. One that ends with RxNE 1 stays in the shift register, sets
 *        BTF and SCL stays low. A DR read clears BTF. With BTF set it moves the shift register to
 *        DR, keeps RxNE 1 and starts the next byte, with BTF clear it clears RxNE. A DR write with
 *        TRA 0 clears RxNE and BTF as a DR read does, and with BTF set the held byte replaces the
 *        written value in DR, inferred. A DR write with TRA 1 clears RxNE, BTF and a byte held from
 *        a reception and goes to the transmitter, inferred. CR1.STOP and START go after the byte
 *        that shifts, at once while SCL is held, and at once after the ADDR clear while ADDR is
 *        set, as for the transmitter. A START or a STOP keeps BTF while a byte waits in the shift
 *        register.
 *
 *        SMBus, PEC, ALERT, ARP and CR2.DMAEN log one model_warning per instance when the firmware
 *        uses them, raise model_error under strict_peripherals at the START that uses them, and the
 *        bus stays idle for them. OAR2, ENGC and NOSTRETCH are stored. ADD10 stays 0: no i2c_target
 *        answers a 10-bit header, so a header address byte takes the NACK path and reaches no
 *        i2c_target::matches().
 *
 *        Interrupts, RM0038 Table 139 and Figure 214: the event line follows SB, ADDR, ADD10, STOPF
 *        and BTF with CR2.ITEVTEN, TxE and RxNE with ITEVTEN and ITBUFEN, the error line BERR,
 *        ARLO, AF, OVR, PECERR, TIMEOUT and SMBALERT with ITERREN. SR1's error flags clear by a
 *        write of 0. PE 0 ends the transfer at once and clears START, STOP, ACK, POS, PEC and
 *        ALERT, every SR1 flag and SR2's MSL and TRA, and BUSY unless the bus is busy. PE 0 during
 *        a controller transfer, which RM0038 26.6.1 forbids, logs one model_warning per instance.
 *        CR1.SWRST 1 resets the I2C as an RCC reset does and keeps SWRST 1. While SWRST is 1 a CR1
 *        write changes nothing but the SWRST clear, which leaves CR1 0. Neither tells the targets.
 *        Debugger accesses to SR1, SR2 and DR have no side effect and a debugger DR write sends
 *        nothing.
 *
 *        Controller port: a controller of board::i2c_controllers, alone on the instance with no
 *        board target, queues i2c_controller_port entries on the lane it names, each with a unit
 *        above the last one. The port runs the entries of one instruction count in unit order. A
 *        phase starts at the latest of its time, the end of the phases queued before it and the
 *        time of the STOPs queued before it, and is answered at its end, or at the release of the
 *        I2C's stretch, as the target receiver below says. A read byte answers the controller's
 *        acknowledge and 0xFF. A STOP drops the phases of lower units still queued or running, each
 *        reported at its time. An RCC reset, CR1.SWRST included, drops the phases started at or
 *        before it, each reported at the later of its end and the reset, and keeps the later phases
 *        and STOPs. A phase queued after it with a start at or before it is dropped the same way,
 *        and a STOP or a drop queued after it is due no earlier than it. A machine reset of any
 *        kind, the model a reset_listener, drops every queued phase at its end rebased by the
 *        reset's local time and removes every queued STOP. A phase a reset drops holds the wire to
 *        its end. Dropped units survive every reset and are reported once, through
 *        i2c_controller_sink::dropped(). The entries, the last unit, the generation and the time of
 *        the last RCC reset are saved with the model, and an instance with no controller refuses a
 *        snapshot holding entries with config_error.
 *
 *        Target receiver, RM0038 26.3.2 and Figure 211: an address phase sets BUSY at its start,
 *        with PE 0 too, as RM0038 26.6.7 BUSY says, and a STOP of the port clears it, the I2C's own
 *        STOP keeping it while the port's transfer runs. At the end of an address phase, while PE
 *        is 1 and no controller transfer of the I2C runs, the address matches OAR1 with ADDMODE 0,
 *        read at that time. Address 0 never matches OAR1, RM0038 26.6.1 ENGC "Address 00h is
 *        NACKed". A match with CR1.ACK 1 sets ADDR with TRA 0 and is acknowledged. SCL stays held,
 *        EV1: the answer reaches the controller once an SR2 read after an SR1 read clears ADDR, and
 *        every later phase and STOP waits for it. A match with CR1.ACK 0 answers NACK at the
 *        address end and sets no ADDR, inferred, the ST HAL clearing ACK to stop listening. Any
 *        other address phase answers NACK. A written byte after an acknowledged write address lands
 *        in DR and sets RxNE, EV2. Popcorn reads CR1.ACK for it at the end of its phase, the
 *        acknowledge period included. A byte that ends with RxNE 1 stays in the shift register with
 *        its acknowledge, sets BTF and holds SCL: a DR read moves it to DR, keeps RxNE 1, clears
 *        BTF and releases SCL. A written byte with no acknowledged address answers NACK. A STOP
 *        clears TRA and sets STOPF when the last acknowledge of the transfer, of the address or of
 *        a byte, was an ACK, RM0038 26.6.6 STOPF "after an acknowledge (if ACK=1)", so no STOPF
 *        after a byte the I2C NACKed. An SR1 read then a CR1 write clears STOPF, EV4, an SR1 read
 *        before STOPF sets arming the clear as RM0038 26.6.7 says for ADDR, inferred. A START the
 *        firmware sets while the bus is busy goes at the STOP, RM0038 26.6.1 START. A repeated
 *        START clears TRA. PE 0, which RM0038 26.6.1 lets act at the end of the communication,
 *        releases a held phase at once with its acknowledge, and later phases answer NACK.
 *        NOSTRETCH at an OAR1 match, a read address, an OAR2 address with ENDUAL, address 0 with
 *        ENGC and a 10-bit header 0b11110xx with ADDMODE 1 each log one model_warning per instance
 *        and raise model_error under strict_peripherals at that address phase, and the last four
 *        answer NACK.
 *
 *        Targets, the STM32L1 I2C on the generic interfaces: each target is attached to the model
 *        at its construction and detached at its destruction, and reset with the machine. A target
 *        that leaves an answer pending is taken as NACK or as a byte of 0xFF, and the phase's event
 *        fails the run with config_error: i2c_target_port::complete() and complete_read() throw
 *        config_error. The model offers no i2c_link_port nor i2c_bus, so an STM32L1 I2C is a link
 *        end of no linked system and takes no host hold. The targets, the phase in flight, the byte
 *        held in the shift register and the set of targets of the transfer are saved with the
 *        model, and a snapshot with another number of targets than the machine's is refused with
 *        config_error. An RCC reset moves the event generation, so the event of a phase in flight
 *        acts on nothing.
 */
class i2c_v1 final : public dispatch_model<i2c_v1, i2c_v1_state>,
                     public i2c_target_port,
                     public i2c_controller_port,
                     public reset_listener,
                     public bus_reset_listener
{
public:
  i2c_v1(
      model_context&                           ctx,
      peripheral_desc const&                   desc,
      std::vector<std::shared_ptr<i2c_target>> targets,
      std::shared_ptr<i2c_controller_sink>     controller,
      i2c_v1_variant const&                    variant);
  i2c_v1(i2c_v1 const&)                    = delete;
  auto operator=(i2c_v1 const&) -> i2c_v1& = delete;
  i2c_v1(i2c_v1&&)                         = delete;
  auto operator=(i2c_v1&&) -> i2c_v1&      = delete;
  ~i2c_v1() override;

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
  void complete(std::uint64_t ticket, i2c_reply reply, vtime at, std::uint32_t lane) override;
  void complete_read(std::uint64_t ticket, i2c_read_reply reply, vtime at, std::uint32_t lane)
      override;
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
  [[nodiscard]] auto last_unit() const -> std::uint64_t override;
  [[nodiscard]] auto queued() const -> std::size_t override;

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
  void               receive_ended();
  void               acknowledge_ended();
  void               address_cleared();
  void               data_written();
  void               received_taken();
  void               shift_out(std::uint8_t byte);
  [[nodiscard]] auto receiving() const -> bool;
  [[nodiscard]] auto pending_condition() -> bool;
  void               answered(i2c_error error, std::uint64_t stretch_ps);
  void               take(std::size_t index, std::optional<i2c_reply> const& reply, bool& ack);
  void take_read(std::size_t index, std::optional<i2c_read_reply> const& reply, std::uint8_t& byte);
  void abort();
  void port_check(std::uint32_t lane, std::uint64_t unit) const;
  void queue_port(i2c_port_entry entry);
  void schedule_port(i2c_port_entry const& entry);
  void queue_phase(
      i2c_phase     phase,
      std::uint8_t  data,
      std::uint64_t phase_ps,
      vtime         at,
      std::uint32_t lane,
      std::uint64_t unit,
      bool          ack = false);
  [[nodiscard]] auto after_model_reset(std::uint64_t due_ps) const -> std::uint64_t;
  [[nodiscard]] auto count_of(std::uint64_t due_ps) const -> icount;
  void               run_port(std::uint32_t lane);
  [[nodiscard]] auto port_answer(i2c_port_entry& entry) -> std::optional<i2c_answer>;
  [[nodiscard]] auto address_answer(i2c_port_entry& entry) -> std::optional<i2c_answer>;
  [[nodiscard]] auto write_answer(i2c_port_entry& entry) -> std::optional<i2c_answer>;
  void               phase_begun(i2c_port_entry const& entry);
  [[nodiscard]] auto held_entry() -> i2c_port_entry*;
  void               release(i2c_port_entry& entry, bool ack);
  void               release_held();
  void               target_ended();
  void               port_stopped(i2c_port_entry const& entry);
  void               rearm_port(i2c_port port, std::optional<reset_kind> kind);
  void               flags();
  void               rethrow_refused();
  [[noreturn]] void  refuse_completion() const;

  register_file&                           regs_;
  std::vector<std::shared_ptr<i2c_target>> targets_;
  std::shared_ptr<i2c_controller_sink>     controller_;
  reg_ref                                  control_;
  reg_ref                                  data_;
  reg_ref                                  status1_;
  reg_ref                                  status2_;
  field_ref                                enable_;
  field_ref                                start_;
  field_ref                                stop_;
  field_ref                                reset_bit_;
  field_ref                                ack_;
  field_ref                                pos_;
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
  field_ref                                stop_detected_;
  field_ref                                own_mode_;
  field_ref                                own_address_;
  field_ref                                no_stretch_;
  field_ref                                general_call_;
  field_ref                                dual_;
  field_ref                                own2_;
  std::uint32_t                            cleared_by_disable_{};
  std::uint32_t                            status1_events_{};
  std::uint32_t                            status1_errors_{};
  std::uint32_t                            unmodelled_control_{};
  core::clock_domain                       kernel_;
  irq_line&                                event_line_;
  irq_line&                                error_line_;
  std::vector<std::string>                 deferred_;
  std::optional<std::uint64_t>             reset_at_;
  std::uint8_t                             timing_logged_{};
  bool                                     timing_checked_{};
  bool                                     error_logged_{};
  bool                                     control_logged_{};
  bool                                     dma_logged_{};
  bool                                     disable_logged_{};
  bool                                     ending_{};
  bool                                     stretch_logged_{};
  bool                                     transmit_logged_{};
  bool                                     general_logged_{};
  bool                                     dual_logged_{};
  bool                                     header_logged_{};
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
    std::shared_ptr<i2c_controller_sink>     controller,
    i2c_v1_variant const&                    variant)
    : dispatch_model{ctx, desc, "I2C", i2c_v1_writes}, regs_{ctx.regs},
      targets_{std::move(targets)}, controller_{std::move(controller)}, control_{reg("CR1")},
      data_{reg("DR")}, status1_{reg("SR1")}, status2_{reg("SR2")}, enable_{field("CR1", "PE")},
      start_{field("CR1", "START")}, stop_{field("CR1", "STOP")}, reset_bit_{field("CR1", "SWRST")},
      ack_{field("CR1", "ACK")}, pos_{field("CR1", "POS")}, frequency_{field("CR2", "FREQ")},
      event_ie_{field("CR2", "ITEVTEN")}, buffer_ie_{field("CR2", "ITBUFEN")},
      error_ie_{field("CR2", "ITERREN")}, dma_enable_{field("CR2", "DMAEN")},
      fast_{field("CCR", "F_S")}, duty_{field("CCR", "DUTY")}, divider_{field("CCR", "CCR")},
      rise_{field("TRISE", "TRISE")}, start_bit_{field("SR1", "SB")},
      address_sent_{field("SR1", "ADDR")}, byte_finished_{field("SR1", "BTF")},
      tx_empty_{field("SR1", "TxE")}, rx_full_{field("SR1", "RxNE")},
      ack_failure_{field("SR1", "AF")}, master_{field("SR2", "MSL")}, busy_{field("SR2", "BUSY")},
      transmitter_{field("SR2", "TRA")}, stop_detected_{field("SR1", "STOPF")},
      own_mode_{field("OAR1", "ADDMODE")}, own_address_{field("OAR1", "ADD_1_7")},
      no_stretch_{field("CR1", "NOSTRETCH")}, general_call_{field("CR1", "ENGC")},
      dual_{field("OAR2", "ENDUAL")}, own2_{field("OAR2", "ADD2")},
      kernel_{ctx.clock.domain(kernel_name(desc, variant))},
      event_line_{irq(desc.name + "_EV_IRQn")}, error_line_{irq(desc.name + "_ER_IRQn")}
{
  if(controller_ and not targets_.empty())
  {
    throw config_error(
        ctx.part.part, 0, std::format("board.i2c.{}", desc.name),
        "an I2C controller alone on the instance, no target next to it");
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
  if(get(regs_, stop_detected_) != 0 and std::exchange(state_.status_read, 0) != 0)
  {
    update(regs_, stop_detected_, 0);
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
    if(get(regs_, start_) != 0 and state_.wire_busy == 0)
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
  release_held();
  state_.addressed = 0;
  state_.last_ack  = 0;
  ++state_.generation;
  state_.phase       = static_cast<std::uint8_t>(wire::idle);
  state_.status_read = 0;
  state_.tx_full     = 0;
  state_.rx_held     = 0;
  state_.released_ps = 0;
  std::ranges::fill(state_.acked, 0);
  std::ranges::fill(state_.in_transfer, 0);
  regs_.poke(control_.offset, regs_.peek(control_.offset) & ~cleared_by_disable_);
  regs_.poke(status1_.offset, 0);
  for(auto const& bit : {master_, transmitter_})
  {
    update(regs_, bit, 0);
  }
  update(regs_, busy_, state_.wire_busy);
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
  if(get(regs_, rx_full_) != 0 or state_.rx_held != 0)
  {
    if(get(regs_, transmitter_) == 0)
    {
      received_taken();
      return;
    }
    update(regs_, rx_full_, 0);
    if(std::exchange(state_.rx_held, 0) != 0)
    {
      update(regs_, byte_finished_, 0);
    }
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

auto i2c_v1::receiving() const -> bool
{
  return controlling()
     and get(regs_, transmitter_) == 0
     and get(regs_, start_bit_) == 0
     and get(regs_, address_sent_) == 0;
}

void i2c_v1::received_taken()
{
  update(regs_, byte_finished_, 0);
  if(std::exchange(state_.rx_held, 0) == 0)
  {
    update(regs_, rx_full_, 0);
    return;
  }
  regs_.poke(data_.offset, state_.shift);
  release_held();
  if(receiving() and step() == wire::idle)
  {
    begin(wire::receive, data_periods);
  }
}

void i2c_v1::address_cleared()
{
  update(regs_, address_sent_, 0);
  if(not controlling())
  {
    release_held();
    return;
  }
  if(get(regs_, transmitter_) != 0)
  {
    update(regs_, tx_empty_, 1);
  }
  if(not pending_condition() and get(regs_, transmitter_) == 0 and state_.rx_held == 0)
  {
    begin(wire::receive, data_periods);
  }
}

void i2c_v1::answered(i2c_error error, std::uint64_t stretch_ps)
{
  if(error != i2c_error::none and not std::exchange(error_logged_, true))
  {
    warn(
        "SR1", std::format(
                   "the {} a target injects: not modelled, the byte completes as its acknowledge "
                   "says",
                   error));
  }
  state_.released_ps = std::max(state_.released_ps, state_.phase_end_ps + stretch_ps);
}

void i2c_v1::take(std::size_t index, std::optional<i2c_reply> const& reply, bool& ack)
{
  if(not reply)
  {
    deferred_.push_back(std::format("the target at {:#04x}", targets_[index]->address()));
    return;
  }
  answered(reply->error, reply->stretch_ps);
  ack = ack or reply->ack;
}

void i2c_v1::take_read(
    std::size_t                          index,
    std::optional<i2c_read_reply> const& reply,
    std::uint8_t&                        byte)
{
  if(not reply)
  {
    deferred_.push_back(std::format("the target at {:#04x}", targets_[index]->address()));
    return;
  }
  answered(reply->error, reply->stretch_ps);
  byte &= reply->byte;
}

void i2c_v1::start_ended()
{
  regs_.poke(control_.offset, regs_.peek(control_.offset) & ~start_.mask());
  update(regs_, start_bit_, 1);
  update(regs_, master_, 1);
  update(regs_, transmitter_, 0);
  update(regs_, tx_empty_, 0);
  update(regs_, byte_finished_, state_.rx_held);
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
  state_.ack_next    = static_cast<std::uint8_t>(get(regs_, ack_));
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
  for(auto const& bit : {master_, transmitter_})
  {
    update(regs_, bit, 0);
  }
  update(regs_, busy_, state_.wire_busy);
  update(regs_, tx_empty_, 0);
  update(regs_, byte_finished_, state_.rx_held);
  state_.tx_full = 0;
  std::ranges::fill(state_.acked, 0);
  for(std::size_t index = 0; index < targets_.size(); ++index)
  {
    if(std::exchange(state_.in_transfer[index], 0) != 0)
    {
      targets_[index]->stop(at);
    }
  }
  if(get(regs_, enable_) != 0 and get(regs_, start_) != 0 and state_.wire_busy == 0)
  {
    begin_start();
  }
}

void i2c_v1::receive_ended()
{
  auto const now   = get(regs_, ack_) != 0;
  state_.ack_pulse = get(regs_, pos_) != 0 ? state_.ack_next : (now ? 1 : 0);
  state_.ack_next  = now ? 1 : 0;
  begin(wire::acknowledge, 1);
}

void i2c_v1::acknowledge_ended()
{
  auto const at   = vtime{state_.phase_end_ps + power_on_offset()};
  auto const ack  = state_.ack_pulse != 0;
  auto       byte = std::uint8_t{0xFF};
  for(std::size_t index = 0; index < targets_.size(); ++index)
  {
    if(state_.acked[index] != 0)
    {
      take_read(index, targets_[index]->read_deferred(at, ack, ticket_of(index)), byte);
    }
  }
  if(not ack)
  {
    std::ranges::fill(state_.acked, 0);
  }
  if(get(regs_, rx_full_) == 0)
  {
    regs_.poke(data_.offset, byte);
    update(regs_, rx_full_, 1);
  }
  else
  {
    state_.shift   = byte;
    state_.rx_held = 1;
    update(regs_, byte_finished_, 1);
  }
  if(pending_condition() or state_.rx_held != 0)
  {
    return;
  }
  begin(wire::receive, data_periods);
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
    case wire::receive:
      receive_ended();
      break;
    case wire::acknowledge:
      acknowledge_ended();
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

void i2c_v1::port_check(std::uint32_t lane, std::uint64_t unit) const
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

auto i2c_v1::after_model_reset(std::uint64_t due_ps) const -> std::uint64_t
{
  return std::max(due_ps, state_.port.reset_ps.value_or(0));
}

auto i2c_v1::count_of(std::uint64_t due_ps) const -> icount
{
  auto const& time  = ctx_.clock;
  auto const  count = time.to_icount(vtime{std::max(due_ps, time.epoch_time().ps)});
  return count.value < time.now().value ? count : time.now();
}

void i2c_v1::schedule_port(i2c_port_entry const& entry)
{
  ctx_.events.schedule_at(
      vtime{entry.due_ps}, port_due, std::uint64_t{state_.port.generation} << 32 | entry.lane,
      entry.lane, ctx_.clock);
}

void i2c_v1::queue_port(i2c_port_entry entry)
{
  state_.port.last_unit = entry.unit;
  state_.port.entries.push_back(entry);
  schedule_port(entry);
}

void i2c_v1::queue_phase(
    i2c_phase     phase,
    std::uint8_t  data,
    std::uint64_t phase_ps,
    vtime         at,
    std::uint32_t lane,
    std::uint64_t unit,
    bool          ack)
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
       .data        = data,
       .ack         = static_cast<std::uint8_t>(ack ? 1 : 0)});
}

void i2c_v1::start(
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

void i2c_v1::write(
    std::uint8_t  byte,
    std::uint64_t phase_ps,
    vtime         at,
    std::uint32_t lane,
    std::uint64_t unit)
{
  queue_phase(i2c_phase::write, byte, phase_ps, at, lane, unit);
}

void i2c_v1::read(
    bool          ack,
    std::uint64_t phase_ps,
    vtime         at,
    std::uint32_t lane,
    std::uint64_t unit)
{
  queue_phase(i2c_phase::read, 0xFF, phase_ps, at, lane, unit, ack);
}

void i2c_v1::stop(vtime at, std::uint32_t lane, std::uint64_t unit)
{
  port_check(lane, unit);
  queue_port(
      {.due_ps    = after_model_reset(at.ps),
       .unit      = unit,
       .posted_ps = at.ps,
       .lane      = lane,
       .kind      = entry_stop});
}

void i2c_v1::drop(i2c_phase phase, vtime at, std::uint32_t lane, std::uint64_t unit)
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

auto i2c_v1::last_unit() const -> std::uint64_t
{
  return state_.port.last_unit;
}

auto i2c_v1::queued() const -> std::size_t
{
  return state_.port.entries.size();
}

void i2c_v1::run_port(std::uint32_t lane)
{
  auto const now = ctx_.clock.time().ps;
  for(;;)
  {
    auto&                      entries = state_.port.entries;
    auto const                 holding = held_unit(entries);
    std::optional<std::size_t> pick;
    auto                       pick_count = icount{};
    for(std::size_t index = 0; index < entries.size(); ++index)
    {
      auto const& entry   = entries[index];
      auto const  waiting = holding
                        and entry.unit > *holding
                        and (entry.kind == entry_phase or entry.kind == entry_stop);
      if(entry.lane > lane or entry.due_ps > now or entry.kind == entry_held or waiting)
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
    auto const erase = [&] { entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(*pick)); };
    switch(entry.kind)
    {
      case entry_phase:
        phase_begun(entry);
        entries[*pick].kind   = entry_ending;
        entries[*pick].due_ps = entry.due_ps + entry.phase_ps;
        schedule_port(entries[*pick]);
        break;
      case entry_ending:
      {
        auto const answer = port_answer(entries[*pick]);
        if(not answer)
        {
          entries[*pick].kind = entry_held;
          flags();
          break;
        }
        erase();
        flags();
        controller_->answered(entry.unit, phase, *answer, vtime{entry.due_ps});
        break;
      }
      case entry_released:
        erase();
        flags();
        controller_->answered(entry.unit, phase, {.ack = entry.ack != 0}, vtime{entry.due_ps});
        break;
      case entry_stop:
        erase();
        port_stopped(entry);
        break;
      default:
        erase();
        flags();
        controller_->dropped(entry.unit, phase, vtime{entry.due_ps});
        break;
    }
  }
}

void i2c_v1::phase_begun(i2c_port_entry const& entry)
{
  if(static_cast<i2c_phase>(entry.phase) != i2c_phase::address)
  {
    return;
  }
  if(state_.addressed != 0)
  {
    update(regs_, transmitter_, 0);
  }
  state_.addressed = 0;
  state_.last_ack  = 0;
  state_.wire_busy = 1;
  update(regs_, busy_, 1);
}

auto i2c_v1::port_answer(i2c_port_entry& entry) -> std::optional<i2c_answer>
{
  switch(static_cast<i2c_phase>(entry.phase))
  {
    case i2c_phase::address:
      return address_answer(entry);
    case i2c_phase::write:
      return write_answer(entry);
    case i2c_phase::read:
      break;
  }
  return i2c_answer{.ack = entry.ack != 0};
}

auto i2c_v1::address_answer(i2c_port_entry& entry) -> std::optional<i2c_answer>
{
  auto const address = static_cast<std::uint32_t>(entry.data >> 1);
  auto const read    = (entry.data & 1u) != 0;
  if(get(regs_, enable_) == 0 or controlling() or step() != wire::idle)
  {
    return i2c_answer{.ack = false};
  }
  if(get(regs_, own_mode_) != 0)
  {
    if((address & 0x7Cu) == 0x78u)
    {
      unmodelled(header_logged_, "OAR1", "10-bit addressing in target mode");
    }
    return i2c_answer{.ack = false};
  }
  if(address == 0)
  {
    if(get(regs_, general_call_) != 0)
    {
      unmodelled(general_logged_, "CR1", "the general call of ENGC");
    }
    return i2c_answer{.ack = false};
  }
  if(get(regs_, dual_) != 0 and get(regs_, own2_) == address)
  {
    unmodelled(dual_logged_, "OAR2", "the dual address of ENDUAL");
    return i2c_answer{.ack = false};
  }
  if(get(regs_, own_address_) != address)
  {
    return i2c_answer{.ack = false};
  }
  check_unmodelled();
  if(get(regs_, no_stretch_) != 0)
  {
    unmodelled(stretch_logged_, "CR1", "NOSTRETCH in target mode");
  }
  if(read)
  {
    unmodelled(transmit_logged_, "SR2", "the target transmitter");
    return i2c_answer{.ack = false};
  }
  if(get(regs_, ack_) == 0)
  {
    return i2c_answer{.ack = false};
  }
  update(regs_, address_sent_, 1);
  update(regs_, transmitter_, 0);
  state_.addressed = 1;
  state_.last_ack  = 1;
  entry.ack        = 1;
  return std::nullopt;
}

auto i2c_v1::write_answer(i2c_port_entry& entry) -> std::optional<i2c_answer>
{
  if(state_.addressed == 0 or get(regs_, enable_) == 0 or get(regs_, transmitter_) != 0)
  {
    return i2c_answer{.ack = false};
  }
  auto const ack  = get(regs_, ack_) != 0;
  state_.last_ack = ack ? 1 : 0;
  if(get(regs_, rx_full_) == 0)
  {
    regs_.poke(data_.offset, entry.data);
    update(regs_, rx_full_, 1);
    return i2c_answer{.ack = ack};
  }
  state_.shift   = entry.data;
  state_.rx_held = 1;
  entry.ack      = ack ? 1 : 0;
  update(regs_, byte_finished_, 1);
  return std::nullopt;
}

auto i2c_v1::held_entry() -> i2c_port_entry*
{
  auto const found = std::ranges::find(state_.port.entries, entry_held, &i2c_port_entry::kind);
  return found != state_.port.entries.end() ? &*found : nullptr;
}

void i2c_v1::release(i2c_port_entry& entry, bool ack)
{
  auto const now  = ctx_.clock.time().ps;
  entry.kind      = entry_released;
  entry.ack       = ack ? 1 : 0;
  entry.due_ps    = now;
  auto const unit = entry.unit;
  schedule_port(entry);
  for(auto const index : place_after(state_.port.entries, unit, now))
  {
    schedule_port(state_.port.entries[index]);
  }
}

void i2c_v1::release_held()
{
  if(auto* const held = held_entry())
  {
    release(*held, held->ack != 0);
  }
}

void i2c_v1::port_stopped(i2c_port_entry const& entry)
{
  auto const cut = [&](i2c_port_entry const& queued) {
    return (queued.kind == entry_phase or queued.kind == entry_ending) and queued.unit < entry.unit;
  };
  std::vector<std::pair<std::uint64_t, i2c_phase>> reports;
  for(auto const& queued : state_.port.entries)
  {
    if(cut(queued))
    {
      reports.emplace_back(queued.unit, static_cast<i2c_phase>(queued.phase));
    }
  }
  std::erase_if(state_.port.entries, cut);
  target_ended();
  std::ranges::sort(reports);
  flags();
  for(auto const& [unit, phase] : reports)
  {
    controller_->dropped(unit, phase, vtime{entry.due_ps});
  }
}

void i2c_v1::target_ended()
{
  auto const waited = state_.wire_busy != 0;
  if(state_.addressed != 0 or state_.last_ack != 0)
  {
    if(get(regs_, enable_) != 0 and state_.last_ack != 0)
    {
      update(regs_, stop_detected_, 1);
    }
    update(regs_, transmitter_, 0);
  }
  state_.addressed = 0;
  state_.last_ack  = 0;
  state_.wire_busy = 0;
  if(not controlling() and step() == wire::idle)
  {
    update(regs_, busy_, 0);
  }
  if(waited
     and get(regs_, enable_) != 0
     and get(regs_, start_) != 0
     and step() == wire::idle
     and not controlling())
  {
    begin_start();
  }
}

void i2c_v1::rearm_port(i2c_port port, std::optional<reset_kind> kind)
{
  auto const now   = ctx_.clock.time().ps;
  auto const shift = kind ? std::exchange(reset_at_, std::nullopt).value_or(0) : 0;
  auto const moved = [&](std::uint64_t due) {
    return std::max(now, due > shift ? due - shift : 0);
  };
  if(auto const holding = held_unit(port.entries))
  {
    auto const floor = kind ? shift : now;
    for(auto& entry : port.entries)
    {
      if(entry.kind == entry_held)
      {
        entry.due_ps = std::max(entry.due_ps, floor);
      }
    }
    std::ignore = place_after(port.entries, *holding, floor);
  }
  if(kind)
  {
    std::erase_if(port.entries, [](auto const& entry) { return entry.kind == entry_stop; });
  }
  for(auto& entry : port.entries)
  {
    if(entry.kind == entry_held or entry.kind == entry_released)
    {
      entry.kind = entry_ending;
    }
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

void i2c_v1::before_reset(reset_kind)
{
  reset_at_ = ctx_.clock.time().ps;
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
  auto       port       = std::move(state_.port);
  state_                = {};
  state_.generation     = generation;
  state_.acked.resize(targets_.size());
  state_.in_transfer.resize(targets_.size());
  deferred_.clear();
  timing_checked_ = false;
  rearm_port(std::move(port), kind);
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
    received_taken();
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
  if(kind == port_due)
  {
    if(payload >> 32 == state_.port.generation)
    {
      run_port(static_cast<std::uint32_t>(payload));
    }
    return;
  }
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
  if(not controller_ and not loaded.port.entries.empty())
  {
    throw config_error(
        ctx_.part.part, 0, std::format("devices.{}", desc_.name),
        "an I2C controller attached to the instance, the snapshot holds port entries");
  }
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
    auto const found      = targets.find(desc.name);
    auto const controller = controllers.find(desc.name);
    return std::make_shared<i2c_v1>(
        ctx, desc,
        found != targets.end() ? found->second : std::vector<std::shared_ptr<i2c_target>>{},
        controller != controllers.end() ? controller->second : nullptr, variant);
  };
}
}
