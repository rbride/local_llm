# tgbot v2

Telegram bot for a local model on llama-server. It streams replies live and can use tools: web search, reading links, Wikipedia, weather, a calculator, dice, random picks, the time, and per-chat memory.

## Build

```
sudo apt install build-essential libcurl4-openssl-dev nlohmann-json3-dev pkg-config
make
make test      # offline self-tests, optional
```

## Run

1. Start llama-server with `--jinja`, which tool calling needs:
   ```
   ./build/bin/llama-server -m model.gguf -ngl 99 -c 32768 -np 1 --jinja --host 127.0.0.1 --port 1234
   ```
   Use a model that is good at tool calling; Qwen models are. Roleplay fine-tunes often ignore tools or call them wrong.
2. Create your config and start the bot:
   ```
   cp bot.env.example bot.env
   nano bot.env
   ./tgbot
   ```
   If you already have a `bot.env` from the old version, it still works as is; the new settings all have defaults.

## Commands

| Command | What it does |
|---|---|
| `/stop` | Stop the reply that's being generated. The stop button on a live draft does the same. |
| `/retry` | Regenerate the last answer. |
| `/reset` | Forget the conversation. Memories and persona are kept. |
| `/persona <text>` | Set the bot's personality for this chat. `/persona reset` undoes it. |
| `/think on\|off` | Toggle the thinking step for this chat. |
| `/memories`, `/forget <n>`, `/forget all` | See and delete what the bot saved with its memory tool. |
| `/tools`, `/stats`, `/id` | Info. |

In groups, @mention the bot or reply to one of its messages.

## Streaming

- **Private chats** use Telegram's live drafts (`sendMessageDraft`). The reply types itself out, with a stop button.
- **Groups** get a message that's edited every few seconds, because Telegram only allows drafts in private chats.

If Telegram refuses drafts for your bot, the bot logs it once and switches to message edits automatically. You can also try enabling Threaded mode in @BotFather.

## Web search

Search works out of the box through DuckDuckGo, but DuckDuckGo may start blocking you if you search a lot. There are two more reliable options:

- **SearXNG** is free and self-hosted:
  ```
  docker run -d -p 8888:8080 --name searxng searxng/searxng
  ```
  Then enable JSON output: in the container's `settings.yml`, add `json` under `search: formats:`, and restart the container. Set `SEARXNG_URL=http://127.0.0.1:8888` in `bot.env`.
- **Brave Search API** has a free tier. Set `BRAVE_API_KEY=` in `bot.env`.

## Safety design

- **The tools can't touch your PC.** No tool reads or writes your files or runs commands. The only thing written to disk is `state.json`, which holds personas and notes.
- **`fetch_url` only reaches the public internet.** Before connecting, it resolves the address, refuses localhost, LAN and link-local addresses, and pins the checked IP, so a later DNS lookup can't be swapped for a private one. It checks every redirect the same way. So nobody can make the bot poke your router or your own llama-server.
- **Faked conversation turns are stripped.** Chat-template control tokens like `<|im_start|>`, `<|eot_id|>` and `<tool_call>` are removed from messages, names and web content. That stops the faked-turn trick you tested.
- **Everything is capped:** tool rounds per message, the size of each tool result, the number of saved notes, and messages per user per minute.
- **Web content is labelled as untrusted.** The model is told it's data, not instructions. That makes it harder for a web page to hijack the bot, but it can't make it impossible.

## Files

| File | What's in it |
|---|---|
| `src/main.cpp` | Update loop, commands, job queue, agent/tool loop |
| `src/llm.cpp` | Streaming client for the model server |
| `src/live.cpp` | Live drafts / edit streaming with Telegram rate limits |
| `src/tools.cpp` | All tools, the calculator parser, URL safety checks |
| `src/telegram.cpp` | Bot API calls, retries on 429, HTML with plain-text fallback |
| `src/markdown.cpp` | Model Markdown to Telegram HTML |
| `src/store.cpp` | Saved per-chat state |
| `src/config.cpp` | `bot.env` / environment settings |
| `src/util.cpp` | String helpers, HTML to text, sanitizer |
