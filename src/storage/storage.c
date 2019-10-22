/**************************************************************************
 **
 ** sngrep - SIP Messages flow viewer
 **
 ** Copyright (C) 2013-2019 Ivan Alonso (Kaian)
 ** Copyright (C) 2013-2019 Irontec SL. All rights reserved.
 **
 ** This program is free software: you can redistribute it and/or modify
 ** it under the terms of the GNU General Public License as published by
 ** the Free Software Foundation, either version 3 of the License, or
 ** (at your option) any later version.
 **
 ** This program is distributed in the hope that it will be useful,
 ** but WITHOUT ANY WARRANTY; without even the implied warranty of
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 ** GNU General Public License for more details.
 **
 ** You should have received a copy of the GNU General Public License
 ** along with this program.  If not, see <http://www.gnu.org/licenses/>.
 **
 ****************************************************************************/
/**
 * @file storage.c
 * @author Ivan Alonso [aka Kaian] <kaian@irontec.com>
 *
 * @brief Source of functions defined in storage.h
 *
 *
 * Storage is in charge of analyze and handle the captured packets.
 *
 *  - Checks if packets match the non-capture filtering conditions
 *  - Sorts and manage calls lists
 *  - Sends interesting packets to capture outputs
 *  - Manages how packets are stored
 *
 *             +--------------------------+
 *             |                          |
 *        +--->|      User Interface      |
 *        |    |                          |
 *        |    +--------------------------+
 *        |    +--------------------------+
 *        |    |                          |
 *        +--->|         Storage          | <----------- You are here.
 *        +--->|                          |----+
 * Packet |    +--------------------------+    |
 * Queue  |    +--------------------------+    |
 *        +----|         Parser           |    |
 *             |--------------------------|    | Capture
 *             |                          |    | Output
 *             |     Capture Manager      |<---+
 *             |                          |
 *             +--------------------------+
 *
 */
#include "config.h"
#include <glib.h>
#include <glib/gasyncqueuesource.h>
#include "glib/glib-extra.h"
#include "parser/packet_sip.h"
#include "setting.h"
#include "filter.h"
#include "storage.h"

/**
 * @brief Global Structure with all storage information
 */
static Storage *storage;

static gint
storage_call_attr_sorter(const Call **a, const Call **b)
{
    gint cmp = call_attr_compare(*a, *b, storage->options.sort.by);
    return (storage->options.sort.asc) ? cmp : cmp * -1;
}

gboolean
storage_calls_changed()
{
    gboolean changed = storage->changed;
    storage->changed = FALSE;
    return changed;
}

guint
storage_calls_count()
{
    return g_ptr_array_len(storage->calls);
}

gboolean
storage_limit_reached()
{
    return g_ptr_array_len(storage->calls) >= storage->options.capture.limit;
}

GPtrArray *
storage_calls()
{
    return storage->calls;
}

StorageStats
storage_calls_stats()
{
    StorageStats stats = { 0 };

    // Total number of calls without filtering
    stats.total = g_ptr_array_len(storage->calls);

    // Total number of calls after filtering
    for (guint i = 0; i < g_ptr_array_len(storage->calls); i++) {
        Call *call = g_ptr_array_index(storage->calls, i);
        if (call->filtered != 1)
            stats.displayed++;
    }

    return stats;
}

void
storage_calls_clear()
{
    // Create again the callid hash table
    g_hash_table_remove_all(storage->callids);
    g_hash_table_remove_all(storage->streams);

    // Remove all items from vector
    g_ptr_array_remove_all(storage->calls);
}

void
storage_calls_clear_soft()
{
    // Filter current call list
    for (guint i = 0; i < g_ptr_array_len(storage->calls); i++) {
        Call *call = g_ptr_array_index(storage->calls, i);

        // Filtered call, remove from all lists
        if (filter_check_call(call, NULL)) {
            // Remove from Call-Id hash table
            g_hash_table_remove(storage->callids, call->callid);
            // Remove from calls list
            g_ptr_array_remove(storage->calls, call);
        }
    }
}

static void
storage_calls_rotate()
{
    for (guint i = 0; i < g_ptr_array_len(storage->calls); i++) {
        Call *call = g_ptr_array_index(storage->calls, i);

        if (!call->locked) {
            // Remove from callids hash
            g_hash_table_remove(storage->callids, call->callid);
            // Remove first call from active and call lists
            g_ptr_array_remove(storage->calls, call);
            return;
        }
    }
}

static gboolean
storage_check_match_expr(const char *payload)
{
    // Everything matches when there is no match
    if (storage->options.match.mexpr == NULL)
        return 1;

    // Check if payload matches the given expresion
    if (g_regex_match(storage->options.match.mregex, payload, 0, NULL)) {
        return 0 == storage->options.match.minvert;
    } else {
        return 1 == storage->options.match.minvert;
    }

}

StorageMatchOpts
storage_match_options()
{
    return storage->options.match;
}

