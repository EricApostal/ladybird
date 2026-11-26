/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Vector.h>
#include <LibWeb/Export.h>
#include <LibWebCommon/HTML/Scripting/EnvironmentId.h>
#include <LibWebCommon/IndexedDB/StoredDatabaseStatus.h>
#include <LibWebCommon/Page/PageId.h>

namespace Web::IndexedDB {

// Where databases are kept beyond the life of the process. A database is rows that only IndexedDB reads, and its name
// is bytes of the same kind. An embedder without one has databases that live only in memory, as do workers, which have
// no page to ask through.
class WEB_API PersistenceHooks {
public:
    using LoadStatus = StoredDatabaseStatus;

    struct LoadResult {
        LoadStatus status { LoadStatus::Unavailable };
        Vector<ByteBuffer> row_keys;
        Vector<ByteBuffer> row_values;
    };

    struct WriteBatch {
        Vector<ByteBuffer> put_keys;
        Vector<ByteBuffer> put_values;
        Vector<ByteBuffer> deleted_keys;
    };

    virtual ~PersistenceHooks() = default;

    static void install(PersistenceHooks*);
    static PersistenceHooks* the();

    // The environment is the one the storage key of the database was obtained for.
    virtual LoadResult load(PageId, HTML::EnvironmentId const&, ByteBuffer const& database_name, bool has_copy) = 0;
    virtual Vector<ByteBuffer> database_names(PageId, HTML::EnvironmentId const&) = 0;
    // A batch that replaces the database writes all of it. One that also puts nothing deletes it.
    virtual void commit(PageId, HTML::EnvironmentId const&, ByteBuffer const& database_name, bool replaces_database, WriteBatch const&) = 0;
};

}
