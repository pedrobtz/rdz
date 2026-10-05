use std::io::{BufWriter, Write};
use std::path::Path;

use crate::atomic_file::AtomicFile;
use crate::checksum::crc32;
use crate::codec::{logical, string};
use crate::format::{
    ATTRIBUTE_ENTRY_LEN, ATTRIBUTE_FLAG_NAMES, BLOCK_HEADER_LEN, BLOCK_MAGIC, BLOCK_SIZE,
    CLOSING_MAGIC, CODEC_NATIVE_V1, CODEC_R_SERIAL_V3, CONTAINER_VERSION, DIRECTORY_ENTRY_LEN,
    DIRECTORY_HEADER_LEN, DIRECTORY_MAGIC, DIRECTORY_VERSION, ENCODING_STRING_PLAIN, FILE_MAGIC,
    FormatError, HEADER_LEN, LOGICAL_BLOCK_VALUES, MAX_BLOCKS, MAX_SYNOPSIS_LEN,
    NATIVE_CODEC_VERSION, OBJECT_ENTRY_LEN, R_SERIAL_CODEC_VERSION, ROLE_ATTRIBUTE_NAME,
    ROLE_ATTRIBUTE_VALUE, ROLE_ROOT, TRAILER_LEN, TRAILER_MAGIC, TYPE_CHARACTER, TYPE_LOGICAL,
    put_u16, put_u32, put_u64, try_vec_with_capacity, try_zeroed_vec,
};

use super::directory::{AttributeEntry, BlockEntry, ObjectEntry, ROOT_PARENT_ID};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Codec {
    RSerialV3,
    NativeV1,
}

struct PositionWriter<W> {
    inner: W,
    position: u64,
}

impl<W> PositionWriter<W> {
    fn new(inner: W) -> Self {
        Self { inner, position: 0 }
    }

    fn position(&self) -> u64 {
        self.position
    }
}

impl<W: Write> Write for PositionWriter<W> {
    fn write(&mut self, buffer: &[u8]) -> std::io::Result<usize> {
        let written = self.inner.write(buffer)?;
        self.position = self
            .position
            .checked_add(written as u64)
            .ok_or_else(|| std::io::Error::other("output position overflow"))?;
        Ok(written)
    }

    fn flush(&mut self) -> std::io::Result<()> {
        self.inner.flush()
    }
}

impl Codec {
    fn id(self) -> u16 {
        match self {
            Self::RSerialV3 => CODEC_R_SERIAL_V3,
            Self::NativeV1 => CODEC_NATIVE_V1,
        }
    }

    fn version(self) -> u16 {
        match self {
            Self::RSerialV3 => R_SERIAL_CODEC_VERSION,
            Self::NativeV1 => NATIVE_CODEC_VERSION,
        }
    }
}

pub(crate) fn write_generic(
    path: &Path,
    payload: &[u8],
    synopsis: &[u8],
) -> Result<(), FormatError> {
    write_container(path, Codec::RSerialV3, payload, synopsis)
}

