#include "dfilter_chain.hpp"
#include "dma_signal.hpp"
#include "factories.hpp"
#include "model_base.hpp"
#include "sigma_delta.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/periph/stm32/pdm.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
namespace
{
constexpr std::size_t   max_filters   = 6;
constexpr std::uint32_t ckab_event    = 1;
constexpr std::uint32_t sample_event  = 2;
constexpr std::uint64_t ps_per_second = 1'000'000'000'000;
constexpr std::size_t   fifo_depth    = 4;
constexpr std::uint64_t chunk_edges   = 1u << 16;

/**
 * @brief What the description gives one MDF or ADF instance, RM0456 39.3 and 40.3: the
 *        filter count, the motor-control blocks of the MDF and the SAD of the ADF.
 */
struct dfilter_layout
{
  std::size_t filters{};
  bool        motor{};
  bool        sad{};
};

struct cck_state
{
  bool          running{};
  std::uint64_t anchor_ps{};
  std::uint64_t base{};
  std::uint64_t kernel_hz{};
  std::uint64_t divider{};

  friend auto operator==(cck_state const&, cck_state const&) -> bool = default;
};

struct sitf_state
{
  bool                           clocking{};
  std::uint64_t                  next{};
  std::array<modulator_state, 2> silence{};

  friend auto operator==(sitf_state const&, sitf_state const&) -> bool = default;
};

struct filter_state
{
  std::uint32_t             ckab_generation{};
  bool                      running{};
  bool                      single{};
  bool                      dma{};
  chain_config              config{};
  chain_state               chain{};
  std::vector<std::int32_t> fifo;

  friend auto operator==(filter_state const&, filter_state const&) -> bool = default;
};

struct mdf_state
{
  std::vector<filter_state>    filter;
  std::vector<sitf_state>      sitf;
  std::array<cck_state, 2>     cck{};
  std::uint32_t                sample_generation{};
  std::optional<std::uint64_t> scheduled_ps;