void
storage_set_sort_options(StorageSortOpts sort)
{
    storage->options.sort = sort;
    g_ptr_array_sort(storage->calls, (GCompareFunc) storage_call_attr_sorter);
}

StorageSortOpts
storage_sort_options()
{
    return storage->options.sort;
}

StorageCaptureOpts
storage_capture_options()
{
    return storage->options.capture;
}

static gchar *
storage_stream_key(const gchar *dstip, guint16 dport)
{
    return g_strdup_printf(
        "%s:%u",
        (dstip) ? dstip : "", dport
    );
}

static void
storage_register_stream(gchar *hashkey, Message *msg)
{
    g_hash_table_remove(storage->streams, hashkey);
    g_hash_table_insert(storage->streams, hashkey, msg);
}

/**
 * @brief Parse SIP Message payload for SDP media streams
 *
 * Parse the payload content to get SDP information
 *
 * @param msg SIP message structure
 * @return 0 in all cases
 */
static void
storage_register_streams(Message *msg)
{
    Packet *packet = msg->packet;

    PacketSdpData *sdp = packet_sdp_data(packet);
    if (sdp == NULL) {
        // Packet without SDP content
        return;
    }

    for (guint i = 0; i < g_list_length(sdp->medias); i++) {
        PacketSdpMedia *media = g_list_nth_data(sdp->medias, i);

        if (media->address == NULL)
            continue;

        // Create RTP stream for this media
        storage_register_stream(
            storage_stream_key(media->address->ip, media->address->port),
            msg
        );

        // Create RTCP stream for this media
        storage_register_stream(
            storage_stream_key(media->address->ip, media->address->port + 1),
            msg
        );

        // Create RTP stream with source of message as destination address
        if (!address_equals(media->address, msg->src)) {
            storage_register_stream(
                storage_stream_key(msg->src->ip, media->address->port),
                msg
            );
        }
    }
}

static Message *
storage_check_sip_packet(Packet *packet)
{
    Call *call = NULL;
    gboolean newcall = FALSE;

    PacketSipData *sip_data = g_ptr_array_index(packet->proto, PACKET_SIP);

    // Find the call for this msg
    if (!(call = g_hash_table_lookup(storage->callids, sip_data->callid))) {

        // Check if payload matches expression
        if (!storage_check_match_expr(sip_data->payload))
            return NULL;

        // User requested only INVITE starting dialogs
        if (storage->options.match.invite && sip_data->code.id != SIP_METHOD_INVITE)
            return NULL;

        // Only create a new call if the first msg
        // is a request message in the following gorup
        if (storage->options.match.complete && sip_data->code.id > SIP_METHOD_MESSAGE)
            return NULL;

        // Rotate call list if limit has been reached
        if (storage->options.capture.limit == storage_calls_count())
            storage_calls_rotate();

        // Create the call if not found
        if ((call = call_create(sip_data->callid, sip_data->xcallid)) == NULL)
            return NULL;

        // Add this Call-Id to hash table
        g_hash_table_insert(storage->callids, g_strdup(call->callid), call);

        // Set call index
        call->index = ++storage->last_index;

        // Mark this as a new call
        newcall = TRUE;
    }

    // At this point we know we're handling an interesting SIP Packet
    Message *msg = msg_new(packet);

    // Always dissect first call message
    if (call_msg_count(call) == 0) {
        // If this call has X-Call-Id, append it to the parent call
        if (call->xcallid) {
            call_add_xcall(g_hash_table_lookup(storage->callids, call->xcallid), call);
        }
    }

    // Add the message to the call
    call_add_message(call, msg);

    // Parse media data
    storage_register_streams(msg);

    if (call_is_invite(call)) {
        // Update Call State
        call_update_state(call, msg);
    }

    if (newcall) {
        // Append this call to the call list
        g_ptr_array_add(storage->calls, call);
    }

    // Mark the list as changed
    storage->changed = TRUE;

    // Send this packet to all capture outputs
    capture_manager_output_packet(capture_manager_get_instance(), packet);

    // Return the loaded message
    return msg;
}

static Stream *
storage_check_rtp_packet(Packet *packet)
{
    PacketRtpData *rtp = g_ptr_array_index(packet->proto, PACKET_RTP);
    g_return_val_if_fail(rtp != NULL, NULL);

    // Get Addresses from packet
    Address *src = packet_src_address(packet);
    Address *dst = packet_dst_address(packet);

    // Find the stream by destination
    g_autofree gchar *hashkey = storage_stream_key(dst->ip, dst->port);
    Message *msg = g_hash_table_lookup(storage->streams, hashkey);

    // No call has setup this stream
    if (msg == NULL)
        return NULL;

    // Mark call as changed
    Call *call = msg_get_call(msg);

    // Find a matching stream in the call
    Stream *stream = call_find_stream(call, src, dst, rtp->encoding->id);

    // If no stream matches this packet, create a new stream for this source
    if (stream == NULL) {
        stream = stream_new(STREAM_RTP, msg, msg_media_for_addr(msg, dst));
        stream_set_data(stream, src, dst);
        stream_set_format(stream, rtp->encoding->id);
        call_add_stream(call, stream);
    }

    // Add packet to existing stream
    stream_add_packet(stream, packet);

    // Store packet if rtp capture is enabled
    if (storage->options.capture.rtp) {
        g_ptr_array_add(stream->packets, packet_ref(packet));
    }

    // Mark the list as changed
    storage->changed = TRUE;

    return stream;
}

