#include "dma.hpp"
#include "dma_signal.hpp"
#include "hook_failure.hpp"
#include "i2c_port_queue.hpp"
#include "i2c_rows.hpp"
#include "model_base.hpp"
#include "target_port.hpp"

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
constexpr std::uint64_t hz_per_mhz   = 1'000'000;
constexpr std::uint32_t phase_done   = 0;
constexpr std::uint32_t port_due     = 1;
constexpr std::uint32_t byte_periods = 9;
constexpr std::uint32_t data_periods = 8;

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

struct i2c_l1_state
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
  std::uint8_t                        matched{};
  std::uint8_t                        last_ack{};
  std::uint8_t                        wire_busy{};
  std::uint8_t                        disabling{};
  std::uint8_t                        tx_active{};
  std::uint8_t                        tx_loaded{};
  std::uint8_t                        tx_sending{};
  std::uint8_t                        tx_shift{};
  std::uint8_t                        dma_last{};
  std::uint64_t                       phase_end_ps{};
  std::uint64_t                       released_ps{};
  std::vector<std::uint8_t>           acked;
  std::vector<std::uint8_t>           in_transfer;
  std::vector<std::vector<std::byte>> targets;
  i2c_port                            port;

  friend auto operator==(i2c_l1_state const&, i2c_l1_state const&) -> bool = default;
};

struct answers
{
  bool          ack{};
  std::uint8_t  byte{0xFF};
  i2c_error     error{};
  std::uint64_t stretch_ps{};

  void note(i2c_error first, std::uint64_t stretch)
  {
    if(error == i2c_error::none)
    {
      error = first;
    }
    stretch_ps = std::max(stretch_ps, stretch);
  }
};

