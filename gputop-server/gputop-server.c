/*
 * GPU Top
 *
 * Copyright (C) 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <config.h>

#include <linux/perf_event.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <sys/stat.h>
#include <intel_chipset.h>

#include <uv.h>

#include <h2o.h>
#include <h2o/websocket.h>
#include <wslay/wslay_event.h>

#include "gputop-server.h"
#include "gputop-perf.h"
#include "gputop-util.h"
#include "gputop-cpu.h"
#include "gputop-mainloop.h"
#include "gputop-log.h"
#include "gputop.pb-c.h"
#include "gputop-debugfs.h"

#ifdef SUPPORT_GL
#include "gputop-gl.h"
#endif

static h2o_websocket_conn_t *h2o_conn;
static h2o_globalconf_t config;
static h2o_context_t ctx;
static SSL_CTX *ssl_ctx;
static uv_tcp_t listener;

static uv_timer_t timer;

static bool update_queued;
static uv_idle_t update_idle;


enum {
    WS_MESSAGE_PERF = 1,
    WS_MESSAGE_PROTOBUF,
    WS_MESSAGE_I915_PERF,
};

struct protobuf_msg_closure;

typedef void (*gputop_closure_done_t)(struct protobuf_msg_closure *closure);

struct protobuf_msg_closure {
    int current_offset;
    int len;
    uint8_t *data;
    gputop_closure_done_t done_callback;
};

static ssize_t
fragmented_protobuf_msg_read_cb(wslay_event_context_ptr ctx,
                                uint8_t *data, size_t len,
                                const union wslay_event_msg_source *source,
                                int *eof,
                                void *user_data)
{
    struct protobuf_msg_closure *closure =
        (struct protobuf_msg_closure *)source->data;
    int remaining;
    int read_len;
    int total = 0;

    if (closure->current_offset == 0) {
        assert(len > 8);
        data[0] = WS_MESSAGE_PROTOBUF;
        total = 8;
        data += 8;
        len -= 8;
    }

    remaining = closure->len - closure->current_offset;
    read_len = MIN(remaining, len);

    memcpy(data, closure->data + closure->current_offset, read_len);
    closure->current_offset += read_len;
    total += read_len;

    if(closure->current_offset == closure->len)
        *eof = 1;

    return total;
}

static void
on_protobuf_msg_sent_cb(const union wslay_event_msg_source *source, void *user_data)
{
    struct protobuf_msg_closure *closure = (void *)source->data;

    free(closure->data);
    free(closure);
}

static gputop_list_t streams;
static gputop_list_t closing_streams;

/*
 * FIXME: don't duplicate these...
 */

#define TAKEN(HEAD, TAIL, POT_SIZE)	(((HEAD) - (TAIL)) & (POT_SIZE - 1))

/* Note: this will equate to 0 when the buffer is exactly full... */
#define REMAINING(HEAD, TAIL, POT_SIZE) (POT_SIZE - TAKEN (HEAD, TAIL, POT_SIZE))

#if defined(__i386__)
#define rmb()           __asm__ volatile("lock; addl $0,0(%%esp)" ::: "memory")
#define mb()            __asm__ volatile("lock; addl $0,0(%%esp)" ::: "memory")
#endif

#if defined(__x86_64__)
#define rmb()           __asm__ volatile("lfence" ::: "memory")
#define mb()            __asm__ volatile("mfence" ::: "memory")
#endif


struct perf_flush_closure {
    bool header_written;
    int id;
    int total_len;
    struct gputop_perf_stream *stream;
    uint64_t head;
    uint64_t tail;
};

static unsigned int
read_perf_head(struct perf_event_mmap_page *mmap_page)
{
    unsigned int head = (*(volatile uint64_t *)&mmap_page->data_head);
    rmb();

    return head;
}

static void
write_perf_tail(struct perf_event_mmap_page *mmap_page,
                unsigned int tail)
{
    /* Make sure we've finished reading all the sample data we
     * were consuming before updating the tail... */
    mb();
    mmap_page->data_tail = tail;
}

static void
send_pb_message(h2o_websocket_conn_t *conn, ProtobufCMessage *pb_message)
{
    struct wslay_event_fragmented_msg msg;
    struct protobuf_msg_closure *closure;

    if (!conn)
        return;

    closure = xmalloc(sizeof(*closure));
    closure->current_offset = 0;
    closure->len = protobuf_c_message_get_packed_size(pb_message);
    closure->data = xmalloc(closure->len);

    protobuf_c_message_pack(pb_message, closure->data);

    msg.opcode = WSLAY_BINARY_FRAME;
    msg.source.data = closure;
    msg.read_callback = fragmented_protobuf_msg_read_cb;
    msg.finish_callback = on_protobuf_msg_sent_cb;

    wslay_event_queue_fragmented_msg(conn->ws_ctx, &msg);
    wslay_event_send(conn->ws_ctx);
}

static void
stream_closed_cb(struct gputop_perf_stream *stream)
{
    Gputop__Message message_ack = GPUTOP__MESSAGE__INIT;
    Gputop__Message message = GPUTOP__MESSAGE__INIT;
    Gputop__CloseNotify notify = GPUTOP__CLOSE_NOTIFY__INIT;

    /* stream->user.data will = a UUID if it was closed
     * in response to a remote request which we need to
     * ACK... */
    if (stream->user.data) {

        message_ack.reply_uuid = stream->user.data;
        message_ack.cmd_case = GPUTOP__MESSAGE__CMD_ACK;
        message_ack.ack = true;

        dbg("CMD_ACK: %s\n", (char *)stream->user.data);

        send_pb_message(h2o_conn, &message_ack.base);

        free(stream->user.data);
        stream->user.data = NULL;
    }

    notify.id = stream->user.id;

    message.cmd_case = GPUTOP__MESSAGE__CMD_CLOSE_NOTIFY;
    message.close_notify = &notify;

    send_pb_message(h2o_conn, &message.base);

    gputop_list_remove(&stream->user.link);

    gputop_perf_stream_unref(stream);
}

