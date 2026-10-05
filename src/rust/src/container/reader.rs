use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

use crate::checksum::crc32;
use crate::codec::{logical, string};
use crate::format::{
    ATTRIBUTE_ENTRY_LEN, ATTRIBUTE_FLAG_NAMES, BLOCK_HEADER_LEN, BLOCK_MAGIC, CLOSING_MAGIC,
    CODEC_NATIVE_V1, CODEC_R_SERIAL_V3, COMPRESSION_NONE, CONTAINER_VERSION, DIRECTORY_ENTRY_LEN,
    DIRECTORY_HEADER_LEN, DIRECTORY_MAGIC, DIRECTORY_VERSION, ENCODING_RAW,
    ENCODING_STRING_DICT_ENTRIES, ENCODING_STRING_DICT_INDICES, ENCODING_STRING_PLAIN, FILE_MAGIC,
    FormatError, HEADER_LEN, LOGICAL_BLOCK_VALUES, MAX_ATTRIBUTES, MAX_BLOCK_SIZE, MAX_BLOCKS,
    MAX_OBJECTS, MAX_SYNOPSIS_LEN, NATIVE_CODEC_VERSION, OBJECT_ENTRY_LEN, R_SERIAL_CODEC_VERSION,
    ROLE_ATTRIBUTE_NAME, ROLE_ATTRIBUTE_VALUE, ROLE_ROOT, TRAILER_LEN, TRAILER_MAGIC,
    TYPE_CHARACTER, TYPE_LOGICAL, checked_add, read_u16, read_u32, read_u64, to_usize,
    try_vec_with_capacity, try_zeroed_vec,
};

use super::directory::{AttributeEntry, BlockEntry, ObjectEntry, ROOT_PARENT_ID};

#[derive(Debug)]
pub(crate) struct ContainerInfo {
    pub(crate) container_version: u16,
    pub(crate) codec_id: u16,
    pub(crate) codec_version: u16,
    pub(crate) block_size: u32,
    pub(crate) block_count: u32,
    pub(crate) object_count: u32,
    pub(crate) attribute_count: u32,
    pub(crate) payload_bytes: u64,
    pub(crate) file_bytes: u64,
    pub(crate) synopsis: Vec<u8>,
    pub(crate) root_type: Option<&'static str>,
    pub(crate) root_length: Option<u64>,
    pub(crate) attribute_names: Vec<&'static str>,
}

struct ValidatedContainer {
    info: ContainerInfo,
    objects: Vec<ObjectEntry>,
    attributes: Vec<AttributeEntry>,
    blocks: Vec<BlockEntry>,
    directory_offset: u64,
}

pub(crate) struct ContainerReader {
    file: File,
    validated: ValidatedContainer,
}

pub(crate) fn read_info(path: &Path) -> Result<ContainerInfo, FormatError> {
    ContainerReader::open(path).map(|reader| reader.validated.info)
}

pub(crate) fn open(path: &Path) -> Result<ContainerReader, FormatError> {
    ContainerReader::open(path)
}

#[cfg(test)]
pub(crate) fn read_generic(path: &Path) -> Result<(Vec<u8>, ContainerInfo), FormatError> {
    let reader = ContainerReader::open(path)?;
    let mut payload = try_zeroed_vec(reader.generic_payload_len()?, "payload allocation")?;
    let info = reader.read_generic_into(&mut payload)?;
    Ok((payload, info))
}

impl ContainerReader {
    fn open(path: &Path) -> Result<Self, FormatError> {
        let mut file = File::open(path)?;
        let validated = validate_structure(&mut file)?;
        Ok(Self { file, validated })
    }

    pub(crate) fn codec_id(&self) -> u16 {
        self.validated.info.codec_id
    }

    pub(crate) fn generic_payload_len(&self) -> Result<usize, FormatError> {
        self.require_codec(CODEC_R_SERIAL_V3, R_SERIAL_CODEC_VERSION)?;
        to_usize(self.validated.info.payload_bytes)
    }

    pub(crate) fn native_logical_len(&self) -> Result<usize, FormatError> {
        self.require_codec(CODEC_NATIVE_V1, NATIVE_CODEC_VERSION)?;
        let root = self
            .validated
            .objects
            .first()
            .ok_or(FormatError::Invalid("native root object is missing"))?;
        to_usize(root.logical_len)
    }

    fn require_codec(&self, id: u16, version: u16) -> Result<(), FormatError> {
        if self.validated.info.codec_id == id && self.validated.info.codec_version == version {
            Ok(())
        } else {
            Err(FormatError::UnsupportedCodec {
                id: self.validated.info.codec_id,
                version: self.validated.info.codec_version,
            })
        }
    }

    pub(crate) fn read_generic_into(
        mut self,
        output: &mut [u8],
    ) -> Result<ContainerInfo, FormatError> {
        let expected_len = self.generic_payload_len()?;
        if output.len() != expected_len {
            return Err(FormatError::Invalid("decoded payload length mismatch"));
        }
        self.file.seek(SeekFrom::Start(HEADER_LEN as u64))?;
        let mut output_offset = 0_usize;
        for entry in &self.validated.blocks {
            validate_block_header_at_current(
                &mut self.file,
                entry,
                self.validated.directory_offset,
            )?;
            let stored_len = entry.stored_len as usize;
            let output_end = output_offset
                .checked_add(stored_len)
                .ok_or(FormatError::Invalid("payload offset overflow"))?;
            let block = output
                .get_mut(output_offset..output_end)
                .ok_or(FormatError::Invalid("decoded payload length mismatch"))?;
            self.file.read_exact(block)?;
            if crc32(block) != entry.checksum {
                return Err(checksum_error(entry.sequence));
            }
            output_offset = output_end;
        }
        if output_offset != expected_len {
            return Err(FormatError::Invalid("decoded payload length mismatch"));
        }
        Ok(self.validated.info)
    }

    /// Decodes the native logical values into `output` and returns any
    /// `names` attribute as owned values. Used by pure-Rust tests; the R
    /// boundary streams names with [`Self::read_native_names_with`].
    #[cfg(test)]
    pub(crate) fn read_native_logical_into(
        mut self,
        output: &mut [i32],
        na_value: i32,
    ) -> Result<Option<Vec<string::StringValue>>, FormatError> {
        self.read_native_logical_values(output, na_value)?;
        self.read_names_attribute()
    }

