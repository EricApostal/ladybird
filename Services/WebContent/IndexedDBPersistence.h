/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace WebContent {

class ConnectionFromClient;

// Has the browser process keep this process's IndexedDB databases.
void keep_indexed_databases_through(ConnectionFromClient&);

}
