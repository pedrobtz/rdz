use crate::format::{
    ENCODING_LOGICAL_2BIT, ENCODING_LOGICAL_CONSTANT, ENCODING_LOGICAL_DENSE_PLANES,
    ENCODING_LOGICAL_PERIODIC, ENCODING_LOGICAL_RUN_ENDS, ENCODING_LOGICAL_SPARSE_PATCHES,
    FormatError, put_u16, put_u32, read_u16, read_u32, try_zeroed_vec,
};

pub(crate) const FALSE_STATE: u8 = 0;
pub(crate) const TRUE_STATE: u8 = 1;
pub(crate) const NA_STATE: u8 = 2;

const DENSE_HEADER_LEN: usize = 4;
const CONSTANT_HEADER_LEN: usize = 4;
const SPARSE_HEADER_LEN: usize = 16;
const RUN_HEADER_LEN: usize = 8;
const RUN_RECORD_LEN: usize = 8;
const PERIODIC_HEADER_LEN: usize = 8;
const MAX_PERIOD: usize = 64;

#[derive(Debug, PartialEq, Eq)]
pub(crate) struct EncodedBlock {
    pub(crate) encoding: u16,
    pub(crate) payload: Vec<u8>,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct LogicalStats {
    counts: [usize; 3],
    runs: usize,
}

#[derive(Debug, PartialEq, Eq)]
struct LogicalPlanes {
    bytes: Vec<u8>,
    plane_len: usize,
    stats: LogicalStats,
}

impl LogicalPlanes {
    fn true_plane(&self) -> &[u8] {
        &self.bytes[..self.plane_len]
    }

