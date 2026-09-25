#include <stdarg.h>
#include <stdint.h>

typedef struct list_node
{
  struct list_node* next;
  int32_t           value;
} list_node_t;

typedef enum color
{
  color_red   = 0,
  color_green = 1,
  color_blue  = 2,
} color_t;

typedef color_t shade_t;
typedef shade_t hue_t;

struct flags
{
  uint32_t enabled : 1;
  uint32_t mode : 3;
  uint32_t reserved : 28;
};

union payload
{
  uint32_t as_u32;
  uint8_t  as_bytes[4];
};

typedef uint32_t matrix_t[3][4];

typedef uint32_t volatile* const reg_ptr_t;

struct point
{
  int32_t x;
  int32_t y;
};

__attribute__((noinline)) struct point make_point(int32_t x, int32_t y)
{
  struct point p;
  p.x = x;
  p.y = y;
  return p;
}

__attribute__((noinline)) int32_t sum_point(struct point p)
{
  return p.x + p.y;
}

__attribute__((noinline)) int32_t sum_variadic(int32_t count, ...)
{
  va_list ap;
  va_start(ap, count);
  int32_t total = 0;
  for(int32_t i = 0; i < count; ++i)
  {
    total += va_arg(ap, int32_t);
  }
  va_end(ap);
  return total;
}

list_node_t node_b = {.next = 0, .value = 2};
list_node_t node_a = {.next = &node_b, .value = 1};

matrix_t global_matrix = {
    {1, 2,  3,  4 },
    {5, 6,  7,  8 },
    {9, 10, 11, 12}
};

struct flags global_flags = {.enabled = 1, .mode = 3, .reserved = 0};

union payload global_payload = {.as_u32 = 0x1122'3344};

hue_t global_color = color_green;

reg_ptr_t status_reg = (reg_ptr_t)0x4000'0000;

void const* volatile status_reg_addr = (void const*)&status_reg;

struct wrapped
{
  uint32_t tag;
  union
  {
    uint32_t as_u32;
    uint8_t  as_bytes[4];
  };
};

struct wrapped global_wrapped = {.tag = 1, .as_u32 = 0x5566'7788};

struct flex_array
{
  int32_t count;
  uint8_t data[];
};

static uint8_t flex_storage[16] = {4, 0, 0, 0, 1, 2, 3, 4};

__attribute__((noinline)) uint32_t flex_len(struct flex_array const* fa)
{
  return (uint32_t)fa->count;
}

static inline uint32_t triple_value(uint32_t x)
{
  return x * 3;
}

uint32_t (*volatile triple_value_ptr)(uint32_t) = triple_value;

__attribute__((noinline)) uint32_t call_triple(uint32_t x)
{
  return triple_value(x) + (*triple_value_ptr)(x + 1);
}

static uint32_t shared_counter = 11;

uint32_t types_touch_shared(void)
{
  return shared_counter;
}

uint32_t types_touch(void)
{
  struct point   p   = make_point(3, 4);
  int32_t        s   = sum_point(p);
  int32_t        v   = sum_variadic(3, 1, 2, 3);
  uint32_t const reg = *status_reg;
  uint32_t       t   = call_triple(2);
  uint32_t       fl  = flex_len((struct flex_array const*)flex_storage);
  return (uint32_t)(s + v)
       + reg
       + global_flags.mode
       + global_payload.as_u32
       + global_matrix[1][2]
       + (uint32_t)global_color
       + (uint32_t)node_a.value
       + (uint32_t)node_b.value
       + (uint32_t)(uintptr_t)status_reg_addr
       + t
       + fl
       + global_wrapped.tag
       + global_wrapped.as_u32
       + types_touch_shared();
}
