#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace popcorn::periph::stm32
{
/**
 * @brief CIC filter order, valued as the DFLTxCICR.CICMOD codes of RM0456 39.8.6.
 *        Build it with cic_order_of(), which maps the codes above 5 to Sinc5.
 */
enum class cic_order : std::uint8_t
{
  fastsinc,
  sinc1,
  sinc2,
  sinc3,
  sinc4,
  sinc5,
};

/**
 * @brief INT division ratio, valued as the DFLTxINTR.INTDIV codes of RM0456 39.8.8.
 */
enum class int_divider : std::uint8_t
{
  by_128,
  by_32,
  by_4,
  by_1,
};

/**
 * @brief Gain of one DFLTxCICR.SCALE code, `mult x 2^shift`, RM0456 Table 377.
 */
struct scale_gain
{
  std::int64_t mult;
  int          shift;

  friend auto operator==(scale_gain const&, scale_gain const&) -> bool = default;
};

/**
 * @brief CIC order of a DFLTxCICR.CICMOD code, RM0456 39.8.6: 110 and 111 are Sinc5.
 * @param cicmod CICMOD field value, 0 to 7.
 * @return the order.
 */
[[nodiscard]] constexpr auto cic_order_of(std::uint8_t cicmod) -> cic_order
{
  return static_cast<cic_order>(std::min<std::uint8_t>(cicmod, 5));
}

/**
 * @brief Gain of a SCALE code: even steps are pure shifts, odd steps x1.5 times a shift.
 * @param code SCALE field value, 0x00 to 0x18 or 0x20 to 0x2F.
 * @return the gain, empty for a reserved code.
 */
[[nodiscard]] constexpr auto scale_of(std::uint8_t code) -> std::optional<scale_gain>
{
  int step = 0;
  int base = 0;
  if(code <= 0x18)
  {
    step = code;
  }
  else if(code >= 0x20 and code <= 0x2F)
  {
    step = code - 0x20;
    base = -8;
  }
  else
  {
    return std::nullopt;
  }
  if(step % 2 == 0)
  {
    return scale_gain{1, base + step / 2};
  }
  return scale_gain{3, base + (step - 1) / 2 - 1};
}

/**
 * @brief Filter chain settings of one digital filter, as its registers give them.
 *
 * `decimation` is the CIC decimation ratio, 2 to 512. `hpf` holds HPFC when the high-pass filter is
 * on. `int_value` holds INTVAL + 1, empty for INTVAL 0, which bypasses INT and ignores `int_div`.
 */
struct chain_config
{
  cic_order                   order{cic_order::fastsinc};
  std::uint16_t               decimation{2};
  std::int32_t                offset{};
  std::uint8_t                scale{};
  bool                        rsflt{};
  bool                        rsflt_decimate{true};
  std::optional<std::uint8_t> hpf{};
  std::optional<std::uint8_t> int_value{};
  int_divider                 int_div{int_divider::by_128};

  friend auto operator==(chain_config const&, chain_config const&) -> bool = default;
};

/**
 * @brief Hogenauer CIC state. Integrators and combs wrap modulo 2^64, a superset of the
 *        26-bit register wrap RM0456 39.4.8 describes. `fast` holds the two previous Sinc2
 *        outputs FastSinc adds.
 */
struct cic_state
{
  std::array<std::uint64_t, 5> integ{};
  std::array<std::uint64_t, 5> comb{};
  std::array<std::uint64_t, 2> fast{};
  std::uint16_t                count{};

  friend auto operator==(cic_state const&, cic_state const&) -> bool = default;
};

/**
 * @brief Direct form I history of one biquad, the newest sample first.
 */
struct biquad_state
{
  std::array<std::int64_t, 2> x{};
  std::array<std::int64_t, 2> y{};

  friend auto operator==(biquad_state const&, biquad_state const&) -> bool = default;
};

/**
 * @brief RSFLT state: two biquads and the decimation phase.
 */
struct rsflt_state
{
  std::array<biquad_state, 2> stage{};
  std::uint8_t                count{};

  friend auto operator==(rsflt_state const&, rsflt_state const&) -> bool = default;
};

/**
 * @brief HPF state: previous input and previous saturated output.
 */
struct hpf_state
{
  std::int64_t x1{};
  std::int64_t y1{};

  friend auto operator==(hpf_state const&, hpf_state const&) -> bool = default;
};

/**
 * @brief INT state: running sum, samples summed and their saturation.
 */
struct int_state
{
  std::int64_t sum{};
  std::uint8_t count{};
  bool         saturated{};

  friend auto operator==(int_state const&, int_state const&) -> bool = default;
};

/**
 * @brief Whole chain state, zero at the filter enable. `saturated` holds saturations of
 *        samples the RSFLT decimated away, carried to its next output.
 */
struct chain_state
{
  cic_state   cic;
  rsflt_state rsflt;
  hpf_state   hpf;
  int_state   integ;
  bool        saturated{};

  friend auto operator==(chain_state const&, chain_state const&) -> bool = default;
};

/**
 * @brief One sample out of a chain stage, with every saturation since the previous one.
 */
struct produced
{
  std::int32_t pcm;
  bool         saturated;

  friend auto operator==(produced const&, produced const&) -> bool = default;
};

namespace dfilter_detail
{
constexpr std::int64_t sample_max = (std::int64_t{1} << 23) - 1;
constexpr std::int64_t sample_min = -(std::int64_t{1} << 23);
constexpr int          cic_bits   = 26;
constexpr int          rsflt_q    = 28;

/**
 * @brief Biquad coefficients `(b0, b1, b2, a1, a2)` in Q2.28. A popcorn-defined 4th-order
 *        Butterworth low-pass at 0.1163 x FRS, low Q section first, not RM0456 coefficients:
 *        the RM gives none.
 */
constexpr std::array<std::array<std::int64_t, 5>, 2> rsflt_coefficients{
    {
     {21197333, 42394666, 21197333, -247306859, 63660735},
     {27296179, 54592358, 27296179, -318461391, 159210650},
     }
};

constexpr std::array<int, 4> hpf_shift{8, 7, 6, 4};
constexpr std::array<int, 4> int_shift{7, 5, 2, 0};

constexpr auto saturate(std::int64_t value, bool& saturated) -> std::int64_t
{
  if(value > sample_max)
  {
    saturated = true;
    return sample_max;
  }
  if(value < sample_min)
  {
    saturated = true;
    return sample_min;
  }
  return value;
}

constexpr auto sign_extend(std::uint64_t value, int bits) -> std::int64_t
{
  auto const sign = std::uint64_t{1} << (bits - 1);
  auto const low  = value & ((sign << 1) - 1);
  return static_cast<std::int64_t>(low ^ sign) - static_cast<std::int64_t>(sign);
}

constexpr auto cic(cic_state& state, chain_config const& config, std::int32_t in)
    -> std::optional<std::int64_t>
{
  auto const fast = config.order == cic_order::fastsinc;
  auto const order =
      fast ? std::size_t{2} : std::min<std::size_t>(static_cast<std::size_t>(config.order), 5);
  auto carry = static_cast<std::uint64_t>(static_cast<std::int64_t>(in));
  for(std::size_t k = 0; k < order; ++k)
  {
    state.integ[k] += carry;
    carry = state.integ[k];
  }
  if(++state.count < config.decimation)
  {
    return std::nullopt;
  }
  state.count = 0;
  for(std::size_t k = 0; k < order; ++k)
  {
    auto const previous = state.comb[k];
    state.comb[k]       = carry;
    carry -= previous;
  }
  if(fast)
  {
    auto const sinc2 = carry;
    carry += state.fast[1];
    state.fast[1] = state.fast[0];
    state.fast[0] = sinc2;
  }
  return sign_extend(carry, cic_bits);
}

constexpr auto biquad(
    biquad_state&                      state,
    std::array<std::int64_t, 5> const& c,
    std::int64_t                       x,
    bool&                              saturated) -> std::int64_t
{
  auto const acc =
      c[0] * x + c[1] * state.x[0] + c[2] * state.x[1] - c[3] * state.y[0] - c[4] * state.y[1];
  auto const y = saturate(acc >> rsflt_q, saturated);
  state.x      = {x, state.x[0]};
  state.y      = {y, state.y[0]};
  return y;
}

constexpr auto
    rsflt(rsflt_state& state, chain_config const& config, std::int64_t x, bool& saturated)
        -> std::optional<std::int64_t>
{
  auto y = saturate(x * 3, saturated);
  for(std::size_t k = 0; k < state.stage.size(); ++k)
  {
    y = biquad(state.stage[k], rsflt_coefficients[k], y, saturated);
  }
  if(not config.rsflt_decimate)
  {
    return y;
  }
  if(++state.count < 4)
  {
    return std::nullopt;
  }
  state.count = 0;
  return y;
}

constexpr auto hpf(hpf_state& state, std::uint8_t hpfc, std::int64_t x, bool& saturated)
    -> std::int64_t
{
  auto const y = saturate(x - state.x1 + state.y1 - (state.y1 >> hpf_shift[hpfc & 3u]), saturated);
  state.x1     = x;
  state.y1     = y;
  return y;
}
}

/**
 * @brief Feeds one remapped input bit through CIC, OEC, SCALE, SAT, RSFLT and HPF, the order
 *        of RM0456 39.4.8 Figure 335. Discard, INT and skip are the caller's.
 * @param state Chain state, zero at the filter enable.
 * @param config Chain settings, SCALE a valid code. An order above Sinc5 runs as Sinc5.
 * @param in +1 for a bit 1, -1 for a bit 0.
 * @return the HPF output when the chain produced a sample, with every saturation since the
 *         previous one.
 */
[[nodiscard]] constexpr auto feed(chain_state& state, chain_config const& config, std::int32_t in)
    -> std::optional<produced>
{
  auto const cic = dfilter_detail::cic(state.cic, config, in);
  if(not cic)
  {
    return std::nullopt;
  }
  auto const gain   = scale_of(config.scale).value_or(scale_gain{1, 0});
  auto       sample = (*cic - config.offset) * gain.mult;
  sample            = gain.shift >= 0 ? sample << gain.shift : sample >> -gain.shift;
  sample            = dfilter_detail::saturate(sample, state.saturated);
  if(config.rsflt)
  {
    auto const out = dfilter_detail::rsflt(state.rsflt, config, sample, state.saturated);
    if(not out)
    {
      return std::nullopt;
    }
    sample = *out;
  }
  if(config.hpf)
  {
    sample = dfilter_detail::hpf(state.hpf, *config.hpf, sample, state.saturated);
  }
  auto const saturated = std::exchange(state.saturated, false);
  return produced{static_cast<std::int32_t>(sample), saturated};
}

/**
 * @brief Runs INT over one sample kept after the discard, RM0456 39.4.8 "Integrator". The
 *        output keeps the low 24 bits of the shifted sum, two's complement, with no SAT.
 * @param state INT state, zero at the filter enable.
 * @param config Chain settings. An empty `int_value` passes @p in through.
 * @param in Sample from feed() that the discard kept.
 * @return the INT output once `int_value` samples are summed, with their saturations ORed.
 */
[[nodiscard]] constexpr auto integrate(int_state& state, chain_config const& config, produced in)
    -> std::optional<produced>
{
  if(not config.int_value)
  {
    return in;
  }
  state.sum += in.pcm;
  state.saturated = state.saturated or in.saturated;
  if(++state.count < *config.int_value)
  {
    return std::nullopt;
  }
  auto const     shift = dfilter_detail::int_shift[static_cast<std::size_t>(config.int_div)];
  auto const     sum   = static_cast<std::uint64_t>(state.sum >> shift);
  produced const out{
      static_cast<std::int32_t>(dfilter_detail::sign_extend(sum, 24)), state.saturated};
  state = {};
  return out;
}

/**
 * @brief SADCR.SADST codes of the sound activity detector, RM0456 40.8.11.
 */
enum class sad_phase : std::uint8_t
{
  learn   = 0,
  monitor = 1,
  detect  = 3,
};

/**
 * @brief Sound activity detector settings, the SADCR and SADCFGR field codes of RM0456 40.8.11
 *        and 40.8.12. `threshold` is SNTHR, 0 to 9.
 */
struct sad_config
{
  std::uint8_t  mode{};
  std::uint8_t  frame{};
  std::uint8_t  learn{};
  std::uint8_t  hangover{};
  std::uint8_t  slope{};
  std::uint8_t  threshold{};
  bool          hysteresis{};
  std::uint16_t floor{};

  friend auto operator==(sad_config const&, sad_config const&) -> bool = default;
};

/**
 * @brief Sound activity detector state, reset at each SADEN 0 to 1. `noise` is ANLVL with 12
 *        fractional bits.
 */
struct sad_state
{
  sad_phase     phase{sad_phase::learn};
  std::uint32_t sum{};
  std::uint16_t count{};
  std::uint8_t  learned{};
  std::uint32_t learn_sum{};
  std::uint32_t noise{};
  std::uint16_t hang{};

  friend auto operator==(sad_state const&, sad_state const&) -> bool = default;
};

/**
 * @brief One frame of the sound activity detector: SDLVL, ANLVL's integer part, the state
 *        after the frame and whether the frame entered or left DETECT.
 */
struct sad_frame
{
  std::uint16_t level;
  std::uint16_t noise;
  sad_phase     phase;
  bool          changed;

  friend auto operator==(sad_frame const&, sad_frame const&) -> bool = default;
};

namespace dfilter_detail
{
constexpr int                           noise_fraction = 12;
constexpr std::uint32_t                 noise_max      = (std::uint32_t{1} << 27) - 1;
constexpr std::uint32_t                 level_max      = 0x7FFF;
constexpr std::array<std::uint64_t, 10> sad_halves{3, 4, 6, 8, 12, 16, 24, 32, 48, 64};

constexpr auto sad_rise(std::uint32_t noise, std::uint8_t slope) -> std::uint32_t
{
  return std::min(noise + (noise >> (12 - slope)), noise_max);
}

constexpr auto sad_fall(std::uint32_t noise, std::uint8_t slope) -> std::uint32_t
{
  auto const shift = 10 - slope;
  return noise - ((noise + (std::uint32_t{1} << shift) - 1) >> shift);
}

constexpr auto sad_track(std::uint32_t noise, std::uint32_t level, std::uint8_t slope)
    -> std::uint32_t
{
  auto const down = sad_fall(noise, slope);
  return std::uint64_t{level} << noise_fraction < down ? down : sad_rise(noise, slope);
}

constexpr auto sad_above(std::uint64_t signal, std::uint64_t base, std::size_t code) -> bool
{
  return 2 * signal > base * sad_halves[code];
}
}

/**
 * @brief Runs the sound activity detector over one DFLT0 output sample, RM0456 40.4.10.
 *
 *        The detector takes PCM[23:8]. SDLVL is the mean of its absolute value over the frame,
 *        rounded down by a shift, saturated to 15 bits. The rest is popcorn's fixed point, the
 *        RM gives none: ANLVL keeps 12 fractional bits, ANLVL_UP adds the value shifted right by
 *        12 - ANSLP, ANLVL_DN subtracts it shifted right by 10 - ANSLP rounded up, so the
 *        product is rounded down and a level of 1 can still fall, and every value saturates below
 *        2^15. Thresholds compare exactly against the Table 400 gains. With SADMOD 00 a frame that
 *        does not trigger against the ANLVL before it updates ANLVL, floored at ANMIN, SADMOD 1x
 *        updates first and compares the new ANLVL x gain against 4 x ANMIN, SADMOD 01 compares
 *        SDLVL against ANMIN x gain and keeps the LEARN mean. Every mode starts with LEARN frames,
 *        which never trigger, Figure 380. A frame under the threshold in DETECT returns to MONITOR
 *        when the hangover counter is 0, else decrements it, Figure 380. HYSTEN is ignored with
 *        SADMOD 1x, where RM0456 forbids it.
 * @param state Detector state, default at the enable.
 * @param config Field codes, `threshold` 0 to 9.
 * @param pcm Signed 24-bit DFLT0 output sample.
 * @return the frame this sample completes.
 */
[[nodiscard]] constexpr auto sad_feed(sad_state& state, sad_config const& config, std::int32_t pcm)
    -> std::optional<sad_frame>
{
  using namespace dfilter_detail;
  auto const msb = pcm >> 8;
  state.sum += static_cast<std::uint32_t>(msb < 0 ? -msb : msb);
  auto const frame_bits = 3 + std::min<int>(config.frame, 6);
  if(++state.count < (1u << frame_bits))
  {
    return std::nullopt;
  }
  auto const level = std::min(state.sum >> frame_bits, level_max);
  state.sum        = 0;
  state.count      = 0;
  auto changed     = false;
  if(state.phase == sad_phase::learn)
  {
    state.learn_sum += level;
    auto const learn_bits = 1 + std::min<int>(config.learn, 4);
    if(++state.learned == (1u << learn_bits))
    {
      state.noise = state.learn_sum << (noise_fraction - learn_bits);
      state.phase = sad_phase::monitor;
    }
    return sad_frame{
        static_cast<std::uint16_t>(level),
        static_cast<std::uint16_t>(state.noise >> noise_fraction), state.phase, changed};
  }
  std::size_t code = config.threshold;
  if(config.hysteresis and config.mode < 2 and state.phase == sad_phase::detect and code != 0)
  {
    --code;
  }
  auto const shifted_level = std::uint64_t{level} << noise_fraction;
  auto const floor         = std::uint64_t{config.floor} << noise_fraction;
  auto       triggered     = false;
  if(config.mode == 0)
  {
    triggered = sad_above(shifted_level, state.noise, code);
    if(not triggered)
    {
      state.noise = std::max<std::uint32_t>(
          sad_track(state.noise, level, config.slope), static_cast<std::uint32_t>(floor));
    }
  }
  else if(config.mode == 1)
  {
    triggered = sad_above(level, config.floor, code);
  }
  else
  {
    state.noise = sad_track(state.noise, level, config.slope);
    triggered   = std::uint64_t{state.noise} * sad_halves[code] > 8 * floor;
  }
  if(triggered)
  {
    changed     = state.phase == sad_phase::monitor;
    state.phase = sad_phase::detect;
    state.hang  = static_cast<std::uint16_t>(4u << config.hangover);
  }
  else if(state.phase == sad_phase::detect)
  {
    if(state.hang == 0)
    {
      state.phase = sad_phase::monitor;
      changed     = true;
    }
    else
    {
      --state.hang;
    }
  }
  return sad_frame{
      static_cast<std::uint16_t>(level), static_cast<std::uint16_t>(state.noise >> noise_fraction),
      state.phase, changed};
}
}
