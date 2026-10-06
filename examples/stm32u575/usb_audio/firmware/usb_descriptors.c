#include "usb_descriptors.h"

#include "tusb.h"

#include <stddef.h>
#include <string.h>

#define USB_VID 0xCafe
#define USB_PID (0x4000 | (1 << 4))
#define USB_BCD 0x0200

enum
{
  STRID_LANGID = 0,
  STRID_MANUFACTURER,
  STRID_PRODUCT,
  STRID_SERIAL,
  STRID_AUDIO,
};

#define EPNUM_AUDIO_OUT 0x01
#define EPNUM_AUDIO_IN  0x81

#define FEATURE_CONTROLS                                 \
  (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS \
   | AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS)

#define CLOCK_CONTROLS                                     \
  (AUDIO20_CTRL_R << AUDIO20_CLOCK_SOURCE_CTRL_CLK_FRQ_POS \
   | AUDIO20_CTRL_R << AUDIO20_CLOCK_SOURCE_CTRL_CLK_VAL_POS)

#define AC_UNITS_LEN                          \
  (TUD_AUDIO20_DESC_CLK_SRC_LEN               \
   + 2 * TUD_AUDIO20_DESC_INPUT_TERM_LEN      \
   + 2 * TUD_AUDIO20_DESC_FEATURE_UNIT_LEN(1) \
   + 2 * TUD_AUDIO20_DESC_OUTPUT_TERM_LEN)

#define AS_STREAMING_LEN                \
  (2 * TUD_AUDIO20_DESC_STD_AS_LEN      \
   + TUD_AUDIO20_DESC_CS_AS_INT_LEN     \
   + TUD_AUDIO20_DESC_TYPE_I_FORMAT_LEN \
   + TUD_AUDIO20_DESC_STD_AS_ISO_EP_LEN \
   + TUD_AUDIO20_DESC_CS_AS_ISO_EP_LEN)

