// models.hpp - discover switchable models under the models directory (Task J).
// A "model" is a folder directly under root that contains a server.args file; the bot
// lists those by folder name and records the chosen one in <root>/.current_model. It
// never launches llama-server itself (scripts/run-model.sh does).
#pragma once
#include <string>
#include <vector>

// Folder names (not paths), alphabetically sorted, that contain a server.args file.
// Non-directories, loose files (e.g. stray .gguf), hidden entries (.cache, .current_model)
// and folders without server.args are skipped. At most max_count are returned.
std::vector<std::string> list_models(const std::string& root, size_t max_count = 100);

// Absolute path of the file holding the currently selected model name.
std::string current_model_path(const std::string& root);

// Contents of <root>/.current_model, trimmed; empty if the file is missing/unreadable.
std::string read_current_model(const std::string& root);

// Write name to <root>/.current_model (single line). True on success.
bool write_current_model(const std::string& root, const std::string& name);
