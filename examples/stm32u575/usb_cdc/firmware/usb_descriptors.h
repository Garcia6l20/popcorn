#ifndef USB_DESCRIPTORS_H
#define USB_DESCRIPTORS_H

#include "tusb.h"

#include <stdint.h>

#define REPORT_ID_BUTTON 1

extern tusb_desc_device_t const desc_device;
extern uint8_t const            desc_configuration[];
extern uint8_t const            desc_hid_report[];

#endif
