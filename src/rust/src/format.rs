use std::fmt;

pub(crate) const FILE_MAGIC: &[u8; 4] = b"RDZ\x1a";
pub(crate) const BLOCK_MAGIC: &[u8; 4] = b"RBLK";
pub(crate) const DIRECTORY_MAGIC: &[u8; 4] = b"RDIR";
pub(crate) const TRAILER_MAGIC: &[u8; 4] = b"RDZT";
pub(crate) const CLOSING_MAGIC: &[u8; 4] = b"ZEND";

pub(crate) const CONTAINER_VERSION: u16 = 2;
pub(crate) const DIRECTORY_VERSION: u16 = 1;
pub(crate) const HEADER_LEN: usize = 32;
pub(crate) const BLOCK_HEADER_LEN: usize = 40;
pub(crate) const DIRECTORY_HEADER_LEN: usize = 36;
pub(crate) const OBJECT_ENTRY_LEN: usize = 48;
pub(crate) const ATTRIBUTE_ENTRY_LEN: usize = 32;
pub(crate) const DIRECTORY_ENTRY_LEN: usize = 56;
pub(crate) const TRAILER_LEN: usize = 32;

pub(crate) const CODEC_R_SERIAL_V3: u16 = 1;
pub(crate) const CODEC_NATIVE_V1: u16 = 2;
pub(crate) const R_SERIAL_CODEC_VERSION: u16 = 3;
pub(crate) const NATIVE_CODEC_VERSION: u16 = 1;

pub(crate) const ENCODING_RAW: u16 = 0;
pub(crate) const ENCODING_LOGICAL_2BIT: u16 = 1;
pub(crate) const ENCODING_STRING_PLAIN: u16 = 2;
pub(crate) const ENCODING_LOGICAL_CONSTANT: u16 = 3;
pub(crate) const ENCODING_LOGICAL_DENSE_PLANES: u16 = 4;
pub(crate) const ENCODING_LOGICAL_SPARSE_PATCHES: u16 = 5;
pub(crate) const ENCODING_LOGICAL_RUN_ENDS: u16 = 6;
pub(crate) const ENCODING_LOGICAL_PERIODIC: u16 = 7;
pub(crate) const COMPRESSION_NONE: u16 = 0;
pub(crate) const TYPE_LOGICAL: u16 = 1;
pub(crate) const TYPE_CHARACTER: u16 = 4;
pub(crate) const ROLE_ROOT: u16 = 0;
pub(crate) const ROLE_ATTRIBUTE_NAME: u16 = 1;
pub(crate) const ROLE_ATTRIBUTE_VALUE: u16 = 2;
pub(crate) const ATTRIBUTE_FLAG_NAMES: u32 = 1;
pub(crate) const BLOCK_SIZE: usize = 1024 * 1024;
pub(crate) const LOGICAL_BLOCK_VALUES: usize = 64 * 1024;
pub(crate) const MAX_BLOCK_SIZE: u64 = 64 * 1024 * 1024;
pub(crate) const MAX_BLOCKS: u32 = 1_000_000;
pub(crate) const MAX_OBJECTS: u32 = 1_000_000;
pub(crate) const MAX_ATTRIBUTES: u32 = 1_000_000;
pub(crate) const MAX_SYNOPSIS_LEN: usize = 64 * 1024;

#[derive(Debug)]
pub(crate) enum FormatError {
    Io(std::io::Error),
    Invalid(&'static str),
    InvalidDetail(String),
    UnsupportedContainerVersion(u16),
    UnsupportedCodec { id: u16, version: u16 },
    Limit(&'static str),
}

impl fmt::Display for FormatError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(error) => write!(f, "rdz file IO failed: {error}"),
            Self::Invalid(message) => write!(f, "invalid rdz file: {message}"),
            Self::InvalidDetail(message) => write!(f, "invalid rdz file: {message}"),
            Self::UnsupportedContainerVersion(version) => {
                write!(f, "unsupported rdz format version {version}")
            }
            Self::UnsupportedCodec { id, version } => {
                write!(f, "unsupported rdz codec {id} version {version}")
            }
            Self::Limit(message) => {
                write!(f, "invalid rdz file: {message} exceeds its format limit")
            }
        }
    }
}

impl std::error::Error for FormatError {}

impl From<std::io::Error> for FormatError {
    fn from(value: std::io::Error) -> Self {
        Self::Io(value)
    }
}

pub(crate) fn checked_add(left: u64, right: u64) -> Result<u64, FormatError> {
    left.checked_add(right)
        .ok_or(FormatError::Invalid("offset arithmetic overflow"))
}

pub(crate) fn to_usize(value: u64) -> Result<usize, FormatError> {
    usize::try_from(value).map_err(|_| FormatError::Limit("allocation size"))
}

pub(crate) fn try_vec_with_capacity<T>(
    capacity: usize,
    label: &'static str,
) -> Result<Vec<T>, FormatError> {
    let mut output = Vec::new();
    output
        .try_reserve_exact(capacity)
        .map_err(|_| FormatError::Limit(label))?;
    Ok(output)
}

pub(crate) fn try_zeroed_vec(length: usize, label: &'static str) -> Result<Vec<u8>, FormatError> {
    let mut output = try_vec_with_capacity(length, label)?;
    output.resize(length, 0);
    Ok(output)
}

pub(crate) fn read_u16(input: &[u8], offset: usize) -> Result<u16, FormatError> {
    let bytes = input
        .get(offset..offset + 2)
        .ok_or(FormatError::Invalid("truncated structure"))?;
    let array =
        <[u8; 2]>::try_from(bytes).map_err(|_| FormatError::Invalid("truncated structure"))?;
    Ok(u16::from_le_bytes(array))
}

pub(crate) fn read_u32(input: &[u8], offset: usize) -> Result<u32, FormatError> {
    let bytes = input
        .get(offset..offset + 4)
        .ok_or(FormatError::Invalid("truncated structure"))?;
    let array =
        <[u8; 4]>::try_from(bytes).map_err(|_| FormatError::Invalid("truncated structure"))?;
    Ok(u32::from_le_bytes(array))
}

pub(crate) fn read_u64(input: &[u8], offset: usize) -> Result<u64, FormatError> {
    let bytes = input
        .get(offset..offset + 8)
        .ok_or(FormatError::Invalid("truncated structure"))?;
    let array =
        <[u8; 8]>::try_from(bytes).map_err(|_| FormatError::Invalid("truncated structure"))?;
    Ok(u64::from_le_bytes(array))
}

pub(crate) fn put_u16(output: &mut [u8], offset: usize, value: u16) {
    output[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
}

pub(crate) fn put_u32(output: &mut [u8], offset: usize, value: u32) {
    output[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

pub(crate) fn put_u64(output: &mut [u8], offset: usize, value: u64) {
    output[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}
