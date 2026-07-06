/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#the-imagebitmaprenderingcontext-interface
// AD-HOC: This does not maintain its own canvas surface/backing store. It instead holds an internal (not
//         JS-exposed) CanvasRenderingContext2D and delegates all of HTMLCanvasElement's backing-store/painting
//         plumbing (canvas_id(), prepare_for_compositing(), etc.) to it, drawing the transferred ImageBitmap
//         into it via the existing Canvas2D drawImage() machinery. This reuses the entire existing
//         canvas-compositing pipeline instead of adding a new one.
class ImageBitmapRenderingContext : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(ImageBitmapRenderingContext, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(ImageBitmapRenderingContext);

public:
    static JS::ThrowCompletionOr<GC::Ref<ImageBitmapRenderingContext>> create(JS::Realm&, HTMLCanvasElement&, JS::Value options);

    GC::Ref<HTMLCanvasElement> canvas() const { return m_canvas; }

    WebIDL::ExceptionOr<void> transfer_from_image_bitmap(GC::Ptr<ImageBitmap>);

    // Not JS-exposed: HTMLCanvasElement delegates its context-agnostic backing-store/painting operations to
    // this context's internal CanvasRenderingContext2D when it is the active context.
    GC::Ref<CanvasRenderingContext2D> internal_2d_context() const { return m_internal_2d_context; }

private:
    ImageBitmapRenderingContext(JS::Realm&, HTMLCanvasElement&, GC::Ref<CanvasRenderingContext2D>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<HTMLCanvasElement> m_canvas;
    GC::Ref<CanvasRenderingContext2D> m_internal_2d_context;
};

}
