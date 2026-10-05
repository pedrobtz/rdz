use std::collections::HashMap;
use std::collections::hash_map::Entry;
use std::hash::{BuildHasherDefault, Hasher};

use crate::format::{
    BLOCK_SIZE, ENCODING_STRING_DICT_ENTRIES, ENCODING_STRING_DICT_INDICES, ENCODING_STRING_PLAIN,
    FormatError, read_u32, try_vec_with_capacity,
};

const RECORD_HEADER_LEN: usize = 5;
const STRING_NA: u8 = 0;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum StringEncoding {
    Native = 1,
    Utf8 = 2,
    Latin1 = 3,
    Bytes = 4,
}

impl StringEncoding {
    fn from_wire(value: u8) -> Result<Self, FormatError> {
        match value {
            1 => Ok(Self::Native),
            2 => Ok(Self::Utf8),
            3 => Ok(Self::Latin1),
            4 => Ok(Self::Bytes),
            _ => Err(FormatError::Invalid("invalid character encoding tag")),
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum StringValue {
    Na,
    Value {
        encoding: StringEncoding,
        bytes: Vec<u8>,
    },
}

#[derive(Debug, PartialEq, Eq)]
pub(crate) struct StringBlock {
    pub(crate) logical_count: u64,
    pub(crate) bytes: Vec<u8>,
}

pub(crate) fn encode_blocks(values: &[StringValue]) -> Result<Vec<StringBlock>, FormatError> {
    let mut blocks = Vec::new();
    let mut bytes = Vec::new();
    let mut count = 0_u64;

    for value in values {
        let value_len = match value {
            StringValue::Na => 0,
            StringValue::Value { bytes, .. } => bytes.len(),
        };
        let record_len = RECORD_HEADER_LEN
            .checked_add(value_len)
            .ok_or(FormatError::Limit("character record"))?;
        if record_len > BLOCK_SIZE {
            return Err(FormatError::Limit("character value block"));
        }
        if !bytes.is_empty() && bytes.len() + record_len > BLOCK_SIZE {
            blocks
                .try_reserve(1)
                .map_err(|_| FormatError::Limit("character block list"))?;
            blocks.push(StringBlock {
                logical_count: count,
                bytes,
            });
            bytes = Vec::new();
            count = 0;
        }
        bytes
            .try_reserve(record_len)
            .map_err(|_| FormatError::Limit("character block allocation"))?;
        match value {
            StringValue::Na => {
                bytes.push(STRING_NA);
                bytes.extend_from_slice(&0_u32.to_le_bytes());
            }
            StringValue::Value {
                encoding,
                bytes: value_bytes,
            } => {
                let length = u32::try_from(value_bytes.len())
                    .map_err(|_| FormatError::Limit("character value"))?;
                bytes.push(*encoding as u8);
                bytes.extend_from_slice(&length.to_le_bytes());
                bytes.extend_from_slice(value_bytes);
            }
        }
        count = count
            .checked_add(1)
            .ok_or(FormatError::Limit("character count"))?;
    }

    if count != 0 || blocks.is_empty() {
        blocks
            .try_reserve(1)
            .map_err(|_| FormatError::Limit("character block list"))?;
        blocks.push(StringBlock {
            logical_count: count,
            bytes,
        });
    }
    Ok(blocks)
}

/// Borrowed access to a character vector while it is written.
///
/// `key(index)` is an identity: elements with equal keys must have equal
/// values. The R adapter passes the CHARSXP address, which R's global string
/// cache makes unique per (bytes, encoding). Keys are used only while
/// encoding and are never written.
pub(crate) trait StringSource {
    fn len(&self) -> usize;
    fn key(&self, index: usize) -> usize;
    fn value(&self, index: usize) -> Result<StringRef<'_>, FormatError>;
}

impl StringSource for [StringValue] {
    fn len(&self) -> usize {
        <[StringValue]>::len(self)
    }

    fn key(&self, index: usize) -> usize {
        index
    }

    fn value(&self, index: usize) -> Result<StringRef<'_>, FormatError> {
        match self.get(index) {
            None => Err(FormatError::Invalid("string index is out of range")),
            Some(StringValue::Na) => Ok(StringRef::Na),
            Some(StringValue::Value { encoding, bytes }) => Ok(StringRef::Value {
                encoding: *encoding,
                bytes,
            }),
        }
    }
}

/// How a character vector's repeated values are stored.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum DictionaryPolicy {
    /// Every element as its own record.
    Plain,
    /// A dictionary that restarts every `DICT_CHUNK_VALUES` elements.
    Block,
    /// One dictionary for the whole vector, sent incrementally.
    Global,
    /// `Global` when a sample estimates that values repeat enough to pay
    /// for the index, otherwise `Plain`.
    Auto,
}

/// Positions sampled by `Auto` to estimate the share of distinct values.
const AUTO_SAMPLE: usize = 16 * 1024;
/// `Auto` uses a dictionary below this estimated share of distinct values.
/// Calibrated with `tools/bench-strings.R` on 2026-09-27: reads favor the
/// dictionary up to about 0.8, writes favor plain records above about 0.5,
/// and 0.75 keeps the read wins where writes lose at most about 30%.
pub(crate) const AUTO_MAX_DISTINCT_SHARE: f64 = 0.75;
/// Dictionary entries at which any dictionary policy writes the rest of the
/// vector plain, bounding the writer's identity map.
const MAX_DICTIONARY_ENTRIES: usize = 1 << 22;

/// Elements per dictionary-index block; bounds an index block to 256 KiB.
pub(crate) const DICT_CHUNK_VALUES: usize = 64 * 1024;
const DICT_INDEX_HEADER_LEN: usize = 8;

fn record_len(value: StringRef<'_>) -> Result<usize, FormatError> {
    let value_len = match value {
        StringRef::Na => 0,
        StringRef::Value { bytes, .. } => bytes.len(),
    };
    let length = RECORD_HEADER_LEN
        .checked_add(value_len)
        .ok_or(FormatError::Limit("character record"))?;
    if length > BLOCK_SIZE {
        return Err(FormatError::Limit("character value block"));
    }
    Ok(length)
}

fn push_record(output: &mut Vec<u8>, value: StringRef<'_>) -> Result<(), FormatError> {
    let (tag, bytes): (u8, &[u8]) = match value {
        StringRef::Na => (STRING_NA, &[]),
        StringRef::Value { encoding, bytes } => (encoding as u8, bytes),
    };
    let length = u32::try_from(bytes.len()).map_err(|_| FormatError::Limit("character value"))?;
    output
        .try_reserve(RECORD_HEADER_LEN + bytes.len())
        .map_err(|_| FormatError::Limit("character block allocation"))?;
    output.push(tag);
    output.extend_from_slice(&length.to_le_bytes());
    output.extend_from_slice(bytes);
    Ok(())
}

/// Encodes `source` under `policy`, calling `emit(encoding, logical_count,
/// payload)` once per block in file order. An empty source is one empty
/// plain block under every policy.
pub(crate) fn encode_values<S, F>(
    source: &S,
    policy: DictionaryPolicy,
    emit: F,
) -> Result<(), FormatError>
where
    S: StringSource + ?Sized,
    F: FnMut(u16, u64, &[u8]) -> Result<(), FormatError>,
{
    encode_values_capped(source, policy, MAX_DICTIONARY_ENTRIES, emit)
}

fn encode_values_capped<S, F>(
    source: &S,
    policy: DictionaryPolicy,
    max_entries: usize,
    mut emit: F,
) -> Result<(), FormatError>
where
    S: StringSource + ?Sized,
    F: FnMut(u16, u64, &[u8]) -> Result<(), FormatError>,
{
    let length = source.len();
    let policy = match policy {
        DictionaryPolicy::Auto if estimated_distinct_share(source) < AUTO_MAX_DISTINCT_SHARE => {
            DictionaryPolicy::Global
        }
        DictionaryPolicy::Auto => DictionaryPolicy::Plain,
        policy => policy,
    };
    if policy == DictionaryPolicy::Plain || length == 0 {
        return encode_plain(source, 0..length, emit);
    }

    let mut ids: HashMap<usize, u32, BuildHasherDefault<IdentityHasher>> = HashMap::default();
    let mut next_id = 0_u32;
    let chunk_capacity = DICT_CHUNK_VALUES.min(length);
    let mut chunk_ids = try_vec_with_capacity(chunk_capacity, "dictionary ids")?;
    let mut first_seen = try_vec_with_capacity(chunk_capacity, "dictionary entries")?;
    let mut entries = Vec::new();
    let mut index_block = Vec::new();
    let mut start = 0_usize;
    while start < length {
        let end = start.saturating_add(DICT_CHUNK_VALUES).min(length);
        if policy == DictionaryPolicy::Block {
            ids.clear();
        }
        // Assign ids first and emit afterwards, so a chunk can still be
        // written plain before any of its entries reach the file.
        chunk_ids.clear();
        first_seen.clear();
        for index in start..end {
            let id = match ids.entry(source.key(index)) {
                Entry::Occupied(entry) => *entry.get(),
                Entry::Vacant(entry) => {
                    let id = next_id;
                    next_id = next_id
                        .checked_add(1)
                        .ok_or(FormatError::Limit("string dictionary size"))?;
                    first_seen.push(index);
                    *entry.insert(id)
                }
            };
            chunk_ids.push(id);
        }
        if ids.len() > max_entries {
            // Nothing from this chunk has been emitted yet.
            return encode_plain(source, start..length, emit);
        }

        let mut entry_count = 0_u64;
        entries.clear();
        for &index in &first_seen {
            let value = source.value(index)?;
            let record = record_len(value)?;
            if entry_count != 0 && entries.len() + record > BLOCK_SIZE {
                emit(ENCODING_STRING_DICT_ENTRIES, entry_count, &entries)?;
                entries.clear();
                entry_count = 0;
            }
            push_record(&mut entries, value)?;
            entry_count += 1;
        }
        if entry_count != 0 {
            emit(ENCODING_STRING_DICT_ENTRIES, entry_count, &entries)?;
        }
        encode_indices(&chunk_ids, &mut index_block)?;
        emit(
            ENCODING_STRING_DICT_INDICES,
            chunk_ids.len() as u64,
            &index_block,
        )?;
        start = end;
    }
    Ok(())
}

/// Estimated share of distinct values in `source`: exact for a vector no
/// larger than the sample, otherwise the bias-corrected Chao1 estimate from
/// a fixed-seed random sample of positions. Random positions make the
/// estimate independent of order, so sorted and shuffled data agree, and the
/// fixed seed keeps output deterministic.
fn estimated_distinct_share<S: StringSource + ?Sized>(source: &S) -> f64 {
    let length = source.len();
    if length == 0 {
        return 0.0;
    }
    let mut counts: HashMap<usize, u32, BuildHasherDefault<IdentityHasher>> = HashMap::default();
    if length <= AUTO_SAMPLE {
        for index in 0..length {
            *counts.entry(source.key(index)).or_insert(0) += 1;
        }
        return counts.len() as f64 / length as f64;
    }
    let mut state = 0x9e37_79b9_7f4a_7c15_u64;
    for _ in 0..AUTO_SAMPLE {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        let position = (state.wrapping_mul(0x2545_f491_4f6c_dd1d) % length as u64) as usize;
        *counts.entry(source.key(position)).or_insert(0) += 1;
    }
    let (singletons, doubletons) =
        counts
            .values()
            .fold((0.0, 0.0), |(f1, f2), &count| match count {
                1 => (f1 + 1.0, f2),
                2 => (f1, f2 + 1.0),
                _ => (f1, f2),
            });
    let estimate =
        counts.len() as f64 + singletons * (singletons - 1.0) / (2.0 * (doubletons + 1.0));
    (estimate / length as f64).min(1.0)
}

fn encode_plain<S, F>(
    source: &S,
    range: std::ops::Range<usize>,
    mut emit: F,
) -> Result<(), FormatError>
where
    S: StringSource + ?Sized,
    F: FnMut(u16, u64, &[u8]) -> Result<(), FormatError>,
{
    let mut buffer = Vec::new();
    let mut count = 0_u64;
    let mut emitted = false;
    for index in range {
        let value = source.value(index)?;
        let record = record_len(value)?;
        if count != 0 && buffer.len() + record > BLOCK_SIZE {
            emit(ENCODING_STRING_PLAIN, count, &buffer)?;
            buffer.clear();
            count = 0;
            emitted = true;
        }
        push_record(&mut buffer, value)?;
        count += 1;
    }
    if count != 0 || !emitted {
        emit(ENCODING_STRING_PLAIN, count, &buffer)?;
    }
    Ok(())
}

/// Writes `width:u8, 0, 0, 0, base:u32` then `ids[i] - base` at the
/// narrowest of 1, 2, or 4 little-endian bytes that fits the block's range.
fn encode_indices(ids: &[u32], output: &mut Vec<u8>) -> Result<(), FormatError> {
    let (low, high) = ids.iter().fold((u32::MAX, 0_u32), |(low, high), &id| {
        (low.min(id), high.max(id))
    });
    let base = if ids.is_empty() { 0 } else { low };
    let range = high.saturating_sub(base);
    let width = if range <= u32::from(u8::MAX) {
        1_usize
    } else if range <= u32::from(u16::MAX) {
        2
    } else {
        4
    };
    output.clear();
    output
        .try_reserve(DICT_INDEX_HEADER_LEN + ids.len() * width)
        .map_err(|_| FormatError::Limit("dictionary index allocation"))?;
    output.push(width as u8);
    output.extend_from_slice(&[0, 0, 0]);
    output.extend_from_slice(&base.to_le_bytes());
    match width {
        1 => output.extend(ids.iter().map(|&id| (id - base) as u8)),
        2 => {
            for &id in ids {
                output.extend_from_slice(&((id - base) as u16).to_le_bytes());
            }
        }
        _ => {
            for &id in ids {
                output.extend_from_slice(&(id - base).to_le_bytes());
            }
        }
    }
    Ok(())
}

/// Stored length of a dictionary-index block with `count` elements at
/// `width` bytes each, or `None` on overflow.
pub(crate) fn index_block_len(count: usize, width: usize) -> Option<usize> {
    count
        .checked_mul(width)
        .and_then(|bytes| bytes.checked_add(DICT_INDEX_HEADER_LEN))
}

/// Decodes a dictionary-index block into absolute ids, rejecting any id that
/// does not refer to one of the `dictionary_len` entries decoded so far.
pub(crate) fn decode_indices(
    encoded: &[u8],
    count: usize,
    dictionary_len: usize,
    output: &mut Vec<u32>,
) -> Result<(), FormatError> {
    let header = encoded
        .get(..DICT_INDEX_HEADER_LEN)
        .ok_or(FormatError::Invalid("truncated dictionary index header"))?;
    let width = usize::from(header[0]);
    if !matches!(width, 1 | 2 | 4) {
        return Err(FormatError::Invalid("invalid dictionary index width"));
    }
    if header[1..4] != [0, 0, 0] {
        return Err(FormatError::Invalid(
            "nonzero dictionary index reserved bytes",
        ));
    }
    let base = u64::from(read_u32(header, 4)?);
    if index_block_len(count, width) != Some(encoded.len()) {
        return Err(FormatError::Invalid(
            "dictionary index block length mismatch",
        ));
    }
    let payload = &encoded[DICT_INDEX_HEADER_LEN..];
    output.clear();
    output
        .try_reserve(count)
        .map_err(|_| FormatError::Limit("dictionary index allocation"))?;
    let mut high = 0_u64;
    match width {
        1 => {
            for &stored in payload {
                high = high.max(u64::from(stored));
                output.push(stored.into());
            }
        }
        2 => {
            for pair in payload.chunks_exact(2) {
                let stored = u16::from_le_bytes([pair[0], pair[1]]);
                high = high.max(u64::from(stored));
                output.push(stored.into());
            }
        }
        _ => {
            for quad in payload.chunks_exact(4) {
                let stored = u32::from_le_bytes([quad[0], quad[1], quad[2], quad[3]]);
                high = high.max(u64::from(stored));
                output.push(stored);
            }
        }
    }
    if count != 0 && (base + high >= dictionary_len as u64 || base + high > u64::from(u32::MAX)) {
        return Err(FormatError::Invalid(
            "dictionary index refers to an undefined entry",
        ));
    }
    // Every `base + stored` is at most `base + high`, checked above to fit a
    // `u32`, so these additions cannot overflow.
    let base = base as u32;
    if base != 0 {
        for id in output.iter_mut() {
            *id += base;
        }
    }
    Ok(())
}

/// One decoded block of a character object, as handed to a reader's sink.
pub(crate) enum StringChunk<'a> {
    /// Elements to append to the output.
    Plain(&'a [StringRef<'a>]),
    /// Entries to append to the dictionary.
    DictionaryEntries(&'a [StringRef<'a>]),
    /// Elements to append, as ids into the entries seen so far.
    DictionaryIndices(&'a [u32]),
}

