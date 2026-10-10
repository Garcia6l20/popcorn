#include "dma_signal.hpp"
#include "factories.hpp"
#include "model_base.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/uart.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
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
constexpr std::uint32_t transmit_done = 0;
constexpr std::uint32_t receive_due   = 1;
constexpr std::uint32_t idle_due      = 2;
constexpr std::uint32_t linked_due    = 3;
constexpr std::size_t   fifo_depth    = 8;
constexpr std::uint64_t ps_per_second = 1'000'000'000'000;

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

auto last_sample(line_frame const& sent, rx_shape const& rx) -> std::uint64_t
{
  return sample_at(rx, sent.start.ps, rx.word_bits + 1, rx.offsets[rx.samples - 1]);
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

struct linked_frame
{
  line_frame    frame{};
  std::uint32_t lane{};

  friend auto operator==(linked_frame const&, linked_frame const&) -> bool = default;
};

struct usart_state
{
  std::array<std::uint32_t, 3> generation{};
  std::vector<std::uint16_t>   tx_fifo;
  std::vector<std::uint16_t>   rx_fifo;
  std::vector<std::uint16_t>   host;
  std::uint16_t                shift{};
  std::uint64_t                tx_end_ps{};
  std::uint64_t                rx_due_ps{};
  bool                         shifting{};
  bool                         receiving{};
  bool                         idle_pending{};
  std::vector<linked_frame>    linked;
  std::uint64_t                idle_due_ps{};
  std::uint32_t                linked_generation{};
  bool                         break_pending{};
  bool                         break_shifting{};

  friend auto operator==(usart_state const&, usart_state const&) -> bool = default;
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
constexpr std::array<std::string_view, 14> usart_cr3{"TXFTCFG", "RXFTCFG", "DEP",  "DEM",  "DDRE",
                                                     "OVRDIS",  "ONEBIT",  "CTSE", "RTSE", "SCEN",
                                                     "NACK",    "HDSEL",   "IRLP", "IREN"};
constexpr std::array<std::string_view, 7>  lpuart_cr1{"M1",   "M0",  "DEAT", "DEDT",
                                                      "WAKE", "PCE", "PS"};
constexpr std::array<std::string_view, 8>  lpuart_cr2{"ADD",   "ADDM7", "MSBFIRST", "DATAINV",
                                                      "TXINV", "RXINV", "SWAP",     "STOP"};
constexpr std::array<std::string_view, 9>  lpuart_cr3{"TXFTCFG", "RXFTCFG", "DEP",  "DEM",  "DDRE",
                                                      "OVRDIS",  "CTSE",    "RTSE", "HDSEL"};
constexpr std::array<std::string_view, 1>  whole_brr{"BRR"};
constexpr std::array<std::string_view, 1>  whole_presc{"PRESCALER"};

struct lock
{
  std::string   reg;
  std::uint32_t offset{};
  std::uint32_t mask{};
  bool          logged{};
};

/**
 * @brief USART, UART and LPUART of the STM32U5, RM0456 sections 66 and 67, asynchronous mode.
 *        Frames leave and arrive one frame time apart at the baud the kernel clock, PRESC and BRR
 *        give, read when each frame starts. FIFO mode, 8 frames each way, or single registers.
 *        TX frames reach the port's endpoint when their stop bit ends. RX frames the board queues
 *        arrive one frame time apart, an overrun drops the frame and sets ORE, IDLE follows one
 *        idle frame. With CR3.DMAT the `<instance>_tx_dma` request follows TXE, with CR3.DMAR the
 *        `<instance>_rx_dma` request follows RXNE, through the GPDMA peer, levels announced again
 *        in reset. With UE 1, a CPU or DMA write leaves the fields RM0456 66.8 and 67.7 lock
 *        while the USART is enabled at their value: CR1, CR2 and CR3 fields per instance kind,
 *        BRR and PRESC whole, ABRMOD while ABREN is 1, and SCARCNT unless the write is 0. The
 *        write logs once per register, a model_error under strict peripherals. A write that
 *        clears UE takes effect whole, which RM0456 leaves open, and debugger writes are never
 *        locked. GTPR and AUTOCR are not served, so their locked fields are not enforced. Not
 *        modelled, logged once when configured: synchronous mode, smartcard,
 *        IrDA, LIN, the RS485 driver enable, auto baud rate, receiver timeout, wake-up from Stop.
 *        For a link to another machine, the endpoint hears each frame when its start bit leaves,
 *        `uart_endpoint::started`, and a clear of UE with a frame in flight, `aborted`. Frames
 *        from another machine, `receive_at`, reach the receiver at their own stop-bit end, IDLE
 *        one receiver frame time after the last one unless the next starts before. Deviations:
 *        an aborted frame vanishes, the receiver sees the line go idle at the abort, where the
 *        chip would see a truncated frame and FE. A reset drops the frames queued from other
 *        machines, also those whose start bit would reach the line after the reset. Events a
 *        delivery from another machine arms, its data and the IDLE of an abort, go on the
 *        wire's lane, so their order against local events of the same instant does not depend on
 *        when the delivery happened. BUSY does not follow frames from other machines. A frame
 *        from another machine whose bit time,
 *        word length or stop length differs from the receiver's is sampled bit by bit on the
 *        sender's ideal waveform at the receiver's bit time, three samples per bit at 7, 8 and 9
 *        sixteenths with OVER16, 3, 4 and 5 eighths with OVER8, the middle one with ONEBIT or on
 *        an LPUART: a disagreement sets NE, a stop bit sampled 0 sets FE, the sampled word enters
 *        RDR. The receiver resynchronises on each sender start bit: a falling edge inside a later
 *        frame is never taken for a start bit. Parity is not modelled: PCE logs a warning once,
 *        a model_error under strict_peripherals. RQR.SBKRQ sets SBKF and sends a break after
 *        the frame in flight, ahead of the TX FIFO: a frame time low, then 2 bits high before
 *        the next frame, SBKF clearing as the low part ends. A break from another machine reads
 *        as 0 with FE, LBDF is LIN only and not modelled.
 */
class usart final : public dispatch_model<usart, usart_state>, public uart_port
{
public:
  usart(model_context& ctx, peripheral_desc const& desc, std::shared_ptr<uart_endpoint> far_end);

  void reset(register_file& regs, reset_kind kind) override;
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

  void               receive(std::span<std::uint16_t const> data) override;
  [[nodiscard]] auto queued() const -> std::size_t override;
  void               attach(std::shared_ptr<uart_endpoint> far_end) override;
  void               receive_at(line_frame const& frame, std::uint32_t lane) override;
  void               cancel_in_flight(vtime at, std::uint32_t lane) override;

  void control_written(std::uint32_t old, std::uint32_t now);
  void clear_written(std::uint32_t old, std::uint32_t now);
  void request_written(std::uint32_t old, std::uint32_t now);
  void transmit_written(std::uint32_t old, std::uint32_t now);
  void plain_written(std::uint32_t old, std::uint32_t now);

private:
  [[nodiscard]] auto name_of(std::string_view with_views, std::string_view plain) const
      -> std::string_view;
  [[nodiscard]] auto enabled() const -> bool;
  [[nodiscard]] auto fifo() const -> bool;
  [[nodiscard]] auto frame_ps() -> std::uint64_t;
  [[nodiscard]] auto word_bits() const -> std::uint32_t;
  [[nodiscard]] auto word_mask() const -> std::uint32_t;
  [[nodiscard]] auto shape() -> rx_shape;
  void               warn_parity();
  auto schedule(std::uint32_t kind, std::uint64_t from_ps, std::uint64_t ps) -> std::uint64_t;
  void start_frame(std::uint64_t from_ps);
  void deliver(std::uint16_t data, bool framing = false, bool noise = false);
  void schedule_linked();
  void idle_at(std::uint64_t due_ps, std::uint32_t lane = 0);
  [[nodiscard]] auto linked_before(std::uint64_t due_ps) const -> bool;
  void               flags();
  void               interrupts();
  void               warn_unsupported();

  register_file&                               regs_;
  std::shared_ptr<uart_endpoint>               far_end_;
  bool                                         low_power_;
  std::string                                  cr1_;
  std::string                                  isr_;
  reg_ref                                      rdr_;
  reg_ref                                      tdr_;
  reg_ref                                      icr_;
  field_ref                                    enable_;
  field_ref                                    transmit_;
  field_ref                                    receive_;
  field_ref                                    fifo_enable_;
  field_ref                                    word_low_;
  field_ref                                    word_high_;
  std::optional<field_ref>                     oversampling_;
  field_ref                                    stop_;
  field_ref                                    overrun_disable_;
  field_ref                                    baud_;
  field_ref                                    prescaler_;
  field_ref                                    data_;
  field_ref                                    tx_not_full_;
  field_ref                                    tx_empty_;
  field_ref                                    complete_;
  field_ref                                    rx_not_empty_;
  field_ref                                    rx_full_;
  field_ref                                    idle_;
  field_ref                                    overrun_;
  field_ref                                    noise_;
  field_ref                                    framing_;
  field_ref                                    parity_;
  field_ref                                    tx_ack_;
  field_ref                                    rx_ack_;
  field_ref                                    busy_;
  field_ref                                    tx_not_full_ie_;
  field_ref                                    tx_empty_ie_;
  field_ref                                    complete_ie_;
  field_ref                                    rx_not_empty_ie_;
  field_ref                                    rx_full_ie_;
  field_ref                                    idle_ie_;
  field_ref                                    parity_ie_;
  field_ref                                    error_ie_;
  field_ref                                    tx_threshold_;
  field_ref                                    dma_transmit_;
  field_ref                                    dma_receive_;
  dma_signal                                   tx_dma_;
  dma_signal                                   rx_dma_;
  field_ref                                    rx_threshold_;
  field_ref                                    tx_threshold_ie_;
  field_ref                                    rx_threshold_ie_;
  field_ref                                    tx_threshold_flag_;
  field_ref                                    rx_threshold_flag_;
  field_ref                                    flush_rx_;
  field_ref                                    flush_tx_;
  field_ref                                    parity_enable_;
  field_ref                                    break_request_;
  field_ref                                    break_flag_;
  std::optional<field_ref>                     one_bit_;
  std::vector<std::pair<field_ref, field_ref>> clears_;
  std::vector<field_ref>                       unsupported_;
  core::clock_domain                           kernel_;
  irq_line&                                    line_;
  bool                                         unsupported_logged_{};
  bool                                         no_baud_logged_{};
  std::vector<lock>                            locks_;
  std::optional<field_ref>                     auto_baud_;
  std::optional<field_ref>                     auto_baud_mode_;
  std::optional<field_ref>                     retransmissions_;
  bool                                         parity_logged_{};
};

auto usart_writes(bool low_power) -> std::span<write_entry<usart> const>
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
  return low_power ? std::span<write_entry<usart> const>{plain} : with_views;
}

auto lower(std::string_view name) -> std::string
{
  std::string out{name};
  std::ranges::transform(
      out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

usart::usart(
    model_context&                 ctx,
    peripheral_desc const&         desc,
    std::shared_ptr<uart_endpoint> far_end)
    : dispatch_model{ctx, desc, desc.ip, usart_writes(desc.ip == "LPUART")}, regs_{ctx.regs},
      far_end_{std::move(far_end)}, low_power_{desc.ip == "LPUART"},
      cr1_{name_of("CR1_enabled", "CR1")}, isr_{name_of("ISR_enabled", "ISR")}, rdr_{reg("RDR")},
      tdr_{reg("TDR")}, icr_{reg("ICR")}, enable_{field(cr1_, "UE")}, transmit_{field(cr1_, "TE")},
      receive_{field(cr1_, "RE")}, fifo_enable_{field(cr1_, "FIFOEN")},
      word_low_{field(cr1_, "M0")}, word_high_{field(cr1_, "M1")},
      oversampling_{low_power_ ? std::nullopt : std::optional{field(cr1_, "OVER8")}},
      stop_{field("CR2", "STOP")}, overrun_disable_{field("CR3", "OVRDIS")},
      baud_{field("BRR", "BRR")}, prescaler_{field("PRESC", "PRESCALER")},
      data_{field("RDR", "RDR")}, tx_not_full_{field(isr_, "TXFNF")},
      tx_empty_{field(isr_, "TXFE")}, complete_{field(isr_, "TC")},
      rx_not_empty_{field(isr_, "RXFNE")}, rx_full_{field(isr_, "RXFF")},
      idle_{field(isr_, "IDLE")}, overrun_{field(isr_, "ORE")}, noise_{field(isr_, "NE")},
      framing_{field(isr_, "FE")}, parity_{field(isr_, "PE")}, tx_ack_{field(isr_, "TEACK")},
      rx_ack_{field(isr_, "REACK")}, busy_{field(isr_, "BUSY")},
      tx_not_full_ie_{field(cr1_, "TXFNFIE")}, tx_empty_ie_{field(cr1_, "TXFEIE")},
      complete_ie_{field(cr1_, "TCIE")}, rx_not_empty_ie_{field(cr1_, "RXFNEIE")},
      rx_full_ie_{field(cr1_, "RXFFIE")}, idle_ie_{field(cr1_, "IDLEIE")},
      parity_ie_{field(cr1_, "PEIE")}, error_ie_{field("CR3", "EIE")},
      tx_threshold_{field("CR3", "TXFTCFG")}, dma_transmit_{field("CR3", "DMAT")},
      dma_receive_{field("CR3", "DMAR")}, tx_dma_{ctx.peers, lower(desc.name) + "_tx_dma"},
      rx_dma_{ctx.peers, lower(desc.name) + "_rx_dma"}, rx_threshold_{field("CR3", "RXFTCFG")},
      tx_threshold_ie_{field("CR3", "TXFTIE")}, rx_threshold_ie_{field("CR3", "RXFTIE")},
      tx_threshold_flag_{field(isr_, "TXFT")}, rx_threshold_flag_{field(isr_, "RXFT")},
      flush_rx_{field("RQR", "RXFRQ")}, flush_tx_{field("RQR", "TXFRQ")},
      parity_enable_{field(cr1_, "PCE")}, break_request_{field("RQR", "SBKRQ")},
      break_flag_{field(isr_, "SBKF")},
      one_bit_{
          std::ranges::contains(
              describe("CR3").fields,
              std::string_view{"ONEBIT"},
              &field_desc::name)
              ? std::optional{field("CR3", "ONEBIT")}
              : std::nullopt},
      kernel_{ctx.clock.domain(lower(desc.name))}, line_{irq(desc.name + "_IRQn")}
{
  for(auto const& clear : describe("ICR").fields)
  {
    auto const target = std::string_view{clear.name}.substr(0, clear.name.size() - 2);
    if(clear.name.ends_with("CF")
       and std::ranges::contains(describe(isr_).fields, target, &field_desc::name))
    {
      clears_.emplace_back(field("ICR", clear.name), field(isr_, target));
    }
  }
  auto const tables = low_power_ ? std::array<locked_fields, 5>{{{cr1_, lpuart_cr1},
                                                                 {"CR2", lpuart_cr2},
                                                                 {"CR3", lpuart_cr3},
                                                                 {"BRR", whole_brr},
                                                                 {"PRESC", whole_presc}}}
                                 : std::array<locked_fields, 5>{{{cr1_, usart_cr1},
                                                                 {"CR2", usart_cr2},
                                                                 {"CR3", usart_cr3},
                                                                 {"BRR", whole_brr},
                                                                 {"PRESC", whole_presc}}};
  for(auto const& [reg_name, names] : tables)
  {
    auto& entry  = locks_.emplace_back();
    entry.reg    = reg_name == cr1_ ? std::string{"CR1"} : std::string{reg_name};
    entry.offset = reg(reg_name).offset;
    for(auto const& bits : describe(reg_name).fields)
    {
      if(std::ranges::contains(names, std::string_view{bits.name}))
      {
        entry.mask |= field(reg_name, bits.name).mask();
      }
    }
  }
  auto const optional = [&](std::string_view reg_name, std::string_view bits) {
    return std::ranges::contains(describe(reg_name).fields, bits, &field_desc::name)
             ? std::optional{field(reg_name, bits)}
             : std::nullopt;
  };
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
}

auto usart::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     access) -> std::expected<void, bus_error>
{
  auto const word  = offset & ~3u;
  auto const found = std::ranges::find(locks_, word, &lock::offset);
  if(found == locks_.end() or access.by == initiator::debugger or not enabled())
  {
    return dispatch_model::on_write(regs, offset, size, old, value, access);
  }
  auto const stored = regs.peek(word);
  auto const merged = merge_write(stored, offset, size, value);
  if(word == enable_.offset and enable_.of(merged) == 0)
  {
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
  auto const kept = (merged & ~mask) | (stored & mask);
  if(kept != merged)
  {
    if(not std::exchange(found->logged, true))
    {
      warn(
          found->reg,
          std::format(
              "{} written with UE 1: RM0456 lets these fields change only with UE 0, they keep "
              "their value",
              found->reg));
    }
    if(ctx_.strict_peripherals)
    {
      throw model_error(desc_.name, found->reg, word);
    }
  }
  return dispatch_model::on_write(regs, word, 4, old, kept, access);
}

auto usart::name_of(std::string_view with_views, std::string_view plain) const -> std::string_view
{
  return has_register(with_views) ? with_views : plain;
}

void usart::reset(register_file& regs, reset_kind)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
  for(auto& generation : state_.generation)
  {
    ++generation;
  }
  state_.tx_fifo.clear();
  state_.rx_fifo.clear();
  state_.host.clear();
  state_.linked.clear();
  state_.break_pending  = false;
  state_.break_shifting = false;
  state_.shifting       = false;
  state_.receiving      = false;
  state_.idle_pending   = false;
  tx_dma_.resolve();
  rx_dma_.resolve();
  flags();
}

auto usart::enabled() const -> bool
{
  return get(regs_, enable_) != 0;
}

auto usart::fifo() const -> bool
{
  return get(regs_, fifo_enable_) != 0;
}

auto usart::frame_ps() -> std::uint64_t
{
  auto const    code = get(regs_, prescaler_);
  auto const    hz  = ctx_.clock.hz(kernel_) / (code < prescalers.size() ? prescalers[code] : 256u);
  auto const    brr = get(regs_, baud_);
  std::uint64_t baud = 0;
  if(brr != 0 and hz != 0)
  {
    if(low_power_)
    {
      baud = brr < 0x300 ? 0 : 256 * hz / brr;
    }
    else if(get(regs_, *oversampling_) != 0)
    {
      baud = 2 * hz / ((brr & 0xfff0u) | ((brr & 7u) << 1));
    }
    else
    {
      baud = hz / brr;
    }
  }
  if(baud == 0)
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
  return half_bits * ps_per_second / (2 * baud);
}

auto usart::schedule(std::uint32_t kind, std::uint64_t from_ps, std::uint64_t ps) -> std::uint64_t
{
  auto const due = std::max(from_ps + ps, ctx_.clock.time().ps);
  ctx_.events.schedule_at(vtime{due}, kind, state_.generation[kind], ctx_.clock);
  return due;
}

void usart::warn_parity()
{
  if(get(regs_, parity_enable_) == 0)
  {
    return;
  }
  if(not std::exchange(parity_logged_, true))
  {
    warn(cr1_, "parity is not modelled: PCE is set, no parity bit is sent or checked");
  }
  if(ctx_.strict_peripherals)
  {
    throw model_error(desc_.name, std::string{cr1_}, parity_enable_.offset);
  }
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
          "half-duplex, driver enable, auto baud or receiver timeout, not modelled");
    }
  }
}