pub(crate) fn write_native_logical<S>(
    path: &Path,
    values: &[i32],
    na_value: i32,
    names: Option<&S>,
    policy: string::DictionaryPolicy,
) -> Result<(), FormatError>
where
    S: string::StringSource + ?Sized,
{
    if let Some(names) = names
        && names.len() != values.len()
    {
        return Err(FormatError::Invalid(
            "names length does not match logical length",
        ));
    }

    let mut output = AtomicFile::create(path)?;
    {
        let file = output.file_mut()?;
        let mut file = PositionWriter::new(BufWriter::with_capacity(BLOCK_SIZE, file));
        file.write_all(&encode_header(Codec::NativeV1))?;
        let mut blocks = Vec::new();

        let logical_block_start = 0_u32;
        for chunk in values.chunks(LOGICAL_BLOCK_VALUES) {
            let encoded = logical::encode_adaptive(chunk, na_value)?;
            write_encoded_block(
                &mut file,
                chunk.len() as u64,
                encoded.encoding,
                &encoded.payload,
                &mut blocks,
            )?;
        }
        if values.is_empty() {
            let encoded = logical::encode_adaptive(&[], na_value)?;
            write_encoded_block(
                &mut file,
                0,
                encoded.encoding,
                &encoded.payload,
                &mut blocks,
            )?;
        }
        let logical_block_count =
            u32::try_from(blocks.len()).map_err(|_| FormatError::Limit("logical block count"))?;

        let mut objects = Vec::new();
        let mut attributes = Vec::new();
        let root_attribute_count = u32::from(names.is_some());
        objects.push(ObjectEntry {
            object_id: 0,
            parent_id: ROOT_PARENT_ID,
            role: ROLE_ROOT,
            type_tag: TYPE_LOGICAL,
            flags: 0,
            logical_len: values.len() as u64,
            first_child: 0,
            child_count: 0,
            first_attribute: 0,
            attribute_count: root_attribute_count,
            first_block: logical_block_start,
            block_count: logical_block_count,
        });

        if let Some(names) = names {
            let attribute_name = [string::StringValue::Value {
                encoding: string::StringEncoding::Native,
                bytes: b"names".to_vec(),
            }];
            let name_blocks = string::encode_blocks(&attribute_name)?;
            let name_first_block =
                u32::try_from(blocks.len()).map_err(|_| FormatError::Limit("block count"))?;
            for block in &name_blocks {
                write_encoded_block(
                    &mut file,
                    block.logical_count,
                    ENCODING_STRING_PLAIN,
                    &block.bytes,
                    &mut blocks,
                )?;
            }

            let value_first_block =
                u32::try_from(blocks.len()).map_err(|_| FormatError::Limit("block count"))?;
            string::encode_values(names, policy, |encoding, count, payload| {
                write_encoded_block(&mut file, count, encoding, payload, &mut blocks)
            })?;
            let value_block_count = u32::try_from(blocks.len())
                .map_err(|_| FormatError::Limit("block count"))?
                - value_first_block;

            objects.push(ObjectEntry {
                object_id: 1,
                parent_id: 0,
                role: ROLE_ATTRIBUTE_NAME,
                type_tag: TYPE_CHARACTER,
                flags: 0,
                logical_len: 1,
                first_child: 0,
                child_count: 0,
                first_attribute: 0,
                attribute_count: 0,
                first_block: name_first_block,
                block_count: name_blocks.len() as u32,
            });
            objects.push(ObjectEntry {
                object_id: 2,
                parent_id: 0,
                role: ROLE_ATTRIBUTE_VALUE,
                type_tag: TYPE_CHARACTER,
                flags: 0,
                logical_len: names.len() as u64,
                first_child: 0,
                child_count: 0,
                first_attribute: 0,
                attribute_count: 0,
                first_block: value_first_block,
                block_count: value_block_count,
            });
            attributes.push(AttributeEntry {
                owner_id: 0,
                name_object_id: 1,
                value_object_id: 2,
                ordinal: 0,
                flags: ATTRIBUTE_FLAG_NAMES,
            });
        }

        finish_container(&mut file, &objects, &attributes, &blocks, &[])?;
        file.flush()?;
    }
    output.commit()
}

pub(crate) fn write_container(
    path: &Path,
    codec: Codec,
    payload: &[u8],
    synopsis: &[u8],
) -> Result<(), FormatError> {
    if synopsis.len() > MAX_SYNOPSIS_LEN {
        return Err(FormatError::Limit("generic synopsis"));
    }

    let block_count = if payload.is_empty() {
        1_usize
    } else {
        payload.len().div_ceil(BLOCK_SIZE)
    };
    if block_count > MAX_BLOCKS as usize {
        return Err(FormatError::Limit("block count"));
    }

    let mut output = AtomicFile::create(path)?;
    {
        let file = output.file_mut()?;
        let mut file = PositionWriter::new(BufWriter::with_capacity(BLOCK_SIZE, file));
        file.write_all(&encode_header(codec))?;

        let mut entries = try_vec_with_capacity(block_count, "block directory")?;
        for (index, chunk) in payload.chunks(BLOCK_SIZE).enumerate() {
            write_block(&mut file, index as u32, chunk, &mut entries)?;
        }
        if payload.is_empty() {
            write_block(&mut file, 0, &[], &mut entries)?;
        }

        finish_container(&mut file, &[], &[], &entries, synopsis)?;
        file.flush()?;
    }
    output.commit()
}

fn finish_container<W: Write>(
    file: &mut PositionWriter<W>,
    objects: &[ObjectEntry],
    attributes: &[AttributeEntry],
    blocks: &[BlockEntry],
    synopsis: &[u8],
) -> Result<(), FormatError> {
    let directory_offset = file.position();
    let directory = encode_directory(objects, attributes, blocks, synopsis)?;
    let directory_checksum = crc32(&directory);
    file.write_all(&directory)?;
    let directory_len =
        u64::try_from(directory.len()).map_err(|_| FormatError::Limit("directory"))?;
    file.write_all(&encode_trailer(
        directory_offset,
        directory_len,
        directory_checksum,
    ))?;
    Ok(())
}

