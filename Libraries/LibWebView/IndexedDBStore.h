/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibDatabase/Forward.h>
#include <LibWebView/Forward.h>

namespace WebView {

// The IndexedDB databases of a browsing session that is kept on disk. WebContent processes hold the databases they use in
// memory and send what their transactions change. This keeps each database as the rows it was sent, which it does not
// look into. It runs on the main thread.
class WEBVIEW_API IndexedDBStore {
    AK_MAKE_NONCOPYABLE(IndexedDBStore);
    AK_MAKE_NONMOVABLE(IndexedDBStore);

public:
    AK_ALLOC_WITH_KMALLOC;

    // The databases get a file of their own, apart from the rest of what is browsed.
    static ErrorOr<NonnullOwnPtr<IndexedDBStore>> create(ByteString const& directory);
    ~IndexedDBStore();

    struct Rows {
        Vector<ByteBuffer> keys;
        Vector<ByteBuffer> values;
    };

    // The rows of a database for a client, or nothing if the copy the client has is still the one kept here. `has_copy`
    // says whether it has one at all.
    ErrorOr<Optional<Rows>> load(u64 client_id, String const& storage_key, ByteBuffer const& database_name, bool has_copy);
    ErrorOr<Vector<ByteBuffer>> database_names(String const& storage_key);

    // Either all of a write is applied or none of it. One that replaces the database and puts nothing deletes it.
    ErrorOr<void> commit(u64 client_id, String const& storage_key, ByteBuffer const& database_name, bool replaces_database,
        Vector<ByteBuffer> const& put_keys, Vector<ByteBuffer> const& put_values, Vector<ByteBuffer> const& deleted_keys);

    // Deletes every database written to since then.
    ErrorOr<void> remove_databases_modified_since(UnixDateTime);

private:
    struct Statements {
        Database::StatementID select_rows { 0 };
        Database::StatementID select_names { 0 };
        Database::StatementID put_row { 0 };
        Database::StatementID delete_row { 0 };
        Database::StatementID delete_database { 0 };
        Database::StatementID touch_database { 0 };
        Database::StatementID delete_modified_since { 0 };
    };

    IndexedDBStore(NonnullRefPtr<Database::Database>, Statements);

    NonnullRefPtr<Database::Database> m_database;
    Statements m_statements;

    // For each database that has been read or written since startup, the clients whose copy of it is up to date.
    HashMap<ByteString, HashTable<u64>> m_current_clients;
};

}
