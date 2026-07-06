/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/ImageBitmapRenderingContext.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/CanvasRenderingContext2D.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/ImageBitmapRenderingContext.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(ImageBitmapRenderingContext);

// https://html.spec.whatwg.org/multipage/canvas.html#dom-canvas-getcontext
JS::ThrowCompletionOr<GC::Ref<ImageBitmapRenderingContext>> ImageBitmapRenderingContext::create(JS::Realm& realm, HTMLCanvasElement& canvas, JS::Value options)
{
    auto internal_2d_context = TRY(CanvasRenderingContext2D::create(realm, canvas, options));
    return realm.create<ImageBitmapRenderingContext>(realm, canvas, internal_2d_context);
}

ImageBitmapRenderingContext::ImageBitmapRenderingContext(JS::Realm& realm, HTMLCanvasElement& canvas, GC::Ref<CanvasRenderingContext2D> internal_2d_context)
    : Bindings::PlatformObject(realm)
    , m_canvas(canvas)
    , m_internal_2d_context(internal_2d_context)
{
}

void ImageBitmapRenderingContext::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ImageBitmapRenderingContext);
}

void ImageBitmapRenderingContext::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_canvas);
    visitor.visit(m_internal_2d_context);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-imagebitmaprenderingcontext-transferfromimagebitmap
// FIXME: This does not sever bitmap's own reference to its bitmap data (the spec requires bitmap to become
//        unusable afterwards) and does not resize the canvas to match bitmap's dimensions if they differ - it
//        clears the canvas and draws the whole bitmap at (0,0), which matches the common case where the
//        canvas was already sized to match (e.g. Flutter's CanvasKit renderer sets canvas.width/height from
//        the ImageBitmap's own dimensions immediately before calling this).
WebIDL::ExceptionOr<void> ImageBitmapRenderingContext::transfer_from_image_bitmap(GC::Ptr<ImageBitmap> bitmap)
{
    auto canvas_width = static_cast<float>(m_canvas->width());
    auto canvas_height = static_cast<float>(m_canvas->height());

    // "Sets the canvas's bitmap to bitmap" / "If bitmap is null, the canvas's bitmap is set to transparent black."
    m_internal_2d_context->clear_rect(0, 0, canvas_width, canvas_height);

    if (bitmap)
        TRY(m_internal_2d_context->draw_image(GC::Ref { *bitmap }, 0, 0));

    m_canvas->set_canvas_content_dirty();
    return {};
}

}