fn write_block<W: Write>(
    output: &mut PositionWriter<W>,
    sequence: u32,
    payload: &[u8],
    entries: &mut Vec<BlockEntry>,
) -> Result<(), FormatError> {
    let block_header_offset = output.position();
    let payload_offset = block_header_offset
        .checked_add(BLOCK_HEADER_LEN as u64)
        .ok_or(FormatError::Invalid("block offset overflow"))?;
    let length = u64::try_from(payload.len()).map_err(|_| FormatError::Limit("block size"))?;
    let checksum = crc32(payload);
    let entry = BlockEntry::raw(
        sequence,
        block_header_offset,
        payload_offset,
        length,
        checksum,
    )?;
    output.write_all(&encode_block_header(&entry))?;
    output.write_all(payload)?;
    entries.push(entry);
    Ok(())
}

fn write_encoded_block<W: Write>(
    output: &mut PositionWriter<W>,
    logical_count: u64,
    encoding: u16,
    payload: &[u8],
    entries: &mut Vec<BlockEntry>,
) -> Result<(), FormatError> {
    entries
        .try_reserve(1)
        .map_err(|_| FormatError::Limit("block directory allocation"))?;
    let sequence = u32::try_from(entries.len()).map_err(|_| FormatError::Limit("block count"))?;
    if sequence >= MAX_BLOCKS {
        return Err(FormatError::Limit("block count"));
    }
    let block_header_offset = output.position();
    let payload_offset = block_header_offset
        .checked_add(BLOCK_HEADER_LEN as u64)
        .ok_or(FormatError::Invalid("block offset overflow"))?;
    let checksum = crc32(payload);
    let entry = BlockEntry::encoded(
        sequence,
        block_header_offset,
        payload_offset,
        logical_count,
        encoding,
        payload,
        checksum,
    )?;
    output.write_all(&encode_block_header(&entry))?;
    output.write_all(payload)?;
    entries.push(entry);
    Ok(())
}

fn encode_header(codec: Codec) -> [u8; HEADER_LEN] {
    let mut output = [0_u8; HEADER_LEN];
    output[0..4].copy_from_slice(FILE_MAGIC);
    put_u16(&mut output, 4, CONTAINER_VERSION);
    put_u16(&mut output, 6, HEADER_LEN as u16);
    put_u32(&mut output, 8, 0);
    put_u16(&mut output, 12, codec.id());
    put_u16(&mut output, 14, codec.version());
    put_u32(&mut output, 16, BLOCK_SIZE as u32);
    put_u32(&mut output, 20, 0);
    let checksum = crc32(&output[..24]);
    put_u32(&mut output, 24, checksum);
    put_u32(&mut output, 28, 0);
    output
}

fn encode_block_header(entry: &BlockEntry) -> [u8; BLOCK_HEADER_LEN] {
    let mut output = [0_u8; BLOCK_HEADER_LEN];
    output[0..4].copy_from_slice(BLOCK_MAGIC);
    put_u16(&mut output, 4, BLOCK_HEADER_LEN as u16);
    put_u16(&mut output, 6, 0);
    put_u32(&mut output, 8, entry.sequence);
    put_u16(&mut output, 12, entry.encoding);
    put_u16(&mut output, 14, entry.compression);
    put_u64(&mut output, 16, entry.logical_count);
    put_u64(&mut output, 24, entry.decoded_len);
    put_u32(&mut output, 32, entry.stored_len);
    put_u32(&mut output, 36, entry.checksum);
    output
}

