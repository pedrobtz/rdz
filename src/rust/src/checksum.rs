/// Streaming XXH3-64 (seed 0), bit-identical to the reference xxHash and to
/// zufast's `zuf_hash64()`, which the C implementation uses.
pub(crate) struct Xxh3 {
    hasher: xxhash_rust::xxh3::Xxh3,
}

impl Xxh3 {
    pub(crate) fn new() -> Self {
        Self {
            hasher: xxhash_rust::xxh3::Xxh3::new(),
        }
    }

    pub(crate) fn update(&mut self, bytes: &[u8]) {
        self.hasher.update(bytes);
    }

    pub(crate) fn finalize(self) -> u64 {
        self.hasher.digest()
    }
}

pub(crate) fn xxh3(bytes: &[u8]) -> u64 {
    xxhash_rust::xxh3::xxh3_64(bytes)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn matches_reference_test_vectors() {
        assert_eq!(xxh3(b""), 0x2d06_8005_38d3_94c2);
        assert_eq!(xxh3(b"a"), 0xe6c6_32b6_1e96_4e1f);
    }

    #[test]
    fn chunked_and_contiguous_updates_match() {
        let input: Vec<u8> = (0..1000_u32).map(|i| (i * 31 % 251) as u8).collect();
        let mut chunked = Xxh3::new();
        for chunk in input.chunks(3) {
            chunked.update(chunk);
        }

        assert_eq!(chunked.finalize(), xxh3(&input));
    }
}