#define AUDIO_FUNCTION_LEN       \
  (TUD_AUDIO20_DESC_IAD_LEN      \
   + TUD_AUDIO20_DESC_STD_AC_LEN \
   + TUD_AUDIO20_DESC_CS_AC_LEN  \
   + AC_UNITS_LEN                \
   + 2 * AS_STREAMING_LEN)

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + AUDIO_FUNCTION_LEN)

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
    TUD_AUDIO20_DESC_IAD(ITF_NUM_AUDIO_CONTROL, ITF_NUM_TOTAL, 0x00),
    TUD_AUDIO20_DESC_STD_AC(ITF_NUM_AUDIO_CONTROL, 0x00, STRID_AUDIO),
    TUD_AUDIO20_DESC_CS_AC(0x0200, AUDIO20_FUNC_HEADSET, AC_UNITS_LEN, 0x00),
    TUD_AUDIO20_DESC_CLK_SRC(
        ENTITY_CLOCK,
        AUDIO20_CLOCK_SOURCE_ATT_INT_FIX_CLK,
        CLOCK_CONTROLS,
        0x00,
        0x00),
    TUD_AUDIO20_DESC_INPUT_TERM(
        ENTITY_SPEAKER_INPUT,
        AUDIO_TERM_TYPE_USB_STREAMING,
        0x00,
        ENTITY_CLOCK,
        AUDIO_CHANNELS,
        AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED,
        0x00,
        0x0000,
        0x00),
    TUD_AUDIO20_DESC_FEATURE_UNIT(
        ENTITY_SPEAKER_FEATURE,
        ENTITY_SPEAKER_INPUT,
        0x00,
        FEATURE_CONTROLS,
        FEATURE_CONTROLS),
    TUD_AUDIO20_DESC_OUTPUT_TERM(
        ENTITY_SPEAKER_OUTPUT,
        AUDIO_TERM_TYPE_OUT_GENERIC_SPEAKER,
        0x00,
        ENTITY_SPEAKER_FEATURE,
        ENTITY_CLOCK,
        0x0000,
        0x00),
    TUD_AUDIO20_DESC_INPUT_TERM(
        ENTITY_MIC_INPUT,
        AUDIO_TERM_TYPE_IN_GENERIC_MIC,
        0x00,
        ENTITY_CLOCK,
        AUDIO_CHANNELS,
        AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED,
        0x00,
        0x0000,
        0x00),
    TUD_AUDIO20_DESC_FEATURE_UNIT(
        ENTITY_MIC_FEATURE,
        ENTITY_MIC_INPUT,
        0x00,
        FEATURE_CONTROLS,
        FEATURE_CONTROLS),
    TUD_AUDIO20_DESC_OUTPUT_TERM(
        ENTITY_MIC_OUTPUT,
        AUDIO_TERM_TYPE_USB_STREAMING,
        0x00,
        ENTITY_MIC_FEATURE,
        ENTITY_CLOCK,
        0x0000,
        0x00),
    TUD_AUDIO20_DESC_STD_AS_INT(ITF_NUM_AUDIO_SPEAKER, 0x00, 0x00, 0x00),
    TUD_AUDIO20_DESC_STD_AS_INT(ITF_NUM_AUDIO_SPEAKER, 0x01, 0x01, 0x00),
    TUD_AUDIO20_DESC_CS_AS_INT(
        ENTITY_SPEAKER_INPUT,
        AUDIO20_CTRL_NONE,
        AUDIO20_FORMAT_TYPE_I,
        AUDIO20_DATA_FORMAT_TYPE_I_PCM,
        AUDIO_CHANNELS,
        AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED,
        0x00),
    TUD_AUDIO20_DESC_TYPE_I_FORMAT(AUDIO_BYTES_PER_SAMPLE, AUDIO_BITS_PER_SAMPLE),
    TUD_AUDIO20_DESC_STD_AS_ISO_EP(
        EPNUM_AUDIO_OUT,
        (uint8_t)((uint8_t)TUSB_XFER_ISOCHRONOUS
                  | (uint8_t)TUSB_ISO_EP_ATT_ADAPTIVE
                  | (uint8_t)TUSB_ISO_EP_ATT_DATA),
        AUDIO_EP_SIZE,
        0x01),
    TUD_AUDIO20_DESC_CS_AS_ISO_EP(
        AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK,
        AUDIO20_CTRL_NONE,
        AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED,
        0x0000),
    TUD_AUDIO20_DESC_STD_AS_INT(ITF_NUM_AUDIO_MIC, 0x00, 0x00, 0x00),
    TUD_AUDIO20_DESC_STD_AS_INT(ITF_NUM_AUDIO_MIC, 0x01, 0x01, 0x00),
    TUD_AUDIO20_DESC_CS_AS_INT(
        ENTITY_MIC_OUTPUT,
        AUDIO20_CTRL_NONE,
        AUDIO20_FORMAT_TYPE_I,
        AUDIO20_DATA_FORMAT_TYPE_I_PCM,
        AUDIO_CHANNELS,
        AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED,
        0x00),
    TUD_AUDIO20_DESC_TYPE_I_FORMAT(AUDIO_BYTES_PER_SAMPLE, AUDIO_BITS_PER_SAMPLE),
    TUD_AUDIO20_DESC_STD_AS_ISO_EP(
        EPNUM_AUDIO_IN,
        (uint8_t)((uint8_t)TUSB_XFER_ISOCHRONOUS
                  | (uint8_t)TUSB_ISO_EP_ATT_ASYNCHRONOUS
                  | (uint8_t)TUSB_ISO_EP_ATT_DATA),
        AUDIO_EP_SIZE,
        0x01),
    TUD_AUDIO20_DESC_CS_AS_ISO_EP(
        AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK,
        AUDIO20_CTRL_NONE,
        AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED,
        0x0000),
};

_Static_assert(sizeof desc_configuration == CONFIG_TOTAL_LEN, "configuration length");

static char const* const strings[] = {
    [STRID_MANUFACTURER] = "TinyUSB",
    [STRID_PRODUCT]      = "TinyUSB Headset",
    [STRID_SERIAL]       = "123456",
    [STRID_AUDIO]        = "TinyUSB UAC2 Headset",
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
