// CBOR, in the four kinds a reply is made of.
//
// The interactors on this bus write CBOR and say so, and `transport-bus-cli` decodes exactly
// four kinds -- maps, text, byte strings and integers -- on the argument that a decoder
// handling everything RFC 8949 allows would be a library, and a reply needing one is a reply
// whose shape nobody agreed on. This is the C++ side of that same agreement, so a caller and
// an interactor do not each invent an encoding.
//
// It is deliberately not a CBOR library. There is no tagging, no indefinite length, no
// float16, no negative-integer decode. Anything outside the four kinds is refused rather than
// guessed at: a truncated message read as a short one is a wrong answer where an error would
// have been a missing one.
//
// SPDX-License-Identifier: Apache-2.0
#ifndef WEFT_CBOR_HPP
#define WEFT_CBOR_HPP

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace weft::cbor {

// ── writing ──────────────────────────────────────────────────────────────────────────────
inline void head(std::vector<unsigned char> &o, unsigned major, std::uint64_t n) {
	const unsigned char m = static_cast<unsigned char>(major << 5);
	if (n < 24) { o.push_back(m | static_cast<unsigned char>(n)); return; }
	if (n <= 0xff) { o.push_back(m | 24); o.push_back(static_cast<unsigned char>(n)); return; }
	if (n <= 0xffff) {
		o.push_back(m | 25);
		o.push_back(static_cast<unsigned char>(n >> 8));
		o.push_back(static_cast<unsigned char>(n));
		return;
	}
	if (n <= 0xffffffffu) {
		o.push_back(m | 26);
		for (int s = 24; s >= 0; s -= 8) o.push_back(static_cast<unsigned char>(n >> s));
		return;
	}
	o.push_back(m | 27);
	for (int s = 56; s >= 0; s -= 8) o.push_back(static_cast<unsigned char>(n >> s));
}

inline void map(std::vector<unsigned char> &o, std::uint64_t pairs) { head(o, 5, pairs); }
inline void uint(std::vector<unsigned char> &o, std::uint64_t v) { head(o, 0, v); }

inline void text(std::vector<unsigned char> &o, const char *s) {
	const std::size_t n = std::strlen(s);
	head(o, 3, n);
	o.insert(o.end(), s, s + n);
}

inline void bytes(std::vector<unsigned char> &o, const void *p, std::size_t n) {
	head(o, 2, n);
	const unsigned char *b = static_cast<const unsigned char *>(p);
	o.insert(o.end(), b, b + n);
}

// A double as its eight IEEE-754 bytes, major 7 / additional 27. Coordinates are doubles all
// the way through the mesher, and rounding them into text to cross the bus would put a
// precision decision in the transport rather than in the algorithm.
inline void real(std::vector<unsigned char> &o, double v) {
	std::uint64_t bits = 0;
	std::memcpy(&bits, &v, 8);
	o.push_back(static_cast<unsigned char>(7 << 5) | 27);
	for (int s = 56; s >= 0; s -= 8) o.push_back(static_cast<unsigned char>(bits >> s));
}

inline void boolean(std::vector<unsigned char> &o, bool b) {
	o.push_back(static_cast<unsigned char>(7 << 5) | (b ? 21 : 20));
}

// ── reading ──────────────────────────────────────────────────────────────────────────────
struct Reader {
	const unsigned char *p = nullptr;
	const unsigned char *end = nullptr;
	bool bad = false;

	Reader(const void *data, std::size_t n)
			: p(static_cast<const unsigned char *>(data)),
			  end(static_cast<const unsigned char *>(data) + n) {}

	bool need(std::size_t n) {
		if (bad || std::size_t(end - p) < n) { bad = true; return false; }
		return true;
	}

	// major and argument, or bad. Anything needing a tag or an indefinite length sets bad.
	bool header(unsigned &major, std::uint64_t &arg) {
		if (!need(1)) return false;
		const unsigned char b = *p++;
		major = b >> 5;
		const unsigned char ai = b & 31;
		if (ai < 24) { arg = ai; return true; }
		std::size_t width = ai == 24 ? 1 : ai == 25 ? 2 : ai == 26 ? 4 : ai == 27 ? 8 : 0;
		if (width == 0 || !need(width)) { bad = true; return false; }
		arg = 0;
		for (std::size_t i = 0; i < width; ++i) arg = (arg << 8) | *p++;
		return true;
	}

	bool map_len(std::uint64_t &pairs) {
		unsigned m = 0;
		if (!header(m, pairs) || m != 5) { bad = true; return false; }
		return true;
	}

	bool key(std::string &out) {
		unsigned m = 0; std::uint64_t n = 0;
		if (!header(m, n) || m != 3 || !need(n)) { bad = true; return false; }
		out.assign(reinterpret_cast<const char *>(p), n);
		p += n;
		return true;
	}

	bool uint(std::uint64_t &v) {
		unsigned m = 0;
		if (!header(m, v) || m != 0) { bad = true; return false; }
		return true;
	}

	bool bytes(const unsigned char *&at, std::size_t &n) {
		unsigned m = 0; std::uint64_t len = 0;
		if (!header(m, len) || m != 2 || !need(len)) { bad = true; return false; }
		at = p; n = static_cast<std::size_t>(len); p += len;
		return true;
	}

	bool real(double &v) {
		if (!need(1)) return false;
		const unsigned char b = *p;
		if (b != ((7 << 5) | 27)) { bad = true; return false; }
		++p;
		if (!need(8)) return false;
		std::uint64_t bits = 0;
		for (int i = 0; i < 8; ++i) bits = (bits << 8) | *p++;
		std::memcpy(&v, &bits, 8);
		return true;
	}
};

}  // namespace weft::cbor

#endif
