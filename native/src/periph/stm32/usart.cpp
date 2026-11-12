#include "dma_signal.hpp"
#include "model_base.hpp"
#include "usart_rows.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/scheduler.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/uart.hpp>

#include <reflex/serde/annotations.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
uart_link_port::~uart_link_port()       = default;
loopback_endpoint::~loopback_endpoint() = default;

namespace
{
using wide = unsigned __int128;

constexpr std::uint32_t transmit_done = 0;
constexpr std::uint32_t receive_due   = 1;
constexpr std::uint32_t idle_due      = 2;
constexpr std::uint32_t linked_due    = 3;
constexpr std::uint32_t cts_due       = 4;
constexpr std::uint32_t hold_due      = 5;
constexpr std::uint32_t release_due   = 6;
constexpr std::uint32_t host_start    = 7;
constexpr std::uint32_t attach_due    = 8;
constexpr std::uint32_t rts_due       = 9;
constexpr std::uint32_t abort_due     = 10;
constexpr std::size_t   fifo_depth    = 8;

/**
 * @brief Runs far-end hooks and keeps the first error, so the handler they belong to finishes its
 *        state change before rethrow() raises it.
 */
class hook_guard
{
public:
  template <typename Hook> void run(Hook&& hook)
  {
    try
    {
      std::forward<Hook>(hook)();
    }
    catch(...)
    {
      if(not failure_)
      {
        failure_ = std::current_exception();
      }
    }
  }

  void rethrow() const
  {
    if(failure_)
    {
      std::rethrow_exception(failure_);
    }
  }

private:
  std::exception_ptr failure_;
};

constexpr std::array<std::uint32_t, 12> prescalers{1, 2, 4, 6, 8, 10, 12, 16, 32, 64, 128, 256};
constexpr std::array<std::uint8_t, 4>   half_stop_codes{2, 1, 4, 3};

/** @brief How a receiver samples a bit: offsets in sixteenths of its bit time. */
struct rx_shape
{
  std::uint64_t                frame_ps{};
  std::uint32_t                word_bits{};
  std::uint32_t                half_stops{};
  std::array<std::uint32_t, 3> offsets{};
  std::size_t                  samples{};
};

struct sampled
{
  std::uint16_t data{};
  bool          framing{};
  bool          noise{};
};

auto half_bits(std::uint32_t word, std::uint32_t half_stops) -> std::uint64_t
{
  return 2 * (1 + std::uint64_t{word}) + half_stops;
}

auto sample_at(rx_shape const& rx, std::uint64_t start, std::uint32_t bit, std::uint32_t sixteenths)
    -> std::uint64_t
{
  auto const halves = half_bits(rx.word_bits, rx.half_stops);
  return start + 2 * rx.frame_ps * (16 * std::uint64_t{bit} + sixteenths) / (16 * halves);
}

auto stop_sample(rx_shape const& rx, std::uint64_t start) -> std::uint64_t
{
  return sample_at(rx, start, rx.word_bits + 1, rx.offsets[rx.samples - 1]);
}

auto last_sample(line_frame const& sent, rx_shape const& rx) -> std::uint64_t
{
  return stop_sample(rx, sent.start.ps);
}

auto level(line_frame const& sent, std::optional<std::uint64_t> next_start, std::uint64_t at)
    -> std::uint32_t
{
  if(at < sent.start.ps)
  {
    return 1;
  }
  if(next_start and at >= *next_start)
  {
    return 0;
  }
  auto const since = at - sent.start.ps;
  if(since >= sent.frame_ps)
  {
    return 1;
  }
  auto const bit = since * half_bits(sent.word_bits, sent.half_stops) / (2 * sent.frame_ps);
  if(bit == 0)
  {
    return 0;
  }
  if(bit <= sent.word_bits)
  {
    return (sent.data >> (bit - 1)) & 1u;
  }
  return 1;
}

auto sample(line_frame const& sent, std::optional<std::uint64_t> next_start, rx_shape const& rx)
    -> sampled
{
  sampled out{};
  for(std::uint32_t bit = 1; bit <= rx.word_bits + 1; ++bit)
  {
    std::uint32_t ones = 0;
    for(std::size_t index = 0; index < rx.samples; ++index)
    {
      ones += level(sent, next_start, sample_at(rx, sent.start.ps, bit, rx.offsets[index]));
    }
    auto const value = 2 * ones > rx.samples ? 1u : 0u;
    out.noise        = out.noise or (ones != 0 and ones != rx.samples);
    if(bit <= rx.word_bits)
    {
      out.data = static_cast<std::uint16_t>(out.data | (value << (bit - 1)));
    }
    else
    {
      out.framing = value == 0;
    }
  }
  return out;
}

auto hear(
    uart_endpoint&     far_end,
    line_format const& format,
    line_frame const&  sent,
    bool               next_follows)
{
  rx_shape const rx{
      .frame_ps   = frame_ps_of(format),
      .word_bits  = 8u + (format.parity == line_parity::none ? 0u : 1u),
      .half_stops = format.half_stops,
      .offsets    = {7, 8, 9},
      .samples    = 3
  };
  auto const seen = sample(sent, next_follows ? std::optional{sent.end.ps} : std::nullopt, rx);
  if(seen.framing)
  {
    far_end.line_error(uart_error::framing, sent.end);
  }
  else if(not parity_ok(seen.data, rx.word_bits, format.parity))
  {
    far_end.line_error(uart_error::parity, sent.end);
  }
  else
  {
    far_end.transmitted(data_of(seen.data, rx.word_bits, format.parity), sent.end, false);
  }
}

auto same_format(line_format const& format, line_frame const& sent) -> bool
{
  auto const ps  = frame_ps_of(format);
  auto const gap = ps > sent.frame_ps ? ps - sent.frame_ps : sent.frame_ps - ps;
  return 8u + (format.parity == line_parity::none ? 0u : 1u) == sent.word_bits
     and format.parity == sent.parity
     and format.half_stops == sent.half_stops
     and gap <= 1;
}

struct linked_frame
{
  line_frame    frame{};
  std::uint32_t lane{};
  bool          cut{};

  friend auto operator==(linked_frame const&, linked_frame const&) -> bool = default;
};

struct line_hold
{
  std::uint64_t from_ps{};
  std::uint64_t until_ps{};
  std::uint64_t start_ps{};
  std::uint64_t due_event{};
  std::uint64_t release_event{};
  std::uint32_t lane{};
  bool          open{};
  bool          sampled{};
  bool          received{};
  bool          released{};

  friend auto operator==(line_hold const&, line_hold const&) -> bool = default;
};

struct cts_change
{
  std::uint64_t board_ps{};
  std::uint64_t id{};
  bool          ready{};

  friend auto operator==(cts_change const&, cts_change const&) -> bool = default;
};

struct rx_entry
{
  std::uint16_t data{};
  bool          framing{};
  bool          noise{};
  bool          parity{};

  friend auto operator==(rx_entry const&, rx_entry const&) -> bool = default;
};

struct usart_state
{
  std::array<std::uint32_t, 3>                                       generation{};
  std::vector<std::uint16_t>                                         tx_fifo;
  [[= reflex::serde::rename{"rx_entries"}]] std::vector<rx_entry>    rx_fifo;
  [[= reflex::serde::rename{"host_frames"}]] std::vector<host_frame> host;
  std::uint16_t                                                      shift{};
  std::uint64_t                                                      tx_start_ps{};
  std::uint64_t                                                      tx_end_ps{};
  std::uint64_t                                                      rx_start_ps{};
  std::uint64_t                                                      abort_start_ps{};
  std::uint32_t                                                      abort_shift{};
  std::uint32_t                                                      abort_word_bits{};
  std::uint32_t                                                      abort_half_stops{};
  std::uint32_t                                                      abort_parity{};
  bool                                                               abort_brk{};
  std::uint64_t                                                      rx_due_ps{};
  std::uint64_t                                                      rx_guest_ps{};
  std::uint64_t                                                      host_start_ps{};
  bool                                                               rx_held{};
  bool                                                               rts_level{true};
  bool                                                               shifting{};
  bool                                                               receiving{};
  bool                                                               idle_pending{};
  std::vector<linked_frame>                                          linked;
  std::uint64_t                                                      idle_due_ps{};
  std::uint32_t                                                      linked_generation{};
  bool                                                               break_pending{};
  bool                                                               break_shifting{};
  bool                                                               cts_ready{true};
  std::uint64_t                                                      cts_hold_from_ps{};
  std::vector<cts_change>                                            cts_changes;
  std::uint64_t                                                      next_cts{};
  std::uint32_t                                                      cts_fired{};
  std::vector<line_hold>                                             holds;
  std::uint64_t                                                      hold_events{};
  std::vector<std::byte>                                             far_end;

