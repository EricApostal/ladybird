/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! How a database is written down for the browser to keep. The browser keeps each database as rows it does not look
//! into: one row for the schema, one for each key generator, and one for each record of an object store, which carries
//! the records the indexes have for it. A transaction then only has to write the rows of what it changed.
//!
//! NB: This knows nothing of the engine beyond keys, so that it can be tested on its own.

use std::rc::Rc;

use ak::Utf16String;

use crate::indexed_db::internal::key::{Key, KeyType, KeyValue};

/// Changes when the rows change shape. Values are structured serializations, which carry versions of their own.
const FORMAT_VERSION: u8 = 1;

const SCHEMA_ROW: u8 = b'S';
const KEY_GENERATOR_ROW: u8 = b'G';
const RECORD_ROW: u8 = b'R';

/// How deeply array keys can nest in a row that is read back, so that a damaged row cannot exhaust the stack.
const MAXIMUM_KEY_DEPTH: usize = 512;

#[cfg_attr(test, derive(Debug, PartialEq))]
pub(crate) enum StoredKeyPath {
    String(Utf16String),
    Sequence(Vec<Utf16String>),
}

#[cfg_attr(test, derive(Debug, PartialEq))]
pub(crate) struct StoredIndex {
    pub(crate) name: Utf16String,
    pub(crate) key_path: StoredKeyPath,
    pub(crate) unique: bool,
    pub(crate) multi_entry: bool,
}

#[cfg_attr(test, derive(Debug, PartialEq))]
pub(crate) struct StoredStore {
    pub(crate) name: Utf16String,
    pub(crate) key_path: Option<StoredKeyPath>,
    pub(crate) uses_a_key_generator: bool,
    pub(crate) indexes: Vec<StoredIndex>,
}

#[cfg_attr(test, derive(Debug, PartialEq))]
pub(crate) struct StoredSchema {
    pub(crate) version: u64,
    pub(crate) stores: Vec<StoredStore>,
}

pub(crate) struct StoredRecord {
    pub(crate) key: Rc<Key>,
    pub(crate) value: Rc<[u8]>,
    /// For each record an index has for this one: the name of the index, and the key it is found by there.
    pub(crate) index_entries: Vec<(Utf16String, Rc<Key>)>,
}

pub(crate) enum StoredRow {
    Schema(StoredSchema),
    KeyGenerator { store: Utf16String, current_number: u64 },
    Record { store: Utf16String, record: StoredRecord },
}

/// A row, as the bytes it is found by and the bytes it holds.
pub(crate) type EncodedRow = (Vec<u8>, Vec<u8>);

#[derive(Default)]
struct Writer(Vec<u8>);

impl Writer {
    fn starting_with(byte: u8) -> Self {
        Self(vec![byte])
    }

    fn u8(&mut self, value: u8) {
        self.0.push(value);
    }

    fn u32(&mut self, value: u32) {
        self.0.extend_from_slice(&value.to_le_bytes());
    }

    fn u64(&mut self, value: u64) {
        self.0.extend_from_slice(&value.to_le_bytes());
    }

    fn length(&mut self, length: usize) {
        self.u32(u32::try_from(length).expect("a database has nothing with 2^32 parts"));
    }

    fn bytes(&mut self, bytes: &[u8]) {
        self.length(bytes.len());
        self.0.extend_from_slice(bytes);
    }

    fn string(&mut self, string: &Utf16String) {
        let units = string.to_utf16();
        self.length(units.len());
        for unit in units.iter() {
            self.0.extend_from_slice(&unit.to_le_bytes());
        }
    }

