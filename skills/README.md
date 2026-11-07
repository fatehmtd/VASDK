# VASDK skills

Task guides for working on VASDK, written for coding agents and people alike. Each folder holds a `SKILL.md` in the open [Agent Skills](https://agentskills.io) format (a `name` and a `description` up front, then plain markdown), so tools that load skills pick them up, and the rest reach them through [AGENTS.md](../AGENTS.md). Claude Code finds them through `.claude/skills`, a link to this folder.

Every skill is short: what the part does, where its contract lives, which existing code to copy, what files a change touches, and what "done" means. The details stay in [docs/](../docs/README.md), and the headers stay the source of truth.

| Skill | Use it to |
| --- | --- |
| [vasdk-provider](vasdk-provider/SKILL.md) | add any provider: the shared touch list, code rules, doc page, tests |
| [vasdk-vad](vasdk-vad/SKILL.md) | use or add a voice activity detector |
| [vasdk-stt](vasdk-stt/SKILL.md) | use or add speech-to-text |
| [vasdk-tts](vasdk-tts/SKILL.md) | use or add text-to-speech |
| [vasdk-turn-detection](vasdk-turn-detection/SKILL.md) | use or add an end-of-turn model, tune turn-taking |
| [vasdk-wake-word](vasdk-wake-word/SKILL.md) | add the first wake-word provider, use activation modes |
| [vasdk-llm](vasdk-llm/SKILL.md) | configure LLMs, add an LLM API or local runtime, give the LLM tools |
| [vasdk-nlu](vasdk-nlu/SKILL.md) | use or add a sentence encoder or entity model |
| [vasdk-audio-io](vasdk-audio-io/SKILL.md) | use or add a mic/speaker backend or audio DSP |
| [vasdk-speaker-id](vasdk-speaker-id/SKILL.md) | use or add speaker identification |
| [vasdk-transport](vasdk-transport/SKILL.md) | use or add a transport (how audio gets in and out) |
| [vasdk-core-component](vasdk-core-component/SKILL.md) | add a new stage, processor or frame to the core |
| [vasdk-agent](vasdk-agent/SKILL.md) | write a scripted conversation: agent file, goals, entities, tests |
| [vasdk-app](vasdk-app/SKILL.md) | put a use case in place: an agent, a car vertical or a new app, with its licenses |
| [vasdk-bindings](vasdk-bindings/SKILL.md) | take a core feature to C, Python and Android |

`check_skills.py` checks every skill (its header, every path and link it names, every provider id) and runs in `ctest` as `vasdk_skills`, so a renamed header or id fails the tests instead of leaving a skill stale. Run it by hand with `python3 skills/check_skills.py`.