static void
on_perf_flush_done(const union wslay_event_msg_source *source, void *user_data)
{
    struct perf_flush_closure *closure =
        (struct perf_flush_closure *)source->data;

    //fprintf(stderr, "wrote perf message: len=%d\n", closure->total_len);
    closure->stream->user.flushing = false;

    if (closure->stream->pending_close)
        gputop_perf_stream_close(closure->stream, stream_closed_cb);

    gputop_perf_stream_unref(closure->stream);

    free(closure);
}

static ssize_t
fragmented_perf_read_cb(wslay_event_context_ptr ctx,
                        uint8_t *data, size_t len,
                        const union wslay_event_msg_source *source,
                        int *eof,
                        void *user_data)
{
    struct perf_flush_closure *closure =
        (struct perf_flush_closure *)source->data;
    struct gputop_perf_stream *stream = closure->stream;
    const uint64_t mask = stream->perf.buffer_size - 1;
    int read_len;
    int total = 0;
    uint64_t head;
    uint64_t tail;
    uint64_t remainder;
    uint8_t *buffer;
    uint8_t *p;

    if (!closure->header_written) {
        assert(len > 8);

        memset(data, 0, 8);
        data[0] = WS_MESSAGE_PERF;
        *(uint32_t *)(data + 4) = closure->id;

        total = 8;
        data += 8;
        len -= 8;
        closure->header_written = true;
    }

    head = closure->head;
    tail = closure->tail;

    buffer = stream->perf.buffer;

    if ((head & mask) < (tail & mask)) {
        int before;

        p = buffer + (tail & mask);
        before = stream->perf.buffer_size - (tail & mask);
        read_len = MIN(before, len);
        memcpy(data, p, read_len);

        len -= read_len;
        tail += read_len;
        data += read_len;
        total += read_len;

        closure->tail = tail;
    }

    p = buffer + (tail & mask);
    remainder = TAKEN(head, tail, stream->perf.buffer_size);
    read_len = MIN(remainder, len);
    memcpy(data, p, read_len);

    len -= read_len;
    tail += read_len;
    total += read_len;
    closure->tail = tail;

    closure->total_len += total;

    if (TAKEN(head, tail, stream->perf.buffer_size) == 0) {
        *eof = 1;
        write_perf_tail(stream->perf.mmap_page, tail);
    }

    return total;
}

static void
flush_perf_stream_samples(struct gputop_perf_stream *stream)
{
    uint64_t head = read_perf_head(stream->perf.mmap_page);
    uint64_t tail = stream->perf.mmap_page->data_tail;
    struct perf_flush_closure *closure;
    struct wslay_event_fragmented_msg msg;

    stream->user.flushing = true;

    /* Ensure the stream can't be freed while we're in the
     * middle of forwarding samples... */
    gputop_perf_stream_ref(stream);

    //gputop_perf_print_records(stream, head, tail, false);

    closure = xmalloc(sizeof(*closure));
    closure->header_written = false;
    closure->id = stream->user.id;
    closure->total_len = 0;
    closure->stream = stream;
    closure->head = head;
    closure->tail = tail;

    memset(&msg, 0, sizeof(msg));
    msg.opcode = WSLAY_BINARY_FRAME;
    msg.source.data = closure;
    msg.read_callback = fragmented_perf_read_cb;
    msg.finish_callback = on_perf_flush_done;

    wslay_event_queue_fragmented_msg(h2o_conn->ws_ctx, &msg);

    wslay_event_send(h2o_conn->ws_ctx);
}

struct i915_perf_flush_closure {
    bool header_written;
    int id;
    int total_len;
    struct gputop_perf_stream *stream;
};

static void
on_i915_perf_flush_done(const union wslay_event_msg_source *source, void *user_data)
{
    struct i915_perf_flush_closure *closure =
        (struct i915_perf_flush_closure *)source->data;

    //fprintf(stderr, "wrote perf message: len=%d\n", closure->total_len);
    closure->stream->user.flushing = false;

    if (closure->stream->pending_close)
        gputop_perf_stream_close(closure->stream, stream_closed_cb);

    gputop_perf_stream_unref(closure->stream);

    free(closure);
}

static ssize_t
fragmented_i915_perf_read_cb(wslay_event_context_ptr ctx,
                             uint8_t *data, size_t len,
                             const union wslay_event_msg_source *source,
                             int *eof,
                             void *user_data)
{
    struct i915_perf_flush_closure *closure =
        (struct i915_perf_flush_closure *)source->data;
    struct gputop_perf_stream *stream = closure->stream;
    int total = 0;
    int read_len;

    if (!closure->header_written) {
        assert(len > 8);

        memset(data, 0, 8);
        data[0] = WS_MESSAGE_I915_PERF;
        *(uint32_t *)(data + 4) = closure->id;

        total = 8;
        data += 8;
        len -= 8;
        closure->header_written = true;
    }

    if (gputop_fake_mode)
        read_len = gputop_perf_fake_read(stream, data, len);
    else
        while ((read_len = read(stream->fd, data, len)) < 0 && errno == EINTR)
            ;
    if (read_len > 0) {
        total += read_len;
        closure->total_len += total;
    } else {
        *eof = 1;
        if (!gputop_fake_mode && errno != EAGAIN)
            dbg("Error reading i915 perf stream %m\n");
    }

    return total;
}

static void
flush_i915_perf_stream_samples(struct gputop_perf_stream *stream)
{
    struct i915_perf_flush_closure *closure;
    struct wslay_event_fragmented_msg msg;

    stream->user.flushing = true;

    /* Ensure the stream can't be freed while we're in the
     * middle of forwarding samples... */
    gputop_perf_stream_ref(stream);

    //gputop_perf_print_records(stream, head, tail, false);

    closure = xmalloc(sizeof(*closure));
    closure->header_written = false;
    closure->id = stream->user.id;
    closure->total_len = 0;
    closure->stream = stream;

    memset(&msg, 0, sizeof(msg));
    msg.opcode = WSLAY_BINARY_FRAME;
    msg.source.data = closure;
    msg.read_callback = fragmented_i915_perf_read_cb;
    msg.finish_callback = on_i915_perf_flush_done;

    wslay_event_queue_fragmented_msg(h2o_conn->ws_ctx, &msg);

    wslay_event_send(h2o_conn->ws_ctx);
}

