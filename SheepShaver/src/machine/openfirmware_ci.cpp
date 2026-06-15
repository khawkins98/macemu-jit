/*
 *  openfirmware_ci.cpp - OF-CI callback + Core99 device-tree model (SS_M18 S2a).
 *
 *  NOTE: as of SS_M18 S2b T4 this TU IS linked into the SheepShaver binary; its
 *  only live consumer is the gated newworld launch seam (sheepshaver_glue.cpp),
 *  which binds of_ci_callback + a Core99 context into the r5 marshalling shim
 *  behind SS_M18_TRAMPOLINE (default OFF). At default-OFF it is unreachable.
 *
 *  See openfirmware_ci.h for scope. The call-method backends are injected by
 *  trampoline_ofci_backends.cpp (T4); /mmu is a NON-ACCEPTANCE recording stub.
 *
 *  Dispatch surface (statically closed per FINDINGS-trampoline-re Q0-A/B,
 *  re-pinned in FINDINGS-s2a-ofci-dt.md):
 *    - 21 direct services
 *    - 14 call-method names (resolved via the call-method service against an
 *      INJECTED backend; an unregistered method counts UNRESOLVED so the gate is
 *      not vacuous)
 *    - 3 interpret literals: key? / key / reset-all
 *
 *  The Core99 device tree is built with CANONICAL, unit-addressed node names
 *  (mac-io@c, interrupt-controller@40000, ...) so that finddevice's component-
 *  wise unit-address-INSENSITIVE match (ADV-1) is genuinely exercised by the
 *  short static spellings the test issues.
 */
#include "openfirmware_ci.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------------- */
/* Device-tree data model                                                 */
/* ---------------------------------------------------------------------- */

struct of_prop {
	const char *name;
	const void *data;
	int len;            /* getproplen value */
	bool nonfinal;      /* Q-S2a.3 provisional (interrupt-map / -mask) */
	bool owned;         /* data was malloc'd here (setprop) — free on teardown */
	bool owned_name;    /* name was strdup'd here (setprop new prop) */
	struct of_prop *next;
};

struct of_node {
	const char *name;   /* canonical node name, may carry @unit-address */
	of_phandle phandle;
	struct of_prop *props;
	struct of_node *parent;
	struct of_node *child;     /* first child */
	struct of_node *sibling;   /* next peer */
};

#define OF_MAX_METHODS  32
#define OF_MAX_IHANDLES 64

struct of_ci_context {
	struct of_node *root;
	of_phandle next_phandle;

	struct {
		const char *name;
		of_call_method_fn fn;
		void *opaque;
	} methods[OF_MAX_METHODS];
	int n_methods;

	struct of_node *ihandles[OF_MAX_IHANDLES]; /* ihandle N -> ihandles[N-1] */
	int n_ihandles;

	unsigned unresolved;

	/* S2b/OF-CI early-environment fidelity (SS_M18 trampoline path only):
	 * a per-context /memory reg blob (so the size cell can be set to the real
	 * guest RAMSize without mutating shared static data — the unit test default
	 * stays size=0), and a bump-allocator arena backing the `claim` service. */
	uint8_t reg_memory[8];          /* BE (base, size); pointed-to by /memory reg */
	uint32_t mem_size;              /* RAMSize cell mirrored into reg_memory[4..7] */
	uint32_t claim_next;            /* next free guest-physical addr (bump ptr) */
	uint32_t claim_base;            /* arena floor (for reset/diagnostics) */
	uint32_t claim_limit;           /* arena ceiling (0 == unbounded) */
};

/* ---- node / property construction ------------------------------------ */

static struct of_node *node_new(of_ci_context *ctx, const char *name)
{
	struct of_node *n = (struct of_node *)calloc(1, sizeof(*n));
	n->name = name;
	n->phandle = ctx->next_phandle++;
	return n;
}

static struct of_node *node_add_child(struct of_node *parent, struct of_node *child)
{
	child->parent = parent;
	/* append to keep a stable, source-ordered sibling list (nextprop/peer
	 * walks then see a deterministic order) */
	if (!parent->child) {
		parent->child = child;
	} else {
		struct of_node *s = parent->child;
		while (s->sibling) s = s->sibling;
		s->sibling = child;
	}
	return child;
}

static void node_add_prop(struct of_node *n, const char *name,
                          const void *data, int len, bool nonfinal)
{
	struct of_prop *p = (struct of_prop *)calloc(1, sizeof(*p));
	p->name = name;
	p->data = data;
	p->len = len;
	p->nonfinal = nonfinal;
	/* append (source order) */
	if (!n->props) {
		n->props = p;
	} else {
		struct of_prop *q = n->props;
		while (q->next) q = q->next;
		q->next = p;
	}
}

static void node_add_str(struct of_node *n, const char *name, const char *val)
{
	node_add_prop(n, name, val, (int)strlen(val) + 1, false);
}

