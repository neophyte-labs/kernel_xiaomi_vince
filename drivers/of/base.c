/*
 * Procedures for creating, accessing and interpreting the device tree.
 *
 * Paul Mackerras	August 1996.
 * Copyright (C) 1996-2005 Paul Mackerras.
 *
 *  Adapted for 64bit PowerPC by Dave Engebretsen and Peter Bergner.
 *    {engebret|bergner}@us.ibm.com
 *
 *  Adapted for sparc and sparc64 by David S. Miller davem@davemloft.net
 *
 *  Reconsolidated from arch/x/kernel/prom.c by Stephen Rothwell and
 *  Grant Likely.
 *
 *      This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#define pr_fmt(fmt)	"OF: " fmt

#include <linux/bootmem.h>
#include <linux/console.h>
#include <linux/ctype.h>
#include <linux/cpu.h>
#include <linux/memblock.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/proc_fs.h>

#include "of_private.h"

LIST_HEAD(aliases_lookup);

struct device_node *of_root;
EXPORT_SYMBOL(of_root);
struct device_node *of_chosen;
struct device_node *of_aliases;
struct device_node *of_stdout;
static const char *of_stdout_options;

struct kset *of_kset;

/*
 * Used to protect the of_aliases, to hold off addition of nodes to sysfs.
 * This mutex must be held whenever modifications are being made to the
 * device tree. The of_{attach,detach}_node() and
 * of_{add,remove,update}_property() helpers make sure this happens.
 */
DEFINE_MUTEX(of_mutex);

/* use when traversing tree through the child, sibling,
 * or parent members of struct device_node.
 */
DEFINE_RAW_SPINLOCK(devtree_lock);

int of_n_addr_cells(struct device_node *np)
{
	const __be32 *ip;

	do {
		if (np->parent)
			np = np->parent;
		ip = of_get_property(np, "#address-cells", NULL);
		if (ip)
			return be32_to_cpup(ip);
	} while (np->parent);
	/* No #address-cells property for the root node */
	return OF_ROOT_NODE_ADDR_CELLS_DEFAULT;
}
EXPORT_SYMBOL(of_n_addr_cells);

int of_n_size_cells(struct device_node *np)
{
	const __be32 *ip;

	do {
		if (np->parent)
			np = np->parent;
		ip = of_get_property(np, "#size-cells", NULL);
		if (ip)
			return be32_to_cpup(ip);
	} while (np->parent);
	/* No #size-cells property for the root node */
	return OF_ROOT_NODE_SIZE_CELLS_DEFAULT;
}
EXPORT_SYMBOL(of_n_size_cells);

#ifdef CONFIG_NUMA
int __weak of_node_to_nid(struct device_node *np)
{
	return NUMA_NO_NODE;
}
#endif

static struct device_node **phandle_cache;
static u32 phandle_cache_mask;

/*
 * Assumptions behind phandle_cache implementation:
 *   - phandle property values are in a contiguous range of 1..n
 *
 * If the assumptions do not hold, then
 *   - the phandle lookup overhead reduction provided by the cache
 *     will likely be less
 */
static void of_populate_phandle_cache(void)
{
	unsigned long flags;
	u32 cache_entries;
	struct device_node *np;
	u32 phandles = 0;

	raw_spin_lock_irqsave(&devtree_lock, flags);

	kfree(phandle_cache);
	phandle_cache = NULL;

	for_each_of_allnodes(np)
		if (np->phandle && np->phandle != OF_PHANDLE_ILLEGAL)
			phandles++;

	cache_entries = roundup_pow_of_two(phandles);
	phandle_cache_mask = cache_entries - 1;

	phandle_cache = kcalloc(cache_entries, sizeof(*phandle_cache),
				GFP_ATOMIC);

	if (phandle_cache)
		for_each_of_allnodes(np)
			if (np->phandle && np->phandle != OF_PHANDLE_ILLEGAL)
				phandle_cache[np->phandle & phandle_cache_mask] = np;

	raw_spin_unlock_irqrestore(&devtree_lock, flags);
}

void __init of_populate_phandle_cache_early(void)
{
	u32 cache_entries;
	struct device_node *np;
	u32 phandles = 0;
	size_t size;

	for_each_of_allnodes(np)
		if (np->phandle && np->phandle != OF_PHANDLE_ILLEGAL)
			phandles++;

	cache_entries = roundup_pow_of_two(phandles);
	phandle_cache_mask = cache_entries - 1;

	size = cache_entries * sizeof(*phandle_cache);
	phandle_cache = memblock_virt_alloc(size, 4);
	memset(phandle_cache, 0, size);

	for_each_of_allnodes(np)
		if (np->phandle && np->phandle != OF_PHANDLE_ILLEGAL)
			phandle_cache[np->phandle & phandle_cache_mask] = np;
}

#ifndef CONFIG_MODULES
static int __init of_free_phandle_cache(void)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);

	kfree(phandle_cache);
	phandle_cache = NULL;

	raw_spin_unlock_irqrestore(&devtree_lock, flags);

	return 0;
}
late_initcall_sync(of_free_phandle_cache);
#endif

void __init of_core_init(void)
{
	unsigned long flags;
	struct device_node *np;
	phys_addr_t size;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	size = (phandle_cache_mask + 1) * sizeof(*phandle_cache);
	memblock_free(__pa(phandle_cache), size);
	phandle_cache = NULL;
	raw_spin_unlock_irqrestore(&devtree_lock, flags);

	of_populate_phandle_cache();

	/* Create the kset, and register existing nodes */
	mutex_lock(&of_mutex);
	of_kset = kset_create_and_add("devicetree", NULL, firmware_kobj);
	if (!of_kset) {
		mutex_unlock(&of_mutex);
		pr_err("failed to register existing nodes\n");
		return;
	}
	for_each_of_allnodes(np)
		__of_attach_node_sysfs(np);
	mutex_unlock(&of_mutex);

	/* Symlink in /proc as required by userspace ABI */
	if (of_root)
		proc_symlink("device-tree", NULL, "/sys/firmware/devicetree/base");
}

static struct property *__of_find_property(const struct device_node *np,
					   const char *name, int *lenp)
{
	struct property *pp;

	if (!np)
		return NULL;

	for (pp = np->properties; pp; pp = pp->next) {
		if (of_prop_cmp(pp->name, name) == 0) {
			if (lenp)
				*lenp = pp->length;
			break;
		}
	}

	return pp;
}

struct property *of_find_property(const struct device_node *np,
				  const char *name,
				  int *lenp)
{
	struct property *pp;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	pp = __of_find_property(np, name, lenp);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);

	return pp;
}
EXPORT_SYMBOL(of_find_property);

struct device_node *__of_find_all_nodes(struct device_node *prev)
{
	struct device_node *np;
	if (!prev) {
		np = of_root;
	} else if (prev->child) {
		np = prev->child;
	} else {
		/* Walk back up looking for a sibling, or the end of the structure */
		np = prev;
		while (np->parent && !np->sibling)
			np = np->parent;
		np = np->sibling; /* Might be null at the end of the tree */
	}
	return np;
}

/**
 * of_find_all_nodes - Get next node in global list
 * @prev:	Previous node or NULL to start iteration
 *		of_node_put() will be called on it
 *
 * Returns a node pointer with refcount incremented, use
 * of_node_put() on it when done.
 */
struct device_node *of_find_all_nodes(struct device_node *prev)
{
	struct device_node *np;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	np = __of_find_all_nodes(prev);
	of_node_get(np);
	of_node_put(prev);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return np;
}
EXPORT_SYMBOL(of_find_all_nodes);

/*
 * Find a property with a given name for a given node
 * and return the value.
 */
const void *__of_get_property(const struct device_node *np,
			      const char *name, int *lenp)
{
	struct property *pp = __of_find_property(np, name, lenp);

	return pp ? pp->value : NULL;
}

/*
 * Find a property with a given name for a given node
 * and return the value.
 */
const void *of_get_property(const struct device_node *np, const char *name,
			    int *lenp)
{
	struct property *pp = of_find_property(np, name, lenp);

	return pp ? pp->value : NULL;
}
EXPORT_SYMBOL(of_get_property);

/*
 * arch_match_cpu_phys_id - Match the given logical CPU and physical id
 *
 * @cpu: logical cpu index of a core/thread
 * @phys_id: physical identifier of a core/thread
 *
 * CPU logical to physical index mapping is architecture specific.
 * However this __weak function provides a default match of physical
 * id to logical cpu index. phys_id provided here is usually values read
 * from the device tree which must match the hardware internal registers.
 *
 * Returns true if the physical identifier and the logical cpu index
 * correspond to the same core/thread, false otherwise.
 */
