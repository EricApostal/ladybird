/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Hex.h>
#include <LibDatabase/Database.h>
#include <LibWebView/IndexedDBStore.h>

namespace WebView {

static constexpr u32 INDEXED_DB_SCHEMA_BASELINE_VERSION = 1u;

// What a database is known by in memory. A storage key holds no newline.
static ByteString identity_of(String const& storage_key, ByteBuffer const& database_name)
{
    return ByteString::formatted("{}\n{}", storage_key, encode_hex(database_name.bytes()));
}

ErrorOr<NonnullOwnPtr<IndexedDBStore>> IndexedDBStore::create(ByteString const& directory)
{
    auto database = TRY(Database::Database::create(directory, "IndexedDB"sv));

    auto migrations = to_array<Database::Migration>({
        {
            .version = INDEXED_DB_SCHEMA_BASELINE_VERSION,
            .sql = R"#(
                CREATE TABLE IF NOT EXISTS IndexedDBRows (
                    storage_key TEXT NOT NULL,
                    database_name BLOB NOT NULL,
                    row_key BLOB NOT NULL,
                    row_value BLOB NOT NULL,
                    last_modified INTEGER NOT NULL,
                    PRIMARY KEY(storage_key, database_name, row_key)
                );
            )#"sv,
        },
    });
    if (TRY(database->migrate("IndexedDB"sv, migrations)) != Database::MigrationOutcome::Success)
        return Error::from_string_literal("The IndexedDB databases were written by a newer version of the browser");

    Statements statements {};
    statements.select_rows = TRY(database->prepare_statement("SELECT row_key, row_value FROM IndexedDBRows WHERE storage_key = ? AND database_name = ?;"sv));
    statements.select_names = TRY(database->prepare_statement("SELECT DISTINCT database_name FROM IndexedDBRows WHERE storage_key = ?;"sv));
    statements.put_row = TRY(database->prepare_statement("INSERT OR REPLACE INTO IndexedDBRows (storage_key, database_name, row_key, row_value, last_modified) VALUES (?, ?, ?, ?, ?);"sv));
    statements.delete_row = TRY(database->prepare_statement("DELETE FROM IndexedDBRows WHERE storage_key = ? AND database_name = ? AND row_key = ?;"sv));
    statements.delete_database = TRY(database->prepare_statement("DELETE FROM IndexedDBRows WHERE storage_key = ? AND database_name = ?;"sv));
    // The schema row, whose key is the single byte "S", carries when the database was last written to.
    statements.touch_database = TRY(database->prepare_statement("UPDATE IndexedDBRows SET last_modified = ? WHERE storage_key = ? AND database_name = ? AND row_key = X'53';"sv));
    statements.delete_modified_since = TRY(database->prepare_statement("DELETE FROM IndexedDBRows WHERE (storage_key, database_name) IN (SELECT storage_key, database_name FROM IndexedDBRows WHERE last_modified >= ?);"sv));

    return adopt_own(*new IndexedDBStore { move(database), statements });
}

IndexedDBStore::IndexedDBStore(NonnullRefPtr<Database::Database> database, Statements statements)
    : m_database(move(database))
    , m_statements(statements)
{
}

IndexedDBStore::~IndexedDBStore() = default;

ErrorOr<Optional<IndexedDBStore::Rows>> IndexedDBStore::load(u64 client_id, String const& storage_key, ByteBuffer const& database_name, bool has_copy)
{
    auto& current_clients = m_current_clients.ensure(identity_of(storage_key, database_name));
    if (has_copy && current_clients.contains(client_id))
        return Optional<Rows> {};

    Rows rows;
    TRY(m_database->try_execute_statement(
        m_statements.select_rows,
        [&](auto statement_id) -> ErrorOr<void> {
            TRY(rows.keys.try_append(m_database->result_column<ByteBuffer>(statement_id, 0)));
            TRY(rows.values.try_append(m_database->result_column<ByteBuffer>(statement_id, 1)));
            return {};
        },
        storage_key, database_name));

    current_clients.set(client_id);
    return Optional<Rows> { move(rows) };
}

ErrorOr<Vector<ByteBuffer>> IndexedDBStore::database_names(String const& storage_key)
{
    Vector<ByteBuffer> names;
    TRY(m_database->try_execute_statement(
        m_statements.select_names,
        [&](auto statement_id) -> ErrorOr<void> {
            TRY(names.try_append(m_database->result_column<ByteBuffer>(statement_id, 0)));
            return {};
        },
        storage_key));
    return names;
}

ErrorOr<void> IndexedDBStore::commit(u64 client_id, String const& storage_key, ByteBuffer const& database_name, bool replaces_database,
    Vector<ByteBuffer> const& put_keys, Vector<ByteBuffer> const& put_values, Vector<ByteBuffer> const& deleted_keys)
{
    if (put_keys.size() != put_values.size())
        return Error::from_string_literal("A write has keys and values that do not match");

    // Whether or not the write succeeds, no other client's copy is the one kept here any more. The writer's is if it
    // was before.
    auto& current_clients = m_current_clients.ensure(identity_of(storage_key, database_name));
    auto writer_was_current = current_clients.contains(client_id);
    current_clients.clear();

    auto now = UnixDateTime::now();
    TRY(m_database->transaction([&]() -> ErrorOr<void> {
        if (replaces_database)
            TRY(m_database->try_execute_statement(m_statements.delete_database, {}, storage_key, database_name));
        for (size_t i = 0; i < put_keys.size(); ++i)
            TRY(m_database->try_execute_statement(m_statements.put_row, {}, storage_key, database_name, put_keys[i], put_values[i], now));
        for (auto const& key : deleted_keys)
            TRY(m_database->try_execute_statement(m_statements.delete_row, {}, storage_key, database_name, key));
        TRY(m_database->try_execute_statement(m_statements.touch_database, {}, now, storage_key, database_name));
        return {};
    }));

    if (writer_was_current)
        current_clients.set(client_id);
    return {};
}

ErrorOr<void> IndexedDBStore::remove_databases_modified_since(UnixDateTime since)
{
    m_current_clients.clear();
    return m_database->try_execute_statement(m_statements.delete_modified_since, {}, since);
}

}
