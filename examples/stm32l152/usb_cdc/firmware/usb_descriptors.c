#include "usb_descriptors.h"

#include "tusb.h"

#include <stddef.h>
#include <string.h>

#define USB_VID 0xCafe
#define USB_PID (0x4000 | (1 << 0))
#define USB_BCD 0x0200

enum
{
  ITF_NUM_CDC = 0,
  ITF_NUM_CDC_DATA,
  ITF_NUM_TOTAL
};

enum
{
  STRID_LANGID = 0,
  STRID_MANUFACTURER,
  STRID_PRODUCT,
  STRID_SERIAL,
  STRID_CDC,
};

#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x82

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

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

uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

static char const* const strings[] = {
    [STRID_MANUFACTURER] = "TinyUSB",
    [STRID_PRODUCT]      = "TinyUSB Device",
    [STRID_SERIAL]       = "123456",
    [STRID_CDC]          = "TinyUSB CDC",
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
