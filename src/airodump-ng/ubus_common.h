#ifndef __UBUS_COMMON_H__
#define __UBUS_COMMON_H__

#include "ap_list.h"
#include "aircrack-ng/osdep/sta_list.h"

#include <libubox/blob.h>

void append_ap_nodes_to_blob(
    struct ap_list_head const * const ap_list,
    struct blob_buf * const b);

void append_sta_nodes_to_blob(
    struct sta_list_head const * const sta_list,
    struct blob_buf * const b);

void ubus_blob_buf_init(struct blob_buf * const b);

#endif /* __UBUS_COMMON_H__ */
