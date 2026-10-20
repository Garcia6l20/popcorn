#pragma once

#include <popcorn/export.hpp>

namespace popcorn::periph
{
/**
 * @brief A model that brings lazily computed state up to the machine's time before a snapshot
 *        saves it. The machine finds it among its models by dynamic_cast and calls it in
 *        description order at the start of every snapshot, before any memory, register, line
 *        level, model state or event is captured.
 */
class POPCORN_API snapshot_listener
{
public:
  /**
   * @brief Called at the start of every snapshot, inside a handler scope, with the clock at the
   *        machine's stopped time. Work done here is what the continuing run would do anyway,
   *        such as consuming input clocked before that time, so it never changes later results.
   * @throws Any exception, which ends the snapshot before anything is captured.
   */
  virtual void before_snapshot() = 0;

protected:
  virtual ~snapshot_listener();
};
}
