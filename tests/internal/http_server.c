/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <fluent-bit/flb_config.h>
#include <fluent-bit/flb_lib.h>
#include <fluent-bit/flb_input.h>
#include <fluent-bit/flb_http_common.h>
#include <fluent-bit/flb_io.h>
#include <fluent-bit/flb_network.h>
#include <fluent-bit/flb_pthread.h>
#include <fluent-bit/flb_socket.h>
#include <fluent-bit/http_server/flb_http_server.h>
#include <fluent-bit/http_server/flb_http_server_config_map.h>

#include <string.h>

#include "flb_tests_internal.h"

#define TEST_HTTP_SERVER_HOST "127.0.0.1"
#define TEST_HTTP_SERVER_WORKERS 2

struct test_http_server_context {
    pthread_mutex_t lock;
    int init_calls;
    int exit_calls;
    int request_calls;
    int exit_thread_mismatches;
    int expected_idle_timeout;
    int idle_timeout_mismatches;
    struct flb_http_server *initialized_servers[TEST_HTTP_SERVER_WORKERS];
    pthread_t initialized_threads[TEST_HTTP_SERVER_WORKERS];
};


static void test_http_server_context_init(struct test_http_server_context *context)
{
    memset(context, 0, sizeof(struct test_http_server_context));
    pthread_mutex_init(&context->lock, NULL);
}

static void test_http_server_context_destroy(struct test_http_server_context *context)
{
    pthread_mutex_destroy(&context->lock);
}

static int test_http_server_worker_init(struct flb_http_server *server, void *data)
{
    struct test_http_server_context *context;

    context = data;

    pthread_mutex_lock(&context->lock);

    if (server->networking_setup == NULL ||
        server->networking_setup->io_timeout != context->expected_idle_timeout) {
        context->idle_timeout_mismatches++;
    }

    if (context->init_calls < TEST_HTTP_SERVER_WORKERS) {
        context->initialized_servers[context->init_calls] = server;
        context->initialized_threads[context->init_calls] = pthread_self();
    }

    context->init_calls++;
    pthread_mutex_unlock(&context->lock);

    return 0;
}

static int test_http_server_worker_exit(struct flb_http_server *server, void *data)
{
    int index;
    int matching_thread;
    struct test_http_server_context *context;

    context = data;
    matching_thread = FLB_FALSE;

    pthread_mutex_lock(&context->lock);

    for (index = 0; index < context->init_calls &&
                    index < TEST_HTTP_SERVER_WORKERS; index++) {
        if (context->initialized_servers[index] == server &&
            pthread_equal(context->initialized_threads[index], pthread_self())) {
            matching_thread = FLB_TRUE;
            break;
        }
    }

    if (matching_thread == FLB_FALSE) {
        context->exit_thread_mismatches++;
    }

    context->exit_calls++;
    pthread_mutex_unlock(&context->lock);

    return 0;
}

static int test_http_server_request_handler(struct flb_http_request *request,
                                            struct flb_http_response *response)
{
    struct test_http_server_context *context;
    struct flb_http_server_session *session;

    session = (struct flb_http_server_session *) request->stream->parent;
    context = session->parent->user_data;

    pthread_mutex_lock(&context->lock);
    context->request_calls++;
    pthread_mutex_unlock(&context->lock);

    flb_http_response_set_status(response, 200);
    flb_http_response_set_body(response,
                               (unsigned char *) "ok",
                               2);

    return flb_http_response_commit(response);
}

static int test_http_server_network_init(void)
{
#ifdef FLB_SYSTEM_WINDOWS
    WSADATA wsa_data;
    int ret;

    ret = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    TEST_CHECK(ret == 0);
    return ret;
#else
    return 0;
#endif
}

static void test_http_server_network_cleanup(void)
{
#ifdef FLB_SYSTEM_WINDOWS
    WSACleanup();
#endif
}

