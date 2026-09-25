#pragma once

typedef enum
{
  Reset_IRQn                = -15,    /*!< Reset Vector, equals the power-up value             */
  SysTick_IRQn               = -1,    /*!< System Tick Timer                                   */

  WWDG_IRQn                  =  0,    /*!< Window WatchDog interrupt                           */
  PVD_IRQn                   =  1,    /*!< PVD through EXTI line, priority = highest            */
  FOO_IRQn                   = 0x2,   /*!< FOO global interrupt, hex value                     */
  BAR_IRQn                   =  5,    /*!< BAR global interrupt, leaves a gap at 3 and 4        */
} IRQn_Type;
