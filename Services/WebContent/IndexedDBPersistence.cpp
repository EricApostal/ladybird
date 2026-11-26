/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Process.h>
#include <LibWeb/IndexedDB/Persistence.h>
#include <WebContent/ConnectionFromClient.h>
#include <WebContent/IndexedDBPersistence.h>

namespace WebContent {

namespace {

class IndexedDBPersistence final : public Web::IndexedDB::PersistenceHooks {
public:
    static IndexedDBPersistence& the()
    {
        // Deliberately never destroyed: a transaction can still commit while the process exits.
        static auto* persistence = new IndexedDBPersistence;
        return *persistence;
    }

    void set_connection(ConnectionFromClient& connection) { m_connection = connection.make_weak_ptr<ConnectionFromClient>(); }

    virtual LoadResult load(Web::PageId page_id, Web::HTML::EnvironmentId const& environment_id, ByteBuffer const& database_name, bool has_copy) override
    {
        if (!m_connection)
            return {};
        auto response = m_connection->send_sync_but_allow_failure<Messages::WebContentClient::LoadIndexedDatabase>(page_id, environment_id, database_name, has_copy);
        // NB: This must not pass for there being nothing kept: a page that then made the database anew would have what
        //     is kept replaced with it.
        if (!response)
            exit_as_the_browser_has_gone();
        return { static_cast<LoadStatus>(response->status()), response->take_row_keys(), response->take_row_values() };
    }

    virtual Vector<ByteBuffer> database_names(Web::PageId page_id, Web::HTML::EnvironmentId const& environment_id) override
    {
        if (!m_connection)
            return {};
        auto response = m_connection->send_sync_but_allow_failure<Messages::WebContentClient::ListIndexedDatabases>(page_id, environment_id);
        if (!response)
            exit_as_the_browser_has_gone();
        return response->take_database_names();
    }

    virtual void commit(Web::PageId page_id, Web::HTML::EnvironmentId const& environment_id, ByteBuffer const& database_name, bool replaces_database, WriteBatch const& batch) override
    {
        if (m_connection)
            m_connection->async_commit_indexed_database(page_id, environment_id, database_name, replaces_database, batch.put_keys, batch.put_values, batch.deleted_keys);
    }

private:
    [[noreturn]] static void exit_as_the_browser_has_gone()
    {
        dbgln("WebContent client disconnected while an IndexedDB database was being read. Exiting peacefully.");
        Core::Process::terminate_immediately(0);
    }

    WeakPtr<ConnectionFromClient> m_connection;
};

}

void keep_indexed_databases_through(ConnectionFromClient& connection)
{
    auto& persistence = IndexedDBPersistence::the();
    persistence.set_connection(connection);
    Web::IndexedDB::PersistenceHooks::install(&persistence);
}

}