/// Pointer-identity hasher for dictionary keys. Addresses are aligned, so the
/// low bits alone would collide; `fmix64` from MurmurHash3 spreads them.
#[derive(Default)]
pub(crate) struct IdentityHasher(u64);

impl Hasher for IdentityHasher {
    fn write(&mut self, bytes: &[u8]) {
        for &byte in bytes {
            self.0 = (self.0 ^ u64::from(byte)).wrapping_mul(0x0100_0000_01b3);
        }
    }

    fn write_usize(&mut self, value: usize) {
        let mut hash = value as u64;
        hash ^= hash >> 33;
        hash = hash.wrapping_mul(0xff51_afd7_ed55_8ccd);
        hash ^= hash >> 33;
        hash = hash.wrapping_mul(0xc4ce_b9fe_1a85_ec53);
        hash ^= hash >> 33;
        self.0 = hash;
    }

    fn finish(&self) -> u64 {
        self.0
    }
}

/// A decoded string that borrows its bytes from the encoded block, so a block
/// can be handed to R without one heap allocation per element.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum StringRef<'a> {
    Na,
    Value {
        encoding: StringEncoding,
        bytes: &'a [u8],
    },
}

impl StringRef<'_> {
    pub(crate) fn to_value(self) -> Result<StringValue, FormatError> {
        match self {
            Self::Na => Ok(StringValue::Na),
            Self::Value { encoding, bytes } => {
                let mut owned = try_vec_with_capacity(bytes.len(), "character value allocation")?;
                owned.extend_from_slice(bytes);
                Ok(StringValue::Value {
                    encoding,
                    bytes: owned,
                })
            }
        }
    }
}

