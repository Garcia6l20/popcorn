#pragma once

namespace popcorn::core
{
/** @brief Virtual clock of a machine, defined by step 04 in `popcorn/core/clock.hpp`. */
class clock;

/** @brief Event queue of a machine, defined by step 04 in `popcorn/core/scheduler.hpp`. */
class scheduler;

/** @brief One model's owner-stamped access to the event queue, defined by step 04 in
 * `popcorn/core/scheduler.hpp`. */
class event_port;

/**
 * @brief Machine services for the arch module and models: deferred stop, guest bus access and
 *        the trace ring, defined by step 05 in `popcorn/core/services.hpp`.
 */
class machine_services;

/** @brief One record of the trace ring, defined by step 06 in `popcorn/core/trace.hpp`. */
struct trace_record;
}
