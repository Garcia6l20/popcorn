#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace popcorn::periph::stm32
{
/**
 * @brief CIC filter order, valued as the DFLTxCICR.CICMOD codes of RM0456 39.8.18.
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
 * @brief INT division ratio, valued as the DFLTxINTR.INTDIV codes of RM0456 39.8.23.
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
};

/**
 * @brief Gain of a SCALE code: even steps are pure shifts, odd steps x1.5 times a shift.
 * @param code SCALE field value, 0x00 to 0x18 or 0x20 to 0x2F.
 * @return the gain, empty for a reserved code.
 */
constexpr auto scale_of(std::uint8_t code) -> std::optional<scale_gain>
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
 * `decimation` is MCICD + 1. `hpf` holds HPFC when the high-pass filter is on. `int_value`
 * holds INTVAL + 1, empty for INTVAL 0, which bypasses INT and ignores `int_div`.
 */
struct chain_config
{
  cic_order                   order{cic_order::fastsinc};
  std::uint16_t               decimation{1};
  std::int32_t                offset{};
  std::uint8_t                scale{};
  bool                        rsflt{};
  bool                        rsflt_decimate{true};
  std::optional<std::uint8_t> hpf;
  std::optional<std::uint8_t> int_value;
  int_divider                 int_div{int_divider::by_128};
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
};

/**
 * @brief Direct form I history of one biquad, the newest sample first.
 */
struct biquad_state
{
  std::array<std::int64_t, 2> x{};
  std::array<std::int64_t, 2> y{};
};

/**
 * @brief RSFLT state: two biquads and the decimation phase.
 */
struct rsflt_state
{
  std::array<biquad_state, 2> stage{};
  std::uint8_t                count{};
};

/**
 * @brief HPF state: previous input and previous saturated output.
 */
struct hpf_state
{
  std::int64_t x1{};
  std::int64_t y1{};
};

/**
 * @brief INT state: running sum, samples summed and their saturation.
 */
struct int_state
{
  std::int64_t sum{};
  std::uint8_t count{};
  bool         saturated{};
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
};

/**
 * @brief One sample out of a chain stage, with every saturation since the previous one.
 */
struct produced
{
  std::int32_t pcm;
  bool         saturated;
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
  auto const fast  = config.order == cic_order::fastsinc;
  auto const order = fast ? std::size_t{2} : static_cast<std::size_t>(config.order);
  auto       carry = static_cast<std::uint64_t>(static_cast<std::int64_t>(in));
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
 * @param config Chain settings, SCALE a valid code.
 * @param in +1 for a bit 1, -1 for a bit 0.
 * @return the HPF output when the chain produced a sample, with every saturation since the
 *         previous one.
 */
constexpr auto feed(chain_state& state, chain_config const& config, std::int32_t in)
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
constexpr auto integrate(int_state& state, chain_config const& config, produced in)
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
}