static void
flush_cpu_stats(struct gputop_perf_stream *stream)
{
    int n_cpus = gputop_cpu_count();
    int n;
    int pos;

    if (stream->cpu.stats_buf_pos == 0 && !stream->cpu.stats_buf_full)
        return;

    if (stream->cpu.stats_buf_full) {
        n = stream->cpu.stats_buf_len / n_cpus;
        pos = stream->cpu.stats_buf_pos;
    } else {
        n = stream->cpu.stats_buf_pos / n_cpus;
        pos = 0;
    }

    for (int i = 0; i < n; i++) {
        Gputop__Message message = GPUTOP__MESSAGE__INIT;
        Gputop__CpuStatsSet set = GPUTOP__CPU_STATS_SET__INIT;
        Gputop__CpuStats **stats_vec;
        Gputop__CpuStats *stats;
        struct cpu_stat *stat = stream->cpu.stats_buf + pos;

        stats_vec = alloca(sizeof(void *) * n_cpus);
        stats = alloca(sizeof(Gputop__CpuStats) * n_cpus);

        message.cmd_case = GPUTOP__MESSAGE__CMD_CPU_STATS;

        message.cpu_stats = &set;

        set.id = stream->user.id;
        set.n_cpus = n_cpus;
        set.cpus = stats_vec;

        for (int i = 0; i < n_cpus; i++) {
            gputop__cpu_stats__init(&stats[i]);
            stats_vec[i] = &stats[i];
            stats[i].timestamp = stat[i].timestamp;
            stats[i].user = stat[i].user;
            stats[i].nice = stat[i].nice;
            stats[i].system = stat[i].system;
            stats[i].idle = stat[i].idle;
            stats[i].iowait = stat[i].iowait;
            stats[i].irq = stat[i].irq;
            stats[i].softirq = stat[i].softirq;
            stats[i].steal = stat[i].steal;
            stats[i].guest = stat[i].guest;
            stats[i].guest_nice = stat[i].guest_nice;
        }

        send_pb_message(h2o_conn, &message.base);

        pos += n_cpus;
        if (pos >= stream->cpu.stats_buf_len)
            pos = 0;
    }

    stream->cpu.stats_buf_pos = 0;
    stream->cpu.stats_buf_full = false;
}

static void
flush_stream_samples(struct gputop_perf_stream *stream)
{
    if (stream->user.flushing) {
        fprintf(stderr, "Throttling websocket forwarding\n");
        return;
    }

    assert(!stream->pending_close);
    assert(!stream->closed);

    if (!gputop_stream_data_pending(stream))
        return;

    switch (stream->type) {
    case GPUTOP_STREAM_PERF:
        flush_perf_stream_samples(stream);
        break;
    case GPUTOP_STREAM_I915_PERF:
        flush_i915_perf_stream_samples(stream);
        break;
    case GPUTOP_STREAM_CPU:
        flush_cpu_stats(stream);
        break;
    }
}

static void
update_perf_head_pointers(struct gputop_perf_stream *stream)
{
    struct gputop_perf_header_buf *hdr_buf = &stream->perf.header_buf;

    gputop_perf_update_header_offsets(stream);

    if (!hdr_buf->full) {
        Gputop__Message message = GPUTOP__MESSAGE__INIT;
        Gputop__BufferFillNotify notify = GPUTOP__BUFFER_FILL_NOTIFY__INIT;

        notify.query_id = stream->user.id;
        notify.fill_percentage =
            (hdr_buf->offsets[(hdr_buf->head - 1) % hdr_buf->len] /
             (float)stream->perf.buffer_size) * 100.0f;
        message.cmd_case = GPUTOP__MESSAGE__CMD_FILL_NOTIFY;
        message.fill_notify = &notify;

        send_pb_message(h2o_conn, &message.base);
        //dbg("XXX: %s > %d%% full\n", stream->query ? stream->query->name : "unknown", notify.fill_percentage);
    }
}

static void
update_streams(void)
{
    struct gputop_perf_stream *stream;

    gputop_list_for_each(stream, &streams, user.link) {
        if (stream->live_updates)
            flush_stream_samples(stream);
        else if (stream->type == GPUTOP_STREAM_PERF)
            update_perf_head_pointers(stream);
    }
}

static void
forward_logs(void)
{
    Gputop__Log *log = gputop_get_pb_log();

    if (log) {
        Gputop__Message msg = GPUTOP__MESSAGE__INIT;

        fprintf(stderr, "forwarding log to UI\n");

        msg.cmd_case = GPUTOP__MESSAGE__CMD_LOG;
        msg.log = log;

        send_pb_message(h2o_conn, &msg.base);

        gputop_pb_log_free(log);
    }
}

static void
update_cb(uv_idle_t *idle)
{
    uv_idle_stop(&update_idle);
    update_queued = false;

    update_streams();

    forward_logs();
}

/* We may have a number of metric streams with events for available data being
 * delievered via the libuv mainloop but to minimize the time spent responding
 * and forwarding those metrics to any UI we consolidate the follow up work via
 * an idle mainloop callback...
 */
static void
queue_update(void)
{
    if (update_queued)
        return;

    uv_idle_start(&update_idle, update_cb);
}

static void
periodic_update_cb(uv_timer_t *timer)
{
    queue_update();
}

static void
i915_perf_ready_cb(struct gputop_perf_stream *stream)
{
    queue_update();
}