void usart::start_frame(std::uint64_t from_ps)
{
  auto const queued = state_.break_pending or not state_.tx_fifo.empty();
  if(state_.shifting or not queued or not enabled() or get(regs_, transmit_) == 0)
  {
    return;
  }
  auto const ps = frame_ps();
  if(ps == 0)
  {
    return;
  }
  auto const brk = state_.break_pending;
  if(brk)
  {
    state_.shift          = 0;
    state_.break_pending  = false;
    state_.break_shifting = true;
  }
  else
  {
    state_.shift = state_.tx_fifo.front();
    state_.tx_fifo.erase(state_.tx_fifo.begin());
  }
  state_.shifting  = true;
  state_.tx_end_ps = schedule(transmit_done, from_ps, ps);
  if(far_end_)
  {
    far_end_->started(
        {.data       = state_.shift,
         .word_bits  = static_cast<std::uint8_t>(word_bits()),
         .half_stops = half_stop_codes[get(regs_, stop_)],
         .kind       = brk ? frame_kind::brk : frame_kind::data,
         .frame_ps   = ps,
         .start      = vtime{state_.tx_end_ps - ps},
         .end        = vtime{state_.tx_end_ps}});
  }
}

auto usart::word_bits() const -> std::uint32_t
{
  return get(regs_, word_high_) != 0 ? 7u : (get(regs_, word_low_) != 0 ? 9u : 8u);
}