    pub(crate) fn read_native_logical_values(
        &mut self,
        output: &mut [i32],
        na_value: i32,
    ) -> Result<(), FormatError> {
        let expected_len = self.native_logical_len()?;
        if output.len() != expected_len {
            return Err(FormatError::Invalid("logical payload length mismatch"));
        }
        let root = self.validated.objects[0].clone();
        let range = object_block_range(&root)?;
        let first = self
            .validated
            .blocks
            .get(range.start)
            .ok_or(FormatError::Invalid("logical block range is empty"))?;
        self.file.seek(SeekFrom::Start(first.block_header_offset))?;
        let mut encoded = Vec::new();
        let mut output_offset = 0_usize;
        for index in range {
            let entry = &self.validated.blocks[index];
            read_block_at_current(
                &mut self.file,
                entry,
                self.validated.directory_offset,
                &mut encoded,
            )?;
            let count = to_usize(entry.logical_count)?;
            let end = output_offset
                .checked_add(count)
                .ok_or(FormatError::Invalid("logical output offset overflow"))?;
            let destination = output
                .get_mut(output_offset..end)
                .ok_or(FormatError::Invalid("logical payload length mismatch"))?;
            logical::decode_block_into(&encoded, entry.encoding, count, na_value, destination)?;
            output_offset = end;
        }
        if output_offset != expected_len {
            return Err(FormatError::Invalid("logical payload length mismatch"));
        }
        Ok(())
    }

    #[cfg(test)]
    pub(crate) fn read_native_names(
        mut self,
    ) -> Result<Option<Vec<string::StringValue>>, FormatError> {
        self.require_codec(CODEC_NATIVE_V1, NATIVE_CODEC_VERSION)?;
        self.read_names_attribute()
    }

    fn names_value_object(&self) -> Result<Option<&ObjectEntry>, FormatError> {
        self.require_codec(CODEC_NATIVE_V1, NATIVE_CODEC_VERSION)?;
        match self.validated.attributes.first() {
            None => Ok(None),
            Some(attribute) => self
                .validated
                .objects
                .get(attribute.value_object_id as usize)
                .map(Some)
                .ok_or(FormatError::Invalid("attribute value object is missing")),
        }
    }

    /// Length of the native `names` attribute, or `None` when absent, without
    /// reading any data block.
    pub(crate) fn native_names_len(&self) -> Result<Option<usize>, FormatError> {
        self.names_value_object()?
            .map(|object| to_usize(object.logical_len))
            .transpose()
    }

    /// Number of dictionary entries in the native `names` attribute, taken
    /// from the validated directory, so a reader can size its dictionary
    /// before decoding. Validation bounds it by the attribute's length.
    pub(crate) fn native_names_dictionary_len(&self) -> Result<usize, FormatError> {
        let Some(object) = self.names_value_object()? else {
            return Ok(0);
        };
        let mut entries = 0_u64;
        for index in object_block_range(object)? {
            let block = &self.validated.blocks[index];
            if block.encoding == ENCODING_STRING_DICT_ENTRIES {
                entries = entries
                    .checked_add(block.logical_count)
                    .ok_or(FormatError::Invalid("dictionary length overflow"))?;
            }
        }
        to_usize(entries)
    }