static void
handle_open_i915_perf_oa_query(h2o_websocket_conn_t *conn,
                               Gputop__Request *request)
{
    Gputop__OpenQuery *open_query = request->open_query;
    uint32_t id = open_query->id;
    Gputop__OAQueryInfo *oa_query_info = open_query->oa_query;
    struct gputop_metric_set *metric_set = NULL;
    struct gputop_hash_entry *entry = NULL;
    struct gputop_perf_stream *stream;
    char *error = NULL;
    struct ctx_handle *ctx = NULL;
    Gputop__Message message = GPUTOP__MESSAGE__INIT;
    message.reply_uuid = request->uuid;
    message.ack = true;

    if (!gputop_perf_initialize()) {
        asprintf(&error, "Failed to initialize perf\n");
        goto err;
    }
    dbg("handle_open_i915_perf_oa_query: id = %d\n", id);

    entry = gputop_hash_table_search(metrics, oa_query_info->guid);
    if (entry != NULL) {
        metric_set = entry->data;
    } else {
        asprintf(&error, "Guid is not available\n");
        goto err;
    }

    // TODO: (matt-auld)
    // Currently we don't support selectable contexts, so we just use the
    // first one which is avaivable to us. Though this would only really
    // make sense if we could make the list of contexts visible to the user.
    // Maybe later the per_ctx_mode could become the context handle...
    if (open_query->per_ctx_mode) {
        ctx = get_first_available_ctx(&error);
        if (!ctx)
            goto err;
    }

    stream = gputop_open_i915_perf_oa_stream(metric_set,
                                             oa_query_info->period_exponent,
                                             ctx,
                                             (open_query->live_updates ?
                                              i915_perf_ready_cb : NULL),
                                             open_query->overwrite,
                                             &error);
    if (stream) {
        stream->user.id = id;
        gputop_list_init(&stream->user.link);
        gputop_list_insert(streams.prev, &stream->user.link);

        stream->live_updates = open_query->live_updates;
    } else {
        dbg("Failed to open perf query set=%s period=%d: %s\n",
            oa_query_info->guid, oa_query_info->period_exponent,
            error);
        goto err;
    }

    message.reply_uuid = request->uuid;
    message.cmd_case = GPUTOP__MESSAGE__CMD_ACK;
    message.ack = true;
    send_pb_message(conn, &message.base);

    return;

err:
    message.cmd_case = GPUTOP__MESSAGE__CMD_ERROR;
    message.error = error;
    send_pb_message(conn, &message.base);
    free(error);

    return;
}

static void
handle_open_tracepoint(h2o_websocket_conn_t *conn,
                       Gputop__Request *request)
{
    Gputop__OpenQuery *open_query = request->open_query;
    uint32_t id = open_query->id;
    Gputop__TracepointConfig *config = open_query->tracepoint;
    struct gputop_perf_stream *stream;
    char *error = NULL;
    int buffer_size;
    Gputop__Message message = GPUTOP__MESSAGE__INIT;

    if (!gputop_perf_initialize()) {
        message.reply_uuid = request->uuid;
        message.cmd_case = GPUTOP__MESSAGE__CMD_ERROR;
        message.error = "Failed to initialize perf\n";
        send_pb_message(conn, &message.base);
        return;
    }

    /* NB: Perf buffer size must be a power of two.
     * We don't need a large buffer if we're periodically forwarding data */
    if (open_query->live_updates)
        buffer_size = 128 * 1024;
    else
        buffer_size = 16 * 1024 * 1024;

    stream = gputop_perf_open_tracepoint(config->pid,
                                         config->cpu,
                                         config->id,
                                         12, /* FIXME: guess trace struct size
                                              * used to estimate number of samples
                                              * that will fit in buffer */
                                         buffer_size,
                                         NULL,
                                         open_query->overwrite,
                                         &error);
    if (stream) {
        stream->user.id = id;
        gputop_list_init(&stream->user.link);
        gputop_list_insert(streams.prev, &stream->user.link);

        stream->live_updates = open_query->live_updates;
    } else {
        dbg("Failed to open trace %"PRIu32": %s\n", config->id, error);
        free(error);
    }

    message.reply_uuid = request->uuid;
    message.cmd_case = GPUTOP__MESSAGE__CMD_ACK;
    message.ack = true;
    send_pb_message(conn, &message.base);
}

static void
handle_open_generic_query(h2o_websocket_conn_t *conn,
                          Gputop__Request *request)
{
    Gputop__OpenQuery *open_query = request->open_query;
    uint32_t id = open_query->id;
    Gputop__GenericEventInfo *generic_info = open_query->generic;
    struct gputop_perf_stream *stream;
    char *error = NULL;
    int buffer_size;
    Gputop__Message message = GPUTOP__MESSAGE__INIT;

    if (!gputop_perf_initialize()) {
        message.reply_uuid = request->uuid;
        message.cmd_case = GPUTOP__MESSAGE__CMD_ERROR;
        message.error = "Failed to initialize perf\n";
        send_pb_message(conn, &message.base);
        return;
    }

    /* NB: Perf buffer size must be a power of two.
     * We don't need a large buffer if we're periodically forwarding data */
    if (open_query->live_updates)
        buffer_size = 128 * 1024;
    else
        buffer_size = 16 * 1024 * 1024;

    stream = gputop_perf_open_generic_counter(generic_info->pid,
                                              generic_info->cpu,
                                              generic_info->type,
                                              generic_info->config,
                                              buffer_size,
                                              NULL,
                                              open_query->overwrite,
                                              &error);
    if (stream) {
        stream->user.id = id;
        gputop_list_init(&stream->user.link);
        gputop_list_insert(streams.prev, &stream->user.link);

        stream->live_updates = open_query->live_updates;
    } else {
        dbg("Failed to open perf event: %s\n", error);
        free(error);
    }

    message.reply_uuid = request->uuid;
    message.cmd_case = GPUTOP__MESSAGE__CMD_ACK;
    message.ack = true;
    send_pb_message(conn, &message.base);
}