  friend auto operator==(mdf_state const&, mdf_state const&) -> bool = default;
};

enum class bank : std::uint8_t
{
  gcr,
  ckgcr,
  sitf,
  bsmx,
  dflt,
  cicr,
  rsfr,
  intr,
  old,
  old_threshold,
  dly,
  scd,
  ier,
  oec,
  sadcr,
  plain,
};

struct sitf_fields
{
  reg_ref   cr;
  field_ref enable;
  field_ref source;
  field_ref mode;
  field_ref threshold;
  field_ref active;
};

struct filter_fields
{
  reg_ref                                      bsmx;
  field_ref                                    select;
  field_ref                                    bsmx_active;
  reg_ref                                      cr;
  field_ref                                    enable;
  field_ref                                    run;
  field_ref                                    active;
  field_ref                                    acquisition;
  field_ref                                    trigger;
  reg_ref                                      cicr;
  field_ref                                    source;
  field_ref                                    cic_mode;
  field_ref                                    decimation;
  field_ref                                    scale;
  reg_ref                                      rsfr;
  field_ref                                    rsflt_bypass;
  field_ref                                    rsflt_decimation;
  field_ref                                    hpf_bypass;
  field_ref                                    hpf_cutoff;
  std::optional<field_ref>                     offset;
  field_ref                                    threshold;
  field_ref                                    dma;
  field_ref                                    rsflt_overrun;
  reg_ref                                      dr;
  field_ref                                    rxne;
  field_ref                                    fifo_threshold;
  field_ref                                    overrun;
  field_ref                                    saturation;
  reg_ref                                      dly;
  field_ref                                    skip_busy;
  std::optional<field_ref>                     old_enable;
  std::optional<field_ref>                     old_active;
  std::optional<field_ref>                     scd_enable;
  std::optional<field_ref>                     scd_active;
  reg_ref                                      ier;
  reg_ref                                      isr;
  field_ref                                    ckab;
  std::uint32_t                                isr_clear{};
  std::uint32_t                                isr_old_clear{};
  std::vector<std::pair<field_ref, field_ref>> interrupts;
};

auto lower(std::string_view name) -> std::string
{
  std::string out{name};
  std::ranges::transform(
      out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

auto layout_of(peripheral_desc const& desc) -> dfilter_layout
{
  dfilter_layout out{
      .filters = 0,
      .motor   = find_register(desc, "OEC0CR") != nullptr,
      .sad     = find_register(desc, "SADCR") != nullptr};
  while(find_register(desc, std::format("DFLT{}CR", out.filters)) != nullptr)
  {
    ++out.filters;
  }
  return out;
}

class mdf;

using handler = void (mdf::*)(std::uint32_t old, std::uint32_t now);

/**
 * @brief MDF1 and ADF1 of the STM32U5, RM0456 chapter 39 "Multi-function digital filter (MDF)"
 *        and chapter 40 "Audio digital filter (ADF)", one model for both through a layout read
 *        from the description: the filter count from the `DFLT<n>CR` registers, the motor
 *        control blocks of RM0456 Table 367 with `OEC0CR`, the sound activity detector with
 *        `SADCR`. One IRQ line per filter, `<instance>_FLT<n>_IRQn`, with the motor blocks, one
 *        `<instance>_IRQn` otherwise.
 *
 *        Registers, RM0456 39.8 and 40.8. Every enable has its ACTIVE flag follow it at once,
 *        a popcorn choice: the resynchronization latency of 39.4.15 is not modelled, polling
 *        firmware sees the same end states. GCR.TRGO reads 0 after any write. BSMXACTIVE is the
 *        OR of DFLTACTIVE, OLDACTIVE and SCDACTIVE of its index, 39.8.4. SKPBF and SNPSxDR read
 *        0. DFLTEN is write-only, 39.8.5 and
 *        40.8.5, so it reads 0 on both, although the ADF1 description marks it read_write: a
 *        read-modify-write of a running DFLTxCR that does not set DFLTEN stops the filter. The
 *        ADF1 description lacks DFLT0CR.TRGSENS, 40.8.5 bit 8, so the bit reads 0 and ignores
 *        writes, which no modelled setting reads: TRGO forces its sensitivity and every other
 *        trigger raises. DFLTxISR flags are write-1-to-clear, OLDF also clears THHF and
 *        THLF, 39.8.16. The IRQ of a filter is the OR of its flags with their enables, 39.6.
 *
 *        Data path, 39.4.3 to 39.4.13. CCKy runs while CKGDEN, CCKyEN and CCKyDIR are set, one
 *        period every (PROCDIV + 1) x (CCKDIV + 1) kernel cycles. Each edge time is computed from
 *        its index since the clock started, never accumulated, and a kernel clock change keeps
 *        the edges up to the change and restarts the count at the next edge at the new rate.
 *        CCK0 and CCK1 share the divider, each starts at the write that enables it. An enabled
 *        SITF takes a bit from its rising-edge sensor at each rising edge of its CCKy, bs_r,
 *        and from its falling-edge sensor at each falling edge, bs_f, open question 6. An edge
 *        without a sensor carries a modulator held at 0, silence. A SITF, a clock or a filter
 *        enabled by a write starts at the first edge after the write, a popcorn choice. Bits
 *        are pulled lazily, at a sample time, at a register access that needs them and at
 *        settle(), and each edge is pulled once, whatever filters use its stream. The filters
 *        run the chain of `dfilter_chain.hpp` from a reset state at their start, 39.4.11, and
 *        write each sample to their 4-deep RXFIFO at the edge of the bit that completed it,
 *        39.4.13, an event of that exact picosecond. ACQMOD 000 runs continuously, 001 writes one
 *        sample, sets DFLTRUN 0 and restarts at the next write of DFLTEN 1, 39.4.10. ACQMOD 01x
 *        waits for its trigger. DFLTEN 0 stops at once, resets the chain and flushes the RXFIFO.
 *        A full RXFIFO drops the sample and sets DOVRF. Silicon has DFLTxDR as a fifth stage,
 *        39.4.13 "Data resynchronization", so the model overruns one sample earlier. A read of
 *        DFLTxDR of any size pops one sample, PCM[23:0] in bits 31:8, an empty RXFIFO reads 0.
 *        RXNEF follows the level, FTHF the level against FTH, 1 or 2 samples. SATF follows the
 *        chain's saturation. Each filter's tap sees every sample written, dropped ones marked.
 *        With DMAEN the filter drives `<instance>_flt<n>_dma`: with FTH 0 while the RXFIFO is not
 *        empty, with FTH 1 from the second sample until the RXFIFO empties, 39.4.13. Independent
 *        transfers only. RFOVRF is set at each RSFLT input sample while mdf_proc_ck is below 24
 *        times its rate, (MCICD + 1) x (CCKDIV + 1) below 24, 39.4.8. Silicon then gives invalid
 *        samples, the model still delivers exact ones, a popcorn choice.
 *
 *        Write protection, 39.4.15 Table 381: a write changing a protected field while its
 *        ACTIVE flag is set keeps the old field value, logs once per register and raises
 *        model_error under `strict_peripherals`. The clock rules of Table 374 are checked at
 *        SITFEN and DFLTEN 0 to 1 with ">" read as ">=": normal SPI needs mdf_proc_ck at least 4
 *        x CCK, LF_MASTER 2 x CCK, with RSFLT 24 x CCK / (MCICD + 1), hclk at least mdf_proc_ck,
 *        CCK at most 25 MHz, 5 MHz in LF_MASTER. STH below 4 breaks 39.8.3. Each rule logs once
 *        per instance and raises model_error under `strict_peripherals`. Checks with no kernel
 *        clock are skipped.
 *
 *        Unmodelled settings raise unmodelled_config naming the register and the field when
 *        they take effect, never at a write that leaves them insignificant: CKGMOD 1 at CKGDEN 0
 *        to 1, an input-direction CCKy used by an enabled SITF with CKGDEN 1, Manchester modes
 *        and MDF_CKIx clocks at SITFEN 0 to 1, the ADCITF sources, the window and snapshot modes,
 *        triggers other than TRGO, a reserved SCALE code, an ADF CICMOD other than Sinc4 and
 *        Sinc5 and ILVNB above the last filter at DFLTEN 0 to 1, a reserved SCALE written while
 *        the filter runs, OLDEN and SCDEN 0 to 1. Every throw from a write restores the written
 *        register first. ACQMOD 110 and 111 run as 000, MDF CICMOD 110 and 111 as Sinc5.
 *
 *        Clock absence, 39.4.3: CKABF is set at SITFEN 0 to 1 in normal SPI mode when the
 *        selected CCKy does not toggle, CKGDEN 0, or CCKyEN 0 with CCKyDIR 1, and again at once
 *        when written 1 while the clock is still absent. A CCKy that stops while the SITF runs
 *        sets CKABF STH mdf_proc_ck cycles later, never without a kernel clock. The SITF index
 *        names the DFLTxISR it flags. Never in LF_MASTER mode. Popcorn choices: with a generated
 *        CCK, CKABF stays 0 at SITFEN, where silicon holds it until the first clock transition,
 *        the zero latency of open question 11. CKGDEN 0 also stops mdf_proc_ck, RM0456 39.8.2,
 *        and the RM does not say what CKABF does then: the model sets it at SITFEN with CKGDEN
 *        0, and STH cycles at the PROCDIV rate after CKGDEN goes to 0.
 */
class mdf final : public dispatch_model<mdf, mdf_state>, public pdm_port, public clock_listener
{
public:
  mdf(model_context& ctx, peripheral_desc const& desc, dfilter_layout layout);

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

  void on_clock_change() override;
  void attach(std::size_t sitf, pdm_edge edge, std::shared_ptr<pdm_source> source) override;
  void attach_tap(std::size_t filter, std::shared_ptr<pdm_tap> tap) override;
  [[nodiscard]] auto sitfs() const -> std::size_t override;
  void               settle(vtime at) override;

  template <bank B, std::size_t F> void written(std::uint32_t old, std::uint32_t now)
  {
    write(B, F, old, now);
  }

  template <bank B, std::size_t... F>
  static constexpr auto handlers(std::index_sequence<F...>) -> std::array<handler, max_filters>
  {
    return {&mdf::written<B, F>...};
  }

  static auto writes(peripheral_desc const& desc, dfilter_layout const& layout)
      -> std::vector<write_entry<mdf>>;

private:
  void write(bank which, std::size_t f, std::uint32_t old, std::uint32_t now);
  void global_written(std::uint32_t old, std::uint32_t now);
  void clock_written(std::uint32_t old, std::uint32_t now);
  void sitf_written(std::size_t s, std::uint32_t old, std::uint32_t now);
  void bsmx_written(std::size_t f, std::uint32_t old, std::uint32_t now);
  void filter_written(std::size_t f, std::uint32_t old, std::uint32_t now);
  void cic_written(std::size_t f, std::uint32_t old, std::uint32_t now);
  void filter_protected(std::size_t f, reg_ref at, std::uint32_t old, std::uint32_t now);
  void old_written(std::size_t f, std::uint32_t old, std::uint32_t now);
  void old_threshold_written(std::size_t f, reg_ref at, std::uint32_t old, std::uint32_t now);
  void dly_written(std::size_t f, std::uint32_t old, std::uint32_t now);
  void scd_written(std::size_t f, std::uint32_t old, std::uint32_t now);
  void sad_written(std::uint32_t old, std::uint32_t now);
  void isr_written(std::size_t f, std::uint32_t lanes, std::uint32_t value);

  [[nodiscard]] auto sitf_of(std::size_t f) const -> sitf_fields;
  [[nodiscard]] auto filter_of(std::size_t f) const -> filter_fields;
  [[nodiscard]] auto
      mask_of(std::string_view reg, std::initializer_list<std::string_view> names) const
      -> std::uint32_t;
  [[nodiscard]] auto bits(field_ref field) const -> std::uint32_t;
  [[nodiscard]] auto name_of(reg_ref at) const -> std::string;
  [[nodiscard]] auto
      protect(reg_ref at, std::uint32_t old, std::uint32_t now, std::uint32_t mask, bool active)
          -> std::uint32_t;
  [[noreturn]] void refuse(
      reg_ref          restore,
      std::uint32_t    old,
      field_ref        field,
      std::string_view field_name,
      std::string_view what);
  void rule(
      std::string_view key,
      reg_ref          restore,
      std::uint32_t    old,
      std::string_view reg,
      std::string      message);
  void               check_sitf_start(std::size_t s, std::uint32_t old);
  void               check_filter_start(std::size_t f, std::uint32_t old);
  void               check_clocks(std::size_t s, reg_ref restore, std::uint32_t old);
  void               check_direction(std::size_t s, reg_ref restore, std::uint32_t old);
  [[nodiscard]] auto sitf_selected(std::size_t f) const -> std::size_t;
  [[nodiscard]] auto proc_hz() const -> std::uint64_t;
  [[nodiscard]] auto watched(std::size_t s) const -> bool;
  [[nodiscard]] auto clock_present(std::size_t s, std::uint32_t ckgcr) const -> bool;
  void               flag_absence(std::size_t s);
  void               update_bsmx(std::size_t f);
  void               update_irq();
  [[nodiscard]] auto now_ps() const -> std::uint64_t;
  [[nodiscard]] auto edge_ps(std::size_t y, std::uint64_t h) const -> std::uint64_t;
  [[nodiscard]] auto edges_until(std::size_t y, std::uint64_t t) const -> std::uint64_t;
  [[nodiscard]] auto cck_of(std::size_t s) const -> std::size_t;
  [[nodiscard]] auto parity_of(std::size_t f) const -> std::uint64_t;
  [[nodiscard]] auto config_of(std::size_t f) const -> chain_config;
  [[nodiscard]] auto bits_needed(std::size_t f) const -> std::uint64_t;
  [[nodiscard]] auto rate_of(std::size_t y) const -> bit_rate;
  void               refresh_clocks(std::uint64_t t);
  void               catch_up(std::size_t s, std::uint64_t t);
  void               write_sample(std::size_t f, produced sample, std::uint64_t at);
  void               start_filter(std::size_t f);
  void               stop_filter(std::size_t f);
  void               refresh_fifo(std::size_t f);
  void               refresh_dma(std::size_t f);
  [[nodiscard]] auto rsflt_overruns(std::size_t f) const -> bool;
  void               reschedule();
  [[nodiscard]] auto pop(std::size_t f) -> std::uint32_t;

  register_file&                                          regs_;
  dfilter_layout                                          layout_;
  reg_ref                                                 gcr_;
  field_ref                                               trgo_;
  std::optional<field_ref>                                interleave_;
  reg_ref                                                 ckgcr_;
  field_ref                                               ckg_enable_;
  field_ref                                               ckg_mode_;
  field_ref                                               ckg_active_;
  field_ref                                               proc_div_;
  field_ref                                               cck_div_;
  std::array<field_ref, 2>                                cck_enable_;
  std::array<field_ref, 2>                                cck_direction_;
  std::uint32_t                                           ckgcr_protected_{};
  std::vector<sitf_fields>                                sitf_;
  std::vector<filter_fields>                              filter_;
  std::optional<reg_ref>                                  sadcr_;
  core::clock_domain                                      kernel_;
  core::clock_domain                                      hclk_;
  std::vector<irq_line*>                                  lines_;
  std::set<std::string>                                   warned_;
  std::vector<std::array<std::shared_ptr<pdm_source>, 2>> sources_;
  std::vector<std::shared_ptr<pdm_tap>>                   taps_;
  std::array<std::vector<std::uint8_t>, 2>                bits_;
  std::vector<dma_signal>                                 requests_;
};

auto mdf::writes(peripheral_desc const& desc, dfilter_layout const& layout)
    -> std::vector<write_entry<mdf>>
{
  std::vector<write_entry<mdf>> out;
  auto const                    add = [&](std::string const& name, handler on_write) {
    auto const* found = find_register(desc, name);
    if(found != nullptr)
    {
      out.push_back({.reg = found->name, .on_write = on_write});
    }
  };
  auto const row = []<bank B>() { return handlers<B>(std::make_index_sequence<max_filters>{}); };
  add("GCR", &mdf::written<bank::gcr, 0>);
  add("CKGCR", &mdf::written<bank::ckgcr, 0>);
  for(std::size_t f = 0; f < layout.filters; ++f)
  {
    add(std::format("SITF{}CR", f), row.operator()<bank::sitf>()[f]);
    add(std::format("BSMX{}CR", f), row.operator()<bank::bsmx>()[f]);
    add(std::format("DFLT{}CR", f), row.operator()<bank::dflt>()[f]);
    add(std::format("DFLT{}CICR", f), row.operator()<bank::cicr>()[f]);
    add(std::format("DFLT{}RSFR", f), row.operator()<bank::rsfr>()[f]);
    add(std::format("DFLT{}INTR", f), row.operator()<bank::intr>()[f]);
    add(std::format("OLD{}CR", f), row.operator()<bank::old>()[f]);
    add(std::format("OLD{}THLR", f), row.operator()<bank::old_threshold>()[f]);
    add(std::format("OLD{}THHR", f), row.operator()<bank::old_threshold>()[f]);
    add(std::format("DLY{}CR", f), row.operator()<bank::dly>()[f]);
    add(std::format("SCD{}CR", f), row.operator()<bank::scd>()[f]);
    add(std::format("DFLT{}IER", f), row.operator()<bank::ier>()[f]);
    add(std::format("DFLT{}ISR", f), nullptr);
    add(std::format("OEC{}CR", f), row.operator()<bank::oec>()[f]);
    add(std::format("SNPS{}DR", f), nullptr);
    add(std::format("DFLT{}DR", f), nullptr);
  }
  if(layout.sad)
  {
    add("SADCR", &mdf::written<bank::sadcr, 0>);
    add("SADCFGR", &mdf::written<bank::plain, 0>);
    add("SADSDLVR", nullptr);
    add("SADANLVR", nullptr);
  }
  return out;
}

mdf::mdf(model_context& ctx, peripheral_desc const& desc, dfilter_layout layout)
    : dispatch_model{ctx, desc, "MDF", writes(desc, layout)}, regs_{ctx.regs}, layout_{layout},
      gcr_{reg("GCR")}, trgo_{field("GCR", "TRGO")}, ckgcr_{reg("CKGCR")},
      ckg_enable_{field("CKGCR", "CKGDEN")}, ckg_mode_{field("CKGCR", "CKGMOD")},
      ckg_active_{field("CKGCR", "CKGACTIVE")}, proc_div_{field("CKGCR", "PROCDIV")},
      cck_div_{field("CKGCR", "CCKDIV")},
      cck_enable_{field("CKGCR", "CCK0EN"), field("CKGCR", "CCK1EN")},
      cck_direction_{field("CKGCR", "CCK0DIR"), field("CKGCR", "CCK1DIR")},
      ckgcr_protected_{mask_of(
          "CKGCR",
          {"PROCDIV", "CCKDIV", "CKGMOD", "TRGSRC", "TRGSENS", "CCK0DIR", "CCK1DIR"})},
      kernel_{ctx.clock.domain(lower(desc.name))}, hclk_{ctx.clock.domain("hclk")}
{
  if(layout_.filters == 0 or layout_.filters > max_filters)
  {
    missing(std::format("DFLT{}CR", layout_.filters == 0 ? 0 : max_filters));
  }
  if(std::ranges::contains(describe("GCR").fields, std::string_view{"ILVNB"}, &field_desc::name))
  {
    interleave_ = field("GCR", "ILVNB");
  }
  for(std::size_t f = 0; f < layout_.filters; ++f)
  {
    sitf_.push_back(sitf_of(f));
    filter_.push_back(filter_of(f));
  }
  if(layout_.sad)
  {
    sadcr_ = reg("SADCR");
  }
  if(layout_.motor)
  {
    for(std::size_t f = 0; f < layout_.filters; ++f)
    {
      lines_.push_back(&irq(std::format("{}_FLT{}_IRQn", desc.name, f)));
    }
  }
  else
  {
    lines_.push_back(&irq(std::format("{}_IRQn", desc.name)));
  }
  state_.filter.resize(layout_.filters);
  state_.sitf.resize(layout_.filters);
  sources_.resize(layout_.filters);
  taps_.resize(layout_.filters);
  for(std::size_t f = 0; f < layout_.filters; ++f)
  {
    requests_.emplace_back(ctx.peers, std::format("{}_flt{}_dma", lower(desc.name), f));
  }
}

auto mdf::sitf_of(std::size_t f) const -> sitf_fields
{
  auto const cr = std::format("SITF{}CR", f);
  return {
      .cr        = reg(cr),
      .enable    = field(cr, "SITFEN"),
      .source    = field(cr, "SCKSRC"),
      .mode      = field(cr, "SITFMOD"),
      .threshold = field(cr, "STH"),
      .active    = field(cr, "SITFACTIVE")};
}

auto mdf::filter_of(std::size_t f) const -> filter_fields
{
  auto const    bsmx = std::format("BSMX{}CR", f);
  auto const    cr   = std::format("DFLT{}CR", f);
  auto const    cicr = std::format("DFLT{}CICR", f);
  auto const    rsfr = std::format("DFLT{}RSFR", f);
  auto const    dly  = std::format("DLY{}CR", f);
  auto const    ier  = std::format("DFLT{}IER", f);
  auto const    isr  = std::format("DFLT{}ISR", f);
  filter_fields out{
      .bsmx             = reg(bsmx),
      .select           = field(bsmx, "BSSEL"),
      .bsmx_active      = field(bsmx, "BSMXACTIVE"),
      .cr               = reg(cr),
      .enable           = field(cr, "DFLTEN"),
      .run              = field(cr, "DFLTRUN"),
      .active           = field(cr, "DFLTACTIVE"),
      .acquisition      = field(cr, "ACQMOD"),
      .trigger          = field(cr, "TRGSRC"),
      .cicr             = reg(cicr),
      .source           = field(cicr, "DATSRC"),
      .cic_mode         = field(cicr, "CICMOD"),
      .decimation       = field(cicr, "MCICD"),
      .scale            = field(cicr, "SCALE"),
      .rsfr             = reg(rsfr),
      .rsflt_bypass     = field(rsfr, "RSFLTBYP"),
      .rsflt_decimation = field(rsfr, "RSFLTD"),
      .hpf_bypass       = field(rsfr, "HPFBYP"),
      .hpf_cutoff       = field(rsfr, "HPFC"),
      .offset           = std::nullopt,
      .threshold        = field(cr, "FTH"),
      .dma              = field(cr, "DMAEN"),
      .rsflt_overrun    = field(isr, "RFOVRF"),
      .dr               = reg(std::format("DFLT{}DR", f)),
      .rxne             = field(isr, "RXNEF"),
      .fifo_threshold   = field(isr, "FTHF"),
      .overrun          = field(isr, "DOVRF"),
      .saturation       = field(isr, "SATF"),
      .dly              = reg(dly),
      .skip_busy        = field(dly, "SKPBF"),
      .old_enable       = std::nullopt,
      .old_active       = std::nullopt,
      .scd_enable       = std::nullopt,
      .scd_active       = std::nullopt,
      .ier              = reg(ier),
      .isr              = reg(isr),
      .ckab             = field(isr, "CKABF"),
      .isr_clear        = 0,
      .isr_old_clear    = 0,
      .interrupts       = {}};
  if(layout_.motor)
  {
    out.old_enable    = field(std::format("OLD{}CR", f), "OLDEN");
    out.old_active    = field(std::format("OLD{}CR", f), "OLDACTIVE");
    out.scd_enable    = field(std::format("SCD{}CR", f), "SCDEN");
    out.scd_active    = field(std::format("SCD{}CR", f), "SCDACTIVE");
    out.isr_old_clear = mask_of(isr, {"THHF", "THLF"});
    out.offset        = field(std::format("OEC{}CR", f), "OFFSET");
  }
  for(auto const& flag : describe(isr).fields)
  {
    if(flag.acc == access::read_write)
    {
      out.isr_clear |= field(isr, flag.name).mask();
    }
  }
  for(auto const& enable : describe(ier).fields)
  {
    auto const flag =
        std::format("{}F", std::string_view{enable.name}.substr(0, enable.name.size() - 2));
    if(enable.name.ends_with("IE")
       and std::ranges::contains(describe(isr).fields, flag, &field_desc::name))
    {
      out.interrupts.emplace_back(field(isr, flag), field(ier, enable.name));
    }
  }
  return out;
}

auto mdf::mask_of(std::string_view reg, std::initializer_list<std::string_view> names) const
    -> std::uint32_t
{
  std::uint32_t out = 0;
  for(auto const name : names)
  {
    if(std::ranges::contains(describe(reg).fields, name, &field_desc::name))
    {
      out |= field(reg, name).mask();
    }
  }
  return out;
}

auto mdf::bits(field_ref field) const -> std::uint32_t
{
  return get(regs_, field);
}

auto mdf::name_of(reg_ref at) const -> std::string
{
  for(auto const& described : desc_.registers)
  {
    if((described.offset & ~3u) == at.offset)
    {
      return described.name;
    }
  }
  return {};
}

void mdf::reset(register_file& regs, reset_kind)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
  for(auto& filter : state_.filter)
  {
    auto const generation  = filter.ckab_generation + 1;
    filter                 = {};
    filter.ckab_generation = generation;
  }
  for(auto& sitf : state_.sitf)
  {
    sitf = {};
  }
  state_.cck = {};
  ++state_.sample_generation;
  state_.scheduled_ps.reset();
  for(auto& request : requests_)
  {
    request.resolve();
    request.set(false);
  }
  update_irq();
}

void mdf::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind == sample_event)
  {
    if(payload == state_.sample_generation)
    {
      state_.scheduled_ps.reset();
      settle(vtime{now_ps()});
      reschedule();
    }
    return;
  }
  auto const s = static_cast<std::size_t>(payload & 0xFF);
  if(kind != ckab_event
     or s >= layout_.filters
     or state_.filter[s].ckab_generation != static_cast<std::uint32_t>(payload >> 8))
  {
    return;
  }
  if(watched(s) and not clock_present(s, regs_.peek(ckgcr_.offset)))
  {
    flag_absence(s);
  }
}