/* big-endian reg/cell tuples stored as static byte blobs (host-built once) */
static void node_add_cells(struct of_node *n, const char *name,
                           const void *cells, int len)
{
	node_add_prop(n, name, cells, len, false);
}

/* add a 4-byte big-endian ihandle cell as an owned property (freed in
 * node_free). Used to publish the /chosen instance-handle contract. */
static void node_add_ihandle(struct of_node *n, const char *name, of_ihandle ih)
{
	uint8_t *cell = (uint8_t *)malloc(4);
	cell[0] = (uint8_t)(ih >> 24); cell[1] = (uint8_t)(ih >> 16);
	cell[2] = (uint8_t)(ih >> 8);  cell[3] = (uint8_t)(ih);
	node_add_prop(n, name, cell, 4, true);
}

/* forward decl: opens an instance handle for a DT node (defined below). */
static of_ihandle ihandle_open(of_ci_context *ctx, struct of_node *n);

static void node_free(struct of_node *n)
{
	if (!n) return;
	struct of_prop *p = n->props;
	while (p) {
		struct of_prop *q = p->next;
		if (p->owned) free((void *)p->data);
		if (p->owned_name) free((void *)p->name);
		free(p);
		p = q;
	}
	struct of_node *c = n->child;
	while (c) { struct of_node *s = c->sibling; node_free(c); c = s; }
	free(n);
}

/* ---------------------------------------------------------------------- */
/* Core99 device-tree construction                                        */
/* ---------------------------------------------------------------------- */

/* reg / cell blobs: stored big-endian (DT convention). Built as static arrays
 * so their addresses outlive of_ci_create_core99(). */
static const uint8_t REG_MEMORY[8]   = {0x10,0,0,0, 0,0,0,0}; /* (0x10000000, RAMSize placeholder) */
static const uint8_t REG_ROM[8]      = {0x50,0,0,0, 0,0x50,0,0}; /* (0x50000000, 0x500000) */
static const uint8_t REG_MACIO[8]    = {0xF3,0,0,0, 0,0x08,0,0}; /* (0xF3000000, 0x80000) */
static const uint8_t REG_OPENPIC[4]  = {0xF3,0x04,0,0};         /* (0xF3040000) - size §5 Q6 open */
static const uint8_t REG_ESCC[8]     = {0xF3,0x01,0x20,0, 0,0,0x01,0}; /* (0xF3012000, 0x100) */
static const uint8_t REG_VIACUDA[8]  = {0xF3,0x01,0x60,0, 0,0,0x20,0}; /* (0xF3016000, 0x2000) */
static const uint8_t CELL_ONE[4]     = {0,0,0,1};
static const uint8_t CELL_TWO[4]     = {0,0,0,2};
static const uint8_t CELL_ZERO[4]    = {0,0,0,0};
#define CELL_ZERO_PLACEHOLDER() (CELL_ZERO)

/* interrupt-map / -mask: PROVISIONAL placeholder (Q-S2a.3 NON-FINAL). The cell
 * content is intentionally a do-not-trust filler; only its presence + length is
 * meaningful. The real tuple shape + §5-Q8 input numbers are owed to S2b. */
static const uint8_t IRQMAP_PROVISIONAL[28]  = {0}; /* shape unknown; NON-FINAL */
static const uint8_t IRQMASK_PROVISIONAL[8]  = {0}; /* shape unknown; NON-FINAL */

