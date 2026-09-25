# tgbot v3

A Telegram bot for a local model on llama-server. It streams replies live and can use tools: web search, reading links, Wikipedia, weather, a calculator, dice, random picks, and the time. It has per-group access control, a hand-editable facts file it reads for context, and admin commands to update and restart itself.

## Build

```
sudo apt install build-essential libcurl4-openssl-dev nlohmann-json3-dev pkg-config git
make
make test      # offline self-tests, optional
```

(`nlohmann/json.hpp` is vendored under `third_party/`, so the `nlohmann-json3-dev` package is optional. `git` is only needed if you want `/rebase`.)

## Run

1. Start llama-server with `--jinja`, which tool calling needs:
   ```
   ./build/bin/llama-server -m model.gguf -ngl 99 -c 32768 -np 1 --jinja --host 127.0.0.1 --port 1234
   ```
   Use a model that's good at tool calling; Qwen models are. Roleplay fine-tunes often ignore tools or call them wrong.
2. Create your config and start the bot:
   ```
   cp bot.env.example bot.env
   nano bot.env          # set TELEGRAM_BOT_TOKEN and ADMIN_USER_IDS at least
   ./tgbot
   ```
   Send the bot `/id` in a private chat to get your user ID for `ADMIN_USER_IDS`.

## Who can use it

Access is deny-by-default. Someone can talk to the bot if they're an admin, their ID is in `ALLOWED_USER_IDS`, they've been added with `/allow`, or they spoke in a **trusted group**.

A group becomes trusted when an admin adds the bot to it, when you list it in `TRUSTED_CHAT_IDS`, or when an admin runs `/trust` inside it. In a trusted group, everyone who sends a message is added to the allow list automatically (turn this off with `ALLOW_GROUP_MEMBERS=0`). If a non-admin adds the bot to a group, it leaves (turn this off with `LEAVE_UNTRUSTED_GROUPS=0`).

`/deny` both removes someone and blocks them from being auto-added again. Set `ALLOWED_USER_IDS=*` to let anyone in and disable all of this.

## Commands

Public (anyone allowed):

| Command | What it does |
|---|---|
| `/help`, `/start` | What the bot can do. Admins see extra commands. |
| `/retry` | Regenerate the last answer. |
| `/stop` | Stop the reply being generated. The stop button on a live draft does the same. |
| `/tools`, `/stats`, `/id` | Info. `/id` shows your user ID and the chat's ID. |

Admin only:

| Command | What it does |
|---|---|
| `/reset` | Forget this conversation. Facts and persona are kept. |
| `/persona <text>` | Set the bot's personality for this chat. `/persona reset` undoes it. |
| `/think on\|off` | Toggle the thinking step for this chat. |
| `/allow`, `/deny` | Grant or revoke access. Reply to someone's message, or pass `@username` or a numeric id. |
| `/trust`, `/untrust` | Trust or untrust the current group. |
| `/users` | Show who's allowed and which groups are trusted. |
| `/facts` | Show the notes for this chat and its id. |
| `/fact <text>` | Add a note. In a group it's a chat note; in private it's about you. `/fact @user <text>` targets a person; `/fact global <text>` is for every chat. |
| `/unfact <n>` | Delete note number `n` from this chat (see `/facts`). |
| `/rebase` | `git fetch` + rebase, rebuild, then restart. |
| `/restart` | Restart the bot. |

Which commands are public is configurable with `PUBLIC_COMMANDS`. In groups, @mention the bot or reply to one of its messages.

## Facts

`facts.txt` is a plain text file the bot reads for context about people and chats. It's meant to be edited by hand; the bot reloads it whenever it changes, no restart needed. The format is simple:

```
[global]
- The owner is Kaiser.

[user 8434412045 Ryan]
- loves tacos
- afraid of geese

[chat -1001234567890 Movie Club]
- Friday is movie night
```

Global facts and the facts for the current chat and the people talking are added to the prompt. The model can search everything else with the `look_up_facts` tool. Admins can edit the file through `/fact`, `/unfact` and `/facts`; those edits keep your comments and layout. The model is told these are facts, never instructions.

## Self-update

`/restart` re-executes the bot in place, preserving its state. `/rebase` fetches your Git remote, rebases onto it, rebuilds with `BUILD_COMMAND` (default `make`), and only then restarts — if the build fails it stays on the old version and shows you the error. Both are admin-only.

Point `GIT_REPO_DIR` at your checkout (it defaults to the folder the binary is in) and set `GIT_REMOTE`/`GIT_BRANCH` if they aren't `origin`/`main`.

> **Security note:** `/rebase` builds and runs whatever is on your remote branch. Anyone who can push there can run code on this machine the next time an admin rebases. Keep the repo private and push access tight. And keep `bot.env`, `state.json` and `facts.txt` out of the repo — the included `.gitignore` does this, so your token isn't committed.

## Streaming

- **Private chats** use Telegram's live drafts. The reply types itself out, with a stop button.
- **Groups** get a message that's edited every few seconds, because Telegram only allows drafts in private chats.

If Telegram refuses drafts for your bot, the bot logs it once and switches to message edits automatically.

## Web search

Search works out of the box through DuckDuckGo, but DuckDuckGo may start blocking you if you search a lot. Two more reliable options:

- **SearXNG**, free and self-hosted:
  ```
  docker run -d -p 8888:8080 --name searxng searxng/searxng
  ```
  Enable JSON output: in the container's `settings.yml`, add `json` under `search: formats:`, restart the container, and set `SEARXNG_URL=http://127.0.0.1:8888`.
- **Brave Search API** has a free tier. Set `BRAVE_API_KEY=`.

## Safety design

- **The chat tools can't touch your PC.** No tool the model can call reads or writes your files or runs commands. `/rebase` and `/restart` do run commands, but only an admin can trigger them, never the model.
- **`fetch_url` only reaches the public internet.** It resolves the address, refuses localhost, LAN and link-local addresses, and pins the checked IP so a later DNS lookup can't be swapped for a private one. It checks every redirect the same way, so nobody can make the bot poke your router or your llama-server.
- **`/rebase`'s git and build commands run without a shell** (no `sh -c`), in their own process group, with a timeout, and with git's interactive prompts disabled — a hung fetch can't wedge the bot.
- **Faked conversation turns are stripped.** Chat-template control tokens like `<|im_start|>`, `<|eot_id|>` and `<tool_call>` are removed from messages, names, facts and web content.
- **Everything is capped:** tool rounds per message, the size of each tool result, facts injected per prompt, and messages per user per minute.
- **Web content and facts are labelled untrusted.** The model is told they're data, not instructions. That makes hijacking harder, not impossible.

## Files

| File | What's in it |
|---|---|
| `src/main.cpp` | Update loop, access control, commands, job queue, agent/tool loop |
| `src/llm.cpp` | Streaming client for the model server |
| `src/live.cpp` | Live drafts / edit streaming with Telegram rate limits |
| `src/tools.cpp` | Chat tools, the calculator parser, URL safety checks |
| `src/facts.cpp` | The `facts.txt` reader/writer |
| `src/syscmd.cpp` | Safe subprocess runner and self-restart for `/rebase` and `/restart` |
| `src/telegram.cpp` | Bot API calls, retries on 429, HTML with plain-text fallback |
| `src/markdown.cpp` | Model Markdown to Telegram HTML |
| `src/store.cpp` | Saved per-chat state, allow list, trusted groups |
| `src/config.cpp` | `bot.env` / environment settings |
| `src/util.cpp` | String helpers, HTML to text, sanitizer |