bool __weak arch_match_cpu_phys_id(int cpu, u64 phys_id)
{
	return (u32)phys_id == cpu;
}

/**
 * Checks if the given "prop_name" property holds the physical id of the
 * core/thread corresponding to the logical cpu 'cpu'. If 'thread' is not
 * NULL, local thread number within the core is returned in it.
 */
static bool __of_find_n_match_cpu_property(struct device_node *cpun,
			const char *prop_name, int cpu, unsigned int *thread)
{
	const __be32 *cell;
	int ac, prop_len, tid;
	u64 hwid;

	ac = of_n_addr_cells(cpun);
	cell = of_get_property(cpun, prop_name, &prop_len);
	if (!cell || !ac)
		return false;
	prop_len /= sizeof(*cell) * ac;
	for (tid = 0; tid < prop_len; tid++) {
		hwid = of_read_number(cell, ac);
		if (arch_match_cpu_phys_id(cpu, hwid)) {
			if (thread)
				*thread = tid;
			return true;
		}
		cell += ac;
	}
	return false;
}

/*
 * arch_find_n_match_cpu_physical_id - See if the given device node is
 * for the cpu corresponding to logical cpu 'cpu'.  Return true if so,
 * else false.  If 'thread' is non-NULL, the local thread number within the
 * core is returned in it.
 */
bool __weak arch_find_n_match_cpu_physical_id(struct device_node *cpun,
					      int cpu, unsigned int *thread)
{
	/* Check for non-standard "ibm,ppc-interrupt-server#s" property
	 * for thread ids on PowerPC. If it doesn't exist fallback to
	 * standard "reg" property.
	 */
	if (IS_ENABLED(CONFIG_PPC) &&
	    __of_find_n_match_cpu_property(cpun,
					   "ibm,ppc-interrupt-server#s",
					   cpu, thread))
		return true;

	return __of_find_n_match_cpu_property(cpun, "reg", cpu, thread);
}

/**
 * of_get_cpu_node - Get device node associated with the given logical CPU
 *
 * @cpu: CPU number(logical index) for which device node is required
 * @thread: if not NULL, local thread number within the physical core is
 *          returned
 *
 * The main purpose of this function is to retrieve the device node for the
 * given logical CPU index. It should be used to initialize the of_node in
 * cpu device. Once of_node in cpu device is populated, all the further
 * references can use that instead.
 *
 * CPU logical to physical index mapping is architecture specific and is built
 * before booting secondary cores. This function uses arch_match_cpu_phys_id
 * which can be overridden by architecture specific implementation.
 *
 * Returns a node pointer for the logical cpu with refcount incremented, use
 * of_node_put() on it when done. Returns NULL if not found.
 */
struct device_node *of_get_cpu_node(int cpu, unsigned int *thread)
{
	struct device_node *cpun;

	for_each_node_by_type(cpun, "cpu") {
		if (arch_find_n_match_cpu_physical_id(cpun, cpu, thread))
			return cpun;
	}
	return NULL;
}
EXPORT_SYMBOL(of_get_cpu_node);

/**
 * __of_device_is_compatible() - Check if the node matches given constraints
 * @device: pointer to node
 * @compat: required compatible string, NULL or "" for any match
 * @type: required device_type value, NULL or "" for any match
 * @name: required node name, NULL or "" for any match
 *
 * Checks if the given @compat, @type and @name strings match the
 * properties of the given @device. A constraints can be skipped by
 * passing NULL or an empty string as the constraint.
 *
 * Returns 0 for no match, and a positive integer on match. The return
 * value is a relative score with larger values indicating better
 * matches. The score is weighted for the most specific compatible value
 * to get the highest score. Matching type is next, followed by matching
 * name. Practically speaking, this results in the following priority
 * order for matches:
 *
 * 1. specific compatible && type && name
 * 2. specific compatible && type
 * 3. specific compatible && name
 * 4. specific compatible
 * 5. general compatible && type && name
 * 6. general compatible && type
 * 7. general compatible && name
 * 8. general compatible
 * 9. type && name
 * 10. type
 * 11. name
 */
static int __of_device_is_compatible(const struct device_node *device,
				     const char *compat, const char *type, const char *name)
{
	struct property *prop;
	const char *cp;
	int index = 0, score = 0;

	/* Compatible match has highest priority */
	if (compat && compat[0]) {
		prop = __of_find_property(device, "compatible", NULL);
		for (cp = of_prop_next_string(prop, NULL); cp;
		     cp = of_prop_next_string(prop, cp), index++) {
			if (of_compat_cmp(cp, compat, strlen(compat)) == 0) {
				score = INT_MAX/2 - (index << 2);
				break;
			}
		}
		if (!score)
			return 0;
	}

	/* Matching type is better than matching name */
	if (type && type[0]) {
		if (!device->type || of_node_cmp(type, device->type))
			return 0;
		score += 2;
	}

	/* Matching name is a bit better than not */
	if (name && name[0]) {
		if (!device->name || of_node_cmp(name, device->name))
			return 0;
		score++;
	}

	return score;
}

/** Checks if the given "compat" string matches one of the strings in
 * the device's "compatible" property
 */
int of_device_is_compatible(const struct device_node *device,
		const char *compat)
{
	unsigned long flags;
	int res;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	res = __of_device_is_compatible(device, compat, NULL, NULL);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return res;
}
EXPORT_SYMBOL(of_device_is_compatible);

/** Checks if the device is compatible with any of the entries in
 *  a NULL terminated array of strings. Returns the best match
 *  score or 0.
 */
int of_device_compatible_match(struct device_node *device,
			       const char *const *compat)
{
	unsigned int tmp, score = 0;

	if (!compat)
		return 0;

	while (*compat) {
		tmp = of_device_is_compatible(device, *compat);
		if (tmp > score)
			score = tmp;
		compat++;
	}

	return score;
}

/**
 * of_machine_is_compatible - Test root of device tree for a given compatible value
 * @compat: compatible string to look for in root node's compatible property.
 *
 * Returns a positive integer if the root node has the given value in its
 * compatible property.
 */
int of_machine_is_compatible(const char *compat)
{
	struct device_node *root;
	int rc = 0;

	root = of_find_node_by_path("/");
	if (root) {
		rc = of_device_is_compatible(root, compat);
		of_node_put(root);
	}
	return rc;
}
EXPORT_SYMBOL(of_machine_is_compatible);

/**
 *  __of_device_is_available - check if a device is available for use
 *
 *  @device: Node to check for availability, with locks already held
 *
 *  Returns true if the status property is absent or set to "okay" or "ok",
 *  false otherwise
 */
static bool __of_device_is_available(const struct device_node *device)
{
	const char *status;
	int statlen;

	if (!device)
		return false;

	status = __of_get_property(device, "status", &statlen);
	if (status == NULL)
		return true;

	if (statlen > 0) {
		if (!strcmp(status, "okay") || !strcmp(status, "ok"))
			return true;
	}

	return false;
}

/**
 *  of_device_is_available - check if a device is available for use
 *
 *  @device: Node to check for availability
 *
 *  Returns true if the status property is absent or set to "okay" or "ok",
 *  false otherwise
 */
bool of_device_is_available(const struct device_node *device)
{
	unsigned long flags;
	bool res;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	res = __of_device_is_available(device);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return res;

}
EXPORT_SYMBOL(of_device_is_available);

/**
 *  of_device_is_big_endian - check if a device has BE registers
 *
 *  @device: Node to check for endianness
 *
 *  Returns true if the device has a "big-endian" property, or if the kernel
 *  was compiled for BE *and* the device has a "native-endian" property.
 *  Returns false otherwise.
 *
 *  Callers would nominally use ioread32be/iowrite32be if
 *  of_device_is_big_endian() == true, or readl/writel otherwise.
 */
bool of_device_is_big_endian(const struct device_node *device)
{
	if (of_property_read_bool(device, "big-endian"))
		return true;
	if (IS_ENABLED(CONFIG_CPU_BIG_ENDIAN) &&
	    of_property_read_bool(device, "native-endian"))
		return true;
	return false;
}
EXPORT_SYMBOL(of_device_is_big_endian);