pub(crate) fn decode_block_refs(
    encoded: &[u8],
    logical_count: usize,
) -> Result<Vec<StringRef<'_>>, FormatError> {
    let mut output = try_vec_with_capacity(logical_count, "character values")?;
    let mut offset = 0_usize;
    for _ in 0..logical_count {
        let tag = *encoded
            .get(offset)
            .ok_or(FormatError::Invalid("truncated character record"))?;
        let length = read_u32(encoded, offset + 1)? as usize;
        offset = offset
            .checked_add(RECORD_HEADER_LEN)
            .ok_or(FormatError::Invalid("character offset overflow"))?;
        let end = offset
            .checked_add(length)
            .ok_or(FormatError::Invalid("character offset overflow"))?;
        let value = encoded
            .get(offset..end)
            .ok_or(FormatError::Invalid("truncated character bytes"))?;
        if tag == STRING_NA {
            if length != 0 {
                return Err(FormatError::Invalid(
                    "missing character record has nonzero length",
                ));
            }
            output.push(StringRef::Na);
        } else {
            output.push(StringRef::Value {
                encoding: StringEncoding::from_wire(tag)?,
                bytes: value,
            });
        }
        offset = end;
    }
    if offset != encoded.len() {
        return Err(FormatError::Invalid(
            "character block has trailing record bytes",
        ));
    }
    Ok(output)
}