static void
handle_open_cpu_stats(h2o_websocket_conn_t *conn,
                      Gputop__Request *request)
{
    Gputop__OpenQuery *open_query = request->open_query;
    uint32_t id = open_query->id;
    Gputop__CpuStatsInfo *stats_info = open_query->cpu_stats;
    Gputop__Message message = GPUTOP__MESSAGE__INIT;
    struct gputop_perf_stream *stream;

    if (!gputop_perf_initialize()) {
        message.reply_uuid = request->uuid;
        message.cmd_case = GPUTOP__MESSAGE__CMD_ERROR;
        message.error = "Failed to initialize perf\n";
        send_pb_message(conn, &message.base);
        return;
    }

    stream = gputop_perf_open_cpu_stats(open_query->overwrite,
                                        stats_info->sample_period_ms);
    if (stream) {
        stream->user.id = id;
        gputop_list_init(&stream->user.link);
        gputop_list_insert(streams.prev, &stream->user.link);

        stream->live_updates = open_query->live_updates;
    }

    message.reply_uuid = request->uuid;
    message.cmd_case = GPUTOP__MESSAGE__CMD_ACK;
    message.ack = true;
    send_pb_message(conn, &message.base);
}

static void
handle_open_query(h2o_websocket_conn_t *conn, Gputop__Request *request)
{
    Gputop__OpenQuery *open_query = request->open_query;
    Gputop__Message message = GPUTOP__MESSAGE__INIT;


    switch (open_query->type_case) {
    case GPUTOP__OPEN_QUERY__TYPE_OA_QUERY:
        handle_open_i915_perf_oa_query(conn, request);
        break;
    case GPUTOP__OPEN_QUERY__TYPE_TRACEPOINT:
        handle_open_tracepoint(conn, request);
        break;
    case GPUTOP__OPEN_QUERY__TYPE_GENERIC:
        handle_open_generic_query(conn, request);
        break;
    case GPUTOP__OPEN_QUERY__TYPE_CPU_STATS:
        handle_open_cpu_stats(conn, request);
        break;
    default:
        message.reply_uuid = request->uuid;
        message.cmd_case = GPUTOP__MESSAGE__CMD_ERROR;
        message.error = "FIXME: implement support for opening GL queries\n";

        send_pb_message(conn, &message.base);
        fprintf(stderr, "TODO: support opening GL queries");
    }
}

static void
close_stream(struct gputop_perf_stream *stream)
{
    /* By moving the stream into the closing_streams list we ensure we
     * won't forward anymore for the stream in case we can't close the
     * stream immediately.
     */
    gputop_list_remove(&stream->user.link);
    gputop_list_insert(closing_streams.prev, &stream->user.link);
    stream->pending_close = true;

    /* NB: we can't synchronously close the perf event if we're in the
     * middle of writing samples to the websocket...
     */
    if (!stream->user.flushing)
        gputop_perf_stream_close(stream, stream_closed_cb);
}

static void
close_all_streams(void)
{
    struct gputop_perf_stream *stream, *tmp;

    gputop_list_for_each_safe(stream, tmp, &streams, user.link) {
        close_stream(stream);
    }
}

static void
handle_close_query(h2o_websocket_conn_t *conn,
                   Gputop__Request *request)
{
    struct gputop_perf_stream *stream;
    uint32_t id = request->close_query;

    dbg("handle_close_query: id=%d, request_uuid=%s\n", id, request->uuid);

    gputop_list_for_each(stream, &streams, user.link) {
        if (stream->user.id == id) {
            assert(stream->user.data == NULL);

            stream->user.data = strdup(request->uuid);
            close_stream(stream);
            return;
        }
    }
}

bool
gputop_get_cmd_line_pid(uint32_t pid, char *buf, int len)
{
    FILE *fp;
    char *line = NULL;
    size_t line_len = 0;
    ssize_t read;
    char pid_path[512];
    bool res = false;

    memset(buf, 0, len);
    snprintf(buf, len, "Unknown");

    snprintf(pid_path, sizeof(pid_path), "/proc/%d/cmdline", pid);
    fp = fopen(pid_path, "r");
    if (!fp)
        return false;

    read = getline(&line, &line_len, fp);
    if (read!= -1) {
        printf("Reading line %s", line);
        snprintf(buf, len, "%s", line);
        res = true;
    }

    fclose(fp);
    free(line);
    return res;
}

static void
handle_get_process_info(h2o_websocket_conn_t *conn,
                    Gputop__Request *request)
{

    char cmdline[128];
    uint32_t pid = request->get_process_info;

    Gputop__Message message = GPUTOP__MESSAGE__INIT;
    Gputop__ProcessInfo process_info = GPUTOP__PROCESS_INFO__INIT;

    message.reply_uuid = request->uuid;

    if (gputop_get_cmd_line_pid(pid, cmdline, sizeof(cmdline))) {
        dbg("  Sending PID = %d\n", pid);
        message.cmd_case = GPUTOP__MESSAGE__CMD_PROCESS_INFO;
        process_info.pid = pid;
        process_info.cmd_line = cmdline;
        message.process_info = &process_info;
        send_pb_message(conn, &message.base);
        return;
    }

    message.cmd_case = GPUTOP__MESSAGE__CMD_ERROR;
    message.error = "Failed to find process\n";
    send_pb_message(conn, &message.base);
    dbg("Failed to find process %d\n", pid);
}

static void
handle_get_tracepoint_info(h2o_websocket_conn_t *conn,
                           Gputop__Request *request)
{

    char *name = request->get_tracepoint_info;
    char filename[1024];
    int len = 0;

    Gputop__Message message = GPUTOP__MESSAGE__INIT;
    Gputop__TracepointInfo tracepoint_info = GPUTOP__TRACEPOINT_INFO__INIT;

    message.reply_uuid = request->uuid;

    snprintf(filename, sizeof(filename), "tracing/events/%s/format", name);
    tracepoint_info.sample_format = gputop_debugfs_read(filename, &len);
    if (!tracepoint_info.sample_format)
        goto error;

    snprintf(filename, sizeof(filename), "tracing/events/%s/id", name);
    tracepoint_info.event_id = gputop_debugfs_read_uint64(filename);

    message.cmd_case = GPUTOP__MESSAGE__CMD_TRACEPOINT_INFO;
    message.tracepoint_info = &tracepoint_info;
    send_pb_message(conn, &message.base);

    free(tracepoint_info.sample_format);

    return;

error:
    message.cmd_case = GPUTOP__MESSAGE__CMD_ERROR;
    send_pb_message(conn, &message.base);
}

