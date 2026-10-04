#include "config.h"

#include <cstdarg>

// Only the part of TOML the options need is supported: top level key = value lines with
// strings, integers, floats and booleans, and comments. Tables and arrays aren't.

constexpr size_t MAX_CONFIG_FILE_SIZE = MEGABYTES(1);
constexpr size_t MAX_CONFIG_VALUE_LENGTH = 1024;

enum class ConfigValueType : uint8_t {
	String,
	Integer,
	Float,
	Boolean
};

struct ConfigValue {
	ConfigValueType type;
	// UTF-8
	char string[MAX_CONFIG_VALUE_LENGTH];
	int64_t integer;
	double number;
	bool boolean;
};

enum class ConfigOptionKind : uint8_t {
	Flag,
	Position,
	Geometry,
	LinespaceFactor,
	CursorTimeout,
	Path
};

struct ConfigOption {
	const char *key;
	// The command-line argument, the value is appended to the ones ending with =
	const wchar_t *arg;
	ConfigOptionKind kind;
};

constexpr ConfigOption CONFIG_OPTIONS[] = {
	{ "position", L"--position=", ConfigOptionKind::Position },
	{ "geometry", L"--geometry=", ConfigOptionKind::Geometry },
	{ "maximize", L"--maximize", ConfigOptionKind::Flag },
	{ "fullscreen", L"--fullscreen", ConfigOptionKind::Flag },
	{ "disable_ligatures", L"--disable-ligatures", ConfigOptionKind::Flag },
	{ "disable_fullscreen", L"--disable-fullscreen", ConfigOptionKind::Flag },
	{ "linespace_factor", L"--linespace-factor=", ConfigOptionKind::LinespaceFactor },
	{ "cursor_timeout", L"--cursor-timeout=", ConfigOptionKind::CursorTimeout },
	{ "neovim_bin", L"--neovim-bin=", ConfigOptionKind::Path }
};
constexpr int CONFIG_OPTION_COUNT = sizeof(CONFIG_OPTIONS) / sizeof(CONFIG_OPTIONS[0]);

struct ConfigErrors {
	char text[4096];
	size_t length;
};

void ConfigAddError(ConfigErrors *errors, int line, const char *format, ...) {
	const auto Append = [&](int written) {
		if (written > 0) {
			errors->length = min(errors->length + written, sizeof(errors->text) - 1);
		}
	};
	// Line 0 is about the whole file
	if (line > 0) {
		Append(snprintf(errors->text + errors->length, sizeof(errors->text) - errors->length, "Line %d: ", line));
	}

	va_list args;
	va_start(args, format);
	Append(vsnprintf(errors->text + errors->length, sizeof(errors->text) - errors->length, format, args));
	va_end(args);

	Append(snprintf(errors->text + errors->length, sizeof(errors->text) - errors->length, "\n"));
}

const char *ConfigSkipWhitespace(const char *p, const char *end) {
	while (p < end && (*p == ' ' || *p == '\t')) ++p;
	return p;
}