of_ci_context *of_ci_create_core99(void)
{
	of_ci_context *ctx = (of_ci_context *)calloc(1, sizeof(*ctx));
	ctx->next_phandle = 1; /* 0 reserved as OF_INVALID_PHANDLE */

	/* Per-context /memory reg blob: starts at the static template (base
	 * 0x10000000, size 0) so the standalone unit test sees the historical
	 * default; of_ci_set_memory_size() patches the size cell to the real
	 * RAMSize on the gated trampoline launch path. */
	memcpy(ctx->reg_memory, REG_MEMORY, 8);
	/* claim bump-allocator arena: default floor 16 MiB into guest RAM (above the
	 * loaded MacOS.elf image, which tops out ~0x211000), unbounded by default.
	 * of_ci_set_claim_arena() retargets it from the real RAMSize on launch. */
	ctx->claim_base = ctx->claim_next = 0x01000000u;
	ctx->claim_limit = 0u;

	struct of_node *root = node_new(ctx, "");
	ctx->root = root;
	node_add_str(root, "name", "device-tree");
	node_add_str(root, "device_type", "bootrom");
	node_add_str(root, "model", "Power Macintosh");
	node_add_str(root, "compatible", "MacRISC"); /* PowerMac3,1 list elided */
	node_add_cells(root, "#address-cells", CELL_ONE, 4);

	/* --- OF-standard scaffolding nodes (NOT in CORE99 §3; build obligation) -- */
	struct of_node *chosen = node_add_child(root, node_new(ctx, "chosen"));
	node_add_str(chosen, "name", "chosen");
	node_add_cells(chosen, "bootpath", CELL_ZERO_PLACEHOLDER(), 4);

	struct of_node *aliases = node_add_child(root, node_new(ctx, "aliases"));
	node_add_str(aliases, "name", "aliases");

	struct of_node *options = node_add_child(root, node_new(ctx, "options"));
	node_add_str(options, "name", "options");

	struct of_node *rtas = node_add_child(root, node_new(ctx, "rtas"));
	node_add_str(rtas, "name", "rtas");
	node_add_str(rtas, "device_type", "rtas");

	struct of_node *cpus = node_add_child(root, node_new(ctx, "cpus"));
	node_add_str(cpus, "name", "cpus");
	node_add_cells(cpus, "#address-cells", CELL_ONE, 4);
	struct of_node *cpu0 = node_add_child(cpus, node_new(ctx, "PowerPC,G4@0"));
	node_add_str(cpu0, "name", "PowerPC,G4");
	node_add_str(cpu0, "device_type", "cpu");
	node_add_cells(cpu0, "reg", CELL_ZERO_PLACEHOLDER(), 4);
	struct of_node *l2 = node_add_child(cpu0, node_new(ctx, "l2-cache"));
	node_add_str(l2, "name", "l2-cache");
	node_add_str(l2, "device_type", "cache");
	node_add_cells(l2, "cache-unified", CELL_ONE, 4);
	struct of_node *l2b = node_add_child(l2, node_new(ctx, "l2-cache"));
	node_add_str(l2b, "name", "l2-cache");
	node_add_str(l2b, "device_type", "cache");

	struct of_node *rom = node_add_child(root, node_new(ctx, "rom"));
	node_add_str(rom, "name", "rom");
	struct of_node *macos = node_add_child(rom, node_new(ctx, "macos"));
	node_add_str(macos, "name", "macos");
	node_add_cells(macos, "AAPL,toolbox-parcels", CELL_ZERO_PLACEHOLDER(), 4);

	/* --- CORE99 §3 hardware nodes ----------------------------------------- */
	struct of_node *memory = node_add_child(root, node_new(ctx, "memory@0"));
	node_add_str(memory, "name", "memory");
	node_add_str(memory, "device_type", "memory");
	node_add_cells(memory, "reg", ctx->reg_memory, 8);

	struct of_node *aaplrom = node_add_child(root, node_new(ctx, "AAPL,ROM"));
	node_add_str(aaplrom, "name", "AAPL,ROM");
	node_add_str(aaplrom, "device_type", "rom");
	node_add_cells(aaplrom, "reg", REG_ROM, 8);

	struct of_node *pci = node_add_child(root, node_new(ctx, "pci@f2000000"));
	node_add_str(pci, "name", "pci");
	node_add_str(pci, "device_type", "pci");
	node_add_cells(pci, "#address-cells", CELL_TWO, 4);

	struct of_node *macio = node_add_child(pci, node_new(ctx, "mac-io@c"));
	node_add_str(macio, "name", "mac-io");
	node_add_str(macio, "device_type", "mac-io");
	node_add_str(macio, "compatible", "keylargo");
	node_add_cells(macio, "reg", REG_MACIO, 8);

	struct of_node *pic = node_add_child(macio, node_new(ctx, "interrupt-controller@40000"));
	node_add_str(pic, "name", "interrupt-controller");
	node_add_str(pic, "device_type", "open-pic");
	node_add_str(pic, "compatible", "chrp,open-pic");
	node_add_cells(pic, "reg", REG_OPENPIC, 4);
	node_add_cells(pic, "#interrupt-cells", CELL_TWO, 4);
	/* Q-S2a.3 NON-FINAL provisional rows: query resolves, content is filler. */
	node_add_prop(pic, "interrupt-map", IRQMAP_PROVISIONAL, sizeof(IRQMAP_PROVISIONAL), true);
	node_add_prop(pic, "interrupt-map-mask", IRQMASK_PROVISIONAL, sizeof(IRQMASK_PROVISIONAL), true);

	struct of_node *escc = node_add_child(macio, node_new(ctx, "escc@12000"));
	node_add_str(escc, "name", "escc");
	node_add_str(escc, "device_type", "escc");
	node_add_cells(escc, "reg", REG_ESCC, 8);

	struct of_node *cuda = node_add_child(macio, node_new(ctx, "via-cuda@16000"));
	node_add_str(cuda, "name", "via-cuda");
	node_add_str(cuda, "device_type", "via-cuda");
	node_add_cells(cuda, "reg", REG_VIACUDA, 8);

	struct of_node *nvram = node_add_child(macio, node_new(ctx, "nvram"));
	node_add_str(nvram, "name", "nvram");
	node_add_str(nvram, "device_type", "nvram");

	/* dynamic concrete boot path: .../ata-3@20000/cdrom@0 (QEMU-behavioral) */
	struct of_node *ata = node_add_child(macio, node_new(ctx, "ata-3@20000"));
	node_add_str(ata, "name", "ata-3");
	node_add_str(ata, "device_type", "ata");
	struct of_node *cdrom = node_add_child(ata, node_new(ctx, "cdrom@0"));
	node_add_str(cdrom, "name", "cdrom");
	node_add_str(cdrom, "device_type", "block");
	/* PCI-config-probe over-provision (P-M3): answer the dynamic getprop family */
	node_add_cells(ata, "vendor-id", CELL_ZERO_PLACEHOLDER(), 4);
	node_add_cells(ata, "device-id", CELL_ZERO_PLACEHOLDER(), 4);
	node_add_cells(ata, "class-code", CELL_ZERO_PLACEHOLDER(), 4);

	struct of_node *video = node_add_child(root, node_new(ctx, "display"));
	node_add_str(video, "name", "display");
	node_add_str(video, "device_type", "display");
	node_add_str(video, "display-type", "LCD");
	node_add_cells(video, "screen", CELL_ONE, 4);

	struct of_node *eth = node_add_child(root, node_new(ctx, "ethernet"));
	node_add_str(eth, "name", "ethernet");
	node_add_str(eth, "device_type", "network");

	/* ADV-1 unit-address-sensitivity scaffolding (S2a-impl adversary fix #2):
	 * two same-name siblings differing ONLY by unit address. An addressed query
	 * (disk@1) must resolve the RIGHT sibling; an omit query (disk) the first. */
	struct of_node *scratch = node_add_child(root, node_new(ctx, "scratch"));
	node_add_str(scratch, "name", "scratch");
	struct of_node *disk0 = node_add_child(scratch, node_new(ctx, "disk@0"));
	node_add_str(disk0, "name", "disk");
	struct of_node *disk1 = node_add_child(scratch, node_new(ctx, "disk@1"));
	node_add_str(disk1, "name", "disk");

	/* --- OF /chosen instance-handle contract (S2b memory-map fidelity) -------
	 * The CHRP Trampoline obtains the memory / mmu / console INSTANCE handles
	 * from /chosen (getprop "memory"/"mmu"/"stdin"/"stdout"), then does
	 * instance-to-package on the memory ihandle and getprop "reg" off the
	 * resulting phandle to drive its /memory range-coalesce/relocation loop.
	 * Without these properties getprop(/chosen,"memory")=-1 leaves the producer's
	 * ihandle = 0, so instance-to-package(0)=0 and getprop(0,"reg")=-1; the loop
	 * then reads count = 0xffffffff and walks its output cursor off mapped RAM
	 * (SIGSEGV in MacOS.elf at 0x2026e0). Publish a real /mmu package node + open
	 * memory/mmu/console instances and wire their ihandles into /chosen. */
	struct of_node *mmu = node_add_child(root, node_new(ctx, "mmu"));
	node_add_str(mmu, "name", "mmu");
	node_add_str(mmu, "device_type", "mmu");

	of_ihandle ih_memory  = ihandle_open(ctx, memory);
	of_ihandle ih_mmu     = ihandle_open(ctx, mmu);
	of_ihandle ih_console = ihandle_open(ctx, escc);
	node_add_ihandle(chosen, "memory", ih_memory);
	node_add_ihandle(chosen, "mmu",    ih_mmu);
	node_add_ihandle(chosen, "stdin",  ih_console);
	node_add_ihandle(chosen, "stdout", ih_console);

	return ctx;
}

