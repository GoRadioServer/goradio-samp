#ifndef GORADIO_JSON_H
#define GORADIO_JSON_H

#include <string>
#include <utility>
#include <vector>

namespace goradio {

// A small recursive-descent JSON reader/writer, sized for protobuf's JSON
// mapping rather than for general-purpose use. Three quirks of that
// mapping are baked into the accessors below, because every one of them
// has burned a hand-written client before (see the audio server's
// "HTTP + JSON API" docs):
//
//   * 64-bit integers arrive as quoted strings, 32-bit ones as numbers --
//     Int() accepts either.
//   * Fields at their default value are omitted entirely -- every
//     accessor takes a default and never treats "absent" as an error.
//   * Field names are lowerCamelCase on the wire, not the .proto's
//     snake_case.
class JsonValue {
public:
	enum Type { kNull, kBool, kNumber, kString, kArray, kObject };

	JsonValue() : type_(kNull), bool_(false), num_(0) {}

	static bool Parse(const std::string &text, JsonValue *out, std::string *err);

	Type type() const { return type_; }
	bool IsNull() const { return type_ == kNull; }
	bool IsObject() const { return type_ == kObject; }
	bool IsArray() const { return type_ == kArray; }

	// Scalar readers for the value itself.
	std::string AsString() const;
	long long AsInt() const;
	double AsDouble() const;
	bool AsBool() const;

	// Object member lookup. Returns null (never 0) for a missing key, so
	// chains like value.Get("source").Get("location").AsString() are safe
	// on a response that omitted the whole sub-message.
	const JsonValue &Get(const std::string &key) const;
	bool Has(const std::string &key) const;

	std::string Str(const std::string &key, const std::string &def = std::string()) const;
	long long Int(const std::string &key, long long def = 0) const;
	bool Bool(const std::string &key, bool def = false) const;

	// Array access. Size() is 0 for a non-array, and At() past the end
	// returns null, so a missing "queue" behaves like an empty one.
	size_t Size() const;
	const JsonValue &At(size_t index) const;

	const std::vector<std::pair<std::string, JsonValue> > &Members() const { return members_; }

private:
	static const JsonValue &Null();

	Type type_;
	bool bool_;
	double num_;
	std::string str_;
	std::vector<JsonValue> elems_;
	std::vector<std::pair<std::string, JsonValue> > members_;

	friend class JsonParser;
};

std::string JsonEscape(const std::string &in);

// Builder for request bodies. Values are appended in call order; nothing
// is validated, so Raw() is how nested objects and maps get in.
class JsonObject {
public:
	JsonObject &Str(const char *key, const std::string &value);
	// Skips the field entirely when value is empty -- proto3 treats an
	// absent string and an empty one identically, and it keeps request
	// bodies readable in debug logs.
	JsonObject &StrIfSet(const char *key, const std::string &value);
	JsonObject &Int(const char *key, long long value);
	JsonObject &Bool(const char *key, bool value);
	JsonObject &Raw(const char *key, const std::string &raw_json);

	bool empty() const { return body_.empty(); }
	std::string Build() const;

private:
	std::string body_;
};

} // namespace goradio

#endif // GORADIO_JSON_H