bool ConfigIsBareKeyChar(char c) {
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

bool ConfigAppendUtf8(char *out, size_t out_size, size_t *length, uint32_t codepoint) {
	char bytes[4];
	int count;
	if (codepoint < 0x80) {
		bytes[0] = static_cast<char>(codepoint);
		count = 1;
	}
	else if (codepoint < 0x800) {
		bytes[0] = static_cast<char>(0xC0 | (codepoint >> 6));
		bytes[1] = static_cast<char>(0x80 | (codepoint & 0x3F));
		count = 2;
	}
	else if (codepoint < 0x10000) {
		bytes[0] = static_cast<char>(0xE0 | (codepoint >> 12));
		bytes[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
		bytes[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
		count = 3;
	}
	else {
		bytes[0] = static_cast<char>(0xF0 | (codepoint >> 18));
		bytes[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
		bytes[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
		bytes[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
		count = 4;
	}
	if (*length + count >= out_size) return false;
	memcpy(out + *length, bytes, count);
	*length += count;
	return true;
}

// A basic "..." string with escapes or a literal '...' string, p is at the opening quote
bool ConfigParseString(const char **p, const char *end, ConfigValue *value, char *error, size_t error_size) {
	char quote = *(*p)++;
	size_t length = 0;
	while (*p < end && **p != quote) {
		uint32_t codepoint = static_cast<uint8_t>(*(*p)++);
		if (quote == '"' && codepoint == '\\') {
			char escape = *p < end ? *(*p)++ : '\0';
			switch (escape) {
			case 'b': codepoint = '\b'; break;
			case 't': codepoint = '\t'; break;
			case 'n': codepoint = '\n'; break;
			case 'f': codepoint = '\f'; break;
			case 'r': codepoint = '\r'; break;
			case '"': codepoint = '"'; break;
			case '\\': codepoint = '\\'; break;
			case 'u':
			case 'U': {
				int digits = escape == 'u' ? 4 : 8;
				codepoint = 0;
				for (int i = 0; i < digits; ++i) {
					char c = *p < end ? *(*p)++ : '\0';
					int digit = c >= '0' && c <= '9' ? c - '0' :
						c >= 'a' && c <= 'f' ? c - 'a' + 10 :
						c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
					if (digit < 0) {
						snprintf(error, error_size, "invalid \\%c escape in a string", escape);
						return false;
					}
					codepoint = codepoint * 16 + digit;
				}
				if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
					snprintf(error, error_size, "invalid unicode escape in a string");
					return false;
				}
				if (!ConfigAppendUtf8(value->string, sizeof(value->string), &length, codepoint)) {
					snprintf(error, error_size, "string is too long");
					return false;
				}
			} continue;
			default: {
				snprintf(error, error_size, "unknown escape sequence in a string, write \\\\ for a backslash");
			} return false;
			}
		}
		// Other bytes are copied as they are, the file is UTF-8
		if (length + 1 >= sizeof(value->string)) {
			snprintf(error, error_size, "string is too long");
			return false;
		}
		value->string[length++] = static_cast<char>(codepoint);
	}
	if (*p >= end) {
		snprintf(error, error_size, "string is missing its closing %c", quote);
		return false;
	}
	++*p;
	value->string[length] = '\0';
	value->type = ConfigValueType::String;
	return true;
}

bool ConfigParseValue(const char **p, const char *end, ConfigValue *value, char *error, size_t error_size) {
	if (*p >= end || **p == '#') {
		snprintf(error, error_size, "expected a value after =");
		return false;
	}
	if (**p == '"' || **p == '\'') {
		return ConfigParseString(p, end, value, error, error_size);
	}
	if (**p == '[' || **p == '{') {
		snprintf(error, error_size, "arrays and inline tables are not supported");
		return false;
	}

	// A boolean or a number, up to the next whitespace or comment
	char token[64];
	size_t length = 0;
	const char *start = *p;
	while (*p < end && **p != ' ' && **p != '\t' && **p != '#') {
		char c = *(*p)++;
		// Digit separators as in 1_000
		if (c == '_') continue;
		if (length + 1 >= sizeof(token)) {
			snprintf(error, error_size, "invalid value");
			return false;
		}
		token[length++] = c;
	}
	token[length] = '\0';
	int token_length = static_cast<int>(*p - start);

	if (!strcmp(token, "true") || !strcmp(token, "false")) {
		value->type = ConfigValueType::Boolean;
		value->boolean = token[0] == 't';
		return true;
	}

	char *number_end = nullptr;
	if (strpbrk(token, ".eE") || strstr(token, "inf") || strstr(token, "nan")) {
		value->type = ConfigValueType::Float;
		value->number = strtod(token, &number_end);
	}
	else {
		value->type = ConfigValueType::Integer;
		value->integer = strtoll(token, &number_end, 10);
		value->number = static_cast<double>(value->integer);
	}
	if (length == 0 || *number_end != '\0') {
		snprintf(error, error_size, "invalid value %.*s, strings need quotes", token_length, start);
		return false;
	}
	return true;
}

// x,y for a position, colsxrows for a geometry
bool ConfigParseIntPair(const char *text, char separator, bool positive, int *a, int *b) {
	const auto ParseInt = [&](const char **p, int *out) {
		char *end;
		if (!positive && **p == '-') {
			*out = static_cast<int>(strtol(*p, &end, 10));
		}
		else if (**p >= '0' && **p <= '9') {
			*out = static_cast<int>(strtol(*p, &end, 10));
		}
		else {
			return false;
		}
		*p = end;
		return !positive || *out > 0;
	};
	const char *p = text;
	return ParseInt(&p, a) && *p++ == separator && ParseInt(&p, b) && *p == '\0';
}

wchar_t *ConfigUtf8ToWide(const char *text) {
	int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
	if (length <= 0) return nullptr;
	wchar_t *wide = static_cast<wchar_t *>(malloc(length * sizeof(wchar_t)));
	MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, length);
	return wide;
}

void ConfigAddArg(ConfigArgs *config, const wchar_t *arg, const wchar_t *value) {
	size_t length = wcslen(arg) + (value ? wcslen(value) : 0) + 1;
	wchar_t *full_arg = static_cast<wchar_t *>(malloc(length * sizeof(wchar_t)));
	wcscpy_s(full_arg, length, arg);
	if (value) {
		wcscat_s(full_arg, length, value);
	}
	config->args[config->count++] = full_arg;
}

void ConfigApplyOption(ConfigArgs *config, ConfigErrors *errors, int line, const ConfigOption *option, const ConfigValue *value) {
	switch (option->kind) {
	case ConfigOptionKind::Flag: {
		if (value->type != ConfigValueType::Boolean) {
			ConfigAddError(errors, line, "%s must be true or false", option->key);
			return;
		}
		if (value->boolean) {
			ConfigAddArg(config, option->arg, nullptr);
		}
	} return;
	case ConfigOptionKind::Position:
	case ConfigOptionKind::Geometry:
	case ConfigOptionKind::Path: {
		if (value->type != ConfigValueType::String) {
			ConfigAddError(errors, line, "%s must be a string in quotes", option->key);
			return;
		}
		int a, b;
		if (option->kind == ConfigOptionKind::Position && strcmp(value->string, "center") &&
			!ConfigParseIntPair(value->string, ',', false, &a, &b)) {
			ConfigAddError(errors, line, "position must be \"center\" or \"<x>,<y>\", e.g. \"500,200\"");
			return;
		}
		if (option->kind == ConfigOptionKind::Geometry && !ConfigParseIntPair(value->string, 'x', true, &a, &b)) {
			ConfigAddError(errors, line, "geometry must be \"<cols>x<rows>\", e.g. \"80x25\"");
			return;
		}
		if (option->kind == ConfigOptionKind::Path && value->string[0] == '\0') {
			ConfigAddError(errors, line, "%s must not be empty", option->key);
			return;
		}
		// In "..." a Windows path like "C:\nvim\bin" turns \n and \b into control characters
		for (const char *c = value->string; *c; ++c) {
			if (static_cast<uint8_t>(*c) < 0x20) {
				ConfigAddError(errors, line, "%s contains a control character, write Windows paths in single "
					"quotes like 'C:\\nvim\\bin\\nvim.exe' or with doubled backslashes", option->key);
				return;
			}
		}
		wchar_t *wide = ConfigUtf8ToWide(value->string);
		if (!wide) {
			ConfigAddError(errors, line, "%s is not valid UTF-8", option->key);
			return;
		}
		ConfigAddArg(config, option->arg, wide);
		free(wide);
	} return;
	case ConfigOptionKind::LinespaceFactor: {
		// Same range as accepted on the command line
		if ((value->type != ConfigValueType::Float && value->type != ConfigValueType::Integer) ||
			!(value->number > 0.0 && value->number < 20.0)) {
			ConfigAddError(errors, line, "linespace_factor must be a number greater than 0 and less than 20");
			return;
		}
		wchar_t number[32];
		swprintf_s(number, L"%g", value->number);
		ConfigAddArg(config, option->arg, number);
	} return;
	case ConfigOptionKind::CursorTimeout: {
		if (value->type != ConfigValueType::Integer || value->integer < 0 || value->integer > INT32_MAX) {
			ConfigAddError(errors, line, "cursor_timeout must be a whole number of milliseconds, 0 turns it off");
			return;
		}
		if (value->integer > 0) {
			wchar_t number[32];
			swprintf_s(number, L"%lld", value->integer);
			ConfigAddArg(config, option->arg, number);
		}
	} return;
	}
}

void ConfigParseLine(ConfigArgs *config, ConfigErrors *errors, bool *seen, int line, const char *p, const char *end) {
	p = ConfigSkipWhitespace(p, end);
	if (p == end || *p == '#') return;
	if (*p == '[') {
		ConfigAddError(errors, line, "tables are not supported, write the options without a [section]");
		return;
	}

	const char *key = p;
	while (p < end && ConfigIsBareKeyChar(*p)) ++p;
	int key_length = static_cast<int>(p - key);
	if (key_length == 0) {
		ConfigAddError(errors, line, "expected an option name");
		return;
	}
	p = ConfigSkipWhitespace(p, end);
	if (p == end || *p != '=') {
		ConfigAddError(errors, line, "expected = after %.*s", key_length, key);
		return;
	}
	p = ConfigSkipWhitespace(p + 1, end);

	ConfigValue value;
	char error[160];
	if (!ConfigParseValue(&p, end, &value, error, sizeof(error))) {
		ConfigAddError(errors, line, "%s", error);
		return;
	}
	p = ConfigSkipWhitespace(p, end);
	if (p < end && *p != '#') {
		ConfigAddError(errors, line, "unexpected text after the value of %.*s", key_length, key);
		return;
	}

	for (int i = 0; i < CONFIG_OPTION_COUNT; ++i) {
		const ConfigOption *option = &CONFIG_OPTIONS[i];
		if (strlen(option->key) != static_cast<size_t>(key_length) || strncmp(option->key, key, key_length)) continue;

		if (seen[i]) {
			ConfigAddError(errors, line, "%s is set more than once", option->key);
			return;
		}
		seen[i] = true;
		ConfigApplyOption(config, errors, line, option, &value);
		return;
	}
	ConfigAddError(errors, line, "unknown option %.*s", key_length, key);
}

void ConfigReportErrors(const wchar_t *path, const ConfigErrors *errors) {
	wchar_t *text = ConfigUtf8ToWide(errors->text);
	size_t length = wcslen(path) + (text ? wcslen(text) : 0) + 128;
	wchar_t *message = static_cast<wchar_t *>(malloc(length * sizeof(wchar_t)));
	swprintf_s(message, length, L"Problems in %s:\n\n%sThe other options are applied.", path, text ? text : L"");
	MessageBoxW(nullptr, message, L"Ndx", MB_OK | MB_ICONWARNING);
	free(message);
	free(text);
}

void ConfigLoad(ConfigArgs *config) {
	config->args = static_cast<wchar_t **>(malloc(CONFIG_OPTION_COUNT * sizeof(wchar_t *)));
	config->count = 0;

	wchar_t path[MAX_PATH];
	DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
	if (length == 0 || length >= MAX_PATH || wcscat_s(path, L"\\Ndx\\config.toml")) return;

	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return;

	ConfigErrors errors {};
	LARGE_INTEGER file_size;
	char *text = nullptr;
	DWORD read = 0;
	if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart > static_cast<LONGLONG>(MAX_CONFIG_FILE_SIZE)) {
		ConfigAddError(&errors, 0, "the file is larger than 1 MB");
	}
	else {
		text = static_cast<char *>(malloc(static_cast<size_t>(file_size.QuadPart) + 1));
		if (!ReadFile(file, text, static_cast<DWORD>(file_size.QuadPart), &read, nullptr)) {
			ConfigAddError(&errors, 0, "the file could not be read");
			read = 0;
		}
	}
	CloseHandle(file);

	if (text) {
		const char *p = text;
		const char *text_end = text + read;
		// Skip the byte order mark some editors write
		if (read >= 3 && !memcmp(p, "\xEF\xBB\xBF", 3)) p += 3;

		bool seen[CONFIG_OPTION_COUNT] {};
		for (int line = 1; p < text_end; ++line) {
			const char *line_end = static_cast<const char *>(memchr(p, '\n', text_end - p));
			if (!line_end) line_end = text_end;
			const char *content_end = line_end > p && line_end[-1] == '\r' ? line_end - 1 : line_end;
			ConfigParseLine(config, &errors, seen, line, p, content_end);
			p = line_end < text_end ? line_end + 1 : text_end;
		}
		free(text);
	}

	if (errors.length > 0) {
		ConfigReportErrors(path, &errors);
	}
}

void ConfigFree(ConfigArgs *config) {
	for (int i = 0; i < config->count; ++i) {
		free(config->args[i]);
	}
	free(config->args);
	config->args = nullptr;
	config->count = 0;
}