void of_ci_destroy(of_ci_context *ctx)
{
	if (!ctx) return;
	node_free(ctx->root);
	free(ctx);
}

/* Set the /memory reg SIZE cell (and the mirrored mem_size) to the real guest
 * RAMSize. The base cell (0x10000000 template hi byte) is left as the DT shape;
 * only the low 32-bit size word is written, big-endian. */
void of_ci_set_memory_size(of_ci_context *ctx, uint32_t ram_size)
{
	if (!ctx) return;
	ctx->mem_size = ram_size;
	ctx->reg_memory[4] = (uint8_t)(ram_size >> 24);
	ctx->reg_memory[5] = (uint8_t)(ram_size >> 16);
	ctx->reg_memory[6] = (uint8_t)(ram_size >> 8);
	ctx->reg_memory[7] = (uint8_t)(ram_size);
}

/* Retarget the claim bump-allocator arena. base/limit are guest-physical;
 * limit==0 means unbounded. Honors SS_M18_CLAIM_BASE (hex) as an override. */
void of_ci_set_claim_arena(of_ci_context *ctx, uint32_t base, uint32_t limit)
{
	if (!ctx) return;
	const char *env = getenv("SS_M18_CLAIM_BASE");
	if (env && *env) base = (uint32_t)strtoul(env, NULL, 0);
	ctx->claim_base = ctx->claim_next = base;
	ctx->claim_limit = limit;
}