fn encode_directory(
    objects: &[ObjectEntry],
    attributes: &[AttributeEntry],
    entries: &[BlockEntry],
    synopsis: &[u8],
) -> Result<Vec<u8>, FormatError> {
    let object_count =
        u32::try_from(objects.len()).map_err(|_| FormatError::Limit("object count"))?;
    let attribute_count =
        u32::try_from(attributes.len()).map_err(|_| FormatError::Limit("attribute count"))?;
    let entry_count =
        u32::try_from(entries.len()).map_err(|_| FormatError::Limit("block count"))?;
    let synopsis_len = u32::try_from(synopsis.len()).map_err(|_| FormatError::Limit("synopsis"))?;
    let object_bytes = objects
        .len()
        .checked_mul(OBJECT_ENTRY_LEN)
        .ok_or(FormatError::Limit("directory"))?;
    let attribute_bytes = attributes
        .len()
        .checked_mul(ATTRIBUTE_ENTRY_LEN)
        .ok_or(FormatError::Limit("directory"))?;
    let entry_bytes = entries
        .len()
        .checked_mul(DIRECTORY_ENTRY_LEN)
        .ok_or(FormatError::Limit("directory"))?;
    let capacity = DIRECTORY_HEADER_LEN
        .checked_add(object_bytes)
        .and_then(|value| value.checked_add(attribute_bytes))
        .and_then(|value| value.checked_add(entry_bytes))
        .and_then(|value| value.checked_add(synopsis.len()))
        .ok_or(FormatError::Limit("directory"))?;
    let mut output = try_zeroed_vec(capacity, "directory")?;
    output[0..4].copy_from_slice(DIRECTORY_MAGIC);
    put_u16(&mut output, 4, DIRECTORY_VERSION);
    put_u16(&mut output, 6, DIRECTORY_HEADER_LEN as u16);
    put_u16(&mut output, 8, OBJECT_ENTRY_LEN as u16);
    put_u16(&mut output, 10, ATTRIBUTE_ENTRY_LEN as u16);
    put_u16(&mut output, 12, DIRECTORY_ENTRY_LEN as u16);
    put_u16(&mut output, 14, 0);
    put_u32(&mut output, 16, object_count);
    put_u32(&mut output, 20, attribute_count);
    put_u32(&mut output, 24, entry_count);
    put_u32(&mut output, 28, synopsis_len);
    let header_checksum = crc32(&output[..32]);
    put_u32(&mut output, 32, header_checksum);

    let mut table_offset = DIRECTORY_HEADER_LEN;
    for object in objects {
        output[table_offset..table_offset + OBJECT_ENTRY_LEN].copy_from_slice(&object.encode());
        table_offset += OBJECT_ENTRY_LEN;
    }
    for attribute in attributes {
        output[table_offset..table_offset + ATTRIBUTE_ENTRY_LEN]
            .copy_from_slice(&attribute.encode());
        table_offset += ATTRIBUTE_ENTRY_LEN;
    }
    for entry in entries {
        let offset = table_offset;
        put_u32(&mut output, offset, entry.sequence);
        put_u32(&mut output, offset + 4, entry.flags);
        put_u64(&mut output, offset + 8, entry.block_header_offset);
        put_u64(&mut output, offset + 16, entry.payload_offset);
        put_u32(&mut output, offset + 24, entry.stored_len);
        put_u32(&mut output, offset + 28, 0);
        put_u64(&mut output, offset + 32, entry.logical_count);
        put_u64(&mut output, offset + 40, entry.decoded_len);
        put_u16(&mut output, offset + 48, entry.encoding);
        put_u16(&mut output, offset + 50, entry.compression);
        put_u32(&mut output, offset + 52, entry.checksum);
        table_offset += DIRECTORY_ENTRY_LEN;
    }
    output[table_offset..].copy_from_slice(synopsis);
    Ok(output)
}

fn encode_trailer(
    directory_offset: u64,
    directory_len: u64,
    directory_checksum: u32,
) -> [u8; TRAILER_LEN] {
    let mut output = [0_u8; TRAILER_LEN];
    output[0..4].copy_from_slice(TRAILER_MAGIC);
    put_u16(&mut output, 4, CONTAINER_VERSION);
    put_u16(&mut output, 6, TRAILER_LEN as u16);
    put_u64(&mut output, 8, directory_offset);
    put_u64(&mut output, 16, directory_len);
    put_u32(&mut output, 24, directory_checksum);
    output[28..32].copy_from_slice(CLOSING_MAGIC);
    output
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::format::{MAX_BLOCK_SIZE, read_u32};

    // Regression for review.md finding 4: stored lengths outside the shared
    // u32 wire representation must be rejected rather than truncated.
    #[test]
    fn stored_lengths_that_do_not_fit_the_wire_are_rejected() {
        let stored_len = u64::from(u32::MAX) + 1;
        let entry = BlockEntry::raw(
            0,
            HEADER_LEN as u64,
            (HEADER_LEN + BLOCK_HEADER_LEN) as u64,
            stored_len,
            0,
        );
        assert!(matches!(entry, Err(FormatError::Limit("block size"))));
    }

    #[test]
    fn block_header_round_trips_the_largest_accepted_block() {
        let entry = BlockEntry::raw(
            0,
            HEADER_LEN as u64,
            (HEADER_LEN + BLOCK_HEADER_LEN) as u64,
            MAX_BLOCK_SIZE,
            0,
        )
        .expect("largest accepted block");

        let header = encode_block_header(&entry);

        assert_eq!(
            u64::from(read_u32(&header, 32).expect("stored length field")),
            MAX_BLOCK_SIZE
        );
    }
}