auto kernel_name(peripheral_desc const& desc, i2c_l1_variant const& variant) -> std::string
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
 *        Controller transmitter, RM0038 26.3.3 and Figure 212: CR1.START with PE 1 sends a START,
 *        BUSY 1 from its start. At its end SB, MSL and BUSY are 1 and START is cleared. A DR write
 *        after an SR1 read clears SB and sends DR as the address byte, EV5. At the end of the
 *        address byte every target is asked i2c_target::matches(), in attachment order, and those
 *        it matches answer through i2c_target::start_deferred(): one acknowledge sets ADDR, TRA
 *        follows the R/W bit, and with none AF is set. The I2C keeps SCL low until an SR2 read
 *        after an SR1 read clears ADDR, EV6, as RM0038 26.6.7 says, an SR1 read before ADDR is set
 *        included. TxE then sets. The first DR write goes to the shift register with TxE kept 1,
 *        EV8_1, and each byte reaches the targets that acknowledged the address through
 *        i2c_target::write_deferred() at its end. A DR write while a byte shifts fills DR and
 *        clears TxE, EV8, and at the acknowledge DR moves to the shift register with TxE 1. A byte
 *        acknowledged with DR empty sets BTF and SCL stays low until a DR write, which clears BTF
 *        and starts the byte with TxE kept 1. A DR read clears BTF too. A NACK sets AF, neither TxE
 *        nor BTF. CR1.STOP sends a STOP at once while SB is set or SCL is held after a byte or a
 *        NACK, after the current byte while one shifts, and after the ADDR clear while ADDR is set.
 *        CR1.START with MSL 1 sends a repeated START by the same rule. The STOP's end clears STOP,
 *        BUSY, MSL, TRA, TxE and BTF and reaches every target an address of the transfer matched
 *        through i2c_target::stop(). A repeated START clears TRA, TxE and BTF. A target's
 *        stretch_ps holds SCL after its byte: the next phase starts at the byte's end plus the
 *        longest stretch. Targets share the open-drain bus: one acknowledge acknowledges. An SR1
 *        read arms both clear sequences until the SR2 read that clears ADDR or the DR write that
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
 *        DMA, RM0038 26.3.7: with CR2.DMAEN 1 the `<instance>_tx_dma` request follows TxE and the
 *        `<instance>_rx_dma` request follows RxNE, in every mode and whatever ITBUFEN holds. A DMA
 *        access to DR acts as a CPU access. When the channel serving the receive request moves the
 *        last but one item of its count, the EOT_1 signal, with CR2.LAST 1, the controller
 *        receiver NACKs the next byte it decides, RM0038 26.3.7 "I2C automatically sends a NACK
 *        after the next byte following EOT_1". Inferred: LAST is read at EOT_1, the byte is the
 *        first whose acknowledge the I2C decides after it, the end of a START drops a pending
 *        EOT_1, so none reaches the next transfer, an EOT_1 with DMAEN 0 changes nothing, since
 *        RM0038 26.3.7 and 26.3.8 name EOT_1 for DMA mode only, and LAST has no effect in target
 *        mode, its CR2 note naming the master receiver only. The DMA end of transfer, EOT, changes
 *        nothing: the firmware sets STOP.
 *
 *        Errors, RM0038 26.3.4: a target's reply carries an i2c_error for its byte, and with
 *        several targets the byte takes the error of the first in attachment order whose reply
 *        carries one. bus_error sets BERR and overrun sets OVR at the end of the byte, which
 *        completes as its acknowledge and stretch say, RM0038 26.3.4 BERR "In Master mode: the
 *        lines are not released and the state of the current transmission is not affected". OVR
 *        exists on silicon in slave mode with NOSTRETCH only, the injection exercises firmware
 *        handlers. arbitration_lost sets ARLO and clears MSL, RM0038 26.6.7 MSL "Cleared by
 *        hardware after detecting ... a loss of arbitration", and TRA, RM0038 26.6.7 TRA, and the
 *        reply's acknowledge and stretch are ignored, as i2c_reply says: no ADDR, AF or RxNE
 *        follows, a read byte is dropped with DR unchanged, and DR bytes the firmware wrote are
 *        not sent, RM0038 26.3.4 ARLO "the I2C Interface goes automatically back to slave
 *        mode" and "lines are released by hardware". Inferred: TxE clears, and BTF follows a
 *        byte still held, as at a START end. Inferred: BUSY stays 1 for one SCL period, then the
 *        winner's STOP reaches every target an address of the transfer matched through
 *        i2c_target::stop(), clears BUSY and CR1.STOP, RM0038 26.6.1 STOP "cleared by hardware
 *        when a Stop condition is detected", and sends a START CR1.START holds, RM0038 26.6.1
 *        START "Start generation when the bus is free". PE 0 in that window clears ARLO and the
 *        bits PE 0 clears and leaves BUSY 1 to the winner's STOP, RM0038 26.6.7 BUSY "still
 *        updated when the interface is disabled". In target mode a port STOP that ends a running
 *        data phase of the I2C's acknowledged address, or a running address phase while the I2C is
 *        enabled, whatever the address, sets BERR, RM0038 26.3.4 BERR "an external Stop or Start
 *        condition during an address or a data transfer" and "in case of a misplaced Stop, the
 *        slave behaves like for a Stop condition", and the byte is dropped. Inferred: a STOP inside
 *        a byte of another address's transfer, or before a queued phase starts, sets no BERR.
 *
 *        SMBus, PEC, ALERT and ARP log one model_warning per instance when the firmware uses them,
 *        raise model_error under strict_peripherals at the START that uses them, and the bus stays
 *        idle for them. ADD10 stays 0: no i2c_target answers a 10-bit header, so a header address
 *        byte takes the NACK path and reaches no i2c_target::matches().
 *
 *        Interrupts, RM0038 Table 139 and Figure 214: the event line follows SB, ADDR, ADD10, STOPF
 *        and BTF with CR2.ITEVTEN, TxE and RxNE with ITEVTEN and ITBUFEN, the error line BERR,
 *        ARLO, AF, OVR, PECERR, TIMEOUT and SMBALERT with ITERREN. SR1's error flags clear by a
 *        write of 0. PE 0 outside a target transfer ends the I2C's transfer at once and clears
 *        START, STOP, ACK, POS, PEC and ALERT, every SR1 flag and SR2's MSL, TRA, DUALF and
 *        GENCALL, and BUSY unless the bus is busy. PE 0 during a controller transfer, which RM0038
 *        26.6.1 forbids, logs one model_warning per instance. CR1.SWRST 1 resets the I2C as an RCC
 *        reset does and keeps SWRST 1. While SWRST is 1 a CR1 write changes nothing but the SWRST
 *        clear, which leaves CR1 0. Neither tells the targets. Debugger accesses to SR1, SR2 and DR
 *        have no side effect and a debugger DR write sends nothing.
 *
 *        Controller port: a controller of board::i2c_controllers, alone on the instance with no
 *        board target, queues i2c_controller_port entries on the lane it names, each with a unit
 *        above the last one. The port runs the entries of one instruction count in unit order. A
 *        phase starts at the latest of its time, the end of the phases queued before it and the
 *        time of the STOPs queued before it, and is answered at its end, or at the release of the
 *        I2C's stretch, as the target rules below say. A STOP drops the phases of lower units still
 *        queued or running, each reported at its time. An RCC reset, CR1.SWRST included, drops the
 *        phases started at or before it, each reported at the later of its end and the reset, and
 *        keeps the later phases and STOPs. A phase queued after it with a start at or before it is
 *        dropped the same way, and a STOP or a drop queued after it is due no earlier than it. A
 *        machine reset of any kind, the model a reset_listener, drops every queued phase at its end
 *        rebased by the reset's local time and removes every queued STOP. A phase a reset drops
 *        holds the wire to its end. Dropped units survive every reset and are reported once,
 *        through i2c_controller_sink::dropped(). The entries, the last unit, the generation and the
 *        time of the last RCC reset are saved with the model, and an instance with no controller
 *        refuses a snapshot holding entries with config_error.
 *
 *        Target receiver and transmitter, RM0038 26.3.2, Figures 210 and 211: an address phase sets
 *        BUSY at its start, with PE 0 too, as RM0038 26.6.7 BUSY says, and a STOP of the port
 *        clears it, the I2C's own STOP keeping it while the port's transfer runs. At the end of an
 *        address phase, while the I2C is enabled and runs no controller transfer, the address
 *        matches, read at that time, OAR1 with ADDMODE 0, OAR2 ADD2 with ENDUAL 1 and ADDMODE 0,
 *        RM0038 26.6.4 "Both OAR1 and OAR2 are recognized in 7-bit addressing mode", and the
 *        general call, address 0 with R/W 0, with ENGC 1 in either ADDMODE, RM0038 26.3.2. An
 *        address equal to OAR1 and to OAR2 matches OAR1, inferred. Address 0 never matches OAR1 nor
 *        OAR2, RM0038 26.6.1 ENGC "Address 00h is NACKed". Address 0 with R/W 1, the I2C-bus START
 *        byte, matches nothing, inferred. A match with CR1.ACK 1 sets ADDR with TRA as the R/W bit,
 *        DUALF 1 for OAR2 and GENCALL 1 for the general call, and is acknowledged. A STOP, a
 *        repeated START and the disable clear DUALF and GENCALL, RM0038 26.6.7. SCL stays held,
 *        EV1: the answer reaches the controller once an SR2 read after an SR1 read clears ADDR, and
 *        every later phase and STOP waits for it. A match with CR1.ACK 0 answers NACK at the
 *        address end and sets no ADDR, inferred, the ST HAL clearing ACK to stop listening. Any
 *        other address phase answers NACK. A written byte after an acknowledged write address lands
 *        in DR and sets RxNE, EV2. Popcorn reads CR1.ACK for it at the end of its phase, the
 *        acknowledge period included. A byte that ends with RxNE 1 stays in the shift register with
 *        its acknowledge, sets BTF and holds SCL: a DR read moves it to DR, keeps RxNE 1, clears
 *        BTF and releases SCL. A written byte with no acknowledged write address answers NACK.
 *        After an acknowledged read address the ADDR clear sets TxE, EV3-1. A DR write with the
 *        shift register empty and no byte sending loads the shift register and keeps TxE 1, as the
 *        RM0038 26.6.6 TxE note says, and starts a read byte the I2C holds for it. A DR write with
 *        the shift register loaded or a byte sending fills DR and clears TxE, EV3. A read byte
 *        starts with the shift register loaded, or holds SCL until that DR write, EV1 note 1. At
 *        the end of a byte the controller acknowledges, DR moves to the shift register when full,
 *        TxE sets, and with DR empty BTF sets at the byte's end, RM0038 26.3.2 "before the end of
 *        the next data transmission, the BTF bit is set", so the next read byte waits for the DR
 *        write that clears BTF. The first read byte after the ADDR clear sets no BTF, inferred. A
 *        read byte the controller NACKs sets AF, not TxE nor BTF, and ends the transmission, and a
 *        DR write after it until the STOP clears TxE and sends nothing, RM0038 26.6.6 TxE "Cleared
 *        by software writing to the DR register". A read byte with no transmission or after a NACK
 *        answers 0xFF. A STOP clears TRA and sets STOPF when the last acknowledge of the transfer,
 *        of the address or of a byte, was an ACK, RM0038 26.6.6 STOPF "after an acknowledge (if
 *        ACK=1)" and "not set after a NACK reception", so no STOPF after a byte the I2C or the
 *        controller NACKed. A STOP or a repeated START clears TxE and BTF of a transmission, RM0038
 *        26.6.6 TxE. An SR1 read then a CR1 write clears STOPF, EV4, an SR1 read before STOPF sets
 *        arming the clear as RM0038 26.6.7 says for ADDR, inferred. A START the firmware sets while
 *        the bus is busy goes at the STOP, RM0038 26.6.1 START. A repeated START clears TRA. PE 0
 *        written between a match of the I2C's address and the port's STOP disables it at that STOP,
 *        RM0038 26.6.1 PE "disabled at the end of the current communication" and "All bit resets
 *        due to PE=0 occur at the end of the communication": until then the I2C answers, stretches
 *        and takes DR accesses as with PE 1, and the STOP clears the bits PE 0 clears. PE 0 with no
 *        address of the I2C matched since the last STOP disables it at once. Inferred: the
 *        communication runs from the match to the port's STOP, repeated STARTs and their address
 *        phases included, a repeated START inside it matches the I2C's addresses, PE 0 with no
 *        address matched is outside it, the STOP that ends it sets no STOPF, and a PE 1 write
 *        before that STOP keeps the I2C enabled. An RCC reset starts a read byte held for DR,
 *        dropped at its end, and a machine reset drops it one phase after the reset in the new
 *        base. A 10-bit header 0b11110xx with ADDMODE 1 logs one model_warning per instance, raises
 *        model_error under strict_peripherals at that address phase, and answers NACK.
 *
 *        Target without clock stretching, CR1.NOSTRETCH 1, read at each decision, RM0038 26.3.4
 *        and 26.3.5: an address match answers at the address end with ADDR set and holds no SCL,
 *        and a read address starts the transmission at once. A written byte that ends with RxNE 1
 *        is lost and sets OVR, DR keeps its byte and BTF stays 0, RM0038 26.6.6 OVR "New received
 *        byte is lost" and BTF "Set by hardware when NOSTRETCH=0". A read byte that starts with the
 *        shift register empty sends the DR byte again and sets OVR, RM0038 26.6.6 OVR "The same
 *        byte is sent twice", and BTF stays 0. Inferred: the lost byte is acknowledged as CR1.ACK
 *        says, a read byte before the ADDR clear sends DR as it is, the acknowledge of a read byte
 *        sets TxE only with ADDR clear, the ADDR clear sets TxE when DR is empty, and the ADDR
 *        clear after a read byte the controller NACKed sets no TxE. That NACK ends the
 *        transmission, RM0038 26.3.4 AF "If Slave: lines are released by hardware".
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
 *
 * @limits i2c
 *
 * @group l1 rule
 * - STM32L1 I2C1 and I2C2, RM0038 section 26, run as a 7-bit controller transmitter and receiver on
 *   board targets. The SCL period is Thigh + Tlow from CCR and PCLK1, RM0038 26.6.8, without the
 *   rise time and the noise filter delay: a START takes 1 period, a byte 9, a STOP 1. FREQ, CCR,
 *   TRISE and the PCLK1 rate are checked at the first START after PE is set with PCLK1 running, and
 *   each rule a configuration breaks logs one `model_warning`. TRISE passes at or above the rise
 *   time.
 * - SMBus, PEC, ALERT and ARP log one `model_warning`, raise `model_error` under
 *   `strict_peripherals` at the START that uses them, and the bus stays idle for them.
 * - DMA, RM0038 26.3.7: with CR2.DMAEN 1 the TX request follows TxE and the RX request follows
 *   RxNE, in controller and target mode and whatever ITBUFEN holds. A DMA access to DR acts as a
 *   CPU access. With CR2.LAST 1 at the EOT_1 of the channel serving the RX request, the controller
 *   receiver NACKs the next byte. Inferred: LAST counts as it is at EOT_1, the NACKed byte is the
 *   first whose acknowledge the I2C decides after EOT_1, the end of a START drops a pending EOT_1,
 *   an EOT_1 with DMAEN 0 changes nothing, and LAST has no effect in target mode. The end of the
 *   DMA transfer sends no STOP: the firmware sets it.
 * - A `board.i2c_controllers` controller, alone on the instance, queues phases through
 *   `i2c_controller_port` with the placement, unit order, STOP, drop, reset and snapshot rules of
 *   the STM32U5 I2C with a board controller.
 * - Target receiver and transmitter, RM0038 26.3.2: a 7-bit match of OAR1, of OAR2 with ENDUAL 1,
 *   or the general call with ENGC 1, with CR1.ACK 1, sets ADDR with TRA as the R/W bit, DUALF for
 *   OAR2 and GENCALL for the general call, and holds SCL until the SR1 then SR2 read. OAR2 matches
 *   with ADDMODE 0 only, the general call in either ADDMODE. Address 0 never matches OAR1 nor OAR2.
 *   A STOP, a repeated START and the disable clear DUALF and GENCALL. Written bytes land in DR with
 *   RxNE, CR1.ACK read at the end of the byte's phase, the acknowledge period included. A byte
 *   ending with RxNE 1 sets BTF and holds SCL until the DR read. A read byte sends the shift
 *   register, which the first DR write after the ADDR clear loads with TxE kept 1, and waits for
 *   that write. The acknowledge of a read byte moves a full DR on and sets TxE, and with DR empty
 *   BTF sets at the byte's end, RM0038 26.3.2. The controller's NACK sets AF, and a DR write after
 *   it clears TxE and sends nothing. BUSY follows the port's START and STOP with PE 0 too. A STOP
 *   sets STOPF after an acknowledged address or byte, none after a NACKed byte. Which acknowledge
 *   counts for that rule, the last one sent or CR1.ACK at the STOP, is inferred as the last one
 *   sent. PE 0 written between a match of an address of the I2C and the port's STOP disables it at
 *   that STOP, RM0038 26.6.1: until then it answers, stretches and takes DR accesses as with PE 1.
 *   Inferred: OAR1 matches before OAR2 for one address, PE 0 with no address matched since the last
 *   STOP disables the I2C at once, a match with CR1.ACK 0 answers NACK at the address end with no
 *   ADDR, address 0 with R/W 1 matches nothing, an SR1 read before STOPF sets arms its clear by a
 *   CR1 write, the first read byte after the ADDR clear sets no BTF, the communication PE 0 waits
 *   for runs from the match to the port's STOP, repeated STARTs and their address phases included,
 *   a repeated START inside it matches the I2C's addresses, the STOP that ends it sets no STOPF,
 *   and a PE 1 write before that STOP keeps the I2C enabled. A firmware START waits for the port's
 *   STOP. A 10-bit header with ADDMODE 1 logs one `model_warning`, raises `model_error` under
 *   `strict_peripherals` at the address phase, and answers NACK.
 * - Target with NOSTRETCH 1, RM0038 26.3.4 and 26.3.5: an address match answers at its end with
 *   ADDR set and no SCL hold. A written byte that ends with RxNE 1 is lost and sets OVR, and a read
 *   byte that starts with the shift register empty sends the DR byte again and sets OVR. BTF never
 *   sets. A read byte NACKed before the ADDR clear ends the transmission, RM0038 26.3.4 AF.
 *   Inferred: the lost byte is acknowledged as CR1.ACK says, a read byte before the ADDR clear
 *   sends DR as it is, TxE sets only with ADDR clear, and the ADDR clear after that NACK sets no
 *   TxE.
 * - Errors, RM0038 26.3.4: a target's `bus_error` sets BERR and its `overrun` sets OVR at the end
 *   of the byte, which completes as its acknowledge says. On silicon OVR sets in slave mode with
 *   NOSTRETCH only, the injection exercises firmware handlers. With several targets the byte takes
 *   the error of the first in attachment order. An `arbitration_lost` sets ARLO, clears MSL and
 *   TRA, RM0038 26.6.7, ignores the reply's acknowledge and stretch, drops a read byte and sends no
 *   DR byte the firmware wrote, RM0038 26.3.4 ARLO. Inferred: TxE clears and BTF follows a byte
 *   still held, as at a START end. Inferred: BUSY stays 1 for one SCL period, then the winner's
 *   STOP reaches the targets, clears BUSY and CR1.STOP and sends a START CR1.START holds. PE 0 in
 *   that window leaves BUSY 1 until that STOP. In target mode a port STOP inside a running data
 *   byte of the I2C's acknowledged address, or inside a running address phase while the I2C is
 *   enabled, sets BERR and drops the byte, RM0038 26.3.4 BERR. Inferred: a STOP inside another
 *   address's byte or before a queued phase starts sets no BERR.
 * - The controller receiver decides a byte's acknowledge at the end of its eighth period, before
 *   the acknowledge pulse, from CR1.ACK, an ACK write inside the eighth period counting, as RM0038
 *   Figure 213 note 4 states. An ACK write during the acknowledge pulse counts for the next byte,
 *   inferred. With POS 1 it takes ACK as it was at the previous byte's decision or at the end of
 *   the address, inferred for an ACK write during a byte. A byte clocked after a NACKed byte reads
 *   0xFF and reaches no target.
 * - A DR write in reception clears RxNE, and with BTF set the held byte replaces the written value
 *   in DR, inferred. A DR write with TRA 1 clears RxNE, BTF and a byte held from a reception, and
 *   the transmitter sends it, inferred.
 * - PE 0 during a controller transfer, which RM0038 26.6.1 forbids, ends it at once and logs one
 *   `model_warning`. The SB clear by an SR1 read then a DR write follows RM0038 26.6.6 SB and
 *   Figure 212 EV5. Inferred: a DR write with SB set and no SR1 read since the last SB or ADDR
 *   clear sends nothing, and the DR write that clears SB takes the SR1 read, so the ADDR clear
 *   needs an SR1 read after it. A DR write while ADDR is set, during the address phase or after a
 *   data NACK sends nothing, inferred. An SR2 read between the SR1 read and the DR write that
 *   clears SB leaves that clear armed, inferred: RM0038 26.6.7 names the ADDR case only. A CR1.STOP
 *   set while ADDR is set sends its STOP after the ADDR clear, inferred where RM0038 26.6.1, Figure
 *   212 and 26.3.3 conflict. A byte written with TxE 1 and NACKed leaves TxE 1, inferred from
 *   RM0038 26.6.6 TxE.
 * - An STM32L1 I2C target answers at the phase's end. A target that leaves its answer pending is
 *   taken as NACK or as a byte of 0xFF and fails the run with `config_error`, and
 *   `i2c_target_port::complete()` and `complete_read()` throw `config_error`. The STM32L1 I2C is a
 *   link end of no linked system and takes no host bus hold.
 * - A Python target whose `on_start`, `on_write`, `on_read` or `on_stop` raises, or whose `matches`
 *   raises, answers that phase as an absent target: a NACK, a read byte of all ones, no match. The
 *   phase and its flags complete, and the exception leaves at the end of the event, inferred:
 *   RM0456 65.4.9 and RM0038 26.3.4 describe a NACK, none describes a target that fails.
 */
