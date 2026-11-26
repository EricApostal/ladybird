/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/IndexedDB/Persistence.h>
#include <LibWeb/Page/Page.h>

namespace Web::IndexedDB {

static PersistenceHooks* s_hooks;

void PersistenceHooks::install(PersistenceHooks* hooks)
{
    s_hooks = hooks;
}

PersistenceHooks* PersistenceHooks::the()
{
    return s_hooks;
}

}

// What the Rust implementation of IndexedDB asks for in order to keep databases, declared on the Rust side in
// Libraries/LibWeb/Bindings/Rust/src/indexed_db/internal/persistence.rs.

using namespace Web;
using IndexedDB::PersistenceHooks;

extern "C" {

u8 web_indexed_db_load_stored_database(void* realm, ReadonlyBytes name, bool has_copy, void* context, void (*on_row)(void*, u8 const*, size_t, u8 const*, size_t));
void web_indexed_db_list_stored_databases(void* realm, void* context, void (*on_name)(void*, u8 const*, size_t));
PersistenceHooks::WriteBatch* web_indexed_db_create_write_batch();
void web_indexed_db_write_batch_put(PersistenceHooks::WriteBatch*, ReadonlyBytes key, ReadonlyBytes value);
void web_indexed_db_write_batch_delete(PersistenceHooks::WriteBatch*, ReadonlyBytes key);
void web_indexed_db_commit_write_batch(void* realm, ReadonlyBytes name, bool replaces_database, PersistenceHooks::WriteBatch*);
}

namespace {

// What the browser knows the environment of a realm by, if it can be asked about it.
struct StorageClient {
    PersistenceHooks* hooks;
    PageId page_id;
    HTML::EnvironmentId environment_id;
};

Optional<StorageClient> storage_client_of(void* realm)
{
    auto* hooks = PersistenceHooks::the();
    if (!hooks)
        return {};
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    auto& settings = HTML::relevant_settings_object(js_realm.global_object());
    auto document = settings.responsible_document();
    if (!document)
        return {};
    return StorageClient { hooks, document->page().client().id(), settings.id };
}

}

u8 web_indexed_db_load_stored_database(void* realm, ReadonlyBytes name, bool has_copy, void* context, void (*on_row)(void*, u8 const*, size_t, u8 const*, size_t))
{
    auto client = storage_client_of(realm);
    if (!client.has_value())
        return to_underlying(PersistenceHooks::LoadStatus::Unavailable);

    auto result = client->hooks->load(client->page_id, client->environment_id, MUST(ByteBuffer::copy(name)), has_copy);
    if (result.row_keys.size() != result.row_values.size())
        return to_underlying(PersistenceHooks::LoadStatus::Unavailable);
    for (size_t i = 0; i < result.row_keys.size(); ++i)
        on_row(context, result.row_keys[i].data(), result.row_keys[i].size(), result.row_values[i].data(), result.row_values[i].size());
    return to_underlying(result.status);
}

void web_indexed_db_list_stored_databases(void* realm, void* context, void (*on_name)(void*, u8 const*, size_t))
{
    auto client = storage_client_of(realm);
    if (!client.has_value())
        return;
    for (auto const& name : client->hooks->database_names(client->page_id, client->environment_id))
        on_name(context, name.data(), name.size());
}

PersistenceHooks::WriteBatch* web_indexed_db_create_write_batch()
{
    return new PersistenceHooks::WriteBatch;
}

void web_indexed_db_write_batch_put(PersistenceHooks::WriteBatch* batch, ReadonlyBytes key, ReadonlyBytes value)
{
    batch->put_keys.append(MUST(ByteBuffer::copy(key)));
    batch->put_values.append(MUST(ByteBuffer::copy(value)));
}

void web_indexed_db_write_batch_delete(PersistenceHooks::WriteBatch* batch, ReadonlyBytes key)
{
    batch->deleted_keys.append(MUST(ByteBuffer::copy(key)));
}

void web_indexed_db_commit_write_batch(void* realm, ReadonlyBytes name, bool replaces_database, PersistenceHooks::WriteBatch* batch)
{
    ScopeGuard delete_batch = [&] { delete batch; };
    auto client = storage_client_of(realm);
    if (!client.has_value())
        return;
    client->hooks->commit(client->page_id, client->environment_id, MUST(ByteBuffer::copy(name)), replaces_database, *batch);
}
