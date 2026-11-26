/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::IndexedDB {

// What the browser process answers a WebContent process that asks for a database it keeps. The values are also known to
// Libraries/LibWeb/Bindings/Rust/src/indexed_db/internal/persistence.rs.
enum class StoredDatabaseStatus : u8 {
    // Nothing is kept, or may not be read.
    Unavailable = 0,
    // The copy the process has is the one that is kept.
    Current = 1,
    // The rows are those of the database, and there are none if there is no such database.
    Loaded = 2,
};

}