class i2c_l1 final : public dispatch_model<i2c_l1, i2c_l1_state>,
                     public i2c_target_port,
                     public i2c_controller_port,
                     public reset_listener,
                     public bus_reset_listener,
                     public dma_requester
{
public:
  i2c_l1(
      model_context&                           ctx,
      peripheral_desc const&                   desc,
      std::vector<std::shared_ptr<i2c_target>> targets,
      std::shared_ptr<i2c_controller_sink>     controller,
      i2c_l1_variant const&                    variant);
  i2c_l1(i2c_l1 const&)                    = delete;
  auto operator=(i2c_l1 const&) -> i2c_l1& = delete;
  i2c_l1(i2c_l1&&)                         = delete;
  auto operator=(i2c_l1&&) -> i2c_l1&      = delete;
  ~i2c_l1() override;

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
  void               transfer_completed(dma_controller const& by, std::uint32_t request) override;
  void               last_but_one(dma_controller const& by, std::uint32_t request) override;

  void control_written(std::uint32_t old, std::uint32_t now);
  void control2_written(std::uint32_t old, std::uint32_t now);
  void status_written(std::uint32_t old, std::uint32_t now);

private:
  void               restore_reset_values(std::optional<reset_kind> kind);
  [[nodiscard]] auto step() const -> wire;
  [[nodiscard]] auto controlling() const -> bool;
  [[nodiscard]] auto enabled() const -> bool;
  [[nodiscard]] auto length_ps(std::uint32_t periods) -> std::optional<std::uint64_t>;
  [[nodiscard]] auto power_on_offset() const -> std::uint64_t;
  [[nodiscard]] auto ticket_of(std::size_t index) const -> std::uint64_t;
  void begin(wire next, std::uint32_t periods, std::optional<std::uint64_t> at = std::nullopt);
  void begin_start(std::optional<std::uint64_t> at = std::nullopt);
  void check_timing();
  void check_unmodelled();
  void note(std::uint8_t warning, std::string_view reg, std::string message);
  void unmodelled(bool& logged, std::string_view name, std::string_view what);
  void phase_ended();
  void start_ended();
  void address_ended();
  void data_ended();
  void stop_ended();
  void receive_ended();
  void acknowledge_ended();
  void address_cleared();
  void data_written();
  void received_taken();
  void shift_out(std::uint8_t byte);
  [[nodiscard]] auto receiving() const -> bool;
  [[nodiscard]] auto stretchless() const -> bool;
  [[nodiscard]] auto pending_condition() -> bool;
  [[nodiscard]] auto answered(answers const& all) -> bool;
  auto take(std::size_t index, std::optional<i2c_reply> const& reply, answers& all) -> bool;
  void take_read(std::size_t index, std::optional<i2c_read_reply> const& reply, answers& all);
  void lose();
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
  void               run_port(std::uint32_t lane);
  [[nodiscard]] auto port_answer(i2c_port_entry& entry) -> std::optional<i2c_answer>;
  [[nodiscard]] auto address_answer(i2c_port_entry& entry) -> std::optional<i2c_answer>;
  [[nodiscard]] auto write_answer(i2c_port_entry& entry) -> std::optional<i2c_answer>;
  [[nodiscard]] auto read_answer(i2c_port_entry const& entry) -> i2c_answer;
  [[nodiscard]] auto read_begun(i2c_port_entry& entry) -> bool;
  void               begin_read(i2c_port_entry& entry, std::uint8_t byte);
  void               target_written(std::uint8_t byte);
  void               transmission_ended();
  void               phase_begun(i2c_port_entry const& entry);
  [[nodiscard]] auto held_entry() -> i2c_port_entry*;
  void               release(i2c_port_entry& entry, bool ack);
  void               release_held();
  void               target_ended(std::uint64_t at_ps);
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
  field_ref                                dma_last_;
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
  field_ref                                bus_error_;
  field_ref                                arbitration_lost_;
  field_ref                                overrun_;
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
  field_ref                                dual_flag_;
  field_ref                                general_flag_;
  dma_signal                               tx_dma_;
  dma_signal                               rx_dma_;
  std::uint32_t                            cleared_by_disable_{};
  std::uint32_t                            status1_events_{};
  std::uint32_t                            status1_errors_{};
  std::uint32_t                            unmodelled_control_{};
  core::clock_domain                       kernel_;
  irq_line&                                event_line_;
  irq_line&                                error_line_;
  std::vector<std::string>                 deferred_;
  hook_failure                             hooks_;
  std::optional<std::uint64_t>             reset_at_;
  std::uint8_t                             timing_logged_{};
  bool                                     timing_checked_{};
  bool                                     control_logged_{};
  bool                                     disable_logged_{};
  bool                                     ending_{};
  bool                                     header_logged_{};
};

