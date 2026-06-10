/*
 *  machine_profile.cpp - Machine Layer profile selection
 *  See docs/planning/MACHINE-LAYER-PLAN.md (M0) and include/machine_profile.h.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "machine_profile.h"

#ifndef MACHINE_PROFILE_STANDALONE_TEST
#include "sysdeps.h"
#include "prefs.h"
#endif

static MachineProfile g_profile = MACHINE_PARAVIRTUAL;

static bool parse_one(const char *s, MachineProfile *out)
{
	if (s == NULL)
		return false;
	if (strcmp(s, "paravirtual") == 0) { *out = MACHINE_PARAVIRTUAL; return true; }
	if (strcmp(s, "newworld") == 0)    { *out = MACHINE_NEWWORLD;    return true; }
	return false;
}

MachineProfile MachineProfileParse(const char *pref, const char *env_machine,
                                   bool legacy_nw_trampoline, const char **warning)
{
	*warning = NULL;
	MachineProfile p;

	// 1. SS_MACHINE env: explicit, wins over everything.
	if (env_machine != NULL && *env_machine != '\0') {
		if (parse_one(env_machine, &p))
			return p;
		*warning = "SS_MACHINE has an unknown value (expected paravirtual|newworld) - ignored";
		// fall through to the next source
	}

	// 2. Legacy SS_NW_TRAMPOLINE: deprecated alias for the newworld profile.
	if (legacy_nw_trampoline) {
		*warning = "SS_NW_TRAMPOLINE is deprecated - use SS_MACHINE=newworld or the 'machine' pref";
		return MACHINE_NEWWORLD;
	}

	// 3. The "machine" pref (registered in Task 2 with default "paravirtual").
	if (pref != NULL && *pref != '\0') {
		if (parse_one(pref, &p))
			return p;
		*warning = "machine pref has an unknown value (expected paravirtual|newworld) - using paravirtual";
	}

	// 4. Default.
	return MACHINE_PARAVIRTUAL;
}

bool MachineEnvFlag(const char *name)
{
	const char *v = getenv(name);
	return v != NULL && *v != '\0' && strcmp(v, "0") != 0;
}

#ifndef MACHINE_PROFILE_STANDALONE_TEST
void MachineProfileInit(void)
{
	const char *warning = NULL;
	g_profile = MachineProfileParse(PrefsFindString("machine"),
	                                getenv("SS_MACHINE"),
	                                getenv("SS_NW_TRAMPOLINE") != NULL,
	                                &warning);
	if (warning)
		fprintf(stderr, "[MACHINE] WARNING: %s\n", warning);
	fprintf(stderr, "[MACHINE] profile=%s\n",
	        g_profile == MACHINE_NEWWORLD ? "newworld" : "paravirtual");
}
#endif

MachineProfile CurrentMachineProfile(void)
{
	return g_profile;
}

bool MachineProfileIsNewWorld(void)
{
	return g_profile == MACHINE_NEWWORLD;
}
