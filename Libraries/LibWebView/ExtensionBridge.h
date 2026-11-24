/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/WeakPtr.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebView/Forward.h>

namespace WebView {

// The browser process's end of the channel that browser extensions use.
//
// Extensions run in web content processes, where a content script lives in a world of its own with a bridge to this
// process. The bridge carries opaque strings, and this class addresses them: it gives every context a number that is
// unique across processes, hands the embedder what the contexts say, and sends the embedder's replies back. What a
// message means is for the embedder to decide.
class WEBVIEW_API ExtensionBridge {
public:
    static ExtensionBridge& the();

    // A context is one extension's world in one document. `view_id` names the tab the document is in, which is the
    // identifier the embedder knows; a page ID only means something within one process.
    Function<void(u64 context_id, Optional<u64> view_id, String extension_uuid, String url, bool is_top_level)> on_context_created;
    Function<void(u64 context_id)> on_context_destroyed;
    Function<void(u64 context_id, String payload)> on_message;

    // A request that is about to be sent and waits to hear what the extensions want done with it. `details` is JSON.
    // The embedder answers with respond_to_intercept(). When it has no one to ask, the request goes ahead.
    Function<void(u64 request_id, Optional<u64> view_id, String details)> on_intercept;

    // Replaces the extension state that every web content process holds, now and for each one started later. It is
    // JSON that the embedder and the processes agree on.
    void set_state(String const& json);

    // Returns false when the context's process is gone.
    bool send_to_context(u64 context_id, String const& payload);

    // Answers a request from on_intercept with what to do with it, as JSON.
    bool respond_to_intercept(u64 request_id, String const& verdict_json);

    // A file of an extension, as `moz-extension://<uuid>/<path>` names it.
    struct Resource {
        ByteBuffer data;
        i64 modified_time { 0 };
        ByteString path;
    };

    // Reads it. Only files inside the directory of an extension the embedder gave in the state can be read, however
    // the path is spelled.
    ErrorOr<Resource> read_resource(URL::URL const&) const;

    // What WebContentClient reports.
    void did_create_client(WebContentClient&);
    void client_did_close(WebContentClient&);
    void context_created(WebContentClient&, Web::PageId, u64 local_context_id, String extension_uuid, String url, bool is_top_level);
    void context_destroyed(WebContentClient&, u64 local_context_id);
    void message_from_context(WebContentClient&, u64 local_context_id, String payload);
    void intercept_requested(WebContentClient&, Web::PageId, u64 local_request_id, String details);

private:
    ExtensionBridge() = default;

    u32 serial_for(WebContentClient&);
    static u64 global_id(u32 serial, u64 local_context_id) { return (static_cast<u64>(serial) << 32) | (local_context_id & 0xFFFFFFFFu); }

    Optional<Core::AnonymousBuffer> m_state_buffer;
    // From the state: where extensions are unpacked, and which extension uuids exist.
    ByteString m_installed_root;
    HashTable<String> m_extension_uuids;
    u32 m_next_serial { 1 };
    HashMap<WebContentClient const*, u32> m_serials;
    HashMap<u32, WeakPtr<WebContentClient>> m_clients;
    HashMap<u32, HashTable<u64>> m_contexts_of_client;
};

}
