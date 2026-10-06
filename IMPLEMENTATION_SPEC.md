# tgbot — implementation spec

You are editing an existing C++17 Telegram bot. Build with `make`, test with `make test`.
The real code is in `src/` (the root `tgbot.cpp` is an OLD single-file version — **ignore it, do not edit it**).
Everything builds from `src/*.cpp` via the Makefile.

Work through the tasks below **one at a time**, in order. After each task:
1. Build with `make`. It must compile clean (no new warnings — the build uses `-Wall -Wextra`).
2. Run `make test`. It must pass.
3. Add or extend a test in `tests/selftest.cpp` for the behaviour you changed, where the thing is testable offline (parsing, permissions, config). UI/Telegram calls can't be unit-tested here; for those, just make sure the build is clean and the logic reads correctly.
4. Commit with a short message naming the task (e.g. `task A: owner-only reset`).

Do NOT refactor unrelated code. Keep each change as small as possible. Preserve the existing code style (the project uses 4-space indent, `snake_case` members with a trailing underscore on class members, and `\xHH` escapes for emoji in strings — match that).

Key files and what they hold:
- `src/config.hpp` / `src/config.cpp` — the `Config` struct and loading from `bot.env`. **Compile-time** settings live here.
- `src/store.hpp` / `src/store.cpp` — `Store`: per-chat state, saved to `state.json`. `ChatSettings` is per-chat. **Runtime-adjustable** settings should live here so they persist and are per-chat where that makes sense.
- `src/main.cpp` — the `Bot` class: command handling (`handle_command`), the update loop (`run`/`handle_update`), building the model request (`build_messages`-style logic around lines 150–300), rebase/restart.
- `src/facts.cpp` — the facts file (`facts.txt`) parser and store.
- `src/util.cpp` — `log()` lives here.
- `tests/selftest.cpp` — offline tests.

There is both an `owner` and `admin` concept being introduced (Task B). Throughout, the gating order is: **owner > admin > allowed user > everyone.** An owner is always also an admin.

---

## Task A — (already done, verify only) `/reset` is admin-only

`/reset` in `src/main.cpp` already sits AFTER the `if (!admin) { say("That's an admin-only command."); return true; }` gate, so non-admins already can't run it. **Do not change the gating.** Just confirm this is true and move on. The real reset problems are Tasks D and F. (If you find `reset` listed in `PUBLIC_COMMANDS` handling anywhere that would bypass the admin gate, remove it — but the gate is in code, not the public list, so there should be nothing to do.)

## Task F — fix the restart loop (do this early; it's the worst bug)

**Symptom:** after `/restart` (or `/rebase`), the bot restarts endlessly, replying "Restarting…" to a message that is often already deleted.

**Root cause:** in the update loop (`src/main.cpp` around line 864), `offset` is advanced in memory as updates are processed, but on `/restart` the process re-execs itself (`restart_self`) and the NEW process starts with `offset = 0`. Telegram then re-delivers every unconfirmed update — including the `/restart` command — so it restarts again, forever.

**Fix:**
1. Persist the update offset so it survives a restart. Add a `long long update_offset = 0;` field to `Store` (saved/loaded in `state.json` via `Store::save`/`Store::load`).
2. In the update loop, after computing `offset = update_id + 1`, also write it to `store_.update_offset` (under `store_.mu`) and save periodically (at least right before any restart).
3. On startup, initialise the loop's `offset` from `store_.update_offset`.
4. **Belt-and-suspenders:** ignore any message whose `date` (Telegram gives each message a Unix timestamp) is older than the time this process started. Record `start_time_ = std::time(nullptr);` when the bot starts and skip command/message handling for anything older. This guarantees a stale command can never re-trigger even if the offset logic fails.
5. Before `restart_self` in `do_rebase_or_restart`, make sure the current offset is saved (it calls `store_.save()` already — just make sure `update_offset` is included in what gets saved).

Add a test in `selftest.cpp` that constructs a fake update stream where an old update is re-delivered, and asserts the "older than start_time" guard rejects it.

---

## Task B — owner tier (super-admin) above admin

Add an **owner** concept. Owners can do everything admins can, plus owner-only actions (Tasks B, I, and owner-protected facts).

