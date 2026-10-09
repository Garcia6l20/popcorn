#include "consts.h"

#define CONSTS_MODE 2

static volatile enum
{
  MODE_FAST = 2
} mode_b;

typedef enum
{
  SHAPE_SQUARE = 1,
  SHAPE_OVAL,
} shape_t;

static volatile shape_t shape_b;

enum opaque;
enum hidden;

volatile enum opaque* volatile opaque_ptr;
volatile enum hidden* volatile hidden_ptr;

void touch_b(void)
{
  mode_b     = MODE_FAST;
  shape_b    = SHAPE_OVAL;
  opaque_ptr = nullptr;
  hidden_ptr = nullptr;
  link       = LINK_UART;
}
