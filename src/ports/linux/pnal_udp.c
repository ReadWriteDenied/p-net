/*********************************************************************
 *        _       _         _
 *  _ __ | |_  _ | |  __ _ | |__   ___
 * | '__|| __|(_)| | / _` || '_ \ / __|
 * | |   | |_  _ | || (_| || |_) |\__ \
 * |_|    \__|(_)|_| \__,_||_.__/ |___/
 *
 * www.rt-labs.com
 * Copyright 2018 rt-labs AB, Sweden.
 *
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 ********************************************************************/

#include "pnal.h"
#include "pf_includes.h"

#include <string.h>
#include <sys/select.h>
#include <sys/eventfd.h>
#include <unistd.h>

typedef struct pnal_udp_handle
{
   pnal_udp_callback_t * callback;
   void * arg;
   int socket;
} pnal_udp_handle_t;

#define MAX_HANDLES 10
static pnal_udp_handle_t handles[MAX_HANDLES];
static os_thread_t * thread = NULL;
static int event_fd = -1;
static bool is_initialized = false;

static pnal_udp_handle_t * get_handle(int id)
{
  for (size_t i = 0; i < MAX_HANDLES; ++i) {
    if (handles[i].socket == id) {
      return &handles[i];
    }
  }
  return NULL;
}

static void os_udp_task (void * thread_arg)
{
   while (1) {
      fd_set rfds;
      FD_ZERO(&rfds);
      size_t nfds = 1;
      FD_SET(event_fd, &rfds);
      for (size_t i = 0; i < MAX_HANDLES; ++i) {
         int fd = handles[i].socket;
         if (fd < 0) {
           continue;
         }
         FD_SET(fd, &rfds);
         ++nfds;
       }

       while (select(nfds, &rfds, NULL, NULL, NULL))
       {
           for (size_t i = 0; i < MAX_HANDLES; ++i) {
              pnal_udp_handle_t * handle = &handles[i];
              int fd = handle->socket;
              if (fd < 0) {
                 continue;
              }
              if (FD_ISSET(fd, &rfds) && handle->callback) {
                 handle->callback(fd, handle->arg);
              }
           }
       }
   }
}

static void initialize(const pnal_cfg_t * pnal_cfg)
{
   for (size_t i = 0; i < MAX_HANDLES; ++i) {
      handles[i].socket = -1;
   }
   event_fd = eventfd(0, EFD_NONBLOCK);
   thread = os_thread_create (
       "os_udp_task",
       pnal_cfg->udp_recv_thread.prio,
       pnal_cfg->udp_recv_thread.stack_size,
       os_udp_task,
       NULL);
   is_initialized = true;
}

int pnal_udp_open (
   pnal_ipaddr_t addr,
   pnal_ipport_t port,
   const pnal_cfg_t * pnal_cfg,
   pnal_udp_callback_t * callback,
   void * arg)
{
   struct sockaddr_in local;
   int id;
   const int enable = 1;

   if (!is_initialized) {
      initialize(pnal_cfg);
   }

   // Try to get a free handle
   pnal_udp_handle_t * handle = get_handle(-1);
   if (handle == NULL)
   {
      return -1;
   }

   id = socket (PF_INET, SOCK_DGRAM, IPPROTO_UDP);
   if (id == -1)
   {
      goto error;
   }

   if (setsockopt(id, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) != 0)
   {
      goto error;
   }

   /* set IP and port number */
   local = (struct sockaddr_in){
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl (addr),
      .sin_port = htons (port),
      .sin_zero = {0},
   };

   if (bind (id, (struct sockaddr *)&local, sizeof (local)) != 0)
   {
      goto error;
   }

   handle->arg = arg;
   handle->callback = callback;
   handle->socket = id;

   // Update monitored fd's
   uint64_t u = 1;
   write(event_fd, &u, sizeof(u));

   return id;

error:
   if (id > -1)
      close (id);
   handle->socket = -1;
   return -1;
}

int pnal_udp_sendto (
   uint32_t id,
   pnal_ipaddr_t dst_addr,
   pnal_ipport_t dst_port,
   const uint8_t * data,
   int size)
{
   struct sockaddr_in remote;
   int len;

   remote = (struct sockaddr_in){
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl (dst_addr),
      .sin_port = htons (dst_port),
      .sin_zero = {0},
   };
   len =
      sendto (id, data, size, 0, (struct sockaddr *)&remote, sizeof (remote));

   return len;
}

int pnal_udp_recvfrom (
   uint32_t id,
   pnal_ipaddr_t * src_addr,
   pnal_ipport_t * src_port,
   uint8_t * data,
   int size)
{
   struct sockaddr_in remote;
   socklen_t addr_len = sizeof (remote);
   int len;

   memset (&remote, 0, sizeof (remote));
   len = recvfrom (
      id,
      data,
      size,
      MSG_DONTWAIT,
      (struct sockaddr *)&remote,
      &addr_len);
   if (len > 0)
   {
      *src_addr = ntohl (remote.sin_addr.s_addr);
      *src_port = ntohs (remote.sin_port);
   }

   return len;
}

void pnal_udp_close (uint32_t id)
{
   pnal_udp_handle_t * handle = get_handle(id);
   if (handle) {
      close (id);
      handle->socket = -1;
   }
}