/**
 *	of_get_parent - Get a node's parent if any
 *	@node:	Node to get parent
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_get_parent(const struct device_node *node)
{
	struct device_node *np;
	unsigned long flags;

	if (!node)
		return NULL;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	np = of_node_get(node->parent);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return np;
}
EXPORT_SYMBOL(of_get_parent);

/**
 *	of_get_next_parent - Iterate to a node's parent
 *	@node:	Node to get parent of
 *
 *	This is like of_get_parent() except that it drops the
 *	refcount on the passed node, making it suitable for iterating
 *	through a node's parents.
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_get_next_parent(struct device_node *node)
{
	struct device_node *parent;
	unsigned long flags;

	if (!node)
		return NULL;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	parent = of_node_get(node->parent);
	of_node_put(node);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return parent;
}
EXPORT_SYMBOL(of_get_next_parent);

static struct device_node *__of_get_next_child(const struct device_node *node,
						struct device_node *prev)
{
	struct device_node *next;

	if (!node)
		return NULL;

	next = prev ? prev->sibling : node->child;
	for (; next; next = next->sibling)
		if (of_node_get(next))
			break;
	of_node_put(prev);
	return next;
}
#define __for_each_child_of_node(parent, child) \
	for (child = __of_get_next_child(parent, NULL); child != NULL; \
	     child = __of_get_next_child(parent, child))

/**
 *	of_get_next_child - Iterate a node childs
 *	@node:	parent node
 *	@prev:	previous child of the parent node, or NULL to get first
 *
 *	Returns a node pointer with refcount incremented, use of_node_put() on
 *	it when done. Returns NULL when prev is the last child. Decrements the
 *	refcount of prev.
 */
struct device_node *of_get_next_child(const struct device_node *node,
	struct device_node *prev)
{
	struct device_node *next;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	next = __of_get_next_child(node, prev);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return next;
}
EXPORT_SYMBOL(of_get_next_child);

/**
 *	of_get_next_available_child - Find the next available child node
 *	@node:	parent node
 *	@prev:	previous child of the parent node, or NULL to get first
 *
 *      This function is like of_get_next_child(), except that it
 *      automatically skips any disabled nodes (i.e. status = "disabled").
 */
struct device_node *of_get_next_available_child(const struct device_node *node,
	struct device_node *prev)
{
	struct device_node *next;
	unsigned long flags;

	if (!node)
		return NULL;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	next = prev ? prev->sibling : node->child;
	for (; next; next = next->sibling) {
		if (!__of_device_is_available(next))
			continue;
		if (of_node_get(next))
			break;
	}
	of_node_put(prev);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return next;
}
EXPORT_SYMBOL(of_get_next_available_child);

/**
 * of_get_compatible_child - Find compatible child node
 * @parent:	parent node
 * @compatible:	compatible string
 *
 * Lookup child node whose compatible property contains the given compatible
 * string.
 *
 * Returns a node pointer with refcount incremented, use of_node_put() on it
 * when done; or NULL if not found.
 */
struct device_node *of_get_compatible_child(const struct device_node *parent,
				const char *compatible)
{
	struct device_node *child;

	for_each_child_of_node(parent, child) {
		if (of_device_is_compatible(child, compatible))
			break;
	}

	return child;
}
EXPORT_SYMBOL(of_get_compatible_child);

/**
 *	of_get_child_by_name - Find the child node by name for a given parent
 *	@node:	parent node
 *	@name:	child name to look for.
 *
 *      This function looks for child node for given matching name
 *
 *	Returns a node pointer if found, with refcount incremented, use
 *	of_node_put() on it when done.
 *	Returns NULL if node is not found.
 */
struct device_node *of_get_child_by_name(const struct device_node *node,
				const char *name)
{
	struct device_node *child;

	for_each_child_of_node(node, child)
		if (child->name && (of_node_cmp(child->name, name) == 0))
			break;
	return child;
}
EXPORT_SYMBOL(of_get_child_by_name);

static struct device_node *__of_find_node_by_path(struct device_node *parent,
						const char *path)
{
	struct device_node *child;
	int len;

	len = strcspn(path, "/:");
	if (!len)
		return NULL;

	__for_each_child_of_node(parent, child) {
		const char *name = strrchr(child->full_name, '/');
		if (WARN(!name, "malformed device_node %s\n", child->full_name))
			continue;
		name++;
		if (strncmp(path, name, len) == 0 && (strlen(name) == len))
			return child;
	}
	return NULL;
}

/**
 *	of_find_node_opts_by_path - Find a node matching a full OF path
 *	@path: Either the full path to match, or if the path does not
 *	       start with '/', the name of a property of the /aliases
 *	       node (an alias).  In the case of an alias, the node
 *	       matching the alias' value will be returned.
 *	@opts: Address of a pointer into which to store the start of
 *	       an options string appended to the end of the path with
 *	       a ':' separator.
 *
 *	Valid paths:
 *		/foo/bar	Full path
 *		foo		Valid alias
 *		foo/bar		Valid alias + relative path
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_find_node_opts_by_path(const char *path, const char **opts)
{
	struct device_node *np = NULL;
	struct property *pp;
	unsigned long flags;
	const char *separator = strchr(path, ':');

	if (opts)
		*opts = separator ? separator + 1 : NULL;

	if (strcmp(path, "/") == 0)
		return of_node_get(of_root);

	/* The path could begin with an alias */
	if (*path != '/') {
		int len;
		const char *p = strchrnul(path, '/');

		if (separator && separator < p)
			p = separator;
		len = p - path;

		/* of_aliases must not be NULL */
		if (!of_aliases)
			return NULL;

		for_each_property_of_node(of_aliases, pp) {
			if (strlen(pp->name) == len && !strncmp(pp->name, path, len)) {
				np = of_find_node_by_path(pp->value);
				break;
			}
		}
		if (!np)
			return NULL;
		path = p;
	}

	/* Step down the tree matching path components */
	raw_spin_lock_irqsave(&devtree_lock, flags);
	if (!np)
		np = of_node_get(of_root);
	while (np && *path == '/') {
		path++; /* Increment past '/' delimiter */
		np = __of_find_node_by_path(np, path);
		path = strchrnul(path, '/');
		if (separator && separator < path)
			break;
	}
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return np;
}
EXPORT_SYMBOL(of_find_node_opts_by_path);

/**
 *	of_find_node_by_name - Find a node by its "name" property
 *	@from:	The node to start searching from or NULL, the node
 *		you pass will not be searched, only the next one
 *		will; typically, you pass what the previous call
 *		returned. of_node_put() will be called on it
 *	@name:	The name string to match against
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_find_node_by_name(struct device_node *from,
	const char *name)
{
	struct device_node *np;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	for_each_of_allnodes_from(from, np)
		if (np->name && (of_node_cmp(np->name, name) == 0)
		    && of_node_get(np))
			break;
	of_node_put(from);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return np;
}
EXPORT_SYMBOL(of_find_node_by_name);

/**
 *	of_find_node_by_type - Find a node by its "device_type" property
 *	@from:	The node to start searching from, or NULL to start searching
 *		the entire device tree. The node you pass will not be
 *		searched, only the next one will; typically, you pass
 *		what the previous call returned. of_node_put() will be
 *		called on from for you.
 *	@type:	The type string to match against
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_find_node_by_type(struct device_node *from,
	const char *type)
{
	struct device_node *np;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	for_each_of_allnodes_from(from, np)
		if (np->type && (of_node_cmp(np->type, type) == 0)
		    && of_node_get(np))
			break;
	of_node_put(from);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return np;
}
EXPORT_SYMBOL(of_find_node_by_type);

/**
 *	of_find_compatible_node - Find a node based on type and one of the
 *                                tokens in its "compatible" property
 *	@from:		The node to start searching from or NULL, the node
 *			you pass will not be searched, only the next one
 *			will; typically, you pass what the previous call
 *			returned. of_node_put() will be called on it
 *	@type:		The type string to match "device_type" or NULL to ignore
 *	@compatible:	The string to match to one of the tokens in the device
 *			"compatible" list.
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_find_compatible_node(struct device_node *from,
	const char *type, const char *compatible)
{
	struct device_node *np;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	for_each_of_allnodes_from(from, np)
		if (__of_device_is_compatible(np, compatible, type, NULL) &&
		    of_node_get(np))
			break;
	of_node_put(from);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return np;
}
EXPORT_SYMBOL(of_find_compatible_node);

/**
 *	of_find_node_with_property - Find a node which has a property with
 *                                   the given name.
 *	@from:		The node to start searching from or NULL, the node
 *			you pass will not be searched, only the next one
 *			will; typically, you pass what the previous call
 *			returned. of_node_put() will be called on it
 *	@prop_name:	The name of the property to look for.
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_find_node_with_property(struct device_node *from,
	const char *prop_name)
{
	struct device_node *np;
	struct property *pp;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	for_each_of_allnodes_from(from, np) {
		for (pp = np->properties; pp; pp = pp->next) {
			if (of_prop_cmp(pp->name, prop_name) == 0) {
				of_node_get(np);
				goto out;
			}
		}
	}
out:
	of_node_put(from);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return np;
}
EXPORT_SYMBOL(of_find_node_with_property);

static
const struct of_device_id *__of_match_node(const struct of_device_id *matches,
					   const struct device_node *node)
{
	const struct of_device_id *best_match = NULL;
	int score, best_score = 0;

	if (!matches)
		return NULL;

	for (; matches->name[0] || matches->type[0] || matches->compatible[0]; matches++) {
		score = __of_device_is_compatible(node, matches->compatible,
						  matches->type, matches->name);
		if (score > best_score) {
			best_match = matches;
			best_score = score;
		}
	}

	return best_match;
}

/**
 * of_match_node - Tell if a device_node has a matching of_match structure
 *	@matches:	array of of device match structures to search in
 *	@node:		the of device structure to match against
 *
 *	Low level utility function used by device matching.
 */
