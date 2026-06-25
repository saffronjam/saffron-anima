# .agents/

Canonical home for agent-facing assets in this repo. Tool-specific directories symlink into
here rather than holding their own copies (`.claude/skills` → `.agents/skills`).

```
skills/   project skills — procedures an agent loads on demand (one folder per skill,
          SKILL.md + optional scripts/)
```

Durable project knowledge stays in versioned repo files (`AGENTS.md`, `plans/`, `docs/`);
this folder is for the agent tooling that acts on it.
