#include <stdint.h>

/** @brief The header a bootloader reads in front of the application's vector table. */
struct app_header
{
  uint32_t magic;
  uint32_t vector_table_offset;
};

__attribute__((section(".app_header"), used)) static struct app_header const app_header = {
    .magic               = 0x41505031u,
    .vector_table_offset = sizeof(struct app_header),
};
