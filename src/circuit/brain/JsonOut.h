/*
 * JsonOut.h
 *
 * Minimal streaming JSON writer for brain snapshots. Output is always a
 * single line, so it can be framed by '\n'.
 */

#ifndef SRC_CIRCUIT_BRAIN_JSONOUT_H_
#define SRC_CIRCUIT_BRAIN_JSONOUT_H_

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace circuit {

class CJsonOut final {
public:
	CJsonOut() { buf.reserve(4096); }

	CJsonOut& BeginObj() { Sep(); buf += '{'; first.push_back(true); return *this; }
	CJsonOut& EndObj() { buf += '}'; first.pop_back(); return *this; }
	CJsonOut& BeginArr() { Sep(); buf += '['; first.push_back(true); return *this; }
	CJsonOut& EndArr() { buf += ']'; first.pop_back(); return *this; }

	CJsonOut& Key(const char* k) {
		Sep();
		Quote(k);
		buf += ':';
		afterKey = true;
		return *this;
	}

	CJsonOut& Val(int v) { Sep(); buf += std::to_string(v); return *this; }
	CJsonOut& Val(unsigned v) { Sep(); buf += std::to_string(v); return *this; }
	CJsonOut& Val(int64_t v) { Sep(); buf += std::to_string(v); return *this; }
	CJsonOut& Val(uint64_t v) { Sep(); buf += std::to_string(v); return *this; }
	CJsonOut& Val(bool v) { Sep(); buf += v ? "true" : "false"; return *this; }
	CJsonOut& Val(const char* v) { Sep(); Quote(v); return *this; }
	CJsonOut& Val(const std::string& v) { Sep(); Quote(v.c_str()); return *this; }
	CJsonOut& Val(float v) { return Val(static_cast<double>(v)); }
	CJsonOut& Val(double v) {
		Sep();
		if (!std::isfinite(v)) {  // JSON has no NaN/Inf
			buf += "null";
			return *this;
		}
		char tmp[32];
		std::snprintf(tmp, sizeof(tmp), "%.6g", v);
		buf += tmp;
		return *this;
	}
	CJsonOut& Null() { Sep(); buf += "null"; return *this; }
	// Pre-serialised JSON value
	CJsonOut& Raw(const std::string& json) { Sep(); buf += json; return *this; }

	template<typename T> CJsonOut& Field(const char* k, T v) { Key(k); return Val(v); }

	// Little-endian float32 array as base64, decoded client-side into a Float32Array
	CJsonOut& Floats(const float* data, size_t count) {
		Sep();
		buf += '"';
		Base64(reinterpret_cast<const uint8_t*>(data), count * sizeof(float));
		buf += '"';
		return *this;
	}

	const std::string& Str() const { return buf; }
	std::string&& Take() { return std::move(buf); }

private:
	void Sep() {
		if (afterKey) {
			afterKey = false;
			return;
		}
		if (first.empty()) {
			return;
		}
		if (first.back()) {
			first.back() = false;
		} else {
			buf += ',';
		}
	}

	void Quote(const char* s) {
		buf += '"';
		for (; *s != '\0'; ++s) {
			const unsigned char c = static_cast<unsigned char>(*s);
			switch (c) {
				case '"': buf += "\\\""; break;
				case '\\': buf += "\\\\"; break;
				case '\n': buf += "\\n"; break;
				case '\r': buf += "\\r"; break;
				case '\t': buf += "\\t"; break;
				default:
					if (c < 0x20) {
						char tmp[8];
						std::snprintf(tmp, sizeof(tmp), "\\u%04x", c);
						buf += tmp;
					} else {
						buf += static_cast<char>(c);
					}
			}
		}
		buf += '"';
	}

	void Base64(const uint8_t* data, size_t len) {
		static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		buf.reserve(buf.size() + (len + 2) / 3 * 4);
		size_t i = 0;
		for (; i + 2 < len; i += 3) {
			const uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
			buf += tbl[(n >> 18) & 63];
			buf += tbl[(n >> 12) & 63];
			buf += tbl[(n >> 6) & 63];
			buf += tbl[n & 63];
		}
		if (i < len) {
			uint32_t n = data[i] << 16;
			if (i + 1 < len) {
				n |= data[i + 1] << 8;
			}
			buf += tbl[(n >> 18) & 63];
			buf += tbl[(n >> 12) & 63];
			buf += (i + 1 < len) ? tbl[(n >> 6) & 63] : '=';
			buf += '=';
		}
	}

	std::string buf;
	std::vector<bool> first;
	bool afterKey = false;
};

} // namespace circuit

#endif // SRC_CIRCUIT_BRAIN_JSONOUT_H_