constexpr std::array<write_entry<i2c_l1>, 9> i2c_l1_writes{
    {
     {"CR1", &i2c_l1::control_written},
     {"CR2", &i2c_l1::control2_written},
     {"OAR1", nullptr},
     {"OAR2", nullptr},
     {"DR", nullptr},
     {"SR1", &i2c_l1::status_written},
     {"SR2", nullptr},
     {"CCR", nullptr},
     {"TRISE", nullptr},
     }
};

i2c_l1::i2c_l1(
    model_context&                           ctx,
    peripheral_desc const&                   desc,
    std::vector<std::shared_ptr<i2c_target>> targets,
    std::shared_ptr<i2c_controller_sink>     controller,
    i2c_l1_variant const&                    variant)
    : dispatch_model{ctx, desc, "I2C", i2c_l1_writes}, regs_{ctx.regs},
      targets_{std::move(targets)}, controller_{std::move(controller)}, control_{reg("CR1")},
      data_{reg("DR")}, status1_{reg("SR1")}, status2_{reg("SR2")}, enable_{field("CR1", "PE")},
      start_{field("CR1", "START")}, stop_{field("CR1", "STOP")}, reset_bit_{field("CR1", "SWRST")},
      ack_{field("CR1", "ACK")}, pos_{field("CR1", "POS")}, frequency_{field("CR2", "FREQ")},
      event_ie_{field("CR2", "ITEVTEN")}, buffer_ie_{field("CR2", "ITBUFEN")},
      error_ie_{field("CR2", "ITERREN")}, dma_enable_{field("CR2", "DMAEN")},
      dma_last_{field("CR2", "LAST")}, fast_{field("CCR", "F_S")}, duty_{field("CCR", "DUTY")},
      divider_{field("CCR", "CCR")}, rise_{field("TRISE", "TRISE")}, start_bit_{field("SR1", "SB")},
      address_sent_{field("SR1", "ADDR")}, byte_finished_{field("SR1", "BTF")},
      tx_empty_{field("SR1", "TxE")}, rx_full_{field("SR1", "RxNE")},
      ack_failure_{field("SR1", "AF")}, bus_error_{field("SR1", "BERR")},
      arbitration_lost_{field("SR1", "ARLO")}, overrun_{field("SR1", "OVR")},
      master_{field("SR2", "MSL")}, busy_{field("SR2", "BUSY")}, transmitter_{field("SR2", "TRA")},
      stop_detected_{field("SR1", "STOPF")}, own_mode_{field("OAR1", "ADDMODE")},
      own_address_{field("OAR1", "ADD_1_7")}, no_stretch_{field("CR1", "NOSTRETCH")},
      general_call_{field("CR1", "ENGC")}, dual_{field("OAR2", "ENDUAL")},
      own2_{field("OAR2", "ADD2")}, dual_flag_{field("SR2", "DUALF")},
      general_flag_{field("SR2", "GENCALL")}, tx_dma_{ctx.peers, lower(desc.name) + "_tx_dma"},
      rx_dma_{ctx.peers, lower(desc.name) + "_rx_dma"},
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

