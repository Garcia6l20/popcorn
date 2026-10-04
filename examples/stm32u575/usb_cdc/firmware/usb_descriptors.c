#include "usb_descriptors.h"

#include "tusb.h"

#include <stddef.h>
#include <string.h>

#define USB_VID 0xCafe
#define USB_PID (0x4000 | (1 << 0) | (1 << 2))
#define USB_BCD 0x0200

enum
{
  ITF_NUM_CDC = 0,
  ITF_NUM_CDC_DATA,
  ITF_NUM_HID,
  ITF_NUM_HID_IO,
  ITF_NUM_TOTAL
};

enum
{
  STRID_LANGID = 0,
  STRID_MANUFACTURER,
  STRID_PRODUCT,
  STRID_SERIAL,
  STRID_CDC,
  STRID_HID,
  STRID_HID_IO,
};

#define EPNUM_CDC_NOTIF  0x81
#define EPNUM_CDC_OUT    0x02
#define EPNUM_CDC_IN     0x82
#define EPNUM_HID        0x83
#define EPNUM_HID_IO_OUT 0x04
#define EPNUM_HID_IO_IN  0x84

#define CONFIG_TOTAL_LEN \
  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 0x01,
};

uint8_t const desc_hid_report[] = {
    HID_USAGE_PAGE_N(HID_USAGE_PAGE_VENDOR, 2),
    HID_USAGE(0x01),
    HID_COLLECTION(HID_COLLECTION_APPLICATION),
    HID_REPORT_ID(REPORT_ID_BUTTON) HID_USAGE(0x02),
    HID_LOGICAL_MIN(0x00),
    HID_LOGICAL_MAX_N(0x00ff, 2),
    HID_REPORT_SIZE(8),
    HID_REPORT_COUNT(1),
    HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),
    HID_REPORT_ID(REPORT_ID_LED) HID_USAGE(0x03),
    HID_LOGICAL_MIN(0x00),
    HID_LOGICAL_MAX_N(0x00ff, 2),
    HID_REPORT_SIZE(8),
    HID_REPORT_COUNT(1),
    HID_OUTPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),
    HID_COLLECTION_END,
};

uint8_t const desc_hid_io_report[] = {
    HID_USAGE_PAGE_N(HID_USAGE_PAGE_VENDOR, 2),
    HID_USAGE(0x01),
    HID_COLLECTION(HID_COLLECTION_APPLICATION),
    HID_USAGE(0x02),
    HID_LOGICAL_MIN(0x00),
    HID_LOGICAL_MAX_N(0x00ff, 2),
    HID_REPORT_SIZE(8),
    HID_REPORT_COUNT(HID_IO_REPORT_SIZE),
    HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),
    HID_USAGE(0x03),
    HID_REPORT_COUNT(HID_IO_REPORT_SIZE),
    HID_OUTPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),
    HID_USAGE(0x04),
    HID_REPORT_COUNT(HID_IO_FEATURE_SIZE),
    HID_FEATURE(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),
    HID_COLLECTION_END,
};

uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    TUD_HID_DESCRIPTOR(
        ITF_NUM_HID,
        STRID_HID,
        HID_ITF_PROTOCOL_NONE,
        sizeof(desc_hid_report),
        EPNUM_HID,
        16,
        10),
    TUD_HID_INOUT_DESCRIPTOR(
        ITF_NUM_HID_IO,
        STRID_HID_IO,
        HID_ITF_PROTOCOL_NONE,
        sizeof(desc_hid_io_report),
        EPNUM_HID_IO_OUT,
        EPNUM_HID_IO_IN,
        16,
        2),
};

static char const* const strings[] = {
    [STRID_MANUFACTURER] = "TinyUSB", [STRID_PRODUCT] = "TinyUSB Device",
    [STRID_SERIAL] = "123456",        [STRID_CDC] = "TinyUSB CDC",
    [STRID_HID] = "TinyUSB HID",      [STRID_HID_IO] = "TinyUSB HID IO",
};

static uint16_t string_buffer[32 + 1];

uint8_t const* tud_descriptor_device_cb(void)
{
  return (uint8_t const*)&desc_device;
}

uint8_t const* tud_descriptor_configuration_cb(uint8_t index)
{
  (void)index;
  return desc_configuration;
}

uint8_t const* tud_hid_descriptor_report_cb(uint8_t instance)
{
  return instance == HID_IO ? desc_hid_io_report : desc_hid_report;
}

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
  (void)langid;
  size_t count = 0;
  if(index == STRID_LANGID)
  {
    string_buffer[1] = 0x0409;
    count            = 1;
  }
  else
  {
    if(index >= sizeof strings / sizeof strings[0] || strings[index] == NULL)
    {
      return NULL;
    }
    char const* const text = strings[index];
    count                  = strlen(text);
    if(count > 32)
    {
      count = 32;
    }
    for(size_t i = 0; i < count; ++i)
    {
      string_buffer[1 + i] = (uint16_t)text[i];
    }
  }
  string_buffer[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * count + 2));
  return string_buffer;
}
