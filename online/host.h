// Shared by ff2online.dll's parts.
#pragma once
#include <winsock2.h>
#include "../shared/online_api.h"

void host_log(const char *fmt, ...);
int host_guests(struct sockaddr_in *to);             // the connected guests' addresses (up to ONLINE_MAX_GUESTS): how many
// This PC's address on the adapter the internet is reached through.
int host_main_address(struct in_addr *out);
// The router's internet address (upnp.c, once it has it).
void host_set_public_address(const char *ip);
void host_send(const struct sockaddr_in *to, const void *data, int len);

void audio_start(void);                              // audio.c
void upnp_start(int port);                           // upnp.c