i2c_l1::~i2c_l1()
{
  for(auto const& target : targets_)
  {
    target->detached();
  }
}

auto i2c_l1::step() const -> wire
{
  return static_cast<wire>(state_.phase);
}

auto i2c_l1::controlling() const -> bool
{
  return get(regs_, master_) != 0;
}

auto i2c_l1::enabled() const -> bool
{
  return get(regs_, enable_) != 0 or state_.disabling != 0;
}

auto i2c_l1::power_on_offset() const -> std::uint64_t
{
  return ctx_.clock.since_power_on().ps - ctx_.clock.time().ps;
}

auto i2c_l1::ticket_of(std::size_t index) const -> std::uint64_t
{
  return std::uint64_t{state_.generation} << 16 | index;
}

auto i2c_l1::length_ps(std::uint32_t periods) -> std::optional<std::uint64_t>
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

void i2c_l1::note(std::uint8_t warning, std::string_view reg, std::string message)
{
  if((timing_logged_ & warning) == 0)
  {
    timing_logged_ |= warning;
    warn(reg, std::move(message));
  }
}

void i2c_l1::unmodelled(bool& logged, std::string_view name, std::string_view what)
{
  warn_once_or_refuse(logged, name, ctx_.strict_peripherals ? reg(name).offset : 0u, [&] {
    return std::format("{}: not modelled, nothing is clocked for it", what);
  });
}

void i2c_l1::check_timing()
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

void i2c_l1::check_unmodelled()
{
  if((regs_.peek(control_.offset) & unmodelled_control_) != 0)
  {
    unmodelled(control_logged_, "CR1", "SMBus, PEC, ALERT or ARP");
  }
}

void i2c_l1::begin(wire next, std::uint32_t periods, std::optional<std::uint64_t> at)
{
  auto const ps = length_ps(periods);
  if(not ps)
  {
    return;
  }
  auto const now      = at ? *at : ending_ ? state_.phase_end_ps : ctx_.clock.time().ps;
  auto const from     = std::max(now, state_.released_ps);
  state_.phase        = static_cast<std::uint8_t>(next);
  state_.phase_end_ps = from + *ps;
  ctx_.events.schedule_at(vtime{state_.phase_end_ps}, phase_done, ++state_.generation, ctx_.clock);
}

void i2c_l1::begin_start(std::optional<std::uint64_t> at)
{
  check_unmodelled();
  check_timing();
  update(regs_, busy_, 1);
  begin(wire::start, 1, at);
}

auto i2c_l1::pending_condition() -> bool
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

void i2c_l1::control_written(std::uint32_t old, std::uint32_t now)
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
    if(state_.disabling != 0
       or (enable_.of(old) != 0 and state_.matched != 0 and not controlling()))
    {
      state_.disabling = 1;
      return;
    }
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
    timing_checked_  = false;
    state_.disabling = 0;
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

void i2c_l1::control2_written(std::uint32_t, std::uint32_t)
{}

void i2c_l1::status_written(std::uint32_t old, std::uint32_t now)
{
  regs_.poke(status1_.offset, (now & ~status1_errors_) | (old & now & status1_errors_));
}

void i2c_l1::abort()
{
  auto const lost   = step() == wire::stop and not controlling();
  state_.tx_active  = 0;
  state_.addressed  = 0;
  state_.last_ack   = 0;
  state_.tx_loaded  = 0;
  state_.tx_sending = 0;
  if(not lost)
  {
    ++state_.generation;
    state_.phase = static_cast<std::uint8_t>(wire::idle);
    std::ranges::fill(state_.in_transfer, 0);
  }
  state_.status_read = 0;
  state_.tx_full     = 0;
  state_.rx_held     = 0;
  state_.released_ps = 0;
  std::ranges::fill(state_.acked, 0);
  regs_.poke(control_.offset, regs_.peek(control_.offset) & ~cleared_by_disable_);
  regs_.poke(status1_.offset, 0);
  for(auto const& bit : {master_, transmitter_, dual_flag_, general_flag_})
  {
    update(regs_, bit, 0);
  }
  update(regs_, busy_, lost ? 1 : state_.wire_busy);
}

