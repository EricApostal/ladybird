/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/StringBuilder.h>
#include <LibCore/File.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebView/ExtensionBridge.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

ExtensionBridge& ExtensionBridge::the()
{
    static auto* bridge = new ExtensionBridge;
    return *bridge;
}

u32 ExtensionBridge::serial_for(WebContentClient& client)
{
    if (auto existing = m_serials.get(&client); existing.has_value())
        return *existing;

    auto serial = m_next_serial++;
    m_serials.set(&client, serial);
    m_clients.set(serial, client.make_weak_ptr<WebContentClient>());
    return serial;
}

void ExtensionBridge::set_state(String const& json)
{
    auto buffer = Core::AnonymousBuffer::create_with_size(json.bytes().size());
    if (buffer.is_error()) {
        warnln("ExtensionBridge: could not allocate the extension state: {}", buffer.error());
        return;
    }

    json.bytes().copy_to(Bytes { buffer.value().data<u8>(), buffer.value().size() });
    m_state_buffer = buffer.release_value();

    // The processes read the whole state; this process only needs to know where the extensions' files are.
    m_installed_root = {};
    m_extension_uuids.clear();
    if (auto parsed = JsonValue::from_string(json); !parsed.is_error() && parsed.value().is_object()) {
        auto const& root = parsed.value().as_object();
        if (auto installed_root = root.get_string("installedRoot"sv); installed_root.has_value())
            m_installed_root = installed_root->to_byte_string();
        if (auto extensions = root.get_array("extensions"sv); extensions.has_value()) {
            extensions.value().for_each([&](JsonValue const& value) {
                if (!value.is_object())
                    return;
                if (auto uuid = value.as_object().get_string("uuid"sv); uuid.has_value())
                    m_extension_uuids.set(*uuid);
            });
        }
    }

    WebContentClient::for_each_client([&](WebContentClient& client) {
        client.async_set_extension_state(*m_state_buffer);
        return IterationDecision::Continue;
    });
}

bool ExtensionBridge::respond_to_intercept(u64 request_id, String const& verdict_json)
{
    auto serial = static_cast<u32>(request_id >> 32);
    auto local_request_id = request_id & 0xFFFFFFFFu;

    auto client = m_clients.find(serial);
    if (client == m_clients.end())
        return false;
    auto strong = client->value.strong_ref();
    if (!strong)
        return false;

    strong->async_extension_intercept_verdict(local_request_id, verdict_json);
    return true;
}

void ExtensionBridge::intercept_requested(WebContentClient& client, Web::PageId page_id, u64 local_request_id, String details)
{
    auto serial = serial_for(client);

    // With no one to ask, the request is not held up.
    if (!on_intercept) {
        client.async_extension_intercept_verdict(local_request_id, "{}"_string);
        return;
    }

    Optional<u64> view_id;
    if (auto* page = client.page(page_id))
        view_id = page->view().view_id();

    on_intercept(global_id(serial, local_request_id), view_id, move(details));
}

bool ExtensionBridge::send_to_context(u64 context_id, String const& payload)
{
    auto serial = static_cast<u32>(context_id >> 32);
    auto local_context_id = context_id & 0xFFFFFFFFu;

    auto client = m_clients.find(serial);
    if (client == m_clients.end())
        return false;
    auto strong = client->value.strong_ref();
    if (!strong)
        return false;

    strong->async_extension_message_to_context(local_context_id, payload);
    return true;
}

void ExtensionBridge::did_create_client(WebContentClient& client)
{
    // A client that was destroyed without closing may have left its address behind, and this one can be at the same
    // address. It is a different process and starts over.
    if (auto stale = m_serials.get(&client); stale.has_value()) {
        m_clients.remove(*stale);
        m_contexts_of_client.remove(*stale);
        m_serials.remove(&client);
    }

    serial_for(client);
    // A process that starts while there is no state has nothing to run, and installs nothing.
    if (m_state_buffer.has_value())
        client.async_set_extension_state(*m_state_buffer);
}

void ExtensionBridge::client_did_close(WebContentClient& client)
{
    auto serial = serial_for(client);

    if (auto contexts = m_contexts_of_client.take(serial); contexts.has_value()) {
        for (auto local_context_id : *contexts) {
            if (on_context_destroyed)
                on_context_destroyed(global_id(serial, local_context_id));
        }
    }

    m_clients.remove(serial);
    m_serials.remove(&client);
}

void ExtensionBridge::context_created(WebContentClient& client, Web::PageId page_id, u64 local_context_id, String extension_uuid, String url, bool is_top_level)
{
    auto serial = serial_for(client);
    m_contexts_of_client.ensure(serial).set(local_context_id);

    Optional<u64> view_id;
    if (auto* page = client.page(page_id))
        view_id = page->view().view_id();

    if (on_context_created)
        on_context_created(global_id(serial, local_context_id), view_id, move(extension_uuid), move(url), is_top_level);
}

void ExtensionBridge::context_destroyed(WebContentClient& client, u64 local_context_id)
{
    auto serial = serial_for(client);
    if (auto contexts = m_contexts_of_client.get(serial); contexts.has_value())
        contexts->remove(local_context_id);

    if (on_context_destroyed)
        on_context_destroyed(global_id(serial, local_context_id));
}

void ExtensionBridge::message_from_context(WebContentClient& client, u64 local_context_id, String payload)
{
    if (!on_message)
        return;

    // A process can only speak for contexts it announced, so a stray number cannot reach another process's context.
    auto serial = serial_for(client);
    auto contexts = m_contexts_of_client.get(serial);
    if (!contexts.has_value() || !contexts->contains(local_context_id))
        return;

    on_message(global_id(serial, local_context_id), move(payload));
}

ErrorOr<ExtensionBridge::Resource> ExtensionBridge::read_resource(URL::URL const& url) const
{
    if (url.scheme() != "moz-extension"sv || m_installed_root.is_empty())
        return Error::from_errno(EINVAL);

    auto uuid = TRY(String::from_utf8(url.serialized_host()));
    if (!m_extension_uuids.contains(uuid))
        return Error::from_errno(ENOENT);

    // The path as the extension wrote it, without a query or fragment. A name that climbs out of the extension's
    // directory, or is empty, is not a file of the extension.
    auto decoded = URL::percent_decode(url.serialize_path());
    StringBuilder relative;
    for (auto segment : decoded.view().split_view('/')) {
        if (segment.is_empty() || segment == "."sv)
            continue;
        if (segment == ".."sv || segment.contains('\0') || segment.contains('\\'))
            return Error::from_errno(EACCES);
        if (!relative.is_empty())
            relative.append('/');
        relative.append(segment);
    }
    if (relative.is_empty())
        return Error::from_errno(ENOENT);

    auto extension_directory = ByteString::formatted("{}/{}", m_installed_root, uuid);
    auto canonical_directory = TRY(FileSystem::real_path(extension_directory));
    auto canonical_path = TRY(FileSystem::real_path(ByteString::formatted("{}/{}", canonical_directory, relative.string_view())));

    // A symbolic link must not lead out of the extension's directory either.
    auto directory_prefix = ByteString::formatted("{}/", canonical_directory);
    if (!canonical_path.starts_with(directory_prefix.view()))
        return Error::from_errno(EACCES);

    auto file = TRY(Core::File::open(canonical_path, Core::File::OpenMode::Read));
    auto stat = TRY(file->stat());
    if (S_ISDIR(stat.st_mode))
        return Error::from_errno(EISDIR);

    return Resource { TRY(file->read_until_eof()), static_cast<i64>(stat.st_mtime), move(canonical_path) };
}

}
