# PocketDisplay Prompt Template

## Standard Task Prompt
Copy-paste this for every new Cascade/Claude Code task:

---
Read CLAUDE.md for full project context.

Task: [DESCRIBE TASK HERE]

Requirements:
- Do not break existing working flows
- Test all 3 scenarios: Windows-first, Android-first, reconnect
- No cmd window popup
- Build both apps if needed
- Install APK via adb
- Commit and push when done

After completing:
- Update CLAUDE.md: move fixed issues to "Recently Fixed", add new issues if any
- Close relevant GitHub issue with summary comment
---

## Tips
- Start a NEW Cascade conversation for each issue/task
- Keep prompts short and focused
- Let Claude Code work autonomously
- Only intervene if regression occurs