void i2c_l1::shift_out(std::uint8_t byte)
{
  state_.shift = byte;
  update(regs_, tx_empty_, 1);
  update(regs_, byte_finished_, 0);
  begin(wire::data, byte_periods);
}

void i2c_l1::data_written()
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
  if(not controlling() and state_.tx_active != 0)
  {
    target_written(byte);
    return;
  }
  if(not controlling() and state_.addressed != 0 and get(regs_, transmitter_) != 0)
  {
    update(regs_, tx_empty_, 0);
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

auto i2c_l1::stretchless() const -> bool
{
  return get(regs_, no_stretch_) != 0;
}

auto i2c_l1::receiving() const -> bool
{
  return controlling()
     and get(regs_, transmitter_) == 0
     and get(regs_, start_bit_) == 0
     and get(regs_, address_sent_) == 0;
}

void i2c_l1::received_taken()
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

void i2c_l1::address_cleared()
{
  update(regs_, address_sent_, 0);
  if(not controlling())
  {
    if(get(regs_, transmitter_) != 0 and state_.addressed != 0 and state_.last_ack != 0)
    {
      state_.tx_active = 1;
      update(regs_, tx_empty_, state_.tx_full == 0 ? 1 : 0);
    }
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

auto i2c_l1::answered(answers const& all) -> bool
{
  switch(all.error)
  {
    case i2c_error::none:
      break;
    case i2c_error::bus_error:
      update(regs_, bus_error_, 1);
      break;
    case i2c_error::overrun:
      update(regs_, overrun_, 1);
      break;
    case i2c_error::arbitration_lost:
      lose();
      return true;
  }
  state_.released_ps = std::max(state_.released_ps, state_.phase_end_ps + all.stretch_ps);
  return false;
}

void i2c_l1::lose()
{
  update(regs_, arbitration_lost_, 1);
  update(regs_, master_, 0);
  update(regs_, transmitter_, 0);
  update(regs_, tx_empty_, 0);
  update(regs_, byte_finished_, state_.rx_held);
  state_.tx_full = 0;
  std::ranges::fill(state_.acked, 0);
  begin(wire::stop, 1);
}

auto i2c_l1::take(std::size_t index, std::optional<i2c_reply> const& reply, answers& all) -> bool
{
  if(not reply)
  {
    deferred_.push_back(std::format("the target at {:#04x}", targets_[index]->address()));
    return false;
  }
  all.note(reply->error, reply->stretch_ps);
  all.ack = all.ack or reply->ack;
  return reply->ack;
}

void i2c_l1::take_read(std::size_t index, std::optional<i2c_read_reply> const& reply, answers& all)
{
  if(not reply)
  {
    deferred_.push_back(std::format("the target at {:#04x}", targets_[index]->address()));
    return;
  }
  all.note(reply->error, reply->stretch_ps);
  all.byte &= reply->byte;
}

void i2c_l1::start_ended()
{
  regs_.poke(control_.offset, regs_.peek(control_.offset) & ~start_.mask());
  update(regs_, start_bit_, 1);
  update(regs_, master_, 1);
  update(regs_, transmitter_, 0);
  update(regs_, tx_empty_, 0);
  update(regs_, byte_finished_, state_.rx_held);
  state_.tx_full  = 0;
  state_.dma_last = 0;
  std::ranges::fill(state_.acked, 0);
  if(get(regs_, stop_) != 0)
  {
    begin(wire::stop, 1);
  }
}

void i2c_l1::address_ended()
{
  auto const at      = vtime{state_.phase_end_ps + power_on_offset()};
  auto const address = static_cast<std::uint8_t>(state_.address >> 1);
  auto const read    = (state_.address & 1u) != 0;
  auto const header  = (state_.address & 0xF8u) == 0xF0u;
  auto       all     = answers{};
  state_.ack_next    = static_cast<std::uint8_t>(get(regs_, ack_));
  for(std::size_t index = 0; index < targets_.size() and not header; ++index)
  {
    if(not hooks_.guard([&] { return targets_[index]->matches(address, at); }, false))
    {
      continue;
    }
    state_.in_transfer[index] = 1;
    auto const reply          = hooks_.guard(
        [&] { return targets_[index]->start_deferred(address, read, at, ticket_of(index)); },
        std::optional{i2c_reply{.ack = false}});
    state_.acked[index] = take(index, reply, all) ? 1 : 0;
  }
  if(answered(all))
  {
    return;
  }
  if(all.ack)
  {
    update(regs_, address_sent_, 1);
    update(regs_, transmitter_, read ? 0 : 1);
    return;
  }
  update(regs_, ack_failure_, 1);
  static_cast<void>(pending_condition());
}

void i2c_l1::data_ended()
{
  auto const at  = vtime{state_.phase_end_ps + power_on_offset()};
  auto       all = answers{};
  for(std::size_t index = 0; index < targets_.size(); ++index)
  {
    if(state_.acked[index] != 0)
    {
      take(
          index,
          hooks_.guard(
              [&] { return targets_[index]->write_deferred(state_.shift, at, ticket_of(index)); },
              std::optional{i2c_reply{.ack = false}}),
          all);
    }
  }
  if(answered(all))
  {
    return;
  }
  if(not all.ack)
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

void i2c_l1::stop_ended()
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
      hooks_.run([&] { targets_[index]->stop(at); });
    }
  }
  if(get(regs_, enable_) != 0 and get(regs_, start_) != 0 and state_.wire_busy == 0)
  {
    begin_start();
  }
}

void i2c_l1::receive_ended()
{
  auto const now   = get(regs_, ack_) != 0;
  auto const last  = std::exchange(state_.dma_last, 0) != 0;
  state_.ack_pulse = last ? 0 : (get(regs_, pos_) != 0 ? state_.ack_next : (now ? 1 : 0));
  state_.ack_next  = now ? 1 : 0;
  begin(wire::acknowledge, 1);
}

void i2c_l1::acknowledge_ended()
{
  auto const at  = vtime{state_.phase_end_ps + power_on_offset()};
  auto const ack = state_.ack_pulse != 0;
  auto       all = answers{};
  for(std::size_t index = 0; index < targets_.size(); ++index)
  {
    if(state_.acked[index] != 0)
    {
      take_read(
          index,
          hooks_.guard(
              [&] { return targets_[index]->read_deferred(at, ack, ticket_of(index)); },
              std::optional{i2c_read_reply{.byte = 0xFF}}),
          all);
    }
  }
  if(answered(all))
  {
    return;
  }
  auto const byte = all.byte;
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

void i2c_l1::phase_ended()
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

void i2c_l1::flags()
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
  auto const dma = get(regs_, dma_enable_) != 0;
  tx_dma_.set(dma and tx_empty_.of(sr1) != 0);
  rx_dma_.set(dma and rx_full_.of(sr1) != 0);
}

void i2c_l1::transfer_completed(dma_controller const&, std::uint32_t)
{}

void i2c_l1::last_but_one(dma_controller const& by, std::uint32_t request)
{
  if(rx_dma_.carries(by, request) and get(regs_, dma_enable_) != 0 and get(regs_, dma_last_) != 0)
  {
    state_.dma_last = 1;
  }
}

void i2c_l1::rethrow_refused()
{
  hooks_.rethrow();
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

void i2c_l1::refuse_completion() const
{
  throw config_error(
      ctx_.part.part, 0, std::format("devices.{}", desc_.name),
      "an answer at the phase's end, the STM32L1 I2C completes no deferred phase");
}

void i2c_l1::complete(std::uint64_t, i2c_reply, vtime, std::uint32_t)
{
  refuse_completion();
}

void i2c_l1::complete_read(std::uint64_t, i2c_read_reply, vtime, std::uint32_t)
{
  refuse_completion();
}

void i2c_l1::port_check(std::uint32_t lane, std::uint64_t unit) const
{
  check_port_entry(ctx_, desc_.name, controller_ != nullptr, state_.port, max_entries, lane, unit);
}

auto i2c_l1::after_model_reset(std::uint64_t due_ps) const -> std::uint64_t
{
  return std::max(due_ps, state_.port.reset_ps.value_or(0));
}

void i2c_l1::schedule_port(i2c_port_entry const& entry)
{
  stm32::schedule_port(ctx_, port_due, state_.port, entry);
}

void i2c_l1::queue_port(i2c_port_entry entry)
{
  stm32::queue_port(ctx_, port_due, state_.port, entry);
}

void i2c_l1::queue_phase(
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
      false));
}