const struct of_device_id *of_match_node(const struct of_device_id *matches,
					 const struct device_node *node)
{
	const struct of_device_id *match;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	match = __of_match_node(matches, node);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return match;
}
EXPORT_SYMBOL(of_match_node);

/**
 *	of_find_matching_node_and_match - Find a node based on an of_device_id
 *					  match table.
 *	@from:		The node to start searching from or NULL, the node
 *			you pass will not be searched, only the next one
 *			will; typically, you pass what the previous call
 *			returned. of_node_put() will be called on it
 *	@matches:	array of of device match structures to search in
 *	@match		Updated to point at the matches entry which matched
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_find_matching_node_and_match(struct device_node *from,
					const struct of_device_id *matches,
					const struct of_device_id **match)
{
	struct device_node *np;
	const struct of_device_id *m;
	unsigned long flags;

	if (match)
		*match = NULL;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	for_each_of_allnodes_from(from, np) {
		m = __of_match_node(matches, np);
		if (m && of_node_get(np)) {
			if (match)
				*match = m;
			break;
		}
	}
	of_node_put(from);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return np;
}
EXPORT_SYMBOL(of_find_matching_node_and_match);

/**
 * of_modalias_node - Lookup appropriate modalias for a device node
 * @node:	pointer to a device tree node
 * @modalias:	Pointer to buffer that modalias value will be copied into
 * @len:	Length of modalias value
 *
 * Based on the value of the compatible property, this routine will attempt
 * to choose an appropriate modalias value for a particular device tree node.
 * It does this by stripping the manufacturer prefix (as delimited by a ',')
 * from the first entry in the compatible list property.
 *
 * This routine returns 0 on success, <0 on failure.
 */
int of_modalias_node(struct device_node *node, char *modalias, int len)
{
	const char *compatible, *p;
	int cplen;

	compatible = of_get_property(node, "compatible", &cplen);
	if (!compatible || strlen(compatible) > cplen)
		return -ENODEV;
	p = strchr(compatible, ',');
	strlcpy(modalias, p ? p + 1 : compatible, len);
	return 0;
}
EXPORT_SYMBOL_GPL(of_modalias_node);

/**
 * of_find_node_by_phandle - Find a node given a phandle
 * @handle:	phandle of the node to find
 *
 * Returns a node pointer with refcount incremented, use
 * of_node_put() on it when done.
 */
struct device_node *of_find_node_by_phandle(phandle handle)
{
	struct device_node *np = NULL;
	unsigned long flags;
	phandle masked_handle;

	if (!handle)
		return NULL;

	raw_spin_lock_irqsave(&devtree_lock, flags);

	masked_handle = handle & phandle_cache_mask;

	if (phandle_cache) {
		if (phandle_cache[masked_handle] &&
		    handle == phandle_cache[masked_handle]->phandle)
			np = phandle_cache[masked_handle];
	}

	if (!np) {
		for_each_of_allnodes(np)
			if (np->phandle == handle) {
				if (phandle_cache)
					phandle_cache[masked_handle] = np;
				break;
			}
	}

	of_node_get(np);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return np;
}
EXPORT_SYMBOL(of_find_node_by_phandle);

/**
 * of_property_count_elems_of_size - Count the number of elements in a property
 *
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @elem_size:	size of the individual element
 *
 * Search for a property in a device node and count the number of elements of
 * size elem_size in it. Returns number of elements on sucess, -EINVAL if the
 * property does not exist or its length does not match a multiple of elem_size
 * and -ENODATA if the property does not have a value.
 */
int of_property_count_elems_of_size(const struct device_node *np,
				const char *propname, int elem_size)
{
	struct property *prop = of_find_property(np, propname, NULL);

	if (!prop)
		return -EINVAL;
	if (!prop->value)
		return -ENODATA;

	if (prop->length % elem_size != 0) {
		pr_err("size of %s in node %s is not a multiple of %d\n",
		       propname, np->full_name, elem_size);
		return -EINVAL;
	}

	return prop->length / elem_size;
}
EXPORT_SYMBOL_GPL(of_property_count_elems_of_size);

/**
 * of_find_property_value_of_size
 *
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @min:	minimum allowed length of property value
 * @max:	maximum allowed length of property value (0 means unlimited)
 * @len:	if !=NULL, actual length is written to here
 *
 * Search for a property in a device node and valid the requested size.
 * Returns the property value on success, -EINVAL if the property does not
 *  exist, -ENODATA if property does not have a value, and -EOVERFLOW if the
 * property data is too small or too large.
 *
 */
static void *of_find_property_value_of_size(const struct device_node *np,
			const char *propname, u32 min, u32 max, size_t *len)
{
	struct property *prop = of_find_property(np, propname, NULL);

	if (!prop)
		return ERR_PTR(-EINVAL);
	if (!prop->value)
		return ERR_PTR(-ENODATA);
	if (prop->length < min)
		return ERR_PTR(-EOVERFLOW);
	if (max && prop->length > max)
		return ERR_PTR(-EOVERFLOW);

	if (len)
		*len = prop->length;

	return prop->value;
}

/**
 * of_property_read_u32_index - Find and read a u32 from a multi-value property.
 *
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @index:	index of the u32 in the list of values
 * @out_value:	pointer to return value, modified only if no error.
 *
 * Search for a property in a device node and read nth 32-bit value from
 * it. Returns 0 on success, -EINVAL if the property does not exist,
 * -ENODATA if property does not have a value, and -EOVERFLOW if the
 * property data isn't large enough.
 *
 * The out_value is modified only if a valid u32 value can be decoded.
 */
int of_property_read_u32_index(const struct device_node *np,
				       const char *propname,
				       u32 index, u32 *out_value)
{
	const u32 *val = of_find_property_value_of_size(np, propname,
					((index + 1) * sizeof(*out_value)),
					0,
					NULL);

	if (IS_ERR(val))
		return PTR_ERR(val);

	*out_value = be32_to_cpup(((__be32 *)val) + index);
	return 0;
}
EXPORT_SYMBOL_GPL(of_property_read_u32_index);

/**
 * of_property_read_variable_u8_array - Find and read an array of u8 from a
 * property, with bounds on the minimum and maximum array size.
 *
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @out_values:	pointer to return value, modified only if return value is 0.
 * @sz_min:	minimum number of array elements to read
 * @sz_max:	maximum number of array elements to read, if zero there is no
 *		upper limit on the number of elements in the dts entry but only
 *		sz_min will be read.
 *
 * Search for a property in a device node and read 8-bit value(s) from
 * it. Returns number of elements read on success, -EINVAL if the property
 * does not exist, -ENODATA if property does not have a value, and -EOVERFLOW
 * if the property data is smaller than sz_min or longer than sz_max.
 *
 * dts entry of array should be like:
 *	property = /bits/ 8 <0x50 0x60 0x70>;
 *
 * The out_values is modified only if a valid u8 value can be decoded.
 */