auto mdf::on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
    -> std::expected<std::uint32_t, bus_error>
{
  auto const word = offset & ~3u;
  for(std::size_t f = 0; f < layout_.filters; ++f)
  {
    auto const& filter = filter_[f];
    if(filter.dr.offset == word)
    {
      settle(vtime{now_ps()});
      return read_lanes(pop(f), offset, size);
    }
    if(filter.isr.offset == word)
    {
      settle(vtime{now_ps()});
    }
    if(filter.cr.offset == word)
    {
      auto const hidden = read_lanes(filter.enable.mask(), offset, size);
      return dispatch_model::on_read(regs, offset, size, ctx)
          .transform([hidden](std::uint32_t value) { return value & ~hidden; });
    }
  }
  return dispatch_model::on_read(regs, offset, size, ctx);
}

auto mdf::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     ctx) -> std::expected<void, bus_error>
{
  auto const word = offset & ~3u;
  for(std::size_t f = 0; f < layout_.filters; ++f)
  {
    if(filter_[f].isr.offset == word)
    {
      auto const shift = 8u * (offset % 4u);
      auto const lanes = size >= 4 ? ~std::uint32_t{} : ((std::uint32_t{1} << (8u * size)) - 1u);
      isr_written(f, lanes << shift, (value & lanes) << shift);
      return {};
    }
  }
  return dispatch_model::on_write(regs, offset, size, old, value, ctx);
}

