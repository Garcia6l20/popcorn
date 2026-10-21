#include <popcorn/core/errors.hpp>
#include <popcorn/periph/stm32/uart_wire.hpp>

#include <reflex/serde/bson.hpp>

#include <algorithm>
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
      forward_{std::move(forward)}, tap_{tap_capacity}
{}

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

void uart_wire::emit(std::vector<tap_frame>& from, core::sys_time through)
{
  auto const key = [](tap_frame const& frame) {
    return std::tuple{frame.end.ps, frame.injected, frame.index};
  };
  auto const ready = std::ranges::stable_partition(
      from, [&](tap_frame const& frame) { return frame.end <= through; });
  std::vector<tap_frame> settled(from.begin(), ready.begin());
  from.erase(from.begin(), ready.begin());
  std::ranges::sort(settled, {}, key);
  auto const cursor = std::tuple{state_.tapped_end.ps, state_.tapped_injected, state_.tapped_index};
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
      found = earliest(found, fault_of(later_is_new ? span : other));
    }
  }
  return found;
}

void uart_wire::deliver_injection(
    detail::queued_injection const& frame,
    core::time_map                  receiver,
    bool                            dropped)
{
  if(not dropped)
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
      {.start    = frame.start,
       .end      = frame.end,
       .injected = true,
       .post     = frame.start,
       .index    = frame.index});
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
  return state_.mail_even.empty() and state_.mail_odd.empty() and state_.injections.empty();
}

auto uart_wire::drain(
    std::uint64_t         window,
    core::sys_time const& barrier,
    core::sys_time const& next,
    core::time_map        receiver) -> std::optional<core::link_fault>
{
  std::erase_if(state_.delivered, [&](detail::line_span const& span) {
    return span.end <= state_.last_barrier;
  });
  std::erase_if(state_.resets, [&](core::sys_time const& reset) {
    return shifted(reset, latency_) <= state_.last_barrier;
  });
  auto&                           posts = mailbox(window);
  std::optional<core::link_fault> fault;
  for(auto const& post : posts)
  {
    if(post.abort)
    {
      auto const at = shifted(post.start, latency_);
      into_.port->cancel_in_flight(receiver.to_local(at), lanes_.peer);
      for(auto& span : state_.delivered)
      {
        if(not span.injected and span.start < at and at < span.end)
        {
          span.end = at;
        }
      }
      continue;
    }
    if(post.dropped)
    {
      continue;
    }
    fault = earliest(fault, check_shape(post));
    detail::line_span const span{
        .start = shifted(post.start, latency_),
        .end   = shifted(post.end, latency_),
        .post  = post.start,
        .index = post.index};
    if(auto clash = overlap(span))
    {
      fault = earliest(fault, std::move(clash));
      state_.delivered.push_back(span);
      continue;
    }
    state_.delivered.push_back(span);
    if(reset_drops(span))
    {
      continue;
    }
    auto frame  = post.frame;
    frame.start = receiver.to_local(span.start);
    frame.end   = receiver.to_local(span.end);
    into_.port->receive_at(frame, lanes_.peer);
  }
  collect(posts, state_.untapped);
  posts.clear();
  std::vector<detail::queued_injection> due;
  {
    core::link_guard const held{inject_mutex_};
    auto const             later = std::ranges::stable_partition(
        state_.injections,
        [&](detail::queued_injection const& frame) { return frame.start < next; });
    due.assign(state_.injections.begin(), later.begin());
    state_.injections.erase(state_.injections.begin(), later.begin());
    state_.drained_through = std::max(state_.drained_through, next);
  }
  std::ranges::sort(due, {}, [](detail::queued_injection const& frame) {
    return std::pair{frame.start.ps, frame.index};
  });
  for(auto const& frame : due)
  {
    detail::line_span const span{
        .start    = frame.start,
        .end      = frame.end,
        .injected = true,
        .post     = frame.start,
        .index    = frame.index};
    if(auto clash = overlap(span))
    {
      fault = earliest(fault, std::move(clash));
      state_.delivered.push_back(span);
      continue;
    }
    deliver_injection(frame, receiver, reset_drops(span));
  }
  emit(state_.untapped, barrier);
  state_.last_barrier = barrier;
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
  for(auto& frame : frames)
  {
    {
      core::link_guard const held{inject_mutex_};
      frame.index = state_.injected++;
    }
    deliver_injection(frame, stopped->map, false);
  }
  return {};
}

void uart_wire::finish(core::sys_time at)
{
  auto pending = state_.untapped;
  collect(mailbox(state_.window + 1), pending);
  collect(mailbox(state_.window), pending);
  emit(pending, at);
}

void uart_wire::save(std::vector<std::byte>& out) const
{
  core::link_guard const held{edit_mutex_};
  reflex::serde::bson::serializer{out}.dump(state_);
}

void uart_wire::load(std::span<std::byte const> in)
{
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
}