  friend auto operator==(usart_state const&, usart_state const&) -> bool = default;
};

struct sci2_saved
{
  usart_state   usart;
  std::uint32_t sequence{};
};

struct locked_fields
{
  std::string_view                  reg;
  std::span<std::string_view const> fields;
};

constexpr std::array<std::string_view, 9>  usart_cr1{"FIFOEN", "M1",   "M0",  "DEAT", "DEDT",
                                                     "OVER8",  "WAKE", "PCE", "PS"};
constexpr std::array<std::string_view, 15> usart_cr2{
    "ADD",  "MSBFIRST", "DATAINV", "TXINV", "RXINV", "SWAP",    "LINEN", "STOP",
    "CPOL", "CPHA",     "LBCL",    "LBDL",  "ADDM7", "DIS_NSS", "SLVEN"};
constexpr std::array<std::string_view, 17> usart_cr3{
    "TXFTCFG", "RXFTCFG", "DEP",   "DEM",  "DDRE", "OVRDIS", "ONEBIT", "CTSE", "RTSE",
    "SCEN",    "NACK",    "HDSEL", "IRLP", "IREN", "WUS",    "WUS0",   "WUS1"};
constexpr std::array<std::string_view, 7>  lpuart_cr1{"M1",   "M0",  "DEAT", "DEDT",
                                                      "WAKE", "PCE", "PS"};
constexpr std::array<std::string_view, 8>  lpuart_cr2{"ADD",   "ADDM7", "MSBFIRST", "DATAINV",
                                                      "TXINV", "RXINV", "SWAP",     "STOP"};
constexpr std::array<std::string_view, 12> lpuart_cr3{"TXFTCFG", "RXFTCFG", "DEP",  "DEM",
                                                      "DDRE",    "OVRDIS",  "CTSE", "RTSE",
                                                      "HDSEL",   "WUS",     "WUS0", "WUS1"};
constexpr std::array<std::string_view, 2>  gtpr_locked{"GT", "PSC"};
constexpr std::array<std::string_view, 5>  autocr_locked{
    "TRIGSEL", "IDLEDIS", "TRIGEN", "TRIGPOL", "TDN"};
constexpr std::array<std::string_view, 1> whole_brr{"BRR"};
constexpr std::array<std::string_view, 1> whole_presc{"PRESCALER"};

struct lock
{
  std::string   reg;
  std::uint32_t offset{};
  std::uint32_t mask{};
  std::uint32_t open_when_receiver_off{};
  bool          logged{};
  bool          unmodelled_logged{};
};

/**
 * @brief USART, UART and LPUART of the STM32U5, RM0456 sections 66 and 67, of the STM32H5, the
 *        STM32L1 and the STM32L4, asynchronous mode.
 *        Frames leave and arrive one frame time apart at the baud the kernel clock, PRESC and BRR
 *        give, read when each frame starts. A frame lasts its bit count times USARTDIV periods of
 *        the kernel clock divided by PRESC, one exact product rounded down to the picosecond.
 *        FIFO mode, 8 frames each way, or single registers.
 *        TX frames reach the port's endpoint when their stop bit ends. RX frames the board queues
 *        arrive one frame time apart, an overrun drops the frame and sets ORE, IDLE follows one
 *        idle frame. With CR3.DMAT the `<instance>_tx_dma` request follows TXE, with CR3.DMAR the
 *        `<instance>_rx_dma` request follows RXNE, through the GPDMA peer, levels announced again
 *        in reset. With UE 1, a CPU or DMA write leaves the fields the manual locks at their
 *        value: RM0456 66.8 and 67.7 on the STM32U5, RM0481 56.8 and 57.7 on the STM32H5, RM0351
 *        40.8 and 41.7 on the STM32L4, the CR1, CR2 and CR3 fields per instance kind, BRR and
 *        PRESC whole, ABRMOD while ABREN is 1 and SCARCNT unless the write is 0. The STM32L4
 *        locks CLKEN too, RM0351 40.8.2. The STM32L4 and the STM32H5 lock WUS, RM0351 40.8.3 and
 *        41.7.3, RM0481 56.8.4 and 57.7.4. ADD changes with UE 1 while RE is 0 on the STM32L4,
 *        RM0351 40.8.2 and 41.7.2, and on the STM32H5 LPUART, RM0481 57.7.3. The write logs once
 *        per register and is a model_error under strict peripherals. The held value of a refused
 *        write, a CR1 write that clears UE taking effect whole, no lock on CLKEN on the STM32U5
 *        and the STM32H5, the same lock for DMA writes, TECLREN taking writes and an STM32L1
 *        USART taking every write with UE 1 are inferred: the manuals say "can only be written"
 *        or nothing at all. SCARCNT written 0 is taken, RM0456 66.8.4, RM0481 56.8.4, RM0351
 *        40.8.3 allow only 0 with UE 1. Debugger writes are never locked. GTPR and AUTOCR are
 *        stored registers where the description has them: GT, PSC, TRIGSEL, IDLEDIS, TRIGEN,
 *        TRIGPOL and TDN are locked as the other fields, RM0456 66.8.7, 66.8.16 and 67.7.14,
 *        RM0481 56.8.7, RM0351 40.8.5, TECLREN is not. A write that sets GT, PSC or TRIGEN logs
 *        one warning per register and raises model_error under strict peripherals, the guard
 *        time, the prescaler and the autonomous trigger act on nothing. Not
 *        modelled, logged once when configured: synchronous mode, smartcard,
 *        IrDA, LIN, the RS485 driver enable, auto baud rate, receiver timeout, wake-up from Stop.
 *        For a link to another machine, the endpoint hears each frame when its start bit leaves,
 *        `uart_endpoint::started`, and a clear of UE with a frame in flight, `aborted`, which a
 *        Python device hears through an event at the time of the write. Frames
 *        from another machine, `receive_at`, reach the receiver at their own stop-bit end, IDLE
 *        one receiver frame time after the last one unless the next starts before. Deviations:
 *        an aborted frame delivers nothing and holds the line until the abort, where the chip
 *        would see a truncated frame and FE: IDLE follows one receiver frame after the abort
 *        unless the next frame starts before, whether frames ahead of it were still queued or
 *        not. A reset drops the frames queued from other
 *        machines, also those whose start bit would reach the line after the reset. Events a
 *        delivery from another machine arms, its data and the end of an aborted frame, go on the
 *        wire's lane, so their order against local events of the same instant does not depend on
 *        when the delivery happened. BUSY does not follow frames from other machines. A frame
 *        from another machine whose bit time,
 *        word length or stop length differs from the receiver's is sampled bit by bit on the
 *        sender's ideal waveform at the receiver's bit time, three samples per bit at 7, 8 and 9
 *        sixteenths with OVER16, 3, 4 and 5 eighths with OVER8, the middle one with ONEBIT or on
 *        an LPUART: a disagreement sets NE, a stop bit sampled 0 sets FE, the sampled word enters
 *        RDR. The receiver resynchronises on each sender start bit: a falling edge inside a later
 *        frame is never taken for a start bit. With CR1.PCE, PS chooses even or odd parity, the
 *        parity bit taking the MSB of the M-bit word, RM0456 66.5: a TX frame carries the bit
 *        computed over the TDR data bits, and the endpoint's `transmitted` gets the data bits
 *        alone. A host frame gets the receiver's bit at delivery, inverted when it is tagged with
 *        a parity error, which then sets PE. A host frame tagged with a framing or a noise error
 *        sets FE or NE with its data, a host break reads as 0 with FE and holds the line one frame
 *        time plus 2 bit times before the next host frame. A frame from another machine keeps its
 *        sender's bit, or the sampled one, and PE rises when it fails the receiver's check, with
 *        RDR holding the word as received. FE, NE and PE rise when their frame becomes RDR's, at
 *        its push into an empty RX FIFO or at the RDR read that exposes it, RM0456 66.8: in FIFO
 *        mode a flag waits behind the frames ahead of it. A frame finding the RX FIFO full is
 *        dropped with its errors and sets ORE. RQR.SBKRQ sets SBKF
 *        and sends a break after the frame in flight, ahead of the TX FIFO: a frame time low,
 *        then 2 bits high before the next frame, SBKF clearing as the low part ends. A break from
 *        another machine reads as 0 with FE and never sets PE, LBDF is LIN only and not modelled.
 *        RS232 flow control, RM0456 66.5.21: with CR3.CTSE a frame, a break included, starts only
 *        while the nCTS input the far end drives, `uart_port::set_cts`, says ready, a frame in
 *        flight completes, and a held frame starts at the change to ready, a break's 2-bit hold
 *        kept. TC sets at the end of a data frame when TXE is set, a break following or not,
 *        RM0456 66.8.10 and RM0038 27.6.1, and stays 0 while a data frame is held. With
 *        `usart_variant::break_keeps_tc_low`, the STM32L4 ES0250 2.21.2 and 2.22.1 and the
 *        STM32L1 ES0125 2.11.3, ES0242 2.11.3, ES0224 2.10.3, ES0240 and ES0241 2.12.3 and
 *        ES0578 2.11.3, a break requested while a data frame shifts with CTSE 1 and nCTS
 *        deasserted at its end leaves TC 0 until the break has run, inferred. With
 *        `usart_variant::break_ignores_cts`, the STM32L1 ES0125 2.11.8, ES0242 2.11.8, ES0224
 *        2.10.8, ES0240 and ES0241 2.12.8 and ES0578 2.11.8, a break leaves at the end of the
 *        current transmission whatever nCTS says. Changes apply through events on
 *        core::host_lane at their own time, kept in the state with their board time and
 *        rescheduled at every reset at the same instant. A frame starting at the instant of a
 *        change to not ready is not held, whatever the event order of that instant. A change runs
 *        at the first block boundary at or after its time: a frame a TDR or SBKRQ write queued in
 *        that block, after the change, starts at the change, up to one block before the write.
 *        Each change of the input with CTSE sets ISR.CTSIF, raising the IRQ with CTSIE, ICR.CTSCF
 *        clears it. ISR.CTS reads the input while CTSE is 1 and 0 while it is 0: an unrouted CTS
 *        input reads 0, the reset value of USART_ISR and LPUART_ISR in RM0456 66.8.10 and 67.7.8,
 *        and CTSE stands for the pin being routed as CTS. With CR3.RTSE and DEM 0, RTS says not
 *        ready while RDR is full, or the RX FIFO in FIFO mode, `uart_port::ready_to_receive`: the
 *        next host frame waits before its start bit and starts at the RDR read or the RXFRQ flush
 *        that frees space, so host frames that obey RTS never overrun, a frame with `ignores_rts`
 *        does. RTS changes with RXNE, which the model sets at the stop-bit end, inside the stop bit
 *        on silicon. With DEM the pin carries DE and the host is not held. Frames from another
 *        machine are not held: the wire carries no flow control and they overrun as without RTSE.
 *        At most max_pending_cts CTS changes wait at once.
 *
 *        A line held low by a link, `hold_line` to `release_line`, RM0456 66.5.4, 66.5.7 "Break
 *        character" and "Framing error", and the same for the LPUART, 67.4.4 and 67.4.7: the
 *        falling edge at the hold's start begins a character, sampled as a frame at the
 *        receiver's shape. Held at its stop-bit sample, the last of the stop bit, it is the break
 *        character: 0 with FE and RXFNE, never PE, LBDF never, LIN not being modelled. Released
 *        before that, each data and stop bit reads 0 when its sample lies before the release,
 *        majority of three as for a sampled frame: NE on a split, FE when the stop bit reads 0,
 *        PE per the receiver's check, the sampled word in RDR. Released before the start bit's
 *        middle sample, nothing is received. No new character until the release: no falling
 *        edge. IDLE one receiver frame after the later of the release and the character's end,
 *        unless the line is busy before, also after a hold that received nothing: the model
 *        never gates IDLE on RXFNE, which RM0456 66.8.10 does. A hold cancels an IDLE due after
 *        its start. A frame from another machine still sampled at the hold's start, its start
 *        before and its stop-bit sample at or after it, becomes that break character at its own
 *        stop-bit sample, data 0: deviation, silicon keeps the bits before the hold. An aborted
 *        frame does too while its abort also lies after the hold's start. A frame whose stop
 *        bit was sampled before the hold starts is received as sent, or stays aborted, and the
 *        hold starts its own character. Every hold event goes on the lane the hold came on. A
 *        hold starting where the last one ends continues it, no new character. Holds less than
 *        a frame apart are each sampled alone, and so is a frame starting after a short hold
 *        while the hold's character is still sampled: silicon reads one garbled character. A
 *        reset ends every hold: nothing after it, and a later release is ignored. Host frames
 *        keep their own path during a hold, a linked UART takes none.
 *
 *        The STM32L4 USART, UART and LPUART, RM0351 40.8 and 41.7, run the sci3 registers in
 *        single-register mode: ISR.TXE, RXNE and NF and CR1.TXEIE and RXNEIE take the bits of
 *        TXFNF, RXFNE, NE, TXFNFIE and RXFNEIE, ICR.NCF clears NF, the kernel clock is the
 *        instance's CCIPR selection, and the part has no PRESC, so the kernel clock divided by BRR
 *        gives the baud, 256 times it on LPUART1. `usart_variant::names` maps the field names of
 *        each family. On every family, a CPU or DMA write that sets a bit of CR1.UESM, CR3.UCESM,
 *        WUFIE or WUS, or its bits WUS0 and WUS1 where the description splits it, logs one
 *        warning and raises model_error under strict peripherals before the store: the model
 *        runs in Run mode only and WUF stays 0.
 *
 *        The `sci2` register set of the STM32L1 USART and UART, RM0038 27.6, drives the same
 *        frame engine with one data word each way: SR holds TXE for an empty TDR, TC, RXNE, IDLE,
 *        ORE, NF, FE, PE, LBD and the CTS change flag, DR writes TDR and reads RDR, CR1.M picks 8
 *        or 9 data bits, BRR is DIV_Mantissa and DIV_Fraction, with OVER8 the fraction counts
 *        eighths in its bits 2:0. A write to SR clears TC, RXNE, LBD and CTS where it writes 0
 *        and leaves every other bit, a cleared RXNE drops the received word. A CPU or DMA read
 *        of SR notes which of IDLE, ORE, NF, FE, PE and TC are set, a following DR read clears
 *        the noted IDLE, ORE, NF, FE and PE, a following DR write the noted TC and PE, RM0038
 *        27.6.1: a flag set after the SR read stays. The note is saved with the model state.
 *        EIE raises the line on ORE, NF or FE only while CR3.DMAR is 1, RM0038 27.6.6. CR1.SBK
 *        requests a break while UE and TE are 1, a write of 0 before the break starts cancels
 *        it, RM0038 27.3.2. Once the low part runs SBK reads 1 until it ends, a write of 0
 *        meanwhile keeps it, inferred, and the line stays high
 *        `usart_variant::break_stop_half_bits` before the next frame. A write with UE 0 clears
 *        SBK and lets the frame in flight end before the output stops, RM0038 27.6.4. The
 *        kernel clock is the APB clock of the bus the instance's address lies on,
 *        `usart_variant::kernel_buses`. Without `usart_variant::flow_control` CTSE and RTSE log
 *        one warning and change nothing. Nothing is locked while UE is 1, RM0038 27.6, inferred.
 *        RWU mute mode is logged as not modelled, LBD never sets.
 *
 * @limits usart USART, UART and LPUART
 *
 * @group all unmodelled
 * - Only plain asynchronous mode is modelled. Synchronous, smartcard, IrDA, LIN, half-duplex,
 *   driver enable, auto baud and receiver timeout are logged as not modelled.
 * - The model runs in Run mode only. A CPU or DMA write that sets CR1.UESM, CR3.UCESM, CR3.WUFIE or
 *   CR3.WUS, or on the STM32H5 its bits WUS0 and WUS1, logs one `model_warning` and raises
 *   `ModelError` under `strict_peripherals` before the store, on every family. WUF stays 0.
 * - GTPR and AUTOCR are stored registers. GT, PSC and TRIGEN act on nothing: a CPU or DMA write
 *   that sets one logs one `model_warning` per register and raises `ModelError` under
 *   `strict_peripherals` before the store. TRIGSEL, IDLEDIS, TRIGPOL and TDN are stored and act
 *   on nothing without a warning. A firmware access to the two registers is not listed in the
 *   unmodelled access report, the `model_warning` for GT, PSC and TRIGEN is the signal.
 *
 * @group all rule
 * - With UE 1 a CPU or DMA write leaves the fields the manual locks at their value: RM0456 66.8
 *   and 67.7 on the STM32U5, RM0481 56.8 and 57.7 on the STM32H5, RM0351 40.8 and 41.7 on the
 *   STM32L4. These are the CR1, CR2 and CR3 fields the manual writes only with UE 0, BRR and
 *   PRESC whole, ABRMOD while ABREN is 1 and SCARCNT unless the write is 0. The write logs once
 *   per register and raises `ModelError` under `strict_peripherals`. Debugger writes pass.
 * - ADD changes with UE 1 while RE is 0 on the STM32L4 USART and LPUART and on the STM32H5
 *   LPUART, RM0351 40.8.2 and 41.7.2, RM0481 57.7.3. On the STM32U5 and the STM32H5 USART it
 *   changes only with UE 0, RM0456 66.8.3 and 67.7.3, RM0481 56.8.3. CLKEN is locked on the
 *   STM32L4 USART, RM0351 40.8.2. WUS is locked on the STM32L4, as WUS0 and WUS1 on the STM32H5,
 *   RM0351 40.8.3 and 41.7.3, RM0481 56.8.4 and 57.7.4.
 * - GT and PSC, RM0456 66.8.7, RM0481 56.8.7, RM0351 40.8.5, and TRIGSEL, IDLEDIS, TRIGEN,
 *   TRIGPOL and TDN, RM0456 66.8.16 and 67.7.14, change only with UE 0 and are held as the
 *   fields above. RM0456 66.8.7 and 66.8.16 state the lock, the readback is inferred.
 * - TECLREN, bit 31 of AUTOCR, takes writes with UE 1: RM0456 66.8.16 and 67.7.14 name bits 31:23
 *   reserved and no lock for it, inferred.
 * - A refused write leaves the field at its old value and reads it back. The manuals say the
 *   field "can only be written when the USART is disabled (UE = 0)" and state neither what a
 *   refused write does nor what the field reads, inferred.
 * - A CR1 write that clears UE takes effect whole, locked fields included. RM0456 66.8.1 leaves
 *   the case of one word clearing UE and changing a locked field open, inferred.
 * - SCARCNT written 0 with UE 1 is taken: RM0456 66.8.4, RM0481 56.8.4 and RM0351 40.8.3 allow
 *   only 0 then. A non-zero write is held, as a refused write.
 * - CLKEN is not locked on the STM32U5 and the STM32H5: RM0456 66.8.3 and RM0481 56.8.3 give
 *   the field no UE sentence, inferred.
 * - The lock applies to DMA writes as to CPU writes, the manuals name no initiator, inferred.
 * - IDLE is never gated on RXFNE, which RM0456 66.8.10 does.
 * - BUSY does not follow frames from other machines.
 * - An aborted frame delivers nothing and holds the line until the abort. The chip would see a
 *   truncated frame and FE.
 * - A frame from another machine still being sampled when a link holds the line low becomes the
 *   break character, data 0. Silicon keeps the bits before the hold.
 * - A `UartDevice` with its own `line` format reads in both directions through the model's sampler:
 *   three samples per bit at 7, 8 and 9 sixteenths of the reader's bit time with a majority vote,
 *   the start bit of a frame that follows reading low from its start, a framing error when the stop
 *   sample reads low. The device's frames arrive at the later of their stop-bit end and the UART's
 *   last stop sample. The firmware's frames reach the device with the stamp of the firmware's
 *   stop-bit end, a slower device sampling later than that stamp. Frames of a device faster than
 *   the UART overlap the frame the UART still samples: the next frame is sampled on a grid anchored
 *   at its own start, as a linked frame is, where a receiver busy until its stop sample might
 *   resynchronise on a later edge. The outcome of a baud, parity or stop-length mismatch is
 *   inferred: the manuals do not state it, and the model's sampler may differ from the chip's.
 *
 * @group l1 rule
 * - RM0038 27.6 names no field written only with UE 0, so an STM32L1 USART takes every write with
 *   UE 1, inferred.
 * - STM32L1 USART and UART, RM0038 section 27, run the same frame engine with one data word each
 *   way. USART_SR resets to 0x000000C0, TXE and TC set, RM0038 Table 158.
 * - The STM32L1 kernel clock is the APB clock of the instance, PCLK2 for USART1, PCLK1 for USART2,
 *   USART3, UART4 and UART5, whatever `RCC_APBxENR` holds, which gates only the register access,
 *   see RCC.
 * - The first frame after TE is set starts at its DR write, without the idle preamble and the 1
 *   bit-time delay of the RM0038 27.6.4 TE note. Clearing UE lets the frame in flight end, then
 *   stops the output, RM0038 27.6.4, and drops the word waiting in TDR.
 * - A write of 0 to RXNE drops the received word, and DR reads 0 while RXNE is 0, inferred.
 * - A write of 0 to SBK before the break starts cancels it, RM0038 27.3.2. Once the break's low
 *   part runs, SBK reads 1 until it ends, a write of 0 meanwhile keeps it, inferred. The line stays
 *   high 1 bit after the break before the next frame, RM0038 27.3.2.
 * - SBK written with TE 0 and UE 1 sends the break once TE is 1. A write with UE 0 clears SBK,
 *   inferred.
 * - Mute mode, RWU, is logged as not modelled. LBD never sets.
 * - UART4 and UART5 have no hardware flow control, RM0038 Table 157: CTSE and RTSE set there log
 *   one `model_warning` and transmission and reception ignore them.
 * - The STM32L1 SR-then-DR clear: a CPU or DMA read of USART_SR notes the IDLE, ORE, NF, FE, PE and
 *   TC flags it reads set, any later DR read clears the noted IDLE, ORE, NF, FE and PE, and any
 *   later DR write the noted TC and PE, whatever other accesses lie between, inferred. A flag set
 *   after the SR read stays set, and a DR write with no SR read since TC rose sends the word and
 *   leaves TC set, RM0038 27.6.1.
 * - STM32L1 EIE raises the interrupt on ORE, NF or FE only while DMAR is 1, RM0038 27.6.6.
 *
 * @group l4 rule "STM32L4"
 * - STM32L4 USART1 to USART3, UART4, UART5 and LPUART1, RM0351 sections 40 and 41, run the sci3
 *   frame engine with one data word each way: ISR.TXE, RXNE and NF, CR1.TXEIE and RXNEIE, ICR.NCF
 *   clears NF. The kernel clock is the instance's `RCC_CCIPR` selection, BRR alone divides it, the
 *   L4 has no PRESC. Every instance has CTS and RTS flow control, RM0351 Table 251. USART_ISR reads
 *   0x000000C0 after reset, TXE and TC, bit 25 0, inferred: RM0351 40.8.8 gives 0x020000C0 with
 *   TCBGT, which the Table 251 note gives to the L49x and L4Ax only.
 *
 * @group all rule "TC at the end of a frame"
 * - TC sets at the end of a data frame when TXE is set, a break following or not, on every family,
 *   RM0456 66.8.10 and 67.7.8, RM0481 56.8 and 57.7, RM0351 40.8.8, RM0038 27.6.1. A held data
 *   frame keeps TC 0.
 *
 * @group l1,l4 note "STM32L1 and STM32L4 errata"
 * - STM32L4 ES0250 2.21.2 and 2.22.1 and STM32L1 ES0125 2.11.3, ES0242 2.11.3, ES0224 2.10.3,
 *   ES0240 and ES0241 2.12.3 and ES0578 2.11.3 are modelled: a break requested while a data frame
 *   shifts with CTSE 1 and nCTS deasserted at its end keeps TC 0 until the break has run, inferred.
 * - STM32L1 ES0125 2.11.8, ES0242 2.11.8, ES0224 2.10.8, ES0240 and ES0241 2.12.8 and ES0578 2.11.8
 *   are modelled: with CTSE 1 a break leaves at the end of the current transmission whatever nCTS
 *   says. A data frame waits for nCTS.
 *
 * @group l4 rule "STM32L4 RTS and LPUART"
 * - STM32L4 ES0250 2.21.3 and 2.22.2: RTS is driven low once RTSE is set, also while UE or RE is 0.
 *   The model reads ready to the host while UE is 0, and while RE is 0 with room in RDR. With RE 0
 *   and RDR full it reads not ready, inferred: the erratum names no RDR state.
 * - STM32L4 LPUART1, RM0351 section 41: 256 times the kernel clock over BRR[19:0] gives the baud,
 *   and a BRR below 0x300 sends and receives nothing, RM0351 41.4.4. The kernel clock is the
 *   `RCC_CCIPR` LPUART1SEL selection. RQR has no TXFRQ, a write of bit 4 does nothing.
 *
 * @group l4 note "STM32L4 errata"
 * - STM32L4 ES0250 2.22.3, LPUART transmitter jitter at a low BRR, and 2.22.4, LPUART1 outputs not
 *   open-drain, are pin-level effects the frame engine leaves out.
 * - STM32L4 ES0250 2.21.1, NACK sampling in smartcard mode, follows smartcard: logged as not
 *   modelled. ES0250 2.21.4, the receiver timeout start with two stop bits, follows RTOEN: logged
 *   as not modelled. ES0250 2.21.5, data corruption on a noisy receive line, and 2.21.6, data
 *   corrupted when ABREN is cleared, are analog and auto-baud effects the frame engine leaves out.
 *   ES0250 2.21.7, NF with ONEBIT: the model samples once with ONEBIT and sets NF only for a host
 *   frame tagged with noise.
 */
class usart final : public dispatch_model<usart, usart_state>,
                    public uart_link_port,
                    public bus_reset_listener
{
public:
  usart(
      model_context&                 ctx,
      peripheral_desc const&         desc,
      std::shared_ptr<uart_endpoint> far_end,
      usart_variant const&           variant);

  void reset(register_file& regs, reset_kind kind) override;
  void on_bus_reset() override;
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

  void               save(std::vector<std::byte>& out) const override;
  void               load(std::span<std::byte const> in) override;
  void               receive_frames(std::span<host_frame const> frames) override;
  [[nodiscard]] auto queued() const -> std::size_t override;
  void               set_cts(bool ready, vtime at) override;
  void               set_cts_stamped(bool ready, vtime at) override;
  [[nodiscard]] auto has_flow_control() const -> bool override;
  [[nodiscard]] auto pending_cts() const -> std::size_t override;
  [[nodiscard]] auto cts_ready() const -> bool override;
  [[nodiscard]] auto ready_to_receive() const -> bool override;
  void               attach(std::shared_ptr<uart_endpoint> far_end) override;
  void               receive_at(line_frame const& frame, std::uint32_t lane) override;
  void               cancel_in_flight(vtime at, std::uint32_t lane) override;
  void               hold_line(vtime from, std::uint32_t lane) override;
  void               release_line(vtime at, std::uint32_t lane) override;

  void control_written(std::uint32_t old, std::uint32_t now);
  void clear_written(std::uint32_t old, std::uint32_t now);
  void request_written(std::uint32_t old, std::uint32_t now);
  void transmit_written(std::uint32_t old, std::uint32_t now);
  void plain_written(std::uint32_t old, std::uint32_t now);
  void status_written(std::uint32_t old, std::uint32_t now);
  void clear_sequence(std::uint32_t flags);
  void break_written();

private:
  void               restore_reset_values(bool keep_host);
  [[nodiscard]] auto enabled() const -> bool;
  [[nodiscard]] auto cts_on() const -> bool;
  [[nodiscard]] auto rts_on() const -> bool;
  [[nodiscard]] auto fifo() const -> bool;
  [[nodiscard]] auto frame_ps() -> std::uint64_t;
  [[nodiscard]] auto word_bits() const -> std::uint32_t;
  [[nodiscard]] auto parity() const -> line_parity;
  [[nodiscard]] auto word_mask() const -> std::uint32_t;
  [[nodiscard]] auto shape() -> rx_shape;
  auto schedule(std::uint32_t kind, std::uint64_t from_ps, std::uint64_t ps) -> std::uint64_t;
  auto schedule_receive(std::uint64_t from_ps, host_frame const& frame, std::uint64_t ps)
      -> std::uint64_t;
  [[nodiscard]] auto can_start(std::uint64_t from_ps) -> bool;
  void               start_frame(std::uint64_t from_ps);
  void               deliver(rx_entry entry);
  void               expose();
  [[nodiscard]] auto next_host_frame() -> std::optional<host_frame>;
  [[nodiscard]] auto entry_of(host_frame const& frame) -> rx_entry;
  [[nodiscard]] auto sampled_entry(host_frame const& frame, std::optional<std::uint64_t> next_start)
      -> rx_entry;
  [[nodiscard]] auto own_format(host_frame const& frame) -> bool;
  [[nodiscard]] auto rx_space() const -> bool;
  [[nodiscard]] auto clear_to_send(std::uint64_t start_ps) const -> bool;
  [[nodiscard]] auto board_base() const -> std::uint64_t;
  void               apply_cts(bool ready, std::uint64_t at_ps);
  void               schedule_cts(bool ready, vtime at);
  void               resume_host();
  void               note_rts(std::uint64_t at_ps);
  [[nodiscard]] auto head_ready() const -> bool;
  void               ask_attached();
  void               note_abort(hook_guard& guard);
  auto               tx_frame(std::uint64_t end_ps) const -> line_frame;
  void               schedule_linked();
  void               idle_at(std::uint64_t due_ps);
  [[nodiscard]] auto linked_before(std::uint64_t due_ps) const -> bool;
  void               schedule_hold(line_hold& hold, std::uint64_t due_ps);
  void               sample_hold(line_hold& hold);
  void               end_hold(std::vector<line_hold>::iterator hold);
  void               flags();
  void               interrupts();
  void               warn_unsupported();
  void               note_unmodelled(lock& entry, std::uint32_t stored, std::uint32_t written);
  [[nodiscard]] auto bit(std::optional<field_ref> const& flag) const -> std::uint32_t;
  [[nodiscard]] auto sci3_field(std::string_view reg_name, std::string_view name) const
      -> std::optional<field_ref>;
  [[nodiscard]] auto named_register(std::string_view reg_name) const -> std::string_view;
  void               put(std::optional<field_ref> const& flag, std::uint32_t value);

  register_file&                               regs_;
  std::shared_ptr<uart_endpoint>               far_end_;
  bool                                         low_power_;
  bool                                         sci2_;
  std::string                                  cr1_;
  std::string                                  isr_;
  reg_ref                                      rdr_;
  reg_ref                                      tdr_;
  std::optional<reg_ref>                       icr_;
  field_table<usart_role>                      names_;
  field_ref                                    enable_;
  field_ref                                    transmit_;
  field_ref                                    receive_;
  std::optional<field_ref>                     fifo_enable_;
  field_ref                                    word_low_;
  std::optional<field_ref>                     word_high_;
  std::optional<field_ref>                     oversampling_;
  field_ref                                    stop_;
  std::optional<field_ref>                     overrun_disable_;
  field_ref                                    baud_;
  std::optional<field_ref>                     baud_fraction_;
  std::optional<field_ref>                     prescaler_;
  field_ref                                    data_;
  field_ref                                    tx_not_full_;
  std::optional<field_ref>                     tx_empty_;
  field_ref                                    complete_;
  field_ref                                    rx_not_empty_;
  std::optional<field_ref>                     rx_full_;
  field_ref                                    idle_;
  field_ref                                    overrun_;
  field_ref                                    noise_;
  field_ref                                    framing_;
  field_ref                                    parity_;
  std::optional<field_ref>                     tx_ack_;
  std::optional<field_ref>                     rx_ack_;
  std::optional<field_ref>                     busy_;
  field_ref                                    tx_not_full_ie_;
  std::optional<field_ref>                     tx_empty_ie_;
  field_ref                                    complete_ie_;
  field_ref                                    rx_not_empty_ie_;
  std::optional<field_ref>                     rx_full_ie_;
  field_ref                                    idle_ie_;
  field_ref                                    parity_ie_;
  field_ref                                    error_ie_;
  std::optional<field_ref>                     tx_threshold_;
  field_ref                                    dma_transmit_;
  field_ref                                    dma_receive_;
  dma_signal                                   tx_dma_;
  dma_signal                                   rx_dma_;
  std::optional<field_ref>                     rx_threshold_;
  std::optional<field_ref>                     tx_threshold_ie_;
  std::optional<field_ref>                     rx_threshold_ie_;
  std::optional<field_ref>                     tx_threshold_flag_;
  std::optional<field_ref>                     rx_threshold_flag_;
  std::optional<field_ref>                     flush_rx_;
  std::optional<field_ref>                     flush_tx_;
  field_ref                                    parity_enable_;
  field_ref                                    parity_odd_;
  field_ref                                    break_request_;
  field_ref                                    break_flag_;
  field_ref                                    cts_enable_;
  field_ref                                    rts_enable_;
  std::optional<field_ref>                     driver_enable_;
  field_ref                                    cts_ie_;
  std::optional<field_ref>                     cts_;
  field_ref                                    cts_flag_;
  std::optional<field_ref>                     one_bit_;
  std::vector<std::pair<field_ref, field_ref>> clears_;
  std::vector<field_ref>                       unsupported_;
  std::vector<field_ref>                       stop_mode_;
  bool                                         stop_mode_logged_{};
  core::clock_domain                           kernel_;
  irq_line&                                    line_;
  bool                                         unsupported_logged_{};
  bool                                         no_baud_logged_{};
  bool                                         parity_tag_logged_{};
  std::vector<lock>                            locks_;
  std::vector<field_ref>                       unmodelled_;
  std::string_view                             manual_;
  std::uint32_t                                status_clears_{};
  std::uint32_t                                break_stop_half_bits_;
  bool                                         flow_control_;
  bool                                         break_keeps_tc_low_;
  bool                                         break_ignores_cts_;
  bool                                         flow_logged_{};
  std::uint32_t                                read_clears_{};
  std::uint32_t                                write_clears_{};
  std::uint32_t                                sequence_{};
  std::optional<field_ref>                     auto_baud_;
  std::optional<field_ref>                     auto_baud_mode_;
  std::optional<field_ref>                     retransmissions_;
};

static_assert(names_every_role(sci3_fifo_names));
static_assert(names_every_role(sci3_names));
static_assert(names_every_role(sci2_names));
static_assert(names_every_role(sci3_lpuart_names));

consteval auto fifo_named(usart_variant const& variant) -> bool
{
  return names_every_role<usart_role>(variant.names)
     and flag_matches_roles<usart_role>(
             variant.names, variant.fifo,
             {usart_role::tx_empty, usart_role::rx_full, usart_role::tx_empty_ie,
              usart_role::rx_full_ie, usart_role::tx_threshold, usart_role::rx_threshold,
              usart_role::tx_threshold_ie, usart_role::rx_threshold_ie,
              usart_role::tx_threshold_flag, usart_role::rx_threshold_flag});
}

static_assert(fifo_named(with_views_usart));
static_assert(fifo_named(h5_usart));
static_assert(fifo_named(u5_lpuart));
static_assert(fifo_named(h5_lpuart));
static_assert(fifo_named(l1_usart));
static_assert(fifo_named(l1_uart));
static_assert(fifo_named(l4_usart));
static_assert(fifo_named(l4_lpuart));

constexpr auto has_prescaler(usart_variant const& variant) -> bool
{
  auto const found =
      std::ranges::find(variant.names, usart_role::prescaler, &named_field<usart_role>::role);
  return found != variant.names.end() and not found->name.reg.empty();
}

auto usart_writes(usart_variant const& variant) -> std::span<write_entry<usart> const>
{
  static constexpr std::array<write_entry<usart>, 10> with_views{
      {
       {"CR1_enabled", &usart::control_written},
       {"CR2", &usart::control_written},
       {"CR3", &usart::control_written},
       {"RQR", &usart::request_written},
       {"ISR_enabled", &usart::plain_written},
       {"ICR", &usart::clear_written},
       {"RDR", &usart::plain_written},
       {"TDR", &usart::transmit_written},
       {"BRR", &usart::control_written},
       {"PRESC", &usart::control_written},
       }
  };
  static constexpr std::array<write_entry<usart>, 10> plain{
      {
       {"CR1", &usart::control_written},
       {"CR2", &usart::control_written},
       {"CR3", &usart::control_written},
       {"RQR", &usart::request_written},
       {"ISR", &usart::plain_written},
       {"ICR", &usart::clear_written},
       {"RDR", &usart::plain_written},
       {"TDR", &usart::transmit_written},
       {"BRR", &usart::control_written},
       {"PRESC", &usart::control_written},
       }
  };
  static constexpr std::array<write_entry<usart>, 9> single{
      {
       {"CR1", &usart::control_written},
       {"CR2", &usart::control_written},
       {"CR3", &usart::control_written},
       {"RQR", &usart::request_written},
       {"ISR", &usart::plain_written},
       {"ICR", &usart::clear_written},
       {"RDR", &usart::plain_written},
       {"TDR", &usart::transmit_written},
       {"BRR", &usart::control_written},
       }
  };
  static constexpr std::array<write_entry<usart>, 6> sci2{
      {
       {"CR1", &usart::control_written},
       {"CR2", &usart::control_written},
       {"CR3", &usart::control_written},
       {"SR", &usart::status_written},
       {"DR", &usart::transmit_written},
       {"BRR", &usart::control_written},
       }
  };
  if(variant.registers == usart_registers::sci2)
  {
    return sci2;
  }
  if(not variant.mode_views and not has_prescaler(variant))
  {
    return single;
  }
  return variant.mode_views ? std::span<write_entry<usart> const>{with_views} : plain;
}

auto line_name(peripheral_desc const& desc, usart_variant const& variant) -> std::string
{
  return variant.irq_name.empty() ? desc.name + "_IRQn" : std::string{variant.irq_name};
}

auto kernel_name(peripheral_desc const& desc, usart_variant const& variant) -> std::string
{
  if(variant.kernel_buses.empty())
  {
    return lower(desc.name);
  }
  auto const* const bus = window_of(variant.kernel_buses, static_cast<std::uint32_t>(desc.base));
  if(bus == nullptr)
  {
    throw config_error(
        desc.name, 0, "base", "an address on one of the buses that clock the USART model");
  }
  return std::string{bus->clock};
}

usart::usart(
    model_context&                 ctx,
    peripheral_desc const&         desc,
    std::shared_ptr<uart_endpoint> far_end,
    usart_variant const&           variant)
    : dispatch_model{ctx, desc, desc.ip, usart_writes(variant)}, regs_{ctx.regs},
      far_end_{std::move(far_end)}, low_power_{desc.ip == "LPUART"},
      sci2_{variant.registers == usart_registers::sci2},
      cr1_{variant.mode_views ? "CR1_enabled" : "CR1"},
      isr_{sci2_ ? "SR" : (variant.mode_views ? "ISR_enabled" : "ISR")},
      rdr_{reg(sci2_ ? "DR" : "RDR")}, tdr_{reg(sci2_ ? "DR" : "TDR")},
      icr_{sci2_ ? std::nullopt : std::optional{reg("ICR")}},
      names_{
          variant.names,
          [this](std::string_view reg_name, std::string_view name) {
            return field(named_register(reg_name), name);
          },
          [this](std::string_view reg_name, std::string_view name) {
            return has_field(named_register(reg_name), name);
          }},
      enable_{field(cr1_, "UE")}, transmit_{field(cr1_, "TE")}, receive_{field(cr1_, "RE")},
      fifo_enable_{variant.fifo ? std::optional{field(cr1_, "FIFOEN")} : std::nullopt},
      word_low_{field(cr1_, sci2_ ? "M" : "M0")}, word_high_{sci3_field(cr1_, "M1")},
      oversampling_{low_power_ ? std::nullopt : std::optional{field(cr1_, "OVER8")}},
      stop_{field("CR2", "STOP")}, overrun_disable_{sci3_field("CR3", "OVRDIS")},
      baud_{field("BRR", sci2_ ? "DIV_Mantissa" : "BRR")},
      baud_fraction_{sci2_ ? std::optional{field("BRR", "DIV_Fraction")} : std::nullopt},
      prescaler_{names_.find(usart_role::prescaler)},
      data_{field(sci2_ ? "DR" : "RDR", sci2_ ? "DR" : "RDR")},
      tx_not_full_{names_[usart_role::tx_not_full]}, tx_empty_{names_.find(usart_role::tx_empty)},
      complete_{field(isr_, "TC")}, rx_not_empty_{names_[usart_role::rx_not_empty]},
      rx_full_{names_.find(usart_role::rx_full)}, idle_{field(isr_, "IDLE")},
      overrun_{field(isr_, "ORE")}, noise_{names_[usart_role::noise]}, framing_{field(isr_, "FE")},
      parity_{field(isr_, "PE")}, tx_ack_{sci3_field(isr_, "TEACK")},
      rx_ack_{sci3_field(isr_, "REACK")}, busy_{sci3_field(isr_, "BUSY")},
      tx_not_full_ie_{names_[usart_role::tx_not_full_ie]},
      tx_empty_ie_{names_.find(usart_role::tx_empty_ie)}, complete_ie_{field(cr1_, "TCIE")},
      rx_not_empty_ie_{names_[usart_role::rx_not_empty_ie]},
      rx_full_ie_{names_.find(usart_role::rx_full_ie)}, idle_ie_{field(cr1_, "IDLEIE")},
      parity_ie_{field(cr1_, "PEIE")}, error_ie_{field("CR3", "EIE")},
      tx_threshold_{names_.find(usart_role::tx_threshold)}, dma_transmit_{field("CR3", "DMAT")},
      dma_receive_{field("CR3", "DMAR")}, tx_dma_{ctx.peers, lower(desc.name) + "_tx_dma"},
      rx_dma_{ctx.peers, lower(desc.name) + "_rx_dma"},
      rx_threshold_{names_.find(usart_role::rx_threshold)},
      tx_threshold_ie_{names_.find(usart_role::tx_threshold_ie)},
      rx_threshold_ie_{names_.find(usart_role::rx_threshold_ie)},
      tx_threshold_flag_{names_.find(usart_role::tx_threshold_flag)},
      rx_threshold_flag_{names_.find(usart_role::rx_threshold_flag)},
      flush_rx_{sci3_field("RQR", "RXFRQ")}, flush_tx_{names_.find(usart_role::flush_tx)},
      parity_enable_{field(cr1_, "PCE")}, parity_odd_{field(cr1_, "PS")},
      break_request_{sci2_ ? field(cr1_, "SBK") : field("RQR", "SBKRQ")},
      break_flag_{sci2_ ? field(cr1_, "SBK") : field(isr_, "SBKF")},
      cts_enable_{field("CR3", "CTSE")}, rts_enable_{field("CR3", "RTSE")},
      driver_enable_{sci3_field("CR3", "DEM")}, cts_ie_{field("CR3", "CTSIE")},
      cts_{sci3_field(isr_, "CTS")}, cts_flag_{field(isr_, sci2_ ? "CTS" : "CTSIF")},
      one_bit_{
          std::ranges::contains(
              describe("CR3").fields,
              std::string_view{"ONEBIT"},
              &field_desc::name)
              ? std::optional{field("CR3", "ONEBIT")}
              : std::nullopt},
      kernel_{ctx.clock.domain(kernel_name(desc, variant))}, line_{irq(line_name(desc, variant))},
      break_stop_half_bits_{variant.break_stop_half_bits}, flow_control_{variant.flow_control},
      break_keeps_tc_low_{variant.break_keeps_tc_low}, break_ignores_cts_{variant.break_ignores_cts}
{
  expect_register("CR1_enabled", variant.mode_views);
  expect_field(cr1_, "FIFOEN", variant.fifo);
  if(icr_)
  {
    auto const noise_clear = names_.find(usart_role::noise_clear);
    for(auto const& clear : describe("ICR").fields)
    {
      auto const target = clear.name == "CTSCF"
                            ? std::string_view{"CTSIF"}
                            : std::string_view{clear.name}.substr(0, clear.name.size() - 2);
      if(noise_clear and field("ICR", clear.name).mask() == noise_clear->mask())
      {
        clears_.emplace_back(*noise_clear, noise_);
      }
      else if(
          clear.name.ends_with("CF")
          and std::ranges::contains(describe(isr_).fields, target, &field_desc::name))
      {
        clears_.emplace_back(field("ICR", clear.name), field(isr_, target));
      }
    }
  }
  manual_ = variant.manual;
  if(not sci2_)
  {
    auto const presc = prescaler_.has_value();
    auto const tables = low_power_ ? std::array<locked_fields, 7>{{{cr1_, lpuart_cr1},
                                                                   {"CR2", lpuart_cr2},
                                                                   {"CR3", lpuart_cr3},
                                                                   {"BRR", whole_brr},
                                                                   {"PRESC", whole_presc},
                                                                   {"GTPR", gtpr_locked},
                                                                   {"AUTOCR", autocr_locked}}}
                                   : std::array<locked_fields, 7>{{{cr1_, usart_cr1},
                                                                   {"CR2", usart_cr2},
                                                                   {"CR3", usart_cr3},
                                                                   {"BRR", whole_brr},
                                                                   {"PRESC", whole_presc},
                                                                   {"GTPR", gtpr_locked},
                                                                   {"AUTOCR", autocr_locked}}};
    for(auto const& [reg_name, names] : tables)
    {
      auto const stored_only = reg_name == "GTPR" or reg_name == "AUTOCR";
      if(reg_name == "PRESC" and not presc)
      {
        continue;
      }
      if(stored_only and not std::ranges::contains(desc.registers, reg_name, &register_desc::name))
      {
        continue;
      }
      if(stored_only)
      {
        bind(std::array<write_entry<usart>, 1>{{{reg_name, nullptr}}});
      }
      auto& entry  = locks_.emplace_back();
      entry.reg    = reg_name == cr1_ ? std::string{"CR1"} : std::string{reg_name};
      entry.offset = reg(reg_name).offset;
      for(auto const& bits : describe(reg_name).fields)
      {
        if(std::ranges::contains(names, std::string_view{bits.name}))
        {
          auto const bit_mask = field(reg_name, bits.name).mask();
          if(variant.add_open_with_receiver_off and reg_name == "CR2" and bits.name == "ADD")
          {
            entry.open_when_receiver_off |= bit_mask;
          }
          else
          {
            entry.mask |= bit_mask;
          }
        }
        else if(variant.clken_locked and reg_name == "CR2" and bits.name == "CLKEN")
        {
          entry.mask |= field(reg_name, bits.name).mask();
        }
      }
    }
  }
  auto const optional = [&](std::string_view reg_name, std::string_view bits) {
    return std::ranges::contains(describe(reg_name).fields, bits, &field_desc::name)
             ? std::optional{field(reg_name, bits)}
             : std::nullopt;
  };
  for(auto const& [reg_name, bits] : std::array<std::pair<std::string_view, std::string_view>, 6>{
          {{cr1_, "UESM"},
           {"CR3", "UCESM"},
           {"CR3", "WUFIE"},
           {"CR3", "WUS"},
           {"CR3", "WUS0"},
           {"CR3", "WUS1"}}
  })
  {
    if(auto const found = optional(reg_name, bits))
    {
      stop_mode_.push_back(*found);
    }
  }
  for(auto const& [reg_name, bits] : std::array<std::pair<std::string_view, std::string_view>, 3>{
          {{"GTPR", "GT"}, {"GTPR", "PSC"}, {"AUTOCR", "TRIGEN"}}
  })
  {
    if(not sci2_ and std::ranges::contains(desc.registers, reg_name, &register_desc::name))
    {
      if(auto const found = optional(reg_name, bits))
      {
        unmodelled_.push_back(*found);
      }
    }
  }
  auto_baud_       = optional("CR2", "ABREN");
  auto_baud_mode_  = optional("CR2", "ABRMOD");
  retransmissions_ = optional("CR3", "SCARCNT");
  for(auto const& [reg, name] : std::array<std::pair<std::string_view, std::string_view>, 8>{
          {
           {"CR2", "CLKEN"},
           {"CR2", "LINEN"},
           {"CR2", "ABREN"},
           {"CR2", "RTOEN"},
           {"CR3", "SCEN"},
           {"CR3", "IREN"},
           {"CR3", "HDSEL"},
           {"CR3", "DEM"},
           }
  })
  {
    if(std::ranges::contains(describe(reg).fields, name, &field_desc::name))
    {
      unsupported_.push_back(field(reg, name));
    }
  }
  if(sci2_)
  {
    unsupported_.push_back(field(cr1_, "RWU"));
    status_clears_ =
        complete_.mask() | rx_not_empty_.mask() | cts_flag_.mask() | field(isr_, "LBD").mask();
    read_clears_ =
        idle_.mask() | overrun_.mask() | noise_.mask() | framing_.mask() | parity_.mask();
    write_clears_ = complete_.mask() | parity_.mask();
  }
}

auto usart::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     access) -> std::expected<void, bus_error>
{
  auto const word = offset & ~3u;
  if(sci2_ and access.by != initiator::debugger and word == tdr_.offset)
  {
    clear_sequence(write_clears_);
  }
  if(access.by != initiator::debugger)
  {
    auto const stored = regs.peek(word);
    auto const merged = merge_write(stored, offset, size, value);
    auto const wakeup = std::ranges::any_of(stop_mode_, [&](field_ref const& bits) {
      return bits.offset == word and (bits.of(merged) & ~bits.of(stored)) != 0;
    });
    if(wakeup)
    {
      if(not std::exchange(stop_mode_logged_, true))
      {
        warn(
            word == enable_.offset ? "CR1" : "CR3",
            "UESM, UCESM, WUFIE or WUS set: Stop mode and the wake-up from it are not "
            "modelled, WUF never sets");
      }
      if(ctx_.strict_peripherals)
      {
        throw model_error(desc_.name, word == enable_.offset ? "CR1" : "CR3", word);
      }
    }
  }
  auto const found = std::ranges::find(locks_, word, &lock::offset);
  if(found == locks_.end() or access.by == initiator::debugger)
  {
    return dispatch_model::on_write(regs, offset, size, old, value, access);
  }
  auto const stored = regs.peek(word);
  auto const merged = merge_write(stored, offset, size, value);
  if(not enabled() or (word == enable_.offset and enable_.of(merged) == 0))
  {
    note_unmodelled(*found, stored, merged);
    return dispatch_model::on_write(regs, offset, size, old, value, access);
  }
  auto mask = found->mask;
  if(auto_baud_mode_ and word == auto_baud_mode_->offset and auto_baud_->of(stored) != 0)
  {
    mask |= auto_baud_mode_->mask();
  }
  if(retransmissions_ and word == retransmissions_->offset and retransmissions_->of(merged) != 0)
  {
    mask |= retransmissions_->mask();
  }
  if(found->open_when_receiver_off != 0 and get(regs_, receive_) == 0)
  {
    mask &= ~found->open_when_receiver_off;
  }
  else
  {
    mask |= found->open_when_receiver_off;
  }
  auto const kept = (merged & ~mask) | (stored & mask);
  if(kept != merged)
  {
    if(not std::exchange(found->logged, true))
    {
      warn(
          found->reg,
          std::format(
              "{} written with UE 1: {} lets these fields change only with UE 0, they keep "
              "their value",
              found->reg, manual_));
    }
    if(ctx_.strict_peripherals)
    {
      throw model_error(desc_.name, found->reg, word);
    }
  }
  note_unmodelled(*found, stored, kept);
  return dispatch_model::on_write(regs, word, 4, old, kept, access);
}