1. In `Config` (`config.hpp`/`config.cpp`), add:
   - `std::set<long long> owner_users;` loaded from a new `OWNER_USER_IDS` env var (comma-separated, same parsing as `ADMIN_USER_IDS`).
   - `bool is_owner(long long id) const { return owner_users.count(id) > 0; }`
   - Make owners automatically count as admins: in `is_admin`, return `true` if the id is in `owner_users` OR `admin_users`. (So you don't have to list yourself in both.)
2. In `bot.env.example`, document `OWNER_USER_IDS` right above `ADMIN_USER_IDS`, explaining owners are a super-admin tier whose facts admins can't delete.
3. In `handle_command`, add a helper `bool owner = cfg_.is_owner(user_id);` next to the existing `bool admin`.

## Owner-protected facts (part of Task B)

Facts added by an owner must not be removable by a regular admin.

1. In `src/facts.cpp` / `facts.hpp`: facts are stored as lines under section headers. Add a notion of a **protected** fact. Simplest approach that fits the existing text format: when an owner adds a fact, prefix the stored line with a marker the parser recognises, e.g. `- !` instead of `- ` (pick a marker and document it in the facts.txt header comment). The parser (`parse_locked`) should record a `bool protected_` on each fact. Keep backward compatibility: a normal `- ` fact is unprotected.
2. `Facts::add` gains a `bool protected_fact` parameter (default false). The `/fact` handler passes `owner` for it.
3. `Facts::remove` must refuse to remove a protected fact unless the caller is an owner. Add a `bool as_owner` parameter to `remove` (or check in the caller). A regular admin's `/unfact` on a protected fact should get "That note is owner-protected." and nothing is deleted.
4. Add a test: add a protected fact, assert a non-owner remove fails and an owner remove succeeds.

## Task B2 — global facts are OWNER-only; fact scoping fixes

Current `/fact` / `/facts` / `/unfact` behaviour (in `handle_command`, `src/main.cpp`):
- `/fact global <text>` writes a global fact (injected into every chat). It is only behind the regular admin gate, so any admin can do it.
- `/fact @user <text>` writes to that user's section.
- `/fact <text>` writes to the current chat's section in a group, or to the *sender's own user section* in a private chat.
- `/facts` and `/unfact` ONLY operate on the current chat's section (or the sender's own section in a DM). Global facts and other users' facts cannot be listed or deleted from Telegram at all.

Required changes:

1. **Global facts are owner-only.** `/fact global <text>` → if not owner, reply "Global notes are owner-only." and save nothing. Owner-added global facts are stored as protected (see Task B).
2. **List and delete by scope.** Extend `/facts` and `/unfact` to take an optional scope as the first argument:
   - `/facts` — this chat (current behaviour).
   - `/facts global` — list global facts (any admin may VIEW).
   - `/facts @user` — list that person's facts.
   - `/unfact <n>` — delete from this chat (current behaviour).
   - `/unfact global <n>` — OWNER only.
   - `/unfact @user <n>` — admin, unless the fact is owner-protected (then owner only).
   Numbering in `/unfact <scope> <n>` must match the numbering shown by `/facts <scope>`.
3. **Fix the DM gotcha.** In a private chat, a bare `/fact <text>` currently saves to the sender's own section as if it were about them. Change it to require an explicit scope in DMs: reply "In a private chat, say who it's about: /fact @user <text>, /fact me <text>, or (owner) /fact global <text>." Add `me` as an explicit scope meaning "the sender".
4. **Stop person facts leaking across chats.** Currently a `[user <id>]` section is injected into *every* chat that person speaks in. Add an optional chat scope to user facts: `/fact @user <text>` used inside a group stores the fact as scoped to that chat (e.g. a new section header form `[user <id> <name> @chat <chat_id>]`, or a per-fact chat tag — pick one, document it in the facts.txt header comment, and keep parsing old unscoped `[user <id>]` sections as "global to that person" for backward compatibility). `Facts::context()` must only inject a user's chat-scoped facts in that same chat, plus their unscoped facts. Add an owner-only way to make a person fact apply everywhere: `/fact @user everywhere <text>`.
5. Tests: (a) non-owner `/fact global` is refused; (b) `/facts global` lists globals; (c) a chat-scoped user fact is injected in its chat and NOT in another chat; (d) an old-format unscoped user section still loads and injects everywhere.

