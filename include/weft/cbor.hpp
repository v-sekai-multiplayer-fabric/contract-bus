// The reply format, and the library that reads it.
//
// `transport-bus-cli`'s decoder opens by saying the interactors write CBOR, and handles four
// kinds -- maps, text, byte strings and integers -- on the argument that a decoder handling
// everything RFC 8949 allows would be a library, and a reply needing one is a reply whose
// shape nobody agreed on. The agreement is worth keeping. The parser is not worth writing.
//
// QCBOR does the parsing. It allocates nothing: encoding writes into a caller's buffer and
// decoding reads in place, so leaks, double frees and reference-count errors are absent rather
// than avoided. That is the property this file is about, and it was chosen against a live
// example -- a first wrapper written over a reference-counted CBOR library dropped a map entry
// silently, because the add call reported failure through a return value it was easy to ignore.
// Here the equivalent failure is recorded in the encode context and reported once, at the end,
// by a function whose result is the encoded buffer.
//
// What stays here is the agreement, not an implementation of it: the four kinds, plus doubles
// and booleans, and one place that decides what a malformed message does.
//
// SPDX-License-Identifier: Apache-2.0
#ifndef WEFT_CBOR_HPP
#define WEFT_CBOR_HPP

#include <qcbor/qcbor_decode.h>
#include <qcbor/qcbor_encode.h>
#include <qcbor/qcbor_spiffy_decode.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace weft::cbor {

// A map written straight into the caller's buffer. No allocation, and no intermediate tree:
// every Add appends bytes, and an overflow is remembered rather than thrown.
class Map {
public:
	Map(unsigned char *out, std::size_t cap) {
		QCBOREncode_Init(&ctx_, UsefulBuf{out, cap});
		QCBOREncode_OpenMap(&ctx_);
	}

	void uint(const char *key, std::uint64_t v) { QCBOREncode_AddUInt64ToMap(&ctx_, key, v); }
	void boolean(const char *key, bool v) { QCBOREncode_AddBoolToMap(&ctx_, key, v); }
	void text(const char *key, const char *v) { QCBOREncode_AddSZStringToMap(&ctx_, key, v); }

	// Doubles as doubles. Coordinates are doubles through the whole mesher, and rounding them
	// to cross the bus would put a precision decision in the transport. QCBOREncode_AddDouble
	// does not shorten to a half or single float, so what is written is what was held.
	void real(const char *key, double v) { QCBOREncode_AddDoubleToMap(&ctx_, key, v); }

	void bytes(const char *key, const void *p, std::size_t n) {
		QCBOREncode_AddBytesToMap(&ctx_, key, UsefulBufC{p, n});
	}

	// The encoded length, or 0 if anything went wrong -- an overflowed buffer, an unclosed map,
	// a key too long. One check at the end rather than one per field, because QCBOR carries the
	// first error forward and refuses to report success over it. A short write can therefore
	// never be mistaken for a short message.
	std::size_t finish() {
		QCBOREncode_CloseMap(&ctx_);
		UsefulBufC done{nullptr, 0};
		return QCBOREncode_Finish(&ctx_, &done) == QCBOR_SUCCESS ? done.len : 0;
	}

private:
	QCBOREncodeContext ctx_{};
};

// A map read in place. Nothing is copied and nothing is owned, so the bytes must outlive it.
//
// Every getter is looked up by label rather than by position, which is what makes the format
// an agreement instead of an ordering: a reply that gains a field does not move the others.
class Reading {
public:
	Reading(const void *data, std::size_t n) {
		ok_ = well_formed(data, n);
		if (!ok_) return;
		QCBORDecode_Init(&ctx_, UsefulBufC{data, n}, QCBOR_DECODE_MODE_NORMAL);
		QCBORDecode_EnterMap(&ctx_, nullptr);
		ok_ = QCBORDecode_GetError(&ctx_) == QCBOR_SUCCESS;
	}

	// True only if the message was a well-formed map. A truncated or malformed message is
	// refused here rather than surfacing as a missing field later, because the two need
	// different answers: one is a broken sender, the other is an older one.
	bool ok() const { return ok_; }

	bool text(const char *key, std::string &out) {
		UsefulBufC s{nullptr, 0};
		QCBORDecode_GetTextStringInMapSZ(&ctx_, key, &s);
		if (!clear()) return false;
		out.assign(static_cast<const char *>(s.ptr), s.len);
		return true;
	}

	bool uint(const char *key, std::uint64_t &out) {
		QCBORDecode_GetUInt64InMapSZ(&ctx_, key, &out);
		return clear();
	}

	bool boolean(const char *key, bool &out) {
		QCBORDecode_GetBoolInMapSZ(&ctx_, key, &out);
		return clear();
	}

	bool real(const char *key, double &out) {
		QCBORDecode_GetDoubleInMapSZ(&ctx_, key, &out);
		return clear();
	}

	bool bytes(const char *key, const unsigned char *&at, std::size_t &n) {
		UsefulBufC b{nullptr, 0};
		QCBORDecode_GetByteStringInMapSZ(&ctx_, key, &b);
		if (!clear()) return false;
		at = static_cast<const unsigned char *>(b.ptr);
		n = b.len;
		return true;
	}

private:
	// QCBOR decodes lazily: entering a map succeeds on a truncated buffer, and the shortfall
	// surfaces only when a read runs off the end. That is right for a decoder walking a file
	// and wrong for one reading a message, where truncated and complete need different
	// answers -- a short message is a broken sender, a missing field is an older one.
	//
	// So the whole message is walked once before anything is read from it. The traversal is
	// QCBOR's own, on a throwaway context, and `Finish` is what reports both a malformed item
	// and trailing bytes past the end.
	static bool well_formed(const void *data, std::size_t n) {
		QCBORDecodeContext c{};
		QCBORDecode_Init(&c, UsefulBufC{data, n}, QCBOR_DECODE_MODE_NORMAL);
		QCBORItem item;
		while (QCBORDecode_GetNext(&c, &item) == QCBOR_SUCCESS) {
		}
		return QCBORDecode_Finish(&c) == QCBOR_SUCCESS;
	}

	// A failed lookup leaves the error set, and QCBOR then refuses every later call so one
	// missing field would silently fail the rest. Reading a field is asking a question, and a
	// no is an answer, so the error is taken and cleared here.
	bool clear() {
		const QCBORError e = QCBORDecode_GetAndResetError(&ctx_);
		return e == QCBOR_SUCCESS;
	}

	QCBORDecodeContext ctx_{};
	bool ok_ = false;
};

}  // namespace weft::cbor

#endif
