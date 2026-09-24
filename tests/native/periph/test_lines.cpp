#include <popcorn/periph/lines.hpp>

#include <popcorn/core/errors.hpp>

#include <doctest/doctest.h>

TEST_CASE("line stubs throw not_implemented and read inactive")
{
  popcorn::periph::irq_line irq;
  CHECK_FALSE(irq.level());
  CHECK_THROWS_AS(irq.raise(), popcorn::not_implemented);
  CHECK_THROWS_AS(irq.lower(), popcorn::not_implemented);
  popcorn::periph::dma_request dma;
  CHECK_THROWS_AS(dma.request(), popcorn::not_implemented);
  CHECK_THROWS_AS(dma.release(), popcorn::not_implemented);
  CHECK_FALSE(dma.level());
}