/* ---------------------------------------------------------------------- */
/* finddevice: component-wise, unit-address-insensitive matching (ADV-1)   */
/* ---------------------------------------------------------------------- */

/* length of the node-name portion (before '@'), 0 if it starts with '@' */
static size_t name_base_len(const char *s)
{
	const char *at = strchr(s, '@');
	return at ? (size_t)(at - s) : strlen(s);
}

static const char *unit_addr(const char *s)
{
	const char *at = strchr(s, '@');
	return at ? at + 1 : "";
}

/* does canonical node `node_name` match path component `comp`? */
static bool comp_match(const char *node_name, const char *comp, size_t comp_len)
{
	/* component may be "mac-io", "mac-io@c", or "@0" (unit-only). */
	size_t cbase = 0;
	{
		const char *at = (const char *)memchr(comp, '@', comp_len);
		cbase = at ? (size_t)(at - comp) : comp_len;
	}
	if (cbase == 0) {
		/* unit-address-only component: match by unit address */
		const char *cu = "";
		size_t culen = 0;
		const char *at = (const char *)memchr(comp, '@', comp_len);
		if (at) { cu = at + 1; culen = comp_len - (cbase + 1); }
		const char *nu = unit_addr(node_name);
		return strlen(nu) == culen && strncmp(nu, cu, culen) == 0;
	}
	size_t nbase = name_base_len(node_name);
	if (!(nbase == cbase && strncmp(node_name, comp, cbase) == 0))
		return false;
	/* ADV-1: the base name matches. If the QUERY carries a unit address, the
	 * node's unit address must also match (unit-address-INSENSITIVE applies
	 * ONLY when the query OMITS the address — handled by the cbase==0 / no-'@'
	 * paths). Without this, mac-io@99 would wrongly match node mac-io@c. */
	const char *cat = (const char *)memchr(comp, '@', comp_len);
	if (cat) {
		const char *cu = cat + 1;
		size_t culen = comp_len - (cbase + 1);
		const char *nu = unit_addr(node_name);
		return strlen(nu) == culen && strncmp(nu, cu, culen) == 0;
	}
	return true;
}

of_phandle of_dt_finddevice(of_ci_context *ctx, const char *path)
{
	if (!ctx || !path || path[0] != '/')
		return OF_INVALID_PHANDLE;
	struct of_node *node = ctx->root;
	const char *p = path + 1;
	while (*p) {
		const char *start = p;
		while (*p && *p != '/') p++;
		size_t clen = (size_t)(p - start);
		if (clen == 0) { /* trailing or doubled '/' */
			if (*p == '/') p++;
			continue;
		}
		struct of_node *c = node->child, *found = NULL;
		for (; c; c = c->sibling) {
			if (comp_match(c->name, start, clen)) { found = c; break; }
		}
		if (!found)
			return OF_INVALID_PHANDLE;
		node = found;
		if (*p == '/') p++;
	}
	return node->phandle;
}

static struct of_node *node_by_phandle(struct of_node *n, of_phandle ph)
{
	if (!n) return NULL;
	if (n->phandle == ph) return n;
	for (struct of_node *c = n->child; c; c = c->sibling) {
		struct of_node *r = node_by_phandle(c, ph);
		if (r) return r;
	}
	return NULL;
}

static struct of_prop *prop_find(struct of_node *n, const char *name)
{
	for (struct of_prop *p = n->props; p; p = p->next)
		if (strcmp(p->name, name) == 0) return p;
	return NULL;
}

int of_dt_getproplen(of_ci_context *ctx, of_phandle ph, const char *name)
{
	struct of_node *n = node_by_phandle(ctx->root, ph);
	if (!n) return -1;
	struct of_prop *p = prop_find(n, name);
	return p ? p->len : -1;
}

bool of_dt_prop_is_nonfinal(of_ci_context *ctx, of_phandle ph, const char *name)
{
	struct of_node *n = node_by_phandle(ctx->root, ph);
	if (!n) return false;
	struct of_prop *p = prop_find(n, name);
	return p && p->nonfinal;
}

/* ---------------------------------------------------------------------- */
/* call-method backend table + ihandle table                              */
/* ---------------------------------------------------------------------- */

bool of_ci_register_method(of_ci_context *ctx, const char *method,
                           of_call_method_fn fn, void *opaque)
{
	if (ctx->n_methods >= OF_MAX_METHODS) return false;
	ctx->methods[ctx->n_methods].name = method;
	ctx->methods[ctx->n_methods].fn = fn;
	ctx->methods[ctx->n_methods].opaque = opaque;
	ctx->n_methods++;
	return true;
}