void usart::note_unmodelled(lock& entry, std::uint32_t stored, std::uint32_t written)
{
  auto const sets = std::ranges::any_of(unmodelled_, [&](field_ref const& bits) {
    return bits.offset == entry.offset and (bits.of(written) & ~bits.of(stored)) != 0;
  });
  if(not sets)
  {
    return;
  }
  if(not std::exchange(entry.unmodelled_logged, true))
  {
    warn(
        entry.reg,
        std::format(
            "{} sets GT, PSC or TRIGEN: the guard time, the prescaler and the autonomous trigger "
            "are not modelled, the value is stored and acts on nothing",
            entry.reg));
  }
  if(ctx_.strict_peripherals)
  {
    throw model_error(desc_.name, entry.reg, entry.offset);
  }
}

void usart::reset(register_file&, reset_kind kind)
{
  restore_reset_values(false);
  auto const base = board_base();
  for(auto const& change : std::span{state_.cts_changes}.subspan(state_.cts_fired))
  {
    auto const due = change.board_ps > base ? change.board_ps - base : 0;
    ctx_.events.schedule_at(vtime{due}, cts_due, change.id, core::host_lane, ctx_.clock);
  }
  ask_attached();
  if(far_end_)
  {
    far_end_->reset(kind);
  }
}

