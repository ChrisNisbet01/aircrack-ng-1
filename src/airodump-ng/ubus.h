#ifndef __UBUS_H__
#define __UBUS_H__

#include "ap_list.h"
#include "aircrack-ng/osdep/sta_list.h"

#include <libubus.h>

struct ubus_state_st;

struct ubus_state_st * ubus_initialise(
    char const * const path,
    struct ap_list_head * const ap_list,
    struct sta_list_head * const sta_list);

void ubus_done(struct ubus_state_st * const state);

void
ubus_state_send_blob_event(
    struct ubus_state_st * const state,
    char const * const event_name,
    struct blob_buf * const b);

#endif /* __UBUS_H__ */