void i2c_l1::start(
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

void i2c_l1::write(
    std::uint8_t  byte,
    std::uint64_t phase_ps,
    vtime         at,
    std::uint32_t lane,
    std::uint64_t unit)
{
  queue_phase(i2c_phase::write, byte, phase_ps, at, lane, unit);
}

void i2c_l1::read(
    bool          ack,
    std::uint64_t phase_ps,
    vtime         at,
    std::uint32_t lane,
    std::uint64_t unit)
{
  queue_phase(i2c_phase::read, 0xFF, phase_ps, at, lane, unit, ack);
}

void i2c_l1::stop(vtime at, std::uint32_t lane, std::uint64_t unit)
{
  port_check(lane, unit);
  queue_port(
      {.due_ps    = after_model_reset(at.ps),
       .unit      = unit,
       .posted_ps = at.ps,
       .lane      = lane,
       .kind      = entry_stop});
}

void i2c_l1::drop(i2c_phase phase, vtime at, std::uint32_t lane, std::uint64_t unit)
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

auto i2c_l1::last_unit() const -> std::uint64_t
{
  return state_.port.last_unit;
}

auto i2c_l1::queued() const -> std::size_t
{
  return state_.port.entries.size();
}

void i2c_l1::run_port(std::uint32_t lane)
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
    auto const erase = [&] { entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(*pick)); };
    switch(entry.kind)
    {
      case entry_phase:
        phase_begun(entry);
        if(phase == i2c_phase::read and not read_begun(entries[*pick]))
        {
          entries[*pick].kind = entry_held;
          flags();
          break;
        }
        flags();
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
        hooks_.run([&] { controller_->answered(entry.unit, phase, *answer, vtime{entry.due_ps}); });
        break;
      }
      case entry_released:
        erase();
        flags();
        hooks_.run([&] {
          controller_->answered(entry.unit, phase, {.ack = entry.ack != 0}, vtime{entry.due_ps});
        });
        break;
      case entry_stop:
        erase();
        port_stopped(entry);
        break;
      default:
        erase();
        flags();
        hooks_.run([&] { controller_->dropped(entry.unit, phase, vtime{entry.due_ps}); });
        break;
    }
  }
}

void i2c_l1::phase_begun(i2c_port_entry const& entry)
{
  if(static_cast<i2c_phase>(entry.phase) != i2c_phase::address)
  {
    return;
  }
  if(state_.addressed != 0)
  {
    transmission_ended();
    update(regs_, transmitter_, 0);
  }
  state_.addressed = 0;
  state_.last_ack  = 0;
  state_.wire_busy = 1;
  update(regs_, busy_, 1);
  update(regs_, dual_flag_, 0);
  update(regs_, general_flag_, 0);
}

auto i2c_l1::port_answer(i2c_port_entry& entry) -> std::optional<i2c_answer>
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
  return read_answer(entry);
}

auto i2c_l1::address_answer(i2c_port_entry& entry) -> std::optional<i2c_answer>
{
  auto const address = static_cast<std::uint32_t>(entry.data >> 1);
  auto const read    = (entry.data & 1u) != 0;
  if(not enabled() or controlling() or step() != wire::idle)
  {
    return i2c_answer{.ack = false};
  }
  auto const seven   = get(regs_, own_mode_) == 0;
  auto const general = address == 0 and not read and get(regs_, general_call_) != 0;
  auto const first   = seven and address != 0 and get(regs_, own_address_) == address;
  auto const second  = seven
                  and not first
                  and address != 0
                  and get(regs_, dual_) != 0
                  and get(regs_, own2_) == address;
  if(not general and not first and not second)
  {
    if(not seven and (address & 0x7Cu) == 0x78u)
    {
      unmodelled(header_logged_, "OAR1", "10-bit addressing in target mode");
    }
    return i2c_answer{.ack = false};
  }
  check_unmodelled();
  if(get(regs_, ack_) == 0)
  {
    return i2c_answer{.ack = false};
  }
  update(regs_, address_sent_, 1);
  update(regs_, transmitter_, read ? 1 : 0);
  update(regs_, dual_flag_, second ? 1 : 0);
  update(regs_, general_flag_, general ? 1 : 0);
  state_.addressed = 1;
  state_.matched   = 1;
  state_.last_ack  = 1;
  entry.ack        = 1;
  if(stretchless())
  {
    state_.tx_active = read ? 1 : 0;
    return i2c_answer{.ack = true};
  }
  return std::nullopt;
}

auto i2c_l1::write_answer(i2c_port_entry& entry) -> std::optional<i2c_answer>
{
  if(state_.addressed == 0 or get(regs_, transmitter_) != 0)
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
  if(stretchless())
  {
    update(regs_, overrun_, 1);
    return i2c_answer{.ack = ack};
  }
  state_.shift   = entry.data;
  state_.rx_held = 1;
  entry.ack      = ack ? 1 : 0;
  update(regs_, byte_finished_, 1);
  return std::nullopt;
}

auto i2c_l1::held_entry() -> i2c_port_entry*
{
  auto const found = std::ranges::find(state_.port.entries, entry_held, &i2c_port_entry::kind);
  return found != state_.port.entries.end() ? &*found : nullptr;
}

void i2c_l1::release(i2c_port_entry& entry, bool ack)
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

void i2c_l1::release_held()
{
  auto* const held = held_entry();
  if(held != nullptr)
  {
    release(*held, held->ack != 0);
  }
}

auto i2c_l1::read_begun(i2c_port_entry& entry) -> bool
{
  if(state_.tx_active == 0)
  {
    return true;
  }
  if(state_.tx_loaded == 0)
  {
    if(not stretchless())
    {
      return false;
    }
    entry.data        = static_cast<std::uint8_t>(regs_.peek(data_.offset));
    state_.tx_sending = 1;
    update(regs_, overrun_, 1);
    return true;
  }
  entry.data        = state_.tx_shift;
  state_.tx_loaded  = 0;
  state_.tx_sending = 1;
  return true;
}

