# tgbot v3

A Telegram bot for a local model on llama-server. It streams replies live and can use tools: web search, reading links, Wikipedia, weather, a calculator, dice, random picks, and the time. It has per-group access control, a hand-editable facts file it reads for context, and admin commands to update and restart itself.

## Build

```
sudo apt install build-essential libcurl4-openssl-dev nlohmann-json3-dev pkg-config git
make
make test      # offline self-tests, optional
```

(`nlohmann/json.hpp` is vendored under `third_party/`, so the `nlohmann-json3-dev` package is optional. `git` is only needed if you want `/rebase`.)

To see per-file line coverage from the self-tests, install [gcovr](https://gcovr.com/) and run `make coverage`. It rebuilds the library objects and tests with `-O0 -g --coverage` into a separate `build-cov/` directory (the normal `build/` is untouched), runs the tests, and prints a coverage table.

```
sudo apt install gcovr
make coverage
```

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
| `/forget` | Also drop this chat's notes, persona and remembered names. |
| `/persona <text>` | Set the bot's personality for this chat. `/persona reset` undoes it. |
| `/persona reminder <n>` | Repeat the persona next to every Nth message here (0 = off, 1 = every, up to 50). `/persona reminder default` follows `bot.env` again. |
| `/think on\|off` | Toggle the thinking step for this chat. |
| `/think budget <n>` | Thinking cap for this chat in tokens (0 = no cap; `/think budget` shows it). |
| `/context <n>` | How many past messages this chat keeps (2-500). Not the model's token context window. |
| `/temp <x>` | Sampling temperature for this chat (0-2). |
| `/maxtokens <n>` | Cap on the answer length for this chat (64-16000). Thinking is capped separately with `/think budget`. |
| `/limits` | All effective limits for this chat: history, temperature, answer cap, think budget, thinking. |
| `/model` | Show the current model and the switchable ones; `/model <name>` switches (takes effect when the model server reloads). |
| `/defaults` | Forget this chat's setting overrides (see *Settings layers* below). |
| `/allow`, `/deny` | Grant or revoke access. Reply to someone's message, or pass `@username` or a numeric id. |
| `/trust`, `/untrust` | Trust or untrust the current group. |
| `/users` | Show who's allowed and which groups are trusted. |
| `/facts [global/@user]` | Show notes for this chat, global notes, or a person. |
| `/fact <text>` | Add a note. In a group it's a chat note; `/fact @user <text>` targets a person (scoped to the current chat unless an owner uses `everywhere`); `/fact me <text>` is about you; `/fact global <text>` is for every chat (owner-only). |
| `/unfact [global/@user] <n>` | Delete note number `n` from this chat, global notes (owner-only), or a person. |
| `/alias <name1>, <name2>` | Add nicknames for someone. Reply to their message, or use `/alias @user name1, name2`. |
| `/unalias <name>` | Remove a nickname (owner-protected aliases need the owner). |
| `/aliases [@user]` | List nicknames. |
| `/rebase` | `git fetch` + rebase, rebuild, then restart. |
| `/restart` | Restart the bot. |

### Settings layers

Per-chat settings resolve in three layers, highest first: **this chat's override** → the **global default** → the value in `bot.env`. Run any setting command (`/persona`, `/think`, `/context`, `/temp`, `/maxtokens`) with no argument to see the value in effect and all three layers. An **owner** can set the global layer with a `global` form of the same command — `/persona global <text>`, `/think global …`, `/context global <n>`, `/temp global <x>`, `/maxtokens global <n>` — which covers every chat that has no override of its own.

| Reset command | What it clears |
|---|---|
| `/defaults` | This chat's overrides. |
| `/defaults global` (owner) | The global overrides. |
| `/defaults all CONFIRM` (owner) | The global overrides and every chat's overrides. |

None of them touch facts, aliases, the allow list, trusted groups or conversation history.

### Owner only

An owner is always also an admin and has this command plus the `global` forms above.

| Command | What it does |
|---|---|
| `/wipefacts CONFIRM` | Delete every note in `facts.txt` — global, per-person and per-chat, including owner-protected ones. The file is rewritten to the empty template. |

Owner-protected facts and aliases can't be removed by a regular admin.

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
- **Faked conversation turns are stripped.** Chat-template control tokens like `<|im_start|>`, `<|eot_id|>` and tool-call tags are removed from messages, names, facts and web content.
- **Everything is capped:** tool rounds per message, the size of each tool result, facts injected per prompt, and messages per user per minute.
- **Web content and facts are labelled untrusted.** The model is told they're data, not instructions. That makes hijacking harder, not impossible.

## Logging

The bot logs the interesting events (commands, access grants/denials, model requests with token counts and tok/s, tool calls, restarts, errors) to the console and appends them to a log file. `LOG_FILE` sets the path, default `tgbot.log`; set it empty for console only. Console lines carry the time of day, file lines the full date. Rotation isn't built in: point `logrotate` at the file with `copytruncate`, or just truncate it (`: > tgbot.log`) — the bot opens the file in append mode and flushes every line, so nothing gets lost or overwritten.

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
