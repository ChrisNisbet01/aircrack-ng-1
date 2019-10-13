#include "ubus_event.h"

static char const event_id[] = "wifi_scanner.nodes";

void ubus_send_nodes_event(
    struct ubus_state_st * state,
    struct ap_list_head const * const ap_list,
    struct sta_list_head const * const sta_list)
{
    struct blob_buf b;

    ubus_blob_buf_init(&b);

    append_ap_nodes_to_blob(ap_list, &b);
    append_sta_nodes_to_blob(sta_list, &b);

    ubus_state_send_blob_event(state, event_id, &b);

    blob_buf_free(&b);
}