void mdf::write(bank which, std::size_t f, std::uint32_t old, std::uint32_t now)
{
  switch(which)
  {
    case bank::gcr:
      global_written(old, now);
      return;
    case bank::ckgcr:
      clock_written(old, now);
      return;
    case bank::sitf:
      sitf_written(f, old, now);
      return;
    case bank::bsmx:
      bsmx_written(f, old, now);
      return;
    case bank::dflt:
      filter_written(f, old, now);
      return;
    case bank::cicr:
      cic_written(f, old, now);
      return;
    case bank::rsfr:
      filter_protected(f, filter_[f].rsfr, old, now);
      return;
    case bank::intr:
      filter_protected(f, reg(std::format("DFLT{}INTR", f)), old, now);
      return;
    case bank::old:
      old_written(f, old, now);
      return;
    case bank::old_threshold:
      return;
    case bank::dly:
      dly_written(f, old, now);
      return;
    case bank::scd:
      scd_written(f, old, now);
      return;
    case bank::ier:
      update_irq();
      return;
    case bank::oec:
      if(state_.filter[f].running and old != now)
      {
        settle(vtime{now_ps()});
        state_.filter[f].config.offset = config_of(f).offset;
      }
      return;
    case bank::plain:
      return;
    case bank::sadcr:
      sad_written(old, now);
      return;
  }
}

