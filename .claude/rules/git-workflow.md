---
description: Git commit conventions, branch strategy, merge policy, release workflow, and PR guidelines
---

# Git Workflow

## Commit Messages

Use [Conventional Commits](https://www.conventionalcommits.org/) in English. Never include `Co-Authored-By` or AI attribution lines.

### Format

```
<type>(<scope>): <short summary>

<optional body>

<optional footer(s)>
```

### Types

| Type | When to use |
|------|-------------|
| `feat` | New user-facing functionality |
| `fix` | Bug fix |
| `refactor` | Code restructuring without behavior change |
| `perf` | Performance improvement without behavior change |
| `test` | Adding or updating tests only |
| `docs` | Documentation only |
| `chore` | Build, deps, config, tooling — no production code |
| `ci` | CI/CD pipeline changes |
| `style` | Formatting, whitespace — no logic change |


### Rules

- Summary line: imperative mood, lowercase, no period, **under 72 chars**
- Body (optional): explain **why**, not what — the diff shows what changed
- Wrap body at 80 chars per line
- Use footer for breaking changes: `BREAKING CHANGE: <description>`
- Do NOT append `Co-Authored-By`, `Signed-off-by`, or any AI-generated attribution


## Branch Strategy

Trunk-based development with short-lived feature branches:

```
main (protected, always releasable)
 ├─ feature/<description>      # new features
 ├─ fix/<description>          # bug fixes
 ├─ refactor/<description>     # refactoring
 ├─ hotfix/<description>       # urgent production fixes
 └─ release/v<semver>          # release stabilization (when needed)
```

### Branch Naming

- Use `snake_case` topic, suffixed with an 8-digit `YYYYMMDD` start date: `feature/menu_manager_20260421`
- Keep the topic short but descriptive — aim for 2-4 words max, joined by `_`
- Prefix with type: `feature/`, `fix/`, `refactor/`, `hotfix/`, `chore/`, `docs/`
- Optional issue number goes between the type prefix and the topic: `fix/123_aria2_connection_timeout_20260421`
- The date anchors when the branch was cut, making stale branches easy to spot and letting topics with the same name coexist across time