void usart::ask_attached()
{
  if(far_end_ and far_end_->awaiting_attach())
  {
    auto const at = ctx_.clock.time();
    ctx_.events.schedule_at(at, attach_due, at.ps, core::host_lane, ctx_.clock);
  }
}

auto usart::tx_frame(std::uint64_t end_ps) const -> line_frame
{
  return {
      .data       = state_.shift,
      .word_bits  = static_cast<std::uint8_t>(word_bits()),
      .half_stops = half_stop_codes[get(regs_, stop_)],
      .parity     = parity(),
      .kind       = state_.break_shifting ? frame_kind::brk : frame_kind::data,
      .frame_ps   = end_ps - state_.tx_start_ps,
      .start      = vtime{state_.tx_start_ps},
      .end        = vtime{end_ps}};
}

void usart::note_abort(hook_guard& guard)
{
  auto const at = ctx_.clock.time();
  if(far_end_->aborts_by_event())
  {
    auto const frame        = tx_frame(at.ps);
    state_.abort_start_ps   = frame.start.ps;
    state_.abort_shift      = frame.data;
    state_.abort_word_bits  = frame.word_bits;
    state_.abort_half_stops = frame.half_stops;
    state_.abort_parity     = static_cast<std::uint32_t>(frame.parity);
    state_.abort_brk        = frame.kind == frame_kind::brk;
    ctx_.events.schedule_at(at, abort_due, at.ps, core::host_lane, ctx_.clock);
    return;
  }
  guard.run([&] {
    far_end_->ending(tx_frame(at.ps));
    far_end_->aborted(at);
  });
}

