# Rules for every task

- Only edit files in src/, tests/, Makefile, bot.env.example, and README.md.
  Never edit bot.env, facts.txt, state.json, aliases.txt, or any .aider* file.
- Never weaken, skip, or delete an existing test to make it pass. Never remove
  assertions. You may only ADD tests or fix a test that is genuinely wrong, and
  if you change an existing test, say why in your summary.
- Never change compiler flags or silence warnings with pragmas or casts. Fix the cause.
- Read the relevant code before editing. Reuse existing helpers (trim, lower,
  utf8_head, utf8_tail, sanitize_untrusted, log) instead of writing new ones.
- Don't invent functions or APIs; check the headers for what actually exists.
- Keep changes small and limited to the current task. No unrelated refactoring.
- If the same error survives 3 attempts, STOP. Don't keep guessing. Append a short
  note to BLOCKED.md saying which task, what you tried, and the exact error, then end.