int of_property_read_variable_u8_array(const struct device_node *np,
					const char *propname, u8 *out_values,
					size_t sz_min, size_t sz_max)
{
	size_t sz, count;
	const u8 *val = of_find_property_value_of_size(np, propname,
						(sz_min * sizeof(*out_values)),
						(sz_max * sizeof(*out_values)),
						&sz);

	if (IS_ERR(val))
		return PTR_ERR(val);

	if (!sz_max)
		sz = sz_min;
	else
		sz /= sizeof(*out_values);

	count = sz;
	while (count--)
		*out_values++ = *val++;

	return sz;
}
EXPORT_SYMBOL_GPL(of_property_read_variable_u8_array);

/**
 * of_property_read_variable_u16_array - Find and read an array of u16 from a
 * property, with bounds on the minimum and maximum array size.
 *
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @out_values:	pointer to return value, modified only if return value is 0.
 * @sz_min:	minimum number of array elements to read
 * @sz_max:	maximum number of array elements to read, if zero there is no
 *		upper limit on the number of elements in the dts entry but only
 *		sz_min will be read.
 *
 * Search for a property in a device node and read 16-bit value(s) from
 * it. Returns number of elements read on success, -EINVAL if the property
 * does not exist, -ENODATA if property does not have a value, and -EOVERFLOW
 * if the property data is smaller than sz_min or longer than sz_max.
 *
 * dts entry of array should be like:
 *	property = /bits/ 16 <0x5000 0x6000 0x7000>;
 *
 * The out_values is modified only if a valid u16 value can be decoded.
 */
int of_property_read_variable_u16_array(const struct device_node *np,
					const char *propname, u16 *out_values,
					size_t sz_min, size_t sz_max)
{
	size_t sz, count;
	const __be16 *val = of_find_property_value_of_size(np, propname,
						(sz_min * sizeof(*out_values)),
						(sz_max * sizeof(*out_values)),
						&sz);

	if (IS_ERR(val))
		return PTR_ERR(val);

	if (!sz_max)
		sz = sz_min;
	else
		sz /= sizeof(*out_values);

	count = sz;
	while (count--)
		*out_values++ = be16_to_cpup(val++);

	return sz;
}
EXPORT_SYMBOL_GPL(of_property_read_variable_u16_array);

/**
 * of_property_read_variable_u32_array - Find and read an array of 32 bit
 * integers from a property, with bounds on the minimum and maximum array size.
 *
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @out_values:	pointer to return value, modified only if return value is 0.
 * @sz_min:	minimum number of array elements to read
 * @sz_max:	maximum number of array elements to read, if zero there is no
 *		upper limit on the number of elements in the dts entry but only
 *		sz_min will be read.
 *
 * Search for a property in a device node and read 32-bit value(s) from
 * it. Returns number of elements read on success, -EINVAL if the property
 * does not exist, -ENODATA if property does not have a value, and -EOVERFLOW
 * if the property data is smaller than sz_min or longer than sz_max.
 *
 * The out_values is modified only if a valid u32 value can be decoded.
 */
int of_property_read_variable_u32_array(const struct device_node *np,
			       const char *propname, u32 *out_values,
			       size_t sz_min, size_t sz_max)
{
	size_t sz, count;
	const __be32 *val = of_find_property_value_of_size(np, propname,
						(sz_min * sizeof(*out_values)),
						(sz_max * sizeof(*out_values)),
						&sz);

	if (IS_ERR(val))
		return PTR_ERR(val);

	if (!sz_max)
		sz = sz_min;
	else
		sz /= sizeof(*out_values);

	count = sz;
	while (count--)
		*out_values++ = be32_to_cpup(val++);

	return sz;
}
EXPORT_SYMBOL_GPL(of_property_read_variable_u32_array);

/**
 * of_property_read_u64 - Find and read a 64 bit integer from a property
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @out_value:	pointer to return value, modified only if return value is 0.
 *
 * Search for a property in a device node and read a 64-bit value from
 * it. Returns 0 on success, -EINVAL if the property does not exist,
 * -ENODATA if property does not have a value, and -EOVERFLOW if the
 * property data isn't large enough.
 *
 * The out_value is modified only if a valid u64 value can be decoded.
 */
int of_property_read_u64(const struct device_node *np, const char *propname,
			 u64 *out_value)
{
	const __be32 *val = of_find_property_value_of_size(np, propname,
						sizeof(*out_value),
						0,
						NULL);

	if (IS_ERR(val))
		return PTR_ERR(val);

	*out_value = of_read_number(val, 2);
	return 0;
}
EXPORT_SYMBOL_GPL(of_property_read_u64);

/**
 * of_property_read_variable_u64_array - Find and read an array of 64 bit
 * integers from a property, with bounds on the minimum and maximum array size.
 *
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @out_values:	pointer to return value, modified only if return value is 0.
 * @sz_min:	minimum number of array elements to read
 * @sz_max:	maximum number of array elements to read, if zero there is no
 *		upper limit on the number of elements in the dts entry but only
 *		sz_min will be read.
 *
 * Search for a property in a device node and read 64-bit value(s) from
 * it. Returns number of elements read on success, -EINVAL if the property
 * does not exist, -ENODATA if property does not have a value, and -EOVERFLOW
 * if the property data is smaller than sz_min or longer than sz_max.
 *
 * The out_values is modified only if a valid u64 value can be decoded.
 */
int of_property_read_variable_u64_array(const struct device_node *np,
			       const char *propname, u64 *out_values,
			       size_t sz_min, size_t sz_max)
{
	size_t sz, count;
	const __be32 *val = of_find_property_value_of_size(np, propname,
						(sz_min * sizeof(*out_values)),
						(sz_max * sizeof(*out_values)),
						&sz);

	if (IS_ERR(val))
		return PTR_ERR(val);

	if (!sz_max)
		sz = sz_min;
	else
		sz /= sizeof(*out_values);

	count = sz;
	while (count--) {
		*out_values++ = of_read_number(val, 2);
		val += 2;
	}

	return sz;
}
EXPORT_SYMBOL_GPL(of_property_read_variable_u64_array);

/**
 * of_property_read_string - Find and read a string from a property
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @out_string:	pointer to null terminated return string, modified only if
 *		return value is 0.
 *
 * Search for a property in a device tree node and retrieve a null
 * terminated string value (pointer to data, not a copy). Returns 0 on
 * success, -EINVAL if the property does not exist, -ENODATA if property
 * does not have a value, and -EILSEQ if the string is not null-terminated
 * within the length of the property data.
 *
 * The out_string pointer is modified only if a valid string can be decoded.
 */
int of_property_read_string(const struct device_node *np, const char *propname,
				const char **out_string)
{
	const struct property *prop = of_find_property(np, propname, NULL);
	if (!prop)
		return -EINVAL;
	if (!prop->value)
		return -ENODATA;
	if (strnlen(prop->value, prop->length) >= prop->length)
		return -EILSEQ;
	*out_string = prop->value;
	return 0;
}
EXPORT_SYMBOL_GPL(of_property_read_string);

/**
 * of_property_match_string() - Find string in a list and return index
 * @np: pointer to node containing string list property
 * @propname: string list property name
 * @string: pointer to string to search for in string list
 *
 * This function searches a string list property and returns the index
 * of a specific string value.
 */
int of_property_match_string(const struct device_node *np, const char *propname,
			     const char *string)
{
	const struct property *prop = of_find_property(np, propname, NULL);
	size_t l;
	int i;
	const char *p, *end;

	if (!prop)
		return -EINVAL;
	if (!prop->value)
		return -ENODATA;

	p = prop->value;
	end = p + prop->length;

	for (i = 0; p < end; i++, p += l) {
		l = strnlen(p, end - p) + 1;
		if (p + l > end)
			return -EILSEQ;
		pr_debug("comparing %s with %s\n", string, p);
		if (strcmp(string, p) == 0)
			return i; /* Found it; return index */
	}
	return -ENODATA;
}
EXPORT_SYMBOL_GPL(of_property_match_string);

/**
 * of_property_read_string_helper() - Utility helper for parsing string properties
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @out_strs:	output array of string pointers.
 * @sz:		number of array elements to read.
 * @skip:	Number of strings to skip over at beginning of list.
 *
 * Don't call this function directly. It is a utility helper for the
 * of_property_read_string*() family of functions.
 */
