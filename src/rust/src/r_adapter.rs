use std::fmt;
use std::os::raw::{c_char, c_int};
use std::path::Path;

use savvy::{LogicalSexp, NotAvailableValue, Sexp, StringSexp, TypedSexp};

use crate::codec::string::{DictionaryPolicy, StringEncoding, StringRef, StringSource};
use crate::container;
use crate::format::{BLOCK_SIZE, FormatError};

unsafe extern "C" {
    fn rdz_logical_attribute_kind(value: savvy::ffi::SEXP) -> c_int;
    fn rdz_is_altrep(value: savvy::ffi::SEXP) -> c_int;
    fn rdz_charsxp_view(
        value: savvy::ffi::SEXP,
        length: *mut c_int,
        encoding: *mut c_int,
        ascii: *mut c_int,
    ) -> *const c_char;
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
    let unsupported = |reason: String| {
        if strict {
            Err(AdapterError::UnsupportedNative(reason))
        } else {
            Ok(false)
        }
    };
    match x.into_typed() {
        TypedSexp::Logical(value) => {
            let names = match logical_names(&value) {
                Ok(names) => names,
                Err(reason) => return unsupported(reason),
            };
            // `names` is an attribute of `value`, so its CHARSXPs stay alive
            // and unmoved for the whole synchronous write.
            let source = names.map(|names| unsafe { RStrings::new(names) });
            match container::write_native_logical(
                path,
                value.as_slice_raw(),
                <i32>::na(),
                source.as_ref(),
                dictionary_policy(),
            ) {
                Ok(()) => Ok(true),
                Err(FormatError::UnsupportedValue(reason)) => unsupported(reason.to_string()),
                Err(error) => Err(error.into()),
            }
        }
        _ => unsupported(type_name),
    }
}

/// Experimental: `RDZ_STRING_DICT` selects `plain` (default), `block`,
/// `global`, or `auto` dictionary encoding for native character values.
fn dictionary_policy() -> DictionaryPolicy {
    match std::env::var("RDZ_STRING_DICT").as_deref() {
        Ok("block") => DictionaryPolicy::Block,
        Ok("global") => DictionaryPolicy::Global,
        Ok("auto") => DictionaryPolicy::Auto,
        _ => DictionaryPolicy::Plain,
    }
}

/// Returns the names STRSXP of an otherwise attribute-free, non-ALTREP
/// logical vector, or a reason the vector is not natively supported.
fn logical_names(value: &LogicalSexp) -> Result<Option<savvy::ffi::SEXP>, String> {
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
            match StringSexp::try_from(names) {
                Ok(names) => Ok(Some(names.inner())),
                Err(_) => Err("a logical vector with invalid names".to_string()),
            }
        }
        -2 => Err("an ALTREP logical vector".to_string()),
        -1 => Err("a logical vector with attributes other than names".to_string()),
        _ => Err("a malformed logical vector".to_string()),
    }
}

/// A non-ALTREP STRSXP viewed as a [`StringSource`]. Keys are CHARSXP
/// addresses; R's global string cache makes them unique per value. Values
/// are validated lazily, so repeats in dictionary mode are never re-read.
struct RStrings<'a> {
    elements: &'a [savvy::ffi::SEXP],
    na: savvy::ffi::SEXP,
}

impl RStrings<'_> {
    /// # Safety
    /// `strings` must be a live, non-ALTREP STRSXP that outlives the value
    /// and is not modified while it is in use.
    unsafe fn new(strings: savvy::ffi::SEXP) -> Self {
        let length = unsafe { savvy_ffi::Rf_xlength(strings) } as usize;
        let elements = if length == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(savvy_ffi::STRING_PTR_RO(strings), length) }
        };
        Self {
            elements,
            na: unsafe { savvy_ffi::R_NaString },
        }
    }
}

impl StringSource for RStrings<'_> {
    fn len(&self) -> usize {
        self.elements.len()
    }

    fn key(&self, index: usize) -> usize {
        self.elements
            .get(index)
            .map_or(0, |&charsxp| charsxp as usize)
    }

    fn value(&self, index: usize) -> Result<StringRef<'_>, FormatError> {
        let charsxp = *self
            .elements
            .get(index)
            .ok_or(FormatError::Invalid("string index is out of range"))?;
        if charsxp == self.na {
            return Ok(StringRef::Na);
        }
        let (mut length, mut encoding, mut ascii) = (0, 0, 0);
        let pointer = unsafe { rdz_charsxp_view(charsxp, &mut length, &mut encoding, &mut ascii) };
        let encoding = match encoding {
            savvy_ffi::cetype_t_CE_NATIVE if ascii != 0 => StringEncoding::Native,
            savvy_ffi::cetype_t_CE_NATIVE => {
                return Err(FormatError::UnsupportedValue(
                    "a logical vector with non-ASCII native-encoded names",
                ));
            }
            savvy_ffi::cetype_t_CE_UTF8 => StringEncoding::Utf8,
            savvy_ffi::cetype_t_CE_LATIN1 => StringEncoding::Latin1,
            savvy_ffi::cetype_t_CE_BYTES => StringEncoding::Bytes,
            _ => {
                return Err(FormatError::UnsupportedValue(
                    "a name with an unsupported R encoding tag",
                ));
            }
        };
        let length = usize::try_from(length)
            .map_err(|_| FormatError::Invalid("negative character length"))?;
        if length + 5 > BLOCK_SIZE {
            return Err(FormatError::UnsupportedValue(
                "a logical vector with a name larger than one block",
            ));
        }
        let bytes = if length == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(pointer.cast::<u8>(), length) }
        };
        Ok(StringRef::Value { encoding, bytes })
    }
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
