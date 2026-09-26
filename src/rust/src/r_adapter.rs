use std::fmt;
use std::path::Path;

use savvy::{LogicalSexp, NotAvailableValue, Sexp, StringSexp, TypedSexp};

use crate::codec::string::{StringEncoding, StringValue};
use crate::container;
use crate::format::{BLOCK_SIZE, FormatError};

unsafe extern "C" {
    fn rdz_logical_attribute_kind(value: savvy::ffi::SEXP) -> std::os::raw::c_int;
    fn rdz_is_altrep(value: savvy::ffi::SEXP) -> std::os::raw::c_int;
    fn rdz_charsxp_length(value: savvy::ffi::SEXP) -> std::os::raw::c_int;
    fn rdz_charsxp_encoding(value: savvy::ffi::SEXP) -> std::os::raw::c_int;
}

#[derive(Debug)]
pub(crate) enum AdapterError {
    UnsupportedNative(String),
    Format(FormatError),
}

impl fmt::Display for AdapterError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::UnsupportedNative(kind) => write!(
                f,
                "native serialization is not implemented for {kind}; use `mode = \"auto\"` or `mode = \"r\"`"
            ),
            Self::Format(error) => error.fmt(f),
        }
    }
}

impl std::error::Error for AdapterError {}

impl From<FormatError> for AdapterError {
    fn from(value: FormatError) -> Self {
        Self::Format(value)
    }
}

pub(crate) fn try_write_native(x: Sexp, path: &Path, strict: bool) -> Result<bool, AdapterError> {
    let type_name = x.get_human_readable_type_name().to_string();
    match x.into_typed() {
        TypedSexp::Logical(value) => match prepare_logical_names(&value) {
            Ok(names) => {
                container::write_native_logical(
                    path,
                    value.as_slice_raw(),
                    <i32>::na(),
                    names.as_deref(),
                )?;
                Ok(true)
            }
            Err(_) if !strict => Ok(false),
            Err(reason) => Err(AdapterError::UnsupportedNative(reason)),
        },
        _ if !strict => Ok(false),
        _ => Err(AdapterError::UnsupportedNative(type_name)),
    }
}

fn prepare_logical_names(value: &LogicalSexp) -> Result<Option<Vec<StringValue>>, String> {
    match unsafe { rdz_logical_attribute_kind(value.inner()) } {
        0 => Ok(None),
        1 => {
            let names = match value.get_attrib("names") {
                Ok(Some(names)) => names,
                _ => return Err("a logical vector with invalid names".to_string()),
            };
            if unsafe { rdz_is_altrep(names.0) } != 0 {
                return Err("a logical vector with ALTREP names".to_string());
            }
            let names = match StringSexp::try_from(names) {
                Ok(names) => names,
                Err(_) => return Err("a logical vector with invalid names".to_string()),
            };
            let mut output = Vec::new();
            output
                .try_reserve_exact(names.len())
                .map_err(|_| "names allocation exceeds its limit".to_string())?;
            for index in 0..names.len() {
                match inspect_string(names.inner(), index) {
                    Ok(Some((StringEncoding::Native, bytes))) if !bytes.is_ascii() => {
                        return Err(
                            "a logical vector with non-ASCII native-encoded names".to_string()
                        );
                    }
                    Ok(Some((_, bytes))) if bytes.len() + 5 > BLOCK_SIZE => {
                        return Err(
                            "a logical vector with a name larger than one block".to_string()
                        );
                    }
                    Ok(None) => output.push(StringValue::Na),
                    Ok(Some((encoding, bytes))) => {
                        output.push(StringValue::Value { encoding, bytes })
                    }
                    Err(reason) => return Err(reason),
                }
            }
            Ok(Some(output))
        }
        -2 => Err("an ALTREP logical vector".to_string()),
        -1 => Err("a logical vector with attributes other than names".to_string()),
        _ => Err("a malformed logical vector".to_string()),
    }
}

fn inspect_string(
    strings: savvy::ffi::SEXP,
    index: usize,
) -> Result<Option<(StringEncoding, Vec<u8>)>, String> {
    let charsxp = unsafe { savvy_ffi::STRING_ELT(strings, index as isize) };
    if charsxp == unsafe { savvy_ffi::R_NaString } {
        return Ok(None);
    }
    let length = unsafe { rdz_charsxp_length(charsxp) };
    let length = usize::try_from(length).map_err(|_| "negative character length".to_string())?;
    let encoding = match unsafe { rdz_charsxp_encoding(charsxp) } {
        savvy_ffi::cetype_t_CE_NATIVE => StringEncoding::Native,
        savvy_ffi::cetype_t_CE_UTF8 => StringEncoding::Utf8,
        savvy_ffi::cetype_t_CE_LATIN1 => StringEncoding::Latin1,
        savvy_ffi::cetype_t_CE_BYTES => StringEncoding::Bytes,
        _ => return Err("a name has an unsupported R encoding tag".to_string()),
    };
    let pointer = unsafe { savvy_ffi::R_CHAR(charsxp) }.cast::<u8>();
    let source = unsafe { std::slice::from_raw_parts(pointer, length) };
    let mut bytes = Vec::new();
    bytes
        .try_reserve_exact(length)
        .map_err(|_| "name bytes exceed their allocation limit".to_string())?;
    bytes.extend_from_slice(source);
    Ok(Some((encoding, bytes)))
}

pub(crate) fn root_length(x: Sexp) -> Option<usize> {
    match x.into_typed() {
        TypedSexp::Integer(value) => Some(value.len()),
        TypedSexp::Real(value) => Some(value.len()),
        TypedSexp::Logical(value) => Some(value.len()),
        TypedSexp::Raw(value) => Some(value.len()),
        TypedSexp::String(value) => Some(value.len()),
        TypedSexp::List(value) => Some(value.len()),
        TypedSexp::Null(_) => Some(0),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn adapter_errors_are_distinguished() {
        assert!(
            AdapterError::UnsupportedNative("integer".to_string())
                .to_string()
                .starts_with("native serialization is not implemented")
        );
    }
}
