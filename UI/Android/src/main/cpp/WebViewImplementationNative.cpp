/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WebViewImplementationNative.h"
#include "JNIHelpers.h"
#include <AK/Debug.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Painter.h>
#include <LibGfx/SharedImageBuffer.h>
#include <android/bitmap.h>
#include <jni.h>

namespace Ladybird {

static Gfx::BitmapFormat to_gfx_bitmap_format(i32 f)
{
    switch (f) {
    case ANDROID_BITMAP_FORMAT_RGBA_8888:
        return Gfx::BitmapFormat::RGBA8888;
    default:
        VERIFY_NOT_REACHED();
    }
}

WebViewImplementationNative::WebViewImplementationNative(jobject thiz)
    : m_java_instance(thiz)
{
    // NOTE: m_java_instance's global ref is controlled by the JNI bindings

    on_ready_to_paint = [this]() {
        ++m_ready_to_paint_count;
        dbgln("[AndroidWebView] on_ready_to_paint #{} usable_bitmap={} front_id={} back_id={} front_size={}x{} viewport={}x{}",
            m_ready_to_paint_count,
            m_client_state.has_usable_bitmap,
            m_client_state.front_bitmap.id,
            m_client_state.back_bitmap.id,
            m_client_state.front_bitmap.last_painted_size.width().value(),
            m_client_state.front_bitmap.last_painted_size.height().value(),
            m_viewport_size.width().value(),
            m_viewport_size.height().value());
        JavaEnvironment env(global_vm);
        env.get()->CallVoidMethod(m_java_instance, invalidate_layout_method);
    };

    on_load_start = [this](URL::URL const& url, bool is_redirect) {
        dbgln("[AndroidWebView] on_load_start url={} redirect={}", url, is_redirect);
        JavaEnvironment env(global_vm);
        auto url_string = env.jstring_from_ak_string(url.to_string());
        env.get()->CallVoidMethod(m_java_instance, on_load_start_method, url_string, is_redirect);
        env.get()->DeleteLocalRef(url_string);
    };

    dbgln("[AndroidWebView] WebViewImplementationNative created");

    m_viewport_size = { 1080, 2188 };
    m_device_pixel_ratio = 2.625;

    initialize_client(CreateNewClient::Yes);
    set_system_visibility_state(Web::HTML::VisibilityState::Visible);
}

void WebViewImplementationNative::paint_into_bitmap(void* android_bitmap_raw, AndroidBitmapInfo const& info)
{
    // Software bitmaps only for now!
    VERIFY((info.flags & ANDROID_BITMAP_FLAGS_IS_HARDWARE) == 0);

    auto android_bitmap = MUST(Gfx::Bitmap::create_wrapper(to_gfx_bitmap_format(info.format), Gfx::AlphaType::Premultiplied, { info.width, info.height }, info.stride, android_bitmap_raw));
    auto painter = Gfx::Painter::create(android_bitmap);

    ++m_paint_call_count;

    if (m_client_state.has_usable_bitmap && m_client_state.front_bitmap.shared_image_buffer) {
        if (!m_logged_first_usable_frame) {
            m_logged_first_usable_frame = true;
            dbgln("[AndroidWebView] first usable frame available at paint #{} front_id={} painted_size={}x{}",
                m_paint_call_count,
                m_client_state.front_bitmap.id,
                m_client_state.front_bitmap.last_painted_size.width().value(),
                m_client_state.front_bitmap.last_painted_size.height().value());
        }

        auto bitmap = m_client_state.front_bitmap.shared_image_buffer->bitmap();
        auto painted_rect = Gfx::IntRect { { }, m_client_state.front_bitmap.last_painted_size.to_type<int>() };
        painter->draw_bitmap(android_bitmap->rect().to_type<float>(), Gfx::DecodedImageFrame { *bitmap }, painted_rect, Gfx::ScalingMode::NearestNeighbor, { }, 1.0f, Gfx::CompositingAndBlendingOperator::Copy);

        if (m_paint_call_count <= 10 || (m_paint_call_count % 30) == 0) {
            dbgln("[AndroidWebView] paint #{} drew frame front_id={} target={}x{} source={}x{}",
                m_paint_call_count,
                m_client_state.front_bitmap.id,
                info.width,
                info.height,
                m_client_state.front_bitmap.last_painted_size.width().value(),
                m_client_state.front_bitmap.last_painted_size.height().value());
        }
    } else {
        if (!m_logged_waiting_for_first_frame) {
            m_logged_waiting_for_first_frame = true;
            dbgln("[AndroidWebView] waiting for first frame at paint #{} (has_usable_bitmap={}, front_buffer={})",
                m_paint_call_count,
                m_client_state.has_usable_bitmap,
                m_client_state.front_bitmap.shared_image_buffer ? "present" : "missing");
        }

        if (m_paint_call_count <= 10 || (m_paint_call_count % 30) == 0) {
            auto background = page_background_color();
            dbgln("[AndroidWebView] paint #{} drew fallback background color={} (no usable frame)",
                m_paint_call_count,
                background.to_byte_string());
        }

        painter->fill_rect(android_bitmap->rect().to_type<float>(), page_background_color());
    }
}

void WebViewImplementationNative::set_viewport_geometry(int w, int h)
{
    dbgln("[AndroidWebView] set_viewport_geometry {}x{} (old {}x{})", w, h, m_viewport_size.width().value(), m_viewport_size.height().value());
    m_viewport_size = { w, h };
    handle_resize();
}

void WebViewImplementationNative::set_device_pixel_ratio(double f)
{
    dbgln("[AndroidWebView] set_device_pixel_ratio {}", f);
    m_device_pixel_ratio = f;
    handle_resize();
}

void WebViewImplementationNative::mouse_event(Web::MouseEvent::Type event_type, float x, float y, float raw_x, float raw_y)
{
    Gfx::IntPoint position = { x, y };
    Gfx::IntPoint screen_position = { raw_x, raw_y };
    auto event = Web::MouseEvent {
        event_type,
        position.to_type<Web::DevicePixels>(),
        screen_position.to_type<Web::DevicePixels>(),
        Web::UIEvents::MouseButton::Primary,
        Web::UIEvents::MouseButton::Primary,
        Web::UIEvents::KeyModifier::Mod_None,
        0,
        0,
        1,
        nullptr
    };

    enqueue_input_event(move(event));
}

}
