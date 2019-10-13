#ifndef __UBUS_SERVER_H__
#define __UBUS_SERVER_H__

#include "ap_list.h"
#include "aircrack-ng/osdep/sta_list.h"
#include "ubus.h"

typedef struct ubus_server_context_st ubus_server_context_st;

struct ubus_server_context_st * ubus_server_initialise(
    struct ubus_context * const ubus_ctx,
    struct ap_list_head * const ap_list,
    struct sta_list_head * const sta_list);

void ubus_server_destroy(struct ubus_server_context_st * const server_context);

#endif /* __UBUS_SERVER_H__ */
