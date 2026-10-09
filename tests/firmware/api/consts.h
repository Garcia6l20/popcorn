#pragma once

#include <stdint.h>

typedef enum
{
  CHANNEL_A,
  CHANNEL_B,
  CHANNEL_MAX,
} channel_t;

enum link
{
  LINK_NONE    = -1,
  LINK_UART    = 3,
  LINK_SPI     = 7,
  LINK_DEFAULT = 3,
};

typedef enum
{
  LINK_HIGH = 0x80000000u,
} link_high_t;

typedef enum link link_t;

typedef enum
{
  ROUTE_CAN = 9,
} route_alias_t;

enum unused
{
  UNUSED_A,
};

extern volatile channel_t     channel;
extern volatile link_t        link;
extern volatile link_high_t   link_high;
extern volatile route_alias_t route_alias;

void touch_b(void);