static of_ihandle ihandle_open(of_ci_context *ctx, struct of_node *n)
{
	if (ctx->n_ihandles >= OF_MAX_IHANDLES) return OF_INVALID_IHANDLE;
	ctx->ihandles[ctx->n_ihandles++] = n;
	return (of_ihandle)ctx->n_ihandles; /* 1-based */
}

/* ---------------------------------------------------------------------- */
/* the OF-CI dispatcher                                                    */
/* ---------------------------------------------------------------------- */

/* The 21 statically-enumerated direct services (FINDINGS-s2a-ofci-dt.md). */
static const char *const DIRECT_SERVICES[] = {
	"getprop", "getproplen", "finddevice", "close", "setprop", "open",
	"seek", "parent", "exit", "read", "package-to-path",
	"instance-to-package", "write", "instance-to-path", "peer", "test",
	"canon", "quiesce", "nextprop", "child", "claim",
};
static const int N_DIRECT_SERVICES =
	(int)(sizeof(DIRECT_SERVICES) / sizeof(DIRECT_SERVICES[0]));

static bool is_direct_service(const char *name)
{
	for (int i = 0; i < N_DIRECT_SERVICES; i++)
		if (strcmp(name, DIRECT_SERVICES[i]) == 0) return true;
	return false;
}

/* S2b OWING (adversary Finding 5, pre-wiring): this cell model is host-native —
 * of_cell is a 64-bit host value and cell_str/cell_ptr reinterpret a cell as a
 * raw HOST pointer (the unit test passes host const char* / buffers). The real
 * MacOS.elf producer writes 32-bit BIG-ENDIAN cells at [r2-0xc] carrying
 * GUEST-PHYSICAL string/buffer pointers. Before S2b wires the real producer,
 * a 32-bit-BE-cell decode + guest->host pointer translation shim MUST replace
 * these two casts (and the of_cell width / array packing in of_ci_callback). */
static inline const char *cell_str(of_cell c) { return (const char *)(uintptr_t)c; }
static inline void *cell_ptr(of_cell c)       { return (void *)(uintptr_t)c; }

/* nextprop: write the name AFTER `prev` (NULL/"" -> first) into `buf`.
 * returns 1 (next exists), 0 (no more / `prev` was the last), -1 (bad node OR
 * `prev` is a non-empty name that is not a property of the node — IEEE-1275
 * "invalid previous"). */
static int do_nextprop(of_ci_context *ctx, of_phandle ph,
                       const char *prev, char *buf)
{
	struct of_node *n = node_by_phandle(ctx->root, ph);
	if (!n) return -1;
	struct of_prop *p = n->props;
	if (prev && prev[0]) {
		bool matched = false;
		for (; p; p = p->next)
			if (strcmp(p->name, prev) == 0) { p = p->next; matched = true; break; }
		if (!matched) { if (buf) buf[0] = '\0'; return -1; } /* unknown previous */
	}
	if (!p) { if (buf) buf[0] = '\0'; return 0; }
	if (buf) strcpy(buf, p->name);
	return 1;
}