void usart::on_bus_reset()
{
  hook_guard guard;
  if(state_.shifting and far_end_)
  {
    note_abort(guard);
  }
  restore_reset_values(true);
  guard.rethrow();
}

void usart::restore_reset_values(bool keep_host)
{
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
  for(auto& generation : state_.generation)
  {
    ++generation;
  }
  state_.tx_fifo.clear();
  state_.rx_fifo.clear();
  if(not keep_host)
  {
    state_.host.clear();
  }
  state_.linked.clear();
  state_.holds.clear();
  state_.break_pending  = false;
  state_.break_shifting = false;
  state_.shifting       = false;
  state_.receiving      = false;
  state_.rx_due_ps      = 0;
  state_.rx_start_ps    = 0;
  state_.rx_guest_ps    = 0;
  state_.tx_start_ps    = 0;
  state_.host_start_ps  = 0;
  state_.rx_held        = keep_host and not state_.host.empty();
  if(not keep_host)
  {
    state_.rts_level = true;
  }
  state_.tx_end_ps        = 0;
  state_.idle_pending     = false;
  state_.cts_hold_from_ps = 0;
  sequence_               = 0;
  tx_dma_.resolve();
  rx_dma_.resolve();
  flags();
}

auto usart::bit(std::optional<field_ref> const& flag) const -> std::uint32_t
{
  return flag ? get(regs_, *flag) : 0;
}

void usart::put(std::optional<field_ref> const& flag, std::uint32_t value)
{
  if(flag)
  {
    update(regs_, *flag, value);
  }
}

auto usart::sci3_field(std::string_view reg_name, std::string_view name) const
    -> std::optional<field_ref>
{
  return sci2_ ? std::nullopt : std::optional{field(reg_name, name)};
}

auto usart::named_register(std::string_view reg_name) const -> std::string_view
{
  if(reg_name == "CR1")
  {
    return cr1_;
  }
  return reg_name == "ISR" ? std::string_view{isr_} : reg_name;
}

auto usart::cts_on() const -> bool
{
  return flow_control_ and get(regs_, cts_enable_) != 0;
}

auto usart::rts_on() const -> bool
{
  return flow_control_ and get(regs_, rts_enable_) != 0;
}

auto usart::enabled() const -> bool
{
  return get(regs_, enable_) != 0;
}

auto usart::fifo() const -> bool
{
  return fifo_enable_ and get(regs_, *fifo_enable_) != 0;
}

auto usart::frame_ps() -> std::uint64_t
{
  auto const code  = bit(prescaler_);
  auto const hz    = ctx_.clock.hz(kernel_);
  auto const presc = std::uint64_t{code < prescalers.size() ? prescalers[code] : 256u};
  auto const brr =
      baud_fraction_ ? get(regs_, baud_) << 4 | get(regs_, *baud_fraction_) : get(regs_, baud_);
  std::uint64_t clocks_per_bit = 0;
  std::uint64_t bit_divider    = 1;
  if(brr != 0 and hz != 0)
  {
    if(low_power_)
    {
      clocks_per_bit = brr < 0x300 ? 0 : brr;
      bit_divider    = 256;
    }
    else if(get(regs_, *oversampling_) != 0)
    {
      clocks_per_bit = (brr & 0xfff0u) | ((brr & 7u) << 1);
      bit_divider    = 2;
    }
    else
    {
      clocks_per_bit = brr;
    }
  }
  if(clocks_per_bit == 0)
  {
    auto const in_use = enabled() and (get(regs_, transmit_) != 0 or get(regs_, receive_) != 0);
    if(in_use and not std::exchange(no_baud_logged_, true))
    {
      warn("BRR", "no baud rate from the kernel clock, PRESC and BRR: nothing is transferred");
    }
    return 0;
  }
  auto const                             word = word_bits();
  constexpr std::array<std::uint32_t, 4> half_stops{2, 1, 4, 3};
  auto const                             half_bits = 2 * (1 + word) + half_stops[get(regs_, stop_)];
  return static_cast<std::uint64_t>(
      wide{half_bits} * clocks_per_bit * presc * ps_per_second / (2 * bit_divider * hz));
}

