//! Minimal SHA-1, used by the mock pool server to build job targets and to
//! re-verify shares before accepting them.

pub fn sha1(input: &[u8]) -> [u8; 20] {
    let mut h = [
        0x67452301u32,
        0xefcdab89u32,
        0x98badcfeu32,
        0x10325476u32,
        0xc3d2e1f0u32,
    ];

    let bit_len = (input.len() as u64) * 8;

    // Process 64-byte blocks, padding the last one the usual way.
    let mut block = [0u8; 64];
    let mut pos = 0;
    let mut i = 0;

    while i < input.len() {
        block[pos] = input[i];
        pos += 1;
        i += 1;
        if pos == 64 {
            compress(&mut h, &block);
            pos = 0;
        }
    }

    block[pos] = 0x80;
    pos += 1;
    if pos > 56 {
        while pos < 64 {
            block[pos] = 0;
            pos += 1;
        }
        compress(&mut h, &block);
        pos = 0;
        block[..64].fill(0);
    }
    while pos < 56 {
        block[pos] = 0;
        pos += 1;
    }
    for k in 0..8 {
        block[56 + k] = ((bit_len >> (56 - 8 * k)) & 0xff) as u8;
    }
    compress(&mut h, &block);

    let mut out = [0u8; 20];
    for (n, word) in h.iter().enumerate() {
        let w = *word;
        out[n * 4] = (w >> 24) as u8;
        out[n * 4 + 1] = (w >> 16) as u8;
        out[n * 4 + 2] = (w >> 8) as u8;
        out[n * 4 + 3] = w as u8;
    }
    out
}

fn compress(h: &mut [u32; 5], block: &[u8; 64]) {
    let mut w = [0u32; 80];
    for i in 0..16 {
        w[i] = ((block[i * 4] as u32) << 24)
            | ((block[i * 4 + 1] as u32) << 16)
            | ((block[i * 4 + 2] as u32) << 8)
            | (block[i * 4 + 3] as u32);
    }
    for i in 16..80 {
        let s = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
        w[i] = (s << 1) | (s >> 31);
    }

    let mut a = h[0];
    let mut b = h[1];
    let mut c = h[2];
    let mut d = h[3];
    let mut e = h[4];

    for i in 0..80 {
        let k = match i / 20 {
            0 => 0x5a827999,
            1 => 0x6ed9eba1,
            2 => 0x8f1bbcdc,
            _ => 0xca62c1d6,
        };
        let f = match i / 20 {
            0 => (b & c) + (!b & d),
            1 => b ^ c ^ d,
            2 => (b & c) | (b & d) | (c & d),
            _ => b ^ c ^ d,
        };
        let temp = a
            .rotate_left(5)
            .wrapping_add(e)
            .wrapping_add(w[i])
            .wrapping_add(k)
            .wrapping_add(f);
        e = d;
        d = c;
        c = b.rotate_left(30);
        b = a;
        a = temp;
    }

    h[0] = h[0].wrapping_add(a);
    h[1] = h[1].wrapping_add(b);
    h[2] = h[2].wrapping_add(c);
    h[3] = h[3].wrapping_add(d);
    h[4] = h[4].wrapping_add(e);
}

pub fn hex(bytes: &[u8]) -> String {
    let mut s = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        s.push_str(&format!("{b:02x}"));
    }
    s
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn known_vectors() {
        assert_eq!(
            hex(&sha1(b"ba29a15896fd2d792d5c4b60668bf2b9feebc51d50")),
            "d0beba883d7e8cd119ea2b0e09b78f60f29e0968"
        );
        assert_eq!(hex(&sha1(b"")), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
        assert_eq!(
            hex(&sha1(b"The quick brown fox jumps over the lazy dog")),
            "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12"
        );
        // block-boundary padding cases
        assert_eq!(hex(&sha1(&[b'a'; 64])), "0098ba824b5c16427bd7a1122a5a442a25ec644d");
        let mut tail63 = [b'a'; 64];
        tail63[63] = b'x';
        assert_eq!(hex(&sha1(&tail63)), "b672b5c55c0a99c7810c311dd759ee857ec0e27d");
        // cross-checked against Python's hashlib
        let table = [
            ("deadbeefcafeb0b1d5e3a2c4f6098712abcdef011234", 1, "b62939d26c4605b24a8a1a5b4fffdfd704cc1961"),
            ("deadbeefcafeb0b1d5e3a2c4f6098712abcdef011234", 999, "073dcb78e3f071d833afe75498c4749b190c9f2c"),
            ("deadbeefcafeb0b1d5e3a2c4f6098712abcdef011234", 9999, "2647b4d37b7058ce5616ba99786749587d71d1e4"),
            ("deadbeefcafeb0b1d5e3a2c4f6098712abcdef011234", 65500, "9391a0aec5ca9c880ad3a0c01517b48850dc341e"),
            ("deadbeefcafeb0b1d5e3a2c4f6098712abcdef011234", 123456, "4b27de12ece86b81fda7011348a4ec9de2facdff"),
            ("0123456789abcdef0123456789abcdef01234567", 10, "0ebccf14133bc7f3b2b122f814d01fc9870e6b39"),
            ("0123456789abcdef0123456789abcdef01234567", 1000, "92aade62192f6c69a745b017f0d437da8ed43449"),
            ("0123456789abcdef0123456789abcdef01234567", 10000, "669ef7758be29e9391493da1b630e0aee5dbe606"),
            ("7a8f1c3e9b2d6f4a5e8c1b3d7f9a2c4e6b8d0f1a", 0, "96c051e03004c911e7805a87bc1d5dab960ef6fa"),
            ("7a8f1c3e9b2d6f4a5e8c1b3d7f9a2c4e6b8d0f1a", 999, "5ec86e2fd9e260de013f83c187d5821140fbea56"),
            ("7a8f1c3e9b2d6f4a5e8c1b3d7f9a2c4e6b8d0f1a", 65500, "3374a031b4ee7414db253e9f0c1c178f2969640b"),
        ];
        for (prev, nonce, want) in table {
            assert_eq!(hex(&sha1(format!("{prev}{nonce}").as_bytes())), want, "{prev} + {nonce}");
        }
    }
}
