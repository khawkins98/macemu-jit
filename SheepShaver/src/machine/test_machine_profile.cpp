/*
 *  test_machine_profile.cpp - standalone unit test for MachineProfileParse /
 *  MachineEnvFlag. Builds without the emulator (MACHINE_PROFILE_STANDALONE_TEST).
 *  Run: make -C SheepShaver/src/machine test
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "machine_profile.h"

static int failures = 0;

#define CHECK(cond) do { \
	if (cond) { printf("PASS: %s\n", #cond); } \
	else      { printf("FAIL: %s (line %d)\n", #cond, __LINE__); failures++; } \
} while (0)

int main()
{
	const char *warn;

	// Defaults: nothing set -> paravirtual, no warning
	warn = (const char *)1;
	CHECK(MachineProfileParse(NULL, NULL, false, &warn) == MACHINE_PARAVIRTUAL);
	CHECK(warn == NULL);

	// Pref selects
	CHECK(MachineProfileParse("paravirtual", NULL, false, &warn) == MACHINE_PARAVIRTUAL);
	warn = (const char *)1;
	CHECK(MachineProfileParse("newworld", NULL, false, &warn) == MACHINE_NEWWORLD);
	CHECK(warn == NULL);

	// Empty pref -> paravirtual, silently (no warning)
	warn = (const char *)1;
	CHECK(MachineProfileParse("", NULL, false, &warn) == MACHINE_PARAVIRTUAL);
	CHECK(warn == NULL);

	// Unknown pref -> paravirtual + warning (never abort)
	CHECK(MachineProfileParse("g3beige", NULL, false, &warn) == MACHINE_PARAVIRTUAL);
	CHECK(warn != NULL);

	// SS_MACHINE env wins over pref
	CHECK(MachineProfileParse("paravirtual", "newworld", false, &warn) == MACHINE_NEWWORLD);
	CHECK(MachineProfileParse("newworld", "paravirtual", false, &warn) == MACHINE_PARAVIRTUAL);

	// Empty SS_MACHINE -> falls through silently (no warning)
	warn = (const char *)1;
	CHECK(MachineProfileParse(NULL, "", false, &warn) == MACHINE_PARAVIRTUAL);
	CHECK(warn == NULL);

	// Unknown env -> fall through to pref + warning
	CHECK(MachineProfileParse("newworld", "bogus", false, &warn) == MACHINE_NEWWORLD);
	CHECK(warn != NULL);

	// Unknown env with no pref -> default + warning
	CHECK(MachineProfileParse(NULL, "bogus", false, &warn) == MACHINE_PARAVIRTUAL);
	CHECK(warn != NULL);

	// Legacy SS_NW_TRAMPOLINE -> newworld + deprecation warning
	CHECK(MachineProfileParse(NULL, NULL, true, &warn) == MACHINE_NEWWORLD);
	CHECK(warn != NULL);
	CHECK(MachineProfileParse("paravirtual", NULL, true, &warn) == MACHINE_NEWWORLD);

	// Explicit SS_MACHINE beats legacy flag
	CHECK(MachineProfileParse(NULL, "paravirtual", true, &warn) == MACHINE_PARAVIRTUAL);

	// MachineEnvFlag: set/unset/zero/empty
	setenv("MP_TEST_FLAG", "1", 1);
	CHECK(MachineEnvFlag("MP_TEST_FLAG") == true);
	setenv("MP_TEST_FLAG", "0", 1);
	CHECK(MachineEnvFlag("MP_TEST_FLAG") == false);
	setenv("MP_TEST_FLAG", "", 1);
	CHECK(MachineEnvFlag("MP_TEST_FLAG") == false);
	unsetenv("MP_TEST_FLAG");
	CHECK(MachineEnvFlag("MP_TEST_FLAG") == false);

	printf(failures ? "RESULT: %d FAILURES\n" : "RESULT: ALL PASS\n", failures);
	return failures ? 1 : 0;
}
