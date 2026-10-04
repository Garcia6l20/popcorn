#ifndef USB_DESCRIPTORS_H
#define USB_DESCRIPTORS_H

#include "tusb.h"

#include <stdint.h>

#define REPORT_ID_BUTTON    1
#define REPORT_ID_LED       2
#define HID_BUTTON          0
#define HID_IO              1
#define HID_IO_REPORT_SIZE  8
#define HID_IO_FEATURE_SIZE 4

extern tusb_desc_device_t const desc_device;
extern uint8_t const            desc_configuration[];
extern uint8_t const            desc_hid_report[];
extern uint8_t const            desc_hid_io_report[];

#endif