#ifdef SUPPORT_GL
static Gputop__GLQueryInfo **
get_gl_query_info(int *n_queries_ret)
{
    Gputop__GLQueryInfo **queries_vec = NULL;
    Gputop__GLQueryInfo *queries = NULL;
    int n_queries = 0;

    pthread_rwlock_rdlock(&gputop_gl_lock);

    /* XXX: we currently assume if there are multiple contexts in use, they
     * have the same queries, with the same IDs available...
     */
    if (gputop_gl_contexts->len) {
        struct winsys_context **contexts = gputop_gl_contexts->data;
        struct winsys_context *first_wctx = contexts[0];
        struct intel_query_info *q;
        int i;

        n_queries = gputop_list_length(&first_wctx->queries);

        queries_vec = xmalloc(sizeof(void *) * n_queries);
        queries = xmalloc(sizeof(Gputop__GLQueryInfo) * n_queries);

        i = 0;
        gputop_list_for_each(q, &first_wctx->queries, link) {
            Gputop__GLQueryInfo *query = &queries[i];
            Gputop__GLCounter **counters_vec;
            Gputop__GLCounter *counters;

            query->name = q->name;
            query->id = q->id;

            counters_vec = xmalloc(sizeof(void *) * q->n_counters);
            counters = xmalloc(sizeof(Gputop__GLCounter) * q->n_counters);
            query->n_counters = q->n_counters;
            query->counters = counters_vec;

            for (int j = 0; j < query->n_counters; j++) {
                struct intel_counter *c = &q->counters[j];
                Gputop__GLCounter *counter = &counters[j];

                counter->id = c->id;
                counter->name = c->name;
                counter->description = c->description;
                counter->maximum = c->max_raw_value;

                switch (c->type) {
                case GL_PERFQUERY_COUNTER_EVENT_INTEL:
                    counter->type = GPUTOP__GLCOUNTER_TYPE__EVENT;
                    break;
                case GL_PERFQUERY_COUNTER_DURATION_NORM_INTEL:
                    counter->type = GPUTOP__GLCOUNTER_TYPE__DURATION_NORM;
                    break;
                case GL_PERFQUERY_COUNTER_DURATION_RAW_INTEL:
                    counter->type = GPUTOP__GLCOUNTER_TYPE__DURATION_RAW;
                    break;
                case GL_PERFQUERY_COUNTER_THROUGHPUT_INTEL:
                    counter->type = GPUTOP__GLCOUNTER_TYPE__THROUGHPUT;
                    break;
                case GL_PERFQUERY_COUNTER_RAW_INTEL:
                    counter->type = GPUTOP__GLCOUNTER_TYPE__RAW;
                    break;
                case GL_PERFQUERY_COUNTER_TIMESTAMP_INTEL:
                    counter->type = GPUTOP__GLCOUNTER_TYPE__TIMESTAMP;
                    break;
                }

                switch(c->data_type) {
                case GPUTOP_PERFQUERY_COUNTER_DATA_UINT32:
                    counter->data_type = GPUTOP__GLCOUNTER_DATA_TYPE__UINT32;
                    break;
                case GPUTOP_PERFQUERY_COUNTER_DATA_UINT64:
                    counter->data_type = GPUTOP__GLCOUNTER_DATA_TYPE__UINT64;
                    break;
                case GPUTOP_PERFQUERY_COUNTER_DATA_FLOAT:
                    counter->data_type = GPUTOP__GLCOUNTER_DATA_TYPE__FLOAT;
                    break;
                case GPUTOP_PERFQUERY_COUNTER_DATA_DOUBLE:
                    counter->data_type = GPUTOP__GLCOUNTER_DATA_TYPE__DOUBLE;
                    break;
                case GPUTOP_PERFQUERY_COUNTER_DATA_BOOL32:
                    counter->data_type = GPUTOP__GLCOUNTER_DATA_TYPE__BOOL32;
                    break;
                }

                counter->data_offset = c->data_offset;

                counters_vec[j] = counter;
            }

            query->data_size = q->max_counter_data_len;

            queries_vec[i] = query;
            i++;
        }
    }

    pthread_rwlock_unlock(&gputop_gl_lock);

    *n_queries_ret = n_queries;

    return queries_vec;
}

static void
free_gl_query_info(Gputop__GLQueryInfo **queries, int n_queries)
{
    for (int i = 0; i < n_queries; i++) {
        Gputop__GLQueryInfo *query = queries[i];

        for (int j = 0; j < query->n_counters; j++)
            free(query->counters[j]);

        free(query->counters);
        free(query);
    }

    free(queries);
}
#endif