auto mdf::protect(reg_ref at, std::uint32_t old, std::uint32_t now, std::uint32_t mask, bool active)
    -> std::uint32_t
{
  if(not active or ((old ^ now) & mask) == 0)
  {
    return now;
  }
  auto const name = name_of(at);
  if(warned_.insert(std::format("protect {}", name)).second)
  {
    warn(
        name, std::format(
                  "{} {}: a write-protected field changed while its ACTIVE flag is set, the "
                  "old value is kept, RM0456 Table 381",
                  desc_.name, name));
  }
  if(ctx_.strict_peripherals)
  {
    regs_.poke(at.offset, old);
    throw model_error(desc_.name, name, at.offset);
  }
  return (now & ~mask) | (old & mask);
}

void mdf::refuse(
    reg_ref          restore,
    std::uint32_t    old,
    field_ref        field,
    std::string_view field_name,
    std::string_view what)
{
  auto const value = bits(field);
  regs_.poke(restore.offset, old);
  throw unmodelled_config(
      desc_.name, name_of(reg_ref{.offset = field.offset}), field.offset, std::string{field_name},
      value, std::string{what});
}

void mdf::rule(
    std::string_view key,
    reg_ref          restore,
    std::uint32_t    old,
    std::string_view reg,
    std::string      message)
{
  if(warned_.insert(std::string{key}).second)
  {
    warn(reg, std::move(message));
  }
  if(ctx_.strict_peripherals)
  {
    regs_.poke(restore.offset, old);
    throw model_error(desc_.name, std::string{reg}, restore.offset);
  }
}

void mdf::global_written(std::uint32_t old, std::uint32_t now)
{
  if(interleave_)
  {
    now = protect(gcr_, old, now, interleave_->mask(), bits(filter_[0].active) != 0);
  }
  regs_.poke(gcr_.offset, trgo_.with(now, 0));
}

auto mdf::proc_hz() const -> std::uint64_t
{
  return ctx_.clock.hz(kernel_) / (bits(proc_div_) + 1u);
}

auto mdf::watched(std::size_t s) const -> bool
{
  return bits(sitf_[s].enable) != 0 and bits(sitf_[s].mode) == 1;
}

auto mdf::clock_present(std::size_t s, std::uint32_t ckgcr) const -> bool
{
  auto const y = bits(sitf_[s].source) & 1u;
  return ckg_enable_.of(ckgcr) != 0
     and cck_enable_[y].of(ckgcr) != 0
     and cck_direction_[y].of(ckgcr) != 0;
}

void mdf::flag_absence(std::size_t s)
{
  update(regs_, filter_[s].ckab, 1);
  update_irq();
}

void mdf::clock_written(std::uint32_t old, std::uint32_t now)
{
  now = protect(ckgcr_, old, now, ckgcr_protected_, ckg_active_.of(old) != 0);
  regs_.poke(ckgcr_.offset, now);
  if(ckg_enable_.of(old) == 0 and ckg_enable_.of(now) != 0)
  {
    if(ckg_mode_.of(now) != 0)
    {
      refuse(ckgcr_, old, ckg_mode_, "CKGMOD", "a CCK start on trigger, only CKGMOD 0 is modelled");
    }
    for(std::size_t s = 0; s < layout_.filters; ++s)
    {
      if(bits(sitf_[s].enable) != 0)
      {
        check_direction(s, ckgcr_, old);
      }
    }
  }
  regs_.poke(ckgcr_.offset, ckg_active_.with(now, ckg_enable_.of(now)));
  settle(vtime{now_ps()});
  refresh_clocks(now_ps());
  reschedule();
  for(std::size_t s = 0; s < layout_.filters; ++s)
  {
    if(not watched(s))
    {
      continue;
    }
    auto const was = clock_present(s, old);
    auto const is  = clock_present(s, now);
    if(was and not is)
    {
      auto const hz     = proc_hz();
      auto const cycles = std::uint64_t{bits(sitf_[s].threshold)};
      auto&      gen    = state_.filter[s].ckab_generation;
      ++gen;
      if(hz != 0)
      {
        auto const ps = (cycles * ps_per_second + hz - 1) / hz;
        ctx_.events.schedule_at(
            vtime{ctx_.clock.time().ps + ps}, ckab_event, (std::uint64_t{gen} << 8) | s,
            ctx_.clock);
      }
    }
    else if(is and not was)
    {
      ++state_.filter[s].ckab_generation;
    }
  }
}

void mdf::check_direction(std::size_t s, reg_ref restore, std::uint32_t old)
{
  auto const ckgcr = regs_.peek(ckgcr_.offset);
  auto const y     = bits(sitf_[s].source) & 1u;
  if(ckg_enable_.of(ckgcr) != 0 and cck_direction_[y].of(ckgcr) == 0)
  {
    refuse(
        restore, old, cck_direction_[y], y == 0 ? "CCK0DIR" : "CCK1DIR",
        std::format("CCK{} as an input clocking SITF{}, only a generated CCK is modelled", y, s));
  }
}

