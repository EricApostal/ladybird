/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Keeping databases beyond the life of the process. The browser process holds them, as rows of the shape
//! persisted_format.rs gives them. A database is read when it is about to be used, and a transaction that commits
//! writes the rows of what it changed before script hears that it completed.
//!
//! Where the browser keeps nothing, as in a private window or a worker, databases live only in memory.
//!
//! NB: Processes each have their own copy. A process takes a fresh one when it opens a database it has no connection
//!     to, if another has written since. While it has a connection, it does not see what others write.

use std::rc::Rc;

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Vm};

use crate::bindings::interface::Ref;
use crate::indexed_db::idb_database::ConnectionState;
use crate::indexed_db::idb_record::{IndexRecord, ObjectStoreRecord};
use crate::indexed_db::idb_transaction::{IDBTransaction, IDBTransactionMode};
use crate::indexed_db::internal::algorithms::KeyPath;
use crate::indexed_db::internal::database::{Database, DatabaseRef};
use crate::indexed_db::internal::index::Index;
use crate::indexed_db::internal::key::Key;
use crate::indexed_db::internal::mutation_log::LoggedChanges;
use crate::indexed_db::internal::object_store::{ObjectStore, ObjectStoreRef};
use crate::indexed_db::internal::persisted_format::{
    EncodedRow, StoredIndex, StoredKeyPath, StoredRecord, StoredRow, StoredSchema, StoredStore, decode_database_name,
    decode_row, encode_database_name, key_generator_row, record_row, record_row_key, schema_row,
};
use crate::storage_api::storage_key::StorageKey;

type OnRow = unsafe extern "C" fn(*mut c_void, *const u8, usize, *const u8, usize);
type OnName = unsafe extern "C" fn(*mut c_void, *const u8, usize);

