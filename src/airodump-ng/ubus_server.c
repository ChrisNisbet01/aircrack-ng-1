#include "ubus_server.h"
#include "ubus_common.h"

struct ubus_server_context_st
{
    struct ubus_object wifi_scanner_object;
    struct ubus_context * ubus_ctx;
    struct ap_list_head * ap_list;
    struct sta_list_head * sta_list;
};

static int ap_list_method(
    struct ubus_context * ctx,
    struct ubus_object * obj,
    struct ubus_request_data * req,
    const char * method,
    struct blob_attr * msg)
{
    ubus_server_context_st * const server_context =
        container_of(obj, ubus_server_context_st, wifi_scanner_object);
    struct blob_buf b;

    (void)method;
    (void)msg;

    ubus_blob_buf_init(&b);

    append_ap_nodes_to_blob(server_context->ap_list, &b);

    ubus_send_reply(ctx, req, b.head);

    return 0;
}

static int sta_list_method(
    struct ubus_context * ctx,
    struct ubus_object * obj,
    struct ubus_request_data * req,
    const char * method,
    struct blob_attr * msg)
{
    ubus_server_context_st * const server_context =
        container_of(obj, ubus_server_context_st, wifi_scanner_object);
    struct blob_buf b;

    (void)method;
    (void)msg;

    ubus_blob_buf_init(&b);

    append_sta_nodes_to_blob(server_context->sta_list, &b);

    ubus_send_reply(ctx, req, b.head);

    return 0;
}

static const struct ubus_method wifi_scanner_methods[] = {
    UBUS_METHOD_NOARG("ap_list", ap_list_method),
    UBUS_METHOD_NOARG("sta_list", sta_list_method),
};

static struct ubus_object_type wifi_scanner_object_type =
    UBUS_OBJECT_TYPE("wifi_scanner", wifi_scanner_methods);

static struct ubus_object wifi_scanner_object = {
    .name = "wifi_scanner",
    .type = &wifi_scanner_object_type,
    .methods = wifi_scanner_methods,
    .n_methods = ARRAY_SIZE(wifi_scanner_methods),
};

typedef struct ubus_server_context_st ubus_server_context_st;

struct ubus_server_context_st * ubus_server_initialise(
    struct ubus_context * const ubus_ctx,
    struct ap_list_head * const ap_list,
    struct sta_list_head * const sta_list)
{
    ubus_server_context_st * server_context =
        calloc(1, sizeof *server_context);
    bool success;

    if (server_context == NULL)
    {
        success = false;
        goto done;
    }

    server_context->ubus_ctx = ubus_ctx;
    server_context->wifi_scanner_object = wifi_scanner_object;
    server_context->ap_list = ap_list;
    server_context->sta_list = sta_list;

    int const ret =
        ubus_add_object(ubus_ctx, &server_context->wifi_scanner_object);
    if (ret)
    {
        fprintf(stderr, "Failed to add object: %s\n", ubus_strerror(ret));
        success = false;

        goto done;
    }

    success = true;

done:
    if (!success)
    {
        free(server_context);
        server_context = NULL;
    }

    return server_context;
}

void ubus_server_destroy(struct ubus_server_context_st * const server_context)
{
    if (server_context == NULL)
    {
        goto done;
    }

    ubus_remove_object(server_context->ubus_ctx, &server_context->wifi_scanner_object);

    free(server_context);

done:
    return;
}
