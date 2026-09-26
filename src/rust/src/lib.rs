mod atomic_file;
mod checksum;
mod codec;
mod container;
mod format;
mod r_adapter;

use std::path::Path;

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
    let reader = container::open(Path::new(path)).map_err(savvy::Error::new)?;
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
            let names = reader
                .read_native_logical_into(raw, <i32>::na())
                .map_err(savvy::Error::new)?;
            if let Some(names) = names {
                let names = strings_to_sexp(names)?;
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
    let reader = container::open(Path::new(path)).map_err(savvy::Error::new)?;
    let names = reader
        .read_native_names()
        .map_err(savvy::Error::new)?
        .unwrap_or_default();
    Ok(strings_to_sexp(names)?.into())
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

fn strings_to_sexp(values: Vec<codec::string::StringValue>) -> savvy::Result<OwnedStringSexp> {
    let mut output = OwnedStringSexp::new(values.len())?;
    for (index, value) in values.into_iter().enumerate() {
        match value {
            codec::string::StringValue::Na => output.set_na(index)?,
            codec::string::StringValue::Value { encoding, bytes } => {
                let length = i32::try_from(bytes.len()).map_err(savvy::Error::new)?;
                let r_encoding = match encoding {
                    codec::string::StringEncoding::Native => savvy_ffi::cetype_t_CE_NATIVE,
                    codec::string::StringEncoding::Utf8 => savvy_ffi::cetype_t_CE_UTF8,
                    codec::string::StringEncoding::Latin1 => savvy_ffi::cetype_t_CE_LATIN1,
                    codec::string::StringEncoding::Bytes => savvy_ffi::cetype_t_CE_BYTES,
                };
                let charsxp = unsafe {
                    savvy::unwind_protect(|| {
                        savvy_ffi::Rf_mkCharLenCE(bytes.as_ptr().cast(), length, r_encoding)
                    })?
                };
                unsafe {
                    savvy::unwind_protect(|| {
                        savvy_ffi::SET_STRING_ELT(output.inner(), index as isize, charsxp);
                        output.inner()
                    })?
                };
            }
        }
    }
    Ok(output)
}
