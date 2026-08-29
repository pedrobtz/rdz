/// Streaming IEEE CRC32 calculation with runtime-selected CPU acceleration.
pub(crate) struct Crc32 {
    hasher: crc32fast::Hasher,
}

impl Crc32 {
    pub(crate) fn new() -> Self {
        Self {
            hasher: crc32fast::Hasher::new(),
        }
    }

    pub(crate) fn update(&mut self, bytes: &[u8]) {
        self.hasher.update(bytes);
    }

    pub(crate) fn finalize(self) -> u32 {
        self.hasher.finalize()
    }
}

pub(crate) fn crc32(bytes: &[u8]) -> u32 {
    let mut checksum = Crc32::new();
    checksum.update(bytes);
    checksum.finalize()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn matches_standard_test_vector() {
        assert_eq!(crc32(b"123456789"), 0xcbf4_3926);
    }

    #[test]
    fn chunked_and_contiguous_updates_match() {
        let input = b"an incrementally checksummed payload";
        let mut chunked = Crc32::new();
        for chunk in input.chunks(3) {
            chunked.update(chunk);
        }

        assert_eq!(chunked.finalize(), crc32(input));
    }
}
