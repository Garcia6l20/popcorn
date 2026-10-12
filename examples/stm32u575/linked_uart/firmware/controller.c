#include "link.h"

#include <stdio.h>
#include <string.h>

#define RESPONDERS      2u
#define BLOCK_PERIOD_MS 16u
#define POLL_PERIOD_MS  100u
#define LOST_AFTER_MS   3000u
#define REPOLL_MS       1500u

struct responder
{
  struct link link;
  uint8_t     id;
  bool        assigned;
  bool        lost;
  uint32_t    last_answer;
  uint32_t    next_block;
  uint32_t    next_poll;
  uint32_t    next_assign;
  uint8_t     block_seq;
  uint8_t     poll_seq;
  uint32_t    sent_sum[256];
  uint32_t    blocks_sent;
  uint32_t    sums_ok;
  uint32_t    sums_bad;
  uint32_t    last_bad_seq;
  uint32_t    polls_answered;
  uint32_t    times_lost;
};

struct responder responders[RESPONDERS];

static uint8_t answer[MESSAGE_BYTES];

void responder_lost(uint8_t id);
void responder_found(uint8_t id);

__attribute__((noipa)) void responder_lost(uint8_t id)
{
  (void)id;
}

__attribute__((noipa)) void responder_found(uint8_t id)
{
  (void)id;
}

static bool due(uint32_t now, uint32_t at)
{
  return (int32_t)(now - at) >= 0;
}

static void send_node_id(struct responder* r)
{
  r->link.tx[0] = MSG_NODE_ID;
  r->link.tx[1] = r->id;
  link_send(&r->link, NODE_ID_BYTES);
}

static void send_block(struct responder* r)
{
  uint8_t const seq = r->block_seq++;
  r->link.tx[0]     = MSG_BLOCK;
  r->link.tx[1]     = seq;
  for(uint16_t index = 2; index < MESSAGE_BYTES; ++index)
  {
    r->link.tx[index] = (uint8_t)(seq + index);
  }
  r->sent_sum[seq] = block_checksum(r->link.tx, MESSAGE_BYTES);
  ++r->blocks_sent;
  link_send(&r->link, MESSAGE_BYTES);
}

static void send_poll(struct responder* r)
{
  r->link.tx[0] = MSG_POLL;
  r->link.tx[1] = r->poll_seq++;
  r->link.tx[2] = 0;
  r->link.tx[3] = 0;
  link_send(&r->link, POLL_BYTES);
}

static uint32_t read_u32(uint8_t const* bytes)
{
  return (uint32_t)bytes[0]
       | (uint32_t)bytes[1] << 8
       | (uint32_t)bytes[2] << 16
       | (uint32_t)bytes[3] << 24;
}

static void found(struct responder* r, uint32_t now)
{
  responder_found(r->id);
  if(r->lost)
  {
    printf("controller: responder %u back\r\n", r->id);
  }
  else
  {
    printf("controller: responder %u assigned\r\n", r->id);
  }
  r->assigned   = true;
  r->lost       = false;
  r->next_block = now;
  r->next_poll  = now + POLL_PERIOD_MS;
}

static void handle(struct responder* r, uint16_t size, uint32_t now)
{
  if(size < 2 || answer[1] != r->id)
  {
    return;
  }
  if(answer[0] == MSG_NODE_ACK && size == NODE_ID_BYTES)
  {
    r->last_answer = now;
    if(!r->assigned || r->lost)
    {
      found(r, now);
    }
  }
  else if(answer[0] == MSG_BLOCK_SUM && size == MESSAGE_BYTES && r->assigned && !r->lost)
  {
    r->last_answer = now;
    if(read_u32(&answer[3]) == r->sent_sum[answer[2]])
    {
      ++r->sums_ok;
    }
    else
    {
      ++r->sums_bad;
      r->last_bad_seq = answer[2];
    }
  }
  else if(answer[0] == MSG_POLL_REPLY && size == POLL_BYTES && r->assigned && !r->lost)
  {
    r->last_answer = now;
    ++r->polls_answered;
  }
}

static void serve(struct responder* r, uint32_t now)
{
  uint16_t size;
  while((size = link_take(&r->link, answer)) != 0)
  {
    handle(r, size, now);
  }
  if(r->assigned && !r->lost && now - r->last_answer >= LOST_AFTER_MS)
  {
    r->lost        = true;
    r->next_assign = now;
    ++r->times_lost;
    responder_lost(r->id);
    printf("controller: responder %u lost\r\n", r->id);
  }
  if(!link_can_send(&r->link))
  {
    return;
  }
  if(!r->assigned || r->lost)
  {
    if(due(now, r->next_assign))
    {
      send_node_id(r);
      r->next_assign = now + REPOLL_MS;
    }
    return;
  }
  if(due(now, r->next_block))
  {
    send_block(r);
    r->next_block += BLOCK_PERIOD_MS;
    if(due(now, r->next_block))
    {
      r->next_block = now + BLOCK_PERIOD_MS;
    }
  }
  else if(due(now, r->next_poll))
  {
    send_poll(r);
    r->next_poll += POLL_PERIOD_MS;
  }
}

static bool work(void)
{
  return link_has_message(&responders[0].link) || link_has_message(&responders[1].link);
}

int main(void)
{
  board_init();
  responders[0].id = 1;
  responders[1].id = 2;
  link_init(&responders[0].link, UART4);
  link_init(&responders[1].link, USART3);
  printf("controller: up%s\r\n", reset_by_watchdog ? ", watchdog reset" : "");
  links_ready();
  for(;;)
  {
    uint32_t const now = HAL_GetTick();
    for(uint32_t index = 0; index < RESPONDERS; ++index)
    {
      serve(&responders[index], now);
    }
    idle(work);
  }
}