## Task L — aliases: nicknames that map to real users (do right after B2)

**Problem:** the bot can only identify a person by `@username`, numeric user id, or exact first name (`Store::find_user` in `src/store.cpp`; an ambiguous first name returns 0). It has no concept of nicknames, so "Drew" and "@BrandRiver" are unrelated to it: `look_up_facts("Drew")` finds nothing, `/fact Drew ...` doesn't attach to the right person, and the model doesn't know who people are talking about.

**Design:**

1. **New file `aliases.txt`**, path from a new config value `ALIASES_FILE` (default `aliases.txt`). Format — one line per person, keyed by numeric user id so there are no alias chains or cycles:
   ```
   # aliases.txt - nicknames for people. One line per person: <user_id>: name, name, @username
   # Matching is case-insensitive; a leading @ is optional. Edit any time; reloads automatically.
   123456789: Drew, D, @BrandRiver
   987654321: Casey, Dr.Business, @YoMamaLlama
   ```
   Owner-added aliases are protected the same way as owner facts (Task B) — use the same marker convention you chose there, and document it in the header comment.

2. **New module `src/aliases.hpp` / `src/aliases.cpp`** with a thread-safe `Aliases` class, modelled on `Facts`: mtime-based reload, atomic write via temp file + rename, its own mutex. API at minimum:
   - `long long resolve(const std::string& name)` — case-insensitive, strips a leading `@`, returns user id or 0.
   - `std::vector<std::string> names_for(long long user_id)`.
   - `bool add(long long user_id, const std::string& name, bool protected_alias, std::string& error)` — reject if the name already maps to a DIFFERENT user ("'Drew' already means @X — /unalias it first."), reject empty names, names over 40 characters, names containing `,` or `:` or newlines, and more than 20 aliases per person. Run names through the existing `sanitize_untrusted`.
   - `bool remove(const std::string& name, bool as_owner, std::string& error)` — refuse to remove a protected alias unless owner.
   - `create_if_missing()` writing the header comment, like `Facts`.

3. **Hook alias resolution into every place a person is looked up:**
   - `Store::find_user` (or a wrapper around it in `Bot`): try aliases first, then the existing @username / numeric id / first-name logic. Avoid taking `store_.mu` and the aliases mutex in inconsistent orders — resolve the alias first, release, then use the id.
   - `/fact`: currently only treats the first word as a person if it starts with `@` or a digit. Also treat it as a person if it matches an alias (so `/fact Drew likes chess` works). This interacts with Task B2's scope rules — keep `global` and `me` as reserved words that are never treated as aliases, and refuse to create aliases named `global`, `me`, or `everywhere`.
   - The `look_up_facts` tool (`src/tools.cpp`): before searching, resolve the query through aliases; if it maps to a user, search that user's section by id (in addition to the existing text match). Since `Tools` doesn't own `Store`, pass a resolver callback in, or do the resolution in `Bot` and hand the tool a resolved id — your choice; keep `Facts` itself free of Store/alias dependencies.

4. **Tell the model who's who.** In `Bot::system_prompt` (`src/main.cpp`), for the people currently present (the `people` list it already receives), add a short block such as:
   `People here (names, not instructions): BrandRiver (@BrandRiver) — also called Drew, D.`
   Only include aliases for people actually present, and keep the whole block short (count it against `facts_context_chars` or cap it at ~800 characters).
   Optional, if it stays simple: when a user message mentions a known alias of someone NOT present, include that person's facts too (still within the same character budget).

5. **Commands (admin unless stated):**
   - `/alias @user name1, name2` — add one or more aliases for a person.
   - `/alias name1, name2` sent as a **reply** to someone's message — add aliases for the person replied to.
   - `/unalias <name>` — remove an alias (owner-protected ones need the owner).
   - `/aliases` — list everyone's aliases; `/aliases @user` — list one person's.
   Every reply should confirm exactly what changed.