    fn key(&mut self, key: &Key) {
        match (key.r#type(), key.value()) {
            (KeyType::Number | KeyType::Date, KeyValue::Double(value)) => {
                self.u8(if key.r#type() == KeyType::Number { 1 } else { 2 });
                // -0 and 0 are one key, so they have to be one row.
                let value = if *value == 0.0 { 0.0 } else { *value };
                self.u64(value.to_bits());
            }
            (KeyType::String, KeyValue::String(value)) => {
                self.u8(3);
                self.string(value);
            }
            (KeyType::Binary, KeyValue::Bytes(value)) => {
                self.u8(4);
                self.bytes(value);
            }
            (KeyType::Array, KeyValue::Keys(subkeys)) => {
                self.u8(5);
                self.length(subkeys.len());
                for subkey in subkeys {
                    self.key(subkey);
                }
            }
            _ => unreachable!("invalid keys are not stored"),
        }
    }

    fn key_path(&mut self, key_path: Option<&StoredKeyPath>) {
        match key_path {
            None => self.u8(0),
            Some(StoredKeyPath::String(path)) => {
                self.u8(1);
                self.string(path);
            }
            Some(StoredKeyPath::Sequence(paths)) => {
                self.u8(2);
                self.length(paths.len());
                for path in paths {
                    self.string(path);
                }
            }
        }
    }
}

/// Every read answers None once the bytes do not hold what is asked of them.
struct Reader<'a>(&'a [u8]);

impl<'a> Reader<'a> {
    fn take(&mut self, count: usize) -> Option<&'a [u8]> {
        let (taken, rest) = self.0.split_at_checked(count)?;
        self.0 = rest;
        Some(taken)
    }

    fn u8(&mut self) -> Option<u8> {
        Some(self.take(1)?[0])
    }

    fn bool(&mut self) -> Option<bool> {
        match self.u8()? {
            0 => Some(false),
            1 => Some(true),
            _ => None,
        }
    }

    fn u32(&mut self) -> Option<u32> {
        Some(u32::from_le_bytes(self.take(4)?.try_into().ok()?))
    }

    fn u64(&mut self) -> Option<u64> {
        Some(u64::from_le_bytes(self.take(8)?.try_into().ok()?))
    }

    /// A count of things that take at least `size` bytes each, so that a damaged count cannot make us allocate for
    /// more than there is.
    fn count(&mut self, size: usize) -> Option<usize> {
        let count = self.u32()? as usize;
        (count.checked_mul(size)? <= self.0.len()).then_some(count)
    }

    fn bytes(&mut self) -> Option<&'a [u8]> {
        let length = self.count(1)?;
        self.take(length)
    }

    fn string(&mut self) -> Option<Utf16String> {
        let length = self.count(2)?;
        Some(string_from_utf16_le(self.take(length * 2)?))
    }

    fn key(&mut self, depth: usize) -> Option<Rc<Key>> {
        match self.u8()? {
            1 => Some(Key::create_number(f64::from_bits(self.u64()?))),
            2 => Some(Key::create_date(f64::from_bits(self.u64()?))),
            3 => Some(Key::create_string(self.string()?)),
            4 => Some(Key::create_binary(self.bytes()?.to_vec())),
            5 => {
                if depth >= MAXIMUM_KEY_DEPTH {
                    return None;
                }
                let count = self.count(1)?;
                let mut subkeys = Vec::with_capacity(count);
                for _ in 0..count {
                    subkeys.push(self.key(depth + 1)?);
                }
                Some(Key::create_array(subkeys))
            }
            _ => None,
        }
    }

    fn key_path(&mut self) -> Option<Option<StoredKeyPath>> {
        match self.u8()? {
            0 => Some(None),
            1 => Some(Some(StoredKeyPath::String(self.string()?))),
            2 => {
                let count = self.count(4)?;
                let mut paths = Vec::with_capacity(count);
                for _ in 0..count {
                    paths.push(self.string()?);
                }
                Some(Some(StoredKeyPath::Sequence(paths)))
            }
            _ => None,
        }
    }

    fn end(&self) -> Option<()> {
        self.0.is_empty().then_some(())
    }
}

fn string_from_utf16_le(bytes: &[u8]) -> Utf16String {
    let units: Vec<u16> = bytes
        .chunks_exact(2)
        .map(|unit| u16::from_le_bytes([unit[0], unit[1]]))
        .collect();
    Utf16String::from_utf16(&units)
}