int of_property_read_string_helper(const struct device_node *np,
				   const char *propname, const char **out_strs,
				   size_t sz, int skip)
{
	const struct property *prop = of_find_property(np, propname, NULL);
	int l = 0, i = 0;
	const char *p, *end;

	if (!prop)
		return -EINVAL;
	if (!prop->value)
		return -ENODATA;
	p = prop->value;
	end = p + prop->length;

	for (i = 0; p < end && (!out_strs || i < skip + sz); i++, p += l) {
		l = strnlen(p, end - p) + 1;
		if (p + l > end)
			return -EILSEQ;
		if (out_strs && i >= skip)
			*out_strs++ = p;
	}
	i -= skip;
	return i <= 0 ? -ENODATA : i;
}
EXPORT_SYMBOL_GPL(of_property_read_string_helper);

void of_print_phandle_args(const char *msg, const struct of_phandle_args *args)
{
	int i;
	printk("%s %s", msg, of_node_full_name(args->np));
	for (i = 0; i < args->args_count; i++)
		printk(i ? ",%08x" : ":%08x", args->args[i]);
	printk("\n");
}

int of_phandle_iterator_init(struct of_phandle_iterator *it,
		const struct device_node *np,
		const char *list_name,
		const char *cells_name,
		int cell_count)
{
	const __be32 *list;
	int size;

	memset(it, 0, sizeof(*it));

	list = of_get_property(np, list_name, &size);
	if (!list)
		return -ENOENT;

	it->cells_name = cells_name;
	it->cell_count = cell_count;
	it->parent = np;
	it->list_end = list + size / sizeof(*list);
	it->phandle_end = list;
	it->cur = list;

	return 0;
}

int of_phandle_iterator_next(struct of_phandle_iterator *it)
{
	uint32_t count = 0;

	if (it->node) {
		of_node_put(it->node);
		it->node = NULL;
	}

	if (!it->cur || it->phandle_end >= it->list_end)
		return -ENOENT;

	it->cur = it->phandle_end;

	/* If phandle is 0, then it is an empty entry with no arguments. */
	it->phandle = be32_to_cpup(it->cur++);

	if (it->phandle) {

		/*
		 * Find the provider node and parse the #*-cells property to
		 * determine the argument length.
		 */
		it->node = of_find_node_by_phandle(it->phandle);

		if (it->cells_name) {
			if (!it->node) {
				pr_err("%s: could not find phandle\n",
				       it->parent->full_name);
				goto err;
			}

			if (of_property_read_u32(it->node, it->cells_name,
						 &count)) {
				pr_err("%s: could not get %s for %s\n",
				       it->parent->full_name,
				       it->cells_name,
				       it->node->full_name);
				goto err;
			}
		} else {
			count = it->cell_count;
		}

		/*
		 * Make sure that the arguments actually fit in the remaining
		 * property data length
		 */
		if (it->cur + count > it->list_end) {
			pr_err("%s: arguments longer than property\n",
			       it->parent->full_name);
			goto err;
		}
	}

	it->phandle_end = it->cur + count;
	it->cur_count = count;

	return 0;

err:
	if (it->node) {
		of_node_put(it->node);
		it->node = NULL;
	}

	return -EINVAL;
}

int of_phandle_iterator_args(struct of_phandle_iterator *it,
			     uint32_t *args,
			     int size)
{
	int i, count;

	count = it->cur_count;

	if (WARN_ON(size < count))
		count = size;

	for (i = 0; i < count; i++)
		args[i] = be32_to_cpup(it->cur++);

	return count;
}

static int __of_parse_phandle_with_args(const struct device_node *np,
					const char *list_name,
					const char *cells_name,
					int cell_count, int index,
					struct of_phandle_args *out_args)
{
	struct of_phandle_iterator it;
	int rc, cur_index = 0;

	/* Loop over the phandles until all the requested entry is found */
	of_for_each_phandle(&it, rc, np, list_name, cells_name, cell_count) {
		/*
		 * All of the error cases bail out of the loop, so at
		 * this point, the parsing is successful. If the requested
		 * index matches, then fill the out_args structure and return,
		 * or return -ENOENT for an empty entry.
		 */
		rc = -ENOENT;
		if (cur_index == index) {
			if (!it.phandle)
				goto err;

			if (out_args) {
				int c;

				c = of_phandle_iterator_args(&it,
							     out_args->args,
							     MAX_PHANDLE_ARGS);
				out_args->np = it.node;
				out_args->args_count = c;
			} else {
				of_node_put(it.node);
			}

			/* Found it! return success */
			return 0;
		}

		cur_index++;
	}

	/*
	 * Unlock node before returning result; will be one of:
	 * -ENOENT : index is for empty phandle
	 * -EINVAL : parsing error on data
	 */

 err:
	of_node_put(it.node);
	return rc;
}

/**
 * of_parse_phandle - Resolve a phandle property to a device_node pointer
 * @np: Pointer to device node holding phandle property
 * @phandle_name: Name of property holding a phandle value
 * @index: For properties holding a table of phandles, this is the index into
 *         the table
 *
 * Returns the device_node pointer with refcount incremented.  Use
 * of_node_put() on it when done.
 */
struct device_node *of_parse_phandle(const struct device_node *np,
				     const char *phandle_name, int index)
{
	struct of_phandle_args args;

	if (index < 0)
		return NULL;

	if (__of_parse_phandle_with_args(np, phandle_name, NULL, 0,
					 index, &args))
		return NULL;

	return args.np;
}
EXPORT_SYMBOL(of_parse_phandle);

/**
 * of_parse_phandle_with_args() - Find a node pointed by phandle in a list
 * @np:		pointer to a device tree node containing a list
 * @list_name:	property name that contains a list
 * @cells_name:	property name that specifies phandles' arguments count
 * @index:	index of a phandle to parse out
 * @out_args:	optional pointer to output arguments structure (will be filled)
 *
 * This function is useful to parse lists of phandles and their arguments.
 * Returns 0 on success and fills out_args, on error returns appropriate
 * errno value.
 *
 * Caller is responsible to call of_node_put() on the returned out_args->np
 * pointer.
 *
 * Example:
 *
 * phandle1: node1 {
 *	#list-cells = <2>;
 * }
 *
 * phandle2: node2 {
 *	#list-cells = <1>;
 * }
 *
 * node3 {
 *	list = <&phandle1 1 2 &phandle2 3>;
 * }
 *
 * To get a device_node of the `node2' node you may call this:
 * of_parse_phandle_with_args(node3, "list", "#list-cells", 1, &args);
 */
int of_parse_phandle_with_args(const struct device_node *np, const char *list_name,
				const char *cells_name, int index,
				struct of_phandle_args *out_args)
{
	if (index < 0)
		return -EINVAL;
	return __of_parse_phandle_with_args(np, list_name, cells_name, 0,
					    index, out_args);
}
EXPORT_SYMBOL(of_parse_phandle_with_args);

/**
 * of_parse_phandle_with_fixed_args() - Find a node pointed by phandle in a list
 * @np:		pointer to a device tree node containing a list
 * @list_name:	property name that contains a list
 * @cell_count: number of argument cells following the phandle
 * @index:	index of a phandle to parse out
 * @out_args:	optional pointer to output arguments structure (will be filled)
 *
 * This function is useful to parse lists of phandles and their arguments.
 * Returns 0 on success and fills out_args, on error returns appropriate
 * errno value.
 *
 * Caller is responsible to call of_node_put() on the returned out_args->np
 * pointer.
 *
 * Example:
 *
 * phandle1: node1 {
 * }
 *
 * phandle2: node2 {
 * }
 *
 * node3 {
 *	list = <&phandle1 0 2 &phandle2 2 3>;
 * }
 *
 * To get a device_node of the `node2' node you may call this:
 * of_parse_phandle_with_fixed_args(node3, "list", 2, 1, &args);
 */
int of_parse_phandle_with_fixed_args(const struct device_node *np,
				const char *list_name, int cell_count,
				int index, struct of_phandle_args *out_args)
{
	if (index < 0)
		return -EINVAL;
	return __of_parse_phandle_with_args(np, list_name, NULL, cell_count,
					   index, out_args);
}
EXPORT_SYMBOL(of_parse_phandle_with_fixed_args);

/**
 * of_count_phandle_with_args() - Find the number of phandles references in a property
 * @np:		pointer to a device tree node containing a list
 * @list_name:	property name that contains a list
 * @cells_name:	property name that specifies phandles' arguments count
 *
 * Returns the number of phandle + argument tuples within a property. It
 * is a typical pattern to encode a list of phandle and variable
 * arguments into a single property. The number of arguments is encoded
 * by a property in the phandle-target node. For example, a gpios
 * property would contain a list of GPIO specifies consisting of a
 * phandle and 1 or more arguments. The number of arguments are
 * determined by the #gpio-cells property in the node pointed to by the
 * phandle.
 */
