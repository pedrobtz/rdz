mod atomic_file;
mod checksum;
mod codec;
mod container;
mod format;
mod r_adapter;

use std::path::Path;

use codec::string::{StringChunk, StringEncoding, StringRef};

use savvy::{
    NotAvailableValue, OwnedIntegerSexp, OwnedListSexp, OwnedLogicalSexp, OwnedRawSexp,
    OwnedRealSexp, OwnedStringSexp, RawSexp, Sexp, savvy,
};

#[savvy]
fn rdz_try_write_native(x: Sexp, path: &str, strict: bool) -> savvy::Result<savvy::Sexp> {
    let written =
        r_adapter::try_write_native(x, Path::new(path), strict).map_err(savvy::Error::new)?;
    Ok(OwnedLogicalSexp::try_from(written)?.into())
}

#[savvy]
fn rdz_root_length(x: Sexp) -> savvy::Result<savvy::Sexp> {
    let length = r_adapter::root_length(x);
    let value = length.map_or(-1.0, |value| value as f64);
    Ok(OwnedRealSexp::try_from(value)?.into())
}

#[savvy]
fn rdz_write_generic(payload: RawSexp, synopsis: RawSexp, path: &str) -> savvy::Result<()> {
    container::write_generic(Path::new(path), payload.as_slice(), synopsis.as_slice())
        .map_err(savvy::Error::new)
}

#[savvy]
fn rdz_read(path: &str) -> savvy::Result<savvy::Sexp> {
    let mut reader = container::open(Path::new(path)).map_err(savvy::Error::new)?;
    let mut result = OwnedListSexp::new(2, true)?;
    match reader.codec_id() {
        format::CODEC_R_SERIAL_V3 => {
            let mut payload =
                OwnedRawSexp::new(reader.generic_payload_len().map_err(savvy::Error::new)?)?;
            reader
                .read_generic_into(payload.as_mut_slice())
                .map_err(savvy::Error::new)?;
            result.set_name_and_value(0, "serialized", OwnedLogicalSexp::try_from(true)?)?;
            result.set_name_and_value(1, "value", payload)?;
        }
        format::CODEC_NATIVE_V1 => {
            let length = reader.native_logical_len().map_err(savvy::Error::new)?;
            let mut value = unsafe { OwnedLogicalSexp::new_without_init(length)? };
            let raw = if length == 0 {
                &mut []
            } else {
                unsafe { std::slice::from_raw_parts_mut(savvy_ffi::LOGICAL(value.inner()), length) }
            };
            reader
                .read_native_logical_values(raw, <i32>::na())
                .map_err(savvy::Error::new)?;
            if let Some(names) = read_names(&mut reader)? {
                value.set_attrib("names", names.into())?;
            }
            result.set_name_and_value(0, "serialized", OwnedLogicalSexp::try_from(false)?)?;
            result.set_name_and_value(1, "value", value)?;
        }
        _ => return Err(savvy::Error::new("unsupported validated rdz codec")),
    }
    Ok(result.into())
}

#[savvy]
fn rdz_read_native_names(path: &str) -> savvy::Result<savvy::Sexp> {
    let mut reader = container::open(Path::new(path)).map_err(savvy::Error::new)?;
    match read_names(&mut reader)? {
        Some(names) => Ok(names.into()),
        None => Ok(OwnedStringSexp::new(0)?.into()),
    }
}

/// Builds the native `names` attribute as an R character vector.
///
/// Plain records and dictionary entries become CHARSXPs straight from the
/// decoded block bytes, under one protected region per block. Dictionary
/// indices reuse the entry's CHARSXP, so a repeated string costs one pointer
/// store instead of a hash-and-lookup in R's global string cache.
fn read_names(reader: &mut container::ContainerReader) -> savvy::Result<Option<OwnedStringSexp>> {
    let Some(length) = reader.native_names_len().map_err(savvy::Error::new)? else {
        return Ok(None);
    };
    let dictionary_len = reader
        .native_names_dictionary_len()
        .map_err(savvy::Error::new)?;
    let output = OwnedStringSexp::new(length)?;
    // Protected for the whole read; a dictionary CHARSXP must survive the
    // allocations between its creation and its first use.
    let dictionary = OwnedStringSexp::new(dictionary_len)?;
    let (target, entries) = (output.inner(), dictionary.inner());
    let mut filled = 0_usize;
    let mut entries_filled = 0_usize;
    reader.read_native_names_with(|chunk| -> savvy::Result<()> {
        match chunk {
            StringChunk::Plain(refs) => make_chars(target, &mut filled, length, refs),
            StringChunk::DictionaryEntries(refs) => {
                make_chars(entries, &mut entries_filled, dictionary_len, refs)
            }
            StringChunk::DictionaryIndices(ids) => {
                let end = filled
                    .checked_add(ids.len())
                    .filter(|&end| end <= length)
                    .ok_or_else(|| savvy::Error::new("character object length mismatch"))?;
                // The reader rejects any id at or beyond the entries already
                // delivered, all of which are in `entries`.
                for (offset, &id) in ids.iter().enumerate() {
                    unsafe {
                        savvy_ffi::SET_STRING_ELT(
                            target,
                            (filled + offset) as isize,
                            savvy_ffi::STRING_ELT(entries, id as isize),
                        );
                    }
                }
                filled = end;
                Ok(())
            }
        }
    })?;
    if filled != length {
        return Err(savvy::Error::new("character object length mismatch"));
    }
    Ok(Some(output))
}

