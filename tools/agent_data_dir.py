"""Point the agent-profile env's LittleFS image at data_agent/ (W5.1).

The stock data/system_prompt.txt is a FULL-toolset charter (gpio, files,
rules, telegram) — on a WIRECLAW_AGENT_TOOLS_RESTRICTED build it actively
instructs tools the in-loop gate refuses, which taught the 4B model to
substitute allowed tools and narrate them as the requested action
(observed live on dudeclaw-02, 2026-07-04). The agent env ships its own
FS image with the restricted charter + honesty rules instead.

PlatformIO's data_dir is project-global, so this pre: script swaps it per
env; the buildfs log line ("Building FS image from ... directory") is the
verification that the swap took.
"""
Import("env")  # noqa: F821 — SCons construction environment

env.Replace(PROJECT_DATA_DIR=env.subst("$PROJECT_DIR/data_agent"))  # noqa: F821
print("[agent_data_dir] LittleFS source swapped to data_agent/")
