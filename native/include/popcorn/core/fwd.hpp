#pragma once

namespace popcorn::core
{
/** @brief Virtual clock of a machine, defined in `popcorn/core/clock.hpp`. */
class clock;

/** @brief Event queue of a machine, defined in `popcorn/core/scheduler.hpp`. */
class scheduler;

/** @brief One model's owner-stamped access to the event queue, defined in
 * `popcorn/core/scheduler.hpp`. */
class event_port;

/**
 * @brief Machine services for the arch module and models: deferred stop, guest bus access and
 *        the trace ring, defined in `popcorn/core/services.hpp`.
 */
class machine_services;

/** @brief Emulated MCU, defined in `popcorn/core/machine.hpp`. */
class machine;

/** @brief Options a machine is built with, defined in `popcorn/core/machine.hpp`. */
struct machine_options;

/** @brief One record of the trace ring, defined in `popcorn/core/trace.hpp`. */
struct trace_record;
}