    fn na_plane(&self) -> &[u8] {
        &self.bytes[self.plane_len..]
    }
}

const fn expand_nibble_table() -> [u8; 16] {
    let mut table = [0_u8; 16];
    let mut value = 0_usize;
    while value < 16 {
        let mut expanded = 0_u8;
        let mut bit = 0_usize;
        while bit < 4 {
            expanded |= (((value >> bit) & 1) as u8) << (bit * 2);
            bit += 1;
        }
        table[value] = expanded;
        value += 1;
    }
    table
}

const fn decode_table() -> [[i32; 4]; 256] {
    let mut table = [[0_i32; 4]; 256];
    let mut byte = 0_usize;
    while byte < 256 {
        let mut index = 0_usize;
        while index < 4 {
            table[byte][index] = match ((byte >> (index * 2)) & 0x03) as u8 {
                FALSE_STATE => 0,
                TRUE_STATE => 1,
                NA_STATE => i32::MIN,
                _ => 0,
            };
            index += 1;
        }
        byte += 1;
    }
    table
}

const fn expand_byte_table() -> [u64; 256] {
    let mut table = [0_u64; 256];
    let mut value = 0_usize;
    while value < 256 {
        let mut expanded = 0_u64;
        let mut bit = 0_usize;
        while bit < 8 {
            if value & (1 << bit) != 0 {
                expanded |= 0xff_u64 << (bit * 8);
            }
            bit += 1;
        }
        table[value] = expanded;
        value += 1;
    }
    table
}

static EXPAND_NIBBLE: [u8; 16] = expand_nibble_table();
static EXPAND_BYTE: [u64; 256] = expand_byte_table();
static DECODE_TABLE: [[i32; 4]; 256] = decode_table();

pub(crate) fn packed_len(logical_count: usize) -> Result<usize, FormatError> {
    logical_count
        .checked_add(3)
        .map(|value| value / 4)
        .ok_or(FormatError::Limit("logical block length"))
}

fn plane_len(logical_count: usize) -> Result<usize, FormatError> {
    logical_count
        .checked_add(7)
        .map(|value| value / 8)
        .ok_or(FormatError::Limit("logical bitmap length"))
}

pub(crate) fn encode(values: &[i32], na_value: i32) -> Result<Vec<u8>, FormatError> {
    let length = packed_len(values.len())?;
    let mut output = try_zeroed_vec(length, "logical block allocation")?;
    for (index, value) in values.iter().copied().enumerate() {
        let is_true = value == 1;
        let is_na = value == na_value;
        if value != 0 && !is_true && !is_na {
            return Err(FormatError::Invalid(
                "logical vector contains an invalid internal value",
            ));
        }
        let state = u8::from(is_true) | (u8::from(is_na) << 1);
        output[index / 4] |= state << ((index % 4) * 2);
    }
    Ok(output)
}

pub(crate) fn encode_adaptive(values: &[i32], na_value: i32) -> Result<EncodedBlock, FormatError> {
    let planes = classify(values, na_value)?;
    if values.is_empty() {
        return encode_dense(&planes, 0, FALSE_STATE);
    }

    let default_state = modal_state(&planes.stats.counts);
    if planes.stats.counts[default_state as usize] == values.len() {
        return encode_constant(default_state);
    }

    let present_planes = present_non_default_states(&planes.stats.counts, default_state);
    let dense_len = DENSE_HEADER_LEN
        .checked_add(
            present_planes
                .len()
                .checked_mul(planes.plane_len)
                .ok_or(FormatError::Limit("logical dense block"))?,
        )
        .ok_or(FormatError::Limit("logical dense block"))?;
    let exception_count = values.len() - planes.stats.counts[default_state as usize];
    let sparse_len = SPARSE_HEADER_LEN
        .checked_add(
            exception_count
                .checked_mul(2)
                .ok_or(FormatError::Limit("logical sparse block"))?,
        )
        .ok_or(FormatError::Limit("logical sparse block"))?;
    let run_len = RUN_HEADER_LEN
        .checked_add(
            planes
                .stats
                .runs
                .checked_mul(RUN_RECORD_LEN)
                .ok_or(FormatError::Limit("logical run block"))?,
        )
        .ok_or(FormatError::Limit("logical run block"))?;
    let periodic = (planes.stats.runs > values.len() / 2)
        .then(|| find_short_period(values))
        .flatten();
    let periodic_len = periodic
        .map(|period| {
            PERIODIC_HEADER_LEN
                .checked_add(packed_len(period)?)
                .ok_or(FormatError::Limit("logical periodic block"))
        })
        .transpose()?;

    if periodic_len
        .is_some_and(|length| length < dense_len && length < sparse_len && length < run_len)
    {
        encode_periodic(values, na_value, periodic.unwrap_or_default())
    } else if run_len < dense_len && run_len <= sparse_len {
        encode_runs(&planes, values.len(), planes.stats.runs)
    } else if sparse_len < dense_len {
        encode_sparse(&planes, values.len(), default_state, &present_planes)
    } else {
        encode_dense(&planes, values.len(), default_state)
    }
}

fn find_short_period(values: &[i32]) -> Option<usize> {
    for period in 2..=MAX_PERIOD.min(values.len() / 4) {
        if values[period..]
            .iter()
            .zip(&values[..values.len() - period])
            .all(|(current, previous)| current == previous)
        {
            return Some(period);
        }
    }
    None
}

fn encode_periodic(
    values: &[i32],
    na_value: i32,
    period: usize,
) -> Result<EncodedBlock, FormatError> {
    if !(2..=MAX_PERIOD).contains(&period) || values.len() < period * 4 {
        return Err(FormatError::Invalid("invalid logical periodic candidate"));
    }
    let pattern = encode(&values[..period], na_value)?;
    let mut payload = try_zeroed_vec(
        PERIODIC_HEADER_LEN
            .checked_add(pattern.len())
            .ok_or(FormatError::Limit("logical periodic block"))?,
        "logical periodic block",
    )?;
    put_u16(
        &mut payload,
        0,
        u16::try_from(period).map_err(|_| FormatError::Limit("logical period"))?,
    );
    payload[PERIODIC_HEADER_LEN..].copy_from_slice(&pattern);
    Ok(EncodedBlock {
        encoding: ENCODING_LOGICAL_PERIODIC,
        payload,
    })
}

fn encode_constant(state: u8) -> Result<EncodedBlock, FormatError> {
    let mut payload = try_zeroed_vec(CONSTANT_HEADER_LEN, "logical constant block")?;
    payload[0] = state;
    Ok(EncodedBlock {
        encoding: ENCODING_LOGICAL_CONSTANT,
        payload,
    })
}

fn encode_dense(
    planes: &LogicalPlanes,
    logical_count: usize,
    default_state: u8,
) -> Result<EncodedBlock, FormatError> {
    let states = present_non_default_states(&planes.stats.counts, default_state);
    let length = DENSE_HEADER_LEN
        .checked_add(
            states
                .len()
                .checked_mul(planes.plane_len)
                .ok_or(FormatError::Limit("logical dense block"))?,
        )
        .ok_or(FormatError::Limit("logical dense block"))?;
    let mut payload = try_zeroed_vec(length, "logical dense block")?;
    payload[0] = default_state;
    payload[1] = states
        .iter()
        .fold(0_u8, |mask, state| mask | (1_u8 << state));
    let mut offset = DENSE_HEADER_LEN;
    for state in states {
        write_state_plane(
            planes,
            state,
            logical_count,
            &mut payload[offset..offset + planes.plane_len],
        )?;
        offset += planes.plane_len;
    }
    Ok(EncodedBlock {
        encoding: ENCODING_LOGICAL_DENSE_PLANES,
        payload,
    })
}

fn encode_sparse(
    planes: &LogicalPlanes,
    logical_count: usize,
    default_state: u8,
    states: &[u8],
) -> Result<EncodedBlock, FormatError> {
    if states.is_empty() || states.len() > 2 || logical_count > u16::MAX as usize + 1 {
        return Err(FormatError::Invalid("invalid logical sparse candidate"));
    }
    let count_one = planes.stats.counts[states[0] as usize];
    let count_two = states
        .get(1)
        .map_or(0, |state| planes.stats.counts[*state as usize]);
    let length = SPARSE_HEADER_LEN
        .checked_add(
            count_one
                .checked_add(count_two)
                .and_then(|count| count.checked_mul(2))
                .ok_or(FormatError::Limit("logical sparse block"))?,
        )
        .ok_or(FormatError::Limit("logical sparse block"))?;
    let mut payload = try_zeroed_vec(length, "logical sparse block")?;
    payload[0] = default_state;
    payload[1] = states[0];
    payload[2] = states.get(1).copied().unwrap_or(u8::MAX);
    put_u32(
        &mut payload,
        4,
        u32::try_from(count_one).map_err(|_| FormatError::Limit("logical sparse count"))?,
    );
    put_u32(
        &mut payload,
        8,
        u32::try_from(count_two).map_err(|_| FormatError::Limit("logical sparse count"))?,
    );
    let mut offset = SPARSE_HEADER_LEN;
    for state in states {
        write_sparse_positions(planes, *state, logical_count, &mut payload, &mut offset)?;
    }
    if offset != payload.len() {
        return Err(FormatError::Invalid("logical sparse count mismatch"));
    }
    Ok(EncodedBlock {
        encoding: ENCODING_LOGICAL_SPARSE_PATCHES,
        payload,
    })
}

fn write_sparse_positions(
    planes: &LogicalPlanes,
    state: u8,
    logical_count: usize,
    output: &mut [u8],
    offset: &mut usize,
) -> Result<(), FormatError> {
    for byte_index in 0..planes.plane_len {
        let mut bits = match state {
            TRUE_STATE => planes.true_plane()[byte_index],
            NA_STATE => planes.na_plane()[byte_index],
            FALSE_STATE => {
                !(planes.true_plane()[byte_index] | planes.na_plane()[byte_index])
                    & valid_bits_for_byte(byte_index, planes.plane_len, logical_count)
            }
            _ => return Err(FormatError::Invalid("invalid logical sparse state")),
        };
        while bits != 0 {
            let bit = bits.trailing_zeros() as usize;
            let position = u16::try_from(byte_index * 8 + bit)
                .map_err(|_| FormatError::Limit("logical sparse position"))?;
            put_u16(output, *offset, position);
            *offset += 2;
            bits &= bits - 1;
        }
    }
    Ok(())
}

fn write_state_plane(
    planes: &LogicalPlanes,
    state: u8,
    logical_count: usize,
    output: &mut [u8],
) -> Result<(), FormatError> {
    if output.len() != planes.plane_len {
        return Err(FormatError::Invalid("logical plane output length mismatch"));
    }
    match state {
        TRUE_STATE => output.copy_from_slice(planes.true_plane()),
        NA_STATE => output.copy_from_slice(planes.na_plane()),
        FALSE_STATE => {
            for (byte_index, destination) in output.iter_mut().enumerate() {
                *destination = !(planes.true_plane()[byte_index] | planes.na_plane()[byte_index])
                    & valid_bits_for_byte(byte_index, planes.plane_len, logical_count);
            }
        }
        _ => return Err(FormatError::Invalid("invalid logical plane state")),
    }
    Ok(())
}

fn encode_runs(
    planes: &LogicalPlanes,
    logical_count: usize,
    expected_runs: usize,
) -> Result<EncodedBlock, FormatError> {
    let length = RUN_HEADER_LEN
        .checked_add(
            expected_runs
                .checked_mul(RUN_RECORD_LEN)
                .ok_or(FormatError::Limit("logical run block"))?,
        )
        .ok_or(FormatError::Limit("logical run block"))?;
    let mut payload = try_zeroed_vec(length, "logical run block")?;
    put_u32(
        &mut payload,
        0,
        u32::try_from(expected_runs).map_err(|_| FormatError::Limit("logical run count"))?,
    );
    let mut offset = RUN_HEADER_LEN;
    let mut previous_state = state_from_planes(planes, 0)?;
    let mut previous_bits = (false, false);
    for group_start in (0..logical_count).step_by(32) {
        let width = (logical_count - group_start).min(32);
        let true_bits = read_plane_mask(planes.true_plane(), group_start / 8, width)?;
        let na_bits = read_plane_mask(planes.na_plane(), group_start / 8, width)?;
        let predecessor_true = (true_bits << 1) | u32::from(previous_bits.0);
        let predecessor_na = (na_bits << 1) | u32::from(previous_bits.1);
        let mut transitions = (true_bits ^ predecessor_true) | (na_bits ^ predecessor_na);
        if group_start == 0 {
            transitions &= !1;
        }
        let valid_mask = if width == 32 {
            u32::MAX
        } else {
            (1_u32 << width) - 1
        };
        transitions &= valid_mask;
        while transitions != 0 {
            let bit = transitions.trailing_zeros() as usize;
            let end = group_start + bit;
            put_u32(
                &mut payload,
                offset,
                u32::try_from(end).map_err(|_| FormatError::Limit("logical run end"))?,
            );
            payload[offset + 4] = previous_state;
            offset += RUN_RECORD_LEN;
            previous_state = state_from_masks(true_bits, na_bits, bit);
            transitions &= transitions - 1;
        }
        let last_bit = width - 1;
        previous_bits = (
            true_bits & (1_u32 << last_bit) != 0,
            na_bits & (1_u32 << last_bit) != 0,
        );
    }
    put_u32(
        &mut payload,
        offset,
        u32::try_from(logical_count).map_err(|_| FormatError::Limit("logical run end"))?,
    );
    payload[offset + 4] = previous_state;
    offset += RUN_RECORD_LEN;
    if offset != payload.len() {
        return Err(FormatError::Invalid("logical run count mismatch"));
    }
    Ok(EncodedBlock {
        encoding: ENCODING_LOGICAL_RUN_ENDS,
        payload,
    })
}

fn read_plane_mask(plane: &[u8], byte_offset: usize, width: usize) -> Result<u32, FormatError> {
    let byte_count = width.div_ceil(8);
    let source = plane
        .get(byte_offset..byte_offset + byte_count)
        .ok_or(FormatError::Invalid(
            "logical plane mask exceeds its bounds",
        ))?;
    let mut bytes = [0_u8; 4];
    bytes[..byte_count].copy_from_slice(source);
    Ok(u32::from_le_bytes(bytes))
}

fn state_from_planes(planes: &LogicalPlanes, position: usize) -> Result<u8, FormatError> {
    if position >= planes.stats.counts.iter().sum() {
        return Err(FormatError::Invalid(
            "logical plane position exceeds its bounds",
        ));
    }
    let bit = 1_u8 << (position % 8);
    let byte = position / 8;
    Ok(if planes.true_plane()[byte] & bit != 0 {
        TRUE_STATE
    } else if planes.na_plane()[byte] & bit != 0 {
        NA_STATE
    } else {
        FALSE_STATE
    })
}

fn state_from_masks(true_bits: u32, na_bits: u32, bit: usize) -> u8 {
    if true_bits & (1_u32 << bit) != 0 {
        TRUE_STATE
    } else if na_bits & (1_u32 << bit) != 0 {
        NA_STATE
    } else {
        FALSE_STATE
    }
}

pub(crate) fn decode_block_into(
    encoded: &[u8],
    encoding: u16,
    logical_count: usize,
    na_value: i32,
    output: &mut [i32],
) -> Result<(), FormatError> {
    if output.len() != logical_count {
        return Err(FormatError::Invalid("logical block length mismatch"));
    }
    if na_value != i32::MIN {
        return Err(FormatError::Invalid(
            "unsupported logical NA representation",
        ));
    }
    match encoding {
        ENCODING_LOGICAL_2BIT => decode_into(encoded, logical_count, na_value, output),
        ENCODING_LOGICAL_CONSTANT => decode_constant(encoded, logical_count, output),
        ENCODING_LOGICAL_DENSE_PLANES => decode_dense(encoded, logical_count, output),
        ENCODING_LOGICAL_SPARSE_PATCHES => decode_sparse(encoded, logical_count, output),
        ENCODING_LOGICAL_RUN_ENDS => decode_runs(encoded, logical_count, output),
        ENCODING_LOGICAL_PERIODIC => decode_periodic(encoded, logical_count, output),
        _ => Err(FormatError::Invalid("unsupported logical block encoding")),
    }
}

fn decode_periodic(
    encoded: &[u8],
    logical_count: usize,
    output: &mut [i32],
) -> Result<(), FormatError> {
    if encoded.len() < PERIODIC_HEADER_LEN
        || read_u16(encoded, 2)? != 0
        || read_u32(encoded, 4)? != 0
    {
        return Err(FormatError::Invalid("invalid logical periodic header"));
    }
    let period = read_u16(encoded, 0)? as usize;
    if !(2..=MAX_PERIOD).contains(&period)
        || logical_count < period * 4
        || encoded.len() != PERIODIC_HEADER_LEN + packed_len(period)?
    {
        return Err(FormatError::Invalid("invalid logical periodic length"));
    }
    decode_into(
        &encoded[PERIODIC_HEADER_LEN..],
        period,
        i32::MIN,
        &mut output[..period],
    )?;
    let mut filled = period;
    while filled < logical_count {
        let copied = filled.min(logical_count - filled);
        output.copy_within(..copied, filled);
        filled += copied;
    }
    Ok(())
}

fn decode_constant(
    encoded: &[u8],
    logical_count: usize,
    output: &mut [i32],
) -> Result<(), FormatError> {
    if logical_count == 0
        || encoded.len() != CONSTANT_HEADER_LEN
        || encoded[0] > NA_STATE
        || encoded[1..].iter().any(|value| *value != 0)
    {
        return Err(FormatError::Invalid("invalid logical constant block"));
    }
    output.fill(value_from_state(encoded[0])?);
    Ok(())
}

fn decode_dense(
    encoded: &[u8],
    logical_count: usize,
    output: &mut [i32],
) -> Result<(), FormatError> {
    if encoded.len() < DENSE_HEADER_LEN || encoded[2] != 0 || encoded[3] != 0 {
        return Err(FormatError::Invalid("invalid logical dense header"));
    }
    let default_state = encoded[0];
    let mask = encoded[1];
    if default_state > NA_STATE || mask & !0x07 != 0 || mask & (1_u8 << default_state) != 0 {
        return Err(FormatError::Invalid("invalid logical dense state mask"));
    }
    if logical_count == 0 {
        if mask != 0 || encoded.len() != DENSE_HEADER_LEN || default_state != FALSE_STATE {
            return Err(FormatError::Invalid("invalid empty logical block"));
        }
        return Ok(());
    }
    if mask == 0 {
        return Err(FormatError::Invalid("non-canonical logical dense block"));
    }
    let bitmap_len = plane_len(logical_count)?;
    let plane_count = mask.count_ones() as usize;
    let expected_len = DENSE_HEADER_LEN
        .checked_add(
            plane_count
                .checked_mul(bitmap_len)
                .ok_or(FormatError::Limit("logical dense block"))?,
        )
        .ok_or(FormatError::Limit("logical dense block"))?;
    if encoded.len() != expected_len {
        return Err(FormatError::Invalid("logical dense block length mismatch"));
    }

    let mut planes: [Option<&[u8]>; 3] = [None, None, None];
    let mut offset = DENSE_HEADER_LEN;
    for state in FALSE_STATE..=NA_STATE {
        if mask & (1_u8 << state) != 0 {
            planes[state as usize] = Some(&encoded[offset..offset + bitmap_len]);
            offset += bitmap_len;
        }
    }
    validate_plane_padding(&planes, logical_count)?;

    #[cfg(target_arch = "x86_64")]
    if std::is_x86_feature_detected!("avx2") {
        // SAFETY: runtime detection proves AVX2 is available. The decoder
        // bounds every bitmap read and output store using `logical_count`.
        return unsafe {
            decode_dense_avx2(&planes, default_state, logical_count, bitmap_len, output)
        };
    }

    decode_dense_scalar(&planes, default_state, logical_count, bitmap_len, output)
}

fn dense_plane_byte(
    planes: &[Option<&[u8]>; 3],
    default_state: u8,
    byte_index: usize,
    bitmap_len: usize,
    logical_count: usize,
) -> Result<(u8, u8), FormatError> {
    let stored_false = planes[FALSE_STATE as usize].map_or(0, |plane| plane[byte_index]);
    let stored_true = planes[TRUE_STATE as usize].map_or(0, |plane| plane[byte_index]);
    let stored_na = planes[NA_STATE as usize].map_or(0, |plane| plane[byte_index]);
    if stored_false & stored_true != 0
        || stored_false & stored_na != 0
        || stored_true & stored_na != 0
    {
        return Err(FormatError::Invalid("overlapping logical dense planes"));
    }
    let valid_bits = valid_bits_for_byte(byte_index, bitmap_len, logical_count);
    let default_bits = valid_bits & !(stored_false | stored_true | stored_na);
    Ok((
        stored_true | u8::from(default_state == TRUE_STATE) * default_bits,
        stored_na | u8::from(default_state == NA_STATE) * default_bits,
    ))
}

fn decode_dense_scalar(
    planes: &[Option<&[u8]>; 3],
    default_state: u8,
    logical_count: usize,
    bitmap_len: usize,
    output: &mut [i32],
) -> Result<(), FormatError> {
    let mut output_offset = 0_usize;
    for byte_index in 0..bitmap_len {
        let (true_bits, na_bits) =
            dense_plane_byte(planes, default_state, byte_index, bitmap_len, logical_count)?;
        decode_plane_byte(true_bits, na_bits, output, &mut output_offset)?;
    }
    if output_offset != logical_count {
        return Err(FormatError::Invalid("logical dense output length mismatch"));
    }
    Ok(())
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn decode_dense_avx2(
    planes: &[Option<&[u8]>; 3],
    default_state: u8,
    logical_count: usize,
    bitmap_len: usize,
    output: &mut [i32],
) -> Result<(), FormatError> {
    use std::arch::x86_64::*;

    let one = _mm256_set1_epi32(1);
    let na_value = _mm256_set1_epi32(i32::MIN);
    let complete_bytes = logical_count / 8;
    for byte_index in 0..complete_bytes {
        let (true_bits, na_bits) =
            dense_plane_byte(planes, default_state, byte_index, bitmap_len, logical_count)?;
        let true_bytes = _mm_cvtsi64_si128(EXPAND_BYTE[true_bits as usize] as i64);
        let na_bytes = _mm_cvtsi64_si128(EXPAND_BYTE[na_bits as usize] as i64);
        let true_lanes = _mm256_cvtepi8_epi32(true_bytes);
        let na_lanes = _mm256_cvtepi8_epi32(na_bytes);
        let values = _mm256_or_si256(
            _mm256_and_si256(true_lanes, one),
            _mm256_and_si256(na_lanes, na_value),
        );
        // SAFETY: each complete bitmap byte represents exactly eight output
        // values and `complete_bytes == logical_count / 8`.
        unsafe { _mm256_storeu_si256(output.as_mut_ptr().add(byte_index * 8).cast(), values) };
    }

    let mut output_offset = complete_bytes * 8;
    if output_offset != logical_count {
        let (true_bits, na_bits) = dense_plane_byte(
            planes,
            default_state,
            complete_bytes,
            bitmap_len,
            logical_count,
        )?;
        decode_plane_byte(true_bits, na_bits, output, &mut output_offset)?;
    }
    if output_offset != logical_count {
        return Err(FormatError::Invalid("logical dense output length mismatch"));
    }
    Ok(())
}

fn decode_sparse(
    encoded: &[u8],
    logical_count: usize,
    output: &mut [i32],
) -> Result<(), FormatError> {
    if encoded.len() < SPARSE_HEADER_LEN
        || logical_count == 0
        || logical_count > u16::MAX as usize + 1
        || encoded[3] != 0
        || read_u32(encoded, 12)? != 0
    {
        return Err(FormatError::Invalid("invalid logical sparse header"));
    }
    let default_state = encoded[0];
    let first_state = encoded[1];
    let second_state = encoded[2];
    if default_state > NA_STATE
        || first_state > NA_STATE
        || first_state == default_state
        || (second_state != u8::MAX
            && (second_state > NA_STATE
                || second_state == default_state
                || second_state <= first_state))
    {
        return Err(FormatError::Invalid("invalid logical sparse states"));
    }
    let first_count = read_u32(encoded, 4)? as usize;
    let second_count = read_u32(encoded, 8)? as usize;
    if first_count == 0 || (second_state == u8::MAX) != (second_count == 0) {
        return Err(FormatError::Invalid("invalid logical sparse counts"));
    }
    let expected_len = SPARSE_HEADER_LEN
        .checked_add(
            first_count
                .checked_add(second_count)
                .and_then(|count| count.checked_mul(2))
                .ok_or(FormatError::Limit("logical sparse block"))?,
        )
        .ok_or(FormatError::Limit("logical sparse block"))?;
    if encoded.len() != expected_len || first_count + second_count >= logical_count {
        return Err(FormatError::Invalid("logical sparse block length mismatch"));
    }
    output.fill(value_from_state(default_state)?);
    let mut offset = SPARSE_HEADER_LEN;
    decode_sparse_positions(
        encoded,
        &mut offset,
        first_count,
        first_state,
        default_state,
        output,
    )?;
    if second_count != 0 {
        decode_sparse_positions(
            encoded,
            &mut offset,
            second_count,
            second_state,
            default_state,
            output,
        )?;
    }
    Ok(())
}

fn decode_sparse_positions(
    encoded: &[u8],
    offset: &mut usize,
    count: usize,
    state: u8,
    default_state: u8,
    output: &mut [i32],
) -> Result<(), FormatError> {
    let mut previous = None;
    for _ in 0..count {
        let position = read_u16(encoded, *offset)? as usize;
        *offset += 2;
        if position >= output.len() || previous.is_some_and(|value| position <= value) {
            return Err(FormatError::Invalid("invalid logical sparse position"));
        }
        if output[position] != value_from_state(default_state)? {
            return Err(FormatError::Invalid("overlapping logical sparse positions"));
        }
        output[position] = value_from_state(state)?;
        previous = Some(position);
    }
    Ok(())
}

fn decode_runs(
    encoded: &[u8],
    logical_count: usize,
    output: &mut [i32],
) -> Result<(), FormatError> {
    if encoded.len() < RUN_HEADER_LEN || logical_count == 0 || read_u32(encoded, 4)? != 0 {
        return Err(FormatError::Invalid("invalid logical run header"));
    }
    let run_count = read_u32(encoded, 0)? as usize;
    let expected_len = RUN_HEADER_LEN
        .checked_add(
            run_count
                .checked_mul(RUN_RECORD_LEN)
                .ok_or(FormatError::Limit("logical run block"))?,
        )
        .ok_or(FormatError::Limit("logical run block"))?;
    if run_count == 0 || encoded.len() != expected_len || run_count > logical_count {
        return Err(FormatError::Invalid("logical run block length mismatch"));
    }
    let mut start = 0_usize;
    let mut previous_state = None;
    for run in 0..run_count {
        let offset = RUN_HEADER_LEN + run * RUN_RECORD_LEN;
        let end = read_u32(encoded, offset)? as usize;
        let state = encoded[offset + 4];
        if encoded[offset + 5..offset + 8]
            .iter()
            .any(|value| *value != 0)
            || state > NA_STATE
            || previous_state == Some(state)
            || end <= start
            || end > logical_count
        {
            return Err(FormatError::Invalid("invalid logical run record"));
        }
        output[start..end].fill(value_from_state(state)?);
        start = end;
        previous_state = Some(state);
    }
    if start != logical_count {
        return Err(FormatError::Invalid("logical run ends before the block"));
    }
    Ok(())
}

fn decode_plane_byte(
    true_bits: u8,
    na_bits: u8,
    output: &mut [i32],
    output_offset: &mut usize,
) -> Result<(), FormatError> {
    for shift in [0_u32, 4] {
        if *output_offset == output.len() {
            break;
        }
        let true_nibble = ((true_bits >> shift) & 0x0f) as usize;
        let na_nibble = ((na_bits >> shift) & 0x0f) as usize;
        let packed = EXPAND_NIBBLE[true_nibble] | (EXPAND_NIBBLE[na_nibble] << 1);
        let count = (output.len() - *output_offset).min(4);
        let end = *output_offset + count;
        output[*output_offset..end].copy_from_slice(&DECODE_TABLE[packed as usize][..count]);
        *output_offset = end;
    }
    Ok(())
}

fn validate_plane_padding(
    planes: &[Option<&[u8]>; 3],
    logical_count: usize,
) -> Result<(), FormatError> {
    if logical_count.is_multiple_of(8) {
        return Ok(());
    }
    let invalid_mask = !((1_u8 << (logical_count % 8)) - 1);
    for plane in planes.iter().flatten() {
        if plane.last().is_some_and(|value| value & invalid_mask != 0) {
            return Err(FormatError::Invalid(
                "logical dense plane has nonzero padding bits",
            ));
        }
    }
    Ok(())
}

pub(crate) fn validate_encoded_length(
    encoding: u16,
    logical_count: usize,
    stored_len: usize,
) -> Result<(), FormatError> {
    let valid = match encoding {
        ENCODING_LOGICAL_2BIT => stored_len == packed_len(logical_count)?,
        ENCODING_LOGICAL_CONSTANT => logical_count != 0 && stored_len == CONSTANT_HEADER_LEN,
        ENCODING_LOGICAL_DENSE_PLANES => {
            if logical_count == 0 {
                stored_len == DENSE_HEADER_LEN
            } else {
                let bitmap_len = plane_len(logical_count)?;
                stored_len >= DENSE_HEADER_LEN + bitmap_len
                    && stored_len <= DENSE_HEADER_LEN + 2 * bitmap_len
            }
        }
        ENCODING_LOGICAL_SPARSE_PATCHES => {
            logical_count != 0
                && logical_count <= u16::MAX as usize + 1
                && stored_len >= SPARSE_HEADER_LEN + 2
                && stored_len <= SPARSE_HEADER_LEN + logical_count.saturating_mul(2)
        }
        ENCODING_LOGICAL_RUN_ENDS => {
            logical_count != 0
                && stored_len >= RUN_HEADER_LEN + RUN_RECORD_LEN
                && stored_len <= RUN_HEADER_LEN + logical_count.saturating_mul(RUN_RECORD_LEN)
        }
        ENCODING_LOGICAL_PERIODIC => {
            logical_count >= 8
                && stored_len >= PERIODIC_HEADER_LEN + 1
                && stored_len <= PERIODIC_HEADER_LEN + packed_len(MAX_PERIOD)?
        }
        _ => false,
    };
    if valid {
        Ok(())
    } else {
        Err(FormatError::Invalid(
            "logical block encoding length mismatch",
        ))
    }
}

pub(crate) fn is_logical_encoding(encoding: u16) -> bool {
    matches!(
        encoding,
        ENCODING_LOGICAL_2BIT
            | ENCODING_LOGICAL_CONSTANT
            | ENCODING_LOGICAL_DENSE_PLANES
            | ENCODING_LOGICAL_SPARSE_PATCHES
            | ENCODING_LOGICAL_RUN_ENDS
            | ENCODING_LOGICAL_PERIODIC
    )
}

pub(crate) fn decode_into(
    encoded: &[u8],
    logical_count: usize,
    na_value: i32,
    output: &mut [i32],
) -> Result<(), FormatError> {
    if output.len() != logical_count || encoded.len() != packed_len(logical_count)? {
        return Err(FormatError::Invalid("logical block length mismatch"));
    }
    if na_value != i32::MIN {
        return Err(FormatError::Invalid(
            "unsupported logical NA representation",
        ));
    }
    let complete_bytes = logical_count / 4;
    for (index, byte) in encoded[..complete_bytes].iter().copied().enumerate() {
        if byte & (byte >> 1) & 0x55 != 0 {
            return Err(FormatError::Invalid(
                "logical block contains the reserved two-bit state",
            ));
        }
        output[index * 4..index * 4 + 4].copy_from_slice(&DECODE_TABLE[byte as usize]);
    }
    let used = logical_count % 4;
    if used != 0 {
        let last = encoded[complete_bytes];
        if last >> (used * 2) != 0 {
            return Err(FormatError::Invalid(
                "logical block has nonzero unused high bits",
            ));
        }
        if last & (last >> 1) & 0x55 != 0 {
            return Err(FormatError::Invalid(
                "logical block contains the reserved two-bit state",
            ));
        }
        let start = complete_bytes * 4;
        output[start..].copy_from_slice(&DECODE_TABLE[last as usize][..used]);
    }
    Ok(())
}

fn classify(values: &[i32], na_value: i32) -> Result<LogicalPlanes, FormatError> {
    if na_value != i32::MIN {
        return Err(FormatError::Invalid(
            "unsupported logical NA representation",
        ));
    }
    let bitmap_len = plane_len(values.len())?;
    let bytes_len = bitmap_len
        .checked_mul(2)
        .ok_or(FormatError::Limit("logical bitmap allocation"))?;
    let mut bytes = try_zeroed_vec(bytes_len, "logical bitmap allocation")?;
    let (true_plane, na_plane) = bytes.split_at_mut(bitmap_len);

    #[cfg(target_arch = "x86_64")]
    let stats = if std::is_x86_feature_detected!("avx2") {
        // SAFETY: runtime detection above proves AVX2 is available. The kernel
        // reads only within `values` and writes within the two allocated planes.
        unsafe { classify_avx2(values, true_plane, na_plane)? }
    } else {
        classify_scalar(values, true_plane, na_plane, na_value)?
    };

    #[cfg(target_arch = "aarch64")]
    let stats = if std::arch::is_aarch64_feature_detected!("neon") {
        // SAFETY: runtime detection above proves NEON is available. The kernel
        // reads only within `values` and writes within the two allocated planes.
        unsafe { classify_neon(values, true_plane, na_plane)? }
    } else {
        classify_scalar(values, true_plane, na_plane, na_value)?
    };

    #[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
    let stats = classify_scalar(values, true_plane, na_plane, na_value)?;

    Ok(LogicalPlanes {
        bytes,
        plane_len: bitmap_len,
        stats,
    })
}

fn classify_scalar(
    values: &[i32],
    true_plane: &mut [u8],
    na_plane: &mut [u8],
    na_value: i32,
) -> Result<LogicalStats, FormatError> {
    let mut stats = LogicalStats::default();
    let mut previous = None;
    for (group, chunk) in values.chunks(32).enumerate() {
        let mut true_bits = 0_u32;
        let mut na_bits = 0_u32;
        for (lane, value) in chunk.iter().copied().enumerate() {
            let state = state_from_value(value, na_value)?;
            true_bits |= u32::from(state == TRUE_STATE) << lane;
            na_bits |= u32::from(state == NA_STATE) << lane;
        }
        write_mask(true_plane, group * 4, true_bits, chunk.len())?;
        write_mask(na_plane, group * 4, na_bits, chunk.len())?;
        update_stats(&mut stats, true_bits, na_bits, chunk.len(), &mut previous);
    }
    Ok(stats)
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn classify_avx2(
    values: &[i32],
    true_plane: &mut [u8],
    na_plane: &mut [u8],
) -> Result<LogicalStats, FormatError> {
    use std::arch::x86_64::*;

    let zero = _mm256_setzero_si256();
    let one = _mm256_set1_epi32(1);
    let na = _mm256_set1_epi32(i32::MIN);
    let groups = values.len() / 32;
    let mut stats = LogicalStats::default();
    let mut previous = None;
    for group in 0..groups {
        let mut true_bits = 0_u32;
        let mut na_bits = 0_u32;
        for lane in 0..4 {
            // SAFETY: `group < values.len() / 32` and `lane < 4`, so the
            // unaligned eight-lane load is entirely within the slice.
            let input =
                unsafe { _mm256_loadu_si256(values.as_ptr().add(group * 32 + lane * 8).cast()) };
            let is_zero = _mm256_cmpeq_epi32(input, zero);
            let is_true = _mm256_cmpeq_epi32(input, one);
            let is_na = _mm256_cmpeq_epi32(input, na);
            let valid = _mm256_or_si256(_mm256_or_si256(is_zero, is_true), is_na);
            if _mm256_movemask_ps(_mm256_castsi256_ps(valid)) != 0xff {
                return Err(FormatError::Invalid(
                    "logical vector contains an invalid internal value",
                ));
            }
            true_bits |= (_mm256_movemask_ps(_mm256_castsi256_ps(is_true)) as u32) << (lane * 8);
            na_bits |= (_mm256_movemask_ps(_mm256_castsi256_ps(is_na)) as u32) << (lane * 8);
        }
        write_mask(true_plane, group * 4, true_bits, 32)?;
        write_mask(na_plane, group * 4, na_bits, 32)?;
        update_stats(&mut stats, true_bits, na_bits, 32, &mut previous);
    }
    classify_tail(
        &values[groups * 32..],
        true_plane,
        na_plane,
        groups * 4,
        &mut stats,
        &mut previous,
    )?;
    Ok(stats)
}

#[cfg(target_arch = "aarch64")]
#[target_feature(enable = "neon")]
unsafe fn classify_neon(
    values: &[i32],
    true_plane: &mut [u8],
    na_plane: &mut [u8],
) -> Result<LogicalStats, FormatError> {
    use std::arch::aarch64::*;

    let zero = vdupq_n_s32(0);
    let one = vdupq_n_s32(1);
    let na = vdupq_n_s32(i32::MIN);
    let weights = unsafe { vld1q_u32([1_u32, 2, 4, 8].as_ptr()) };
    let groups = values.len() / 32;
    let mut stats = LogicalStats::default();
    let mut previous = None;
    for group in 0..groups {
        let mut true_bits = 0_u32;
        let mut na_bits = 0_u32;
        for lane in 0..8 {
            // SAFETY: `group < values.len() / 32` and `lane < 8`, so this
            // four-lane load is entirely within the input slice.
            let input = unsafe { vld1q_s32(values.as_ptr().add(group * 32 + lane * 4)) };
            let is_zero = vceqq_s32(input, zero);
            let is_true = vceqq_s32(input, one);
            let is_na = vceqq_s32(input, na);
            let valid = vorrq_u32(vorrq_u32(is_zero, is_true), is_na);
            let valid_mask = vaddvq_u32(vmulq_u32(vshrq_n_u32(valid, 31), weights));
            if valid_mask != 0x0f {
                return Err(FormatError::Invalid(
                    "logical vector contains an invalid internal value",
                ));
            }
            let true_mask = vaddvq_u32(vmulq_u32(vshrq_n_u32(is_true, 31), weights));
            let na_mask = vaddvq_u32(vmulq_u32(vshrq_n_u32(is_na, 31), weights));
            true_bits |= true_mask << (lane * 4);
            na_bits |= na_mask << (lane * 4);
        }
        write_mask(true_plane, group * 4, true_bits, 32)?;
        write_mask(na_plane, group * 4, na_bits, 32)?;
        update_stats(&mut stats, true_bits, na_bits, 32, &mut previous);
    }
    classify_tail(
        &values[groups * 32..],
        true_plane,
        na_plane,
        groups * 4,
        &mut stats,
        &mut previous,
    )?;
    Ok(stats)
}

fn classify_tail(
    values: &[i32],
    true_plane: &mut [u8],
    na_plane: &mut [u8],
    byte_offset: usize,
    stats: &mut LogicalStats,
    previous: &mut Option<(bool, bool)>,
) -> Result<(), FormatError> {
    if values.is_empty() {
        return Ok(());
    }
    let mut true_bits = 0_u32;
    let mut na_bits = 0_u32;
    for (lane, value) in values.iter().copied().enumerate() {
        let state = state_from_value(value, i32::MIN)?;
        true_bits |= u32::from(state == TRUE_STATE) << lane;
        na_bits |= u32::from(state == NA_STATE) << lane;
    }
    write_mask(true_plane, byte_offset, true_bits, values.len())?;
    write_mask(na_plane, byte_offset, na_bits, values.len())?;
    update_stats(stats, true_bits, na_bits, values.len(), previous);
    Ok(())
}

fn write_mask(
    output: &mut [u8],
    offset: usize,
    bits: u32,
    width: usize,
) -> Result<(), FormatError> {
    let byte_count = width.div_ceil(8);
    let destination = output
        .get_mut(offset..offset + byte_count)
        .ok_or(FormatError::Invalid("logical bitmap output is too small"))?;
    destination.copy_from_slice(&bits.to_le_bytes()[..byte_count]);
    Ok(())
}

fn update_stats(
    stats: &mut LogicalStats,
    true_bits: u32,
    na_bits: u32,
    width: usize,
    previous: &mut Option<(bool, bool)>,
) {
    let valid_mask = if width == 32 {
        u32::MAX
    } else {
        (1_u32 << width) - 1
    };
    let true_bits = true_bits & valid_mask;
    let na_bits = na_bits & valid_mask;
    let true_count = true_bits.count_ones() as usize;
    let na_count = na_bits.count_ones() as usize;
    stats.counts[TRUE_STATE as usize] += true_count;
    stats.counts[NA_STATE as usize] += na_count;
    stats.counts[FALSE_STATE as usize] += width - true_count - na_count;

    let predecessor_true = (true_bits << 1) | u32::from(previous.is_some_and(|value| value.0));
    let predecessor_na = (na_bits << 1) | u32::from(previous.is_some_and(|value| value.1));
    let mut transitions = (true_bits ^ predecessor_true) | (na_bits ^ predecessor_na);
    if previous.is_none() {
        stats.runs = 1;
        transitions &= !1;
    }
    stats.runs += (transitions & valid_mask).count_ones() as usize;
    let last_bit = width - 1;
    *previous = Some((
        true_bits & (1_u32 << last_bit) != 0,
        na_bits & (1_u32 << last_bit) != 0,
    ));
}

fn modal_state(counts: &[usize; 3]) -> u8 {
    let mut state = FALSE_STATE;
    for candidate in TRUE_STATE..=NA_STATE {
        if counts[candidate as usize] > counts[state as usize] {
            state = candidate;
        }
    }
    state
}

fn present_non_default_states(counts: &[usize; 3], default_state: u8) -> Vec<u8> {
    (FALSE_STATE..=NA_STATE)
        .filter(|state| *state != default_state && counts[*state as usize] != 0)
        .collect()
}

fn state_from_value(value: i32, na_value: i32) -> Result<u8, FormatError> {
    match value {
        0 => Ok(FALSE_STATE),
        1 => Ok(TRUE_STATE),
        value if value == na_value => Ok(NA_STATE),
        _ => Err(FormatError::Invalid(
            "logical vector contains an invalid internal value",
        )),
    }
}

fn value_from_state(state: u8) -> Result<i32, FormatError> {
    match state {
        FALSE_STATE => Ok(0),
        TRUE_STATE => Ok(1),
        NA_STATE => Ok(i32::MIN),
        _ => Err(FormatError::Invalid("invalid logical state")),
    }
}

fn valid_bits_for_byte(byte_index: usize, bitmap_len: usize, logical_count: usize) -> u8 {
    if byte_index + 1 == bitmap_len && !logical_count.is_multiple_of(8) {
        (1_u8 << (logical_count % 8)) - 1
    } else {
        u8::MAX
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn round_trip(values: &[i32]) -> Result<EncodedBlock, FormatError> {
        let encoded = encode_adaptive(values, i32::MIN)?;
        let mut output = vec![0; values.len()];
        decode_block_into(
            &encoded.payload,
            encoded.encoding,
            values.len(),
            i32::MIN,
            &mut output,
        )?;
        assert_eq!(output, values);
        Ok(encoded)
    }

    #[test]
    fn two_bit_logicals_round_trip_missing_values() -> Result<(), FormatError> {
        let na = i32::MIN;
        let input = [0, 1, na, 1, 0, na, 0];
        let encoded = encode(&input, na)?;
        assert_eq!(encoded, vec![0b01_10_01_00, 0b00_00_10_00]);
        let mut output = [0; 7];
        decode_into(&encoded, input.len(), na, &mut output)?;
        assert_eq!(output, input);
        Ok(())
    }

    #[test]
    fn adaptive_modes_cover_constant_dense_sparse_runs_and_periods() -> Result<(), FormatError> {
        assert_eq!(
            round_trip(&vec![1; 1_000])?.encoding,
            ENCODING_LOGICAL_CONSTANT
        );
        let mut seed = 0x1234_5678_u32;
        let dense = (0..10_000)
            .map(|_| {
                seed ^= seed << 13;
                seed ^= seed >> 17;
                seed ^= seed << 5;
                [0, 1, i32::MIN][seed as usize % 3]
            })
            .collect::<Vec<_>>();
        assert_eq!(round_trip(&dense)?.encoding, ENCODING_LOGICAL_DENSE_PLANES);
        let mut sparse = vec![0; 10_000];
        sparse[17] = 1;
        sparse[8_001] = i32::MIN;
        assert_eq!(
            round_trip(&sparse)?.encoding,
            ENCODING_LOGICAL_SPARSE_PATCHES
        );
        let runs = [vec![0; 3_000], vec![1; 4_000], vec![i32::MIN; 3_000]].concat();
        assert_eq!(round_trip(&runs)?.encoding, ENCODING_LOGICAL_RUN_ENDS);
        let periodic = (0..10_000)
            .map(|index| [0, 1, i32::MIN][index % 3])
            .collect::<Vec<_>>();
        assert_eq!(round_trip(&periodic)?.encoding, ENCODING_LOGICAL_PERIODIC);
        Ok(())
    }

    #[test]
    fn no_na_dense_blocks_use_one_bit_per_value() -> Result<(), FormatError> {
        let mut seed = 0x9e37_79b9_u32;
        let values = (0..65_536)
            .map(|_| {
                seed ^= seed << 13;
                seed ^= seed >> 17;
                seed ^= seed << 5;
                (seed & 1) as i32
            })
            .collect::<Vec<_>>();
        let encoded = round_trip(&values)?;
        assert_eq!(encoded.encoding, ENCODING_LOGICAL_DENSE_PLANES);
        assert_eq!(encoded.payload.len(), DENSE_HEADER_LEN + values.len() / 8);
        Ok(())
    }

    #[test]
    fn scalar_and_runtime_classifiers_match() -> Result<(), FormatError> {
        let values = (0..65_557)
            .map(|index| [0, 1, i32::MIN][index % 3])
            .collect::<Vec<_>>();
        let runtime = classify(&values, i32::MIN)?;
        let mut bytes = try_zeroed_vec(runtime.bytes.len(), "test planes")?;
        let (true_plane, na_plane) = bytes.split_at_mut(runtime.plane_len);
        let stats = classify_scalar(&values, true_plane, na_plane, i32::MIN)?;
        assert_eq!(runtime.bytes, bytes);
        assert_eq!(runtime.stats, stats);
        Ok(())
    }

    #[test]
    fn malformed_adaptive_blocks_are_rejected() -> Result<(), FormatError> {
        let mut output = [0; 8];
        assert!(
            decode_block_into(
                &[0, 0, 0, 0],
                ENCODING_LOGICAL_CONSTANT,
                8,
                i32::MIN,
                &mut output
            )
            .is_ok()
        );

        let dense = [FALSE_STATE, 0b110, 0, 0, 1, 1];
        assert!(
            decode_block_into(
                &dense,
                ENCODING_LOGICAL_DENSE_PLANES,
                8,
                i32::MIN,
                &mut output
            )
            .is_err()
        );

        let mut sparse = [0_u8; 20];
        sparse[0] = FALSE_STATE;
        sparse[1] = TRUE_STATE;
        sparse[2] = u8::MAX;
        put_u32(&mut sparse, 4, 2);
        put_u16(&mut sparse, 16, 3);
        put_u16(&mut sparse, 18, 3);
        assert!(
            decode_block_into(
                &sparse,
                ENCODING_LOGICAL_SPARSE_PATCHES,
                8,
                i32::MIN,
                &mut output
            )
            .is_err()
        );

        let mut runs = [0_u8; 24];
        put_u32(&mut runs, 0, 2);
        put_u32(&mut runs, 8, 4);
        runs[12] = TRUE_STATE;
        put_u32(&mut runs, 16, 8);
        runs[20] = TRUE_STATE;
        assert!(
            decode_block_into(&runs, ENCODING_LOGICAL_RUN_ENDS, 8, i32::MIN, &mut output).is_err()
        );

        let mut periodic = [0_u8; PERIODIC_HEADER_LEN + 1];
        put_u16(&mut periodic, 0, 1);
        assert!(
            decode_block_into(
                &periodic,
                ENCODING_LOGICAL_PERIODIC,
                8,
                i32::MIN,
                &mut output
            )
            .is_err()
        );
        Ok(())
    }

    #[test]
    fn reserved_states_and_nonzero_padding_are_rejected() {
        let mut output = [0; 1];
        assert!(decode_into(&[3], 1, i32::MIN, &mut output).is_err());
        assert!(decode_into(&[0b1000_0000], 1, i32::MIN, &mut output).is_err());
    }
}