pub(crate) fn decode_block(
    encoded: &[u8],
    logical_count: usize,
) -> Result<Vec<StringValue>, FormatError> {
    decode_block_refs(encoded, logical_count)?
        .into_iter()
        .map(StringRef::to_value)
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn encoded_strings_round_trip_na_and_encoding_tags() -> Result<(), FormatError> {
        let input = vec![
            StringValue::Na,
            StringValue::Value {
                encoding: StringEncoding::Utf8,
                bytes: "Grüezi".as_bytes().to_vec(),
            },
            StringValue::Value {
                encoding: StringEncoding::Bytes,
                bytes: vec![0xff, 0x00],
            },
        ];
        let blocks = encode_blocks(&input)?;
        assert_eq!(blocks.len(), 1);
        assert_eq!(
            decode_block(&blocks[0].bytes, blocks[0].logical_count as usize)?,
            input
        );
        Ok(())
    }

    #[test]
    fn malformed_character_records_are_rejected() {
        assert!(decode_block(&[StringEncoding::Utf8 as u8, 4, 0, 0, 0, b'x'], 1).is_err());
        assert!(decode_block(&[STRING_NA, 1, 0, 0, 0, 0], 1).is_err());
    }

    /// A source whose keys follow value identity, as CHARSXP addresses do.
    struct Keyed {
        keys: Vec<usize>,
        values: Vec<StringValue>,
    }

    impl Keyed {
        /// Element `i` is distinct value `pick(i)`, rendered by `render`.
        fn new(
            length: usize,
            pick: impl Fn(usize) -> usize,
            render: impl Fn(usize) -> StringValue,
        ) -> Self {
            let keys: Vec<usize> = (0..length).map(pick).collect();
            let values = keys.iter().map(|&key| render(key)).collect();
            Self { keys, values }
        }
    }

    impl StringSource for Keyed {
        fn len(&self) -> usize {
            self.keys.len()
        }

        fn key(&self, index: usize) -> usize {
            self.keys[index]
        }

        fn value(&self, index: usize) -> Result<StringRef<'_>, FormatError> {
            self.values.value(index)
        }
    }

    fn mixed_value(key: usize) -> StringValue {
        match key % 5 {
            0 => StringValue::Na,
            1 => StringValue::Value {
                encoding: StringEncoding::Utf8,
                bytes: format!("ü-{key}").into_bytes(),
            },
            2 => StringValue::Value {
                encoding: StringEncoding::Latin1,
                bytes: vec![0xe4, b'0' + (key % 10) as u8],
            },
            3 => StringValue::Value {
                encoding: StringEncoding::Bytes,
                bytes: vec![0xff, (key % 251) as u8],
            },
            _ => StringValue::Value {
                encoding: StringEncoding::Native,
                bytes: format!("k{key}").into_bytes(),
            },
        }
    }

    struct Encoded {
        blocks: Vec<(u16, u64, Vec<u8>)>,
    }

    impl Encoded {
        fn of<S: StringSource + ?Sized>(
            source: &S,
            policy: DictionaryPolicy,
        ) -> Result<Self, FormatError> {
            let mut blocks = Vec::new();
            encode_values(source, policy, |encoding, count, payload| {
                assert!(payload.len() <= BLOCK_SIZE);
                blocks.push((encoding, count, payload.to_vec()));
                Ok(())
            })?;
            Ok(Self { blocks })
        }

        fn entries(&self) -> u64 {
            self.blocks
                .iter()
                .filter(|block| block.0 == ENCODING_STRING_DICT_ENTRIES)
                .map(|block| block.1)
                .sum()
        }

        /// Mirrors the container reader's single forward pass.
        fn decode(&self) -> Result<Vec<StringValue>, FormatError> {
            let mut output = Vec::new();
            let mut dictionary = Vec::new();
            let mut ids = Vec::new();
            for (encoding, count, payload) in &self.blocks {
                let count = *count as usize;
                match *encoding {
                    ENCODING_STRING_PLAIN => output.extend(decode_block(payload, count)?),
                    ENCODING_STRING_DICT_ENTRIES => {
                        dictionary.extend(decode_block(payload, count)?)
                    }
                    ENCODING_STRING_DICT_INDICES => {
                        decode_indices(payload, count, dictionary.len(), &mut ids)?;
                        output.extend(ids.iter().map(|&id| dictionary[id as usize].clone()));
                    }
                    other => panic!("unexpected encoding {other}"),
                }
            }
            Ok(output)
        }
    }

    #[test]
    fn every_policy_round_trips_at_chunk_boundaries() -> Result<(), FormatError> {
        let chunk = DICT_CHUNK_VALUES;
        for length in [0, 1, chunk - 1, chunk, chunk + 1, 3 * chunk + 7] {
            let source = Keyed::new(length, |index| (index * 7) % 13, mixed_value);
            for policy in [
                DictionaryPolicy::Plain,
                DictionaryPolicy::Block,
                DictionaryPolicy::Global,
                DictionaryPolicy::Auto,
            ] {
                let encoded = Encoded::of(&source, policy)?;
                assert_eq!(
                    encoded.decode()?,
                    source.values,
                    "{policy:?} at length {length}"
                );
            }
        }
        Ok(())
    }

    #[test]
    fn global_dictionaries_store_each_value_once_and_blocks_restart() -> Result<(), FormatError> {
        let length = 3 * DICT_CHUNK_VALUES;
        let source = Keyed::new(length, |index| index % 100, mixed_value);
        assert_eq!(
            Encoded::of(&source, DictionaryPolicy::Global)?.entries(),
            100
        );
        assert_eq!(
            Encoded::of(&source, DictionaryPolicy::Block)?.entries(),
            300
        );
        let plain = Encoded::of(&source, DictionaryPolicy::Plain)?;
        assert!(
            plain
                .blocks
                .iter()
                .all(|block| block.0 == ENCODING_STRING_PLAIN)
        );
        Ok(())
    }

    #[test]
    fn index_width_follows_the_block_range_not_the_dictionary_size() -> Result<(), FormatError> {
        // All unique: ids grow to 70_000, but each chunk spans < 65_536.
        let unique = Keyed::new(70_000, |index| index, mixed_value);
        let encoded = Encoded::of(&unique, DictionaryPolicy::Global)?;
        let widths: Vec<u8> = encoded
            .blocks
            .iter()
            .filter(|block| block.0 == ENCODING_STRING_DICT_INDICES)
            .map(|block| block.2[0])
            .collect();
        assert_eq!(widths, vec![2, 2]);
        assert_eq!(encoded.decode()?, unique.values);

        let small = Keyed::new(1_000, |index| index % 200, mixed_value);
        let encoded = Encoded::of(&small, DictionaryPolicy::Global)?;
        assert!(
            encoded
                .blocks
                .iter()
                .any(|block| block.0 == ENCODING_STRING_DICT_INDICES && block.2[0] == 1)
        );
        Ok(())
    }

    /// Keys only; enough for the estimator.
    struct Keys<F: Fn(usize) -> usize> {
        length: usize,
        key: F,
    }

    impl<F: Fn(usize) -> usize> StringSource for Keys<F> {
        fn len(&self) -> usize {
            self.length
        }

        fn key(&self, index: usize) -> usize {
            (self.key)(index)
        }

        fn value(&self, _index: usize) -> Result<StringRef<'_>, FormatError> {
            Ok(StringRef::Na)
        }
    }

    #[test]
    fn distinct_share_estimates_are_order_independent() {
        let length = 1_000_000;
        let scatter = |index: usize| index.wrapping_mul(0x9e37_79b9) % length;
        for share in [0.0001, 0.01, 0.1, 0.5, 1.0] {
            let distinct = ((length as f64 * share) as usize).max(1);
            let shuffled = Keys {
                length,
                key: |index| scatter(index) % distinct,
            };
            let sorted = Keys {
                length,
                key: |index| index * distinct / length,
            };
            for (order, estimate) in [
                ("shuffled", estimated_distinct_share(&shuffled)),
                ("sorted", estimated_distinct_share(&sorted)),
            ] {
                assert!(
                    (estimate - share).abs() <= 0.05 + 0.25 * share,
                    "{order} share {share}: estimated {estimate}"
                );
            }
        }
        let small = Keys {
            length: 1_000,
            key: |index| index % 250,
        };
        assert_eq!(estimated_distinct_share(&small), 0.25);
    }

    #[test]
    fn auto_uses_a_dictionary_only_when_values_repeat() -> Result<(), FormatError> {
        let length = 2 * DICT_CHUNK_VALUES + 5;
        let kinds = |encoded: &Encoded| -> Vec<u16> {
            encoded.blocks.iter().map(|block| block.0).collect()
        };

        let unique = Keyed::new(length, |index| index, mixed_value);
        let encoded = Encoded::of(&unique, DictionaryPolicy::Auto)?;
        assert!(
            kinds(&encoded)
                .iter()
                .all(|&kind| kind == ENCODING_STRING_PLAIN)
        );
        assert_eq!(encoded.decode()?, unique.values);

        let repeated = Keyed::new(length, |index| index % 1_000, mixed_value);
        let encoded = Encoded::of(&repeated, DictionaryPolicy::Auto)?;
        assert!(!kinds(&encoded).contains(&ENCODING_STRING_PLAIN));
        assert_eq!(encoded.entries(), 1_000);
        Ok(())
    }

    #[test]
    fn a_full_dictionary_writes_the_rest_plain() -> Result<(), FormatError> {
        let length = 3 * DICT_CHUNK_VALUES;
        let source = Keyed::new(
            length,
            |index| index % 5_000 + index / DICT_CHUNK_VALUES * 5_000,
            mixed_value,
        );
        let mut blocks = Vec::new();
        encode_values_capped(
            &source,
            DictionaryPolicy::Global,
            7_000,
            |encoding, count, payload| {
                blocks.push((encoding, count, payload.to_vec()));
                Ok(())
            },
        )?;
        let encoded = Encoded { blocks };
        let kinds: Vec<u16> = encoded.blocks.iter().map(|block| block.0).collect();
        assert_eq!(kinds.first(), Some(&ENCODING_STRING_DICT_ENTRIES));
        assert_eq!(kinds.last(), Some(&ENCODING_STRING_PLAIN));
        assert_eq!(encoded.entries(), 5_000);
        assert_eq!(encoded.decode()?, source.values);
        Ok(())
    }

    #[test]
    fn large_new_entries_split_into_bounded_entry_blocks() -> Result<(), FormatError> {
        let wide = |key: usize| StringValue::Value {
            encoding: StringEncoding::Native,
            bytes: format!("{key:06}{}", "x".repeat(5_000)).into_bytes(),
        };
        let source = Keyed::new(600, |index| index % 300, wide);
        let encoded = Encoded::of(&source, DictionaryPolicy::Global)?;
        let entry_blocks = encoded
            .blocks
            .iter()
            .filter(|block| block.0 == ENCODING_STRING_DICT_ENTRIES)
            .count();
        assert!(entry_blocks >= 2);
        assert_eq!(encoded.decode()?, source.values);
        Ok(())
    }

    #[test]
    fn malformed_dictionary_indices_are_rejected() {
        let mut ids = Vec::new();
        let header = |width: u8, base: u32| {
            let mut bytes = vec![width, 0, 0, 0];
            bytes.extend_from_slice(&base.to_le_bytes());
            bytes
        };
        let with = |mut bytes: Vec<u8>, tail: &[u8]| {
            bytes.extend_from_slice(tail);
            bytes
        };
        // Valid: base 5 plus stored 0 and 2 against a 10-entry dictionary.
        assert!(decode_indices(&with(header(1, 5), &[0, 2]), 2, 10, &mut ids).is_ok());
        assert_eq!(ids, vec![5, 7]);
        assert!(decode_indices(&with(header(3, 0), &[0, 0, 0]), 1, 10, &mut ids).is_err());
        assert!(
            decode_indices(&with(vec![1, 0, 1, 0, 0, 0, 0, 0], &[0]), 1, 10, &mut ids).is_err()
        );
        assert!(decode_indices(&with(header(1, 0), &[0]), 2, 10, &mut ids).is_err());
        assert!(decode_indices(&with(header(1, 0), &[10]), 1, 10, &mut ids).is_err());
        assert!(decode_indices(&with(header(1, 9), &[1]), 1, 10, &mut ids).is_err());
        assert!(
            decode_indices(
                &with(header(4, u32::MAX), &[1, 0, 0, 0]),
                1,
                usize::MAX,
                &mut ids
            )
            .is_err()
        );
        assert!(decode_indices(&[1, 0, 0], 0, 10, &mut ids).is_err());
    }
}
