# Rules for every task
- Only edit files in src/, tests/, scripts/, Makefile, .gitignore, bot.env.example, and README.md.
  Never edit bot.env, facts.txt, state.json, aliases.txt, or any .aider* file.
- Never weaken, skip, or delete an existing test to make it pass. Never remove
  assertions. Only ADD tests, or fix a test that is genuinely wrong and say why.
- Never change compiler flags or silence warnings with pragmas or casts. Fix the cause.
- Read the relevant code before editing. Reuse existing helpers (trim, lower,
  utf8_head, utf8_tail, sanitize_untrusted, log) instead of writing new ones.
- Don't invent functions or APIs; check the headers for what actually exists.
- Keep changes small and limited to the current task. No unrelated refactoring.
- If the same error survives 3 attempts, STOP. Append a short note to BLOCKED.md
  (task, what you tried, the exact error) and end.
- Never write the model's control tags literally (the word think or /think, or tool_call or /tool_call, inside angle brackets): not in code, comments, docs, tests, or your own reasoning. Refer to them by name ("the close think tag"). In C++ string literals, spell them with octal escapes, for example "\074/think\076" for the close think tag.