void mdf::check_clocks(std::size_t s, reg_ref restore, std::uint32_t old)
{
  auto const kernel = ctx_.clock.hz(kernel_);
  if(kernel == 0)
  {
    return;
  }
  auto const proc   = proc_hz();
  auto const cck    = proc / (bits(cck_div_) + 1u);
  auto const lf     = bits(sitf_[s].mode) == 0;
  auto const factor = lf ? 2u : 4u;
  auto const cr     = name_of(sitf_[s].cr);
  if(proc < factor * cck)
  {
    rule(
        lf ? "ratio lf_master" : "ratio spi", restore, old, cr,
        std::format(
            "{} needs mdf_proc_ck >= {} x CCK, RM0456 Table 374: CKGCR PROCDIV {} and CCKDIV {} "
            "give {} Hz for a CCK of {} Hz with {} SITFMOD {}",
            lf ? "LF_MASTER SPI" : "normal SPI", factor, bits(proc_div_), bits(cck_div_), proc, cck,
            cr, bits(sitf_[s].mode)));
  }
  auto const limit = lf ? 5'000'000u : 25'000'000u;
  if(cck > limit)
  {
    rule(
        lf ? "cck lf_master" : "cck spi", restore, old, cr,
        std::format(
            "a CCK of {} Hz is above the {} Hz of {} SITFMOD {}, RM0456 Table 374", cck, limit, cr,
            bits(sitf_[s].mode)));
  }
  if(ctx_.clock.hz(hclk_) < proc)
  {
    rule(
        "hclk", restore, old, "CKGCR",
        std::format(
            "hclk {} Hz is below mdf_proc_ck {} Hz, RM0456 Table 374", ctx_.clock.hz(hclk_), proc));
  }
  for(std::size_t f = 0; f < layout_.filters; ++f)
  {
    auto const& filter = filter_[f];
    if(bits(filter.enable) == 0 or sitf_selected(f) != s or bits(filter.rsflt_bypass) != 0)
    {
      continue;
    }
    auto const ratio = std::max<std::uint32_t>(bits(filter.decimation), 1u) + 1u;
    if(proc * ratio < 24u * cck)
    {
      rule(
          "ratio rsflt", restore, old, name_of(filter.cr),
          std::format(
              "RSFLT needs mdf_proc_ck >= 24 x CCK / (MCICD + 1), RM0456 Table 374: {} Hz for "
              "a CCK of {} Hz at a CIC decimation of {}",
              proc, cck, ratio));
    }
  }
}

void mdf::check_sitf_start(std::size_t s, std::uint32_t old)
{
  auto const& sitf = sitf_[s];
  if(bits(sitf.mode) >= 2)
  {
    refuse(sitf.cr, old, sitf.mode, "SITFMOD", "Manchester mode");
  }
  if(bits(sitf.source) >= 2)
  {
    refuse(sitf.cr, old, sitf.source, "SCKSRC", "the MDF_CKIx clock input");
  }
  check_direction(s, sitf.cr, old);
  check_clocks(s, sitf.cr, old);
  if(bits(sitf.threshold) < 4)
  {
    rule(
        "sth", sitf.cr, old, name_of(sitf.cr),
        std::format("STH {} is below 4, RM0456 39.8.3", bits(sitf.threshold)));
  }
}

void mdf::sitf_written(std::size_t s, std::uint32_t old, std::uint32_t now)
{
  auto const& sitf = sitf_[s];
  now              = protect(
      sitf.cr, old, now, sitf.threshold.mask() | sitf.mode.mask() | sitf.source.mask(),
      sitf.active.of(old) != 0);
  now = sitf.active.with(now, sitf.enable.of(now));
  regs_.poke(sitf.cr.offset, now);
  auto const started = sitf.enable.of(old) == 0 and sitf.enable.of(now) != 0;
  if(started)
  {
    check_sitf_start(s, old);
  }
  if(sitf.enable.of(old) != sitf.enable.of(now))
  {
    ++state_.filter[s].ckab_generation;
    settle(vtime{now_ps()});
    refresh_clocks(now_ps());
    reschedule();
  }
  if(started and watched(s) and not clock_present(s, regs_.peek(ckgcr_.offset)))
  {
    flag_absence(s);
  }
}

auto mdf::sitf_selected(std::size_t f) const -> std::size_t
{
  auto const stream = bits(filter_[f].select);
  return std::min<std::size_t>(stream / 2u, layout_.filters - 1);
}

void mdf::update_bsmx(std::size_t f)
{
  auto const& filter = filter_[f];
  auto        active = bits(filter.active) != 0;
  if(filter.old_active)
  {
    active = active or bits(*filter.old_active) != 0 or bits(*filter.scd_active) != 0;
  }
  update(regs_, filter.bsmx_active, active ? 1 : 0);
}

void mdf::bsmx_written(std::size_t f, std::uint32_t old, std::uint32_t now)
{
  auto const& filter = filter_[f];
  now = protect(filter.bsmx, old, now, filter.select.mask(), filter.bsmx_active.of(old) != 0);
  regs_.poke(filter.bsmx.offset, filter.bsmx_active.with(now, filter.bsmx_active.of(old)));
}

void mdf::check_filter_start(std::size_t f, std::uint32_t old)
{
  auto const& filter = filter_[f];
  if(bits(filter.source) >= 2)
  {
    refuse(filter.cr, old, filter.source, "DATSRC", "the ADCITF parallel input");
  }
  auto const mode = bits(filter.acquisition);
  if(mode == 4 or mode == 5)
  {
    refuse(
        filter.cr, old, filter.acquisition, "ACQMOD",
        mode == 4 ? "the window continuous mode" : "the synchronous snapshot mode");
  }
  if((mode == 2 or mode == 3) and bits(filter.trigger) != 0)
  {
    refuse(filter.cr, old, filter.trigger, "TRGSRC", "a trigger other than TRGO");
  }
  if(not scale_of(static_cast<std::uint8_t>(bits(filter.scale))))
  {
    refuse(filter.cr, old, filter.scale, "SCALE", "a reserved SCALE code");
  }
  if(not layout_.motor)
  {
    auto const cic = bits(filter.cic_mode);
    if(cic != 4 and cic != 5)
    {
      refuse(filter.cr, old, filter.cic_mode, "CICMOD", "a reserved ADF CICMOD code");
    }
  }
  if(f == 0 and interleave_ and bits(*interleave_) >= layout_.filters)
  {
    refuse(filter.cr, old, *interleave_, "ILVNB", "an interleave past the last filter");
  }
  auto const s = sitf_selected(f);
  if(bits(sitf_[s].enable) != 0)
  {
    check_clocks(s, filter.cr, old);
  }
}

void mdf::filter_written(std::size_t f, std::uint32_t old, std::uint32_t now)
{
  auto const& filter = filter_[f];
  auto const  mask   = mask_of(
      name_of(filter.cr), {"NBDIS", "TRGSRC", "TRGSENS", "FTH", "DMAEN", "SNPSFMT", "ACQMOD"});
  now = protect(filter.cr, old, now, mask, filter.active.of(old) != 0);
  now = filter.run.with(filter.active.with(now, filter.enable.of(now)), filter.run.of(old));
  regs_.poke(filter.cr.offset, now);
  auto const restart = filter.enable.of(now) != 0
                   and filter.enable.of(old) != 0
                   and state_.filter[f].single
                   and not state_.filter[f].running;
  if(filter.enable.of(old) == 0 and filter.enable.of(now) != 0)
  {
    check_filter_start(f, old);
  }
  if(filter.enable.of(old) != filter.enable.of(now) or restart)
  {
    settle(vtime{now_ps()});
    if(filter.enable.of(now) != 0)
    {
      start_filter(f);
    }
    else
    {
      stop_filter(f);
    }
    reschedule();
  }
  update_bsmx(f);
}

void mdf::cic_written(std::size_t f, std::uint32_t old, std::uint32_t now)
{
  auto const& filter = filter_[f];
  auto const  active = bits(filter.active) != 0;
  now                = protect(
      filter.cicr, old, now,
      filter.decimation.mask() | filter.cic_mode.mask() | filter.source.mask(), active);
  regs_.poke(filter.cicr.offset, now);
  if(active
     and filter.scale.of(old) != filter.scale.of(now)
     and not scale_of(static_cast<std::uint8_t>(filter.scale.of(now))))
  {
    refuse(filter.cicr, old, filter.scale, "SCALE", "a reserved SCALE code");
  }
  if(state_.filter[f].running and filter.scale.of(old) != filter.scale.of(now))
  {
    settle(vtime{now_ps()});
    state_.filter[f].config.scale = static_cast<std::uint8_t>(filter.scale.of(now));
  }
}

