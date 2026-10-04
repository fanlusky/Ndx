#pragma once

// Startup options read from %LOCALAPPDATA%\Ndx\config.toml. Each option is turned into the
// equivalent command-line argument, e.g. position = "center" into --position=center. They
// come before the actual arguments, so those take precedence.
struct ConfigArgs {
	wchar_t **args;
	int count;
};

// A missing file is not an error. Other problems are reported in a message box,
// the valid options still apply.
void ConfigLoad(ConfigArgs *config);
void ConfigFree(ConfigArgs *config);