auto usart::word_mask() const -> std::uint32_t
{
  return (std::uint32_t{1} << word_bits()) - 1u;
}

void usart::flags()
{
  auto const                           depth = fifo() ? fifo_depth : 1;
  constexpr std::array<std::size_t, 6> eighths{1, 2, 4, 6, 7, 0};
  auto const                           tx_code = get(regs_, tx_threshold_);
  auto const                           rx_code = get(regs_, rx_threshold_);
  auto const                           tx_at   = tx_code < eighths.size() ? eighths[tx_code] : 0;
  auto const rx_at = rx_code < eighths.size() ? (rx_code == 5 ? 8 : eighths[rx_code]) : 8;
  update(regs_, tx_threshold_flag_, fifo() and state_.tx_fifo.size() <= tx_at ? 1 : 0);
  update(regs_, rx_threshold_flag_, fifo() and state_.rx_fifo.size() >= rx_at ? 1 : 0);
  update(regs_, tx_not_full_, state_.tx_fifo.size() < depth ? 1 : 0);
  update(regs_, tx_empty_, state_.tx_fifo.empty() ? 1 : 0);
  update(regs_, rx_not_empty_, state_.rx_fifo.empty() ? 0 : 1);
  update(regs_, rx_full_, state_.rx_fifo.size() >= depth ? 1 : 0);
  update(regs_, busy_, state_.receiving ? 1 : 0);
  update(regs_, tx_ack_, enabled() and get(regs_, transmit_) != 0 ? 1 : 0);
  update(regs_, rx_ack_, enabled() and get(regs_, receive_) != 0 ? 1 : 0);
  regs_.poke(rdr_.offset, state_.rx_fifo.empty() ? 0 : state_.rx_fifo.front());
  interrupts();
  auto const up        = enabled();
  auto const tx_wanted = get(regs_, dma_transmit_) != 0 and get(regs_, transmit_) != 0;
  auto const rx_wanted = get(regs_, dma_receive_) != 0 and get(regs_, receive_) != 0;
  tx_dma_.set(up and tx_wanted and get(regs_, tx_not_full_) != 0);
  rx_dma_.set(up and rx_wanted and get(regs_, rx_not_empty_) != 0);
}

