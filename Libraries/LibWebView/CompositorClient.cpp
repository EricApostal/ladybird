/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/CompositorClient.h>

#include <LibCore/EventLoop.h>
#include <LibWebView/WebContentClient.h>

namespace WebView {

CompositorClient::CompositorClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<CompositorControlClientEndpoint, CompositorControlServerEndpoint>(*this, move(transport))
{
}

void CompositorClient::die()
{
    if (auto callback = move(on_death)) {
        Core::deferred_invoke([callback = move(callback)]() mutable {
            callback();
        });
    }
}

void CompositorClient::did_allocate_backing_stores(Web::Compositor::CompositorContextId context_id, i32 front_bitmap_id, Gfx::SharedImage front_backing_store, i32 back_bitmap_id, Gfx::SharedImage back_backing_store)
{
    dbgln("[CompositorUI] did_allocate_backing_stores context={} front={} back={}", context_id, front_bitmap_id, back_bitmap_id);

    auto web_content_client = WebContentClient::client_for_compositor_context_id(context_id);
    if (!web_content_client.has_value()) {
        warnln("[CompositorUI] No WebContentClient mapped for context {} while allocating backing stores", context_id);
        return;
    }

    auto page_id = web_content_client->page_id_for_compositor_context_id(context_id);
    VERIFY(page_id.has_value());

    dbgln("[CompositorUI] Routing backing stores context {} -> page {}", context_id, *page_id);

    web_content_client->did_present_backing_stores(*page_id, front_bitmap_id, move(front_backing_store), back_bitmap_id, move(back_backing_store));
}

void CompositorClient::did_present_frame(Web::Compositor::CompositorContextId context_id, Gfx::IntRect content_rect, i32 bitmap_id)
{
    dbgln("[CompositorUI] did_present_frame context={} bitmap={} rect={}x{}@{},{}", context_id, bitmap_id, content_rect.width(), content_rect.height(), content_rect.x(), content_rect.y());

    auto web_content_client = WebContentClient::client_for_compositor_context_id(context_id);
    if (web_content_client.has_value()) {
        auto page_id = web_content_client->page_id_for_compositor_context_id(context_id);
        VERIFY(page_id.has_value());
        dbgln("[CompositorUI] Routing frame context {} -> page {}", context_id, *page_id);
        web_content_client->did_present_bitmap(*page_id, content_rect, bitmap_id);
        return;
    }

    warnln("[CompositorUI] No WebContentClient mapped for context {} while presenting bitmap {}, acking directly", context_id, bitmap_id);
    async_presented_bitmap_ready_to_paint(context_id, bitmap_id);
}

}