void mdf::filter_protected(std::size_t f, reg_ref at, std::uint32_t old, std::uint32_t now)
{
  regs_.poke(at.offset, protect(at, old, now, ~std::uint32_t{}, bits(filter_[f].active) != 0));
}

void mdf::old_written(std::size_t f, std::uint32_t old, std::uint32_t now)
{
  auto const& filter = filter_[f];
  if(filter.old_enable->of(old) == 0 and filter.old_enable->of(now) != 0)
  {
    refuse(
        reg_ref{.offset = filter.old_enable->offset}, old, *filter.old_enable, "OLDEN",
        "the out-of-limit detector");
  }
  regs_.poke(filter.old_enable->offset, filter.old_active->with(now, filter.old_enable->of(now)));
  update_bsmx(f);
}

void mdf::scd_written(std::size_t f, std::uint32_t old, std::uint32_t now)
{
  auto const& filter = filter_[f];
  if(filter.scd_enable->of(old) == 0 and filter.scd_enable->of(now) != 0)
  {
    refuse(
        reg_ref{.offset = filter.scd_enable->offset}, old, *filter.scd_enable, "SCDEN",
        "the short-circuit detector");
  }
  regs_.poke(filter.scd_enable->offset, filter.scd_active->with(now, filter.scd_enable->of(now)));
  update_bsmx(f);
}

void mdf::dly_written(std::size_t f, std::uint32_t old, std::uint32_t now)
{
  auto const& filter = filter_[f];
  now = protect(filter.dly, old, now, ~filter.skip_busy.mask(), filter.skip_busy.of(old) != 0);
  regs_.poke(filter.dly.offset, filter.skip_busy.with(now, filter.skip_busy.of(old)));
}

void mdf::sad_written(std::uint32_t, std::uint32_t now)
{
  auto const enable = field("SADCR", "SADEN");
  regs_.poke(sadcr_->offset, field("SADCR", "SADACTIVE").with(now, enable.of(now)));
}

void mdf::isr_written(std::size_t f, std::uint32_t lanes, std::uint32_t value)
{
  auto const& filter  = filter_[f];
  auto const  cleared = value & lanes & filter.isr_clear;
  auto        word    = regs_.peek(filter.isr.offset) & ~cleared;
  if(filter.old_enable and (cleared & field(name_of(filter.isr), "OLDF").mask()) != 0)
  {
    word &= ~filter.isr_old_clear;
  }
  regs_.poke(filter.isr.offset, word);
  if((cleared & filter.ckab.mask()) != 0
     and watched(f)
     and not clock_present(f, regs_.peek(ckgcr_.offset)))
  {
    update(regs_, filter.ckab, 1);
  }
  update_irq();
}

void mdf::update_irq()
{
  std::vector<bool> level(lines_.size(), false);
  for(std::size_t f = 0; f < layout_.filters; ++f)
  {
    for(auto const& [flag, enable] : filter_[f].interrupts)
    {
      if(bits(flag) != 0 and bits(enable) != 0)
      {
        level[std::min(f, lines_.size() - 1)] = true;
      }
    }
  }
  for(std::size_t l = 0; l < lines_.size(); ++l)
  {
    if(level[l])
    {
      lines_[l]->raise();
    }
    else
    {
      lines_[l]->lower();
    }
  }
}
}

pdm_source::~pdm_source() = default;
pdm_tap::~pdm_tap()       = default;

auto mdf_factory() -> model_factory
{
  return [](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<mdf>(ctx, desc, layout_of(desc));
  };
}
}

