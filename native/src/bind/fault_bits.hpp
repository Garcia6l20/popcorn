#pragma once

#include <reflex/py.hpp>

#include <cstdint>

namespace popcorn::bind
{
/**
 * @brief Bits of the Arm configurable fault status register, CFSR, Armv8-M ARM B3.2.15. Each
 *        value is the bit position. Bits 0-7 are MMFSR, 8-15 BFSR and 16-31 UFSR.
 */
enum class[[= reflex::py::rename{"CfsrBit"}]] cfsr_bit : std::uint8_t
{
  iaccviol    = 0,
  daccviol    = 1,
  munstkerr   = 3,
  mstkerr     = 4,
  mlsperr     = 5,
  mmarvalid   = 7,
  ibuserr     = 8,
  preciserr   = 9,
  impreciserr = 10,
  unstkerr    = 11,
  stkerr      = 12,
  lsperr      = 13,
  bfarvalid   = 15,
  undefinstr  = 16,
  invstate    = 17,
  invpc       = 18,
  nocp        = 19,
  stkof       = 20,
  unaligned   = 24,
  divbyzero   = 25,
};

/** @brief Bits of the Arm HardFault status register, HFSR. Each value is the bit position. */
enum class[[= reflex::py::rename{"HfsrBit"}]] hfsr_bit : std::uint8_t
{
  vecttbl  = 1,
  forced   = 30,
  debugevt = 31,
};
}