void i2c_l1::begin_read(i2c_port_entry& entry, std::uint8_t byte)
{
  auto const now    = ctx_.clock.time().ps;
  entry.kind        = entry_ending;
  entry.data        = byte;
  entry.due_ps      = now + entry.phase_ps;
  state_.tx_sending = state_.tx_active;
  auto const unit   = entry.unit;
  auto const end    = entry.due_ps;
  schedule_port(entry);
  for(auto const index : place_after(state_.port.entries, unit, end))
  {
    schedule_port(state_.port.entries[index]);
  }
}

auto i2c_l1::read_answer(i2c_port_entry const& entry) -> i2c_answer
{
  auto const ack = entry.ack != 0;
  if(std::exchange(state_.tx_sending, 0) == 0 or state_.tx_active == 0)
  {
    return {.ack = ack};
  }
  state_.last_ack = ack ? 1 : 0;
  if(not ack)
  {
    update(regs_, ack_failure_, 1);
    state_.tx_active = 0;
    return {.ack = ack, .byte = entry.data};
  }
  if(state_.tx_full != 0)
  {
    state_.tx_shift  = static_cast<std::uint8_t>(regs_.peek(data_.offset));
    state_.tx_loaded = 1;
    state_.tx_full   = 0;
  }
  else if(not stretchless())
  {
    update(regs_, byte_finished_, 1);
  }
  if(get(regs_, address_sent_) == 0)
  {
    update(regs_, tx_empty_, 1);
  }
  return {.ack = ack, .byte = entry.data};
}

void i2c_l1::target_written(std::uint8_t byte)
{
  update(regs_, byte_finished_, 0);
  if(state_.tx_loaded == 0 and state_.tx_sending == 0)
  {
    state_.tx_shift  = byte;
    state_.tx_loaded = 1;
    auto* const held = held_entry();
    if(held != nullptr and static_cast<i2c_phase>(held->phase) == i2c_phase::read)
    {
      state_.tx_loaded = 0;
      begin_read(*held, byte);
    }
    return;
  }
  state_.tx_full = 1;
  update(regs_, tx_empty_, 0);
}

void i2c_l1::transmission_ended()
{
  if(get(regs_, transmitter_) == 0)
  {
    return;
  }
  state_.tx_active  = 0;
  state_.tx_loaded  = 0;
  state_.tx_sending = 0;
  state_.tx_full    = 0;
  update(regs_, tx_empty_, 0);
  update(regs_, byte_finished_, 0);
}

void i2c_l1::port_stopped(i2c_port_entry const& entry)
{
  auto const cut = [&](i2c_port_entry const& queued) {
    return (queued.kind == entry_phase or queued.kind == entry_ending) and queued.unit < entry.unit;
  };
  std::vector<std::pair<std::uint64_t, i2c_phase>> reports;
  auto                                             misplaced = false;
  for(auto const& queued : state_.port.entries)
  {
    if(cut(queued))
    {
      auto const phase = static_cast<i2c_phase>(queued.phase);
      misplaced        = misplaced
                      or (queued.kind == entry_ending
                          and (state_.addressed != 0 or (phase == i2c_phase::address and enabled())));
      reports.emplace_back(queued.unit, phase);
    }
  }
  std::erase_if(state_.port.entries, cut);
  if(misplaced)
  {
    update(regs_, bus_error_, 1);
  }
  target_ended(entry.due_ps);
  std::ranges::sort(reports);
  flags();
  for(auto const& [unit, phase] : reports)
  {
    hooks_.run([&] { controller_->dropped(unit, phase, vtime{entry.due_ps}); });
  }
}

void i2c_l1::target_ended(std::uint64_t at_ps)
{
  auto const waited = state_.wire_busy != 0;
  if(state_.addressed != 0 or state_.last_ack != 0)
  {
    if(get(regs_, enable_) != 0 and state_.last_ack != 0)
    {
      update(regs_, stop_detected_, 1);
    }
    transmission_ended();
    update(regs_, transmitter_, 0);
  }
  state_.addressed = 0;
  state_.matched   = 0;
  state_.last_ack  = 0;
  state_.wire_busy = 0;
  update(regs_, dual_flag_, 0);
  update(regs_, general_flag_, 0);
  if(std::exchange(state_.disabling, 0) != 0)
  {
    abort();
    return;
  }
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
    begin_start(at_ps);
  }
}

void i2c_l1::rearm_port(i2c_port port, std::optional<reset_kind> kind)
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
        if(static_cast<i2c_phase>(entry.phase) == i2c_phase::read)
        {
          entry.kind = entry_phase;
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

void i2c_l1::before_reset(reset_kind)
{
  reset_at_ = ctx_.clock.time().ps;
}

void i2c_l1::reset(register_file&, reset_kind kind)
{
  tx_dma_.resolve();
  rx_dma_.resolve();
  restore_reset_values(kind);
}

void i2c_l1::on_bus_reset()
{
  restore_reset_values(std::nullopt);
}

void i2c_l1::restore_reset_values(std::optional<reset_kind> kind)
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
      hooks_.run([&] { target->reset(*kind); });
    }
  }
  flags();
  hooks_.rethrow();
}

void i2c_l1::power_cut()
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

auto i2c_l1::on_read(
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

auto i2c_l1::on_write(
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
    if(enabled())
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

void i2c_l1::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind == port_due)
  {
    if(payload >> 32 == state_.port.generation)
    {
      run_port(static_cast<std::uint32_t>(payload));
      hooks_.rethrow();
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

void i2c_l1::save(std::vector<std::byte>& out) const
{
  auto copy = state_;
  copy.targets.clear();
  for(auto const& target : targets_)
  {
    target->save(copy.targets.emplace_back());
  }
  reflex::serde::bson::serializer{out}.dump(copy);
}

void i2c_l1::load(std::span<std::byte const> in)
{
  auto loaded = reflex::serde::bson::deserializer{in}.template load<i2c_l1_state>();
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
    hooks_.run([&] { targets_[index]->load(state_.targets[index]); });
  }
  state_.targets.clear();
  state_.acked.resize(targets_.size());
  state_.in_transfer.resize(targets_.size());
  hooks_.rethrow();
}
}

auto i2c_l1_factory(
    decltype(board::i2c) const&             targets_,
    decltype(board::i2c_controllers) const& controllers_,
    i2c_l1_variant                          variant) -> model_factory
{
  return [targets = targets_, controllers = controllers_,
          variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    auto const found      = targets.find(desc.name);
    auto const controller = controllers.find(desc.name);
    return std::make_shared<i2c_l1>(
        ctx, desc,
        found != targets.end() ? found->second : std::vector<std::shared_ptr<i2c_target>>{},
        controller != controllers.end() ? controller->second : nullptr, variant);
  };
}

namespace
{
constexpr auto i2c_l1_row_table = std::to_array<builtin>({
    row_of<^^i2c_l1_factory, ^^l1_i2c, ^^board::i2c, ^^board::i2c_controllers>(
        {.ip = "I2C", .ip_version_glob = "i2c1_v1_*", .families = family::l1}),
});
}

auto i2c_l1_rows() -> std::span<builtin const>
{
  return i2c_l1_row_table;
}
}