    /// Streams the native `names` attribute one block at a time. `sink`
    /// receives borrowed chunks whose bytes live only for that call. Every
    /// dictionary id it receives refers to an entry already delivered.
    pub(crate) fn read_native_names_with<E, F>(&mut self, mut sink: F) -> Result<bool, E>
    where
        E: From<FormatError>,
        F: FnMut(string::StringChunk<'_>) -> Result<(), E>,
    {
        self.require_codec(CODEC_NATIVE_V1, NATIVE_CODEC_VERSION)?;
        if self.validated.attributes.is_empty() {
            return Ok(false);
        }
        let name_object_id = self.validated.attributes[0].name_object_id;
        let value_object_id = self.validated.attributes[0].value_object_id;
        let name_object = self.validated.objects[name_object_id as usize].clone();
        let name = self.read_string_object(&name_object)?;
        if name
            != [string::StringValue::Value {
                encoding: string::StringEncoding::Native,
                bytes: b"names".to_vec(),
            }]
        {
            return Err(FormatError::Invalid("native attribute name is not names").into());
        }
        let value_object = self.validated.objects[value_object_id as usize].clone();
        self.for_each_string_chunk(&value_object, &mut sink)?;
        Ok(true)
    }

    fn for_each_string_chunk<E, F>(&mut self, object: &ObjectEntry, sink: &mut F) -> Result<(), E>
    where
        E: From<FormatError>,
        F: FnMut(string::StringChunk<'_>) -> Result<(), E>,
    {
        let length = to_usize(object.logical_len)?;
        let range = object_block_range(object)?;
        let first = self
            .validated
            .blocks
            .get(range.start)
            .ok_or(FormatError::Invalid("character block range is empty"))?;
        self.file
            .seek(SeekFrom::Start(first.block_header_offset))
            .map_err(FormatError::from)?;
        let mut encoded = Vec::new();
        let mut ids = Vec::new();
        let mut elements = 0_usize;
        let mut entries = 0_usize;
        for index in range {
            let entry = &self.validated.blocks[index];
            read_block_at_current(
                &mut self.file,
                entry,
                self.validated.directory_offset,
                &mut encoded,
            )?;
            let count = to_usize(entry.logical_count)?;
            match entry.encoding {
                ENCODING_STRING_PLAIN => {
                    let refs = string::decode_block_refs(&encoded, count)?;
                    sink(string::StringChunk::Plain(&refs))?;
                    elements = elements
                        .checked_add(count)
                        .ok_or(FormatError::Invalid("character object length overflow"))?;
                }
                ENCODING_STRING_DICT_ENTRIES => {
                    let refs = string::decode_block_refs(&encoded, count)?;
                    sink(string::StringChunk::DictionaryEntries(&refs))?;
                    entries = entries
                        .checked_add(count)
                        .ok_or(FormatError::Invalid("dictionary length overflow"))?;
                }
                ENCODING_STRING_DICT_INDICES => {
                    string::decode_indices(&encoded, count, entries, &mut ids)?;
                    sink(string::StringChunk::DictionaryIndices(&ids))?;
                    elements = elements
                        .checked_add(count)
                        .ok_or(FormatError::Invalid("character object length overflow"))?;
                }
                _ => return Err(FormatError::Invalid("unexpected character block encoding").into()),
            }
        }
        if elements != length {
            return Err(FormatError::Invalid("character object length mismatch").into());
        }
        Ok(())
    }

    #[cfg(test)]
    fn read_names_attribute(&mut self) -> Result<Option<Vec<string::StringValue>>, FormatError> {
        let length = match self.native_names_len()? {
            None => return Ok(None),
            Some(length) => length,
        };
        let mut output = try_vec_with_capacity(length, "character object allocation")?;
        let mut dictionary: Vec<string::StringValue> = Vec::new();
        self.read_native_names_with(|chunk| -> Result<(), FormatError> {
            match chunk {
                string::StringChunk::Plain(refs) => {
                    for value in refs {
                        output.push(value.to_value()?);
                    }
                }
                string::StringChunk::DictionaryEntries(refs) => {
                    for value in refs {
                        dictionary.push(value.to_value()?);
                    }
                }
                string::StringChunk::DictionaryIndices(ids) => {
                    for &id in ids {
                        output.push(dictionary[id as usize].clone());
                    }
                }
            }
            Ok(())
        })?;
        Ok(Some(output))
    }

    fn read_string_object(
        &mut self,
        object: &ObjectEntry,
    ) -> Result<Vec<string::StringValue>, FormatError> {
        let length = to_usize(object.logical_len)?;
        let mut output = try_vec_with_capacity(length, "character object allocation")?;
        let range = object_block_range(object)?;
        let first = self
            .validated
            .blocks
            .get(range.start)
            .ok_or(FormatError::Invalid("character block range is empty"))?;
        self.file.seek(SeekFrom::Start(first.block_header_offset))?;
        let mut encoded = Vec::new();
        for index in range {
            let entry = &self.validated.blocks[index];
            read_block_at_current(
                &mut self.file,
                entry,
                self.validated.directory_offset,
                &mut encoded,
            )?;
            let count = to_usize(entry.logical_count)?;
            let values = string::decode_block(&encoded, count)?;
            output.extend(values);
        }
        if output.len() != length {
            return Err(FormatError::Invalid("character object length mismatch"));
        }
        Ok(output)
    }
}

fn validate_structure(file: &mut File) -> Result<ValidatedContainer, FormatError> {
    let file_len = file.metadata()?.len();
    let minimum_len = (HEADER_LEN + BLOCK_HEADER_LEN + DIRECTORY_HEADER_LEN + TRAILER_LEN) as u64;
    if file_len < minimum_len {
        return Err(FormatError::Invalid("file is truncated"));
    }

    let mut header = [0_u8; HEADER_LEN];
    file.read_exact(&mut header)?;
    if &header[0..4] != FILE_MAGIC {
        return Err(FormatError::Invalid("incorrect magic bytes"));
    }
    let container_version = read_u16(&header, 4)?;
    if container_version != CONTAINER_VERSION {
        return Err(FormatError::UnsupportedContainerVersion(container_version));
    }
    if read_u16(&header, 6)? as usize != HEADER_LEN {
        return Err(FormatError::Invalid("unsupported header length"));
    }
    if read_u32(&header, 8)? != 0 || read_u32(&header, 20)? != 0 || read_u32(&header, 28)? != 0 {
        return Err(FormatError::Invalid(
            "unsupported header flags or reserved fields",
        ));
    }
    if read_u32(&header, 24)? != crc32(&header[..24]) {
        return Err(FormatError::Invalid("header checksum mismatch"));
    }
    let codec_id = read_u16(&header, 12)?;
    let codec_version = read_u16(&header, 14)?;
    validate_codec(codec_id, codec_version)?;
    let block_size = read_u32(&header, 16)?;
    if block_size == 0 || block_size as u64 > MAX_BLOCK_SIZE {
        return Err(FormatError::Limit("maximum decoded block size"));
    }

    file.seek(SeekFrom::End(-(TRAILER_LEN as i64)))?;
    let mut trailer = [0_u8; TRAILER_LEN];
    file.read_exact(&mut trailer)?;
    if &trailer[0..4] != TRAILER_MAGIC || &trailer[28..32] != CLOSING_MAGIC {
        return Err(FormatError::Invalid("closing trailer magic mismatch"));
    }
    if read_u16(&trailer, 4)? != CONTAINER_VERSION || read_u16(&trailer, 6)? as usize != TRAILER_LEN
    {
        return Err(FormatError::Invalid("unsupported closing trailer"));
    }
    let directory_offset = read_u64(&trailer, 8)?;
    let directory_len = read_u64(&trailer, 16)?;
    let directory_checksum = read_u32(&trailer, 24)?;
    if directory_offset < HEADER_LEN as u64 {
        return Err(FormatError::Invalid("directory overlaps the file header"));
    }
    let trailer_offset = file_len - TRAILER_LEN as u64;
    if checked_add(directory_offset, directory_len)? != trailer_offset {
        return Err(FormatError::Invalid(
            "directory bounds do not reach the closing trailer",
        ));
    }
    let directory_len_usize = to_usize(directory_len)?;
    let max_directory_len = DIRECTORY_HEADER_LEN
        + MAX_OBJECTS as usize * OBJECT_ENTRY_LEN
        + MAX_ATTRIBUTES as usize * ATTRIBUTE_ENTRY_LEN
        + MAX_BLOCKS as usize * DIRECTORY_ENTRY_LEN
        + MAX_SYNOPSIS_LEN;
    if directory_len_usize > max_directory_len {
        return Err(FormatError::Limit("directory length"));
    }
    file.seek(SeekFrom::Start(directory_offset))?;
    let mut directory = try_zeroed_vec(directory_len_usize, "directory allocation")?;
    file.read_exact(&mut directory)?;
    if crc32(&directory) != directory_checksum {
        return Err(FormatError::Invalid("directory checksum mismatch"));
    }
    let parsed = parse_directory(&directory, directory_offset, block_size, codec_id)?;
    let payload_bytes = parsed
        .blocks
        .iter()
        .try_fold(0_u64, |total, entry| checked_add(total, entry.decoded_len))?;
    let root = parsed.objects.first();
    let root_type = root.map(|_| "logical");
    let root_length = root.map(|object| object.logical_len);
    let attribute_names = if parsed.attributes.is_empty() {
        Vec::new()
    } else {
        vec!["names"]
    };

    Ok(ValidatedContainer {
        info: ContainerInfo {
            container_version,
            codec_id,
            codec_version,
            block_size,
            block_count: parsed.blocks.len() as u32,
            object_count: parsed.objects.len() as u32,
            attribute_count: parsed.attributes.len() as u32,
            payload_bytes,
            file_bytes: file_len,
            synopsis: parsed.synopsis,
            root_type,
            root_length,
            attribute_names,
        },
        objects: parsed.objects,
        attributes: parsed.attributes,
        blocks: parsed.blocks,
        directory_offset,
    })
}

struct ParsedDirectory {
    objects: Vec<ObjectEntry>,
    attributes: Vec<AttributeEntry>,
    blocks: Vec<BlockEntry>,
    synopsis: Vec<u8>,
}

fn parse_directory(
    directory: &[u8],
    directory_offset: u64,
    block_size: u32,
    codec_id: u16,
) -> Result<ParsedDirectory, FormatError> {
    if directory.len() < DIRECTORY_HEADER_LEN || &directory[0..4] != DIRECTORY_MAGIC {
        return Err(FormatError::Invalid("invalid directory header"));
    }
    if read_u16(directory, 4)? != DIRECTORY_VERSION
        || read_u16(directory, 6)? as usize != DIRECTORY_HEADER_LEN
        || read_u16(directory, 8)? as usize != OBJECT_ENTRY_LEN
        || read_u16(directory, 10)? as usize != ATTRIBUTE_ENTRY_LEN
        || read_u16(directory, 12)? as usize != DIRECTORY_ENTRY_LEN
    {
        return Err(FormatError::Invalid(
            "unsupported directory version or entry size",
        ));
    }
    if read_u16(directory, 14)? != 0 {
        return Err(FormatError::Invalid("unsupported directory flags"));
    }
    let object_count = read_u32(directory, 16)?;
    let attribute_count = read_u32(directory, 20)?;
    let block_count = read_u32(directory, 24)?;
    let synopsis_len = read_u32(directory, 28)? as usize;
    if object_count > MAX_OBJECTS {
        return Err(FormatError::Limit("object count"));
    }
    if attribute_count > MAX_ATTRIBUTES {
        return Err(FormatError::Limit("attribute count"));
    }
    if block_count == 0 || block_count > MAX_BLOCKS {
        return Err(FormatError::Limit("block count"));
    }
    if synopsis_len > MAX_SYNOPSIS_LEN {
        return Err(FormatError::Limit("synopsis length"));
    }
    if read_u32(directory, 32)? != crc32(&directory[..32]) {
        return Err(FormatError::Invalid("directory header checksum mismatch"));
    }
    let object_bytes = (object_count as usize)
        .checked_mul(OBJECT_ENTRY_LEN)
        .ok_or(FormatError::Limit("object directory entries"))?;
    let attribute_bytes = (attribute_count as usize)
        .checked_mul(ATTRIBUTE_ENTRY_LEN)
        .ok_or(FormatError::Limit("attribute directory entries"))?;
    let block_bytes = (block_count as usize)
        .checked_mul(DIRECTORY_ENTRY_LEN)
        .ok_or(FormatError::Limit("block directory entries"))?;
    let expected_len = DIRECTORY_HEADER_LEN
        .checked_add(object_bytes)
        .and_then(|value| value.checked_add(attribute_bytes))
        .and_then(|value| value.checked_add(block_bytes))
        .and_then(|value| value.checked_add(synopsis_len))
        .ok_or(FormatError::Limit("directory length"))?;
    if expected_len != directory.len() {
        return Err(FormatError::Invalid("directory length mismatch"));
    }

    let mut offset = DIRECTORY_HEADER_LEN;
    let mut objects = try_vec_with_capacity(object_count as usize, "object entries")?;
    for _ in 0..object_count {
        objects.push(ObjectEntry::decode(
            &directory[offset..offset + OBJECT_ENTRY_LEN],
        )?);
        offset += OBJECT_ENTRY_LEN;
    }
    let mut attributes = try_vec_with_capacity(attribute_count as usize, "attribute entries")?;
    for _ in 0..attribute_count {
        attributes.push(AttributeEntry::decode(
            &directory[offset..offset + ATTRIBUTE_ENTRY_LEN],
        )?);
        offset += ATTRIBUTE_ENTRY_LEN;
    }
    let mut blocks = try_vec_with_capacity(block_count as usize, "block entries")?;
    let mut expected_block_offset = HEADER_LEN as u64;
    for index in 0..block_count as usize {
        let entry = BlockEntry {
            sequence: read_u32(directory, offset)?,
            flags: read_u32(directory, offset + 4)?,
            block_header_offset: read_u64(directory, offset + 8)?,
            payload_offset: read_u64(directory, offset + 16)?,
            stored_len: read_u32(directory, offset + 24)?,
            logical_count: read_u64(directory, offset + 32)?,
            decoded_len: read_u64(directory, offset + 40)?,
            encoding: read_u16(directory, offset + 48)?,
            compression: read_u16(directory, offset + 50)?,
            checksum: read_u32(directory, offset + 52)?,
        };
        if read_u32(directory, offset + 28)? != 0 {
            return Err(FormatError::Invalid(
                "nonzero block directory reserved field",
            ));
        }
        if entry.sequence != index as u32 || entry.flags != 0 {
            return Err(FormatError::Invalid("invalid block sequence or flags"));
        }
        if entry.compression != COMPRESSION_NONE
            || u64::from(entry.stored_len) != entry.decoded_len
            || entry.decoded_len > MAX_BLOCK_SIZE
        {
            return Err(FormatError::Limit("block size"));
        }
        if entry.decoded_len > u64::from(block_size) {
            return Err(FormatError::Invalid(
                "decoded block length exceeds the declared block size",
            ));
        }
        if entry.block_header_offset != expected_block_offset
            || entry.payload_offset
                != checked_add(entry.block_header_offset, BLOCK_HEADER_LEN as u64)?
        {
            return Err(FormatError::Invalid("non-canonical block offsets"));
        }
        expected_block_offset = checked_add(entry.payload_offset, u64::from(entry.stored_len))?;
        if expected_block_offset > directory_offset {
            return Err(FormatError::Invalid("block overlaps the directory"));
        }
        blocks.push(entry);
        offset += DIRECTORY_ENTRY_LEN;
    }
    if expected_block_offset != directory_offset {
        return Err(FormatError::Invalid(
            "gap or trailing bytes before the directory",
        ));
    }
    let synopsis = directory[offset..].to_vec();
    match codec_id {
        CODEC_R_SERIAL_V3 => validate_generic_schema(&objects, &attributes, &blocks, &synopsis)?,
        CODEC_NATIVE_V1 => {
            validate_native_logical_schema(&objects, &attributes, &blocks, &synopsis)?
        }
        _ => return Err(FormatError::Invalid("unsupported codec directory")),
    }
    Ok(ParsedDirectory {
        objects,
        attributes,
        blocks,
        synopsis,
    })
}

fn validate_generic_schema(
    objects: &[ObjectEntry],
    attributes: &[AttributeEntry],
    blocks: &[BlockEntry],
    _synopsis: &[u8],
) -> Result<(), FormatError> {
    if !objects.is_empty() || !attributes.is_empty() {
        return Err(FormatError::Invalid(
            "generic codec cannot contain native object entries",
        ));
    }
    for block in blocks {
        if block.encoding != ENCODING_RAW
            || block.logical_count != block.decoded_len
            || u64::from(block.stored_len) != block.logical_count
        {
            return Err(FormatError::Invalid("invalid generic raw block"));
        }
    }
    Ok(())
}

fn validate_native_logical_schema(
    objects: &[ObjectEntry],
    attributes: &[AttributeEntry],
    blocks: &[BlockEntry],
    synopsis: &[u8],
) -> Result<(), FormatError> {
    if !synopsis.is_empty() {
        return Err(FormatError::Invalid(
            "native codec cannot contain a generic synopsis",
        ));
    }
    if !matches!((objects.len(), attributes.len()), (1, 0) | (3, 1)) {
        return Err(FormatError::Invalid(
            "invalid logical object directory shape",
        ));
    }
    let root = &objects[0];
    validate_object_common(root, 0, ROOT_PARENT_ID, ROLE_ROOT, TYPE_LOGICAL)?;
    if root.first_child != 0 || root.child_count != 0 || root.first_attribute != 0 {
        return Err(FormatError::Invalid("invalid logical root references"));
    }
    if root.attribute_count != attributes.len() as u32 {
        return Err(FormatError::Invalid("logical attribute count mismatch"));
    }
    validate_logical_object_blocks(root, blocks)?;
    for index in object_block_range(root)? {
        let entry = &blocks[index];
        let count = to_usize(entry.logical_count)?;
        logical::validate_encoded_length(entry.encoding, count, entry.stored_len as usize)?;
    }

    if attributes.is_empty() {
        if root.first_block != 0 || root.block_count as usize != blocks.len() {
            return Err(FormatError::Invalid("logical blocks are not fully indexed"));
        }
        return Ok(());
    }

    let name_object = &objects[1];
    let value_object = &objects[2];
    validate_object_common(name_object, 1, 0, ROLE_ATTRIBUTE_NAME, TYPE_CHARACTER)?;
    validate_object_common(value_object, 2, 0, ROLE_ATTRIBUTE_VALUE, TYPE_CHARACTER)?;
    if name_object.logical_len != 1 || value_object.logical_len != root.logical_len {
        return Err(FormatError::Invalid("names object length mismatch"));
    }
    validate_leaf_object(name_object)?;
    validate_leaf_object(value_object)?;
    validate_object_blocks(name_object, blocks, ENCODING_STRING_PLAIN)?;
    validate_string_object_blocks(value_object, blocks)?;
    let attribute = &attributes[0];
    if attribute.owner_id != 0
        || attribute.name_object_id != 1
        || attribute.value_object_id != 2
        || attribute.ordinal != 0
        || attribute.flags != ATTRIBUTE_FLAG_NAMES
    {
        return Err(FormatError::Invalid("invalid names attribute entry"));
    }
    let root_end = root
        .first_block
        .checked_add(root.block_count)
        .ok_or(FormatError::Invalid("block range overflow"))?;
    let name_end = name_object
        .first_block
        .checked_add(name_object.block_count)
        .ok_or(FormatError::Invalid("block range overflow"))?;
    let value_end = value_object
        .first_block
        .checked_add(value_object.block_count)
        .ok_or(FormatError::Invalid("block range overflow"))?;
    if root.first_block != 0
        || name_object.first_block != root_end
        || value_object.first_block != name_end
        || value_end as usize != blocks.len()
    {
        return Err(FormatError::Invalid(
            "native object blocks are not canonical",
        ));
    }
    Ok(())
}

fn validate_logical_object_blocks(
    object: &ObjectEntry,
    blocks: &[BlockEntry],
) -> Result<(), FormatError> {
    let mut logical_len = 0_u64;
    let range = object_block_range(object)?;
    for (ordinal, index) in range.clone().enumerate() {
        let block = &blocks[index];
        if !logical::is_logical_encoding(block.encoding) {
            return Err(FormatError::Invalid("unexpected logical block encoding"));
        }
        let count = to_usize(block.logical_count)?;
        if count > LOGICAL_BLOCK_VALUES
            || (object.logical_len != 0 && count == 0)
            || (ordinal + 1 < range.len() && count != LOGICAL_BLOCK_VALUES)
        {
            return Err(FormatError::Invalid("non-canonical logical block size"));
        }
        logical_len = logical_len
            .checked_add(block.logical_count)
            .ok_or(FormatError::Invalid("logical object length overflow"))?;
    }
    if logical_len != object.logical_len {
        return Err(FormatError::Invalid("logical object length mismatch"));
    }
    Ok(())
}

fn validate_object_common(
    object: &ObjectEntry,
    object_id: u32,
    parent_id: u32,
    role: u16,
    type_tag: u16,
) -> Result<(), FormatError> {
    if object.object_id != object_id
        || object.parent_id != parent_id
        || object.role != role
        || object.type_tag != type_tag
        || object.flags != 0
    {
        return Err(FormatError::Invalid("invalid native object descriptor"));
    }
    Ok(())
}

fn validate_leaf_object(object: &ObjectEntry) -> Result<(), FormatError> {
    if object.first_child != 0
        || object.child_count != 0
        || object.first_attribute != 0
        || object.attribute_count != 0
    {
        return Err(FormatError::Invalid("attribute object is not a leaf"));
    }
    Ok(())
}

fn validate_object_blocks(
    object: &ObjectEntry,
    blocks: &[BlockEntry],
    encoding: u16,
) -> Result<(), FormatError> {
    if object.block_count == 0 {
        return Err(FormatError::Invalid("object has no data block"));
    }
    let mut logical_count = 0_u64;
    for index in object_block_range(object)? {
        let block = blocks
            .get(index)
            .ok_or(FormatError::Invalid("object block range is out of bounds"))?;
        if block.encoding != encoding {
            return Err(FormatError::Invalid("object block encoding mismatch"));
        }
        logical_count = logical_count
            .checked_add(block.logical_count)
            .ok_or(FormatError::Limit("object logical length"))?;
    }
    if logical_count != object.logical_len {
        return Err(FormatError::Invalid("object logical length mismatch"));
    }
    Ok(())
}

/// A character object may mix plain blocks with dictionary entry and index
/// blocks. Plain and index blocks carry its elements; entry blocks do not
/// count toward its length, and their total cannot exceed it, which bounds
/// the dictionary a reader allocates.
fn validate_string_object_blocks(
    object: &ObjectEntry,
    blocks: &[BlockEntry],
) -> Result<(), FormatError> {
    if object.block_count == 0 {
        return Err(FormatError::Invalid("object has no data block"));
    }
    let mut elements = 0_u64;
    let mut entries = 0_u64;
    for index in object_block_range(object)? {
        let block = blocks
            .get(index)
            .ok_or(FormatError::Invalid("object block range is out of bounds"))?;
        match block.encoding {
            ENCODING_STRING_PLAIN => {
                elements = elements
                    .checked_add(block.logical_count)
                    .ok_or(FormatError::Limit("object logical length"))?;
            }
            ENCODING_STRING_DICT_ENTRIES => {
                entries = entries
                    .checked_add(block.logical_count)
                    .ok_or(FormatError::Limit("dictionary length"))?;
            }
            ENCODING_STRING_DICT_INDICES => {
                let count = to_usize(block.logical_count)?;
                let stored = block.stored_len as usize;
                if ![1, 2, 4]
                    .iter()
                    .any(|&width| string::index_block_len(count, width) == Some(stored))
                {
                    return Err(FormatError::Invalid(
                        "dictionary index block length mismatch",
                    ));
                }
                elements = elements
                    .checked_add(block.logical_count)
                    .ok_or(FormatError::Limit("object logical length"))?;
            }
            _ => return Err(FormatError::Invalid("object block encoding mismatch")),
        }
    }
    if elements != object.logical_len {
        return Err(FormatError::Invalid("object logical length mismatch"));
    }
    if entries > object.logical_len {
        return Err(FormatError::Invalid(
            "string dictionary is larger than its vector",
        ));
    }
    Ok(())
}

fn object_block_range(object: &ObjectEntry) -> Result<std::ops::Range<usize>, FormatError> {
    let start = object.first_block as usize;
    let end = object
        .first_block
        .checked_add(object.block_count)
        .ok_or(FormatError::Invalid("block range overflow"))? as usize;
    Ok(start..end)
}

fn validate_codec(id: u16, version: u16) -> Result<(), FormatError> {
    if matches!(
        (id, version),
        (CODEC_R_SERIAL_V3, R_SERIAL_CODEC_VERSION) | (CODEC_NATIVE_V1, NATIVE_CODEC_VERSION)
    ) {
        Ok(())
    } else {
        Err(FormatError::UnsupportedCodec { id, version })
    }
}

fn validate_block_header_at_current(
    file: &mut File,
    entry: &BlockEntry,
    directory_offset: u64,
) -> Result<(), FormatError> {
    if checked_add(entry.payload_offset, u64::from(entry.stored_len))? > directory_offset {
        return Err(FormatError::Invalid("block payload exceeds its bounds"));
    }
    let mut header = [0_u8; BLOCK_HEADER_LEN];
    file.read_exact(&mut header)?;
    if &header[0..4] != BLOCK_MAGIC
        || read_u16(&header, 4)? as usize != BLOCK_HEADER_LEN
        || read_u16(&header, 6)? != 0
        || read_u32(&header, 8)? != entry.sequence
        || read_u16(&header, 12)? != entry.encoding
        || read_u16(&header, 14)? != entry.compression
        || read_u64(&header, 16)? != entry.logical_count
        || read_u64(&header, 24)? != entry.decoded_len
        || read_u32(&header, 32)? != entry.stored_len
        || read_u32(&header, 36)? != entry.checksum
    {
        return Err(FormatError::InvalidDetail(format!(
            "block {} header does not match the directory",
            entry.sequence
        )));
    }
    Ok(())
}

fn read_block_at_current(
    file: &mut File,
    entry: &BlockEntry,
    directory_offset: u64,
    output: &mut Vec<u8>,
) -> Result<(), FormatError> {
    validate_block_header_at_current(file, entry, directory_offset)?;
    let stored_len = entry.stored_len as usize;
    if output.capacity() < stored_len {
        output
            .try_reserve_exact(stored_len - output.len())
            .map_err(|_| FormatError::Limit("block allocation"))?;
    }
    output.resize(stored_len, 0);
    file.read_exact(output)?;
    if crc32(output) != entry.checksum {
        return Err(checksum_error(entry.sequence));
    }
    Ok(())
}

fn checksum_error(sequence: u32) -> FormatError {
    FormatError::InvalidDetail(format!("checksum mismatch in block {sequence}"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::codec::string::{DictionaryPolicy, StringEncoding, StringValue};
    use crate::container::{Codec, write_container, write_native_logical};
    use std::fs;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn temp_path(label: &str) -> Result<std::path::PathBuf, FormatError> {
        let stamp = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_err(|_| FormatError::Invalid("clock before Unix epoch"))?
            .as_nanos();
        Ok(std::env::temp_dir().join(format!("rdz-{label}-{}-{stamp}.rdz", std::process::id())))
    }

    #[test]
    fn generic_round_trip_is_multiblock_and_indexed() -> Result<(), FormatError> {
        let path = temp_path("generic")?;
        let payload = vec![42_u8; crate::format::BLOCK_SIZE * 2 + 17];
        let synopsis = b"bounded synopsis";
        write_container(&path, Codec::RSerialV3, &payload, synopsis)?;
        let info = read_info(&path)?;
        assert_eq!(info.codec_id, CODEC_R_SERIAL_V3);
        assert_eq!(info.block_count, 3);
        assert_eq!(info.payload_bytes, payload.len() as u64);
        assert_eq!(info.synopsis, synopsis);
        let (decoded, _) = read_generic(&path)?;
        assert_eq!(decoded, payload);
        fs::remove_file(path)?;
        Ok(())
    }

    #[test]
    fn dictionary_names_round_trip_through_the_container() -> Result<(), FormatError> {
        let length = 2 * crate::codec::string::DICT_CHUNK_VALUES + 3;
        let values: Vec<i32> = (0..length).map(|index| (index % 2) as i32).collect();
        let names: Vec<StringValue> = (0..length)
            .map(|index| match index % 4 {
                0 => StringValue::Na,
                _ => StringValue::Value {
                    encoding: StringEncoding::Utf8,
                    bytes: format!("n{}", index % 4).into_bytes(),
                },
            })
            .collect();
        for policy in [
            DictionaryPolicy::Plain,
            DictionaryPolicy::Block,
            DictionaryPolicy::Global,
            DictionaryPolicy::Auto,
        ] {
            let path = temp_path("dictionary")?;
            write_native_logical(&path, &values, i32::MIN, Some(names.as_slice()), policy)?;
            let reader = open(&path)?;
            let mut decoded = vec![0; length];
            assert_eq!(
                reader.read_native_logical_into(&mut decoded, i32::MIN)?,
                Some(names.clone()),
                "{policy:?}"
            );
            assert_eq!(decoded, values);
            assert_eq!(open(&path)?.read_native_names()?, Some(names.clone()));
            fs::remove_file(path)?;
        }
        Ok(())
    }

    #[test]
    fn string_objects_bound_their_dictionary_and_index_lengths() {
        let object = ObjectEntry {
            object_id: 2,
            parent_id: 0,
            role: ROLE_ATTRIBUTE_VALUE,
            type_tag: TYPE_CHARACTER,
            flags: 0,
            logical_len: 2,
            first_child: 0,
            child_count: 0,
            first_attribute: 0,
            attribute_count: 0,
            first_block: 0,
            block_count: 2,
        };
        let block = |encoding: u16, logical_count: u64, stored_len: u32| BlockEntry {
            sequence: 0,
            flags: 0,
            block_header_offset: 0,
            payload_offset: 0,
            stored_len,
            logical_count,
            decoded_len: u64::from(stored_len),
            encoding,
            compression: COMPRESSION_NONE,
            checksum: 0,
        };
        let valid = [
            block(ENCODING_STRING_DICT_ENTRIES, 2, 12),
            block(ENCODING_STRING_DICT_INDICES, 2, 10),
        ];
        assert!(validate_string_object_blocks(&object, &valid).is_ok());
        let oversized_dictionary = [
            block(ENCODING_STRING_DICT_ENTRIES, 3, 18),
            block(ENCODING_STRING_DICT_INDICES, 2, 10),
        ];
        assert!(validate_string_object_blocks(&object, &oversized_dictionary).is_err());
        let bad_index_length = [
            block(ENCODING_STRING_DICT_ENTRIES, 2, 12),
            block(ENCODING_STRING_DICT_INDICES, 2, 11),
        ];
        assert!(validate_string_object_blocks(&object, &bad_index_length).is_err());
        let foreign_encoding = [
            block(ENCODING_STRING_DICT_ENTRIES, 2, 12),
            block(ENCODING_RAW, 2, 2),
        ];
        assert!(validate_string_object_blocks(&object, &foreign_encoding).is_err());
    }

    #[test]
    fn native_logical_round_trips_with_names() -> Result<(), FormatError> {
        let path = temp_path("native-logical")?;
        let values = [0, 1, i32::MIN, 1, 0];
        let names = vec![
            StringValue::Value {
                encoding: StringEncoding::Native,
                bytes: b"false".to_vec(),
            },
            StringValue::Value {
                encoding: StringEncoding::Utf8,
                bytes: "Grüezi".as_bytes().to_vec(),
            },
            StringValue::Na,
            StringValue::Value {
                encoding: StringEncoding::Latin1,
                bytes: vec![0xe4],
            },
            StringValue::Value {
                encoding: StringEncoding::Bytes,
                bytes: vec![0xff],
            },
        ];
        write_native_logical(
            &path,
            &values,
            i32::MIN,
            Some(names.as_slice()),
            DictionaryPolicy::Plain,
        )?;
        let info = read_info(&path)?;
        assert_eq!(info.codec_id, CODEC_NATIVE_V1);
        assert_eq!(info.root_type, Some("logical"));
        assert_eq!(info.root_length, Some(values.len() as u64));
        assert_eq!(info.attribute_names, vec!["names"]);
        let reader = open(&path)?;
        let mut decoded = [0; 5];
        let decoded_names = reader.read_native_logical_into(&mut decoded, i32::MIN)?;
        assert_eq!(decoded, values);
        assert_eq!(decoded_names, Some(names));
        fs::remove_file(path)?;
        Ok(())
    }

    #[test]
    fn native_logical_wire_fixture_is_stable() -> Result<(), FormatError> {
        let path = temp_path("native-fixture")?;
        write_native_logical(
            &path,
            &[0, 1, i32::MIN],
            i32::MIN,
            None::<&[StringValue]>,
            DictionaryPolicy::Plain,
        )?;
        let actual = fs::read(&path)?;
        let fixture = include_str!("../../tests/fixtures/logical-v1.hex");
        let digits: Vec<u8> = fixture
            .bytes()
            .filter(|byte| !byte.is_ascii_whitespace())
            .collect();
        if digits.len() % 2 != 0 {
            return Err(FormatError::Invalid("golden fixture has odd hex length"));
        }
        let mut expected = try_vec_with_capacity(digits.len() / 2, "golden fixture")?;
        for pair in digits.chunks_exact(2) {
            let text = std::str::from_utf8(pair)
                .map_err(|_| FormatError::Invalid("golden fixture is not ASCII"))?;
            expected.push(
                u8::from_str_radix(text, 16)
                    .map_err(|_| FormatError::Invalid("golden fixture contains invalid hex"))?,
            );
        }
        assert_eq!(actual, expected);
        fs::remove_file(path)?;
        Ok(())
    }

    #[test]
    fn native_empty_logical_uses_one_zero_length_block() -> Result<(), FormatError> {
        let path = temp_path("native-empty")?;
        write_native_logical(
            &path,
            &[],
            i32::MIN,
            None::<&[StringValue]>,
            DictionaryPolicy::Plain,
        )?;
        let info = read_info(&path)?;
        assert_eq!(info.block_count, 1);
        assert_eq!(info.root_length, Some(0));
        let reader = open(&path)?;
        let mut decoded = [];
        assert_eq!(
            reader.read_native_logical_into(&mut decoded, i32::MIN)?,
            None
        );
        fs::remove_file(path)?;
        Ok(())
    }

    #[test]
    fn malformed_native_logical_states_are_rejected_after_checksum_validation()
    -> Result<(), FormatError> {
        for (label, payload_index, invalid_byte) in
            [("state", 0_usize, 0x03_u8), ("reserved", 1, 0x01)]
        {
            let path = temp_path(label)?;
            write_native_logical(
                &path,
                &[0],
                i32::MIN,
                None::<&[StringValue]>,
                DictionaryPolicy::Plain,
            )?;
            let mut bytes = fs::read(&path)?;
            let trailer_start = bytes.len() - TRAILER_LEN;
            let directory_offset = read_u64(&bytes[trailer_start..], 8)? as usize;
            let payload_offset = HEADER_LEN + BLOCK_HEADER_LEN;
            let stored_len = read_u32(&bytes[HEADER_LEN..], 32)? as usize;
            bytes[payload_offset + payload_index] = invalid_byte;
            let checksum = crc32(&bytes[payload_offset..payload_offset + stored_len]);
            bytes[HEADER_LEN + 36..HEADER_LEN + 40].copy_from_slice(&checksum.to_le_bytes());
            let block_entry = directory_offset + DIRECTORY_HEADER_LEN + OBJECT_ENTRY_LEN;
            bytes[block_entry + 52..block_entry + 56].copy_from_slice(&checksum.to_le_bytes());
            let directory_checksum = crc32(&bytes[directory_offset..trailer_start]);
            bytes[trailer_start + 24..trailer_start + 28]
                .copy_from_slice(&directory_checksum.to_le_bytes());
            fs::write(&path, bytes)?;

            assert!(read_info(&path).is_ok());
            let reader = open(&path)?;
            let mut output = [0];
            let error = reader
                .read_native_logical_into(&mut output, i32::MIN)
                .expect_err("malformed logical payload was accepted");
            assert!(error.to_string().contains("invalid logical constant block"));
            fs::remove_file(path)?;
        }
        Ok(())
    }

    #[test]
    fn malformed_native_object_descriptors_are_rejected() -> Result<(), FormatError> {
        let path = temp_path("native-object")?;
        write_native_logical(
            &path,
            &[1],
            i32::MIN,
            None::<&[StringValue]>,
            DictionaryPolicy::Plain,
        )?;
        let mut bytes = fs::read(&path)?;
        let trailer_start = bytes.len() - TRAILER_LEN;
        let directory_offset = read_u64(&bytes[trailer_start..], 8)? as usize;
        let root_type_offset = directory_offset + DIRECTORY_HEADER_LEN + 10;
        bytes[root_type_offset..root_type_offset + 2].copy_from_slice(&2_u16.to_le_bytes());
        let directory_checksum = crc32(&bytes[directory_offset..trailer_start]);
        bytes[trailer_start + 24..trailer_start + 28]
            .copy_from_slice(&directory_checksum.to_le_bytes());
        fs::write(&path, bytes)?;

        assert!(read_info(&path).is_err());
        fs::remove_file(path)?;
        Ok(())
    }

    #[test]
    fn corruption_and_truncation_are_rejected() -> Result<(), FormatError> {
        let path = temp_path("corrupt")?;
        write_container(&path, Codec::RSerialV3, b"payload", b"synopsis")?;
        let mut bytes = fs::read(&path)?;
        let last = bytes
            .len()
            .checked_sub(TRAILER_LEN + 1)
            .ok_or(FormatError::Invalid("test file too short"))?;
        bytes[last] ^= 0xff;
        fs::write(&path, &bytes)?;
        assert!(read_info(&path).is_err());
        bytes.truncate(20);
        fs::write(&path, &bytes)?;
        assert!(read_info(&path).is_err());
        fs::remove_file(path)?;
        Ok(())
    }

    #[test]
    fn failed_write_leaves_existing_destination_intact() -> Result<(), FormatError> {
        let path = temp_path("replace")?;
        fs::write(&path, b"old")?;
        let oversized = vec![0_u8; MAX_SYNOPSIS_LEN + 1];
        assert!(write_container(&path, Codec::RSerialV3, b"new", &oversized).is_err());
        assert_eq!(fs::read(&path)?, b"old");
        fs::remove_file(path)?;
        Ok(())
    }

    #[test]
    fn overflowing_trailer_bounds_are_rejected() -> Result<(), FormatError> {
        let path = temp_path("overflow")?;
        write_container(&path, Codec::RSerialV3, b"payload", &[])?;
        let mut bytes = fs::read(&path)?;
        let trailer_start = bytes.len() - TRAILER_LEN;
        bytes[trailer_start + 16..trailer_start + 24].fill(0xff);
        fs::write(&path, bytes)?;
        assert!(matches!(
            read_info(&path),
            Err(FormatError::Invalid("offset arithmetic overflow"))
        ));
        fs::remove_file(path)?;
        Ok(())
    }

    #[test]
    fn a_maximum_block_size_contradicting_the_blocks_is_rejected() -> Result<(), FormatError> {
        let path = temp_path("blocksize")?;
        let payload = vec![3_u8; crate::format::BLOCK_SIZE + 1];
        write_container(&path, Codec::RSerialV3, &payload, &[])?;
        let mut bytes = fs::read(&path)?;
        bytes[16..20].copy_from_slice(&4096_u32.to_le_bytes());
        let repaired = crc32(&bytes[..24]);
        bytes[24..28].copy_from_slice(&repaired.to_le_bytes());
        fs::write(&path, &bytes)?;
        assert!(read_info(&path).is_err());
        fs::remove_file(path)?;
        Ok(())
    }

    #[test]
    fn generic_empty_payload_round_trips() -> Result<(), FormatError> {
        let path = temp_path("empty")?;
        write_container(&path, Codec::RSerialV3, &[], b"synopsis")?;
        let (decoded, info) = read_generic(&path)?;
        assert!(decoded.is_empty());
        assert_eq!(info.block_count, 1);
        fs::remove_file(path)?;
        Ok(())
    }

    #[test]
    fn directory_destinations_are_rejected_without_modification() -> Result<(), FormatError> {
        let path = temp_path("directory")?;
        fs::create_dir(&path)?;
        assert!(write_container(&path, Codec::RSerialV3, b"payload", &[]).is_err());
        assert!(path.is_dir());
        fs::remove_dir(path)?;
        Ok(())
    }
}