auto usart::schedule(std::uint32_t kind, std::uint64_t from_ps, std::uint64_t ps) -> std::uint64_t
{
  auto const due = std::max(from_ps + ps, ctx_.clock.time().ps);
  ctx_.events.schedule_at(vtime{due}, kind, state_.generation[kind], ctx_.clock);
  return due;
}

auto usart::schedule_receive(std::uint64_t from_ps, host_frame const& frame, std::uint64_t ps)
    -> std::uint64_t
{
  state_.rx_start_ps = from_ps;
  state_.rx_guest_ps = ps;
  if(not own_format(frame))
  {
    schedule(receive_due, from_ps, ps);
    return from_ps + ps;
  }
  auto rx         = shape();
  rx.frame_ps     = ps;
  auto const wire = from_ps + frame_ps_of(frame.line);
  schedule(receive_due, std::max(stop_sample(rx, from_ps), wire), 0);
  return wire;
}

auto usart::shape() -> rx_shape
{
  rx_shape out{
      .frame_ps   = frame_ps(),
      .word_bits  = word_bits(),
      .half_stops = half_stop_codes[get(regs_, stop_)],
      .offsets    = {7, 8, 9},
      .samples    = 3
  };
  if(oversampling_ and get(regs_, *oversampling_) != 0)
  {
    out.offsets = {6, 8, 10};
  }
  if(low_power_ or (one_bit_ and get(regs_, *one_bit_) != 0))
  {
    out.offsets = {8, 8, 8};
    out.samples = 1;
  }
  return out;
}

void usart::warn_unsupported()
{
  for(auto const& feature : unsupported_)
  {
    if(get(regs_, feature) != 0 and not std::exchange(unsupported_logged_, true))
    {
      warn(
          "CR1",
          "a mode other than plain asynchronous is configured: synchronous, smartcard, IrDA, LIN, "
          "half-duplex, driver enable, auto baud, receiver timeout or mute mode, not modelled");
    }
  }
  auto const flow = get(regs_, cts_enable_) != 0 or get(regs_, rts_enable_) != 0;
  if(not flow_control_ and flow and not std::exchange(flow_logged_, true))
  {
    warn(
        "CR3",
        "CTSE or RTSE set on a UART without hardware flow control, RM0038 Table 157: transmission "
        "and reception ignore them");
  }
}

auto usart::can_start(std::uint64_t from_ps) -> bool
{
  auto const queued = state_.break_pending or not state_.tx_fifo.empty();
  return not state_.shifting
     and queued
     and enabled()
     and get(regs_, transmit_) != 0
     and (clear_to_send(from_ps) or (state_.break_pending and break_ignores_cts_))
     and frame_ps() != 0;
}

void usart::start_frame(std::uint64_t from_ps)
{
  if(not can_start(from_ps))
  {
    return;
  }
  auto const ps  = frame_ps();
  auto const brk = state_.break_pending;
  if(brk)
  {
    state_.shift          = 0;
    state_.break_pending  = false;
    state_.break_shifting = true;
  }
  else
  {
    state_.shift = with_parity(state_.tx_fifo.front(), word_bits(), parity());
    state_.tx_fifo.erase(state_.tx_fifo.begin());
  }
  state_.shifting    = true;
  state_.tx_end_ps   = schedule(transmit_done, from_ps, ps);
  state_.tx_start_ps = state_.tx_end_ps - ps;
  if(far_end_)
  {
    far_end_->started(
        {.data       = state_.shift,
         .word_bits  = static_cast<std::uint8_t>(word_bits()),
         .half_stops = half_stop_codes[get(regs_, stop_)],
         .parity     = parity(),
         .kind       = brk ? frame_kind::brk : frame_kind::data,
         .frame_ps   = ps,
         .start      = vtime{state_.tx_end_ps - ps},
         .end        = vtime{state_.tx_end_ps}});
  }
}

auto usart::parity() const -> line_parity
{
  if(get(regs_, parity_enable_) == 0)
  {
    return line_parity::none;
  }
  return get(regs_, parity_odd_) == 0 ? line_parity::even : line_parity::odd;
}

auto usart::word_bits() const -> std::uint32_t
{
  return bit(word_high_) != 0 ? 7u : (get(regs_, word_low_) != 0 ? 9u : 8u);
}

auto usart::word_mask() const -> std::uint32_t
{
  return (std::uint32_t{1} << word_bits()) - 1u;
}

void usart::flags()
{
  note_rts(ctx_.clock.time().ps);
  auto const                           depth = fifo() ? fifo_depth : 1;
  constexpr std::array<std::size_t, 6> eighths{1, 2, 4, 6, 7, 0};
  auto const                           tx_code = bit(tx_threshold_);
  auto const                           rx_code = bit(rx_threshold_);
  auto const                           tx_at   = tx_code < eighths.size() ? eighths[tx_code] : 0;
  auto const rx_at = rx_code < eighths.size() ? (rx_code == 5 ? 8 : eighths[rx_code]) : 8;
  put(tx_threshold_flag_, fifo() and state_.tx_fifo.size() <= tx_at ? 1 : 0);
  put(rx_threshold_flag_, fifo() and state_.rx_fifo.size() >= rx_at ? 1 : 0);
  update(regs_, tx_not_full_, state_.tx_fifo.size() < depth ? 1 : 0);
  put(tx_empty_, state_.tx_fifo.empty() ? 1 : 0);
  update(regs_, rx_not_empty_, state_.rx_fifo.empty() ? 0 : 1);
  put(rx_full_, state_.rx_fifo.size() >= depth ? 1 : 0);
  put(busy_, state_.receiving ? 1 : 0);
  put(tx_ack_, enabled() and get(regs_, transmit_) != 0 ? 1 : 0);
  put(rx_ack_, enabled() and get(regs_, receive_) != 0 ? 1 : 0);
  put(cts_, cts_on() and state_.cts_ready ? 1 : 0);
  regs_.poke(rdr_.offset, state_.rx_fifo.empty() ? 0 : state_.rx_fifo.front().data);
  interrupts();
  auto const up        = enabled();
  auto const tx_wanted = get(regs_, dma_transmit_) != 0 and get(regs_, transmit_) != 0;
  auto const rx_wanted = get(regs_, dma_receive_) != 0 and get(regs_, receive_) != 0;
  tx_dma_.set(up and tx_wanted and get(regs_, tx_not_full_) != 0);
  rx_dma_.set(up and rx_wanted and get(regs_, rx_not_empty_) != 0);
}

void usart::interrupts()
{
  auto const on = [&](std::optional<field_ref> flag, std::optional<field_ref> enable) {
    return bit(flag) != 0 and bit(enable) != 0;
  };
  auto const errors =
      get(regs_, overrun_) != 0 or get(regs_, noise_) != 0 or get(regs_, framing_) != 0;
  auto const level =
      on(tx_not_full_, tx_not_full_ie_)
      or on(tx_empty_, tx_empty_ie_)
      or on(complete_, complete_ie_)
      or on(rx_not_empty_, rx_not_empty_ie_)
      or on(rx_full_, rx_full_ie_)
      or on(idle_, idle_ie_)
      or on(parity_, parity_ie_)
      or on(cts_flag_, cts_ie_)
      or on(overrun_, rx_not_empty_ie_)
      or on(tx_threshold_flag_, tx_threshold_ie_)
      or on(rx_threshold_flag_, rx_threshold_ie_)
      or (errors and get(regs_, error_ie_) != 0 and (not sci2_ or get(regs_, dma_receive_) != 0));
  if(level)
  {
    line_.raise();
  }
  else
  {
    line_.lower();
  }
}

void usart::control_written(std::uint32_t, std::uint32_t)
{
  hook_guard guard;
  warn_unsupported();
  if(not enabled())
  {
    auto const finishing = sci2_ and state_.shifting;
    if(state_.shifting and far_end_ and not finishing)
    {
      note_abort(guard);
    }
    for(std::size_t kind = 0; kind < state_.generation.size(); ++kind)
    {
      if(not finishing or kind != transmit_done)
      {
        ++state_.generation[kind];
      }
    }
    state_.tx_fifo.clear();
    state_.rx_fifo.clear();
    state_.receiving     = false;
    state_.idle_pending  = false;
    state_.break_pending = false;
    state_.rx_due_ps     = 0;
    state_.rx_start_ps   = 0;
    state_.rx_guest_ps   = 0;
    state_.rx_held       = not state_.host.empty();
    if(not finishing)
    {
      state_.shifting       = false;
      state_.break_shifting = false;
      state_.tx_end_ps      = 0;
      state_.tx_start_ps    = 0;
      update(regs_, complete_, 1);
    }
    if(not state_.break_shifting)
    {
      update(regs_, break_flag_, 0);
    }
  }
  else if(get(regs_, receive_) != 0)
  {
    resume_host();
  }
  if(sci2_)
  {
    break_written();
  }
  start_frame(std::max(ctx_.clock.time().ps, state_.tx_end_ps));
  flags();
  guard.rethrow();
}

void usart::break_written()
{
  auto const requested = get(regs_, break_request_) != 0;
  if(not requested)
  {
    state_.break_pending = false;
  }
  else if(
      not state_.break_pending
      and not state_.break_shifting
      and enabled()
      and get(regs_, transmit_) != 0)
  {
    state_.break_pending = true;
  }
  if(state_.break_shifting)
  {
    update(regs_, break_flag_, 1);
  }
}

void usart::clear_sequence(std::uint32_t flags)
{
  auto const seen = sequence_ & flags;
  sequence_ &= ~flags;
  if(seen != 0)
  {
    regs_.poke(complete_.offset, regs_.peek(complete_.offset) & ~seen);
    this->flags();
  }
}

void usart::save(std::vector<std::byte>& out) const
{
  auto copy = state_;
  if(far_end_)
  {
    far_end_->save(copy.far_end);
  }
  if(not sci2_)
  {
    reflex::serde::bson::serializer{out}.dump(copy);
    return;
  }
  reflex::serde::bson::serializer{out}.dump(
      sci2_saved{.usart = std::move(copy), .sequence = sequence_});
}

void usart::load(std::span<std::byte const> in)
{
  if(not sci2_)
  {
    dispatch_model::load(in);
  }
  else
  {
    auto saved = reflex::serde::bson::deserializer{in}.load<sci2_saved>();
    state_     = std::move(saved.usart);
    sequence_  = saved.sequence;
  }
  auto const far_end = std::exchange(state_.far_end, {});
  if(far_end.empty())
  {
    ask_attached();
  }
  else if(far_end_)
  {
    far_end_->load(far_end);
  }
}

void usart::status_written(std::uint32_t old, std::uint32_t now)
{
  auto const cleared = status_clears_ & ~now;
  regs_.poke(complete_.offset, old & ~cleared);
  if(rx_not_empty_.of(cleared) != 0 and not state_.rx_fifo.empty())
  {
    state_.rx_fifo.erase(state_.rx_fifo.begin());
    resume_host();
  }
  flags();
}

void usart::plain_written(std::uint32_t, std::uint32_t)
{}

void usart::transmit_written(std::uint32_t, std::uint32_t now)
{
  auto const depth = fifo() ? fifo_depth : 1;
  if(enabled() and get(regs_, transmit_) != 0 and state_.tx_fifo.size() < depth)
  {
    state_.tx_fifo.push_back(static_cast<std::uint16_t>(now & word_mask()));
    if(not sci2_)
    {
      update(regs_, complete_, 0);
    }
    start_frame(std::max(ctx_.clock.time().ps, state_.tx_end_ps));
  }
  flags();
}

void usart::clear_written(std::uint32_t, std::uint32_t now)
{
  for(auto const& [clear, target] : clears_)
  {
    if(clear.of(now) != 0)
    {
      update(regs_, target, 0);
    }
  }
  regs_.poke(icr_->offset, 0);
  flags();
}

void usart::request_written(std::uint32_t, std::uint32_t now)
{
  if(break_request_.of(now) != 0 and enabled() and get(regs_, transmit_) != 0)
  {
    update(regs_, break_flag_, 1);
    state_.break_pending = true;
    start_frame(std::max(ctx_.clock.time().ps, state_.tx_end_ps));
  }
  if(flush_rx_->of(now) != 0)
  {
    state_.rx_fifo.clear();
    resume_host();
  }
  if(flush_tx_ and flush_tx_->of(now) != 0)
  {
    state_.tx_fifo.clear();
  }
  flags();
}

