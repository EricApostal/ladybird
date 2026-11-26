/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/VM.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/HTML/ExtensionHooks.h>

namespace Web::HTML {

String ExtensionHooks::resource_type_for(Fetch::Infrastructure::Request const& request)
{
    using Destination = Fetch::Infrastructure::Request::Destination;
    using InitiatorType = Fetch::Infrastructure::Request::InitiatorType;

    if (request.destination().has_value()) {
        switch (*request.destination()) {
        case Destination::Document:
            return "main_frame"_string;
        case Destination::Frame:
        case Destination::IFrame:
            return "sub_frame"_string;
        case Destination::Script:
        case Destination::AudioWorklet:
        case Destination::PaintWorklet:
        case Destination::ServiceWorker:
        case Destination::SharedWorker:
        case Destination::Worker:
            return "script"_string;
        case Destination::Style:
        case Destination::XSLT:
            return "stylesheet"_string;
        case Destination::Image:
            return "image"_string;
        case Destination::Font:
            return "font"_string;
        case Destination::Audio:
        case Destination::Video:
        case Destination::Track:
            return "media"_string;
        case Destination::Object:
        case Destination::Embed:
            return "object"_string;
        case Destination::Report:
            return "csp_report"_string;
        default:
            return "other"_string;
        }
    }

    // A request with no destination was made by script, or is a beacon.
    if (request.initiator_type().has_value()) {
        switch (*request.initiator_type()) {
        case InitiatorType::Fetch:
        case InitiatorType::XMLHttpRequest:
            return "xmlhttprequest"_string;
        case InitiatorType::Beacon:
        case InitiatorType::Ping:
            return "ping"_string;
        default:
            break;
        }
    }
    return "other"_string;
}

static ExtensionHooks* s_extension_hooks = nullptr;

ExtensionHooks* ExtensionHooks::the()
{
    return s_extension_hooks;
}

void ExtensionHooks::install(ExtensionHooks* hooks)
{
    s_extension_hooks = hooks;
}

Optional<String> ExtensionHooks::content_script_extension_running()
{
    auto* hooks = the();
    if (!hooks)
        return {};
    auto& vm = JS::VM::the();
    if (!vm.has_running_execution_context())
        return {};
    auto* realm = vm.current_realm();
    if (!realm)
        return {};
    return hooks->content_script_extension(*realm);
}

bool ExtensionHooks::is_content_script_reading_its_own_extension(Fetch::Infrastructure::Request const& request)
{
    auto const& extension = request.content_script_extension();
    auto const& url = request.current_url();
    return extension.has_value() && url.scheme() == "moz-extension"sv && url.serialized_host() == extension->bytes_as_string_view();
}

}
