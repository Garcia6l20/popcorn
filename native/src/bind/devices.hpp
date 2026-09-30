#pragma once

#include <popcorn/core/description.hpp>
#include <popcorn/periph/model.hpp>

#include <reflex/py.hpp>

#include <map>
#include <string>
#include <vector>

namespace popcorn::bind::detail
{
/** @brief Binds `I2CTargetBase` and `SpiTargetBase`, the trampolines over the bus targets. */
void bind_device_bases(reflex::py::nb::module_ scope);

/**
 * @brief Factories of the built-in bus models serving the peripherals that have Python devices.
 * @param description The MCU.
 * @param devices Python targets per peripheral name, `popcorn.board.I2CTarget` or `SpiTarget`
 *        instances, an SPI target carrying its chip select as `_cs_port` and `_cs_pin`.
 * @return One factory per named peripheral.
 * @throws config_error when a named peripheral is not an I2C or SPI peripheral of @p description,
 *         a target is of the other bus kind, an object is not a target, or no built-in bus model
 *         serves a peripheral with targets. A peripheral with no targets is skipped.
 */
[[nodiscard]] auto device_factories(
    mcu_description const&                                            description,
    std::map<std::string, std::vector<reflex::py::nb::object>> const& devices)
    -> std::map<std::string, periph::model_factory, std::less<>>;
}