auto usart::on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
    -> std::expected<std::uint32_t, bus_error>
{
  auto const word = offset & ~3u;
  if(sci2_ and ctx.by != initiator::debugger and word == complete_.offset)
  {
    sequence_ = regs.peek(word) & (read_clears_ | write_clears_);
  }
  if(word != rdr_.offset)
  {
    return dispatch_model::on_read(regs, offset, size, ctx);
  }
  if(sci2_ and ctx.by != initiator::debugger)
  {
    clear_sequence(read_clears_);
  }
  if(ctx.by == initiator::debugger or state_.rx_fifo.empty())
  {
    return dispatch_model::on_read(regs, offset, size, ctx);
  }
  auto const value = state_.rx_fifo.front().data;
  state_.rx_fifo.erase(state_.rx_fifo.begin());
  expose();
  resume_host();
  flags();
  return read_lanes(value, offset, size);
}

void usart::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind == attach_due)
  {
    if(far_end_ and far_end_->awaiting_attach())
    {
      far_end_->attached(vtime{payload});
    }
    return;
  }
  if(kind == host_start)
  {
    resume_host();
    flags();
    return;
  }
  if(kind == abort_due)
  {
    if(far_end_)
    {
      far_end_->ending(
          {.data       = static_cast<std::uint16_t>(state_.abort_shift),
           .word_bits  = static_cast<std::uint8_t>(state_.abort_word_bits),
           .half_stops = static_cast<std::uint8_t>(state_.abort_half_stops),
           .parity     = static_cast<line_parity>(state_.abort_parity),
           .kind       = state_.abort_brk ? frame_kind::brk : frame_kind::data,
           .frame_ps   = payload - state_.abort_start_ps,
           .start      = vtime{state_.abort_start_ps},
           .end        = vtime{payload}});
      far_end_->aborted(vtime{payload});
    }
    return;
  }
  if(kind == rts_due)
  {
    if(far_end_)
    {
      far_end_->rts_changed((payload & 1) != 0, vtime{payload >> 1});
    }
    return;
  }
  if(kind == linked_due)
  {
    if(payload != state_.linked_generation or state_.linked.empty())
    {
      return;
    }
    auto const head = state_.linked.front();
    auto const rx   = shape();
    if(auto const done = rx.frame_ps != 0 and not head.cut ? last_sample(head.frame, rx) : 0;
       done > ctx_.clock.time().ps)
    {
      ++state_.linked_generation;
      ctx_.events.schedule_at(
          vtime{done}, linked_due, state_.linked_generation, head.lane, ctx_.clock);
      return;
    }
    state_.linked.erase(state_.linked.begin());
    auto const same = head.frame.kind == frame_kind::data
                  and rx.frame_ps == head.frame.frame_ps
                  and rx.word_bits == head.frame.word_bits
                  and rx.half_stops == head.frame.half_stops;
    if(not head.cut and head.frame.kind == frame_kind::brk)
    {
      deliver({.framing = true});
    }
    else if(not head.cut and (same or rx.frame_ps == 0))
    {
      deliver(
          {.data   = head.frame.data,
           .parity = not parity_ok(head.frame.data, rx.word_bits, parity())});
    }
    else if(not head.cut)
    {
      auto const next = std::ranges::find(state_.linked, head.lane, &linked_frame::lane);
      auto const seen = sample(
          head.frame,
          next != state_.linked.end() ? std::optional{next->frame.start.ps} : std::nullopt, rx);
      auto const word = static_cast<std::uint16_t>(seen.data & word_mask());
      deliver(
          {.data    = word,
           .framing = seen.framing,
           .noise   = seen.noise,
           .parity  = not parity_ok(word, rx.word_bits, parity())});
    }
    schedule_linked();
    auto const ps    = frame_ps();
    auto const quiet = not linked_before(head.frame.end.ps + ps);
    if(ps != 0 and quiet and not state_.receiving)
    {
      idle_at(head.frame.end.ps + ps);
    }
    flags();
    return;
  }
  if(kind == cts_due)
  {
    auto&      changes = state_.cts_changes;
    auto const live    = changes.begin() + state_.cts_fired;
    auto const found   = std::ranges::find(live, changes.end(), payload, &cts_change::id);
    if(found != changes.end())
    {
      auto const change = *found;
      if(found == live)
      {
        ++state_.cts_fired;
        if(state_.cts_fired * 2 >= changes.size())
        {
          changes.erase(changes.begin(), changes.begin() + state_.cts_fired);
          state_.cts_fired = 0;
        }
      }
      else
      {
        changes.erase(found);
      }
      auto const base = board_base();
      auto const at   = change.board_ps > base ? change.board_ps - base : 0;
      apply_cts(change.ready, std::min(at, ctx_.clock.time().ps));
    }
    return;
  }
  if(kind == hold_due or kind == release_due)
  {
    auto const found = std::ranges::find_if(state_.holds, [&](line_hold const& hold) {
      return kind == hold_due ? hold.due_event == payload and not hold.sampled
                              : hold.release_event == payload and not hold.open;
    });
    if(found == state_.holds.end())
    {
      return;
    }
    if(kind == hold_due)
    {
      sample_hold(*found);
    }
    else
    {
      found->released = true;
    }
    if(found->sampled and found->released)
    {
      end_hold(found);
    }
    flags();
    return;
  }
  if(kind > idle_due or payload != state_.generation[kind])
  {
    return;
  }
  if(kind == transmit_done)
  {
    state_.shifting   = false;
    auto const data   = state_.shift;
    auto const ending = far_end_ ? tx_frame(state_.tx_end_ps) : line_frame{};
    auto const brk    = std::exchange(state_.break_shifting, false);
    auto const heard  = far_end_ ? far_end_->heard_format() : line_format{};
    hook_guard guard;
    if(far_end_)
    {
      guard.run([&] {
        far_end_->ending(ending);
        if(brk or heard.baud == 0 or same_format(heard, ending))
        {
          far_end_->transmitted(data_of(data, word_bits(), parity()), vtime{state_.tx_end_ps}, brk);
        }
        else
        {
          hear(*far_end_, heard, ending, can_start(state_.tx_end_ps));
        }
      });
    }
    auto next = state_.tx_end_ps;
    if(brk)
    {
      update(regs_, break_flag_, 0);
      next += break_stop_half_bits_
            * frame_ps()
            / half_bits(word_bits(), half_stop_codes[get(regs_, stop_)]);
      state_.tx_end_ps = next;
    }
    auto const data_sent = not brk and state_.tx_fifo.empty();
    auto const kept_low  = break_keeps_tc_low_ and state_.break_pending and not clear_to_send(next);
    start_frame(next);
    auto const held =
        (state_.break_pending and (brk or break_keeps_tc_low_)) or not state_.tx_fifo.empty();
    if((data_sent and not kept_low)
       or (not state_.shifting and not(held and not clear_to_send(next))))
    {
      update(regs_, complete_, 1);
    }
    flags();
    guard.rethrow();
    return;
  }
  if(kind == idle_due)
  {
    state_.idle_pending = false;
    if(not state_.receiving)
    {
      update(regs_, idle_, 1);
    }
    flags();
    return;
  }
  state_.receiving = false;
  auto const frame = next_host_frame();
  if(not frame)
  {
    flags();
    return;
  }
  auto const own      = own_format(*frame);
  auto const now      = ctx_.clock.time().ps;
  auto       guest_rx = shape();
  guest_rx.frame_ps   = state_.rx_guest_ps;
  auto const planned  = own ? std::max(state_.rx_due_ps, stop_sample(guest_rx, state_.rx_start_ps))
                            : state_.rx_due_ps;
  auto const sampled_late = planned > state_.rx_due_ps;
  auto const follows      = own
                   and not state_.host.empty()
                   and state_.host.front().start_ps <= planned
                   and (state_.host.front().ignores_rts or ready_to_receive());
  if(far_end_)
  {
    far_end_->received(
        {.data      = frame->data,
         .word_bits = static_cast<std::uint8_t>(
             own ? 8u + (frame->line.parity == line_parity::none ? 0u : 1u) : word_bits()),
         .half_stops = static_cast<std::uint8_t>(
             own ? frame->line.half_stops : half_stop_codes[get(regs_, stop_)]),
         .parity   = own ? frame->line.parity : parity(),
         .kind     = frame->kind,
         .frame_ps = state_.rx_due_ps - state_.rx_start_ps,
         .start    = vtime{state_.rx_start_ps},
         .end      = vtime{state_.rx_due_ps}});
  }
  deliver(
      own ? sampled_entry(
                *frame,
                follows ? std::optional{std::max(state_.host.front().start_ps, state_.rx_due_ps)}
                        : std::nullopt)
          : entry_of(*frame));
  note_rts(planned);
  auto const ps   = frame_ps();
  auto const hold = frame->kind == frame_kind::brk
                      ? 4 * ps / half_bits(word_bits(), half_stop_codes[get(regs_, stop_)])
                      : 0;
  auto const chained =
      sampled_late
          ? follows
          : not state_.host.empty() and head_ready() and state_.host.front().start_ps <= now;
  if(ps != 0 and chained)
  {
    state_.receiving = true;
    state_.rx_due_ps = schedule_receive(
        std::max(state_.rx_due_ps + hold, state_.host.front().start_ps), state_.host.front(), ps);
  }
  else if(ps != 0 and not state_.idle_pending and not linked_before(state_.rx_due_ps + ps))
  {
    state_.idle_pending = true;
    ++state_.generation[idle_due];
    state_.idle_due_ps = schedule(idle_due, state_.rx_due_ps, ps);
  }
  if(not state_.receiving)
  {
    state_.rx_due_ps += hold;
    resume_host();
  }
  flags();
}

auto usart::linked_before(std::uint64_t due_ps) const -> bool
{
  return std::ranges::any_of(
             state_.linked,
             [due_ps](linked_frame const& queued) { return queued.frame.start.ps < due_ps; })
      or std::ranges::any_of(
             state_.holds, [due_ps](line_hold const& hold) { return hold.from_ps < due_ps; });
}

void usart::schedule_hold(line_hold& hold, std::uint64_t due_ps)
{
  hold.due_event = ++state_.hold_events;
  ctx_.events.schedule_at(vtime{due_ps}, hold_due, hold.due_event, hold.lane, ctx_.clock);
}

void usart::sample_hold(line_hold& hold)
{
  hold.sampled   = true;
  auto const rx  = shape();
  auto const low = [&](std::uint64_t at) { return hold.open or at < hold.until_ps; };
  if(rx.frame_ps == 0)
  {
    hold.received = true;
    deliver({.framing = true});
    return;
  }
  if(not low(sample_at(rx, hold.start_ps, 0, 8)))
  {
    return;
  }
  hold.received = true;
  sampled out{};
  auto    all_low = true;
  for(std::uint32_t bit = 1; bit <= rx.word_bits + 1; ++bit)
  {
    std::uint32_t ones = 0;
    for(std::size_t index = 0; index < rx.samples; ++index)
    {
      ones += low(sample_at(rx, hold.start_ps, bit, rx.offsets[index])) ? 0u : 1u;
    }
    all_low          = all_low and ones == 0;
    auto const value = 2 * ones > rx.samples ? 1u : 0u;
    out.noise        = out.noise or (ones != 0 and ones != rx.samples);
    if(bit <= rx.word_bits)
    {
      out.data = static_cast<std::uint16_t>(out.data | (value << (bit - 1)));
    }
    else
    {
      out.framing = value == 0;
    }
  }
  if(all_low)
  {
    deliver({.framing = true});
    return;
  }
  auto const word = static_cast<std::uint16_t>(out.data & word_mask());
  deliver(
      {.data    = word,
       .framing = out.framing,
       .noise   = out.noise,
       .parity  = not parity_ok(word, rx.word_bits, parity())});
}

void usart::end_hold(std::vector<line_hold>::iterator hold)
{
  auto const ps       = frame_ps();
  auto const char_end = hold->received ? hold->start_ps + ps : 0;
  auto const due      = std::max(hold->until_ps, char_end) + ps;
  state_.holds.erase(hold);
  if(ps != 0 and not linked_before(due) and not state_.receiving)
  {
    idle_at(due);
  }
}

void usart::hold_line(vtime from, std::uint32_t lane)
{
  if(not state_.holds.empty())
  {
    auto& last = state_.holds.back();
    if(not last.open and not last.released and last.until_ps == from.ps)
    {
      last.open = true;
      return;
    }
  }
  auto const rx = shape();
  if(state_.idle_pending and from.ps < state_.idle_due_ps)
  {
    state_.idle_pending = false;
    ++state_.generation[idle_due];
  }
  line_hold  hold{.from_ps = from.ps, .start_ps = from.ps, .lane = lane, .open = true};
  auto const crossing = std::ranges::find_if(state_.linked, [&](linked_frame const& queued) {
    return queued.frame.start.ps < from.ps
       and rx.frame_ps != 0
       and from.ps <= last_sample(queued.frame, rx)
       and (not queued.cut or from.ps < queued.frame.end.ps);
  });
  if(crossing != state_.linked.end())
  {
    hold.start_ps       = crossing->frame.start.ps;
    auto const was_head = crossing == state_.linked.begin();
    state_.linked.erase(crossing);
    if(was_head)
    {
      schedule_linked();
    }
  }
  auto const due = rx.frame_ps != 0 ? stop_sample(rx, hold.start_ps) : from.ps;
  schedule_hold(state_.holds.emplace_back(hold), due);
}

void usart::release_line(vtime at, std::uint32_t)
{
  if(state_.holds.empty() or not state_.holds.back().open)
  {
    return;
  }
  auto& hold         = state_.holds.back();
  hold.open          = false;
  hold.until_ps      = at.ps;
  hold.release_event = ++state_.hold_events;
  ctx_.events.schedule_at(at, release_due, hold.release_event, hold.lane, ctx_.clock);
}

