#pragma once

#include <stdint.h>

#define REG32(address) (*(volatile uint32_t*)(address))
#define REG8(address)  (*(volatile uint8_t*)(address))
#define BOOTS          REG32(0x20080000u)

extern uint32_t log_buf[256];
extern uint32_t log_len;

#define LOG(value) (log_buf[log_len++ & 0xffu] = (uint32_t)(value))