/// Appends `refs` to the STRSXP `target` at `*filled`, one `unwind_protect`
/// for the whole block. A context per element costs a `setjmp` per R call.
fn make_chars(
    target: savvy_ffi::SEXP,
    filled: &mut usize,
    capacity: usize,
    refs: &[StringRef<'_>],
) -> savvy::Result<()> {
    let start = *filled;
    let end = start
        .checked_add(refs.len())
        .filter(|&end| end <= capacity)
        .ok_or_else(|| savvy::Error::new("character object length mismatch"))?;
    if refs.iter().any(|value| {
        matches!(value, StringRef::Value { bytes, .. } if i32::try_from(bytes.len()).is_err())
    }) {
        return Err(savvy::Error::new("a string is longer than R permits"));
    }
    unsafe {
        savvy::unwind_protect(|| {
            for (offset, value) in refs.iter().enumerate() {
                let charsxp = match value {
                    StringRef::Na => savvy_ffi::R_NaString,
                    StringRef::Value { encoding, bytes } => savvy_ffi::Rf_mkCharLenCE(
                        bytes.as_ptr().cast(),
                        bytes.len() as i32,
                        r_encoding(*encoding),
                    ),
                };
                savvy_ffi::SET_STRING_ELT(target, (start + offset) as isize, charsxp);
            }
            target
        })?
    };
    *filled = end;
    Ok(())
}

fn r_encoding(encoding: StringEncoding) -> savvy_ffi::cetype_t {
    match encoding {
        StringEncoding::Native => savvy_ffi::cetype_t_CE_NATIVE,
        StringEncoding::Utf8 => savvy_ffi::cetype_t_CE_UTF8,
        StringEncoding::Latin1 => savvy_ffi::cetype_t_CE_LATIN1,
        StringEncoding::Bytes => savvy_ffi::cetype_t_CE_BYTES,
    }
}

#[savvy]
fn rdz_file_info(path: &str) -> savvy::Result<savvy::Sexp> {
    let info = container::read_info(Path::new(path)).map_err(savvy::Error::new)?;
    let codec = match (info.codec_id, info.codec_version) {
        (format::CODEC_R_SERIAL_V3, format::R_SERIAL_CODEC_VERSION) => "r_serial_v3",
        (format::CODEC_NATIVE_V1, format::NATIVE_CODEC_VERSION) => "native_v1",
        _ => "unknown",
    };
    let mut output = OwnedListSexp::new(14, true)?;
    output.set_name_and_value(
        0,
        "container_version",
        OwnedIntegerSexp::try_from(i32::from(info.container_version))?,
    )?;
    output.set_name_and_value(1, "codec", OwnedStringSexp::try_from(codec)?)?;
    output.set_name_and_value(
        2,
        "codec_id",
        OwnedIntegerSexp::try_from(i32::from(info.codec_id))?,
    )?;
    output.set_name_and_value(
        3,
        "codec_version",
        OwnedIntegerSexp::try_from(i32::from(info.codec_version))?,
    )?;
    output.set_name_and_value(
        4,
        "block_size",
        OwnedIntegerSexp::try_from(i32::try_from(info.block_size).map_err(savvy::Error::new)?)?,
    )?;
    output.set_name_and_value(
        5,
        "block_count",
        OwnedIntegerSexp::try_from(i32::try_from(info.block_count).map_err(savvy::Error::new)?)?,
    )?;
    output.set_name_and_value(
        6,
        "object_count",
        OwnedIntegerSexp::try_from(i32::try_from(info.object_count).map_err(savvy::Error::new)?)?,
    )?;
    output.set_name_and_value(
        7,
        "attribute_count",
        OwnedIntegerSexp::try_from(
            i32::try_from(info.attribute_count).map_err(savvy::Error::new)?,
        )?,
    )?;
    output.set_name_and_value(
        8,
        "payload_bytes",
        OwnedRealSexp::try_from(info.payload_bytes as f64)?,
    )?;
    output.set_name_and_value(
        9,
        "file_bytes",
        OwnedRealSexp::try_from(info.file_bytes as f64)?,
    )?;
    output.set_name_and_value(10, "synopsis", OwnedRawSexp::try_from_slice(info.synopsis)?)?;
    output.set_name_and_value(
        11,
        "root_type",
        OwnedStringSexp::try_from(info.root_type.unwrap_or(""))?,
    )?;
    output.set_name_and_value(
        12,
        "root_length",
        OwnedRealSexp::try_from(info.root_length.map_or(-1.0, |value| value as f64))?,
    )?;
    output.set_name_and_value(
        13,
        "attribute_names",
        OwnedStringSexp::try_from_slice(info.attribute_names)?,
    )?;
    Ok(output.into())
}