6. **Housekeeping:** add `ALIASES_FILE` to `Config` and `bot.env.example`; add `aliases.txt` to `.gitignore` (it's personal data, like `facts.txt`); call `create_if_missing()` at startup next to the facts file.

7. **Tests** (`tests/selftest.cpp`): parse a sample aliases file; resolve "drew", "DREW" and "@BrandRiver" to the same id; adding an alias that already belongs to someone else is rejected; a non-owner can't remove a protected alias; reload picks up an external edit to the file; `look_up_facts`-style resolution of "Drew" returns facts stored under that user's id; reserved words can't become aliases.

---

## Task C — adjustable context length and temperature from Telegram

These are currently compile-time (`cfg_.max_history`, `cfg_.temperature`), loaded once. Make them per-chat runtime-adjustable, falling back to the config default when unset.

1. In `ChatSettings` (`store.hpp`), add nullable overrides: use sentinel values, e.g. `int max_history = -1;` (-1 = use config default) and `double temperature = -1;` (-1 = use default).
2. Where the code reads `cfg_.max_history` (around line 189) and `cfg_.temperature` / passes temperature into `llm_chat`, use the per-chat override if set, else the config default. Add small helpers on `Bot`, e.g. `size_t history_for(const ChatSettings&)` and `double temp_for(const ChatSettings&)`.
3. Commands (admin-only):
   - `/context <n>` — set how many past messages this chat keeps (the history window). Clamp to a sane range (e.g. 2–500). `/context` with no arg shows the current value. **Note:** this is the *message history count* (`max_history`). It is NOT the model's token context window `-c`, which is fixed at llama-server launch — say so in the reply so it isn't confused with Task H.
   - `/temp <x>` — set sampling temperature, clamp 0.0–2.0. `/temp` alone shows current value and this note: *"Lower = more focused and less sassy (try 0.4–0.6). Higher = more random (0.8–1.2). Qwen's default is ~0.6."*
4. Persist both in `state.json` (they're in `ChatSettings`, which is already saved).
5. Tests: set and read back both, assert clamping works.

## Task M — persona reminder every N messages, configurable from bot.env and Telegram

**Current behaviour and bug:** `Config::persona_reminder` is a `bool` parsed with `get_bool("PERSONA_REMINDER", true)` (`src/config.cpp`). `get_bool` only treats `1`, `true`, `yes`, `on` as true. The shipped `bot.env.example` sets `PERSONA_REMINDER=4` — which `get_bool` reads as **false**, so the reminder is silently disabled. When it *is* enabled, `Bot::process` (`src/main.cpp`, around line 202) appends the full persona (up to 2000 chars) to the last user message on **every** request.

**Required changes:**

1. Make `persona_reminder` an **integer** N in `Config` (`int persona_reminder = 1;`):
   - `0` = off.
   - `1` = remind on every user message (the old `true` behaviour).
   - `N > 1` = remind on every Nth user message in that chat.
   - Backward compatibility when parsing: `true`/`yes`/`on` → 1, `false`/`no`/`off` → 0, otherwise parse as an integer (invalid → default 1, and log a warning).
2. Decide when to remind by counting the user messages in that chat's history (`hist`): remind when `count % N == 0`. Also **always** remind on the first user message after `/reset`, `/forget`, or a `/persona` change, so a new persona takes hold immediately.
3. Add a per-chat override in `ChatSettings`: `int persona_reminder = -1;` (-1 = use the config default). Saved in `state.json` with the rest of `ChatSettings`. Add a helper `int reminder_for(const ChatSettings&)` on `Bot`.
4. Add a length cap so the reminder doesn't re-send a huge persona every time: new config value `PERSONA_REMINDER_MAX_CHARS` (default 600). If the persona is longer, the reminder uses the first `PERSONA_REMINDER_MAX_CHARS` characters (cut at a sentence or word boundary via the existing UTF-8-safe helpers) — the full persona is still in the system prompt.
5. **Admin commands** (extend the existing `/persona` handler; keep `/persona <text>` and `/persona reset` working exactly as now):
   - `/persona reminder <n>` — set this chat's reminder frequency (clamp 0–50).
   - `/persona reminder` — show this chat's current value and whether it's the default.
   - `/persona reminder default` — clear the override.
   Make sure the word `reminder` as the first argument is treated as this subcommand, not as persona text.
6. Show the effective reminder setting in `/limits` (Task H).
7. Update `bot.env.example`: document `PERSONA_REMINDER` as "0 = off, 1 = every message, N = every Nth message" and add `PERSONA_REMINDER_MAX_CHARS`.
8. **Tests:** parsing of `4`, `true`, `off`, and garbage; the every-Nth decision for a range of message counts; the per-chat override takes precedence over the config value; the reminder text is truncated to the cap.

## Task N — settings layers, guaranteed persistence, and reset-to-defaults

Do this together with (or immediately after) Tasks C, E/G, H, and M, since it defines where all of their settings live.

**Current state:** `state.json` persists only two per-chat settings, `persona` and `thinking` (`Store::load`/`Store::save` in `src/store.cpp`). Note the save condition: a chat is only written if `!s.persona.empty() || s.thinking != -1`. **Any new `ChatSettings` field must be added to `load`, to `save`, AND to that condition** — otherwise a chat whose only change is, say, temperature will never be saved and the change will be lost on restart. Replace that condition with a helper `bool ChatSettings::has_overrides() const` that checks every field, so future fields can't be forgotten.

**Three layers, highest priority first:**
1. **Per-chat override** — `ChatSettings` in `state.json` (`chats`). Set by admins.
2. **Global override** — a new `ChatSettings global_settings` in `Store`, saved in `state.json` under `"global_settings"`. Set by the **owner** only. Applies to every chat without a per-chat override.
3. **Factory defaults** — the values in `bot.env` (`Config`). The bot must **never write to `bot.env`**; it is human-edited only and always represents the defaults.

All the per-setting helpers (`thinking_for`, `temp_for`, `history_for`, `max_tokens_for`, `think_budget_for`, `reminder_for`, and persona) must resolve in exactly that order: chat → global → `bot.env`. Write one small generic pattern for this rather than repeating the logic in each helper.

**Commands:**
- Each setting command from Tasks C, E/G, H, M (`/temp`, `/context`, `/maxtokens`, `/think budget`, `/think on|off`, `/persona reminder`) works per-chat for admins, and gains a `global` form for the owner, e.g. `/temp global 0.6`.
- With no value, each command shows all three layers, e.g. `Temperature: 0.6 (this chat) · global: not set · default (bot.env): 0.7`.
- `/defaults` (admin) — clear **this chat's** overrides, including persona and thinking. Reply lists what was reset.
- `/defaults global` (owner) — clear the global overrides.
- `/defaults all CONFIRM` (owner) — clear the global overrides AND every chat's overrides. Without `CONFIRM`, explain what it does and ask for it.
- `/defaults` never touches facts, aliases, the allow list, trusted groups, or conversation history — say so in the reply.
- `/limits` (Task H) shows the effective value of every setting and which layer it came from.

**Tests:**
- Precedence: chat override beats global, global beats `bot.env`, and unset falls through correctly.
- Save → load round trip preserves every `ChatSettings` field.
- A chat whose ONLY change is a temperature override survives save → load (this is the regression test for the save-condition bug).
- `/defaults` clears one chat; `/defaults all CONFIRM` clears every chat plus global; facts and aliases are untouched.
- The bot never opens `bot.env` for writing (assert the config path isn't written; at minimum, grep-level review).

## Task E / G — adjustable and better-controlled reply length

"Length of messages it has access to" (E) overlaps with Task C's `/context` (history count) — that's covered. "Control the length of messages better" (G) is about the **reply** length.

**Decision: `max_tokens` caps the ANSWER only. Thinking has its own separate budget (Task H).** Do E/G together with H, since they share the streaming logic.

**The technical catch:** llama-server's `max_tokens` request field counts ALL generated tokens, thinking included — the server can't cap them separately. So the server's limit can only be a total safety ceiling, and the bot itself must enforce the two separate caps while streaming. This is feasible because `src/llm.cpp` already receives thinking and answer separately: thinking arrives as `delta.reasoning_content` (and, as a fallback, inline `<think>...</think>` text), and the answer arrives as `delta.content`. The `on_update` hook already reports both.

1. Add per-chat `int max_tokens = -1;` to `ChatSettings` (-1 = use the layered default, Task N). This value is the **answer** cap.
2. **Server ceiling:** when calling `llm_chat`, send `max_tokens = answer_cap + think_budget` if thinking is on with a budget; `answer_cap` alone if thinking is off. If thinking is on with no budget (unlimited), use a new safety ceiling `MAX_TOTAL_TOKENS` from config (default 32768) so nothing can run forever.
3. **Bot-side answer cap:** count answer tokens as they stream — count `content` deltas (llama-server sends roughly one token per streamed delta; this approximation is fine, note it in a comment) and use the exact `usage.completion_tokens` at the end for stats. When the answer count exceeds the cap, cancel the generation (reuse the existing `cancel_` mechanism), keep what was written, and append a short marker such as ` … (reply length limit)`. Thinking tokens never count toward this cap.
4. **Avoid ugly mid-sentence cuts:** also tell the model its limit, so it usually stays under it on its own. Add to the system prompt a line like `Keep your reply under about N words.`, where N ≈ answer_cap × 0.7. Only add this line when the cap is below ~2000 tokens; for large caps it's unnecessary. The hard cutoff in step 3 is the safety net.
5. **Retire `RETRY_MAX_TOKENS`.** The retry path (~line 261) runs with thinking off, so its cap is now simply the answer cap. Keep parsing `RETRY_MAX_TOKENS` for backward compatibility, but if it's set, log a one-time warning that it's deprecated and ignored. Update `bot.env.example` accordingly.
6. **Admin command `/maxtokens <n>`** (with the owner `global` form from Task N) — clamp to 64–16000. No-arg shows all three layers. The reply should say: *"This caps the answer only. Thinking is capped separately with /think budget. At ~32 tok/s, 1000 answer tokens ≈ 30 seconds."*
7. `/limits` (Task H) shows answer cap, thinking budget, and the resulting server ceiling separately.
8. **Interplay with thinking on/off:**

   | Mode | Server ceiling | Thinking | Answer |
   |---|---|---|---|
   | Thinking off | answer cap | none (`enable_thinking: false`) | capped at answer cap |
   | Thinking on, budget set | answer cap + think budget | forced to wrap up at the budget (Task H) | capped at answer cap |
   | Thinking on, no budget | `MAX_TOTAL_TOKENS` | runs until done | capped at answer cap |

   - With thinking off, the think budget is ignored; `/limits` should say "thinking: off (budget not used)".
   - **Guard: a model that thinks despite thinking being off.** Some models ignore `enable_thinking: false`, or emit inline `<think>` text. If any reasoning arrives while thinking is off, the bot must not let it consume the answer cap: send a server ceiling of `answer_cap + FALLBACK_THINK_TOKENS` (new config, default 1024) whenever thinking is off, and apply the Task H force-wrap-up at `FALLBACK_THINK_TOKENS` if reasoning shows up anyway. Log a warning the first time this happens per model, since it means the model isn't honoring the flag.
   - **Separate sampling for thinking vs non-thinking.** Qwen models recommend different settings per mode (e.g. Qwen3.8-Flash-Next: thinking `temperature=1.0, top_p=0.95, presence_penalty=0.0`; non-thinking `temperature=0.7, top_p=0.80, presence_penalty=1.5`). Add config defaults `TEMPERATURE_THINKING` and `TEMPERATURE_NO_THINKING` (plus `TOP_P_*` and `PRESENCE_PENALTY_*` the same way), sent per request based on the mode actually used. A per-chat `/temp` override (Task C) still wins over both. The existing single `TEMPERATURE` remains as a fallback for both modes if the mode-specific ones aren't set.
   - The retry path (thinking off) uses the non-thinking sampling settings.

9. **Tests:** the server ceiling is computed correctly for thinking on with budget, thinking on without budget (uses `MAX_TOTAL_TOKENS`), and thinking off (answer cap + `FALLBACK_THINK_TOKENS`); a simulated thinking-off stream that emits reasoning anyway gets force-wrapped at `FALLBACK_THINK_TOKENS` and still produces an answer; the mode-specific sampling values are selected correctly and a per-chat `/temp` overrides them; a simulated stream with 5000 reasoning tokens and a 500-token answer cap is NOT cut during thinking but IS cut at 500 answer tokens; the "keep under N words" line appears only for small caps; clamping works.

## Task H — thinking-token budget + a status command

**Goal:** stop the bot from thinking for 10 minutes. Let an admin cap thinking tokens; after the cap, force it to answer.

1. Add per-chat `int think_budget = -1;` to `ChatSettings` (-1 = no separate cap / use config default; 0 could mean "unlimited" — pick and document).
2. Add config default `int think_budget = 0;` (0 = off) in `Config` + `THINK_BUDGET` in bot.env.example.
3. Implement budget enforcement in the streaming loop. The model emits reasoning inside `<think>…</think>` (or the server exposes a reasoning channel — check how `llm_chat`/`live.cpp` currently streams; match whatever is there). While still inside the thinking section, count tokens/characters streamed. When the count exceeds the budget:
   - Cancel the current generation, then re-issue the request with the thinking so far plus a forced close of the thinking section (append `</think>` to the assistant turn) so the model must produce its answer from what it has.
   - If forcing a continuation is impractical with the current `llm_chat` interface, the acceptable fallback is: cancel and re-issue the SAME request with thinking **off** and `retry_max_tokens` (this reuses the existing retry path — see ~line 261). Document which approach you chose in a comment.
4. Add admin command `/think budget <n>` (extend the existing `/think` handler) — set the per-chat thinking cap; `/think budget` with no arg shows it. Keep `/think on|off` working as-is.
5. Add a **status** command `/limits` (admin) that prints, for this chat: effective max_history, temperature, max_tokens, think_budget, thinking on/off, and the **fixed** server context size if discoverable (you can show the `-c` value only if the server reports it via `/props` or `/v1/models`; otherwise print "server context: set at launch"). Also fold this summary into `/help` for admins, or reference `/limits` there.
6. Test: budget comparison logic (given a token count and a budget, does it trip?).

---

## Task D — true context flush + Task I — owner-only facts wipe

**Problem (D):** after `/reset`, and even after a bot restart, the bot still "knows" things it shouldn't and calls people names it shouldn't. That information is NOT in the chat history — it's in three other places, which `/reset` currently doesn't touch:
- `facts.txt` (global + per-user + per-chat facts) — persists on disk.
- `ChatSettings.persona` in `state.json` — a custom persona can contain instructions/nicknames.
- `Store::speakers` / `Store::known` — remembered display names.

Implement a layered flush:

1. **`/reset`** (admin) — keep current behaviour (clears this chat's history) but also clear this chat's `speakers` entry, and say clearly what it did and did NOT clear ("Cleared this chat's conversation. Facts and persona are kept — use /forget or /wipefacts for those.").
2. **`/forget`** (admin) — new. Clears, for THIS chat: history, speakers, the chat persona (reset to default), and this chat's `[chat <id>]` facts section. Does NOT touch global facts or other chats. Reply lists what was cleared.
3. **`/wipefacts`** (OWNER only — this is Task I) — clears the entire facts store: all global, per-user, and per-chat facts, including owner-protected ones (owner is doing it). Rewrite `facts.txt` to the empty template. Require a confirmation: `/wipefacts CONFIRM` actually wipes; `/wipefacts` alone explains and asks for the CONFIRM arg. Log it.
4. Make sure all three reload/rewrite cleanly and the in-memory facts match the file afterward (call the facts reload).
5. Tests: after `/forget`, a chat fact is gone but a global fact remains; `/wipefacts CONFIRM` empties everything.

---

## Task J — model switching from Telegram

Let an admin list and switch models. Models live in subfolders under `/models` (e.g. `/models/q122/Qwen3.5-...gguf`, `/models/flashnext/...gguf`).

**Important architecture note:** THIS bot does not load models — `llama-server` does. The bot only talks to `llm_url`. So "switching models" means telling llama-server to serve a different file. Two possible designs — implement **Design 1** (simpler, robust) unless you find llama-server already running with a router:

**Design 1 — restart a model-server script (recommended).**
1. The bot does not manage llama-server directly; instead it writes the desired model path to a file (e.g. `/models/.current_model`) and relies on an external supervisor script to (re)launch llama-server with that model. Provide this as `scripts/run-model.sh` (write it) that reads `.current_model` and execs llama-server with the right flags, and document that llama-server should be run via this script under systemd or tmux.
2. `/model` (admin, no arg) — show the current model (query `llm_model_name(cfg_)` / the `/v1/models` endpoint, already wired as `model_name_`) and list available `.gguf` files found by scanning `/models/**` (recurse one or two levels). Number them.
3. `/model <name-or-number>` (admin) — match against the discovered list; write the chosen path to `/models/.current_model`; tell the user it will take effect when the model server reloads; optionally trigger the supervisor (e.g. `touch` a reload file or restart a named systemd unit if `MODEL_RELOAD_CMD` is set in config). Add `MODEL_RELOAD_CMD` to Config/bot.env.example (empty = manual).
4. Guard the directory scan: only list regular files ending in `.gguf`, ignore the `.cache` folder, and cap the count.

**Do NOT** have the bot fork llama-server itself inside the bot process. Keep model serving as a separate process the bot signals.

5. Tests: the model-list scan (point it at a temp dir with fake `.gguf` files, assert it finds them and ignores non-gguf).

---

## Task K — logging to a file

Currently `log()` in `src/util.cpp` only writes to `std::cout`.

1. Extend `log()` to ALSO append to a logfile. Add a `void set_log_file(const std::string& path);` in `util.hpp` that opens an `std::ofstream` (append mode, line-buffered, guarded by the existing log mutex). `log()` writes the timestamped line to both `std::cout` and the file if one is set.
2. Use a fuller timestamp in the file (date + time), keep the short time for console.
3. In `Config`, add `std::string log_file;` from `LOG_FILE` env (default empty = console only, or default `tgbot.log` — your call, document it). In `main()`, call `set_log_file(cfg.log_file)` early if set.
4. Make the key events log clearly: every command received (user id + command, truncate args), every model request start/finish with token count and tok/s, tool calls, access grants/denials, restarts, and errors. Several of these already log — just ensure command receipts and errors are covered.
5. Rotate-safe isn't required; keep it simple (single appending file). Document in README that the user can rotate with `logrotate` or just truncate.
6. Test: `set_log_file` to a temp path, call `log()`, assert the line appears in the file.

---

## Final checklist (do this after all tasks)

- `make` builds clean, `make test` passes.
- Update `bot.env.example` with every new variable: `OWNER_USER_IDS`, `THINK_BUDGET`, `LOG_FILE`, `MODEL_RELOAD_CMD`, `ALIASES_FILE`, `MAX_TOTAL_TOKENS`, `FALLBACK_THINK_TOKENS`, `TEMPERATURE_THINKING` / `TEMPERATURE_NO_THINKING` (and the matching `TOP_P_*` / `PRESENCE_PENALTY_*`), and note any new defaults. Document that `MAX_TOKENS` now caps the answer only, and mark `RETRY_MAX_TOKENS` as deprecated.
- `/alias`, `/unalias`, `/aliases` are in `/help` (admin section) and README.
- `/persona reminder <n>|default` is in `/help` and README; `PERSONA_REMINDER` and `PERSONA_REMINDER_MAX_CHARS` are documented in `bot.env.example`.
- `/defaults`, `/defaults global`, `/defaults all CONFIRM`, and the `global` form of each setting command are in `/help` and README, with the three-layer explanation (chat → global → bot.env).
- Update `README.md`'s command list with: `/context`, `/temp`, `/maxtokens`, `/think budget`, `/limits`, `/forget`, `/wipefacts` (owner), `/model`, `/facts global|@user`, `/unfact global|@user <n>`, `/fact me|global|@user [everywhere]`, and the owner tier.
- Update the in-bot `/help` text (HELP_ADMIN) to include the new admin commands, and add an owner-only section for `/wipefacts`.
- Each command that changes a setting should, with no argument, PRINT the current value.
- Commit everything. Do not push; the human will review and push.

## Notes / gotchas

- Telegram strips `||...||` as spoiler markup — never rely on `||` surviving a round trip through chat; this only matters for humans pasting code, not for you editing files directly, but keep bot replies free of accidental `||`.
- Keep owner/admin checks server-side in `handle_command`; never trust the client.
- When unsure whether a setting is per-chat or global: per-chat (in `ChatSettings`) is the safer default for C, E, G, H; global/owner for I and J.
- If a task's exact mechanism fights the existing code, prefer the smallest change that satisfies the intent, and leave a short `// NOTE:` comment explaining the choice.
