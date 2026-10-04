#ifndef USB_DESCRIPTORS_H
#define USB_DESCRIPTORS_H

#include "tusb.h"

#include <stdint.h>

enum
{
  ITF_NUM_AUDIO_CONTROL = 0,
  ITF_NUM_AUDIO_SPEAKER,
  ITF_NUM_AUDIO_MIC,
  ITF_NUM_TOTAL
};

#define ENTITY_CLOCK           0x04
#define ENTITY_SPEAKER_INPUT   0x01
#define ENTITY_SPEAKER_FEATURE 0x02
#define ENTITY_SPEAKER_OUTPUT  0x03
#define ENTITY_MIC_INPUT       0x11
#define ENTITY_MIC_FEATURE     0x12
#define ENTITY_MIC_OUTPUT      0x13

extern tusb_desc_device_t const desc_device;
extern uint8_t const            desc_configuration[];

#endif