static void
handle_get_features(h2o_websocket_conn_t *conn,
                    Gputop__Request *request)
{
    char kernel_release[128];
    char kernel_version[256];
    char cpu_model[128];
    Gputop__Message message = GPUTOP__MESSAGE__INIT;
    Gputop__Features features = GPUTOP__FEATURES__INIT;
    Gputop__DevInfo devinfo = GPUTOP__DEV_INFO__INIT;
    char *notices[] = {
        "RC6 power saving mode disabled"
    };

    if (!gputop_perf_initialize()) {
        message.reply_uuid = request->uuid;
        message.cmd_case = GPUTOP__MESSAGE__CMD_ERROR;
        message.error = "Failed to initialize perf\n";
        send_pb_message(conn, &message.base);
        return;
    }

    features.server_pid = getpid();

    devinfo.devid = gputop_devinfo.devid;
    devinfo.gen = gputop_devinfo.gen;
    devinfo.n_eus = gputop_devinfo.n_eus;
    devinfo.n_eu_slices = gputop_devinfo.n_eu_slices;
    devinfo.n_eu_sub_slices = gputop_devinfo.n_eu_sub_slices;
    devinfo.eu_threads_count = gputop_devinfo.eu_threads_count;
    devinfo.subslice_mask = gputop_devinfo.subslice_mask;
    devinfo.slice_mask = gputop_devinfo.slice_mask;
    devinfo.timestamp_frequency = gputop_devinfo.timestamp_frequency;
    devinfo.gt_min_freq = gputop_devinfo.gt_min_freq;
    devinfo.gt_max_freq = gputop_devinfo.gt_max_freq;

    if (IS_HASWELL(devinfo.devid)) {
        devinfo.devname = "hsw";
    } else if (IS_BROADWELL(devinfo.devid)) {
        devinfo.devname = "bdw";
    } else if (IS_CHERRYVIEW(devinfo.devid)) {
        devinfo.devname = "chv";
    } else if (IS_SKYLAKE(devinfo.devid)) {
        devinfo.devname = "skl";
    }

    features.fake_mode = gputop_fake_mode;

    features.devinfo = &devinfo;

#ifdef SUPPORT_GL
    features.has_gl_performance_query = gputop_gl_has_intel_performance_query_ext;

    if (gputop_gl_has_intel_performance_query_ext && !gputop_fake_mode) {
        int n_gl_queries;
        features.gl_queries = get_gl_query_info(&n_gl_queries);
        features.n_gl_queries = n_gl_queries;
    }
#else
    features.has_gl_performance_query = false;
#endif
    features.has_i915_oa = true;

    features.n_cpus = gputop_cpu_count();

    gputop_cpu_model(cpu_model, sizeof(cpu_model));
    features.cpu_model = cpu_model;

    features.tracepoints = gputop_debugfs_get_tracepoint_names();
    if (features.tracepoints) {
        for (features.n_tracepoints = 0;
             features.tracepoints[features.n_tracepoints];
             features.n_tracepoints++) {
        }
    }

    gputop_read_file("/proc/sys/kernel/osrelease", kernel_release, sizeof(kernel_release));
    gputop_read_file("/proc/sys/kernel/version", kernel_version, sizeof(kernel_version));
    features.kernel_release = kernel_release;
    features.kernel_build = kernel_version;
    features.n_supported_oa_query_guids = gputop_perf_oa_supported_metric_set_guids->len;
    features.supported_oa_query_guids = gputop_perf_oa_supported_metric_set_guids->data;

    features.n_notices = ARRAY_SIZE(notices);
    features.notices = notices;

    message.reply_uuid = request->uuid;
    message.cmd_case = GPUTOP__MESSAGE__CMD_FEATURES;
    message.features = &features;

    dbg("GPU:\n");
    dbg("  Device ID = 0x%x\n", devinfo.devid);
    dbg("  Gen = %"PRIu32"\n", devinfo.gen);
    dbg("  EU Slice Count = %"PRIu64"\n", devinfo.n_eu_slices);
    dbg("  EU Sub Slice Count = %"PRIu64"\n", devinfo.n_eu_sub_slices);
    dbg("  EU Count (total) = %"PRIu64"\n", devinfo.n_eus);
    dbg("  EU Threads Count (total) = %"PRIu64"\n", devinfo.eu_threads_count);
    dbg("  Slice Mask = 0x%"PRIx64"\n", devinfo.slice_mask);
    dbg("  Sub Slice Mask = 0x%"PRIx64"\n", devinfo.subslice_mask);
    dbg("  OA Metrics Available = %s\n", features.has_i915_oa ? "true" : "false");
    dbg("  Timestamp Frequency = %"PRIu64"\n", devinfo.timestamp_frequency);
    dbg("  Min Frequency = %"PRIu64"\n", devinfo.gt_min_freq);
    dbg("  Max Frequency = %"PRIu64"\n", devinfo.gt_max_freq);
    dbg("\n");
    dbg("CPU:\n");
    dbg("  Model = %s\n", features.cpu_model);
    dbg("  Core Count = %u\n", features.n_cpus);
    dbg("\n");
    dbg("SYSTEM:\n");
    dbg("  Kernel Release = %s\n", features.kernel_release);
    dbg("  Kernel Build = %s\n", features.kernel_build);
    dbg("NOTICES:\n");
    for (int i = 0; i< features.n_notices; i++) {
        const char *notice = features.notices[i];
        dbg("  %s\n", notice);
    }

    send_pb_message(conn, &message.base);

    gputop_debugfs_free_tracepoint_names(features.tracepoints);

#ifdef SUPPORT_GL
    if (features.n_gl_queries)
        free_gl_query_info(features.gl_queries, features.n_gl_queries);
#endif
}