void usart::deliver(rx_entry entry)
{
  auto const depth = fifo() ? fifo_depth : 1;
  if(not enabled() or get(regs_, receive_) == 0)
  {
    return;
  }
  if(state_.rx_fifo.size() < depth)
  {
    state_.rx_fifo.push_back(entry);
    if(state_.rx_fifo.size() == 1)
    {
      expose();
    }
  }
  else if(bit(overrun_disable_) == 0)
  {
    update(regs_, overrun_, 1);
  }
}

void usart::expose()
{
  if(state_.rx_fifo.empty())
  {
    return;
  }
  auto const& head = state_.rx_fifo.front();
  if(head.framing)
  {
    update(regs_, framing_, 1);
  }
  if(head.noise)
  {
    update(regs_, noise_, 1);
  }
  if(head.parity)
  {
    update(regs_, parity_, 1);
  }
}

auto usart::next_host_frame() -> std::optional<host_frame>
{
  if(state_.host.empty())
  {
    return std::nullopt;
  }
  auto const frame = state_.host.front();
  state_.host.erase(state_.host.begin());
  return frame;
}

auto usart::own_format(host_frame const& frame) -> bool
{
  if(frame.kind != frame_kind::data or frame.line.baud == 0)
  {
    return false;
  }
  auto const own_ps = frame_ps_of(frame.line);
  auto const gap =
      own_ps > state_.rx_guest_ps ? own_ps - state_.rx_guest_ps : state_.rx_guest_ps - own_ps;
  auto const same = 8u + (frame.line.parity == line_parity::none ? 0u : 1u) == word_bits()
                and frame.line.parity == parity()
                and frame.line.half_stops == half_stop_codes[get(regs_, stop_)]
                and gap <= 1;
  return not same;
}

auto usart::sampled_entry(host_frame const& frame, std::optional<std::uint64_t> next_start)
    -> rx_entry
{
  auto const       bits = 8u + (frame.line.parity == line_parity::none ? 0u : 1u);
  line_frame const sent{
      .data       = with_parity(frame.data, bits, frame.line.parity),
      .word_bits  = static_cast<std::uint8_t>(bits),
      .half_stops = static_cast<std::uint8_t>(frame.line.half_stops),
      .parity     = frame.line.parity,
      .kind       = frame_kind::data,
      .frame_ps   = frame_ps_of(frame.line),
      .start      = vtime{state_.rx_start_ps},
      .end        = vtime{state_.rx_due_ps}};
  auto rx         = shape();
  rx.frame_ps     = state_.rx_guest_ps;
  auto const seen = sample(sent, next_start, rx);
  auto const word = static_cast<std::uint16_t>(seen.data & word_mask());
  return {
      .data    = word,
      .framing = seen.framing,
      .noise   = seen.noise,
      .parity  = not parity_ok(word, rx.word_bits, parity())};
}

auto usart::entry_of(host_frame const& frame) -> rx_entry
{
  if(frame.kind == frame_kind::brk)
  {
    return {.framing = true};
  }
  auto const bits = word_bits();
  auto       word = with_parity(frame.data, bits, parity());
  auto const odd  = frame.error == uart_error::parity;
  if(odd and parity() == line_parity::none)
  {
    if(not std::exchange(parity_tag_logged_, true))
    {
      warn(
          "CR1",
          "a host frame tagged with a parity error reached a receiver without PCE: delivered "
          "clean");
    }
  }
  else if(odd)
  {
    word = static_cast<std::uint16_t>(word ^ (1u << (bits - 1)));
  }
  return {
      .data    = word,
      .framing = frame.error == uart_error::framing,
      .noise   = frame.error == uart_error::noise,
      .parity  = not parity_ok(word, bits, parity())};
}

void usart::schedule_linked()
{
  ++state_.linked_generation;
  if(state_.linked.empty())
  {
    return;
  }
  auto const& head = state_.linked.front();
  ctx_.events.schedule_at(
      head.frame.end, linked_due, state_.linked_generation, head.lane, ctx_.clock);
}

void usart::idle_at(std::uint64_t due_ps)
{
  state_.idle_pending = true;
  ++state_.generation[idle_due];
  state_.idle_due_ps = schedule(idle_due, due_ps, 0);
}

void usart::receive_at(line_frame const& frame, std::uint32_t lane)
{
  if(state_.idle_pending and frame.start.ps < state_.idle_due_ps)
  {
    state_.idle_pending = false;
    ++state_.generation[idle_due];
  }
  auto const rx   = shape();
  auto const over = std::ranges::find_if(state_.holds, [&](line_hold const& hold) {
    return not hold.sampled
       and hold.start_ps == hold.from_ps
       and frame.start.ps < hold.from_ps
       and rx.frame_ps != 0
       and hold.from_ps <= last_sample(frame, rx);
  });
  if(over != state_.holds.end())
  {
    over->start_ps = frame.start.ps;
    schedule_hold(*over, stop_sample(rx, frame.start.ps));
    return;
  }
  auto const key = std::pair{frame.end.ps, lane};
  auto const where =
      std::ranges::upper_bound(state_.linked, key, {}, [](linked_frame const& queued) {
        return std::pair{queued.frame.end.ps, queued.lane};
      });
  auto const head = where == state_.linked.begin();
  state_.linked.insert(where, {.frame = frame, .lane = lane});
  if(head)
  {
    schedule_linked();
  }
}

void usart::cancel_in_flight(vtime at, std::uint32_t lane)
{
  auto const found = std::ranges::find_if(state_.linked, [&](linked_frame const& queued) {
    return queued.lane == lane
       and not queued.cut
       and queued.frame.start.ps < at.ps
       and at.ps < queued.frame.end.ps;
  });
  if(found == state_.linked.end())
  {
    return;
  }
  auto cut      = *found;
  auto was_head = found == state_.linked.begin();
  state_.linked.erase(found);
  cut.frame.end  = at;
  cut.cut        = true;
  auto const key = std::pair{at.ps, lane};
  auto const where =
      std::ranges::upper_bound(state_.linked, key, {}, [](linked_frame const& queued) {
        return std::pair{queued.frame.end.ps, queued.lane};
      });
  auto const is_head = where == state_.linked.begin();
  state_.linked.insert(where, cut);
  if(was_head or is_head)
  {
    schedule_linked();
  }
}

void usart::receive_frames(std::span<host_frame const> frames)
{
  state_.host.insert(state_.host.end(), frames.begin(), frames.end());
  if(state_.receiving)
  {
    return;
  }
  resume_host();
  if(state_.receiving)
  {
    flags();
  }
}

void usart::note_rts(std::uint64_t at_ps)
{
  auto const ready = ready_to_receive();
  if(std::exchange(state_.rts_level, ready) == ready)
  {
    return;
  }
  if(far_end_ and far_end_->hears_rts())
  {
    ctx_.events.schedule_at(
        vtime{at_ps}, rts_due, at_ps << 1 | (ready ? 1u : 0u), core::host_lane, ctx_.clock);
  }
}

auto usart::head_ready() const -> bool
{
  return ready_to_receive() or state_.host.front().ignores_rts;
}

void usart::resume_host()
{
  note_rts(ctx_.clock.time().ps);
  if(state_.receiving or state_.host.empty())
  {
    return;
  }
  auto const ps = head_ready() ? frame_ps() : 0;
  if(ps == 0)
  {
    state_.rx_held = true;
    return;
  }
  auto const now   = ctx_.clock.time().ps;
  auto const start = state_.host.front().start_ps;
  if(start > now)
  {
    state_.rx_held = false;
    if(std::exchange(state_.host_start_ps, start) != start)
    {
      ctx_.events.schedule_at(vtime{start}, host_start, 0, core::host_lane, ctx_.clock);
    }
    return;
  }
  state_.receiving = true;
  ++state_.generation[idle_due];
  state_.idle_pending = false;
  auto const from     = start != 0 and not state_.rx_held ? start : now;
  state_.rx_held      = false;
  state_.rx_due_ps    = schedule_receive(std::max(from, state_.rx_due_ps), state_.host.front(), ps);
}

auto usart::rx_space() const -> bool
{
  return state_.rx_fifo.size() < (fifo() ? fifo_depth : 1);
}

auto usart::ready_to_receive() const -> bool
{
  return not rts_on() or bit(driver_enable_) != 0 or rx_space();
}

auto usart::clear_to_send(std::uint64_t start_ps) const -> bool
{
  return not cts_on() or state_.cts_ready or start_ps < state_.cts_hold_from_ps;
}

auto usart::board_base() const -> std::uint64_t
{
  return ctx_.clock.board_time().ps - ctx_.clock.time().ps;
}

void usart::set_cts(bool ready, vtime at)
{
  auto const now = ctx_.clock.time().ps;
  if(at.ps < now)
  {
    throw std::invalid_argument(
        std::format("a CTS change at {} ps lies before the machine time {} ps", at.ps, now));
  }
  if(pending_cts() >= max_pending_cts)
  {
    throw config_error(
        desc_.name, 0, "cts", std::format("at most {} pending CTS changes", max_pending_cts));
  }
  schedule_cts(ready, at);
}

void usart::set_cts_stamped(bool ready, vtime at)
{
  if(pending_cts() >= max_pending_cts)
  {
    throw config_error(
        desc_.name, 0, "cts", std::format("at most {} pending CTS changes", max_pending_cts));
  }
  schedule_cts(ready, at);
}

auto usart::has_flow_control() const -> bool
{
  return flow_control_;
}

void usart::schedule_cts(bool ready, vtime at)
{
  auto const base = board_base();
  if(at.ps > std::numeric_limits<std::uint64_t>::max() - base)
  {
    throw std::invalid_argument("a CTS change time leaves the 64-bit picosecond range");
  }
  auto const change = cts_change{.board_ps = base + at.ps, .id = ++state_.next_cts, .ready = ready};
  state_.cts_changes.insert(
      std::ranges::upper_bound(
          state_.cts_changes.begin() + state_.cts_fired, state_.cts_changes.end(), change.board_ps,
          {}, &cts_change::board_ps),
      change);
  ctx_.events.schedule_at(at, cts_due, change.id, core::host_lane, ctx_.clock);
}

auto usart::pending_cts() const -> std::size_t
{
  return state_.cts_changes.size() - state_.cts_fired;
}

auto usart::cts_ready() const -> bool
{
  return state_.cts_ready;
}

void usart::apply_cts(bool ready, std::uint64_t at_ps)
{
  if(std::exchange(state_.cts_ready, ready) == ready)
  {
    return;
  }
  if(not ready)
  {
    state_.cts_hold_from_ps = at_ps + 1;
  }
  if(cts_on())
  {
    update(regs_, cts_flag_, 1);
  }
  start_frame(std::max(at_ps, state_.tx_end_ps));
  flags();
}

auto usart::queued() const -> std::size_t
{
  return state_.host.size();
}

void usart::attach(std::shared_ptr<uart_endpoint> far_end)
{
  far_end_ = std::move(far_end);
}
}

auto usart_factory(decltype(board::uarts) const& uarts_, usart_variant variant) -> model_factory
{
  return [uarts = uarts_,
          variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    auto const found = uarts.find(desc.name);
    return std::make_shared<usart>(
        ctx, desc, found != uarts.end() ? found->second : nullptr, variant);
  };
}

namespace
{
constexpr auto usart_row_table = std::to_array<builtin>({
    row_of<^^usart_factory, ^^with_views_usart, ^^board::uarts>(
        {.ip = "USART", .ip_version_glob = "sci3_v2*", .families = family::u5}),
    row_of<^^usart_factory, ^^h5_usart, ^^board::uarts>(
        {.ip = "USART", .ip_version_glob = "sci3_v2*", .families = family::h5}),
    row_of<^^usart_factory, ^^with_views_usart, ^^board::uarts>(
        {.ip = "UART", .ip_version_glob = "sci2_*", .families = family::u5}),
    row_of<^^usart_factory, ^^h5_usart, ^^board::uarts>(
        {.ip = "UART", .ip_version_glob = "sci2_*", .families = family::h5}),
    row_of<^^usart_factory, ^^l1_usart, ^^board::uarts>(
        {.ip = "USART", .ip_version_glob = "sci2_*", .families = family::l1}),
    row_of<^^usart_factory, ^^l1_uart, ^^board::uarts>(
        {.ip = "UART", .ip_version_glob = "sci2_*", .families = family::l1}),
    row_of<^^usart_factory, ^^l4_usart, ^^board::uarts>(
        {.ip = "USART", .ip_version_glob = "sci3_v1*", .families = family::l4}),
    row_of<^^usart_factory, ^^l4_usart, ^^board::uarts>(
        {.ip = "UART", .ip_version_glob = "sci2_v2*", .families = family::l4}),
    row_of<^^usart_factory, ^^u5_lpuart, ^^board::uarts>(
        {.ip = "LPUART", .ip_version_glob = "sci3_v1*", .families = family::u5}),
    row_of<^^usart_factory, ^^h5_lpuart, ^^board::uarts>(
        {.ip = "LPUART", .ip_version_glob = "sci3_v1*", .families = family::h5}),
    row_of<^^usart_factory, ^^l4_lpuart, ^^board::uarts>(
        {.ip = "LPUART", .ip_version_glob = "sci3_v1*", .families = family::l4}),
});
}

auto usart_rows() -> std::span<builtin const>
{
  return usart_row_table;
}
}
