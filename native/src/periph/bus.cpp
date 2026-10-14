#include <popcorn/periph/i2c.hpp>
#include <popcorn/periph/spi.hpp>
#include <popcorn/periph/uart.hpp>

namespace popcorn::periph
{
spi_target::~spi_target()                   = default;
spi_controller_sink::~spi_controller_sink() = default;
spi_controller_port::~spi_controller_port() = default;
i2c_target::~i2c_target()                   = default;
uart_endpoint::~uart_endpoint()             = default;
uart_port::~uart_port()                     = default;
capture_endpoint::~capture_endpoint()       = default;
}
