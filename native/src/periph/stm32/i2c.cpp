#include "dma_signal.hpp"
#include "factories.hpp"
#include "i2c_link_port.hpp"
#include "i2c_port_queue.hpp"
#include "model_base.hpp"
#include "target_port.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/scheduler.hpp>
#include <popcorn/periph/i2c.hpp>
#include <popcorn/periph/reset_listener.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/i2c.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
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
i2c_bus::~i2c_bus() = default;

namespace
{
constexpr std::uint32_t phase_done     = 0;
constexpr std::uint32_t hold_begin     = 1;
constexpr std::uint32_t hold_end       = 2;
constexpr std::uint32_t port_due       = 3;
constexpr std::uint32_t timeout_due    = 4;
constexpr std::uint32_t completed      = 5;
constexpr std::uint64_t low_cycles     = 2048;
constexpr std::uint64_t idle_cycles    = 4;
constexpr std::uint32_t address_length = 10;
constexpr std::uint32_t byte_length    = 9;
constexpr std::uint32_t stop_length    = 1;

constexpr std::string_view unsupported_text =
    "10-bit addressing, SBC or NOSTRETCH in controller mode, the SMBus host or device address, "
    "ALERT, PEC or the autonomous trigger is configured: not modelled, the I2C runs as a plain "
    "7-bit controller and target";

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

struct i2c_answering
{
  std::uint32_t index{};
  std::uint8_t  acked{};

  friend auto operator==(i2c_answering const&, i2c_answering const&) -> bool = default;
};

struct i2c_pending_answer
{
  std::uint32_t index{};
  std::uint8_t  done{};
  std::uint8_t  ack{};
  std::uint8_t  byte{0xFF};
  std::uint8_t  error{};
  std::uint64_t at_ps{};
  std::uint64_t stretch_ps{};

  friend auto operator==(i2c_pending_answer const&, i2c_pending_answer const&) -> bool = default;
};

struct i2c_completion
{
  std::uint64_t ticket{};
  std::uint64_t at_ps{};
  std::uint64_t stretch_ps{};
  std::uint8_t  read{};
  std::uint8_t  ack{};
  std::uint8_t  byte{0xFF};
  std::uint8_t  error{};

  friend auto operator==(i2c_completion const&, i2c_completion const&) -> bool = default;
};

struct i2c_link_attachment
{
  std::shared_ptr<i2c_link_target> device;
  bool                             muted{};
};

struct i2c_transmit
{
  std::uint8_t  target{};
  std::uint8_t  shift{};
  std::uint8_t  loaded{};
  std::uint8_t  wait{};
  std::uint8_t  ask{};
  std::uint8_t  first{};
  std::uint32_t left{};
  std::uint32_t asks{};

  friend auto operator==(i2c_transmit const&, i2c_transmit const&) -> bool = default;
};

struct i2c_timer
{
  std::optional<std::uint64_t> low_ps;
  std::optional<std::uint64_t> low_end_ps;
  std::optional<std::uint64_t> own_ps;
  std::uint64_t                extended_ps{};
  std::optional<std::uint64_t> idle_ps;
  std::uint8_t                 idle_spent{};
  std::uint64_t                a_from_ps{};
  std::uint64_t                b_from_ps{};
  std::optional<std::uint64_t> armed_ps;
  std::uint64_t                seq{};
  std::optional<std::uint64_t> gap_ps;
  std::uint64_t                gap_unit{};
  std::optional<std::uint64_t> released_ps;
  std::uint64_t                released_unit{};

  friend auto operator==(i2c_timer const&, i2c_timer const&) -> bool = default;
};

enum class expiry : std::uint8_t
{
  low,
  extend,
  idle,
};

struct i2c_expiry
{
  std::uint64_t at{};
  expiry        kind{};
};

struct i2c_state
{
  std::uint32_t                       generation{};
  std::uint8_t                        step{};
  std::uint8_t                        read{};
  std::vector<i2c_answering>          targets;
  std::vector<i2c_pending_answer>     answers;
  std::uint32_t                       owed{};
  std::uint8_t                        timeout_stop{};
  std::vector<i2c_completion>         completions;
  std::uint64_t                       refused{};
  std::uint32_t                       left{};
  std::uint8_t                        tx_full{};
  std::uint8_t                        tx_data{};
  std::uint8_t                        shift{};
  std::uint8_t                        rx_held{};
  std::uint8_t                        rx_data{};
  std::uint64_t                       end_ps{};
  std::uint64_t                       phase_ps{};
  std::uint32_t                       holds{};
  i2c_hold                            pending{};
  std::uint8_t                        pending_waits{};
  i2c_hold                            active{};
  std::uint8_t                        addressed{};
  std::uint8_t                        involved{};
  std::uint8_t                        wire_busy{};
  i2c_transmit                        tx;
  i2c_timer                           timer;
  i2c_port                            port;
  std::vector<std::vector<std::byte>> devices;

