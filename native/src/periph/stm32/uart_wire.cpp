#include <popcorn/core/errors.hpp>
#include <popcorn/periph/stm32/uart_wire.hpp>

#include <reflex/serde/bson.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace popcorn::periph::stm32
{
namespace
{
constexpr std::uint64_t ps_per_second = 1'000'000'000'000;

auto half_bits(std::uint32_t word, std::uint32_t half_stops) -> std::uint64_t
{
  return 2 * (1 + std::uint64_t{word}) + half_stops;
}

auto shifted(core::sys_time const& at, core::sys_time const& by) -> core::sys_time
{
  return {at.ps + by.ps};
}

constinit thread_local int registrations_held = 0;

class registration_hold
{
public:
  explicit registration_hold(link_registration& registration) : registration_{registration}
  {
    contract_assert(registrations_held == 0);
    registration_.contenders.fetch_add(1);
    registration_.mutex.lock();
    registration_.contenders.fetch_sub(1);
    ++registrations_held;
  }

  registration_hold(registration_hold const&)                    = delete;
  auto operator=(registration_hold const&) -> registration_hold& = delete;
  registration_hold(registration_hold&&)                         = delete;
  auto operator=(registration_hold&&) -> registration_hold&      = delete;

  ~registration_hold()
  {
    --registrations_held;
    registration_.mutex.unlock();
  }

private:
  link_registration& registration_;
};

class sole_tapper
{
public:
  explicit sole_tapper(std::atomic<bool>& tapping) noexcept : tapping_{tapping}
  {
    auto const taken = tapping_.exchange(true, std::memory_order_relaxed);
    contract_assert(not taken);
  }

  sole_tapper(sole_tapper const&)                    = delete;
  auto operator=(sole_tapper const&) -> sole_tapper& = delete;
  sole_tapper(sole_tapper&&)                         = delete;
  auto operator=(sole_tapper&&) -> sole_tapper&      = delete;

  ~sole_tapper()
  {
    tapping_.store(false, std::memory_order_relaxed);
  }

private:
  std::atomic<bool>& tapping_;
};

auto overlaps(detail::line_cut const& cut, core::sys_time start, core::sys_time end) -> bool
{
  return cut.start < end and start < cut.end;
}

auto inside(detail::line_cut const& cut, core::sys_time at) -> bool
{
  return cut.start <= at and at < cut.end;
}

auto earliest(std::optional<core::link_fault> kept, std::optional<core::link_fault> found)
    -> std::optional<core::link_fault>
{
  if(not kept or (found and found->at < kept->at))
  {
    return found;
  }
  return kept;
}
}

auto line_shape::frame_ps() const noexcept -> std::uint64_t
{
  return half_bits(word_bits, half_stops) * ps_per_second / (2 * std::uint64_t{baud});
}

auto line_shape::differs(line_frame const& frame) const noexcept -> bool
{
  if(frame.word_bits != word_bits or frame.half_stops != half_stops or frame.parity != parity)
  {
    return true;
  }
  auto const expected = frame_ps();
  auto const off =
      frame.frame_ps > expected ? frame.frame_ps - expected : expected - frame.frame_ps;
  return 100 * off > 3 * expected;
}

tap_queue::tap_queue(std::size_t capacity) : capacity_{capacity}
{}

void tap_queue::push(tap_frame const& frame)
{
  core::link_guard const held{mutex_};
  if(frames_.size() == capacity_)
  {
    frames_.pop_front();
    ++dropped_;
  }
  frames_.push_back(frame);
}

auto tap_queue::take() -> std::vector<tap_frame>
{
  core::link_guard const held{mutex_};
  std::vector<tap_frame> out(frames_.begin(), frames_.end());
  frames_.clear();
  return out;
}

auto tap_queue::dropped() const -> std::uint64_t
{
  core::link_guard const held{mutex_};
  return dropped_;
}

uart_wire::uart_wire(
    wire_ends                      ends,
    wire_lanes                     lanes,
    core::sys_time                 latency,
    line_shape                     declared,
    wire_receiver                  into,
    std::shared_ptr<uart_endpoint> forward,
    std::size_t                    tap_capacity)
    : ends_{ends}, lanes_{lanes}, latency_{latency}, declared_{declared}, into_{std::move(into)},
      forward_{std::move(forward)}, tap_{tap_capacity},
      registration_{std::make_shared<link_registration>()}
{}

void uart_wire::share_registration(std::shared_ptr<link_registration> registration)
{
  registration_ = std::move(registration);
}

auto uart_wire::shares_registration(uart_wire const& other) const noexcept -> bool
{
  return registration_ == other.registration_;
}

auto uart_wire::cuts_before(core::sys_time reach) const -> std::vector<detail::line_cut>
{
  core::link_guard const held{inject_mutex_};
  return {
      state_.cuts.begin(),
      std::ranges::lower_bound(state_.cuts, reach, {}, &detail::line_cut::start)};
}

auto uart_wire::mailbox(std::uint64_t window) -> std::vector<detail::wire_post>&
{
  return window % 2 == 0 ? state_.mail_even : state_.mail_odd;
}

void uart_wire::started(line_frame const& frame)
{
  core::time_map const map{state_.sender_base};
  detail::wire_post    post{
      .frame = frame, .start = map.to_system(frame.start), .end = map.to_system(frame.end)};
  {
    core::link_guard const held{edit_mutex_};
    post.index        = state_.started++;
    auto const edited = std::ranges::find_if(state_.edits, [&](detail::armed_edit const& edit) {
      return edit.first <= post.index and post.index < edit.first + edit.count;
    });
    if(edited != state_.edits.end())
    {
      if(edited->data.empty())
      {
        post.dropped = true;
      }
      else
      {
        post.frame.data =
            with_parity(edited->data[post.index - edited->first], frame.word_bits, frame.parity);
      }
      if(post.index + 1 == edited->first + edited->count)
      {
        state_.edits.erase(edited);
      }
    }
  }
  state_.flight_start = post.start;
  state_.flight_end   = post.end;
  mailbox(state_.window).push_back(post);
  if(forward_)
  {
    forward_->started(frame);
  }
}

void uart_wire::aborted(vtime at)
{
  auto const system = core::time_map{state_.sender_base}.to_system(at);
  mailbox(state_.window).push_back({.abort = true, .start = system, .end = system});
  if(forward_)
  {
    forward_->aborted(at);
  }
}

void uart_wire::transmitted(std::uint16_t data, vtime at, bool brk)
{
  if(forward_)
  {
    forward_->transmitted(data, at, brk);
  }
}

auto uart_wire::sender() const noexcept -> std::size_t
{
  return ends_.sender;
}

auto uart_wire::receiver() const noexcept -> std::size_t
{
  return ends_.receiver;
}

auto uart_wire::latency() const noexcept -> core::sys_time
{
  return latency_;
}

void uart_wire::enter_window(std::uint64_t window, core::time_map sender)
{
  state_.window      = window;
  state_.sender_base = sender.base();
}

void uart_wire::sender_reset(core::sys_time at)
{
  if(state_.flight_start < at and at < state_.flight_end)
  {
    mailbox(state_.window).push_back({.abort = true, .start = at, .end = at});
    state_.flight_end = at;
  }
}

void uart_wire::receiver_reset(core::sys_time at)
{
  state_.resets.push_back(at);
}

auto uart_wire::reset_drops(detail::line_span const& span) const -> bool
{
  return std::ranges::any_of(state_.resets, [&](core::sys_time const& reset) {
    return reset < span.end and span.start < shifted(reset, latency_);
  });
}

auto uart_wire::arm(detail::armed_edit edit) -> bool
{
  core::link_guard const held{edit_mutex_};
  auto const overlaps = std::ranges::any_of(state_.edits, [&](detail::armed_edit const& armed) {
    return armed.first < edit.first + edit.count and edit.first < armed.first + armed.count;
  });
  auto const wraps    = edit.count > std::numeric_limits<std::uint64_t>::max() - edit.first;
  if(wraps or edit.first < state_.started or overlaps)
  {
    return false;
  }
  state_.edits.push_back(std::move(edit));
  return true;
}

auto uart_wire::replace(std::uint64_t index, std::span<std::uint16_t const> data) -> bool
{
  return arm({
      .first = index, .count = data.size(), .data = {data.begin(), data.end()}
  });
}

auto uart_wire::drop(std::uint64_t index, std::uint64_t count) -> bool
{
  return arm({.first = index, .count = count, .data = {}});
}

auto uart_wire::posted() const -> std::uint64_t
{
  core::link_guard const held{edit_mutex_};
  return state_.started;
}

auto uart_wire::check_shape(detail::wire_post const& post) -> std::optional<core::link_fault>
{
  if(state_.mismatch_logged or not declared_.differs(post.frame))
  {
    return std::nullopt;
  }
  state_.mismatch_logged = true;
  auto const baud        = half_bits(post.frame.word_bits, post.frame.half_stops)
                         * ps_per_second
                         / (2 * std::max<std::uint64_t>(post.frame.frame_ps, 1));
  if(into_.log)
  {
    into_.log(
        {.kind       = core::log_kind::model_warning,
         .peripheral = into_.unit,
         .message    = std::format(
             "frame at {} baud, {} data bits, {} half stop bits, parity {} on a link declared {} "
             "baud, {} data bits, {} half stop bits, parity {}",
             baud, post.frame.word_bits, post.frame.half_stops, post.frame.parity, declared_.baud,
             declared_.word_bits, declared_.half_stops, declared_.parity)});
  }
  if(not into_.strict)
  {
    return std::nullopt;
  }
  return core::link_fault{
      .at     = post.start,
      .member = ends_.receiver,
      .error  = std::make_exception_ptr(model_error(into_.unit, "", 0))};
}

void uart_wire::collect(std::vector<detail::wire_post> const& posts, std::vector<tap_frame>& into)
    const
{
  for(auto const& post : posts)
  {
    if(post.abort)
    {
      auto const lost = std::ranges::find_if(into, [&](tap_frame const& frame) {
        return not frame.injected
           and not frame.lost
           and frame.start < post.start
           and post.start < frame.end;
      });
      if(lost != into.end())
      {
        lost->lost    = true;
        lost->end     = post.start;
        lost->arrival = shifted(post.start, latency_);
      }
      continue;
    }
    into.push_back(
        {.from    = ends_.sender,
         .to      = ends_.receiver,
         .index   = post.index,
         .data    = data_of(post.frame.data, post.frame.word_bits, post.frame.parity),
         .start   = post.start,
         .end     = post.end,
         .arrival = shifted(post.end, latency_),
         .brk     = post.frame.kind == frame_kind::brk,
         .dropped = post.dropped});
  }
}

void uart_wire::emit(
    std::vector<tap_frame>&           from,
    core::sys_time                    through,
    std::span<detail::line_cut const> cuts)
{
  auto const key = [](tap_frame const& frame) {
    return std::tuple{frame.end.ps, not frame.injected, frame.index};
  };
  auto const ready = std::ranges::stable_partition(
      from, [&](tap_frame const& frame) { return frame.end <= through; });
  std::vector<tap_frame> settled(from.begin(), ready.begin());
  from.erase(from.begin(), ready.begin());
  std::ranges::sort(settled, {}, key);
  for(auto& frame : settled)
  {
    if(frame.dropped)
    {
      continue;
    }
    auto const shift = frame.injected ? latency_ : core::sys_time{0};
    auto const start = core::sys_time{frame.start.ps - shift.ps};
    auto const end   = core::sys_time{frame.end.ps - shift.ps};
    for(auto const& one : cuts)
    {
      if(not overlaps(one, start, end))
      {
        continue;
      }
      frame.cut = true;
      if(start < one.start or one.end < end)
      {
        frame.lost = true;
      }
    }
  }
  auto const cursor =
      std::tuple{state_.tapped_end.ps, not state_.tapped_injected, state_.tapped_index};
  for(auto const& frame : settled)
  {
    if(state_.tapped_any and key(frame) <= cursor)
    {
      continue;
    }
    tap_.push(frame);
    state_.tapped_any      = true;
    state_.tapped_end      = frame.end;
    state_.tapped_injected = frame.injected;
    state_.tapped_index    = frame.index;
  }
}

auto uart_wire::fault_of(detail::line_span const& span) const -> core::link_fault
{
  auto const at     = span.injected ? span.start : span.post;
  auto const member = span.injected ? ends_.receiver : ends_.sender;
  return {
      .at     = at,
      .member = member,
      .error  = std::make_exception_ptr(emulation_error(
          std::format(
              "two frames overlap on the line of {}: the {} frame {} starts at {} ps of system "
              "time while another is on the line",
              into_.unit, span.injected ? "injected" : "peer", span.index, span.start.ps)))};
}

auto uart_wire::overlap(detail::line_span const& span) const -> std::optional<core::link_fault>
{
  std::optional<core::link_fault> found;
  for(auto const& other : state_.delivered)
  {
    if(other.start < span.end and span.start < other.end)
    {
      auto const later_is_new =
          span.start > other.start or (span.start == other.start and span.injected);
      auto const* dated = later_is_new ? &span : &other;
      if(span.injected != other.injected)
      {
        auto const& injected = span.injected ? span : other;
        auto const& peer     = span.injected ? other : span;
        dated                = peer.post > injected.start ? &peer : &injected;
      }
      found = earliest(found, fault_of(*dated));
    }
  }
  return found;
}

void uart_wire::deliver_injection(
    detail::queued_injection const&   frame,
    core::time_map                    receiver,
    bool                              dropped,
    std::span<detail::line_cut const> cuts)
{
  auto const sent = core::sys_time{frame.start.ps - latency_.ps};
  auto const suppressed =
      std::ranges::any_of(cuts, [&](detail::line_cut const& cut) { return inside(cut, sent); });
  if(not dropped and not suppressed)
  {
    into_.port->receive_at(
        {.data       = with_parity(frame.data, declared_.word_bits, declared_.parity),
         .word_bits  = declared_.word_bits,
         .half_stops = declared_.half_stops,
         .parity     = declared_.parity,
         .kind       = frame_kind::data,
         .frame_ps   = declared_.frame_ps(),
         .start      = receiver.to_local(frame.start),
         .end        = receiver.to_local(frame.end)},
        lanes_.injected);
  }
  state_.delivered.push_back(
      {.start      = frame.start,
       .end        = frame.end,
       .injected   = true,
       .post       = frame.start,
       .index      = frame.index,
       .suppressed = suppressed or dropped});
  state_.untapped.push_back(
      {.from     = ends_.sender,
       .to       = ends_.receiver,
       .index    = frame.index,
       .data     = data_of(frame.data, declared_.word_bits, declared_.parity),
       .start    = frame.start,
       .end      = frame.end,
       .arrival  = frame.end,
       .injected = true});
}

auto uart_wire::quiet() const -> bool
{
  core::link_guard const held{inject_mutex_};
  return state_.mail_even.empty()
     and state_.mail_odd.empty()
     and state_.injections.empty()
     and state_.carried.empty()
     and std::ranges::none_of(state_.cuts, [](detail::line_cut const& cut) {
           return cut.low and (not cut.held or (not cut.open and not cut.released));
         });
}

auto uart_wire::truncating(
    detail::line_span const&          span,
    std::span<detail::line_cut const> cuts,
    core::sys_time                    before) const -> detail::line_cut const*
{
  if(span.suppressed or span.truncated)
  {
    return nullptr;
  }
  auto const crossed = std::ranges::find_if(cuts, [&](detail::line_cut const& cut) {
    auto const at = shifted(cut.start, latency_);
    return not cut.low and cut.start < before and span.start < at and at < span.end;
  });
  return crossed == cuts.end() ? nullptr : &*crossed;
}

void uart_wire::truncate(
    detail::line_span&                span,
    std::span<detail::line_cut const> cuts,
    core::sys_time                    before,
    core::time_map                    receiver)
{
  auto const* const crossed = truncating(span, cuts, before);
  if(crossed == nullptr)
  {
    return;
  }
  into_.port->cancel_in_flight(
      receiver.to_local(shifted(crossed->start, latency_)),
      span.injected ? lanes_.injected : lanes_.peer);
  span.truncated = true;
}

auto uart_wire::drain(
    std::uint64_t         window,
    core::sys_time const& barrier,
    core::sys_time const& next,
    core::time_map        receiver) -> std::optional<core::link_fault>
{
  sole_tapper const                     sole{tapping_};
  auto const                            shown = state_.last_barrier;
  std::vector<detail::line_cut>         cuts;
  std::vector<detail::queued_injection> due;
  std::vector<detail::wire_post>        held_over;
  {
    registration_hold const registered{*registration_};
    core::link_guard const  held{inject_mutex_};
    cuts.assign(
        state_.cuts.begin(),
        std::ranges::lower_bound(state_.cuts, next, {}, &detail::line_cut::start));
    auto const later = std::ranges::stable_partition(
        state_.injections,
        [&](detail::queued_injection const& frame) { return frame.start < next; });
    due.assign(state_.injections.begin(), later.begin());
    state_.injections.erase(state_.injections.begin(), later.begin());
    state_.drained_through = std::max(state_.drained_through, next);
    held_over              = std::exchange(state_.carried, {});
  }
  std::erase_if(state_.delivered, [&](detail::line_span const& span) {
    return span.end <= state_.last_barrier;
  });
  std::erase_if(state_.resets, [&](core::sys_time const& reset) {
    return shifted(reset, latency_) <= state_.last_barrier;
  });
  std::vector<detail::wire_post> carry;
  auto const                     sent_of = [&](detail::wire_post const& post) {
    return detail::line_span{
        .start = shifted(post.start, latency_),
        .end   = shifted(post.end, latency_),
        .post  = post.start,
        .index = post.index};
  };
  auto const closed_over = [&](detail::wire_post const& post) {
    return std::ranges::any_of(cuts, [&](detail::line_cut const& cut) {
      return not cut.open and inside(cut, post.start);
    });
  };
  auto const cancels_any = [&](detail::wire_post const& post) {
    auto const at = shifted(post.start, latency_);
    return std::ranges::any_of(state_.delivered, [&](detail::line_span const& span) {
      return not span.injected
         and not span.suppressed
         and span.start < at
         and at <= span.end
         and not span.truncated
         and not reset_drops(span)
         and truncating(span, cuts, post.start) == nullptr;
    });
  };
  auto const hand_abort = [&](detail::wire_post const& post) {
    auto const at = shifted(post.start, latency_);
    for(auto& span : state_.delivered)
    {
      if(not span.injected and span.start < at and at <= span.end)
      {
        truncate(span, cuts, post.start, receiver);
      }
    }
    into_.port->cancel_in_flight(receiver.to_local(at), lanes_.peer);
  };
  auto const hand_frame = [&](detail::wire_post const& post, detail::line_span& span) {
    auto const sent = sent_of(post);
    span.carried    = false;
    span.suppressed =
        reset_drops(sent) or std::ranges::any_of(cuts, [&](detail::line_cut const& cut) {
          return inside(cut, post.start);
        });
    if(span.suppressed)
    {
      return;
    }
    auto frame  = post.frame;
    frame.start = receiver.to_local(sent.start);
    frame.end   = receiver.to_local(sent.end);
    into_.port->receive_at(frame, lanes_.peer);
  };
  for(auto const& post : held_over)
  {
    if(post.abort)
    {
      if(post.start < barrier)
      {
        hand_abort(post);
      }
      else if(cancels_any(post))
      {
        carry.push_back(post);
      }
      continue;
    }
    auto const span = std::ranges::find_if(state_.delivered, [&](detail::line_span const& one) {
      return one.carried and not one.injected and one.index == post.index;
    });
    contract_assert(span != state_.delivered.end());
    if(post.start < barrier)
    {
      hand_frame(post, *span);
    }
    else if(reset_drops(sent_of(post)) or closed_over(post))
    {
      span->carried    = false;
      span->suppressed = true;
    }
    else
    {
      carry.push_back(post);
    }
  }
  auto&                           posts = mailbox(window);
  std::optional<core::link_fault> fault;
  for(auto const& post : posts)
  {
    if(post.abort)
    {
      auto const at = shifted(post.start, latency_);
      for(auto& span : state_.delivered)
      {
        if(not span.injected and span.start < at and at < span.end)
        {
          span.end = at;
        }
      }
      if(post.start < barrier)
      {
        hand_abort(post);
      }
      else if(cancels_any(post))
      {
        carry.push_back(post);
      }
      continue;
    }
    if(post.dropped)
    {
      continue;
    }
    fault     = earliest(fault, check_shape(post));
    auto span = sent_of(post);
    if(auto clash = overlap(span))
    {
      fault           = earliest(fault, std::move(clash));
      span.suppressed = true;
      state_.delivered.push_back(span);
      continue;
    }
    if(post.start >= barrier and (reset_drops(span) or closed_over(post)))
    {
      span.suppressed = true;
      state_.delivered.push_back(span);
      continue;
    }
    span.carried = post.start >= barrier;
    state_.delivered.push_back(span);
    if(span.carried)
    {
      carry.push_back(post);
      continue;
    }
    hand_frame(post, state_.delivered.back());
  }
  collect(posts, state_.untapped);
  posts.clear();
  std::ranges::sort(due, {}, [](detail::queued_injection const& frame) {
    return std::pair{frame.start.ps, frame.index};
  });
  for(auto const& frame : due)
  {
    detail::line_span span{
        .start    = frame.start,
        .end      = frame.end,
        .injected = true,
        .post     = frame.start,
        .index    = frame.index};
    if(auto clash = overlap(span))
    {
      fault           = earliest(fault, std::move(clash));
      span.suppressed = true;
      state_.delivered.push_back(span);
      continue;
    }
    deliver_injection(frame, receiver, reset_drops(span), cuts);
  }
  if(not cuts.empty())
  {
    for(auto& span : state_.delivered)
    {
      truncate(span, cuts, barrier, receiver);
    }
  }
  for(auto& cut : cuts)
  {
    if(not cut.low)
    {
      continue;
    }
    auto const from = shifted(cut.start, latency_);
    if(not cut.held and cut.start < barrier)
    {
      cut.held = true;
      if(reset_drops(
             {.start = from,
              .end   = core::sys_time{from.ps + static_cast<std::int64_t>(declared_.frame_ps())}}))
      {
        cut.released = true;
      }
      else
      {
        into_.port->hold_line(receiver.to_local(from), lanes_.peer);
      }
    }
    if(cut.held and not cut.open and not cut.released and cut.end < barrier)
    {
      cut.released = true;
      into_.port->release_line(receiver.to_local(shifted(cut.end, latency_)), lanes_.peer);
    }
  }
  contract_assert(not fault or fault->at >= shown);
  if(barrier < next)
  {
    emit(state_.untapped, shown, cuts);
  }
  {
    registration_hold const registered{*registration_};
    core::link_guard const  held{inject_mutex_};
    auto&                   live = state_.cuts;
    auto const ahead = std::ranges::lower_bound(live, barrier, {}, &detail::line_cut::start);
    for(auto const& done : cuts)
    {
      if(not done.low or not done.held)
      {
        continue;
      }
      auto const found =
          std::ranges::lower_bound(live.begin(), ahead, done.start, {}, &detail::line_cut::start);
      if(found != ahead and found->start == done.start)
      {
        found->held     = true;
        found->released = found->released or done.released;
      }
    }
    auto const kept = std::remove_if(live.begin(), ahead, [&](detail::line_cut const& cut) {
      return not cut.open
         and (not cut.low or cut.released)
         and shifted(cut.end, latency_) <= barrier
         and std::ranges::none_of(state_.untapped, [&](tap_frame const& frame) {
               return frame.injected ? frame.start < shifted(cut.end, latency_)
                                     : frame.start < cut.end;
             });
    });
    live.erase(kept, ahead);
    state_.carried      = std::move(carry);
    state_.last_barrier = barrier;
  }
  return fault;
}

auto uart_wire::inject(
    std::span<std::uint16_t const>  data,
    core::sys_time                  at,
    std::optional<stopped_receiver> stopped) -> std::expected<void, inject_refusal>
{
  auto const                            frame_ps = static_cast<std::int64_t>(declared_.frame_ps());
  std::vector<detail::queued_injection> frames;
  for(std::size_t offset = 0; offset < data.size(); ++offset)
  {
    auto const start = at.ps + static_cast<std::int64_t>(offset) * frame_ps;
    frames.push_back({.data = data[offset], .start = {start}, .end = {start + frame_ps}});
  }
  if(not stopped)
  {
    core::link_guard const held{inject_mutex_};
    if(at < state_.drained_through)
    {
      return std::unexpected{
          inject_refusal{
                         .reason = inject_refusal_reason::before_drained, .bound = state_.drained_through}
      };
    }
    for(auto& frame : frames)
    {
      frame.index = state_.injected++;
      state_.injections.push_back(frame);
    }
    return {};
  }
  sole_tapper const sole{tapping_};
  if(at < stopped->now)
  {
    return std::unexpected{
        inject_refusal{.reason = inject_refusal_reason::before_receiver, .bound = stopped->now}
    };
  }
  std::vector<detail::line_span> busy = state_.delivered;
  for(auto const* posts : {&mailbox(state_.window + 1), &mailbox(state_.window)})
  {
    for(auto const& post : *posts)
    {
      auto const start = shifted(post.start, latency_);
      if(post.abort)
      {
        for(auto& span : busy)
        {
          if(not span.injected and span.start < start and start < span.end)
          {
            span.end = start;
          }
        }
      }
      else if(not post.dropped)
      {
        busy.push_back({.start = start, .end = shifted(post.end, latency_), .post = post.start});
      }
    }
  }
  {
    core::link_guard const held{inject_mutex_};
    for(auto const& queued : state_.injections)
    {
      busy.push_back({.start = queued.start, .end = queued.end, .injected = true});
    }
  }
  for(auto const& frame : frames)
  {
    auto const clash = std::ranges::find_if(busy, [&](detail::line_span const& span) {
      return span.start < frame.end and frame.start < span.end;
    });
    if(clash != busy.end())
    {
      return std::unexpected{
          inject_refusal{.reason = inject_refusal_reason::overlaps, .bound = clash->start}
      };
    }
  }
  core::sys_time reach{};
  {
    core::link_guard const held{inject_mutex_};
    reach = std::max(state_.drained_through, latency_);
  }
  auto const cuts = cuts_before(reach);
  for(auto& frame : frames)
  {
    {
      core::link_guard const held{inject_mutex_};
      frame.index = state_.injected++;
      if(frame.start >= reach)
      {
        state_.injections.push_back(frame);
        continue;
      }
    }
    deliver_injection(
        frame, stopped->map, reset_drops({.start = frame.start, .end = frame.end}), cuts);
    truncate(state_.delivered.back(), cuts, state_.last_barrier, stopped->map);
  }
  return {};
}

void uart_wire::show_through(core::sys_time at)
{
  auto pending = state_.untapped;
  collect(mailbox(state_.window + 1), pending);
  collect(mailbox(state_.window), pending);
  emit(pending, at, cuts_before(at));
}

void uart_wire::release_through(core::sys_time at)
{
  sole_tapper const sole{tapping_};
  {
    core::link_guard const held{inject_mutex_};
    state_.drained_through = std::max(state_.drained_through, at);
  }
  show_through(at);
}

void uart_wire::show_drained(core::sys_time at)
{
  sole_tapper const sole{tapping_};
  emit(state_.untapped, at, cuts_before(at));
}

void uart_wire::finish(core::sys_time at)
{
  sole_tapper const sole{tapping_};
  show_through(at);
}

void uart_wire::save(std::vector<std::byte>& out) const
{
  std::vector<detail::line_cut>  cuts;
  std::vector<detail::wire_post> carried;
  {
    core::link_guard const held{inject_mutex_};
    cuts    = state_.cuts;
    carried = state_.carried;
  }
  core::link_guard const held{edit_mutex_};
  auto                   state = state_;
  state.cuts                   = std::move(cuts);
  state.carried                = std::move(carried);
  reflex::serde::bson::serializer{out}.dump(state);
}

void uart_wire::load(std::span<std::byte const> in)
{
  sole_tapper const  sole{tapping_};
  detail::wire_state loaded;
  try
  {
    loaded = reflex::serde::bson::deserializer{in}.load<detail::wire_state>();
  }
  catch(std::runtime_error const& failure)
  {
    throw config_error(
        "uart wire state", 0, "bytes",
        std::format("a state uart_wire::save wrote: {}", failure.what()));
  }
  core::link_guard const held{edit_mutex_};
  state_ = std::move(loaded);
}

auto uart_wire::tap() noexcept -> tap_queue&
{
  return tap_;
}

auto cut_link(
    uart_wire&                    forth,
    uart_wire&                    back,
    core::sys_time                start,
    std::optional<core::sys_time> end,
    bool                          low,
    std::optional<core::sys_time> now) -> std::expected<core::sys_time, cut_refusal>
{
  contract_assert(&forth != &back);
  contract_assert(forth.shares_registration(back));
  registration_hold const registered{*forth.registration_};
  detail::line_cut const  cut{
      .start = start,
      .end   = end.value_or(core::sys_time{std::numeric_limits<std::int64_t>::max()}),
      .open  = not end,
      .low   = low};
  auto const wires = std::array{&forth, &back};
  auto       bound = now.value_or(core::sys_time{std::numeric_limits<std::int64_t>::min()});
  for(auto* wire : wires)
  {
    core::link_guard const held{wire->inject_mutex_};
    if(now)
    {
      contract_assert(*now >= wire->state_.last_barrier);
    }
    else
    {
      bound = std::max(bound, wire->state_.drained_through);
    }
  }
  if(start < bound)
  {
    return std::unexpected{
        cut_refusal{.reason = cut_refusal_reason::before_bound, .bound = bound}
    };
  }
  for(auto* wire : wires)
  {
    core::link_guard const held{wire->inject_mutex_};
    auto const&            cuts = wire->state_.cuts;
    if(not end and not cuts.empty() and cuts.back().open)
    {
      return std::unexpected{
          cut_refusal{
                      .reason = cut_refusal_reason::already_open,
                      .bound  = cuts.back().start,
                      .end    = cuts.back().end}
      };
    }
    if(auto const hit = std::ranges::upper_bound(cuts, cut.start, {}, &detail::line_cut::end);
       hit != cuts.end() and overlaps(*hit, cut.start, cut.end))
    {
      return std::unexpected{
          cut_refusal{
                      .reason = cut_refusal_reason::overlaps, .bound = hit->start, .end = hit->end}
      };
    }
    if(cuts.size() >= max_waiting_cuts)
    {
      return std::unexpected{cut_refusal{.reason = cut_refusal_reason::full}};
    }
  }
  if(forth.registration_->between_checks_and_commit)
  {
    forth.registration_->between_checks_and_commit();
  }
  for(auto* wire : wires)
  {
    core::link_guard const held{wire->inject_mutex_};
    auto&                  cuts = wire->state_.cuts;
    cuts.insert(std::ranges::upper_bound(cuts, start, {}, &detail::line_cut::start), cut);
  }
  return start;
}

auto restore_link(uart_wire& forth, uart_wire& back, core::sys_time now)
    -> std::expected<core::sys_time, cut_refusal>
{
  contract_assert(&forth != &back);
  contract_assert(forth.shares_registration(back));
  registration_hold const registered{*forth.registration_};
  auto const              wires = std::array{&forth, &back};
  for(auto* wire : wires)
  {
    core::link_guard const held{wire->inject_mutex_};
    contract_assert(now >= wire->state_.last_barrier);
    if(wire->state_.cuts.empty() or not wire->state_.cuts.back().open)
    {
      return std::unexpected{cut_refusal{.reason = cut_refusal_reason::none_open}};
    }
  }
  for(auto* wire : wires)
  {
    core::link_guard const held{wire->inject_mutex_};
    auto&                  cuts = wire->state_.cuts;
    auto&                  open = cuts.back();
    if(now <= open.start)
    {
      cuts.pop_back();
      continue;
    }
    open.end  = now;
    open.open = false;
  }
  return now;
}
}
