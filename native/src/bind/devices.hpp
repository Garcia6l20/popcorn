#pragma once

#include <popcorn/core/description.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/rcc.hpp>

#include <reflex/py.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace popcorn::bind::detail
{
/** @brief Binds `I2CTargetBase` and `SpiTargetBase`, the trampolines over the bus targets. */
void bind_device_bases(reflex::py::nb::module_ scope);

/**
 * @brief Binds `At25sfBase`, `Eeprom24xxBase` and `TableTargetBase`, the native bus devices,
 *        derived from `SpiTargetBase` and `I2CTargetBase`. Call after bind_device_bases().
 */
void bind_native_devices(reflex::py::nb::module_ scope);

/**
 * @brief Factories of the built-in bus models serving the peripherals that have Python devices,
 *        and of the RCC model when the board sets its clock sources.
 * @param description The MCU.
 * @param devices Python targets per peripheral name, `popcorn.board.I2CTarget` or `SpiTarget`
 *        instances, an SPI target carrying its chip select as `_cs_port` and `_cs_pin`.
 * @param rcc Board clock sources of the RCC model, empty for its defaults and no RCC factory.
 * @param python_owned Receives, borrowed, each Python device of @p devices whose reference a
 *        factory now holds, once per reference.
 * @return One factory per named peripheral, plus one for the RCC with @p rcc.
 * @throws config_error when a named peripheral is not an I2C or SPI peripheral of @p description,
 *         a target is of the other bus kind, an object is not a target, or no built-in bus model
 *         serves a peripheral with targets. A peripheral with no targets is skipped. With @p rcc,
 *         when no built-in model serves an RCC of @p description.
 */
[[nodiscard]] auto device_factories(
    mcu_description const&                                            description,
    std::map<std::string, std::vector<reflex::py::nb::object>> const& devices,
    std::optional<periph::stm32::rcc_options> const&                  rcc,
    std::vector<reflex::py::nb::handle>&                              python_owned)
    -> std::map<std::string, periph::model_factory, std::less<>>;
}