// Defined in Libraries/LibWeb/IndexedDB/Persistence.cpp.
unsafe extern "C" {
    fn web_indexed_db_load_stored_database(
        realm: *mut c_void,
        name: ak::Span<'_, u8>,
        has_copy: bool,
        context: *mut c_void,
        on_row: OnRow,
    ) -> u8;
    fn web_indexed_db_list_stored_databases(realm: *mut c_void, context: *mut c_void, on_name: OnName);
    fn web_indexed_db_create_write_batch() -> *mut c_void;
    fn web_indexed_db_write_batch_put(batch: *mut c_void, key: ak::Span<'_, u8>, value: ak::Span<'_, u8>);
    fn web_indexed_db_write_batch_delete(batch: *mut c_void, key: ak::Span<'_, u8>);
    fn web_indexed_db_commit_write_batch(
        realm: *mut c_void,
        name: ak::Span<'_, u8>,
        replaces_database: bool,
        batch: *mut c_void,
    );
}

/// Web::IndexedDB::PersistenceHooks::LoadStatus, of which only Loaded brings rows. The others leave the copy this
/// process has as it is.
const LOADED: u8 = 2;

/// # Safety
///
/// `data` must point to `length` bytes, or `length` be zero.
unsafe fn lent_bytes<'a>(data: *const u8, length: usize) -> &'a [u8] {
    if length == 0 {
        return &[];
    }
    // SAFETY: The caller says so.
    unsafe { core::slice::from_raw_parts(data, length) }
}

unsafe extern "C" fn collect_row(
    context: *mut c_void,
    key: *const u8,
    key_length: usize,
    value: *const u8,
    value_length: usize,
) {
    // SAFETY: The context is the vector the caller of the load passed, and C++ lends the bytes for the call.
    unsafe {
        (*context.cast::<Vec<EncodedRow>>()).push((
            lent_bytes(key, key_length).to_vec(),
            lent_bytes(value, value_length).to_vec(),
        ));
    }
}

unsafe extern "C" fn collect_name(context: *mut c_void, name: *const u8, name_length: usize) {
    // SAFETY: The context is the vector the caller of the listing passed, and C++ lends the bytes for the call.
    unsafe {
        if let Some(name) = decode_database_name(lent_bytes(name, name_length)) {
            (*context.cast::<Vec<Utf16String>>()).push(name);
        }
    }
}

/// Brings this process's copy of a database up to date with the browser's, before it is looked up. A database that is
/// in use here is left alone.
pub(crate) fn load_database_if_changed(vm: &Vm, realm: Gc<Realm>, storage_key: &StorageKey, name: &Utf16String) {
    let cached = Database::for_key_and_name(vm, storage_key, name);
    if let Some(database) = cached
        && (database.upgrade_transaction().is_some()
            || database
                .associated_connections(vm)
                .values()
                .iter()
                .any(|connection| connection.state() != ConnectionState::Closed))
    {
        return;
    }

    let mut rows: Vec<EncodedRow> = Vec::new();
    let encoded_name = encode_database_name(name);
    // SAFETY: The realm is live, the name is lent for the call, and the rows outlive it.
    let status = unsafe {
        web_indexed_db_load_stored_database(
            realm.as_ptr().cast(),
            ak::Span::new(&encoded_name),
            cached.is_some(),
            (&raw mut rows).cast(),
            collect_row,
        )
    };
    if status != LOADED {
        return;
    }

    // Another process has written since this copy was taken, or this one has none.
    if cached.is_some() {
        Database::delete_for_key_and_name(vm, storage_key, name);
    }
    restore_database(vm, storage_key, name, rows);
}

/// The same, for every database of a storage key.
pub(crate) fn load_all_databases(vm: &Vm, realm: Gc<Realm>, storage_key: &StorageKey) {
    let mut names: Vec<Utf16String> = Vec::new();
    // SAFETY: The realm is live, and the names outlive the call.
    unsafe { web_indexed_db_list_stored_databases(realm.as_ptr().cast(), (&raw mut names).cast(), collect_name) };

    // Those this process has a copy of may have been deleted since.
    for database in Database::for_key(vm, storage_key).values().iter() {
        let name = database.name();
        if !names.contains(&name) {
            names.push(name);
        }
    }

    for name in &names {
        load_database_if_changed(vm, realm, storage_key, name);
    }
}

fn key_path_from_stored(key_path: StoredKeyPath) -> KeyPath {
    match key_path {
        StoredKeyPath::String(path) => KeyPath::String(path),
        StoredKeyPath::Sequence(paths) => KeyPath::Sequence(paths),
    }
}

fn key_path_to_stored(key_path: &KeyPath) -> StoredKeyPath {
    match key_path {
        KeyPath::String(path) => StoredKeyPath::String(path.clone()),
        KeyPath::Sequence(paths) => StoredKeyPath::Sequence(paths.clone()),
    }
}

fn restore_database(vm: &Vm, storage_key: &StorageKey, name: &Utf16String, rows: Vec<EncodedRow>) {
    let mut schema: Option<StoredSchema> = None;
    let mut key_generators: Vec<(Utf16String, u64)> = Vec::new();
    let mut records: Vec<(Utf16String, Vec<StoredRecord>)> = Vec::new();

    // There are no rows for a database the browser does not have.
    if rows.is_empty() {
        return;
    }

    for (key, value) in &rows {
        match decode_row(key, value) {
            Some(StoredRow::Schema(stored)) => schema = Some(stored),
            Some(StoredRow::KeyGenerator { store, current_number }) => key_generators.push((store, current_number)),
            Some(StoredRow::Record { store, record }) => match records.iter_mut().find(|(name, _)| *name == store) {
                Some((_, records)) => records.push(record),
                None => records.push((store, vec![record])),
            },
            None => {
                eprintln!("IndexedDB: a stored database has a row that cannot be read, so it is not loaded");
                return;
            }
        }
    }
    let Some(schema) = schema else {
        eprintln!("IndexedDB: a stored database has no schema, so it is not loaded");
        return;
    };

    let database = Database::create_for_key_and_name(vm, storage_key, name);
    database.set_version(schema.version);

    for stored_store in schema.stores {
        let store = ObjectStore::create(
            vm,
            database,
            stored_store.name.clone(),
            stored_store.uses_a_key_generator,
            stored_store.key_path.map(key_path_from_stored),
        );
        if stored_store.uses_a_key_generator
            && let Some((_, current_number)) = key_generators.iter().find(|(name, _)| *name == stored_store.name)
        {
            store.key_generator().set(*current_number);
        }

        let stored_records = records
            .iter()
            .position(|(name, _)| *name == stored_store.name)
            .map(|position| records.swap_remove(position).1)
            .unwrap_or_default();

        for stored_index in stored_store.indexes {
            let index = Index::create(
                vm,
                store,
                &stored_index.name,
                &key_path_from_stored(stored_index.key_path),
                stored_index.unique,
                stored_index.multi_entry,
            );
            let mut index_records = Vec::new();
            for record in &stored_records {
                for (index_name, key) in &record.index_entries {
                    if *index_name == stored_index.name {
                        index_records.push(IndexRecord {
                            key: key.clone(),
                            value: record.key.clone(),
                        });
                    }
                }
            }
            index.restore_records(index_records);
        }

        store.restore_records(
            stored_records
                .into_iter()
                .map(|record| ObjectStoreRecord {
                    key: record.key,
                    value: record.value,
                })
                .collect(),
        );
    }
}

#[derive(Default)]
struct Changes {
    puts: Vec<EncodedRow>,
    deletes: Vec<Vec<u8>>,
}

/// Adds the rows of the records of `store` with `primary_keys`, which are sorted and distinct. A key with no record
/// has its row deleted.
fn add_record_rows(vm: &Vm, store: ObjectStoreRef, primary_keys: &[Rc<Key>], changes: &mut Changes) {
    let store_name = store.name();

    // What the indexes have for each of these records, found by going through each index once.
    let mut index_entries: Vec<Vec<(Utf16String, Rc<Key>)>> = vec![Vec::new(); primary_keys.len()];
    for index in store.indexes(vm).values().iter() {
        let index_name = index.name();
        for record in index.records().iter() {
            if let Ok(position) = primary_keys.binary_search_by(|key| Key::ordering(key, &record.value)) {
                index_entries[position].push((index_name.clone(), record.key.clone()));
            }
        }
    }

    let records = store.records();
    for (primary_key, index_entries) in primary_keys.iter().zip(index_entries) {
        match records.binary_search_by(|record| Key::ordering(&record.key, primary_key)) {
            Ok(position) => changes.puts.push(record_row(
                &store_name,
                &StoredRecord {
                    key: records[position].key.clone(),
                    value: records[position].value.clone(),
                    index_entries,
                },
            )),
            Err(_) => changes.deletes.push(record_row_key(&store_name, primary_key)),
        }
    }
}

fn key_generator_row_of(store: ObjectStoreRef) -> EncodedRow {
    let current_number = store.key_generator().current_number();
    key_generator_row(&store.name(), current_number)
}

/// The rows of the whole of a database.
fn database_rows(vm: &Vm, database: DatabaseRef) -> Changes {
    let mut changes = Changes::default();
    let mut stores = Vec::new();

    for store in database.object_stores(vm).values().iter() {
        let store = *store;
        let indexes = store
            .indexes(vm)
            .values()
            .iter()
            .map(|index| StoredIndex {
                name: index.name(),
                key_path: key_path_to_stored(index.key_path()),
                unique: index.unique(),
                multi_entry: index.multi_entry(),
            })
            .collect();
        stores.push(StoredStore {
            name: store.name(),
            key_path: store.key_path().map(key_path_to_stored),
            uses_a_key_generator: store.uses_a_key_generator(),
            indexes,
        });

        if store.uses_a_key_generator() {
            changes.puts.push(key_generator_row_of(store));
        }
        let primary_keys: Vec<Rc<Key>> = store.records().iter().map(|record| record.key.clone()).collect();
        add_record_rows(vm, store, &primary_keys, &mut changes);
    }

    changes.puts.push(schema_row(&StoredSchema {
        version: database.version(),
        stores,
    }));
    changes
}

fn commit_changes(realm: Gc<Realm>, name: &Utf16String, replaces_database: bool, changes: &Changes) {
    let name = encode_database_name(name);
    // SAFETY: The realm is live, every span is lent for its call, and the last call takes over the batch.
    unsafe {
        let batch = web_indexed_db_create_write_batch();
        for (key, value) in &changes.puts {
            web_indexed_db_write_batch_put(batch, ak::Span::new(key), ak::Span::new(value));
        }
        for key in &changes.deletes {
            web_indexed_db_write_batch_delete(batch, ak::Span::new(key));
        }
        web_indexed_db_commit_write_batch(realm.as_ptr().cast(), ak::Span::new(&name), replaces_database, batch);
    }
}

/// Writes what a transaction that is committing changed. Its mutation logs must still be there.
pub(crate) fn save_committed_transaction(vm: &Vm, transaction: Ref<IDBTransaction>) {
    let database = transaction.connection_object().associated_database();

    // An upgrade can change anything, and only one runs at a time, so the whole database is written again.
    if transaction.is_upgrade_transaction() {
        let changes = database_rows(vm, database);
        commit_changes(transaction.realm(), &database.name(), true, &changes);
        return;
    }
    if transaction.transaction_mode() == IDBTransactionMode::Readonly {
        return;
    }

    let mut changes = Changes::default();
    for (store, log) in transaction.changed_stores(vm).values().iter() {
        let LoggedChanges {
            mut primary_keys,
            key_generator,
        } = log.changes();
        if key_generator && store.uses_a_key_generator() {
            changes.puts.push(key_generator_row_of(*store));
        }
        primary_keys.sort_by(|a, b| Key::ordering(a, b));
        primary_keys.dedup_by(|a, b| Key::equals(a, b));
        if !primary_keys.is_empty() {
            add_record_rows(vm, *store, &primary_keys, &mut changes);
        }
    }

    if !changes.puts.is_empty() || !changes.deletes.is_empty() {
        commit_changes(transaction.realm(), &database.name(), false, &changes);
    }
}

pub(crate) fn save_database_deletion(realm: Gc<Realm>, name: &Utf16String) {
    commit_changes(realm, name, true, &Changes::default());
}