namespace popcorn::periph::stm32
{
namespace
{
auto mdf::now_ps() const -> std::uint64_t
{
  return ctx_.clock.time().ps;
}

auto mdf::edge_ps(std::size_t y, std::uint64_t h) const -> std::uint64_t
{
  auto const& cck = state_.cck[y];
  auto const  num = static_cast<unsigned __int128>(h - cck.base) * cck.divider * ps_per_second;
  return cck.anchor_ps + static_cast<std::uint64_t>(num / (2u * cck.kernel_hz));
}

auto mdf::edges_until(std::size_t y, std::uint64_t t) const -> std::uint64_t
{
  auto const& cck = state_.cck[y];
  if(t < cck.anchor_ps)
  {
    return cck.base;
  }
  auto const num = static_cast<unsigned __int128>(t - cck.anchor_ps + 1) * 2u * cck.kernel_hz;
  auto const den = static_cast<unsigned __int128>(cck.divider) * ps_per_second;
  return cck.base + static_cast<std::uint64_t>((num + den - 1) / den);
}

auto mdf::cck_of(std::size_t s) const -> std::size_t
{
  return bits(sitf_[s].source) & 1u;
}

auto mdf::parity_of(std::size_t f) const -> std::uint64_t
{
  auto const stream = bits(filter_[f].select);
  return stream / 2u >= layout_.filters ? 1u : stream & 1u;
}

auto mdf::rate_of(std::size_t y) const -> bit_rate
{
  return {.kernel_hz = state_.cck[y].kernel_hz, .divider = state_.cck[y].divider};
}

auto mdf::config_of(std::size_t f) const -> chain_config
{
  auto const&  filter = filter_[f];
  chain_config out{
      .order          = cic_order_of(static_cast<std::uint8_t>(bits(filter.cic_mode))),
      .decimation     = static_cast<std::uint16_t>(std::max(bits(filter.decimation), 1u) + 1u),
      .offset         = 0,
      .scale          = static_cast<std::uint8_t>(bits(filter.scale)),
      .rsflt          = bits(filter.rsflt_bypass) == 0,
      .rsflt_decimate = bits(filter.rsflt_decimation) == 0,
      .hpf            = std::nullopt,
      .int_value      = std::nullopt,
      .int_div        = int_divider::by_128};
  if(bits(filter.hpf_bypass) == 0)
  {
    out.hpf = static_cast<std::uint8_t>(bits(filter.hpf_cutoff));
  }
  if(filter.offset)
  {
    out.offset = static_cast<std::int32_t>(bits(*filter.offset) << 6) >> 6;
  }
  return out;
}

auto mdf::bits_needed(std::size_t f) const -> std::uint64_t
{
  auto const& filter = state_.filter[f];
  auto const  d      = std::uint64_t{filter.config.decimation};
  auto        need   = d - filter.chain.cic.count;
  if(filter.config.rsflt and filter.config.rsflt_decimate)
  {
    need += (3u - filter.chain.rsflt.count) * d;
  }
  return need;
}

void mdf::refresh_clocks(std::uint64_t t)
{
  auto const ckgcr  = regs_.peek(ckgcr_.offset);
  auto const kernel = ctx_.clock.hz(kernel_);
  auto const div    = std::uint64_t{proc_div_.of(ckgcr) + 1u} * (cck_div_.of(ckgcr) + 1u);
  for(std::size_t y = 0; y < 2; ++y)
  {
    auto const should = ckg_enable_.of(ckgcr) != 0
                    and cck_enable_[y].of(ckgcr) != 0
                    and cck_direction_[y].of(ckgcr) != 0
                    and kernel != 0;
    auto&      cck = state_.cck[y];
    if(should and not cck.running)
    {
      cck = {.running = true, .anchor_ps = t, .base = 0, .kernel_hz = kernel, .divider = div};
      for(std::size_t s = 0; s < layout_.filters; ++s)
      {
        if(cck_of(s) == y)
        {
          state_.sitf[s].clocking = false;
        }
      }
    }
    else if(not should)
    {
      cck.running = false;
    }
  }
  for(std::size_t s = 0; s < layout_.filters; ++s)
  {
    auto&      sitf   = state_.sitf[s];
    auto const y      = cck_of(s);
    auto const should = bits(sitf_[s].enable) != 0 and state_.cck[y].running;
    if(should and not sitf.clocking)
    {
      sitf.clocking = true;
      sitf.next     = edges_until(y, t);
    }
    else if(not should)
    {
      sitf.clocking = false;
    }
  }
}

void mdf::on_clock_change()
{
  auto const t = now_ps();
  settle(vtime{t});
  auto const kernel = ctx_.clock.hz(kernel_);
  for(std::size_t y = 0; y < 2; ++y)
  {
    auto& cck = state_.cck[y];
    if(not cck.running or cck.kernel_hz == kernel)
    {
      continue;
    }
    if(kernel == 0)
    {
      cck.running = false;
      continue;
    }
    auto const next = edges_until(y, t);
    cck.anchor_ps   = edge_ps(y, next);
    cck.base        = next;
    cck.kernel_hz   = kernel;
  }
  refresh_clocks(t);
  reschedule();
}

void mdf::attach(std::size_t sitf, pdm_edge edge, std::shared_ptr<pdm_source> source)
{
  sources_.at(sitf)[edge == pdm_edge::rising ? 0 : 1] = std::move(source);
}

void mdf::attach_tap(std::size_t filter, std::shared_ptr<pdm_tap> tap)
{
  taps_.at(filter) = std::move(tap);
}

auto mdf::sitfs() const -> std::size_t
{
  return layout_.filters;
}

void mdf::settle(vtime at)
{
  for(std::size_t s = 0; s < layout_.filters; ++s)
  {
    catch_up(s, at.ps);
  }
}

void mdf::catch_up(std::size_t s, std::uint64_t t)
{
  auto& sitf = state_.sitf[s];
  if(not sitf.clocking)
  {
    return;
  }
  auto const y   = cck_of(s);
  auto const end = edges_until(y, t);
  while(sitf.next < end)
  {
    auto const                   from = sitf.next;
    auto const                   to   = std::min(end, from + 2 * chunk_edges);
    std::array<std::uint64_t, 2> first{};
    for(std::uint64_t e = 0; e < 2; ++e)
    {
      first[e]     = from + ((from ^ e) & 1u);
      auto const n = first[e] < to ? (to - first[e] + 1) / 2 : 0;
      bits_[e].resize(n);
      if(auto const& source = sources_[s][e])
      {
        source->clock(bits_[e], rate_of(y));
      }
      else
      {
        for(auto& bit : bits_[e])
        {
          bit = modulate(sitf.silence[e], 0) ? 1 : 0;
        }
      }
    }
    for(std::size_t f = 0; f < layout_.filters; ++f)
    {
      auto& filter = state_.filter[f];
      if(not filter.running or sitf_selected(f) != s)
      {
        continue;
      }
      auto const e       = parity_of(f);
      auto const overrun = rsflt_overruns(f);
      for(std::size_t j = 0; j < bits_[e].size() and filter.running; ++j)
      {
        auto const sample = feed(filter.chain, filter.config, bits_[e][j] != 0 ? 1 : -1);
        if(overrun and filter.chain.cic.count == 0)
        {
          update(regs_, filter_[f].rsflt_overrun, 1);
          update_irq();
        }
        if(sample)
        {
          write_sample(f, *sample, edge_ps(y, first[e] + 2 * j));
        }
      }
    }
    sitf.next = to;
  }
}

void mdf::write_sample(std::size_t f, produced sample, std::uint64_t at)
{
  auto&      filter  = state_.filter[f];
  auto const dropped = filter.fifo.size() >= fifo_depth;
  if(dropped)
  {
    update(regs_, filter_[f].overrun, 1);
  }
  else
  {
    filter.fifo.push_back(sample.pcm);
  }
  if(sample.saturated)
  {
    update(regs_, filter_[f].saturation, 1);
  }
  if(filter.single)
  {
    filter.running = false;
    filter.chain   = {};
    update(regs_, filter_[f].run, 0);
  }
  refresh_fifo(f);
  if(auto const& tap = taps_[f])
  {
    tap->wrote(sample.pcm, vtime{at}, dropped, true);
  }
}

void mdf::refresh_fifo(std::size_t f)
{
  auto const  level  = state_.filter[f].fifo.size();
  auto const& filter = filter_[f];
  update(regs_, filter.rxne, level != 0 ? 1 : 0);
  update(regs_, filter.fifo_threshold, level >= (bits(filter.threshold) != 0 ? 2u : 1u) ? 1 : 0);
  refresh_dma(f);
  update_irq();
}

void mdf::refresh_dma(std::size_t f)
{
  auto&      filter = state_.filter[f];
  auto const level  = filter.fifo.size();
  if(bits(filter_[f].dma) == 0 or level == 0)
  {
    filter.dma = false;
  }
  else if(bits(filter_[f].threshold) == 0 or level >= fifo_depth / 2)
  {
    filter.dma = true;
  }
  requests_[f].set(filter.dma);
}

auto mdf::rsflt_overruns(std::size_t f) const -> bool
{
  auto const& config = state_.filter[f].config;
  return config.rsflt and std::uint32_t{config.decimation} * (bits(cck_div_) + 1u) < 24u;
}

auto mdf::pop(std::size_t f) -> std::uint32_t
{
  auto& fifo = state_.filter[f].fifo;
  if(fifo.empty())
  {
    return 0;
  }
  auto const value = static_cast<std::uint32_t>(fifo.front()) << 8;
  fifo.erase(fifo.begin());
  refresh_fifo(f);
  return value;
}

void mdf::start_filter(std::size_t f)
{
  auto&      filter = state_.filter[f];
  auto const mode   = bits(filter_[f].acquisition);
  filter.chain      = {};
  filter.config     = config_of(f);
  filter.single     = mode == 1;
  filter.running    = mode != 2 and mode != 3;
  update(regs_, filter_[f].run, filter.running ? 1 : 0);
}

void mdf::stop_filter(std::size_t f)
{
  auto& filter   = state_.filter[f];
  filter.running = false;
  filter.single  = false;
  filter.chain   = {};
  filter.fifo.clear();
  update(regs_, filter_[f].run, 0);
  refresh_fifo(f);
}

void mdf::reschedule()
{
  std::optional<std::uint64_t> earliest;
  for(std::size_t f = 0; f < layout_.filters; ++f)
  {
    auto const& filter = state_.filter[f];
    auto const  s      = sitf_selected(f);
    auto const& sitf   = state_.sitf[s];
    if(not filter.running or not sitf.clocking)
    {
      continue;
    }
    auto const e     = parity_of(f);
    auto const first = sitf.next + ((sitf.next ^ e) & 1u);
    auto const need  = rsflt_overruns(f)
                         ? std::uint64_t{filter.config.decimation} - filter.chain.cic.count
                         : bits_needed(f);
    auto const at    = edge_ps(cck_of(s), first + 2 * (need - 1));
    earliest         = earliest ? std::min(*earliest, at) : at;
  }
  if(earliest == state_.scheduled_ps)
  {
    return;
  }
  ++state_.sample_generation;
  state_.scheduled_ps = earliest;
  if(earliest)
  {
    ctx_.events.schedule_at(vtime{*earliest}, sample_event, state_.sample_generation, ctx_.clock);
  }
}
}
}