static Stream *
storage_check_rtcp_packet(Packet *packet)
{
    PacketRtcpData *rtcp = g_ptr_array_index(packet->proto, PACKET_RTP);
    g_return_val_if_fail(rtcp != NULL, NULL);

    // Get Addresses from packet
    Address *src = packet_src_address(packet);
    Address *dst = packet_dst_address(packet);

    // Find the stream by destination
    g_autofree gchar *hashkey = storage_stream_key(dst->ip, dst->port);
    Message *msg = g_hash_table_lookup(storage->streams, hashkey);

    // No call has setup this stream
    if (msg == NULL)
        return NULL;

    // Mark call as changed
    Call *call = msg_get_call(msg);

    // Find a matching stream in the call
    Stream *stream = call_find_stream(call, src, dst, 0);

    // If no stream matches this packet, create a new stream for this source
    if (stream == NULL) {
        stream = stream_new(STREAM_RTCP, msg, msg_media_for_addr(msg, dst));
        stream_set_data(stream, src, dst);
        call_add_stream(call, stream);
    }

    // Add packet to existing stream
    stream_add_packet(stream, packet);

    // Store packet if rtp capture is enabled
    if (storage->options.capture.rtp) {
        g_ptr_array_add(stream->packets, packet_ref(packet));
    }

    // Mark the list as changed
    storage->changed = TRUE;

    return stream;
}

void
storage_add_packet(Packet *packet)
{
    // Add the packet to the queue
    g_async_queue_push(storage->queue, (gpointer) packet_ref(packet));
}

//! Start capturing packets function
static gboolean
storage_check_packet(Packet *packet, G_GNUC_UNUSED gpointer user_data)
{
    if (packet_has_type(packet, PACKET_SIP)) {
        storage_check_sip_packet(packet);
    } else if (packet_has_type(packet, PACKET_RTP)) {
        storage_check_rtp_packet(packet);
    } else if (packet_has_type(packet, PACKET_RTCP)) {
        storage_check_rtcp_packet(packet);
    }

    // Remove packet reference after parsing (added in storage_add_packet)
    packet_unref(packet);
    return TRUE;
}

gint
storage_pending_packets()
{
    return g_async_queue_length(storage->queue);
}

void
storage_free(Storage *storage)
{
    // Remove all calls
    storage_calls_clear();
    // Remove storage pending packets queue
    g_async_queue_unref(storage->queue);
    // Remove Call-id hash table
    g_hash_table_destroy(storage->callids);
}

Storage *
storage_new(StorageOpts options, GError **error)
{
    GRegexCompileFlags cflags = G_REGEX_EXTENDED;
    GRegexMatchFlags mflags = G_REGEX_MATCH_NEWLINE_CRLF;

    storage = g_malloc0(sizeof(Storage));
    storage->options = options;

    // Store capture limit
    storage->last_index = 0;

    // Validate match expression
    if (storage->options.match.mexpr) {
        // Case insensitive requested
        if (storage->options.match.micase) {
            cflags |= G_REGEX_CASELESS;
        }

        // Check the expresion is a compilable regexp
        storage->options.match.mregex = g_regex_new(storage->options.match.mexpr, cflags, mflags, error);
        if (storage->options.match.mregex == NULL) {
            return FALSE;
        }
    }

    // Create a vector to store calls
    storage->calls = g_ptr_array_new_with_free_func(call_destroy);

    // Create hash tables for fast call and stream search
    storage->callids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    storage->streams = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    // Set default sorting field
    if (attr_find_by_name(setting_get_value(SETTING_CL_SORTFIELD)) >= 0) {
        storage->options.sort.by = attr_find_by_name(setting_get_value(SETTING_CL_SORTFIELD));
        storage->options.sort.asc = (!strcmp(setting_get_value(SETTING_CL_SORTORDER), "asc"));
    } else {
        // Fallback to default sorting field
        storage->options.sort.by = ATTR_CALLINDEX;
        storage->options.sort.asc = TRUE;
    }

    // Parsed packet to check
    storage->queue = g_async_queue_new();

    // Storage check source
    GSource *source = g_async_queue_source_new(storage->queue, NULL);
    g_source_set_callback(source, (GSourceFunc) G_CALLBACK(storage_check_packet), NULL, NULL);
    g_source_attach(source, NULL);

    return storage;
}