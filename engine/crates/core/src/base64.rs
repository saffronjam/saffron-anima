//! Standard base64 for shipping small binary blobs over the JSON control plane.

use base64::Engine as _;
use base64::engine::general_purpose::STANDARD;

/// Encodes a byte buffer as standard base64 (RFC 4648, the `A-Za-z0-9+/`
/// alphabet with `=` padding).
///
/// Used to carry small binary blobs (e.g. thumbnail PNGs) over the JSON control
/// protocol.
#[must_use]
pub fn base64_encode(bytes: &[u8]) -> String {
    STANDARD.encode(bytes)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn empty_buffer() {
        assert_eq!(base64_encode(&[]), "");
    }

    #[test]
    fn one_byte_tail_double_pad() {
        // One trailing byte → two padding chars.
        assert_eq!(base64_encode(b"f"), "Zg==");
    }

    #[test]
    fn two_byte_tail_single_pad() {
        // Two trailing bytes → one padding char.
        assert_eq!(base64_encode(b"fo"), "Zm8=");
    }

    #[test]
    fn three_byte_group_no_pad() {
        // A full three-byte group → no padding.
        assert_eq!(base64_encode(b"foo"), "Zm9v");
    }

    #[test]
    fn rfc4648_vectors() {
        assert_eq!(base64_encode(b"foob"), "Zm9vYg==");
        assert_eq!(base64_encode(b"fooba"), "Zm9vYmE=");
        assert_eq!(base64_encode(b"foobar"), "Zm9vYmFy");
    }

    #[test]
    fn high_bytes_use_plus_and_slash() {
        // 0xFF,0xFF,0xFF → all-ones sextets exercise the '+' and '/' table entries.
        assert_eq!(base64_encode(&[0xFF, 0xFF, 0xFF]), "////");
        assert_eq!(base64_encode(&[0xFB, 0xFF, 0xBF]), "+/+/");
    }
}
