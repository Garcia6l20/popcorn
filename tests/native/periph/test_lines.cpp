#include <popcorn/periph/lines.hpp>

#include <doctest/doctest.h>

TEST_CASE("unbound lines keep their level")
{
  popcorn::periph::irq_line irq;
  CHECK_FALSE(irq.level());
  irq.raise();
  CHECK(irq.level());
  irq.raise();
  CHECK(irq.level());
  irq.lower();
  CHECK_FALSE(irq.level());
  popcorn::periph::dma_request dma;
  dma.request();
  CHECK(dma.level());
  dma.release();
  CHECK_FALSE(dma.level());
}