static int of_ci_dispatch(of_ci_context *ctx, of_cell *array)
{
	if (!ctx || !array) return OF_CI_FAIL;

	const char *service = cell_str(array[0]);
	int n_args = (int)array[1];
	int n_rets = (int)array[2];
	of_cell *args = &array[3];
	of_cell *rets = &array[3 + n_args];

	if (!service) { ctx->unresolved++; return OF_CI_FAIL; }

	/* ---- call-method: resolve the method name against an injected double -- */
	if (strcmp(service, "call-method") == 0) {
		const char *method = (n_args >= 1) ? cell_str(args[0]) : NULL;
		of_ihandle ih = (n_args >= 2) ? (of_ihandle)args[1] : OF_INVALID_IHANDLE;
		if (!method) { ctx->unresolved++; return OF_CI_FAIL; }
		for (int i = 0; i < ctx->n_methods; i++) {
			if (strcmp(ctx->methods[i].name, method) == 0) {
				int n_in = (n_args >= 2) ? n_args - 2 : 0;
				int n_out = n_rets;
				int catch_code = ctx->methods[i].fn(
					ctx->methods[i].opaque, method, ih,
					&args[2], n_in, rets, n_out);
				if (n_rets >= 1) rets[0] = (of_cell)catch_code;
				return OF_CI_OK;
			}
		}
		/* method name not backed -> UNRESOLVED (anti-vacuity: a missing
		 * double FAILS the gate by construction). */
		ctx->unresolved++;
		return OF_CI_FAIL;
	}

	/* ---- interpret: resolve the forth literal ---------------------------- */
	if (strcmp(service, "interpret") == 0) {
		const char *forth = (n_args >= 1) ? cell_str(args[0]) : NULL;
		if (forth && (strcmp(forth, "key?") == 0 ||
		              strcmp(forth, "key") == 0 ||
		              strcmp(forth, "reset-all") == 0)) {
			if (n_rets >= 1) rets[0] = 0; /* catch-result OK */
			return OF_CI_OK;
		}
		ctx->unresolved++;
		return OF_CI_FAIL;
	}

	/* ---- direct services ------------------------------------------------- */
	if (!is_direct_service(service)) {
		ctx->unresolved++;
		return OF_CI_FAIL;
	}

	if (strcmp(service, "finddevice") == 0) {
		of_phandle ph = of_dt_finddevice(ctx, cell_str(args[0]));
		if (n_rets >= 1) rets[0] = (ph == OF_INVALID_PHANDLE) ? (of_cell)-1 : ph;
	} else if (strcmp(service, "getprop") == 0) {
		of_phandle ph = (of_phandle)args[0];
		const char *name = cell_str(args[1]);
		void *buf = cell_ptr(args[2]);
		int buflen = (int)args[3];
		struct of_node *n = node_by_phandle(ctx->root, ph);
		struct of_prop *p = n ? prop_find(n, name) : NULL;
		int len = -1;
		if (p) {
			len = p->len;
			if (buf && buflen > 0) {
				int cp = (len < buflen) ? len : buflen;
				memcpy(buf, p->data, cp);
			}
		}
		if (n_rets >= 1) rets[0] = (of_cell)len;
	} else if (strcmp(service, "setprop") == 0) {
		/* Adversary fix #1: setprop MUST mutate the node (the Trampoline writes
		 * AAPL,toolbox-parcels / AAPL,reserved-memory-space|-io-space). The model
		 * stores const void* blobs, so store an OWNED malloc'd copy that lives for
		 * the DT's lifetime (freed in node_free). Returns the new length. */
		of_phandle ph = (of_phandle)args[0];
		const char *name = cell_str(args[1]);
		const void *src = cell_ptr(args[2]);
		int len = (int)args[3];
		struct of_node *n = node_by_phandle(ctx->root, ph);
		int result = -1;
		if (n && name) {
			void *copy = NULL;
			if (len > 0 && src) { copy = malloc((size_t)len); memcpy(copy, src, (size_t)len); }
			struct of_prop *p = prop_find(n, name);
			if (p) {
				if (p->owned) free((void *)p->data);
				p->data = copy;
				p->len = len;
				p->owned = (copy != NULL);
			} else {
				char *namecopy = strdup(name);
				node_add_prop(n, namecopy, copy, len, false);
				struct of_prop *np = prop_find(n, namecopy);
				np->owned = (copy != NULL);
				np->owned_name = true;
			}
			result = len;
		}
		if (n_rets >= 1) rets[0] = (of_cell)result;
	} else if (strcmp(service, "getproplen") == 0) {
		int len = of_dt_getproplen(ctx, (of_phandle)args[0], cell_str(args[1]));
		if (n_rets >= 1) rets[0] = (of_cell)len;
	} else if (strcmp(service, "nextprop") == 0) {
		int r = do_nextprop(ctx, (of_phandle)args[0], cell_str(args[1]),
		                    (char *)cell_ptr(args[2]));
		if (n_rets >= 1) rets[0] = (of_cell)r;
	} else if (strcmp(service, "peer") == 0) {
		struct of_node *n = node_by_phandle(ctx->root, (of_phandle)args[0]);
		of_phandle r = OF_INVALID_PHANDLE;
		if ((of_phandle)args[0] == OF_INVALID_PHANDLE)
			r = ctx->root->phandle;       /* peer(0) == root */
		else if (n && n->sibling)
			r = n->sibling->phandle;
		if (n_rets >= 1) rets[0] = r;
	} else if (strcmp(service, "child") == 0) {
		struct of_node *n = node_by_phandle(ctx->root, (of_phandle)args[0]);
		of_phandle r = (n && n->child) ? n->child->phandle : OF_INVALID_PHANDLE;
		if (n_rets >= 1) rets[0] = r;
	} else if (strcmp(service, "parent") == 0) {
		struct of_node *n = node_by_phandle(ctx->root, (of_phandle)args[0]);
		of_phandle r = (n && n->parent) ? n->parent->phandle : OF_INVALID_PHANDLE;
		if (n_rets >= 1) rets[0] = r;
	} else if (strcmp(service, "open") == 0) {
		of_phandle ph = of_dt_finddevice(ctx, cell_str(args[0]));
		struct of_node *n = node_by_phandle(ctx->root, ph);
		of_ihandle ih = n ? ihandle_open(ctx, n) : OF_INVALID_IHANDLE;
		if (n_rets >= 1) rets[0] = ih;
	} else if (strcmp(service, "instance-to-package") == 0) {
		of_ihandle ih = (of_ihandle)args[0];
		of_phandle r = OF_INVALID_PHANDLE;
		if (ih >= 1 && (int)ih <= ctx->n_ihandles && ctx->ihandles[ih - 1])
			r = ctx->ihandles[ih - 1]->phandle;
		if (n_rets >= 1) rets[0] = r;
	} else if (strcmp(service, "canon") == 0 ||
	           strcmp(service, "package-to-path") == 0 ||
	           strcmp(service, "instance-to-path") == 0) {
		/* normalize / stringify a path: minimal — echo arg back when a buffer
		 * is supplied. Resolution (service exists) is what the gate checks.
		 * S2b OWING (adversary Finding 3): canon is a no-op and /aliases is empty
		 * — consistent with binding ADV-1 (finddevice does component matching,
		 * NOT alias lookup). S2b MUST confirm against the real trace whether the
		 * producer consumes the canon OUTPUT BUFFER or a /aliases getprop; if so,
		 * canon must write a real canonical path and /aliases must be populated. */
		if (n_rets >= 1) rets[0] = 0;
	} else if (strcmp(service, "claim") == 0) {
		/* IEEE-1275 claim(virt, size, align) -> allocated base.
		 *  - align == 0: allocate at the exact `virt` (caller-chosen address);
		 *    honor it verbatim (this is how the OF caller pins a fixed region).
		 *  - align != 0: ignore `virt`, hand out `size` bytes from the bump arena
		 *    aligned up to max(align, page). Page-round the advance so successive
		 *    claims never overlap.
		 * This replaces the S2a stub return of 0, which the Trampoline consumed as
		 * "allocated at address 0" and then relocated/jumped into the zero gap. */
		uint32_t virt  = (uint32_t)args[0];
		uint32_t size  = (uint32_t)args[1];
		uint32_t align = (uint32_t)args[2];
		uint32_t result;
		if (align == 0) {
			result = virt;                       /* fixed-address claim */
		} else {
			uint32_t a = (align < 0x1000u) ? 0x1000u : align;
			uint32_t base = (ctx->claim_next + (a - 1)) & ~(a - 1);
			uint32_t adv  = (size + 0xfffu) & ~0xfffu;
			result = base;
			ctx->claim_next = base + adv;
		}
		if (n_rets >= 1) rets[0] = (of_cell)result;
	} else {
		/* close, seek, exit, read, write, test, quiesce:
		 * benign accepted handlers (seam exists; real semantics out of S2a
		 * scope). They RESOLVE, so the gate stays satisfied. */
		if (n_rets >= 1) rets[0] = 0;
	}
	return OF_CI_OK;
}

