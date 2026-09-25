// markdown.hpp - convert the Markdown models like to write into Telegram's HTML subset
#pragma once
#include <string>

// Supports: ```code blocks```, `inline code`, **bold**, __bold__, *italic*, _italic_,
// ~~strike~~, ||spoiler||, [links](https://...), # headings, bullet lists, > quotes.
// Always produces balanced tags; anything it doesn't understand is escaped as plain text.
std::string markdown_to_html(const std::string& md);
