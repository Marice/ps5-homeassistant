/* Rest mode and power off, on Home Assistant's request.
 *
 * Both go through the system's own services in libSceSystemService, as the console's menu does:
 * sceSystemStateMgrEnterStandby (rest mode; John Tornblom's shsrv uses it the same way) and
 * sceSystemStateMgrTurnOff (power off; PS5-Suite's first path). The raw reboot() syscall is not used: PS5-Suite
 * found it can leave the console in a state that needs the power plug.
 *
 * Power off ends the jailbreak until the exploit runs again, and the console can then not be woken over the
 * network. Both are off unless the config's power_actions is on. */
#ifndef POWER_H
#define POWER_H

#include <stdbool.h>
#include <stddef.h>

#define POWER_MIN_GAP_MS 10000

/* Whether a power command may run now: switched on, and the last one at least POWER_MIN_GAP_MS ago
   (last_ms is 0 when there was none). On false, reason says why. Pure; tested on the host. */
static inline bool power_allowed(bool enabled, long long last_ms, long long now_ms, const char** reason)
{
	if (!enabled) {
		*reason = "power actions are off";
		return false;
	}
	if (last_ms != 0 && now_ms - last_ms < POWER_MIN_GAP_MS) {
		*reason = "one power action per 10 seconds";
		return false;
	}
	*reason = "";
	return true;
}

/* Each returns the service's result: 0 when it accepted. Console only. */
int power_rest(void);
int power_off(void);

#endif
