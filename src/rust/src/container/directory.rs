use crate::format::{
    ATTRIBUTE_ENTRY_LEN, COMPRESSION_NONE, ENCODING_RAW, FormatError, MAX_BLOCK_SIZE,
    OBJECT_ENTRY_LEN, put_u16, put_u32, put_u64, read_u16, read_u32, read_u64,
};

pub(crate) const ROOT_PARENT_ID: u32 = u32::MAX;

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct ObjectEntry {
    pub(crate) object_id: u32,
    pub(crate) parent_id: u32,
    pub(crate) role: u16,
    pub(crate) type_tag: u16,
    pub(crate) flags: u32,
    pub(crate) logical_len: u64,
    pub(crate) first_child: u32,
    pub(crate) child_count: u32,
    pub(crate) first_attribute: u32,
    pub(crate) attribute_count: u32,
    pub(crate) first_block: u32,
    pub(crate) block_count: u32,
}

impl ObjectEntry {
    pub(crate) fn encode(&self) -> [u8; OBJECT_ENTRY_LEN] {
        let mut output = [0_u8; OBJECT_ENTRY_LEN];
        put_u32(&mut output, 0, self.object_id);
        put_u32(&mut output, 4, self.parent_id);
        put_u16(&mut output, 8, self.role);
        put_u16(&mut output, 10, self.type_tag);
        put_u32(&mut output, 12, self.flags);
        put_u64(&mut output, 16, self.logical_len);
        put_u32(&mut output, 24, self.first_child);
        put_u32(&mut output, 28, self.child_count);
        put_u32(&mut output, 32, self.first_attribute);
        put_u32(&mut output, 36, self.attribute_count);
        put_u32(&mut output, 40, self.first_block);
        put_u32(&mut output, 44, self.block_count);
        output
    }

    pub(crate) fn decode(input: &[u8]) -> Result<Self, FormatError> {
        if input.len() != OBJECT_ENTRY_LEN {
            return Err(FormatError::Invalid(
                "invalid object directory entry length",
            ));
        }
        Ok(Self {
            object_id: read_u32(input, 0)?,
            parent_id: read_u32(input, 4)?,
            role: read_u16(input, 8)?,
            type_tag: read_u16(input, 10)?,
            flags: read_u32(input, 12)?,
            logical_len: read_u64(input, 16)?,
            first_child: read_u32(input, 24)?,
            child_count: read_u32(input, 28)?,
            first_attribute: read_u32(input, 32)?,
            attribute_count: read_u32(input, 36)?,
            first_block: read_u32(input, 40)?,
            block_count: read_u32(input, 44)?,
        })
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct AttributeEntry {
    pub(crate) owner_id: u32,
    pub(crate) name_object_id: u32,
    pub(crate) value_object_id: u32,
    pub(crate) ordinal: u32,
    pub(crate) flags: u32,
}

impl AttributeEntry {
    pub(crate) fn encode(&self) -> [u8; ATTRIBUTE_ENTRY_LEN] {
        let mut output = [0_u8; ATTRIBUTE_ENTRY_LEN];
        put_u32(&mut output, 0, self.owner_id);
        put_u32(&mut output, 4, self.name_object_id);
        put_u32(&mut output, 8, self.value_object_id);
        put_u32(&mut output, 12, self.ordinal);
        put_u32(&mut output, 16, self.flags);
        output
    }

    pub(crate) fn decode(input: &[u8]) -> Result<Self, FormatError> {
        if input.len() != ATTRIBUTE_ENTRY_LEN {
            return Err(FormatError::Invalid(
                "invalid attribute directory entry length",
            ));
        }
        if read_u32(input, 20)? != 0 || read_u64(input, 24)? != 0 {
            return Err(FormatError::Invalid(
                "nonzero attribute directory reserved field",
            ));
        }
        Ok(Self {
            owner_id: read_u32(input, 0)?,
            name_object_id: read_u32(input, 4)?,
            value_object_id: read_u32(input, 8)?,
            ordinal: read_u32(input, 12)?,
            flags: read_u32(input, 16)?,
        })
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct BlockEntry {
    pub(crate) sequence: u32,
    pub(crate) flags: u32,
    pub(crate) block_header_offset: u64,
    pub(crate) payload_offset: u64,
    pub(crate) stored_len: u32,
    pub(crate) logical_count: u64,
    pub(crate) decoded_len: u64,
    pub(crate) encoding: u16,
    pub(crate) compression: u16,
    pub(crate) checksum: u64,
}

impl BlockEntry {
    pub(crate) fn raw(
        sequence: u32,
        block_header_offset: u64,
        payload_offset: u64,
        length: u64,
        checksum: u64,
    ) -> Result<Self, FormatError> {
        if length > MAX_BLOCK_SIZE {
            return Err(FormatError::Limit("block size"));
        }
        let stored_len = u32::try_from(length).map_err(|_| FormatError::Limit("block size"))?;
        Ok(Self {
            sequence,
            flags: 0,
            block_header_offset,
            payload_offset,
            stored_len,
            logical_count: length,
            decoded_len: length,
            encoding: ENCODING_RAW,
            compression: COMPRESSION_NONE,
            checksum,
        })
    }

    pub(crate) fn encoded(
        sequence: u32,
        block_header_offset: u64,
        payload_offset: u64,
        logical_count: u64,
        encoding: u16,
        payload: &[u8],
        checksum: u64,
    ) -> Result<Self, FormatError> {
        let length = u64::try_from(payload.len()).map_err(|_| FormatError::Limit("block size"))?;
        if length > MAX_BLOCK_SIZE {
            return Err(FormatError::Limit("block size"));
        }
        let stored_len = u32::try_from(length).map_err(|_| FormatError::Limit("block size"))?;
        Ok(Self {
            sequence,
            flags: 0,
            block_header_offset,
            payload_offset,
            stored_len,
            logical_count,
            decoded_len: length,
            encoding,
            compression: COMPRESSION_NONE,
            checksum,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn object_entries_have_an_executable_wire_layout() -> Result<(), FormatError> {
        let entry = ObjectEntry {
            object_id: 7,
            parent_id: ROOT_PARENT_ID,
            role: 1,
            type_tag: 2,
            flags: 3,
            logical_len: u64::from(u32::MAX) + 9,
            first_child: 10,
            child_count: 11,
            first_attribute: 12,
            attribute_count: 13,
            first_block: 14,
            block_count: 15,
        };
        assert_eq!(ObjectEntry::decode(&entry.encode())?, entry);
        Ok(())
    }

    #[test]
    fn attribute_entries_have_an_executable_wire_layout() -> Result<(), FormatError> {
        let entry = AttributeEntry {
            owner_id: 1,
            name_object_id: 2,
            value_object_id: 3,
            ordinal: 4,
            flags: 5,
        };
        assert_eq!(AttributeEntry::decode(&entry.encode())?, entry);
        Ok(())
    }

    #[test]
    fn attribute_entries_reject_nonzero_reserved_fields() {
        let mut bytes = [0_u8; ATTRIBUTE_ENTRY_LEN];
        put_u32(&mut bytes, 20, 1);
        assert!(AttributeEntry::decode(&bytes).is_err());
    }
}
