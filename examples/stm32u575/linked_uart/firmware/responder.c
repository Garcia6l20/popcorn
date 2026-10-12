#include "link.h"

#include <stdio.h>
#include <string.h>

#define CONTROLLER_LOST_MS 5000u

struct link uplink;
uint8_t     node_id;
uint32_t    blocks_answered;
uint32_t    polls_answered;
uint32_t    last_sum;
uint32_t    last_rx;
bool        controller_lost;

static uint8_t  message[MESSAGE_BYTES];
static bool     block_pending;
static uint8_t  block_seq;
static uint8_t  short_reply[POLL_BYTES];
static uint16_t short_size;

static void receive(uint16_t size)
{
  if(message[0] == MSG_NODE_ID && size == NODE_ID_BYTES)
  {
    node_id        = message[1];
    short_reply[0] = MSG_NODE_ACK;
    short_reply[1] = node_id;
    short_size     = NODE_ID_BYTES;
    printf("responder: node id %u\r\n", node_id);
  }
  else if(node_id == 0)
  {
    return;
  }
  else if(message[0] == MSG_BLOCK && size == MESSAGE_BYTES)
  {
    last_sum      = block_checksum(message, MESSAGE_BYTES);
    block_seq     = message[1];
    block_pending = true;
  }
  else if(message[0] == MSG_POLL && size == POLL_BYTES)
  {
    short_reply[0] = MSG_POLL_REPLY;
    short_reply[1] = node_id;
    short_reply[2] = message[1];
    short_reply[3] = 0;
    short_size     = POLL_BYTES;
  }
}

static void send_short(void)
{
  memcpy(uplink.tx, short_reply, short_size);
  if(short_reply[0] == MSG_POLL_REPLY)
  {
    ++polls_answered;
  }
  link_send(&uplink, short_size);
  short_size = 0;
}

static void send_block_sum(void)
{
  uplink.tx[0] = MSG_BLOCK_SUM;
  uplink.tx[1] = node_id;
  uplink.tx[2] = block_seq;
  uplink.tx[3] = (uint8_t)last_sum;
  uplink.tx[4] = (uint8_t)(last_sum >> 8);
  uplink.tx[5] = (uint8_t)(last_sum >> 16);
  uplink.tx[6] = (uint8_t)(last_sum >> 24);
  memset(&uplink.tx[7], node_id, MESSAGE_BYTES - 7u);
  block_pending = false;
  ++blocks_answered;
  link_send(&uplink, MESSAGE_BYTES);
}

static bool work(void)
{
  return link_has_message(&uplink);
}

int main(void)
{
  board_init();
  link_init(&uplink, USART3);
  printf("responder: up%s\r\n", reset_by_watchdog ? ", watchdog reset" : "");
  last_rx = HAL_GetTick();
  links_ready();
  for(;;)
  {
    uint32_t const now  = HAL_GetTick();
    uint16_t const size = link_take(&uplink, message);
    if(size != 0)
    {
      last_rx = now;
      if(controller_lost)
      {
        controller_lost = false;
        printf("responder: controller back\r\n");
      }
      receive(size);
    }
    else if(!controller_lost && now - last_rx >= CONTROLLER_LOST_MS)
    {
      controller_lost = true;
      printf("responder: controller lost\r\n");
    }
    if(link_can_send(&uplink))
    {
      if(short_size != 0)
      {
        send_short();
      }
      else if(block_pending)
      {
        send_block_sum();
      }
    }
    idle(work);
  }
}
