#include "json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace goradio {
namespace {

const int kMaxDepth = 64;

void AppendUtf8(std::string *out, unsigned int cp) {
	if (cp < 0x80) {
		out->push_back(static_cast<char>(cp));
	} else if (cp < 0x800) {
		out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
		out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	} else if (cp < 0x10000) {
		out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
		out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	} else {
		out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
		out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
		out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	}
}

} // namespace

class JsonParser {
public:
	JsonParser(const std::string &text) : s_(text), pos_(0) {}

	bool ParseDocument(JsonValue *out) {
		SkipWhitespace();
		if (!ParseValue(out, 0)) {
			return false;
		}
		SkipWhitespace();
		if (pos_ != s_.size()) {
			return Fail("trailing data after JSON value");
		}
		return true;
	}

	const std::string &error() const { return err_; }

private:
	bool Fail(const char *msg) {
		if (err_.empty()) {
			char buf[64];
			std::snprintf(buf, sizeof(buf), " at offset %lu", static_cast<unsigned long>(pos_));
			err_ = std::string(msg) + buf;
		}
		return false;
	}

	void SkipWhitespace() {
		while (pos_ < s_.size()) {
			char c = s_[pos_];
			if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
				++pos_;
			} else {
				break;
			}
		}
	}

	bool Literal(const char *lit) {
		size_t n = std::strlen(lit);
		if (s_.compare(pos_, n, lit) != 0) {
			return false;
		}
		pos_ += n;
		return true;
	}

	bool ParseValue(JsonValue *out, int depth) {
		if (depth > kMaxDepth) {
			return Fail("nesting too deep");
		}
		if (pos_ >= s_.size()) {
			return Fail("unexpected end of input");
		}
		char c = s_[pos_];
		switch (c) {
			case '{': return ParseObject(out, depth);
			case '[': return ParseArray(out, depth);
			case '"': {
				out->type_ = JsonValue::kString;
				return ParseString(&out->str_);
			}
			case 't':
				if (!Literal("true")) return Fail("invalid literal");
				out->type_ = JsonValue::kBool;
				out->bool_ = true;
				return true;
			case 'f':
				if (!Literal("false")) return Fail("invalid literal");
				out->type_ = JsonValue::kBool;
				out->bool_ = false;
				return true;
			case 'n':
				if (!Literal("null")) return Fail("invalid literal");
				out->type_ = JsonValue::kNull;
				return true;
			default:
				return ParseNumber(out);
		}
	}

	bool ParseObject(JsonValue *out, int depth) {
		++pos_; // '{'
		out->type_ = JsonValue::kObject;
		SkipWhitespace();
		if (pos_ < s_.size() && s_[pos_] == '}') {
			++pos_;
			return true;
		}
		for (;;) {
			SkipWhitespace();
			if (pos_ >= s_.size() || s_[pos_] != '"') {
				return Fail("expected object key");
			}
			std::string key;
			if (!ParseString(&key)) {
				return false;
			}
			SkipWhitespace();
			if (pos_ >= s_.size() || s_[pos_] != ':') {
				return Fail("expected ':'");
			}
			++pos_;
			SkipWhitespace();
			out->members_.push_back(std::make_pair(key, JsonValue()));
			if (!ParseValue(&out->members_.back().second, depth + 1)) {
				return false;
			}
			SkipWhitespace();
			if (pos_ >= s_.size()) {
				return Fail("unterminated object");
			}
			if (s_[pos_] == ',') {
				++pos_;
				continue;
			}
			if (s_[pos_] == '}') {
				++pos_;
				return true;
			}
			return Fail("expected ',' or '}'");
		}
	}

	bool ParseArray(JsonValue *out, int depth) {
		++pos_; // '['
		out->type_ = JsonValue::kArray;
		SkipWhitespace();
		if (pos_ < s_.size() && s_[pos_] == ']') {
			++pos_;
			return true;
		}
		for (;;) {
			SkipWhitespace();
			out->elems_.push_back(JsonValue());
			if (!ParseValue(&out->elems_.back(), depth + 1)) {
				return false;
			}
			SkipWhitespace();
			if (pos_ >= s_.size()) {
				return Fail("unterminated array");
			}
			if (s_[pos_] == ',') {
				++pos_;
				continue;
			}
			if (s_[pos_] == ']') {
				++pos_;
				return true;
			}
			return Fail("expected ',' or ']'");
		}
	}

	bool ParseHex4(unsigned int *out) {
		if (pos_ + 4 > s_.size()) {
			return false;
		}
		unsigned int v = 0;
		for (int i = 0; i < 4; ++i) {
			char c = s_[pos_ + i];
			v <<= 4;
			if (c >= '0' && c <= '9') {
				v |= static_cast<unsigned int>(c - '0');
			} else if (c >= 'a' && c <= 'f') {
				v |= static_cast<unsigned int>(c - 'a' + 10);
			} else if (c >= 'A' && c <= 'F') {
				v |= static_cast<unsigned int>(c - 'A' + 10);
			} else {
				return false;
			}
		}
		pos_ += 4;
		*out = v;
		return true;
	}

	bool ParseString(std::string *out) {
		++pos_; // opening quote
		out->clear();
		while (pos_ < s_.size()) {
			char c = s_[pos_++];
			if (c == '"') {
				return true;
			}
			if (c != '\\') {
				out->push_back(c);
				continue;
			}
			if (pos_ >= s_.size()) {
				break;
			}
			char esc = s_[pos_++];
			switch (esc) {
				case '"': out->push_back('"'); break;
				case '\\': out->push_back('\\'); break;
				case '/': out->push_back('/'); break;
				case 'b': out->push_back('\b'); break;
				case 'f': out->push_back('\f'); break;
				case 'n': out->push_back('\n'); break;
				case 'r': out->push_back('\r'); break;
				case 't': out->push_back('\t'); break;
				case 'u': {
					unsigned int cp = 0;
					if (!ParseHex4(&cp)) {
						return Fail("bad \\u escape");
					}
					// A code point above the BMP arrives as a surrogate
					// pair; recombine it rather than emitting two
					// unpaired halves as UTF-8.
					if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 1 < s_.size() &&
					    s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
						size_t save = pos_;
						pos_ += 2;
						unsigned int low = 0;
						if (ParseHex4(&low) && low >= 0xDC00 && low <= 0xDFFF) {
							cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
						} else {
							pos_ = save;
						}
					}
					AppendUtf8(out, cp);
					break;
				}
				default:
					return Fail("unknown escape");
			}
		}
		return Fail("unterminated string");
	}

	bool ParseNumber(JsonValue *out) {
		size_t start = pos_;
		if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) {
			++pos_;
		}
		bool any = false;
		while (pos_ < s_.size()) {
			char c = s_[pos_];
			if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
				any = true;
				++pos_;
			} else {
				break;
			}
		}
		if (!any) {
			return Fail("invalid number");
		}
		std::string text = s_.substr(start, pos_ - start);
		out->type_ = JsonValue::kNumber;
		out->num_ = std::strtod(text.c_str(), 0);
		out->str_ = text;
		return true;
	}

	const std::string &s_;
	size_t pos_;
	std::string err_;
};

