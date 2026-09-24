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
 * @brief Machine services for models: deferred stop and guest bus access for bus-mastering
 *        models, defined by step 05 in `popcorn/core/services.hpp`.
 */
class machine_services;
}