  friend auto operator==(i2c_state const&, i2c_state const&) -> bool = default;
};

/**
 * @brief I2C1 to I2C4 of the STM32U5, RM0456 section 65 "Inter-integrated circuit interface
 *        (I2C)", controller mode with 7-bit addresses. The SCL period is
 *        ((SCLH + 1) + (SCLL + 1)) x (PRESC + 1) kernel clock cycles, TIMINGR, without the
 *        tSYNC delays of the SCL edges, which depend on the board. A START and the address byte
 *        take 10 periods, a data byte 9, a STOP 1. A phase keeps the duration it started with
 *        across a clock change. CR2.START sends a START, or a repeated START after the NBYTES
 *        transfer, and the address. Every board target whose i2c_target::matches() takes SADD
 *        answers through i2c_target::start_address(), in attachment order, and the targets share
 *        the open-drain SDA and SCL, I2C-bus specification UM10204 "SDA and SCL signals": an
 *        address or a written byte is acknowledged when one target acknowledges it, written
 *        bytes reach every target that acknowledged the address, a read byte is the AND of the
 *        bytes those targets send, SCL stays low for the longest stretch, and the byte ends with
 *        the error of the first target in attachment order whose reply carries one. A repeated
 *        START to another address keeps the targets of the earlier addresses in the transfer,
 *        and a STOP or a lost arbitration reaches every target an address of the transfer
 *        matched, as an STM32 target sets STOPF for a transfer it was addressed in, RM0456
 *        65.9.7 STOPF, RM0481 53.9.7. The controller acknowledges each read byte except the one
 *        that ends NBYTES with RELOAD clear, RM0456 65.4.9 "Controller receiver", RM0481 53.4.9,
 *        and passes that acknowledge to i2c_target::read_acked(). A NACK of the address sets
 *        NACKF and sends a STOP. Transmit: TXIS while TXDR is empty and a byte of NBYTES is
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
 *        that window ends it at once, the target's STOP at the time of the write. PECERR and
 *        ALERT are never set. DMA requests, RM0456 section 65.7, through the GPDMA
 *        peer, low while PE is clear since PE = 0 clears their flags: `<instance>_tx_dma` follows
 *        TXIS with CR1.TXDMAEN, `<instance>_rx_dma` RXNE with CR1.RXDMAEN, `<instance>_evc_dma` TC
 *        with AUTOCR.TCDMAEN or TCR with AUTOCR.TCRDMAEN. The targets and the set of targets
 *        the current address matched are saved and loaded with the model.
 *
 *        Deferred answers, controller mode: at the end of an address, write or read phase the
 *        model asks each answering target through i2c_target::start_deferred(),
 *        i2c_target::write_deferred() or i2c_target::read_deferred(), with a ticket made of the
 *        phase's generation and the target's attachment index. A target that leaves its answer
 *        pending completes it through i2c_target_port::complete() or
 *        i2c_target_port::complete_read(), one event on the lane it names. A due time in the
 *        past keeps its own instruction count, and its time stays the one the target gave. SCL
 *        stays low from the phase's end to the last completion. The phase then runs as for a
 *        stretch ending at the latest answer time plus that answer's stretch_ps, with the
 *        answers combined in attachment order as above: NACKF, RXNE, the next phase, the STOP
 *        and every flag the answer sets follow that time. The hold counts as SCL low for
 *        TIMOUTEN and as the target's stretch for TEXTEN: TIMEOUT sets once the hold reaches
 *        TIMEOUTA, a completion due at the TIMEOUT instant comes after it, and the STOP it sends
 *        leaves at the end of the hold. Across a System link of latency L the hold of a phase is
 *        2L + p + s, p the phase time and s the remote target's stretch, so TIMEOUT sets when that
 *        sum reaches TIMEOUTA. Clearing PE or a reset of any
 *        kind drops the pending phase and moves the generation, so its later completions are
 *        refused when they fall due and counted in the saved `refused`, as are a second
 *        completion of one answer and a completion of the wrong kind. The pending answers and
 *        every completion not yet due are saved with the model. A completion event survives an
 *        RCC reset with its record, a machine reset clears both. Each target is attached to the
 *        model at its construction and detached at its destruction.
 *
 *        Link attachments, i2c_link_port: a link target answers after every board target, in its
 *        attachment order, with the ticket index that follows the board targets'. It hears every
 *        address phase through i2c_target::matches(), every data phase of the transfer and its STOP
 *        or lost arbitration, and answers a data phase of an address it did not acknowledge with
 *        NACK or 0xFF and no stretch. The saved acknowledged set holds board targets only. A
 *        machine reset calls its reset() after the board targets', a power cut its power_cut()
 *        after theirs. A link controller drives the port as a board controller does. Its phases
 *        hold the wire only while they run: at an RCC reset a phase that started is dropped at the
 *        later of its end and the reset, and every phase not started restarts from its posted time
 *        and the phases and STOPs still queued ahead of it, dropped at the later of its end and the
 *        reset when that start is at or before the reset. A phase queued after the reset starts the
 *        same way, so its outcome does not depend on whether it reached the port before or after
 *        the reset. The model saves no link attachment.
 *
 *        Target mode, address, write and read phases, RM0456 65.4.8 and RM0481 53.4.8: the
 *        controller of board::i2c_controllers queues i2c_controller_port entries on the lane it
 *        names, each with a unit above the last one. The port runs the entries of one instruction
 *        count in unit order. A phase starts at the latest of its time, the end of the phases
 *        queued before it and the time of the STOPs queued before it. A START with PE set sets BUSY
 *        until the STOP. At the end of an address phase the address matches OAR1 with OA1EN and
 *        OA1MODE clear, OAR2 with OA2EN and its OA2MSK low bits ignored, no reserved address
 *        0b0000xxx or 0b1111xxx once OA2MSK is not 0, and the address byte 0b00000000 with GCEN,
 *        each read at that time, RM0456 65.9.3, 65.9.4 and 65.9.1, RM0481 53.9.3, 53.9.4 and
 *        53.9.1. A match while PE is set, set already at the START, and no transfer of the I2C runs
 *        as controller is acknowledged: ADDCODE takes the address, DIR the direction and ADDR sets
 *        unless ADDRACLR is set, RM0456 65.9.7, RM0481 53.9.7. Any other address phase, PE clear
 *        included, answers NACK, as does the I2C held in reset. An address match clears CR2.NACK.
 *        The answer reaches the controller at the end of the phase, or, while ADDR is set, once
 *        ADDRCF or PE = 0 clears it: SCL stays stretched after the acknowledge and every later
 *        phase and STOP waits for the release, RM0456 65.4.8, RM0481 53.4.8. A written byte after
 *        an acknowledged write address lands in RXDR and sets RXNE at the end of its phase, RM0456
 *        65.4.8 "Target receiver", RM0481 53.4.8. The I2C acknowledges it unless CR2.NACK is set,
 *        and a NACK sent clears CR2.NACK, RM0456 65.9.2, RM0481 53.9.2. While RXNE is set, the next
 *        byte stays in the shift register and SCL stretched, and the read of RXDR moves it into
 *        RXDR, sets RXNE again, takes the acknowledge from CR2.NACK at that time and releases SCL.
 *        The answer then reaches the controller at the read. Silicon copies a byte into RXDR after
 *        its eighth SCL pulse and stretches between the eighth and the ninth, RM0456 65.4.7, RM0481
 *        53.4.7: the model sets RXNE and releases a stretched byte after the ninth, one bit later.
 *        PE = 0 releases a stretched byte with NACK. A written byte with no acknowledged write
 *        address answers NACK and reaches no register. After an acknowledged read address the I2C
 *        is a target transmitter, RM0456 65.4.8 "Target transmitter", RM0481 53.4.8. At the release
 *        of the address and after each byte the controller acknowledges, the shift register takes
 *        TXDR when TXE is 0, which sets TXE, and TXIS sets to ask for the next byte. When TXE is 1
 *        there, TXIS sets unless NOSTRETCH is set, and SCL stays stretched before the next read
 *        byte until the TXDR write, which loads the byte and starts it. A TXDR write clears TXIS. A
 *        read phase sends the loaded byte and answers it with the controller's acknowledge at its
 *        end. A NACK sets NACKF and ends the transmission, RM0456 65.9.7, RM0481 53.9.7: no later
 *        load and no later TXIS. TXDR keeps its byte, and a TXIS already set stays until a TXDR
 *        write, at a NACK and at a STOP alike. A read phase with no acknowledged read address,
 *        after a NACK or with PE clear answers 0xFF, and PE = 0 starts a held read byte at once,
 *        read as 0xFF. With NOSTRETCH, RM0456 65.4.8 "Target without clock stretching" and 65.4.17,
 *        RM0481 53.4.8 and 53.4.17, ADDR holds no SCL, a read byte that starts with the shift
 *        register empty sends 0xFF and sets OVR, the first read byte of a transfer sets OVR while
 *        STOPF is set, a written byte that ends while RXNE is set is lost with NACK and sets OVR,
 *        and a firmware write of ISR.TXIS sets TXIS, RM0456 65.9.7. With SBC and NOSTRETCH clear,
 *        RM0456 65.4.8 "Target byte control mode", RM0481 53.4.8, the address release takes NBYTES.
 *        Each received byte counts it down, and the byte that ends it with RELOAD set lands in
 *        RXDR, sets TCR and holds SCL before its acknowledge until a non-zero NBYTES write, which
 *        takes the acknowledge from CR2.NACK and reloads the count. A transmitter asks TXIS at most
 *        NBYTES times, and the acknowledged byte that ends NBYTES with RELOAD set sets TCR and
 *        holds the next read byte until the NBYTES write, which reloads the count and loads the
 *        shift register. A non-zero NBYTES write clears TCR whatever came between, RM0456 65.9.7,
 *        RM0481 53.9.7, a STOP included: a transmitter's TCR or empty-TXDR stretch holds only the
 *        next read byte, so a STOP the controller queues runs past it, and TCR stays set until that
 *        write. A STOP that waits for a stretch runs after the stretched byte's answer, the end of
 *        a held read byte included. With RELOAD clear the count stops at 0 and bytes are
 *        acknowledged as without SBC. A STOP sets STOPF when an address of the transfer was
 *        acknowledged, unless STOPFACLR is set, and clears CR2.NACK. It ends the phases of lower
 *        units still queued or running, reported as dropped at its time and not received, and BERR
 *        sets for a data byte of an acknowledged address it ends, RM0456 65.4.17, RM0481 53.4.17. A
 *        START the firmware requests during the controller's transfer waits for its STOP. While
 *        OA1EN is set, OAR1 keeps OA1 and OA1MODE, and while OA2EN is set OAR2 keeps OA2 and
 *        OA2MSK, RM0456 65.9.3 and 65.9.4, RM0481 53.9.3 and 53.9.4, and while PE is set CR1 keeps
 *        NOSTRETCH, ANFOFF and DNF, RM0456 65.9.1, RM0481 53.9.1: one warning per register kind and
 *        instance, model_error under strict_peripherals. An RCC reset releases a stretch at its
 *        time, starts a held read byte there, and drops the phases started at or before it, each
 *        reported at the later of its end and the reset, and keeps the later phases and STOPs,
 *        which run in the reset model. A phase queued after it with a start at or before it is
 *        dropped the same way, and a STOP or a drop queued after it is due no earlier than it. A
 *        machine reset of any kind, the model a reset_listener, starts a held read byte at the
 *        reset and drops every queued phase at its end rebased by the reset's local time and
 *        removes every queued STOP. A phase a reset drops holds the wire to its end, in the new
 *        base after a machine reset, and a later phase starts no earlier. Dropped units survive
 *        every reset and are reported once, through i2c_controller_sink::dropped(). The port
 *        entries, stretched ones included, the last unit, the generation and the time of the last
 *        RCC reset are saved with the model. Not modelled, logged once: 10-bit addressing, SBC or
 *        NOSTRETCH in controller mode, SBC with NOSTRETCH in target mode, the SMBus host and device
 *        addresses, ALERT and PEC, the autonomous trigger. A START as controller or a target
 *        address phase with one of them set raises model_error under strict_peripherals. A write of
 *        I2C_AUTOCR only warns, since a modelled target setup with SBC or NOSTRETCH writes it too,
 *        and the refusal comes at the next controller START.
 *
 *        SMBus timeouts, RM0456 65.4.12 "Timeout detection" and 65.4.17 "Timeout error", RM0481
 *        53.4.12 and 53.4.17, run while PE is set and the kernel clock runs, at the kernel clock
 *        rate the model reads at each of its events and register accesses. With TIMOUTEN set and
 *        TIDLE clear, TIMEOUT sets once SCL has been low for (TIMEOUTA + 1) x 2048 kernel periods,
 *        counted from the later of the SCL fall and the TIMOUTEN set. As a controller SCL is low
 *        from the end of an address or data phase to the start of the next phase or STOP, a
 *        target's stretch and the TXIS, RXNE, TCR and TC waits included. As a target addressed by
 *        the last address phase of the controller's transfer, SCL is low from the end of an address
 *        or data phase to the start of the next phase or STOP the port holds, the I2C's own stretch
 *        included. A target the last address phase did not match counts no SCL low between the
 *        controller's phases, for a board controller and a remote one alike, where silicon measures
 *        SCL on the line whatever the address. A phase or STOP queued with a start at or before the
 *        timeout's instant releases SCL in time, and so does a release of the bus by the remote
 *        controller at an RCC reset or a PE clear of its I2C, i2c_link_port::bus_released(), which
 *        an RCC reset of the target keeps. The system holds every event due at or past a barrier
 *        until the drains of that barrier ran, so a post a drain hands late is in the port before
 *        the timeout of its window fires. With TIMOUTEN and TIDLE set, TIMEOUT sets once the bus
 *        has been idle, PE set and BUSY 0, for (TIMEOUTA + 1) x 4 kernel periods, once per idle
 *        period, which starts at the end of the STOP or of the host hold that frees the bus, or at
 *        the PE set. With TEXTEN set, TIMEOUT sets once the I2C's own stretches of one transfer,
 *        START to STOP, add up to (TIMEOUTB + 1) x 2048 kernel periods, each counted from the later
 *        of its start and the TEXTEN set: the waits as a controller, each starting once the
 *        target's stretch before it ends, the held phases as a target. The sum restarts at 0 then,
 *        at each STOP and at each TEXTEN set. A timeout of SCL low or of the stretch sum ends the
 *        transfer. As a controller the I2C sends a STOP, at once or after the stretch of the target
 *        when one runs. As a target it releases its stretch as PE = 0 does, an ADDR stretch with
 *        ACK, a written byte with NACK and a held read byte as 0xFF, and leaves the transfer: later
 *        phases answer NACK or 0xFF until the next address match. TIMEOUT raises the error IRQ with
 *        ERRIE and is cleared by TIMOUTCF and by PE = 0. While TIMOUTEN is set TIMEOUTR keeps
 *        TIMEOUTA and TIDLE, and while TEXTEN is set it keeps TIMEOUTB, RM0456 65.9.6, RM0481
 *        53.9.6: one warning per instance, model_error under strict_peripherals. The timer state is
 *        saved with the model.
 */
class i2c final : public dispatch_model<i2c, i2c_state>,
                  public i2c_bus,
                  public i2c_controller_port,
                  public i2c_target_port,
                  public i2c_link_port,
                  public reset_listener,
                  public bus_reset_listener
{
public:
  i2c(model_context&                           ctx,
      peripheral_desc const&                   desc,
      std::vector<std::shared_ptr<i2c_target>> targets,
      std::shared_ptr<i2c_controller_sink>     controller,
      i2c_variant const&                       variant);
  i2c(i2c const&)                    = delete;
  auto operator=(i2c const&) -> i2c& = delete;
  i2c(i2c&&)                         = delete;
  auto operator=(i2c&&) -> i2c&      = delete;
  ~i2c() override;

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
  [[nodiscard]] auto last_unit() const -> std::uint64_t override;
  [[nodiscard]] auto queued() const -> std::size_t override;
  void complete(std::uint64_t ticket, i2c_reply reply, vtime at, std::uint32_t lane) override;
  void complete_read(std::uint64_t ticket, i2c_read_reply reply, vtime at, std::uint32_t lane)
      override;
  void attach_target(std::shared_ptr<i2c_link_target> target) override;
  void detach_target(i2c_link_target& target) noexcept override;
  void attach_controller(std::shared_ptr<i2c_link_controller> controller) override;
  void detach_controller(i2c_link_controller& controller) noexcept override;
  void mute(i2c_link_target const& target) override;
  void unmute(i2c_link_target const& target) override;
  auto muted(i2c_link_target const& target) const -> bool override;
  auto phase_time() const -> std::uint64_t override;
  void bus_released(vtime at, std::uint64_t unit) override;
  auto board_use() const -> i2c_board_use override;

  void enable_written(std::uint32_t old, std::uint32_t now);
  void command_written(std::uint32_t old, std::uint32_t now);
  void status_written(std::uint32_t old, std::uint32_t now);
  void clear_written(std::uint32_t old, std::uint32_t now);
  void plain_written(std::uint32_t old, std::uint32_t now);
  void timeout_written(std::uint32_t old, std::uint32_t now);
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
  [[nodiscard]] auto ticket_of(std::uint32_t index) const -> std::uint64_t;
  void               answer(std::uint32_t index, std::optional<i2c_reply> const& reply);
  void               answer(std::uint32_t index, std::optional<i2c_read_reply> const& reply);
  void               answered();
  void               queue_completion(i2c_completion record, vtime at, std::uint32_t lane);
  void               completion_due(std::uint64_t ticket);
  void               stop_on_timeout(std::uint64_t from_ps);
  void               stop_done();
  void               stop_targets(vtime at);
  void               release_links();
  void               lost_done();
  void               hold_begun();
  void               hold_ended(std::uint32_t id);
  void               take_bus();
  [[nodiscard]] auto held() const -> bool;
  void               lose();
  [[nodiscard]] auto failed(i2c_error error) -> bool;
  void               abort();
  void               flags();
  [[nodiscard]] auto unsupported() const -> bool;
  [[nodiscard]] auto bus_taken() const -> bool;
  void               stop_flag();

  void port_check(std::uint32_t lane, std::uint64_t unit) const;
  void queue_phase(
      i2c_phase     phase,
      std::uint8_t  data,
      std::uint64_t phase_ps,
      vtime         at,
      std::uint32_t lane,
      std::uint64_t unit,
      bool          ack = false);
  [[nodiscard]] auto after_model_reset(std::uint64_t due_ps) const -> std::uint64_t;
  void               queue_port(i2c_port_entry entry);
  void               schedule_port(i2c_port_entry const& entry);
  void               rearm_port(i2c_port port, std::optional<reset_kind> kind);
  void               run_port(std::uint32_t lane);
  void               phase_begun(i2c_port_entry const& entry);
  [[nodiscard]] auto phase_ended(i2c_port_entry const& entry) -> std::optional<i2c_answer>;
  [[nodiscard]] auto received(std::uint8_t byte) -> std::optional<bool>;
  [[nodiscard]] auto taken_ack() -> bool;
  [[nodiscard]] auto read_begun(i2c_port_entry& entry) -> bool;
  [[nodiscard]] auto read_ended(i2c_port_entry const& entry) -> i2c_answer;
  [[nodiscard]] auto counted() -> bool;
  [[nodiscard]] auto no_stretch() const -> bool;
  [[nodiscard]] auto byte_control() const -> bool;
  void               address_released();
  void               load_point();
  void               ask_transmit();
  void               resume_read();
  void               begin_read(i2c_port_entry& entry, std::uint8_t byte);
  void               target_reload();
  [[nodiscard]] auto held_entry(std::uint8_t phase) -> i2c_port_entry*;
  void               release(i2c_port_entry& entry, bool ack);
  void               port_stopped(i2c_port_entry const& entry);
  void               target_transfer_ended(std::uint64_t at_ps);
  auto               link_of(this auto& self, i2c_link_target const& target) -> auto&;
  [[nodiscard]] auto matches(std::uint8_t address, bool read) const -> bool;
  void               warn_target_unsupported();
  [[nodiscard]] auto cycles_ps(std::uint64_t cycles) const -> std::optional<std::uint64_t>;
  [[nodiscard]] auto held_since() const -> std::optional<std::uint64_t>;
  [[nodiscard]] auto expiry_due() const -> std::optional<i2c_expiry>;
  void               arm_timeout();
  void               stretch_ended(std::uint64_t since);
  void               transfer_ended();
  void               timed_out();
  void               unmodelled(std::string_view reg_name, bool& logged, std::string message);
  void               rethrow_strict();

  register_file&                               regs_;
  std::vector<std::shared_ptr<i2c_target>>     targets_;
  std::shared_ptr<i2c_controller_sink>         controller_;
  std::vector<i2c_link_attachment>             links_;
  std::shared_ptr<i2c_link_controller>         link_controller_;
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
  field_ref                                    nack_;
  field_ref                                    no_stretch_;
  field_ref                                    byte_control_;
  field_ref                                    timeout_a_;
  field_ref                                    idle_mode_;
  field_ref                                    timeout_enable_;
  field_ref                                    timeout_b_;
  field_ref                                    extend_enable_;
  field_ref                                    timeout_flag_;
  reg_ref                                      control_;
  std::uint32_t                                fixed_while_enabled_{};
  reg_ref                                      own1_reg_;
  reg_ref                                      own2_reg_;
  reg_ref                                      timeout_reg_;
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
  bool                                         protect_logged_{};
  bool                                         timeout_protect_logged_{};
  bool                                         control_protect_logged_{};
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
     {"TIMEOUTR", &i2c::timeout_written},
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
      nack_{field("CR2", "NACK")}, no_stretch_{field("CR1", "NOSTRETCH")},
      byte_control_{field("CR1", "SBC")}, timeout_a_{field("TIMEOUTR", "TIMEOUTA")},
      idle_mode_{field("TIMEOUTR", "TIDLE")}, timeout_enable_{field("TIMEOUTR", "TIMOUTEN")},
      timeout_b_{field("TIMEOUTR", "TIMEOUTB")}, extend_enable_{field("TIMEOUTR", "TEXTEN")},
      timeout_flag_{field("ISR", "TIMEOUT")}, control_{reg("CR1")},
      fixed_while_enabled_{
          field("CR1", "NOSTRETCH").mask()
          | field("CR1", "ANFOFF").mask()
          | field("CR1", "DNF").mask()},
      own1_reg_{reg("OAR1")}, own2_reg_{reg("OAR2")}, timeout_reg_{reg("TIMEOUTR")},
      error_enable_{field("CR1", "ERRIE")}, kernel_{ctx.clock.domain(lower(desc.name))},
      event_line_{irq(desc.name + "_EV_IRQn")}, error_line_{irq(desc.name + "_ER_IRQn")},
      tx_dma_enable_{field("CR1", "TXDMAEN")}, rx_dma_enable_{field("CR1", "RXDMAEN")},
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
  }
  for(auto const name : {"SMBHEN", "SMBDEN", "ALERTEN", "PECEN"})
  {
    target_unsupported_.push_back(field("CR1", name));
  }
  if(variant.autocr)
  {
    unsupported_.push_back(field("I2C_AUTOCR", "TRIGEN"));
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

i2c::~i2c()
{
  for(auto const& target : targets_)
  {
    target->detached();
  }
  for(auto const& link : links_)
  {
    link.device->detached();
  }
  if(link_controller_)
  {
    link_controller_->unlinked();
  }
}

auto i2c::step() const -> phase
{
  return static_cast<phase>(state_.step);
}

void i2c::enter(phase next)
{
  state_.step = static_cast<std::uint8_t>(next);
  if(next == phase::wait_tx
     or next == phase::wait_rx
     or next == phase::wait_reload
     or next == phase::wait_end)
  {
    auto& timer  = state_.timer;
    timer.own_ps = timer.own_ps.value_or(state_.end_ps);
  }
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
  auto& timer = state_.timer;
  if(timer.low_ps and not timer.low_end_ps)
  {
    timer.low_end_ps = from_ps;
  }
  if(auto const own = std::exchange(timer.own_ps, std::nullopt))
  {
    stretch_ended(*own);
  }
  enter(next);
  ++state_.generation;
  auto const ps   = length_ps(periods);
  state_.phase_ps = ps;
  if(ps == 0)
  {
    return;
  }
  state_.end_ps = std::max(from_ps + ps, ctx_.clock.time().ps);
  ctx_.events.schedule_at(vtime{state_.end_ps}, phase_done, state_.generation, ctx_.clock);
}

void i2c::begin_start(std::uint64_t from_ps)
{
  if(unsupported())
  {
    unmodelled("CR1", unsupported_logged_, std::string{unsupported_text});
  }
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
  auto const address = static_cast<std::uint8_t>((get(regs_, address_) >> 1) & 0x7Fu);
  for(auto& answering : state_.targets)
  {
    answering.acked = 0;
  }
  state_.answers.clear();
  state_.owed = 0;
  if(get(regs_, ten_bit_) == 0)
  {
    auto const at = target_at();
    for(std::size_t index = 0; index < targets_.size(); ++index)
    {
      if(targets_[index]->matches(address, at))
      {
        auto const slot = static_cast<std::uint32_t>(index);
        auto const place =
            std::ranges::lower_bound(state_.targets, slot, {}, &i2c_answering::index);
        if(place == state_.targets.end() or place->index != slot)
        {
          state_.targets.insert(place, {.index = slot});
        }
        answer(
            slot, targets_[index]->start_deferred(address, state_.read != 0, at, ticket_of(slot)));
      }
    }
    for(std::size_t index = 0; index < links_.size(); ++index)
    {
      auto const& device = links_[index].device;
      if(device->matches(address, at))
      {
        auto const slot = static_cast<std::uint32_t>(targets_.size() + index);
        answer(slot, device->start_deferred(address, state_.read != 0, at, ticket_of(slot)));
      }
    }
  }
  if(state_.owed == 0)
  {
    answered();
  }
}

void i2c::byte_done()
{
  auto const at = target_at();
  state_.answers.clear();
  state_.owed = 0;
  if(state_.read == 0)
  {
    for(auto const& answering : state_.targets)
    {
      if(answering.acked != 0)
      {
        answer(
            answering.index, targets_[answering.index]->write_deferred(
                                 state_.shift, at, ticket_of(answering.index)));
      }
    }
    for(std::size_t index = 0; index < links_.size(); ++index)
    {
      auto const slot = static_cast<std::uint32_t>(targets_.size() + index);
      answer(slot, links_[index].device->write_deferred(state_.shift, at, ticket_of(slot)));
    }
  }
  else
  {
    auto const ack = state_.left > 1 or get(regs_, reload_) != 0;
    for(auto const& answering : state_.targets)
    {
      if(answering.acked != 0)
      {
        answer(
            answering.index,
            targets_[answering.index]->read_deferred(at, ack, ticket_of(answering.index)));
      }
    }
    for(std::size_t index = 0; index < links_.size(); ++index)
    {
      auto const slot = static_cast<std::uint32_t>(targets_.size() + index);
      answer(slot, links_[index].device->read_deferred(at, ack, ticket_of(slot)));
    }
  }
  if(state_.owed == 0)
  {
    answered();
  }
}

auto i2c::ticket_of(std::uint32_t index) const -> std::uint64_t
{
  return std::uint64_t{state_.generation} << 32 | index;
}

void i2c::answer(std::uint32_t index, std::optional<i2c_reply> const& reply)
{
  auto& slot =
      state_.answers.emplace_back(i2c_pending_answer{.index = index, .at_ps = state_.end_ps});
  if(not reply)
  {
    ++state_.owed;
    return;
  }
  slot.done       = 1;
  slot.ack        = reply->ack ? 1 : 0;
  slot.error      = static_cast<std::uint8_t>(reply->error);
  slot.stretch_ps = reply->stretch_ps;
}

void i2c::answer(std::uint32_t index, std::optional<i2c_read_reply> const& reply)
{
  auto& slot =
      state_.answers.emplace_back(i2c_pending_answer{.index = index, .at_ps = state_.end_ps});
  if(not reply)
  {
    ++state_.owed;
    return;
  }
  slot.done       = 1;
  slot.byte       = reply->byte;
  slot.error      = static_cast<std::uint8_t>(reply->error);
  slot.stretch_ps = reply->stretch_ps;
}

void i2c::answered()
{
  auto const address = step() == phase::address;
  auto       ack     = false;
  auto       byte    = std::uint8_t{0xFF};
  auto       error   = i2c_error::none;
  auto       base    = state_.end_ps;
  auto       release = state_.end_ps;
  for(auto const& slot : std::exchange(state_.answers, {}))
  {
    ack  = ack or slot.ack != 0;
    byte = static_cast<std::uint8_t>(byte & slot.byte);
    if(error == i2c_error::none)
    {
      error = static_cast<i2c_error>(slot.error);
    }
    base    = std::max(base, slot.at_ps);
    release = std::max(release, slot.at_ps + slot.stretch_ps);
    if(address and slot.index < targets_.size())
    {
      auto const target = std::ranges::find(state_.targets, slot.index, &i2c_answering::index);
      target->acked     = slot.ack;
    }
  }
  state_.owed   = 0;
  state_.end_ps = base;
  if(not address)
  {
    --state_.left;
  }
  if(failed(error))
  {
    state_.timeout_stop = 0;
    return;
  }
  state_.end_ps = release;
  if(address)
  {
    if(not ack)
    {
      update(regs_, nack_received_, 1);
      begin_stop(release);
    }
    else
    {
      next_byte(release);
    }
  }
  else if(state_.read == 0)
  {
    if(not ack)
    {
      update(regs_, nack_received_, 1);
      begin_stop(release);
    }
    else
    {
      after_byte(release);
    }
  }
  else
  {
    if(get(regs_, rx_full_) != 0)
    {
      state_.rx_held = 1;
      state_.rx_data = byte;
      enter(phase::wait_rx);
    }
    else
    {
      regs_.poke(receive_.offset, byte);
      update(regs_, rx_full_, 1);
      after_byte(release);
    }
  }
  if(std::exchange(state_.timeout_stop, 0) != 0)
  {
    stop_on_timeout(release);
  }
}

void i2c::stop_on_timeout(std::uint64_t from_ps)
{
  switch(step())
  {
    case phase::address:
    case phase::byte:
    case phase::wait_tx:
    case phase::wait_rx:
    case phase::wait_reload:
    case phase::wait_end:
      state_.rx_held = 0;
      begin_stop(from_ps);
      break;
    case phase::idle:
    case phase::stop:
    case phase::lost:
      break;
  }
}

void i2c::complete(std::uint64_t ticket, i2c_reply reply, vtime at, std::uint32_t lane)
{
  queue_completion(
      {.ticket     = ticket,
       .stretch_ps = reply.stretch_ps,
       .ack        = reply.ack ? std::uint8_t{1} : std::uint8_t{0},
       .error      = static_cast<std::uint8_t>(reply.error)},
      at, lane);
}

void i2c::complete_read(std::uint64_t ticket, i2c_read_reply reply, vtime at, std::uint32_t lane)
{
  queue_completion(
      {.ticket     = ticket,
       .stretch_ps = reply.stretch_ps,
       .read       = 1,
       .byte       = reply.byte,
       .error      = static_cast<std::uint8_t>(reply.error)},
      at, lane);
}

void i2c::queue_completion(i2c_completion record, vtime at, std::uint32_t lane)
{
  if(lane == 0)
  {
    throw std::invalid_argument("an I2C completion goes on a lane of 1 and up");
  }
  auto const offset = ctx_.clock.since_power_on().ps - ctx_.clock.time().ps;
  auto const since  = lane_due_ps(at.ps, offset);
  if(not since)
  {
    throw std::invalid_argument(
        std::format(
            "an I2C completion at {} ps since power-on falls before the last reset at {} ps", at.ps,
            offset));
  }
  record.at_ps = *since;
  state_.completions.push_back(record);
  ctx_.events.schedule_at(vtime{*since}, completed, record.ticket, lane, ctx_.clock);
}

void i2c::completion_due(std::uint64_t ticket)
{
  auto of_ticket = state_.completions | std::views::filter([ticket](i2c_completion const& record) {
                     return record.ticket == ticket;
                   });
  auto const found = std::ranges::min_element(of_ticket, {}, &i2c_completion::at_ps);
  if(found == of_ticket.end())
  {
    ++state_.refused;
    return;
  }
  auto const done = *found;
  state_.completions.erase(found.base());
  auto const reading = step() == phase::byte and state_.read != 0;
  auto const slot    = std::ranges::find_if(state_.answers, [&](i2c_pending_answer const& one) {
    return one.done == 0 and one.index == static_cast<std::uint32_t>(ticket);
  });
  if(state_.owed == 0
     or ticket >> 32 != state_.generation
     or slot == state_.answers.end()
     or (done.read != 0) != reading)
  {
    ++state_.refused;
    return;
  }
  slot->done       = 1;
  slot->ack        = done.ack;
  slot->byte       = done.byte;
  slot->error      = done.error;
  slot->at_ps      = done.at_ps;
  slot->stretch_ps = done.stretch_ps;
  if(--state_.owed == 0)
  {
    answered();
  }
}

void i2c::stop_done()
{
  transfer_ended();
  enter(phase::idle);
  update(regs_, stop_, 0);
  update(regs_, busy_, 0);
  state_.timer.idle_ps = state_.end_ps;
  stop_flag();
  state_.left = 0;
  stop_targets(target_at());
  if(state_.pending_waits != 0)
  {
    take_bus();
  }
  if(get(regs_, start_) != 0 and not bus_taken())
  {
    begin_start(state_.end_ps);
  }
}

void i2c::stop_targets(vtime at)
{
  for(auto const& answering : std::exchange(state_.targets, {}))
  {
    targets_[answering.index]->stop(at);
  }
  for(auto const& link : links_)
  {
    link.device->stop(at);
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
  transfer_ended();
  enter(phase::idle);
  update(regs_, stop_, 0);
  update(regs_, busy_, 0);
  state_.timer.idle_ps = state_.end_ps;
  stop_targets(target_at());
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
  state_.timer.idle_ps = until;
  if(enabled() and get(regs_, start_) != 0)
  {
    begin_start(until);
  }
}

void i2c::abort()
{
  if(auto* const held = held_entry(static_cast<std::uint8_t>(i2c_phase::address)))
  {
    release(*held, true);
  }
  if(auto* const held = held_entry(static_cast<std::uint8_t>(i2c_phase::write)))
  {
    release(*held, false);
  }
  if(auto* const held = held_entry(static_cast<std::uint8_t>(i2c_phase::read)))
  {
    begin_read(*held, 0xFF);
  }
  state_.tx = {};
  state_.answers.clear();
  state_.owed         = 0;
  state_.timeout_stop = 0;
  transfer_ended();
  state_.timer.idle_ps.reset();
  state_.timer.idle_spent = 0;
  state_.timer.gap_ps.reset();
  ++state_.generation;
  auto const lost = step() == phase::lost;
  enter(phase::idle);
  state_.left    = 0;
  state_.tx_full = 0;
  state_.rx_held = 0;
  if(lost)
  {
    stop_targets(vtime{ctx_.clock.since_power_on().ps});
  }
  else
  {
    release_links();
  }
  state_.targets.clear();
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
  release_links();
}

void i2c::restore_reset_values(std::optional<reset_kind> kind)
{
  if(state_.timer.armed_ps)
  {
    ctx_.events.cancel(state_.timer.seq);
  }
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
  auto const generation      = state_.generation + 1;
  auto const holds           = state_.holds;
  auto       port            = std::move(state_.port);
  auto       completions     = kind ? std::vector<i2c_completion>{} : std::move(state_.completions);
  auto const refused         = state_.refused;
  auto const released        = kind ? i2c_timer{} : state_.timer;
  state_                     = {};
  state_.generation          = generation;
  state_.holds               = holds;
  state_.completions         = std::move(completions);
  state_.refused             = refused;
  state_.timer.released_ps   = released.released_ps;
  state_.timer.released_unit = released.released_unit;
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
    for(auto const& link : links_)
    {
      link.device->reset(*kind);
    }
  }
  flags();
}

void i2c::power_cut()
{
  auto const         at = ctx_.clock.since_power_on();
  std::exception_ptr first;
  auto const         cut = [&](i2c_target& target) {
    try
    {
      target.power_cut(at);
    }
    catch(...)
    {
      if(not first)
      {
        first = std::current_exception();
      }
    }
  };
  for(auto const& target : targets_)
  {
    cut(*target);
  }
  for(auto const& link : links_)
  {
    cut(*link.device);
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
  auto const asked   = sending and state_.tx_full == 0 and to_load() != 0;
  update(regs_, tx_interrupt_, asked or state_.tx.ask != 0 ? 1 : 0);
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
  arm_timeout();
}

auto i2c::unsupported() const -> bool
{
  return get(regs_, ten_bit_) != 0
      or std::ranges::any_of(unsupported_, [&](field_ref bits) { return get(regs_, bits) != 0; });
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
      if(get(regs_, reload_complete_) != 0 and count_.of(word) != 0)
      {
        target_reload();
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
  if(tx_interrupt_.of(now) != 0 and no_stretch() and enabled())
  {
    state_.tx.ask = 1;
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
  if(get(regs_, address_matched_) == 0)
  {
    if(auto* const held = held_entry(static_cast<std::uint8_t>(i2c_phase::address)))
    {
      release(*held, true);
      address_released();
    }
  }
}

void i2c::plain_written(std::uint32_t, std::uint32_t)
{}

void i2c::trigger_written(std::uint32_t, std::uint32_t)
{
  if(unsupported() and not std::exchange(unsupported_logged_, true))
  {
    warn("CR1", std::string{unsupported_text});
  }
}

void i2c::port_check(std::uint32_t lane, std::uint64_t unit) const
{
  check_port_entry(ctx_, desc_.name, controller_ != nullptr, state_.port, max_entries, lane, unit);
}

auto i2c::after_model_reset(std::uint64_t due_ps) const -> std::uint64_t
{
  return std::max(due_ps, state_.port.reset_ps.value_or(0));
}

void i2c::schedule_port(i2c_port_entry const& entry)
{
  stm32::schedule_port(ctx_, port_due, state_.port, entry);
}

void i2c::queue_port(i2c_port_entry entry)
{
  stm32::queue_port(ctx_, port_due, state_.port, entry);
}

void i2c::queue_phase(
    i2c_phase     phase,
    std::uint8_t  data,
    std::uint64_t phase_ps,
    vtime         at,
    std::uint32_t lane,
    std::uint64_t unit,
    bool          ack)
{
  port_check(lane, unit);
  queue_port(placed_phase(
      state_.port,
      {.unit      = unit,
       .phase_ps  = phase_ps,
       .posted_ps = at.ps,
       .lane      = lane,
       .phase     = static_cast<std::uint8_t>(phase),
       .data      = data,
       .ack       = static_cast<std::uint8_t>(ack ? 1 : 0)},
      link_controller_ != nullptr));
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
  queue_phase(i2c_phase::read, 0xFF, phase_ps, at, lane, unit, ack);
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

auto i2c::last_unit() const -> std::uint64_t
{
  return state_.port.last_unit;
}

auto i2c::queued() const -> std::size_t
{
  return state_.port.entries.size();
}

void i2c::run_port(std::uint32_t lane)
{
  auto const now = ctx_.clock.time().ps;
  for(;;)
  {
    auto&      entries = state_.port.entries;
    auto const holding = held_unit(entries);
    auto const pick =
        next_due<i2c_port_entry>(entries, lane, now, ctx_.clock, [&](auto const& entry) {
          return entry.kind == entry_held
              or (holding
                  and entry.unit > *holding
                  and (entry.kind == entry_phase or entry.kind == entry_stop));
        });
    if(not pick)
    {
      return;
    }
    auto const entry = entries[*pick];
    auto const phase = static_cast<i2c_phase>(entry.phase);
    switch(entry.kind)
    {
      case entry_phase:
        state_.timer.gap_ps.reset();
        phase_begun(entry);
        if(phase == i2c_phase::read and not read_begun(entries[*pick]))
        {
          entries[*pick].kind = entry_held;
          flags();
          break;
        }
        entries[*pick].kind   = entry_ending;
        entries[*pick].due_ps = entry.due_ps + entry.phase_ps;
        schedule_port(entries[*pick]);
        flags();
        break;
      case entry_ending:
      {
        auto const answer = phase_ended(entry);
        if(state_.addressed != 0)
        {
          state_.timer.gap_ps   = entry.due_ps;
          state_.timer.gap_unit = entry.unit;
        }
        if(not answer)
        {
          state_.port.entries[*pick].kind = entry_held;
          flags();
          break;
        }
        state_.port.entries.erase(state_.port.entries.begin() + static_cast<std::ptrdiff_t>(*pick));
        flags();
        controller_->answered(entry.unit, phase, *answer, vtime{entry.due_ps});
        break;
      }
      case entry_released:
        entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(*pick));
        flags();
        controller_->answered(entry.unit, phase, {.ack = entry.ack != 0}, vtime{entry.due_ps});
        break;
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

auto i2c::phase_ended(i2c_port_entry const& entry) -> std::optional<i2c_answer>
{
  switch(static_cast<i2c_phase>(entry.phase))
  {
    case i2c_phase::address:
    {
      auto const address = static_cast<std::uint8_t>(entry.data >> 1);
      auto const read    = entry.data & 1u;
      warn_target_unsupported();
      state_.tx = {.ask = state_.tx.ask};
      if(not enabled()
         or step() != phase::idle
         or state_.wire_busy == 0
         or not matches(address, read != 0))
      {
        state_.addressed = 0;
        return i2c_answer{.ack = false};
      }
      update(regs_, address_code_, address);
      update(regs_, target_direction_, read);
      update(regs_, nack_, 0);
      if(get(regs_, address_auto_clear_) == 0)
      {
        update(regs_, address_matched_, 1);
      }
      state_.addressed = 1;
      state_.involved  = 1;
      if(get(regs_, address_matched_) != 0 and not no_stretch())
      {
        return std::nullopt;
      }
      address_released();
      return i2c_answer{.ack = true};
    }
    case i2c_phase::write:
      if(state_.addressed == 0
         or get(regs_, target_direction_) != 0
         or not enabled()
         or step() != phase::idle)
      {
        return i2c_answer{.ack = false};
      }
      if(get(regs_, rx_full_) != 0)
      {
        if(no_stretch())
        {
          update(regs_, overrun_, 1);
          return i2c_answer{.ack = false};
        }
        return std::nullopt;
      }
      if(auto const ack = received(entry.data))
      {
        return i2c_answer{.ack = *ack};
      }
      return std::nullopt;
    case i2c_phase::read:
      return read_ended(entry);
  }
  return i2c_answer{};
}

auto i2c::received(std::uint8_t byte) -> std::optional<bool>
{
  regs_.poke(receive_.offset, byte);
  update(regs_, rx_full_, 1);
  if(not counted())
  {
    return std::nullopt;
  }
  return taken_ack();
}

auto i2c::taken_ack() -> bool
{
  auto const nack = get(regs_, nack_) != 0;
  update(regs_, nack_, 0);
  return not nack;
}

auto i2c::counted() -> bool
{
  if(not byte_control())
  {
    return true;
  }
  if(state_.tx.left != 0)
  {
    --state_.tx.left;
  }
  if(state_.tx.left == 0 and get(regs_, reload_) != 0)
  {
    update(regs_, reload_complete_, 1);
    return false;
  }
  return true;
}

auto i2c::no_stretch() const -> bool
{
  return get(regs_, no_stretch_) != 0;
}

auto i2c::byte_control() const -> bool
{
  return get(regs_, byte_control_) != 0 and not no_stretch();
}

void i2c::address_released()
{
  state_.tx.left = state_.tx.asks = get(regs_, count_);
  if(get(regs_, target_direction_) == 0)
  {
    return;
  }
  state_.tx.target = 1;
  state_.tx.first  = 1;
  load_point();
}

void i2c::ask_transmit()
{
  if(byte_control())
  {
    if(state_.tx.asks == 0)
    {
      return;
    }
    --state_.tx.asks;
  }
  state_.tx.ask = 1;
}

void i2c::load_point()
{
  if(state_.tx.target == 0)
  {
    return;
  }
  if(state_.tx_full != 0)
  {
    state_.tx.shift  = state_.tx_data;
    state_.tx.loaded = 1;
    state_.tx.wait   = 0;
    state_.tx_full   = 0;
    ask_transmit();
    return;
  }
  state_.tx.wait = 1;
  if(not no_stretch())
  {
    ask_transmit();
  }
}

auto i2c::read_begun(i2c_port_entry& entry) -> bool
{
  if(state_.tx.target == 0 or not enabled())
  {
    return true;
  }
  if(std::exchange(state_.tx.first, 0) != 0 and no_stretch() and get(regs_, stop_detected_) != 0)
  {
    update(regs_, overrun_, 1);
  }
  if(state_.tx.loaded != 0)
  {
    entry.data       = state_.tx.shift;
    state_.tx.loaded = 0;
    return true;
  }
  if(no_stretch())
  {
    update(regs_, overrun_, 1);
    state_.tx.wait = 0;
    return true;
  }
  return false;
}

auto i2c::read_ended(i2c_port_entry const& entry) -> i2c_answer
{
  auto const ack = entry.ack != 0;
  if(state_.tx.target == 0 or not enabled())
  {
    return {.ack = ack};
  }
  if(not ack)
  {
    update(regs_, nack_received_, 1);
    state_.tx = {.ask = state_.tx.ask};
    return {.ack = ack, .byte = entry.data};
  }
  if(counted())
  {
    load_point();
  }
  return {.ack = ack, .byte = entry.data};
}

void i2c::begin_read(i2c_port_entry& entry, std::uint8_t byte)
{
  stretch_ended(entry.due_ps);
  auto const now  = ctx_.clock.time().ps;
  entry.kind      = entry_ending;
  entry.data      = byte;
  entry.due_ps    = now + entry.phase_ps;
  auto const unit = entry.unit;
  auto const end  = entry.due_ps;
  schedule_port(entry);
  for(auto const index : place_after(state_.port.entries, unit, end))
  {
    schedule_port(state_.port.entries[index]);
  }
}

void i2c::resume_read()
{
  if(state_.tx.loaded == 0)
  {
    return;
  }
  if(auto* const held = held_entry(static_cast<std::uint8_t>(i2c_phase::read)))
  {
    state_.tx.loaded = 0;
    begin_read(*held, state_.tx.shift);
  }
}

void i2c::target_reload()
{
  update(regs_, reload_complete_, 0);
  state_.tx.left = state_.tx.asks = get(regs_, count_);
  if(auto* const held = held_entry(static_cast<std::uint8_t>(i2c_phase::write)))
  {
    release(*held, taken_ack());
  }
  load_point();
  resume_read();
}

auto i2c::held_entry(std::uint8_t phase) -> i2c_port_entry*
{
  auto const found = std::ranges::find_if(state_.port.entries, [phase](auto const& entry) {
    return entry.kind == entry_held and entry.phase == phase;
  });
  return found != state_.port.entries.end() ? &*found : nullptr;
}

void i2c::release(i2c_port_entry& entry, bool ack)
{
  stretch_ended(entry.due_ps);
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
    update(regs_, nack_, 0);
    if(misplaced)
    {
      update(regs_, bus_error_, 1);
    }
    if(state_.involved != 0)
    {
      stop_flag();
    }
  }
  target_transfer_ended(entry.due_ps);
  std::ranges::sort(reports);
  flags();
  for(auto const& [unit, phase] : reports)
  {
    controller_->dropped(unit, phase, vtime{entry.due_ps});
  }
}

void i2c::target_transfer_ended(std::uint64_t at_ps)
{
  if(enabled() and state_.wire_busy != 0 and not held())
  {
    update(regs_, busy_, 0);
    state_.timer.idle_ps = at_ps;
  }
  auto const waited = state_.wire_busy != 0;
  if(step() == phase::idle)
  {
    transfer_ended();
  }
  state_.tx        = {.ask = state_.tx.ask};
  state_.addressed = 0;
  state_.involved  = 0;
  state_.wire_busy = 0;
  state_.timer.gap_ps.reset();
  state_.timer.released_ps.reset();
  if(waited and enabled() and step() == phase::idle and get(regs_, start_) != 0 and not held())
  {
    begin_start(ctx_.clock.time().ps);
  }
}

void i2c::rearm_port(i2c_port port, std::optional<reset_kind> kind)
{
  auto const now   = ctx_.clock.time().ps;
  auto const shift = kind ? std::exchange(reset_at_, std::nullopt).value_or(0) : 0;
  auto const moved = [&](std::uint64_t due) {
    return std::max(now, due > shift ? due - shift : 0);
  };
  auto const restart = not kind and link_controller_;
  if(auto const holding = held_unit(port.entries))
  {
    auto const floor = kind ? shift : now;
    for(auto& entry : port.entries)
    {
      if(entry.kind == entry_held)
      {
        entry.due_ps = std::max(entry.due_ps, floor);
        if(static_cast<i2c_phase>(entry.phase) == i2c_phase::read)
        {
          entry.kind = entry_phase;
          if(restart)
          {
            entry.due_ps += entry.phase_ps;
            entry.kind = entry_ending;
          }
        }
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
    if(entry.kind == entry_phase and (kind or (not restart and entry.due_ps <= now)))
    {
      entry.due_ps += entry.phase_ps;
      entry.kind = entry_ending;
    }
    if(entry.kind == entry_ending)
    {
      if(not link_controller_)
      {
        entry.wire_end_ps = kind ? moved(entry.due_ps) : entry.due_ps;
      }
      entry.due_ps = moved(entry.due_ps);
      entry.kind   = entry_dropped;
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
  if(restart)
  {
    for(auto& entry : port.entries)
    {
      if(entry.kind != entry_phase)
      {
        continue;
      }
      auto const start = phase_start(port.entries, entry.posted_ps, entry.unit);
      if(start <= now)
      {
        entry.due_ps = std::max(now, start + entry.phase_ps);
        entry.kind   = entry_dropped;
      }
      else
      {
        entry.due_ps = start;
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

auto i2c::link_of(this auto& self, i2c_link_target const& target) -> auto&
{
  auto const found = std::ranges::find(
      self.links_, &target, [](i2c_link_attachment const& link) { return link.device.get(); });
  if(found == self.links_.end())
  {
    throw std::invalid_argument(
        std::format("the target is not a link target attached to {}", self.desc_.name));
  }
  return *found;
}

void i2c::attach_target(std::shared_ptr<i2c_link_target> target)
{
  auto const by_device = [](i2c_link_attachment const& link) { return link.device.get(); };
  if(not target)
  {
    throw std::invalid_argument("an I2C link target is a target");
  }
  if(std::ranges::contains(links_, target.get(), by_device))
  {
    throw std::invalid_argument(
        std::format("the link target is attached to {} already", desc_.name));
  }
  if(controller_)
  {
    throw config_error(
        ctx_.part.part, 0, std::format("links.{}", desc_.name),
        "an I2C instance with no controller, for a link target");
  }
  links_.push_back({.device = target});
  try
  {
    target->linked(*this, ctx_.clock);
    target->attached(*this);
  }
  catch(...)
  {
    std::erase_if(links_, [&](i2c_link_attachment const& one) { return one.device == target; });
    throw;
  }
}

void i2c::detach_target(i2c_link_target& target) noexcept
{
  auto const by_device = [](i2c_link_attachment const& link) { return link.device.get(); };
  auto const found     = std::ranges::find(links_, &target, by_device);
  if(found == links_.end())
  {
    return;
  }
  auto const device = found->device;
  device->detached();
  links_.erase(std::ranges::find(links_, device.get(), by_device));
}

void i2c::attach_controller(std::shared_ptr<i2c_link_controller> controller)
{
  if(not controller)
  {
    throw std::invalid_argument("an I2C link controller is a controller");
  }
  if(link_controller_)
  {
    throw std::invalid_argument(
        std::format("a link controller is attached to {} already", desc_.name));
  }
  if(controller_ or not targets_.empty() or not links_.empty())
  {
    throw config_error(
        ctx_.part.part, 0, std::format("links.{}", desc_.name),
        "an I2C instance with no board controller, no board target and no link target, for a "
        "link controller");
  }
  auto const last_unit = std::exchange(state_.port.last_unit, 0);
  controller_          = controller;
  link_controller_     = controller;
  try
  {
    controller->linked(*this, ctx_.clock);
  }
  catch(...)
  {
    controller_.reset();
    link_controller_.reset();
    state_.port.last_unit = last_unit;
    throw;
  }
}

void i2c::detach_controller(i2c_link_controller& controller) noexcept
{
  if(link_controller_.get() != &controller)
  {
    return;
  }
  auto& port = state_.port;
  if(not port.entries.empty())
  {
    port.entries.clear();
    ++port.generation;
  }
  port.last_unit = 0;
  target_transfer_ended(ctx_.clock.time().ps);
  controller_.reset();
  auto const gone = std::move(link_controller_);
  flags();
  gone->unlinked();
}

void i2c::mute(i2c_link_target const& target)
{
  link_of(target).muted = true;
}

void i2c::unmute(i2c_link_target const& target)
{
  link_of(target).muted = false;
}

auto i2c::muted(i2c_link_target const& target) const -> bool
{
  return link_of(target).muted;
}

auto i2c::phase_time() const -> std::uint64_t
{
  return state_.phase_ps;
}

void i2c::bus_released(vtime at, std::uint64_t unit)
{
  auto& timer         = state_.timer;
  timer.released_ps   = at.ps;
  timer.released_unit = unit;
  arm_timeout();
}

auto i2c::board_use() const -> i2c_board_use
{
  return {
      .controller = controller_ != nullptr and link_controller_ == nullptr,
      .targets    = not targets_.empty()};
}

void i2c::release_links()
{
  auto const at = vtime{ctx_.clock.since_power_on().ps};
  for(auto const& link : links_)
  {
    link.device->bus_released(at);
  }
}

void i2c::warn_target_unsupported()
{
  auto const ten_bit = get(regs_, own1_enable_) != 0 and get(regs_, own1_mode_) != 0;
  auto const both    = get(regs_, byte_control_) != 0 and no_stretch();
  if(ten_bit or both or std::ranges::any_of(target_unsupported_, [&](field_ref bits) {
       return get(regs_, bits) != 0;
     }))
  {
    unmodelled(
        ten_bit ? "OAR1" : "CR1", target_logged_,
        "a 10-bit own address, SBC with NOSTRETCH, the SMBus host or device address, ALERT or PEC "
        "in target mode: not modelled, the I2C matches 7-bit addresses only, without target "
        "byte control");
  }
}

auto i2c::cycles_ps(std::uint64_t cycles) const -> std::optional<std::uint64_t>
{
  auto const hz = ctx_.clock.hz(kernel_);
  if(hz == 0)
  {
    return std::nullopt;
  }
  return cycles * ps_per_second / hz;
}

auto i2c::held_since() const -> std::optional<std::uint64_t>
{
  auto const unit = held_unit(state_.port.entries);
  if(not unit)
  {
    return std::nullopt;
  }
  auto const found = std::ranges::find(state_.port.entries, *unit, &i2c_port_entry::unit);
  return found->due_ps;
}

auto i2c::expiry_due() const -> std::optional<i2c_expiry>
{
  auto const& timer = state_.timer;
  if(not enabled())
  {
    return std::nullopt;
  }
  std::optional<i2c_expiry> out;
  auto const                keep = [&](std::uint64_t at, expiry kind) {
    if(not out or at < out->at)
    {
      out = i2c_expiry{.at = at, .kind = kind};
    }
  };
  auto const held = held_since();
  if(get(regs_, timeout_enable_) != 0)
  {
    auto const units = std::uint64_t{get(regs_, timeout_a_)} + 1;
    if(get(regs_, idle_mode_) != 0)
    {
      auto const length = cycles_ps(units * idle_cycles);
      if(length and timer.idle_ps)
      {
        keep(std::max(*timer.idle_ps, timer.a_from_ps) + *length, expiry::idle);
      }
    }
    else if(auto const length = cycles_ps(units * low_cycles))
    {
      if(held)
      {
        keep(std::max(*held, timer.a_from_ps) + *length, expiry::low);
      }
      else if(timer.gap_ps)
      {
        auto const at       = std::max(*timer.gap_ps, timer.a_from_ps) + *length;
        auto const released = timer.released_ps
                          and timer.gap_unit <= timer.released_unit
                          and *timer.released_ps <= at;
        auto const rose =
            released or std::ranges::any_of(state_.port.entries, [at](auto const& entry) {
              return (entry.kind == entry_phase or entry.kind == entry_stop) and entry.due_ps <= at;
            });
        if(not rose)
        {
          keep(at, expiry::low);
        }
      }
      else if(timer.low_ps)
      {
        auto const at = std::max(*timer.low_ps, timer.a_from_ps) + *length;
        if(not timer.low_end_ps or *timer.low_end_ps > at)
        {
          keep(at, expiry::low);
        }
      }
    }
  }
  if(get(regs_, extend_enable_) != 0)
  {
    auto const own    = held ? held : timer.own_ps;
    auto const length = cycles_ps((std::uint64_t{get(regs_, timeout_b_)} + 1) * low_cycles);
    if(own and length)
    {
      auto const left = *length > timer.extended_ps ? *length - timer.extended_ps : 0;
      keep(std::max(*own, timer.b_from_ps) + left, expiry::extend);
    }
  }
  return out;
}

void i2c::arm_timeout()
{
  auto&      timer = state_.timer;
  auto const now   = ctx_.clock.time().ps;
  if(enabled() and get(regs_, busy_) == 0)
  {
    if(not timer.idle_ps and timer.idle_spent == 0)
    {
      timer.idle_ps = now;
    }
  }
  else
  {
    timer.idle_ps.reset();
    timer.idle_spent = 0;
  }
  auto const due = expiry_due();
  auto const at  = due ? std::optional{due->at} : std::nullopt;
  if(at == timer.armed_ps)
  {
    return;
  }
  if(timer.armed_ps)
  {
    ctx_.events.cancel(timer.seq);
  }
  timer.armed_ps = at;
  if(at)
  {
    timer.seq = ctx_.events.schedule_at(vtime{std::max(*at, now)}, timeout_due, 0, ctx_.clock);
  }
}

void i2c::stretch_ended(std::uint64_t since)
{
  auto&      timer = state_.timer;
  auto const now   = ctx_.clock.time().ps;
  auto const from  = std::max(since, timer.b_from_ps);
  if(get(regs_, extend_enable_) != 0 and now > from)
  {
    timer.extended_ps += now - from;
  }
}

void i2c::transfer_ended()
{
  auto& timer = state_.timer;
  timer.low_ps.reset();
  timer.low_end_ps.reset();
  timer.own_ps.reset();
  timer.extended_ps = 0;
}

void i2c::timed_out()
{
  auto&      timer = state_.timer;
  auto const now   = ctx_.clock.time().ps;
  timer.armed_ps.reset();
  auto const due = expiry_due();
  if(not due or due->at > now)
  {
    return;
  }
  update(regs_, timeout_flag_, 1);
  if(due->kind == expiry::idle)
  {
    timer.idle_ps.reset();
    timer.idle_spent = 1;
    return;
  }
  if(held_unit(state_.port.entries))
  {
    if(auto* const held = held_entry(static_cast<std::uint8_t>(i2c_phase::address)))
    {
      release(*held, true);
    }
    if(auto* const held = held_entry(static_cast<std::uint8_t>(i2c_phase::write)))
    {
      release(*held, false);
    }
    if(auto* const held = held_entry(static_cast<std::uint8_t>(i2c_phase::read)))
    {
      begin_read(*held, 0xFF);
    }
    state_.addressed = 0;
    state_.tx        = {.ask = state_.tx.ask};
    timer.gap_ps.reset();
  }
  else if(timer.gap_ps)
  {
    state_.addressed = 0;
    state_.tx        = {.ask = state_.tx.ask};
    timer.gap_ps.reset();
  }
  else if(state_.owed != 0)
  {
    state_.timeout_stop = 1;
  }
  else
  {
    stop_on_timeout(timer.low_end_ps ? std::max(now, *timer.low_end_ps) : released());
  }
  timer.low_ps.reset();
  timer.low_end_ps.reset();
  if(due->kind == expiry::extend)
  {
    timer.extended_ps = 0;
  }
}

void i2c::timeout_written(std::uint32_t old, std::uint32_t now)
{
  auto const time = ctx_.clock.time().ps;
  if(timeout_enable_.of(old) == 0 and timeout_enable_.of(now) != 0)
  {
    state_.timer.a_from_ps  = time;
    state_.timer.idle_spent = 0;
  }
  if(extend_enable_.of(old) == 0 and extend_enable_.of(now) != 0)
  {
    state_.timer.b_from_ps   = time;
    state_.timer.extended_ps = 0;
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
    else if(
        auto* const held = held_entry(static_cast<std::uint8_t>(i2c_phase::write));
        held != nullptr and get(regs_, reload_complete_) == 0)
    {
      if(auto const ack = received(held->data))
      {
        release(*held, *ack);
      }
    }
  }
  flags();
  rethrow_strict();
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
      state_.tx.ask  = 0;
      regs_.poke(transmit_.offset, data);
      if(step() == phase::wait_tx)
      {
        next_byte(released());
      }
      if(state_.tx.wait != 0)
      {
        load_point();
        resume_read();
      }
    }
    flags();
    rethrow_strict();
    return {};
  }
  auto const word    = offset & ~3u;
  auto const timed   = word == timeout_reg_.offset;
  auto const control = word == control_.offset;
  auto const kept =
      control ? (enabled() ? fixed_while_enabled_ : 0u)
      : word == own1_reg_.offset and get(regs_, own1_enable_) != 0
          ? own1_.mask() | own1_mode_.mask()
      : word == own2_reg_.offset and get(regs_, own2_enable_) != 0
          ? own2_.mask() | own2_mask_.mask()
      : timed ? (get(regs_, timeout_enable_) != 0 ? timeout_a_.mask() | idle_mode_.mask() : 0u)
                    | (get(regs_, extend_enable_) != 0 ? timeout_b_.mask() : 0u)
              : 0u;
  auto const before = regs_.peek(word);
  auto const result = dispatch_model::on_write(regs, offset, size, old, value, access);
  auto const now    = regs_.peek(word);
  if(((now ^ before) & kept) != 0)
  {
    regs_.poke(word, (now & ~kept) | (before & kept));
    if(access.by != initiator::debugger and timed)
    {
      if(not std::exchange(timeout_protect_logged_, true))
      {
        warn(
            "TIMEOUTR",
            std::format(
                "{} TIMEOUTR written while TIMOUTEN or TEXTEN is set, TIMEOUTA and TIDLE are "
                "kept while TIMOUTEN is set and TIMEOUTB while TEXTEN is set, RM0456 65.9.6, "
                "RM0481 53.9.6",
                desc_.name));
      }
      if(ctx_.strict_peripherals)
      {
        throw model_error(desc_.name, "TIMEOUTR", word);
      }
    }
    else if(access.by != initiator::debugger and control)
    {
      if(not std::exchange(control_protect_logged_, true))
      {
        warn(
            "CR1", std::format(
                       "{} CR1 written while PE is set, NOSTRETCH, ANFOFF and DNF are kept, "
                       "RM0456 65.9.1, RM0481 53.9.1",
                       desc_.name));
      }
      if(ctx_.strict_peripherals)
      {
        throw model_error(desc_.name, "CR1", word);
      }
    }
    else if(access.by != initiator::debugger)
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
  rethrow_strict();
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
  if(kind == timeout_due)
  {
    timed_out();
    flags();
    return;
  }
  if(kind == completed)
  {
    completion_due(payload);
    flags();
    rethrow_strict();
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
    rethrow_strict();
    return;
  }
  if(kind != phase_done or payload != state_.generation)
  {
    return;
  }
  if(step() == phase::address or step() == phase::byte)
  {
    state_.timer.low_ps = state_.end_ps;
    state_.timer.low_end_ps.reset();
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
  rethrow_strict();
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
  auto const needed = std::ranges::fold_left(
      loaded.targets | std::views::transform([](i2c_answering const& answering) {
        return std::size_t{answering.index} + 1;
      }),
      std::size_t{}, [](std::size_t one, std::size_t other) { return std::max(one, other); });
  if(needed > targets_.size())
  {
    throw config_error(
        ctx_.part.part, 0, std::format("devices.{}", desc_.name),
        std::format(
            "at least {} board targets attached to the instance, the snapshot's transfer names "
            "target {}",
            needed, needed - 1));
  }
  auto const answering = targets_.size() + links_.size();
  if(std::ranges::any_of(loaded.answers, [answering](i2c_pending_answer const& slot) {
       return slot.index >= answering;
     }))
  {
    throw config_error(
        ctx_.part.part, 0, std::format("devices.{}", desc_.name),
        std::format(
            "the targets the snapshot's held phase waits for, {} attached to the instance",
            answering));
  }
  state_ = std::move(loaded);
  for(std::size_t index = 0; index < targets_.size() and index < state_.devices.size(); ++index)
  {
    targets_[index]->load(state_.devices[index]);
  }
  state_.devices.clear();
}
}

void i2c_link_target::bus_released(vtime)
{}

i2c_link_target::~i2c_link_target()         = default;
i2c_link_controller::~i2c_link_controller() = default;
i2c_link_port::~i2c_link_port()             = default;

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
