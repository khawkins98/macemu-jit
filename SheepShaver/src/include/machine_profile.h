/*
 *  machine_profile.h - Machine Layer profile selection (MACHINE-LAYER-PLAN.md M0)
 *
 *  Two machine profiles:
 *    MACHINE_PARAVIRTUAL - today's proven path (ROM patches + HLE shims). Default.
 *    MACHINE_NEWWORLD    - the Core99-class fidelity profile (MMIO bus, device
 *                          models, supervisor environment), grown milestone by
 *                          milestone. Selecting it disables the legacy PC-keyed
 *                          serial-skip hacks and the ignoresegv blanket skip.
 *
 *  Precedence: SS_MACHINE env > legacy SS_NW_TRAMPOLINE env (deprecated alias
 *  for newworld) > "machine" pref > default paravirtual. Unknown values fall
 *  back to paravirtual with a warning (never abort).
 */

#ifndef MACHINE_PROFILE_H
#define MACHINE_PROFILE_H

enum MachineProfile {
	MACHINE_PARAVIRTUAL = 0,
	MACHINE_NEWWORLD    = 1
};

// Pure decision function (unit-tested standalone; no emulator dependencies).
// pref / env_machine may be NULL (the "machine" pref is registered in Task 2
// with default "paravirtual"). legacy_nw_trampoline = SS_NW_TRAMPOLINE set.
// warning must not be NULL: on deprecated/unknown input, *warning receives a
// static message (else NULL).
extern MachineProfile MachineProfileParse(const char *pref,
                                          const char *env_machine,
                                          bool legacy_nw_trampoline,
                                          const char **warning);

// Process-wide state. Init once, immediately after PrefsInit().
extern void MachineProfileInit(void);
extern MachineProfile CurrentMachineProfile(void);
extern bool MachineProfileIsNewWorld(void);

// Centralized diagnostic env flags (SS_NW_MODEL, SS_NW_SYNTH_ENTRY, ...):
// true iff the variable is set, non-empty, and not "0".
extern bool MachineEnvFlag(const char *name);

// True when the MMIO bus + device models are live: the newworld profile, or the
// named third config "paravirtual + bus + devices - serial-skips" (SS_MMIO_BUS=1).
// MACHINE-LAYER-PLAN.md M1 row, consumer (a).
extern bool MachineUsesMMIOBus(void);

// Pure decision function for the above (unit-tested standalone; no emulator deps).
// env_mmio_bus is the value of SS_MMIO_BUS (may be NULL).
extern bool MachineUsesMMIOBusParse(MachineProfile p, const char *env_mmio_bus);

#endif // MACHINE_PROFILE_H