static int test_http_server_reserve_port(void)
{
    int ret;
    socklen_t address_length;
    flb_sockfd_t socket_fd;
    struct sockaddr_in address;

    socket_fd = flb_net_server("0", TEST_HTTP_SERVER_HOST,
                               FLB_NETWORK_DEFAULT_BACKLOG_SIZE, FLB_FALSE);
    if (socket_fd == FLB_INVALID_SOCKET) {
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address_length = sizeof(address);
    ret = getsockname(socket_fd, (struct sockaddr *) &address, &address_length);
    flb_socket_close(socket_fd);
    if (ret != 0) {
        return -1;
    }

    return ntohs(address.sin_port);
}

void test_http_server_options_defaults()
{
    struct flb_http_server_options options;
    struct flb_http_server_config config;

    flb_http_server_options_init(&options);
    flb_http_server_config_init(&config);

    TEST_CHECK(options.workers == 1);
    TEST_CHECK(options.use_caller_event_loop == FLB_TRUE);
    TEST_CHECK(options.reuse_port == FLB_FALSE);
    TEST_CHECK(options.idle_timeout == HTTP_SERVER_DEFAULT_IDLE_TIMEOUT);
    TEST_CHECK(options.buffer_max_size == HTTP_SERVER_MAXIMUM_BUFFER_SIZE);
    TEST_CHECK(options.max_connections == 0);
    TEST_CHECK(config.http2 == FLB_TRUE);
    TEST_CHECK(config.idle_timeout == HTTP_SERVER_DEFAULT_IDLE_TIMEOUT);
    TEST_CHECK(config.buffer_max_size == HTTP_SERVER_MAXIMUM_BUFFER_SIZE);
    TEST_CHECK(config.max_connections == 0);
    TEST_CHECK(flb_http_server_property_is_allowed("http_server.idle_timeout") == FLB_TRUE);
    TEST_CHECK(flb_http_server_property_is_allowed("idle_timeout") == FLB_FALSE);
}

void test_http_server_options_multi_worker_magic()
{
    struct flb_config *config;
    struct flb_net_setup net_setup;
    struct flb_http_server server;
    struct flb_http_server_options options;
    int ret;

    config = flb_config_init();
    if (!TEST_CHECK(config != NULL)) {
        return;
    }

    flb_net_setup_init(&net_setup);
    flb_http_server_options_init(&options);

    options.protocol_version = HTTP_PROTOCOL_VERSION_AUTODETECT;
    options.request_callback = test_http_server_request_handler;
    options.address = (char *) TEST_HTTP_SERVER_HOST;
    options.port = 10001;
    options.networking_flags = FLB_IO_TCP;
    options.networking_setup = &net_setup;
    options.system_context = config;
    options.workers = 2;
    options.max_connections = 7;

    ret = flb_http_server_init_with_options(&server, &options);
    TEST_CHECK(ret == 0);
    if (ret != 0) {
        flb_config_exit(config);
        return;
    }
    TEST_CHECK(server.workers == 2);
    TEST_CHECK(server.reuse_port == FLB_TRUE);
    TEST_CHECK(server.use_caller_event_loop == FLB_FALSE);
    TEST_CHECK(server.idle_timeout == HTTP_SERVER_DEFAULT_IDLE_TIMEOUT);
    TEST_CHECK(net_setup.share_port == FLB_TRUE);
    TEST_CHECK(server.max_connections == 7);

    flb_http_server_destroy(&server);
    flb_config_exit(config);
}

void test_http_server_managed_worker_contract()
{
    struct flb_config *config;
    struct flb_net_setup net_setup;
    struct flb_http_server server;
    struct flb_http_server_options options;
    struct test_http_server_context context;
    int ret;
    config = flb_config_init();
    if (!TEST_CHECK(config != NULL)) {
        return;
    }

    test_http_server_context_init(&context);

    flb_net_setup_init(&net_setup);
    flb_http_server_options_init(&options);

    options.protocol_version = HTTP_PROTOCOL_VERSION_AUTODETECT;
    options.request_callback = test_http_server_request_handler;
    options.user_data = &context;
    options.address = (char *) TEST_HTTP_SERVER_HOST;
    options.port = 10002;
    options.networking_flags = FLB_IO_TCP;
    options.networking_setup = &net_setup;
    options.system_context = config;
    options.workers = 2;
    options.max_connections = 3;
    options.cb_worker_init = test_http_server_worker_init;
    options.cb_worker_exit = test_http_server_worker_exit;

    ret = flb_http_server_init_with_options(&server, &options);
    TEST_CHECK(ret == 0);
    if (ret != 0) {
        test_http_server_context_destroy(&context);
        flb_config_exit(config);
        return;
    }
    TEST_CHECK(server.workers == 2);
    TEST_CHECK(server.use_caller_event_loop == FLB_FALSE);
    TEST_CHECK(server.reuse_port == FLB_TRUE);
    TEST_CHECK(server.idle_timeout == HTTP_SERVER_DEFAULT_IDLE_TIMEOUT);
    TEST_CHECK(server.max_connections == 3);
    TEST_CHECK(server.cb_worker_init == test_http_server_worker_init);
    TEST_CHECK(server.cb_worker_exit == test_http_server_worker_exit);
    TEST_CHECK(context.request_calls == 0);
    TEST_CHECK(context.init_calls == 0);
    TEST_CHECK(context.exit_calls == 0);

    flb_http_server_destroy(&server);
    test_http_server_context_destroy(&context);
    flb_config_exit(config);
}

void test_http_server_worker_exit_runs_on_worker_thread()
{
    int port;
    int ret;
    struct flb_config *config;
    struct flb_net_setup net_setup;
    struct flb_http_server server;
    struct flb_http_server_options options;
    struct test_http_server_context context;

    ret = test_http_server_network_init();
    if (ret != 0) {
        return;
    }

    config = flb_config_init();
    if (!TEST_CHECK(config != NULL)) {
        test_http_server_network_cleanup();
        return;
    }

    test_http_server_context_init(&context);
    context.expected_idle_timeout = HTTP_SERVER_DEFAULT_IDLE_TIMEOUT;

    port = test_http_server_reserve_port();
    if (!TEST_CHECK(port > 0)) {
        test_http_server_context_destroy(&context);
        flb_config_exit(config);
        test_http_server_network_cleanup();
        return;
    }

    flb_net_setup_init(&net_setup);
    flb_http_server_options_init(&options);

    options.protocol_version = HTTP_PROTOCOL_VERSION_AUTODETECT;
    options.request_callback = test_http_server_request_handler;
    options.user_data = &context;
    options.address = (char *) TEST_HTTP_SERVER_HOST;
    options.port = port;
    options.networking_flags = FLB_IO_TCP;
    options.networking_setup = &net_setup;
    options.system_context = config;
    options.workers = TEST_HTTP_SERVER_WORKERS;
    options.cb_worker_init = test_http_server_worker_init;
    options.cb_worker_exit = test_http_server_worker_exit;

    ret = flb_http_server_init_with_options(&server, &options);
    TEST_CHECK(ret == 0);
    if (ret == 0) {
        ret = flb_http_server_start(&server);
#if defined(SO_REUSEPORT) && !defined(FLB_SYSTEM_WINDOWS)
        TEST_CHECK(ret == 0);

        if (ret == 0) {
            TEST_CHECK(context.init_calls == TEST_HTTP_SERVER_WORKERS);

            flb_http_server_destroy(&server);

            TEST_CHECK(context.exit_calls == TEST_HTTP_SERVER_WORKERS);
            TEST_CHECK(context.exit_thread_mismatches == 0);
            TEST_CHECK(context.idle_timeout_mismatches == 0);
        }
        else {
            flb_http_server_destroy(&server);
        }
#else
        TEST_CHECK(ret != 0);
        flb_http_server_destroy(&server);
#endif
    }

    test_http_server_context_destroy(&context);
    flb_config_exit(config);
    test_http_server_network_cleanup();
}

void test_http_server_single_managed_worker_start()
{
    int port;
    int ret;
    struct flb_config *config;
    struct flb_net_setup net_setup;
    struct flb_http_server server;
    struct flb_http_server_options options;
    struct test_http_server_context context;

    ret = test_http_server_network_init();
    if (ret != 0) {
        return;
    }

    config = flb_config_init();
    if (!TEST_CHECK(config != NULL)) {
        test_http_server_network_cleanup();
        return;
    }

    test_http_server_context_init(&context);
    context.expected_idle_timeout = HTTP_SERVER_DEFAULT_IDLE_TIMEOUT;

    port = test_http_server_reserve_port();
    if (!TEST_CHECK(port > 0)) {
        test_http_server_context_destroy(&context);
        flb_config_exit(config);
        test_http_server_network_cleanup();
        return;
    }

    flb_net_setup_init(&net_setup);
    flb_http_server_options_init(&options);

    options.protocol_version = HTTP_PROTOCOL_VERSION_AUTODETECT;
    options.request_callback = test_http_server_request_handler;
    options.user_data = &context;
    options.address = (char *) TEST_HTTP_SERVER_HOST;
    options.port = port;
    options.networking_flags = FLB_IO_TCP;
    options.networking_setup = &net_setup;
    options.system_context = config;
    options.workers = 1;
    options.use_caller_event_loop = FLB_FALSE;
    options.cb_worker_init = test_http_server_worker_init;
    options.cb_worker_exit = test_http_server_worker_exit;

    ret = flb_http_server_init_with_options(&server, &options);
    TEST_CHECK(ret == 0);
    if (ret == 0) {
        TEST_CHECK(server.reuse_port == FLB_FALSE);

        ret = flb_http_server_start(&server);
        TEST_CHECK(ret == 0);
        if (ret == 0) {
            TEST_CHECK(context.init_calls == 1);
            TEST_CHECK(context.idle_timeout_mismatches == 0);
        }

        flb_http_server_destroy(&server);

        if (ret == 0) {
            TEST_CHECK(context.exit_calls == 1);
            TEST_CHECK(context.exit_thread_mismatches == 0);
        }
    }

    test_http_server_context_destroy(&context);
    flb_config_exit(config);
    test_http_server_network_cleanup();
}

void test_http_server_workers_reject_distinct_ephemeral_ports()
{
    int ret;
    struct flb_config *config;
    struct flb_net_setup net_setup;
    struct flb_http_server server;
    struct flb_http_server_options options;

    ret = test_http_server_network_init();
    if (ret != 0) {
        return;
    }

    config = flb_config_init();
    if (!TEST_CHECK(config != NULL)) {
        test_http_server_network_cleanup();
        return;
    }

    flb_net_setup_init(&net_setup);
    flb_http_server_options_init(&options);

    options.protocol_version = HTTP_PROTOCOL_VERSION_AUTODETECT;
    options.request_callback = test_http_server_request_handler;
    options.address = (char *) TEST_HTTP_SERVER_HOST;
    options.port = 0;
    options.networking_flags = FLB_IO_TCP;
    options.networking_setup = &net_setup;
    options.system_context = config;
    options.workers = 2;

    ret = flb_http_server_init_with_options(&server, &options);
    TEST_CHECK(ret == 0);
    if (ret == 0) {
        ret = flb_http_server_start(&server);
        TEST_CHECK(ret != 0);
        flb_http_server_destroy(&server);
    }

    flb_config_exit(config);
    test_http_server_network_cleanup();
}

void test_http_server_idle_timeout_applies_to_networking_setup()
{
    struct flb_config *config;
    struct flb_net_setup net_setup;
    struct flb_http_server server;
    struct flb_http_server_options options;
    int ret;

    config = flb_config_init();
    if (!TEST_CHECK(config != NULL)) {
        return;
    }

    flb_net_setup_init(&net_setup);
    flb_http_server_options_init(&options);

    options.protocol_version = HTTP_PROTOCOL_VERSION_AUTODETECT;
    options.request_callback = test_http_server_request_handler;
    options.address = (char *) TEST_HTTP_SERVER_HOST;
    options.port = 10003;
    options.networking_flags = FLB_IO_TCP;
    options.networking_setup = &net_setup;
    options.system_context = config;
    options.idle_timeout = 17;

    ret = flb_http_server_init_with_options(&server, &options);
    TEST_CHECK(ret == 0);
    if (ret != 0) {
        flb_config_exit(config);
        return;
    }

    TEST_CHECK(net_setup.io_timeout == 17);
    TEST_CHECK(server.idle_timeout == 17);

    flb_http_server_destroy(&server);
    flb_config_exit(config);
}

void test_http_server_explicit_network_timeout_is_preserved()
{
    struct flb_config *config;
    struct flb_net_setup net_setup;
    struct flb_http_server server;
    struct flb_http_server_options options;
    int ret;

    config = flb_config_init();
    if (!TEST_CHECK(config != NULL)) {
        return;
    }

    flb_net_setup_init(&net_setup);
    flb_http_server_options_init(&options);

    net_setup.io_timeout = 23;

    options.protocol_version = HTTP_PROTOCOL_VERSION_AUTODETECT;
    options.request_callback = test_http_server_request_handler;
    options.address = (char *) TEST_HTTP_SERVER_HOST;
    options.port = 10004;
    options.networking_flags = FLB_IO_TCP;
    options.networking_setup = &net_setup;
    options.system_context = config;
    options.idle_timeout = 17;

    ret = flb_http_server_init_with_options(&server, &options);
    TEST_CHECK(ret == 0);
    if (ret != 0) {
        flb_config_exit(config);
        return;
    }

    TEST_CHECK(net_setup.io_timeout == 23);
    TEST_CHECK(server.idle_timeout == 17);

    flb_http_server_destroy(&server);
    flb_config_exit(config);
}

void test_http_server_multi_worker_disabled_idle_timeout_is_preserved()
{
    int port;
    struct flb_config *config;
    struct flb_net_setup net_setup;
    struct flb_http_server server;
    struct flb_http_server_options options;
    struct test_http_server_context context;
    int ret;

    ret = test_http_server_network_init();
    if (ret != 0) {
        return;
    }

    config = flb_config_init();
    if (!TEST_CHECK(config != NULL)) {
        test_http_server_network_cleanup();
        return;
    }

    flb_net_setup_init(&net_setup);
    flb_http_server_options_init(&options);
    test_http_server_context_init(&context);
    context.expected_idle_timeout = 0;

    port = test_http_server_reserve_port();
    if (!TEST_CHECK(port > 0)) {
        test_http_server_context_destroy(&context);
        flb_config_exit(config);
        test_http_server_network_cleanup();
        return;
    }

    options.protocol_version = HTTP_PROTOCOL_VERSION_AUTODETECT;
    options.request_callback = test_http_server_request_handler;
    options.address = (char *) TEST_HTTP_SERVER_HOST;
    options.port = port;
    options.networking_flags = FLB_IO_TCP;
    options.networking_setup = &net_setup;
    options.system_context = config;
    options.workers = 2;
    options.idle_timeout = 0;
    options.user_data = &context;
    options.cb_worker_init = test_http_server_worker_init;
    options.cb_worker_exit = test_http_server_worker_exit;

    ret = flb_http_server_init_with_options(&server, &options);
    TEST_CHECK(ret == 0);
    if (ret != 0) {
        test_http_server_context_destroy(&context);
        flb_config_exit(config);
        test_http_server_network_cleanup();
        return;
    }

    TEST_CHECK(server.idle_timeout == 0);
    TEST_CHECK(net_setup.io_timeout == 0);

    ret = flb_http_server_start(&server);
#if defined(SO_REUSEPORT) && !defined(FLB_SYSTEM_WINDOWS)
    TEST_CHECK(ret == 0);
    if (ret != 0) {
        flb_http_server_destroy(&server);
        flb_config_exit(config);
        test_http_server_network_cleanup();
        return;
    }

    TEST_CHECK(context.init_calls == TEST_HTTP_SERVER_WORKERS);
    TEST_CHECK(context.idle_timeout_mismatches == 0);

    flb_http_server_destroy(&server);
    TEST_CHECK(context.exit_calls == TEST_HTTP_SERVER_WORKERS);
    TEST_CHECK(context.exit_thread_mismatches == 0);
#else
    TEST_CHECK(ret != 0);
    flb_http_server_destroy(&server);
#endif
    test_http_server_context_destroy(&context);
    flb_config_exit(config);
    test_http_server_network_cleanup();
}

void test_http_server_session_destroy_with_closed_connection()
{
    struct flb_connection connection;
    struct flb_http_server_session *session;

    memset(&connection, 0, sizeof(struct flb_connection));
    connection.fd = FLB_INVALID_SOCKET;

    session = flb_http_server_session_create(HTTP_PROTOCOL_VERSION_11);
    if (!TEST_CHECK(session != NULL)) {
        return;
    }

    session->connection = &connection;
    connection.user_data = session;

    flb_http_server_session_destroy(session);

    TEST_CHECK(connection.user_data == NULL);
}

void test_http_server_session_destroy_clears_drop_pending()
{
    struct flb_connection connection;
    struct flb_http_server_session *session;

    memset(&connection, 0, sizeof(struct flb_connection));
    connection.fd = FLB_INVALID_SOCKET;

    session = flb_http_server_session_create(HTTP_PROTOCOL_VERSION_11);
    if (!TEST_CHECK(session != NULL)) {
        return;
    }

    session->connection = &connection;
    session->drop_pending = FLB_TRUE;
    session->releasable = FLB_FALSE;
    connection.user_data = session;

    flb_http_server_session_destroy(session);

    TEST_CHECK(connection.user_data == NULL);
    TEST_CHECK(session->drop_pending == FLB_FALSE);

    flb_free(session);
}

void test_http_server_session_destroy_is_reentrant_safe()
{
    struct flb_connection connection;
    struct flb_http_server_session *session;

    memset(&connection, 0, sizeof(struct flb_connection));
    connection.fd = FLB_INVALID_SOCKET;

    session = flb_http_server_session_create(HTTP_PROTOCOL_VERSION_11);
    if (!TEST_CHECK(session != NULL)) {
        return;
    }

    session->connection = &connection;
    session->releasable = FLB_FALSE;
    session->destroying = FLB_TRUE;
    connection.user_data = session;

    flb_http_server_session_destroy(session);

    TEST_CHECK(session->connection == &connection);
    TEST_CHECK(connection.user_data == session);

    session->destroying = FLB_FALSE;
    flb_http_server_session_destroy(session);

    TEST_CHECK(connection.user_data == NULL);
    TEST_CHECK(session->connection == NULL);

    flb_free(session);
}

/* 队列暂满时整个多 Tag 批次不得留下前缀，永久超限与临时背压必须区分。 */
static void test_batch_queue(void)
{
    const char records[] = "\x92\x01\x80\x92\x01\x80\x92\x01\x80\x92\x01\x80";
    const char large[] = "\x92\x01\x81\xa1" "x" "\xb4" "abcdefghijklmnopqrst";
    struct flb_input_ingress_log first = {"a", 1, records, 6, 2};
    struct flb_input_ingress_log second[] = {
        {"b", 1, records, 3, 1}, {"c", 1, records + 3, 3, 1}
    };
    struct flb_input_ingress_log oversized = {"d", 1, records, 12, 4};
    struct flb_input_ingress_log bytes = {"e", 1, large, sizeof(large) - 1, 1};
    struct flb_input_instance *ins;
    flb_ctx_t *ctx;
    int id;

    ctx = flb_create();
    if (!TEST_CHECK(ctx != NULL)) {
        return;
    }
    ctx->config->evl = mk_event_loop_create(32);
    if (!TEST_CHECK(ctx->config->evl != NULL)) {
        flb_destroy(ctx);
        return;
    }
    id = flb_input(ctx, "dummy", NULL);
    ins = flb_input_get_instance(ctx->config, id);
    if (!TEST_CHECK(ins != NULL)) {
        flb_destroy(ctx);
        return;
    }
    if (!TEST_CHECK(flb_input_ingress_enable(ins) == 0)) {
        flb_destroy(ctx);
        return;
    }
    /* 不启动 owner，使两个已接收记录稳定占用配额，避免依赖线程调度复现。 */
    ins->ingress_queue_event_limit = 3;
    ins->ingress_queue_byte_limit = 16;
    if (ins->http_server_config == NULL) {
        ins->http_server_config = flb_calloc(1, sizeof(*ins->http_server_config));
    }
    if (!TEST_CHECK(ins->http_server_config != NULL)) {
        flb_destroy(ctx);
        return;
    }
    ins->http_server_config->ingress_queue_wait_timeout_ms = 250;
    TEST_CHECK(flb_input_ingress_queue_log_batch(ins, &first, 1) == 0);
    TEST_CHECK(ins->ingress_queue_pending_events == 2);
    TEST_CHECK(ins->ingress_queue_pending_bytes == 6);
    {
        uint64_t started = cfl_time_now();

        TEST_CHECK(flb_input_ingress_queue_log_batch(ins, second, 2) == FLB_INPUT_INGRESS_BUSY);
        /* 配置的等待窗口必须生效，不能仍按原来的 100ms 提前拒绝。 */
        TEST_CHECK(cfl_time_now() - started >= 200000000);
    }
    TEST_CHECK(ins->ingress_queue_pending_events == 2);
    TEST_CHECK(ins->ingress_queue_pending_bytes == 6);
    TEST_CHECK(mk_list_size(&ins->ingress_queue) == 1);
    TEST_CHECK(flb_input_ingress_queue_log_batch(ins, &oversized, 1) ==
               FLB_INPUT_INGRESS_TOO_LARGE);
    TEST_CHECK(flb_input_ingress_queue_log_batch(ins, &bytes, 1) == FLB_INPUT_INGRESS_TOO_LARGE);
    TEST_CHECK(ins->ingress_queue_pending_events == 2);
    TEST_CHECK(ins->ingress_queue_pending_bytes == 6);
    flb_destroy(ctx);
}

TEST_LIST = {
    { "batch_queue", test_batch_queue },
    { "http_server_options_defaults", test_http_server_options_defaults },
    { "http_server_options_multi_worker_magic", test_http_server_options_multi_worker_magic },
    { "http_server_managed_worker_contract", test_http_server_managed_worker_contract },
    { "http_server_worker_exit_runs_on_worker_thread",
      test_http_server_worker_exit_runs_on_worker_thread },
    { "http_server_single_managed_worker_start",
      test_http_server_single_managed_worker_start },
    { "http_server_workers_reject_distinct_ephemeral_ports",
      test_http_server_workers_reject_distinct_ephemeral_ports },
    { "http_server_idle_timeout_applies_to_networking_setup",
      test_http_server_idle_timeout_applies_to_networking_setup },
    { "http_server_explicit_network_timeout_is_preserved",
      test_http_server_explicit_network_timeout_is_preserved },
    { "http_server_multi_worker_disabled_idle_timeout_is_preserved",
      test_http_server_multi_worker_disabled_idle_timeout_is_preserved },
    { "http_server_session_destroy_with_closed_connection",
      test_http_server_session_destroy_with_closed_connection },
    { "http_server_session_destroy_clears_drop_pending",
      test_http_server_session_destroy_clears_drop_pending },
    { "http_server_session_destroy_is_reentrant_safe",
      test_http_server_session_destroy_is_reentrant_safe },
    { 0 }
};