bool JsonValue::Parse(const std::string &text, JsonValue *out, std::string *err) {
	*out = JsonValue();
	JsonParser parser(text);
	if (!parser.ParseDocument(out)) {
		if (err != 0) {
			*err = parser.error();
		}
		*out = JsonValue();
		return false;
	}
	return true;
}

const JsonValue &JsonValue::Null() {
	static const JsonValue null_value;
	return null_value;
}

std::string JsonValue::AsString() const {
	if (type_ == kString || type_ == kNumber) {
		return str_;
	}
	if (type_ == kBool) {
		return bool_ ? "true" : "false";
	}
	return std::string();
}

long long JsonValue::AsInt() const {
	// int64 fields come back quoted; int32 fields come back bare. Both
	// land here, so accept either rather than making callers care.
	if (type_ == kString || type_ == kNumber) {
		return static_cast<long long>(std::strtoll(str_.c_str(), 0, 10));
	}
	if (type_ == kBool) {
		return bool_ ? 1 : 0;
	}
	return 0;
}

double JsonValue::AsDouble() const {
	if (type_ == kNumber) {
		return num_;
	}
	if (type_ == kString) {
		return std::strtod(str_.c_str(), 0);
	}
	return 0.0;
}

bool JsonValue::AsBool() const {
	if (type_ == kBool) {
		return bool_;
	}
	if (type_ == kNumber) {
		return num_ != 0.0;
	}
	if (type_ == kString) {
		return str_ == "true";
	}
	return false;
}