/* Public entry: dispatch + an env-gated per-call trace (SS_M18_OFCI_TRACE).
 * The trace prints the resolved service, its raw input cells, and the first
 * return cell — the diagnostic ladder for the early CHRP boot-setup sequence
 * (finddevice/getprop/claim) before the /mmu phase. Default OFF = silent. */
int of_ci_callback(of_ci_context *ctx, of_cell *array)
{
	static int trace = -1;
	if (trace < 0) {
		const char *e = getenv("SS_M18_OFCI_TRACE");
		trace = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
	}
	if (!trace || !ctx || !array)
		return of_ci_dispatch(ctx, array);

	const char *service = cell_str(array[0]);
	int n_args = (int)array[1];
	int n_rets = (int)array[2];
	of_cell *args = &array[3];
	of_cell *rets = &array[3 + n_args];

	int rc = of_ci_dispatch(ctx, array);

	char abuf[160]; size_t off = 0; abuf[0] = '\0';
	for (int i = 0; i < n_args && i < 6; i++) {
		int w = snprintf(abuf + off, sizeof(abuf) - off,
		                 "%s0x%llx", i ? "," : "",
		                 (unsigned long long)args[i]);
		if (w < 0 || (size_t)w >= sizeof(abuf) - off) break;
		off += (size_t)w;
	}
	const char *detail = "";
	char dbuf[80];
	if (service && strcmp(service, "call-method") == 0 && n_args >= 1) {
		snprintf(dbuf, sizeof(dbuf), " method=%s", cell_str(args[0]) ? cell_str(args[0]) : "?");
		detail = dbuf;
	}
	fprintf(stderr, "[OFCI-TRACE] service=%s%s nargs=%d args=[%s] -> ret=0x%llx (rc=%d)\n",
	        service ? service : "(null)", detail, n_args, abuf,
	        (unsigned long long)(n_rets >= 1 ? rets[0] : 0), rc);
	return rc;
}

unsigned of_ci_unresolved_count(const of_ci_context *ctx)
{
	return ctx ? ctx->unresolved : 0;
}
