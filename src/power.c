#include "power.h"

/* In libSceSystemService (the SDK's stub library); both take no arguments. */
int sceSystemStateMgrEnterStandby(void);
int sceSystemStateMgrTurnOff(void);

int power_rest(void)
{
	return sceSystemStateMgrEnterStandby();
}

int power_off(void)
{
	return sceSystemStateMgrTurnOff();
}