int of_count_phandle_with_args(const struct device_node *np, const char *list_name,
				const char *cells_name)
{
	struct of_phandle_iterator it;
	int rc, cur_index = 0;

	rc = of_phandle_iterator_init(&it, np, list_name, cells_name, 0);
	if (rc)
		return rc;

	while ((rc = of_phandle_iterator_next(&it)) == 0)
		cur_index += 1;

	if (rc != -ENOENT)
		return rc;

	return cur_index;
}
EXPORT_SYMBOL(of_count_phandle_with_args);

/**
 * __of_add_property - Add a property to a node without lock operations
 */
int __of_add_property(struct device_node *np, struct property *prop)
{
	struct property **next;

	prop->next = NULL;
	next = &np->properties;
	while (*next) {
		if (strcmp(prop->name, (*next)->name) == 0)
			/* duplicate ! don't insert it */
			return -EEXIST;

		next = &(*next)->next;
	}
	*next = prop;

	return 0;
}

/**
 * of_add_property - Add a property to a node
 */
int of_add_property(struct device_node *np, struct property *prop)
{
	unsigned long flags;
	int rc;

	mutex_lock(&of_mutex);

	raw_spin_lock_irqsave(&devtree_lock, flags);
	rc = __of_add_property(np, prop);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);

	if (!rc)
		__of_add_property_sysfs(np, prop);

	mutex_unlock(&of_mutex);

	if (!rc)
		of_property_notify(OF_RECONFIG_ADD_PROPERTY, np, prop, NULL);

	return rc;
}

int __of_remove_property(struct device_node *np, struct property *prop)
{
	struct property **next;

	for (next = &np->properties; *next; next = &(*next)->next) {
		if (*next == prop)
			break;
	}
	if (*next == NULL)
		return -ENODEV;

	/* found the node */
	*next = prop->next;
	prop->next = np->deadprops;
	np->deadprops = prop;

	return 0;
}

/**
 * of_remove_property - Remove a property from a node.
 *
 * Note that we don't actually remove it, since we have given out
 * who-knows-how-many pointers to the data using get-property.
 * Instead we just move the property to the "dead properties"
 * list, so it won't be found any more.
 */
int of_remove_property(struct device_node *np, struct property *prop)
{
	unsigned long flags;
	int rc;

	if (!prop)
		return -ENODEV;

	mutex_lock(&of_mutex);

	raw_spin_lock_irqsave(&devtree_lock, flags);
	rc = __of_remove_property(np, prop);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);

	if (!rc)
		__of_remove_property_sysfs(np, prop);

	mutex_unlock(&of_mutex);

	if (!rc)
		of_property_notify(OF_RECONFIG_REMOVE_PROPERTY, np, prop, NULL);

	return rc;
}

int __of_update_property(struct device_node *np, struct property *newprop,
		struct property **oldpropp)
{
	struct property **next, *oldprop;

	for (next = &np->properties; *next; next = &(*next)->next) {
		if (of_prop_cmp((*next)->name, newprop->name) == 0)
			break;
	}
	*oldpropp = oldprop = *next;

	if (oldprop) {
		/* replace the node */
		newprop->next = oldprop->next;
		*next = newprop;
		oldprop->next = np->deadprops;
		np->deadprops = oldprop;
	} else {
		/* new node */
		newprop->next = NULL;
		*next = newprop;
	}

	return 0;
}

/*
 * of_update_property - Update a property in a node, if the property does
 * not exist, add it.
 *
 * Note that we don't actually remove it, since we have given out
 * who-knows-how-many pointers to the data using get-property.
 * Instead we just move the property to the "dead properties" list,
 * and add the new property to the property list
 */
int of_update_property(struct device_node *np, struct property *newprop)
{
	struct property *oldprop;
	unsigned long flags;
	int rc;

	if (!newprop->name)
		return -EINVAL;

	mutex_lock(&of_mutex);

	raw_spin_lock_irqsave(&devtree_lock, flags);
	rc = __of_update_property(np, newprop, &oldprop);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);

	if (!rc)
		__of_update_property_sysfs(np, newprop, oldprop);

	mutex_unlock(&of_mutex);

	if (!rc)
		of_property_notify(OF_RECONFIG_UPDATE_PROPERTY, np, newprop, oldprop);

	return rc;
}

static void of_alias_add(struct alias_prop *ap, struct device_node *np,
			 int id, const char *stem, int stem_len)
{
	ap->np = np;
	ap->id = id;
	strncpy(ap->stem, stem, stem_len);
	ap->stem[stem_len] = 0;
	list_add_tail(&ap->link, &aliases_lookup);
	pr_debug("adding DT alias:%s: stem=%s id=%i node=%s\n",
		 ap->alias, ap->stem, ap->id, of_node_full_name(np));
}

/**
 * of_alias_scan - Scan all properties of the 'aliases' node
 *
 * The function scans all the properties of the 'aliases' node and populates
 * the global lookup table with the properties.  It returns the
 * number of alias properties found, or an error code in case of failure.
 *
 * @dt_alloc:	An allocator that provides a virtual address to memory
 *		for storing the resulting tree
 */
void of_alias_scan(void * (*dt_alloc)(u64 size, u64 align))
{
	struct property *pp;

	of_aliases = of_find_node_by_path("/aliases");
	of_chosen = of_find_node_by_path("/chosen");
	if (of_chosen == NULL)
		of_chosen = of_find_node_by_path("/chosen@0");

	if (of_chosen) {
		/* linux,stdout-path and /aliases/stdout are for legacy compatibility */
		const char *name = of_get_property(of_chosen, "stdout-path", NULL);
		if (!name)
			name = of_get_property(of_chosen, "linux,stdout-path", NULL);
		if (IS_ENABLED(CONFIG_PPC) && !name)
			name = of_get_property(of_aliases, "stdout", NULL);
		if (name)
			of_stdout = of_find_node_opts_by_path(name, &of_stdout_options);
	}

	if (!of_aliases)
		return;

	for_each_property_of_node(of_aliases, pp) {
		const char *start = pp->name;
		const char *end = start + strlen(start);
		struct device_node *np;
		struct alias_prop *ap;
		int id, len;

		/* Skip those we do not want to proceed */
		if (!strcmp(pp->name, "name") ||
		    !strcmp(pp->name, "phandle") ||
		    !strcmp(pp->name, "linux,phandle"))
			continue;

		np = of_find_node_by_path(pp->value);
		if (!np)
			continue;

		/* walk the alias backwards to extract the id and work out
		 * the 'stem' string */
		while (isdigit(*(end-1)) && end > start)
			end--;
		len = end - start;

		if (kstrtoint(end, 10, &id) < 0)
			continue;

		/* Allocate an alias_prop with enough space for the stem */
		ap = dt_alloc(sizeof(*ap) + len + 1, __alignof__(*ap));
		if (!ap)
			continue;
		memset(ap, 0, sizeof(*ap) + len + 1);
		ap->alias = start;
		of_alias_add(ap, np, id, start, len);
	}
}

/**
 * of_alias_get_id - Get alias id for the given device_node
 * @np:		Pointer to the given device_node
 * @stem:	Alias stem of the given device_node
 *
 * The function travels the lookup table to get the alias id for the given
 * device_node and alias stem.  It returns the alias id if found.
 */
int of_alias_get_id(struct device_node *np, const char *stem)
{
	struct alias_prop *app;
	int id = -ENODEV;

	mutex_lock(&of_mutex);
	list_for_each_entry(app, &aliases_lookup, link) {
		if (strcmp(app->stem, stem) != 0)
			continue;

		if (np == app->np) {
			id = app->id;
			break;
		}
	}
	mutex_unlock(&of_mutex);

	return id;
}
EXPORT_SYMBOL_GPL(of_alias_get_id);

/**
 * of_alias_get_highest_id - Get highest alias id for the given stem
 * @stem:	Alias stem to be examined
 *
 * The function travels the lookup table to get the highest alias id for the
 * given alias stem.  It returns the alias id if found.
 */
int of_alias_get_highest_id(const char *stem)
{
	struct alias_prop *app;
	int id = -ENODEV;

	mutex_lock(&of_mutex);
	list_for_each_entry(app, &aliases_lookup, link) {
		if (strcmp(app->stem, stem) != 0)
			continue;

		if (app->id > id)
			id = app->id;
	}
	mutex_unlock(&of_mutex);

	return id;
}
EXPORT_SYMBOL_GPL(of_alias_get_highest_id);