/// A database's name, as the browser finds the database by. A name is any sequence of code units, lone surrogates
/// included, so it is kept as it is rather than as text.
pub(crate) fn encode_database_name(name: &Utf16String) -> Vec<u8> {
    name.to_utf16().iter().flat_map(|unit| unit.to_le_bytes()).collect()
}

pub(crate) fn decode_database_name(bytes: &[u8]) -> Option<Utf16String> {
    (bytes.len() % 2 == 0).then(|| string_from_utf16_le(bytes))
}

pub(crate) fn schema_row(schema: &StoredSchema) -> EncodedRow {
    let mut value = Writer::starting_with(FORMAT_VERSION);
    value.u64(schema.version);
    value.length(schema.stores.len());
    for store in &schema.stores {
        value.string(&store.name);
        value.key_path(store.key_path.as_ref());
        value.u8(u8::from(store.uses_a_key_generator));
        value.length(store.indexes.len());
        for index in &store.indexes {
            value.string(&index.name);
            value.key_path(Some(&index.key_path));
            value.u8(u8::from(index.unique));
            value.u8(u8::from(index.multi_entry));
        }
    }
    (vec![SCHEMA_ROW], value.0)
}

pub(crate) fn key_generator_row(store: &Utf16String, current_number: u64) -> EncodedRow {
    let mut key = Writer::starting_with(KEY_GENERATOR_ROW);
    key.string(store);
    (key.0, current_number.to_le_bytes().to_vec())
}

/// What the row of the record of `store` with `primary_key` is found by.
pub(crate) fn record_row_key(store: &Utf16String, primary_key: &Key) -> Vec<u8> {
    let mut key = Writer::starting_with(RECORD_ROW);
    key.string(store);
    key.key(primary_key);
    key.0
}

pub(crate) fn record_row(store: &Utf16String, record: &StoredRecord) -> EncodedRow {
    let mut value = Writer::default();
    value.bytes(&record.value);
    value.length(record.index_entries.len());
    for (index, key) in &record.index_entries {
        value.string(index);
        value.key(key);
    }
    (record_row_key(store, &record.key), value.0)
}

fn decode_schema(value: &mut Reader<'_>) -> Option<StoredSchema> {
    if value.u8()? != FORMAT_VERSION {
        return None;
    }
    let version = value.u64()?;
    let store_count = value.count(1)?;
    let mut stores = Vec::with_capacity(store_count);
    for _ in 0..store_count {
        let name = value.string()?;
        let key_path = value.key_path()?;
        let uses_a_key_generator = value.bool()?;
        let index_count = value.count(1)?;
        let mut indexes = Vec::with_capacity(index_count);
        for _ in 0..index_count {
            indexes.push(StoredIndex {
                name: value.string()?,
                key_path: value.key_path()??,
                unique: value.bool()?,
                multi_entry: value.bool()?,
            });
        }
        stores.push(StoredStore {
            name,
            key_path,
            uses_a_key_generator,
            indexes,
        });
    }
    Some(StoredSchema { version, stores })
}

pub(crate) fn decode_row(key: &[u8], value: &[u8]) -> Option<StoredRow> {
    let mut key = Reader(key);
    let mut value = Reader(value);
    let row = match key.u8()? {
        SCHEMA_ROW => StoredRow::Schema(decode_schema(&mut value)?),
        KEY_GENERATOR_ROW => StoredRow::KeyGenerator {
            store: key.string()?,
            current_number: value.u64()?,
        },
        RECORD_ROW => {
            let store = key.string()?;
            let primary_key = key.key(0)?;
            let bytes: Rc<[u8]> = value.bytes()?.into();
            let entry_count = value.count(1)?;
            let mut index_entries = Vec::with_capacity(entry_count);
            for _ in 0..entry_count {
                index_entries.push((value.string()?, value.key(0)?));
            }
            StoredRow::Record {
                store,
                record: StoredRecord {
                    key: primary_key,
                    value: bytes,
                    index_entries,
                },
            }
        }
        _ => return None,
    };
    key.end()?;
    value.end()?;
    Some(row)
}
