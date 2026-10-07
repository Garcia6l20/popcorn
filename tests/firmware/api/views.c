#include <stdint.h>

extern uint32_t _estack;
extern uint32_t _sidata[];
extern uint32_t _sdata[];
extern uint32_t _edata[];
extern uint32_t _sbss[];
extern uint32_t _ebss[];

typedef enum
{
  ROUTE_NONE = 0,
  ROUTE_UART = 3,
  ROUTE_CAN  = 7,
} route_kind;

struct route
{
  uint16_t   source;
  uint16_t   target;
  route_kind kind;
  float      gain;
};

typedef struct
{
  uint32_t      count;
  struct route  entries[5];
  struct route* current;
  uint8_t       flags : 3;
  uint8_t       mode : 5;
  int8_t        delta : 4;
  int16_t       offset;
  double        scale;
} route_table;

route_table table = {
    .count = 5,
    .entries =
        {
                  {1, 10, ROUTE_UART, 0.5f},
                  {2, 20, ROUTE_CAN, 1.5f},
                  {3, 30, ROUTE_NONE, 2.5f},
                  {4, 40, ROUTE_UART, 3.5f},
                  {5, 50, ROUTE_CAN, 4.5f},
                  },
    .current = &table.entries[3],
    .flags   = 5,
    .mode    = 17,
    .delta   = -3,
    .offset  = -300,
    .scale   = 2.25,
};

const char banner[] = "popcorn views";

typedef enum
{
  LEVEL_LOW  = -2,
  LEVEL_ZERO = 0,
  LEVEL_HIGH = 2,
} level;

struct extras
{
  char  letter;
  level low;
};

struct extras extras = {.letter = (char)0xE9, .low = LEVEL_LOW};

int32_t matrix[2][3] = {
    {1, 2, 3},
    {4, 5, 6}
};
volatile uint32_t counter;

static const uint16_t gains[4] = {100, 250, 400, 1000};

struct wide
{
  uint32_t low : 6;
  uint32_t mid : 12;
  int32_t  high : 14;
};

struct wide wide = {.low = 9, .mid = 0xABC, .high = -5};

struct [[gnu::packed]] odd
{
  uint8_t  pad : 4;
  uint64_t big : 62;
};

struct odd odd = {.pad = 1, .big = 7};

bool armed = true;

typedef union
{
  route_kind value;
} wrapped_kind;

struct shadowed
{
  uint32_t     size;
  uint16_t     address;
  wrapped_kind kind;
};

struct shadowed shadowed = {.size = 7, .address = 0x1234, .kind = {.value = ROUTE_CAN}};

[[noreturn]] void Reset_Handler(void)
{
  for(uint32_t *from = _sidata, *to = _sdata; to < _edata;)
  {
    *to++ = *from++;
  }
  for(uint32_t* to = _sbss; to < _ebss;)
  {
    *to++ = 0;
  }
  __asm__ volatile("bkpt #0" ::: "memory");
  for(;;)
  {
    uint32_t const now = counter;
    counter            = now + (uint32_t)banner[0] + gains[now & 3U];
  }
}

__attribute__((section(".isr_vector"), used)) const void* const vectors[2] = {
    &_estack,
    (const void*)Reset_Handler,
};