const __be32 *of_prop_next_u32(struct property *prop, const __be32 *cur,
			       u32 *pu)
{
	const void *curv = cur;

	if (!prop)
		return NULL;

	if (!cur) {
		curv = prop->value;
		goto out_val;
	}

	curv += sizeof(*cur);
	if (curv >= prop->value + prop->length)
		return NULL;

out_val:
	*pu = be32_to_cpup(curv);
	return curv;
}
EXPORT_SYMBOL_GPL(of_prop_next_u32);

const char *of_prop_next_string(struct property *prop, const char *cur)
{
	const void *curv = cur;

	if (!prop)
		return NULL;

	if (!cur)
		return prop->value;

	curv += strlen(cur) + 1;
	if (curv >= prop->value + prop->length)
		return NULL;

	return curv;
}
EXPORT_SYMBOL_GPL(of_prop_next_string);

/**
 * of_console_check() - Test and setup console for DT setup
 * @dn - Pointer to device node
 * @name - Name to use for preferred console without index. ex. "ttyS"
 * @index - Index to use for preferred console.
 *
 * Check if the given device node matches the stdout-path property in the
 * /chosen node. If it does then register it as the preferred console and return
 * TRUE. Otherwise return FALSE.
 */
bool of_console_check(struct device_node *dn, char *name, int index)
{
	if (!dn || dn != of_stdout || console_set_on_cmdline)
		return false;
	return !add_preferred_console(name, index,
				      kstrdup(of_stdout_options, GFP_KERNEL));
}
EXPORT_SYMBOL_GPL(of_console_check);

/**
 *	of_find_next_cache_node - Find a node's subsidiary cache
 *	@np:	node of type "cpu" or "cache"
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.  Caller should hold a reference
 *	to np.
 */
struct device_node *of_find_next_cache_node(const struct device_node *np)
{
	struct device_node *child;
	const phandle *handle;

	handle = of_get_property(np, "l2-cache", NULL);
	if (!handle)
		handle = of_get_property(np, "next-level-cache", NULL);

	if (handle)
		return of_find_node_by_phandle(be32_to_cpup(handle));

	/* OF on pmac has nodes instead of properties named "l2-cache"
	 * beneath CPU nodes.
	 */
	if (IS_ENABLED(CONFIG_PPC_PMAC) && !strcmp(np->type, "cpu"))
		for_each_child_of_node(np, child)
			if (!strcmp(child->type, "cache"))
				return child;

	return NULL;
}

/**
 * of_graph_parse_endpoint() - parse common endpoint node properties
 * @node: pointer to endpoint device_node
 * @endpoint: pointer to the OF endpoint data structure
 *
 * The caller should hold a reference to @node.
 */
int of_graph_parse_endpoint(const struct device_node *node,
			    struct of_endpoint *endpoint)
{
	struct device_node *port_node = of_get_parent(node);

	WARN_ONCE(!port_node, "%s(): endpoint %s has no parent node\n",
		  __func__, node->full_name);

	memset(endpoint, 0, sizeof(*endpoint));

	endpoint->local_node = node;
	/*
	 * It doesn't matter whether the two calls below succeed.
	 * If they don't then the default value 0 is used.
	 */
	of_property_read_u32(port_node, "reg", &endpoint->port);
	of_property_read_u32(node, "reg", &endpoint->id);

	of_node_put(port_node);

	return 0;
}
EXPORT_SYMBOL(of_graph_parse_endpoint);

/**
 * of_graph_get_port_by_id() - get the port matching a given id
 * @parent: pointer to the parent device node
 * @id: id of the port
 *
 * Return: A 'port' node pointer with refcount incremented. The caller
 * has to use of_node_put() on it when done.
 */
struct device_node *of_graph_get_port_by_id(struct device_node *parent, u32 id)
{
	struct device_node *node, *port;

	node = of_get_child_by_name(parent, "ports");
	if (node)
		parent = node;

	for_each_child_of_node(parent, port) {
		u32 port_id = 0;

		if (of_node_cmp(port->name, "port") != 0)
			continue;
		of_property_read_u32(port, "reg", &port_id);
		if (id == port_id)
			break;
	}

	of_node_put(node);

	return port;
}
EXPORT_SYMBOL(of_graph_get_port_by_id);

/**
 * of_graph_get_next_endpoint() - get next endpoint node
 * @parent: pointer to the parent device node
 * @prev: previous endpoint node, or NULL to get first
 *
 * Return: An 'endpoint' node pointer with refcount incremented. Refcount
 * of the passed @prev node is decremented.
 */
struct device_node *of_graph_get_next_endpoint(const struct device_node *parent,
					struct device_node *prev)
{
	struct device_node *endpoint;
	struct device_node *port;

	if (!parent)
		return NULL;

	/*
	 * Start by locating the port node. If no previous endpoint is specified
	 * search for the first port node, otherwise get the previous endpoint
	 * parent port node.
	 */
	if (!prev) {
		struct device_node *node;

		node = of_get_child_by_name(parent, "ports");
		if (node)
			parent = node;

		port = of_get_child_by_name(parent, "port");
		of_node_put(node);

		if (!port) {
			pr_err("graph: no port node found in %s\n",
			       parent->full_name);
			return NULL;
		}
	} else {
		port = of_get_parent(prev);
		if (WARN_ONCE(!port, "%s(): endpoint %s has no parent node\n",
			      __func__, prev->full_name))
			return NULL;
	}

	while (1) {
		/*
		 * Now that we have a port node, get the next endpoint by
		 * getting the next child. If the previous endpoint is NULL this
		 * will return the first child.
		 */
		endpoint = of_get_next_child(port, prev);
		if (endpoint) {
			of_node_put(port);
			return endpoint;
		}

		/* No more endpoints under this port, try the next one. */
		prev = NULL;

		do {
			port = of_get_next_child(parent, port);
			if (!port)
				return NULL;
		} while (of_node_cmp(port->name, "port"));
	}
}
EXPORT_SYMBOL(of_graph_get_next_endpoint);

/**
 * of_graph_get_endpoint_by_regs() - get endpoint node of specific identifiers
 * @parent: pointer to the parent device node
 * @port_reg: identifier (value of reg property) of the parent port node
 * @reg: identifier (value of reg property) of the endpoint node
 *
 * Return: An 'endpoint' node pointer which is identified by reg and at the same
 * is the child of a port node identified by port_reg. reg and port_reg are
 * ignored when they are -1.
 */
struct device_node *of_graph_get_endpoint_by_regs(
	const struct device_node *parent, int port_reg, int reg)
{
	struct of_endpoint endpoint;
	struct device_node *node = NULL;

	for_each_endpoint_of_node(parent, node) {
		of_graph_parse_endpoint(node, &endpoint);
		if (((port_reg == -1) || (endpoint.port == port_reg)) &&
			((reg == -1) || (endpoint.id == reg)))
			return node;
	}

	return NULL;
}
EXPORT_SYMBOL(of_graph_get_endpoint_by_regs);

/**
 * of_graph_get_remote_port_parent() - get remote port's parent node
 * @node: pointer to a local endpoint device_node
 *
 * Return: Remote device node associated with remote endpoint node linked
 *	   to @node. Use of_node_put() on it when done.
 */
struct device_node *of_graph_get_remote_port_parent(
			       const struct device_node *node)
{
	struct device_node *np;
	unsigned int depth;

	/* Get remote endpoint node. */
	np = of_parse_phandle(node, "remote-endpoint", 0);

	/* Walk 3 levels up only if there is 'ports' node. */
	for (depth = 3; depth && np; depth--) {
		np = of_get_next_parent(np);
		if (depth == 2 && of_node_cmp(np->name, "ports"))
			break;
	}
	return np;
}
EXPORT_SYMBOL(of_graph_get_remote_port_parent);

/**
 * of_graph_get_remote_port() - get remote port node
 * @node: pointer to a local endpoint device_node
 *
 * Return: Remote port node associated with remote endpoint node linked
 *	   to @node. Use of_node_put() on it when done.
 */
struct device_node *of_graph_get_remote_port(const struct device_node *node)
{
	struct device_node *np;

	/* Get remote endpoint node. */
	np = of_parse_phandle(node, "remote-endpoint", 0);
	if (!np)
		return NULL;
	return of_get_next_parent(np);
}
EXPORT_SYMBOL(of_graph_get_remote_port);