const JsonValue &JsonValue::Get(const std::string &key) const {
	for (size_t i = 0; i < members_.size(); ++i) {
		if (members_[i].first == key) {
			return members_[i].second;
		}
	}
	return Null();
}

bool JsonValue::Has(const std::string &key) const {
	for (size_t i = 0; i < members_.size(); ++i) {
		if (members_[i].first == key) {
			return true;
		}
	}
	return false;
}

std::string JsonValue::Str(const std::string &key, const std::string &def) const {
	const JsonValue &v = Get(key);
	return v.IsNull() ? def : v.AsString();
}

long long JsonValue::Int(const std::string &key, long long def) const {
	const JsonValue &v = Get(key);
	return v.IsNull() ? def : v.AsInt();
}

bool JsonValue::Bool(const std::string &key, bool def) const {
	const JsonValue &v = Get(key);
	return v.IsNull() ? def : v.AsBool();
}

size_t JsonValue::Size() const { return elems_.size(); }

const JsonValue &JsonValue::At(size_t index) const {
	if (index >= elems_.size()) {
		return Null();
	}
	return elems_[index];
}

std::string JsonEscape(const std::string &in) {
	std::string out;
	out.reserve(in.size() + 8);
	for (size_t i = 0; i < in.size(); ++i) {
		unsigned char c = static_cast<unsigned char>(in[i]);
		switch (c) {
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\b': out += "\\b"; break;
			case '\f': out += "\\f"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (c < 0x20) {
					char buf[8];
					std::snprintf(buf, sizeof(buf), "\\u%04x", c);
					out += buf;
				} else {
					// Bytes >= 0x80 pass through untouched: PAWN strings
					// carrying UTF-8 stay valid UTF-8, and anything else
					// is the caller's encoding problem, not ours to
					// mangle.
					out.push_back(static_cast<char>(c));
				}
		}
	}
	return out;
}

JsonObject &JsonObject::Str(const char *key, const std::string &value) {
	if (!body_.empty()) {
		body_ += ',';
	}
	body_ += '"';
	body_ += key;
	body_ += "\":\"";
	body_ += JsonEscape(value);
	body_ += '"';
	return *this;
}

JsonObject &JsonObject::StrIfSet(const char *key, const std::string &value) {
	if (value.empty()) {
		return *this;
	}
	return Str(key, value);
}

JsonObject &JsonObject::Int(const char *key, long long value) {
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%lld", value);
	if (!body_.empty()) {
		body_ += ',';
	}
	body_ += '"';
	body_ += key;
	body_ += "\":";
	body_ += buf;
	return *this;
}

JsonObject &JsonObject::Bool(const char *key, bool value) {
	if (!body_.empty()) {
		body_ += ',';
	}
	body_ += '"';
	body_ += key;
	body_ += "\":";
	body_ += value ? "true" : "false";
	return *this;
}

JsonObject &JsonObject::Raw(const char *key, const std::string &raw_json) {
	if (!body_.empty()) {
		body_ += ',';
	}
	body_ += '"';
	body_ += key;
	body_ += "\":";
	body_ += raw_json;
	return *this;
}

std::string JsonObject::Build() const {
	return "{" + body_ + "}";
}

} // namespace goradio