static void on_ws_message(h2o_websocket_conn_t *conn,
                          const struct wslay_event_on_msg_recv_arg *arg)
{
    //fprintf(stderr, "on_ws_message\n");
    //dbg("on_ws_message\n");

    if (arg == NULL) {
        //dbg("socket closed\n");
        h2o_conn = NULL;
        close_all_streams();
        h2o_websocket_close(conn);
        return;
    }

    if (!wslay_is_ctrl_frame(arg->opcode)) {
        Gputop__Request *request =
            (void *)protobuf_c_message_unpack(&gputop__request__descriptor,
                                              NULL, /* default allocator */
                                              arg->msg_length,
                                              arg->msg);

        if (!request) {
            fprintf(stderr, "Failed to unpack message\n");
            dbg("Failed to unpack message\n");
            return;
        }

        switch (request->req_case) {
        case GPUTOP__REQUEST__REQ_GET_TRACEPOINT_INFO:
            fprintf(stderr, "GetTracepointInfo request received\n");
            handle_get_tracepoint_info(conn, request);
            break;
        case GPUTOP__REQUEST__REQ_GET_PROCESS_INFO:
            fprintf(stderr, "GetProcessInfo request received\n");
            handle_get_process_info(conn, request);
            break;
        case GPUTOP__REQUEST__REQ_GET_FEATURES:
            fprintf(stderr, "GetFeatures request received\n");
            handle_get_features(conn, request);
            break;
        case GPUTOP__REQUEST__REQ_OPEN_QUERY:
            fprintf(stderr, "OpenQuery request received\n");
            handle_open_query(conn, request);
            break;
        case GPUTOP__REQUEST__REQ_CLOSE_QUERY:
            fprintf(stderr, "CloseQuery request received\n");
            handle_close_query(conn, request);
            break;
        case GPUTOP__REQUEST__REQ_TEST_LOG:
            fprintf(stderr, "TEST LOG: %s\n", request->test_log);
            break;
        case GPUTOP__REQUEST__REQ__NOT_SET:
            assert(0);
        }

        free(request);
    }
}

static int on_req(h2o_handler_t *self, h2o_req_t *req)
{
    const char *client_key;
    ssize_t proto_header_index;

    //dbg("on_req\n");

    if (h2o_is_websocket_handshake(req, &client_key) != 0 || client_key == NULL) {
        return -1;
    }

    proto_header_index = h2o_find_header_by_str(&req->headers,
                                                "sec-websocket-protocol",
                                                strlen("sec-websocket-protocol"),
                                                SIZE_MAX);
    if (proto_header_index != -1) {
        //dbg("sec-websocket-protocols found\n");
        h2o_add_header_by_str(&req->pool, &req->res.headers,
                              "sec-websocket-protocol",
                              strlen("sec-websocket-protocol"),
                              0, "binary", strlen("binary"));
    }

    h2o_conn = h2o_upgrade_to_websocket(req, client_key, NULL, on_ws_message);

    return 0;
}

static void on_connect(uv_stream_t *server, int status)
{
    uv_tcp_t *conn;
    h2o_socket_t *sock;

    //dbg("on_connect\n");

    if (status != 0)
        return;

    signal(SIGPIPE, SIG_IGN);

    conn = h2o_mem_alloc(sizeof(*conn));
    uv_tcp_init(server->loop, conn);
    if (uv_accept(server, (uv_stream_t *)conn) != 0) {
        uv_close((uv_handle_t *)conn, (uv_close_cb)free);
        return;
    }

    sock = h2o_uv_socket_create((uv_stream_t *)conn, NULL, 0, (uv_close_cb)free);
    if (ssl_ctx != NULL)
        h2o_accept_ssl(&ctx, ctx.globalconf->hosts, sock, ssl_ctx);
    else
        h2o_http1_accept(&ctx, ctx.globalconf->hosts, sock);

    uv_timer_start(&timer, periodic_update_cb, 200, 200);
}

static h2o_iovec_t cache_control;
static h2o_headers_command_t uncache_cmd[2];

bool gputop_server_run(void)
{
    uv_loop_t *loop;
    struct sockaddr_in sockaddr;
    h2o_hostconf_t *hostconf;
    h2o_pathconf_t *pathconf;
    h2o_pathconf_t *root;
    const char *web_root;
    int r;
    char *port_env;
    unsigned long port;

    gputop_list_init(&streams);
    gputop_list_init(&closing_streams);

    loop = gputop_mainloop;

    uv_timer_init(gputop_mainloop, &timer);
    uv_idle_init(gputop_mainloop, &update_idle);

    if ((r = uv_tcp_init(loop, &listener)) != 0) {
        dbg("uv_tcp_init:%s\n", uv_strerror(r));
        goto error;
    }
    port_env = getenv("GPUTOP_PORT");
    if (!port_env)
        port_env = "7890";
    port = strtoul(port_env, NULL, 10);

    uv_ip4_addr("0.0.0.0", port, &sockaddr);
    if ((r = uv_tcp_bind(&listener, (struct sockaddr *)&sockaddr, sizeof(sockaddr))) != 0) {
        dbg("uv_tcp_bind:%s\n", uv_strerror(r));
        goto error;
    }
    if ((r = uv_listen((uv_stream_t *)&listener, 128, on_connect)) != 0) {
        dbg("uv_listen:%s\n", uv_strerror(r));
        goto error;
    }
    printf("Web server listening on port %lu\n", port);


    h2o_config_init(&config);
    hostconf = h2o_config_register_host(&config, h2o_iovec_init(H2O_STRLIT("default")), 7890);
    pathconf = h2o_config_register_path(hostconf, "/gputop");
    h2o_create_handler(pathconf, sizeof(h2o_handler_t))->on_req = on_req;

    /* Without the web ui enabled we still support remote access to metrics via
     * a websocket + protocol buffers, we just don't host the web ui assets.
     */
#ifdef ENABLE_WEBUI
    root = h2o_config_register_path(hostconf, "/");

    web_root = getenv("GPUTOP_WEB_ROOT");
    if (!web_root)
            web_root = GPUTOP_WEB_ROOT;

    h2o_file_register(root, web_root, NULL, NULL, 0);

    cache_control = h2o_iovec_init(H2O_STRLIT("Cache-Control"));
    uncache_cmd[0].cmd = H2O_HEADERS_CMD_APPEND;
    uncache_cmd[0].name = &cache_control;
    uncache_cmd[0].value.base = "no-store";
    uncache_cmd[0].value.len = strlen("no-store");
    uncache_cmd[1].cmd = H2O_HEADERS_CMD_NULL;

    h2o_headers_register(root, uncache_cmd);
#endif

    h2o_context_init(&ctx, loop, &config);

    /* disabled by default: uncomment the block below to use HTTPS instead of HTTP */
    /*
    if (setup_ssl("server.crt", "server.key") != 0)
        goto Error;
    */

    return true;

error:
    return false;
}