void usart::interrupts()
{
  auto const on = [&](field_ref flag, field_ref enable) {
    return get(regs_, flag) != 0 and get(regs_, enable) != 0;
  };
  auto const errors =
      get(regs_, overrun_) != 0 or get(regs_, noise_) != 0 or get(regs_, framing_) != 0;
  auto const level = on(tx_not_full_, tx_not_full_ie_)
                  or on(tx_empty_, tx_empty_ie_)
                  or on(complete_, complete_ie_)
                  or on(rx_not_empty_, rx_not_empty_ie_)
                  or on(rx_full_, rx_full_ie_)
                  or on(idle_, idle_ie_)
                  or on(parity_, parity_ie_)
                  or on(overrun_, rx_not_empty_ie_)
                  or on(tx_threshold_flag_, tx_threshold_ie_)
                  or on(rx_threshold_flag_, rx_threshold_ie_)
                  or (errors and get(regs_, error_ie_) != 0);
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
  warn_unsupported();
  warn_parity();
  if(not enabled())
  {
    if(state_.shifting and far_end_)
    {
      far_end_->aborted(ctx_.clock.time());
    }
    for(auto& generation : state_.generation)
    {
      ++generation;
    }
    state_.tx_fifo.clear();
    state_.rx_fifo.clear();
    state_.shifting       = false;
    state_.receiving      = false;
    state_.idle_pending   = false;
    state_.break_pending  = false;
    state_.break_shifting = false;
    update(regs_, break_flag_, 0);
    update(regs_, complete_, 1);
  }
  else if(get(regs_, receive_) != 0 and not state_.receiving and not state_.host.empty())
  {
    if(auto const ps = frame_ps(); ps != 0)
    {
      state_.receiving = true;
      state_.rx_due_ps = schedule(receive_due, ctx_.clock.time().ps, ps);
    }
  }
  start_frame(ctx_.clock.time().ps);
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
    update(regs_, complete_, 0);
    start_frame(ctx_.clock.time().ps);
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
  regs_.poke(icr_.offset, 0);
  flags();
}

