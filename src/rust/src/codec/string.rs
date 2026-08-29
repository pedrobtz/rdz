use crate::format::{BLOCK_SIZE, FormatError, read_u32, try_vec_with_capacity};

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

pub(crate) fn decode_block(
    encoded: &[u8],
    logical_count: usize,
) -> Result<Vec<StringValue>, FormatError> {
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
            output.push(StringValue::Na);
        } else {
            let mut bytes = try_vec_with_capacity(length, "character value allocation")?;
            bytes.extend_from_slice(value);
            output.push(StringValue::Value {
                encoding: StringEncoding::from_wire(tag)?,
                bytes,
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
}
