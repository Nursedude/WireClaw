/**
 * @file tools.h
 * @brief ESP32 tool definitions and handlers for LLM tool calling
 *
 * Defines the tools the LLM can call and their execution handlers.
 */

#ifndef TOOLS_H
#define TOOLS_H

#include <Arduino.h>

/* Max length of a tool result string */
#define TOOL_RESULT_MAX_LEN 512

/**
 * Get the JSON array of tool definitions for the LLM API.
 * Returns a static string - do not free.
 */
const char *toolsGetDefinitions();

/**
 * Execute a tool by name with JSON arguments.
 *
 * @param name       Tool name (e.g., "led_set")
 * @param args_json  JSON arguments string (e.g., "{\"r\":255,\"g\":0,\"b\":0}")
 * @param result     Output buffer for result string
 * @param result_len Size of result buffer
 * @return true if tool was found and executed
 */
bool toolExecute(const char *name, const char *args_json,
                  char *result, int result_len);

/**
 * Measure battery voltage via the switched VBAT divider. One measurement
 * path shared by the battery_read tool and the display's SELF page — two
 * copies would drift.
 *
 * @param adc_mv_out  optional raw ADC millivolts (may be NULL)
 * @return volts, or NAN on builds without battery sense
 */
float batteryReadVolts(unsigned int *adc_mv_out);

/*
 * On-device agent tool restriction (WIRECLAW_AGENT_TOOLS_RESTRICTED).
 *
 * The on-device LLM agent's tool surface can be compile-time restricted to
 * display/LED/read-only sensors — actuation, RF transmission, filesystem,
 * bus reach and self-rewiring stay behind the NATS tool_exec path (the
 * brain's ratified rules), which is NOT affected by this flag. Without the
 * flag both calls fall through to the full set, so existing builds are
 * byte-identical.
 */
#ifdef WIRECLAW_AGENT_TOOLS_RESTRICTED

/** Tool definitions the on-device agent is offered (filtered allowlist). */
const char *toolsGetAgentDefinitions();

/** True when the on-device agent may execute this tool. */
bool toolAllowedForAgent(const char *name);

#else /* unrestricted builds: the agent sees the full tool set */

static inline const char *toolsGetAgentDefinitions() {
    return toolsGetDefinitions();
}
static inline bool toolAllowedForAgent(const char *) { return true; }

#endif /* WIRECLAW_AGENT_TOOLS_RESTRICTED */

#endif /* TOOLS_H */