void usart::request_written(std::uint32_t, std::uint32_t now)
{
  if(break_request_.of(now) != 0 and enabled() and get(regs_, transmit_) != 0)
  {
    update(regs_, break_flag_, 1);
    state_.break_pending = true;
    start_frame(ctx_.clock.time().ps);
  }
  if(flush_rx_.of(now) != 0)
  {
    state_.rx_fifo.clear();
  }
  if(flush_tx_.of(now) != 0)
  {
    state_.tx_fifo.clear();
  }
  flags();
}

auto usart::on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
    -> std::expected<std::uint32_t, bus_error>
{
  if((offset & ~3u) != rdr_.offset)
  {
    return dispatch_model::on_read(regs, offset, size, ctx);
  }
  if(ctx.by == initiator::debugger or state_.rx_fifo.empty())
  {
    return dispatch_model::on_read(regs, offset, size, ctx);
  }
  auto const value = state_.rx_fifo.front();
  state_.rx_fifo.erase(state_.rx_fifo.begin());
  flags();
  return read_lanes(value, offset, size);
}

void usart::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind == linked_due)
  {
    if(payload != state_.linked_generation or state_.linked.empty())
    {
      return;
    }
    auto const head = state_.linked.front();
    auto const rx   = shape();
    if(auto const done = rx.frame_ps != 0 ? last_sample(head.frame, rx) : 0;
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
    if(head.frame.kind == frame_kind::brk)
    {
      deliver(0, true);
    }
    else if(same or rx.frame_ps == 0)
    {
      deliver(head.frame.data);
    }
    else
    {
      auto const next = std::ranges::find(state_.linked, head.lane, &linked_frame::lane);
      auto const seen = sample(
          head.frame,
          next != state_.linked.end() ? std::optional{next->frame.start.ps} : std::nullopt, rx);
      deliver(static_cast<std::uint16_t>(seen.data & word_mask()), seen.framing, seen.noise);
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
  if(kind > idle_due or payload != state_.generation[kind])
  {
    return;
  }
  if(kind == transmit_done)
  {
    state_.shifting = false;
    auto const data = state_.shift;
    auto const brk  = std::exchange(state_.break_shifting, false);
    if(far_end_)
    {
      far_end_->transmitted(data, vtime{state_.tx_end_ps}, brk);
    }
    auto next = state_.tx_end_ps;
    if(brk)
    {
      update(regs_, break_flag_, 0);
      next += 4 * frame_ps() / half_bits(word_bits(), half_stop_codes[get(regs_, stop_)]);
    }
    start_frame(next);
    if(not state_.shifting)
    {
      update(regs_, complete_, 1);
    }
    flags();
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
  if(state_.host.empty())
  {
    flags();
    return;
  }
  auto const data = state_.host.front();
  state_.host.erase(state_.host.begin());
  deliver(data);
  auto const ps = frame_ps();
  if(ps != 0 and not state_.host.empty())
  {
    state_.receiving = true;
    state_.rx_due_ps = schedule(receive_due, state_.rx_due_ps, ps);
  }
  else if(ps != 0 and not state_.idle_pending and not linked_before(state_.rx_due_ps + ps))
  {
    state_.idle_pending = true;
    ++state_.generation[idle_due];
    state_.idle_due_ps = schedule(idle_due, state_.rx_due_ps, ps);
  }
  flags();
}

auto usart::linked_before(std::uint64_t due_ps) const -> bool
{
  return std::ranges::any_of(state_.linked, [due_ps](linked_frame const& queued) {
    return queued.frame.start.ps < due_ps;
  });
}

void usart::deliver(std::uint16_t data, bool framing, bool noise)
{
  auto const depth = fifo() ? fifo_depth : 1;
  if(enabled() and get(regs_, receive_) != 0)
  {
    if(framing)
    {
      update(regs_, framing_, 1);
    }
    if(noise)
    {
      update(regs_, noise_, 1);
    }
    if(state_.rx_fifo.size() < depth)
    {
      state_.rx_fifo.push_back(data);
    }
    else if(get(regs_, overrun_disable_) == 0)
    {
      update(regs_, overrun_, 1);
    }
  }
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

void usart::idle_at(std::uint64_t due_ps, std::uint32_t lane)
{
  state_.idle_pending = true;
  ++state_.generation[idle_due];
  if(lane == 0)
  {
    state_.idle_due_ps = schedule(idle_due, due_ps, 0);
    return;
  }
  state_.idle_due_ps = due_ps;
  ctx_.events.schedule_at(vtime{due_ps}, idle_due, state_.generation[idle_due], lane, ctx_.clock);
}

void usart::receive_at(line_frame const& frame, std::uint32_t lane)
{
  auto const key = std::pair{frame.end.ps, lane};
  auto const where =
      std::ranges::upper_bound(state_.linked, key, {}, [](linked_frame const& queued) {
        return std::pair{queued.frame.end.ps, queued.lane};
      });
  auto const head = where == state_.linked.begin();
  state_.linked.insert(where, {.frame = frame, .lane = lane});
  if(state_.idle_pending and frame.start.ps < state_.idle_due_ps)
  {
    state_.idle_pending = false;
    ++state_.generation[idle_due];
  }
  if(head)
  {
    schedule_linked();
  }
}

void usart::cancel_in_flight(vtime at, std::uint32_t lane)
{
  auto const found = std::ranges::find_if(state_.linked, [&](linked_frame const& queued) {
    return queued.lane == lane and queued.frame.start.ps < at.ps and at.ps < queued.frame.end.ps;
  });
  if(found == state_.linked.end())
  {
    return;
  }
  auto const head = found == state_.linked.begin();
  state_.linked.erase(found);
  if(head)
  {
    schedule_linked();
  }
  auto const ps = frame_ps();
  if(ps != 0 and not linked_before(at.ps + ps))
  {
    idle_at(at.ps + ps, lane);
  }
}

void usart::receive(std::span<std::uint16_t const> data)
{
  state_.host.insert(state_.host.end(), data.begin(), data.end());
  if(state_.receiving or state_.host.empty())
  {
    return;
  }
  if(auto const ps = frame_ps(); ps != 0)
  {
    state_.receiving = true;
    ++state_.generation[idle_due];
    state_.idle_pending = false;
    state_.rx_due_ps    = schedule(receive_due, ctx_.clock.time().ps, ps);
    flags();
  }
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

auto usart_factory(board const& b) -> model_factory
{
  return
      [uarts = b.uarts](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
        auto const found = uarts.find(desc.name);
        return std::make_shared<usart>(ctx, desc, found != uarts.end() ? found->second : nullptr);
      };
}
}
