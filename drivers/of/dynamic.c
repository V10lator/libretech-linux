/*
 * Support for dynamic device trees.
 *
 * On some platforms, the device tree can be manipulated at runtime.
 * The routines in this section support adding, removing and changing
 * device tree nodes.
 */

#define pr_fmt(fmt)	"OF: " fmt

#include <linux/of.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/proc_fs.h>
#include <linux/rhashtable.h>

#include "of_private.h"

/**
 * of_node_get() - Increment refcount of a node
 * @node:	Node to inc refcount, NULL is supported to simplify writing of
 *		callers
 *
 * Returns node.
 */
struct device_node *of_node_get(struct device_node *node)
{
	if (node)
		kobject_get(&node->kobj);
	return node;
}
EXPORT_SYMBOL(of_node_get);

/**
 * of_node_put() - Decrement refcount of a node
 * @node:	Node to dec refcount, NULL is supported to simplify writing of
 *		callers
 */
void of_node_put(struct device_node *node)
{
	if (node)
		kobject_put(&node->kobj);
}
EXPORT_SYMBOL(of_node_put);

void __of_detach_node_post(struct device_node *np)
{
	struct property *pp;
	int rc;

	if (of_phandle_ht_available()) {
		rc = of_phandle_ht_remove(np);
		WARN(rc, "remove from phandle hash fail @%s\n",
				of_node_full_name(np));
	}

	if (!IS_ENABLED(CONFIG_SYSFS))
		return;

	BUG_ON(!of_node_is_initialized(np));
	if (!of_kset)
		return;

	/* only remove properties if on sysfs */
	if (of_node_is_attached(np)) {
		for_each_property_of_node(np, pp)
			__of_sysfs_remove_bin_file(np, pp);
		kobject_del(&np->kobj);
	}

	/* finally remove the kobj_init ref */
	of_node_put(np);
}

static BLOCKING_NOTIFIER_HEAD(of_reconfig_chain);

int of_reconfig_notifier_register(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&of_reconfig_chain, nb);
}
EXPORT_SYMBOL_GPL(of_reconfig_notifier_register);

int of_reconfig_notifier_unregister(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&of_reconfig_chain, nb);
}
EXPORT_SYMBOL_GPL(of_reconfig_notifier_unregister);

#ifdef DEBUG
const char *action_names[] = {
	[OF_RECONFIG_ATTACH_NODE] = "ATTACH_NODE",
	[OF_RECONFIG_DETACH_NODE] = "DETACH_NODE",
	[OF_RECONFIG_ADD_PROPERTY] = "ADD_PROPERTY",
	[OF_RECONFIG_REMOVE_PROPERTY] = "REMOVE_PROPERTY",
	[OF_RECONFIG_UPDATE_PROPERTY] = "UPDATE_PROPERTY",
};
#endif

int of_reconfig_notify(unsigned long action, struct of_reconfig_data *p)
{
	int rc;
#ifdef DEBUG
	struct of_reconfig_data *pr = p;

	switch (action) {
	case OF_RECONFIG_ATTACH_NODE:
	case OF_RECONFIG_DETACH_NODE:
		pr_debug("notify %-15s %s\n", action_names[action],
			pr->dn->full_name);
		break;
	case OF_RECONFIG_ADD_PROPERTY:
	case OF_RECONFIG_REMOVE_PROPERTY:
	case OF_RECONFIG_UPDATE_PROPERTY:
		pr_debug("notify %-15s %s:%s\n", action_names[action],
			pr->dn->full_name, pr->prop->name);
		break;

	}
#endif
	rc = blocking_notifier_call_chain(&of_reconfig_chain, action, p);
	return notifier_to_errno(rc);
}

/*
 * of_reconfig_get_state_change()	- Returns new state of device
 * @action	- action of the of notifier
 * @arg		- argument of the of notifier
 *
 * Returns the new state of a device based on the notifier used.
 * Returns 0 on device going from enabled to disabled, 1 on device
 * going from disabled to enabled and -1 on no change.
 */
int of_reconfig_get_state_change(unsigned long action, struct of_reconfig_data *pr)
{
	struct property *prop, *old_prop = NULL;
	int is_status, status_state, old_status_state, prev_state, new_state;

	/* figure out if a device should be created or destroyed */
	switch (action) {
	case OF_RECONFIG_ATTACH_NODE:
	case OF_RECONFIG_DETACH_NODE:
		prop = of_find_property(pr->dn, "status", NULL);
		break;
	case OF_RECONFIG_ADD_PROPERTY:
	case OF_RECONFIG_REMOVE_PROPERTY:
		prop = pr->prop;
		break;
	case OF_RECONFIG_UPDATE_PROPERTY:
		prop = pr->prop;
		old_prop = pr->old_prop;
		break;
	default:
		return OF_RECONFIG_NO_CHANGE;
	}

	is_status = 0;
	status_state = -1;
	old_status_state = -1;
	prev_state = -1;
	new_state = -1;

	if (prop && !strcmp(prop->name, "status")) {
		is_status = 1;
		status_state = !strcmp(prop->value, "okay") ||
			       !strcmp(prop->value, "ok");
		if (old_prop)
			old_status_state = !strcmp(old_prop->value, "okay") ||
					   !strcmp(old_prop->value, "ok");
	}

	switch (action) {
	case OF_RECONFIG_ATTACH_NODE:
		prev_state = 0;
		/* -1 & 0 status either missing or okay */
		new_state = status_state != 0;
		break;
	case OF_RECONFIG_DETACH_NODE:
		/* -1 & 0 status either missing or okay */
		prev_state = status_state != 0;
		new_state = 0;
		break;
	case OF_RECONFIG_ADD_PROPERTY:
		if (is_status) {
			/* no status property -> enabled (legacy) */
			prev_state = 1;
			new_state = status_state;
		}
		break;
	case OF_RECONFIG_REMOVE_PROPERTY:
		if (is_status) {
			prev_state = status_state;
			/* no status property -> enabled (legacy) */
			new_state = 1;
		}
		break;
	case OF_RECONFIG_UPDATE_PROPERTY:
		if (is_status) {
			prev_state = old_status_state != 0;
			new_state = status_state != 0;
		}
		break;
	}

	if (prev_state == new_state)
		return OF_RECONFIG_NO_CHANGE;

	return new_state ? OF_RECONFIG_CHANGE_ADD : OF_RECONFIG_CHANGE_REMOVE;
}
EXPORT_SYMBOL_GPL(of_reconfig_get_state_change);

int of_property_notify(int action, struct device_node *np,
		       struct property *prop, struct property *oldprop)
{
	struct of_reconfig_data pr;

	/* only call notifiers if the node is attached */
	if (!of_node_is_attached(np))
		return 0;

	pr.dn = np;
	pr.prop = prop;
	pr.old_prop = oldprop;
	return of_reconfig_notify(action, &pr);
}

static void __of_attach_node(struct device_node *np)
{
	const __be32 *phandle;
	int sz;

	np->name = __of_get_property(np, "name", NULL) ? : "<NULL>";
	np->type = __of_get_property(np, "device_type", NULL) ? : "<NULL>";

	phandle = __of_get_property(np, "phandle", &sz);
	if (!phandle)
		phandle = __of_get_property(np, "linux,phandle", &sz);
	if (IS_ENABLED(CONFIG_PPC_PSERIES) && !phandle)
		phandle = __of_get_property(np, "ibm,phandle", &sz);
	np->phandle = (phandle && (sz >= 4)) ? be32_to_cpup(phandle) : 0;

	np->child = NULL;
	np->sibling = np->parent->child;
	np->parent->child = np;
	of_node_clear_flag(np, OF_DETACHED);
}

/**
 * of_attach_node() - Plug a device node into the tree and global list.
 */
int of_attach_node(struct device_node *np)
{
	struct of_reconfig_data rd;
	unsigned long flags;

	memset(&rd, 0, sizeof(rd));
	rd.dn = np;

	mutex_lock(&of_mutex);
	raw_spin_lock_irqsave(&devtree_lock, flags);
	__of_attach_node(np);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);

	__of_attach_node_post(np);
	mutex_unlock(&of_mutex);

	of_reconfig_notify(OF_RECONFIG_ATTACH_NODE, &rd);

	return 0;
}

void __of_detach_node(struct device_node *np)
{
	struct device_node *parent;

	if (WARN_ON(of_node_check_flag(np, OF_DETACHED)))
		return;

	parent = np->parent;
	if (WARN_ON(!parent))
		return;

	if (parent->child == np)
		parent->child = np->sibling;
	else {
		struct device_node *prevsib;
		for (prevsib = np->parent->child;
		     prevsib->sibling != np;
		     prevsib = prevsib->sibling)
			;
		prevsib->sibling = np->sibling;
	}

	of_node_set_flag(np, OF_DETACHED);
}

/**
 * of_detach_node() - "Unplug" a node from the device tree.
 *
 * The caller must hold a reference to the node.  The memory associated with
 * the node is not freed until its refcount goes to zero.
 */
int of_detach_node(struct device_node *np)
{
	struct of_reconfig_data rd;
	unsigned long flags;
	int rc = 0;

	memset(&rd, 0, sizeof(rd));
	rd.dn = np;

	mutex_lock(&of_mutex);
	raw_spin_lock_irqsave(&devtree_lock, flags);
	__of_detach_node(np);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);

	__of_detach_node_post(np);
	mutex_unlock(&of_mutex);

	of_reconfig_notify(OF_RECONFIG_DETACH_NODE, &rd);

	return rc;
}
EXPORT_SYMBOL_GPL(of_detach_node);

/**
 * of_node_release() - release a dynamically allocated node
 * @kref: kref element of the node to be released
 *
 * In of_node_put() this function is passed to kref_put() as the destructor.
 */
void of_node_release(struct kobject *kobj)
{
	struct device_node *node = kobj_to_device_node(kobj);
	struct property *prop = node->properties;

	/* We should never be releasing nodes that haven't been detached. */
	if (!of_node_check_flag(node, OF_DETACHED)) {
		pr_err("ERROR: Bad of_node_put() on %s\n", node->full_name);
		dump_stack();
		return;
	}

	if (!of_node_check_flag(node, OF_DYNAMIC))
		return;

	while (prop) {
		struct property *next = prop->next;
		kfree(prop->name);
		kfree(prop->value);
		kfree(prop);
		prop = next;

		if (!prop) {
			prop = node->deadprops;
			node->deadprops = NULL;
		}
	}
	kfree(node->full_name);
	kfree(node->data);
	kfree(node);
}

/**
 * __of_prop_dup - Copy a property dynamically.
 * @prop:	Property to copy
 * @allocflags:	Allocation flags (typically pass GFP_KERNEL)
 *
 * Copy a property by dynamically allocating the memory of both the
 * property structure and the property name & contents. The property's
 * flags have the OF_DYNAMIC bit set so that we can differentiate between
 * dynamically allocated properties and not.
 * Returns the newly allocated property or NULL on out of memory error.
 */
struct property *__of_prop_dup(const struct property *prop, gfp_t allocflags)
{
	struct property *new;

	new = kzalloc(sizeof(*new), allocflags);
	if (!new)
		return NULL;

	/*
	 * NOTE: There is no check for zero length value.
	 * In case of a boolean property, this will allocate a value
	 * of zero bytes. We do this to work around the use
	 * of of_get_property() calls on boolean values.
	 */
	new->name = kstrdup(prop->name, allocflags);
	new->value = kmemdup(prop->value, prop->length, allocflags);
	new->length = prop->length;
	if (!new->name || !new->value)
		goto err_free;

	/* mark the property as dynamic */
	of_property_set_flag(new, OF_DYNAMIC);

	return new;

 err_free:
	kfree(new->name);
	kfree(new->value);
	kfree(new);
	return NULL;
}

/**
 * __of_node_dupv() - Duplicate or create an empty device node dynamically.
 * @fmt: Format string for new full name of the device node
 * @vargs: va_list containing the arugments for the node full name
 *
 * Create an device tree node, either by duplicating an empty node or by allocating
 * an empty one suitable for further modification.  The node data are
 * dynamically allocated and all the node flags have the OF_DYNAMIC &
 * OF_DETACHED bits set. Returns the newly allocated node or NULL on out of
 * memory error.
 */
struct device_node *__of_node_dupv(const struct device_node *np,
		const char *fmt, va_list vargs)
{
	struct device_node *node;

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return NULL;
	node->full_name = kvasprintf(GFP_KERNEL, fmt, vargs);
	if (!node->full_name) {
		kfree(node);
		return NULL;
	}

	of_node_set_flag(node, OF_DYNAMIC);
	of_node_set_flag(node, OF_DETACHED);
	of_node_init(node);

	/* Iterate over and duplicate all properties */
	if (np) {
		struct property *pp, *new_pp;
		for_each_property_of_node(np, pp) {
			new_pp = __of_prop_dup(pp, GFP_KERNEL);
			if (!new_pp)
				goto err_prop;
			if (__of_add_property(node, new_pp)) {
				kfree(new_pp->name);
				kfree(new_pp->value);
				kfree(new_pp);
				goto err_prop;
			}
		}
	}
	return node;

 err_prop:
	of_node_put(node); /* Frees the node and properties */
	return NULL;
}

/**
 * __of_node_dup() - Duplicate or create an empty device node dynamically.
 * @fmt: Format string (plus vargs) for new full name of the device node
 *
 * See: __of_node_dupv()
 */
struct device_node *__of_node_dup(const struct device_node *np,
		const char *fmt, ...)
{
	va_list vargs;
	struct device_node *node;

	va_start(vargs, fmt);
	node = __of_node_dupv(np, fmt, vargs);
	va_end(vargs);
	return node;
}

static void __of_changeset_entry_destroy(struct of_changeset_entry *ce)
{
	of_node_put(ce->np);
	list_del(&ce->node);
	kfree(ce);
}

#ifdef DEBUG
static void __of_changeset_entry_dump(struct of_changeset_entry *ce)
{
	switch (ce->action) {
	case OF_RECONFIG_ADD_PROPERTY:
	case OF_RECONFIG_REMOVE_PROPERTY:
	case OF_RECONFIG_UPDATE_PROPERTY:
		pr_debug("cset<%p> %-15s %s/%s\n", ce, action_names[ce->action],
			ce->np->full_name, ce->prop->name);
		break;
	case OF_RECONFIG_ATTACH_NODE:
	case OF_RECONFIG_DETACH_NODE:
		pr_debug("cset<%p> %-15s %s\n", ce, action_names[ce->action],
			ce->np->full_name);
		break;
	}
}
#else
static inline void __of_changeset_entry_dump(struct of_changeset_entry *ce)
{
	/* empty */
}
#endif

static void __of_changeset_entry_invert(struct of_changeset_entry *ce,
					  struct of_changeset_entry *rce)
{
	memcpy(rce, ce, sizeof(*rce));

	switch (ce->action) {
	case OF_RECONFIG_ATTACH_NODE:
		rce->action = OF_RECONFIG_DETACH_NODE;
		break;
	case OF_RECONFIG_DETACH_NODE:
		rce->action = OF_RECONFIG_ATTACH_NODE;
		break;
	case OF_RECONFIG_ADD_PROPERTY:
		rce->action = OF_RECONFIG_REMOVE_PROPERTY;
		break;
	case OF_RECONFIG_REMOVE_PROPERTY:
		rce->action = OF_RECONFIG_ADD_PROPERTY;
		break;
	case OF_RECONFIG_UPDATE_PROPERTY:
		rce->old_prop = ce->prop;
		rce->prop = ce->old_prop;
		/* update was used but original property did not exist */
		if (!rce->prop) {
			rce->action = OF_RECONFIG_REMOVE_PROPERTY;
			rce->prop = ce->prop;
		}
		break;
	}
}

static void *alias_alloc(u64 size, u64 align)
{
	return kzalloc(size, GFP_KERNEL);
}

static void __of_changeset_entry_notify(struct of_changeset_entry *ce, bool revert)
{
	struct of_reconfig_data rd;
	struct of_changeset_entry ce_inverted;
	int ret;

	if (revert) {
		__of_changeset_entry_invert(ce, &ce_inverted);
		ce = &ce_inverted;
	}

	// FIXME register a notifier
	// FIXME handle aliases node itself
	if (ce->np == of_aliases) {
		switch (ce->action) {
		case OF_RECONFIG_ADD_PROPERTY:
			of_alias_create(ce->prop, alias_alloc);
			break;
		case OF_RECONFIG_REMOVE_PROPERTY:
			of_alias_destroy(ce->prop->name);
			break;
		case OF_RECONFIG_UPDATE_PROPERTY:
			of_alias_destroy(ce->old_prop->name);
			of_alias_create(ce->prop, alias_alloc);
			break;
		}
	}
	switch (ce->action) {
	case OF_RECONFIG_ATTACH_NODE:
	case OF_RECONFIG_DETACH_NODE:
		memset(&rd, 0, sizeof(rd));
		rd.dn = ce->np;
		ret = of_reconfig_notify(ce->action, &rd);
		break;
	case OF_RECONFIG_ADD_PROPERTY:
	case OF_RECONFIG_REMOVE_PROPERTY:
	case OF_RECONFIG_UPDATE_PROPERTY:
		ret = of_property_notify(ce->action, ce->np, ce->prop, ce->old_prop);
		break;
	default:
		pr_err("invalid devicetree changeset action: %i\n",
			(int)ce->action);
		return;
	}

	if (ret)
		pr_err("changeset notifier error @%s\n", ce->np->full_name);
}

static int __of_changeset_entry_apply(struct of_changeset_entry *ce)
{
	struct property *old_prop, **propp;
	unsigned long flags;
	int ret = 0;

	__of_changeset_entry_dump(ce);

	raw_spin_lock_irqsave(&devtree_lock, flags);
	switch (ce->action) {
	case OF_RECONFIG_ATTACH_NODE:
		__of_attach_node(ce->np);
		break;
	case OF_RECONFIG_DETACH_NODE:
		__of_detach_node(ce->np);
		break;
	case OF_RECONFIG_ADD_PROPERTY:
		/* If the property is in deadprops then it must be removed */
		for (propp = &ce->np->deadprops; *propp; propp = &(*propp)->next) {
			if (*propp == ce->prop) {
				*propp = ce->prop->next;
				ce->prop->next = NULL;
				break;
			}
		}

		ret = __of_add_property(ce->np, ce->prop);
		if (ret) {
			pr_err("changeset: add_property failed @%s/%s\n",
				ce->np->full_name,
				ce->prop->name);
			break;
		}
		break;
	case OF_RECONFIG_REMOVE_PROPERTY:
		ret = __of_remove_property(ce->np, ce->prop);
		if (ret) {
			pr_err("changeset: remove_property failed @%s/%s\n",
				ce->np->full_name,
				ce->prop->name);
			break;
		}
		break;

	case OF_RECONFIG_UPDATE_PROPERTY:
		/* If the property is in deadprops then it must be removed */
		for (propp = &ce->np->deadprops; *propp; propp = &(*propp)->next) {
			if (*propp == ce->prop) {
				*propp = ce->prop->next;
				ce->prop->next = NULL;
				break;
			}
		}

		ret = __of_update_property(ce->np, ce->prop, &old_prop);
		if (ret) {
			pr_err("changeset: update_property failed @%s/%s\n",
				ce->np->full_name,
				ce->prop->name);
			break;
		}
		break;
	default:
		ret = -EINVAL;
	}
	raw_spin_unlock_irqrestore(&devtree_lock, flags);

	if (ret)
		return ret;

	switch (ce->action) {
	case OF_RECONFIG_ATTACH_NODE:
		__of_attach_node_post(ce->np);
		break;
	case OF_RECONFIG_DETACH_NODE:
		__of_detach_node_post(ce->np);
		break;
	case OF_RECONFIG_ADD_PROPERTY:
		/* ignore duplicate names */
		__of_add_property_sysfs(ce->np, ce->prop);
		break;
	case OF_RECONFIG_REMOVE_PROPERTY:
		__of_remove_property_sysfs(ce->np, ce->prop);
		break;
	case OF_RECONFIG_UPDATE_PROPERTY:
		__of_update_property_sysfs(ce->np, ce->prop, ce->old_prop);
		break;
	}

	return 0;
}

static inline int __of_changeset_entry_revert(struct of_changeset_entry *ce)
{
	struct of_changeset_entry ce_inverted;

	__of_changeset_entry_invert(ce, &ce_inverted);
	return __of_changeset_entry_apply(&ce_inverted);
}

/**
 * of_changeset_init - Initialize a changeset for use
 *
 * @ocs:	changeset pointer
 *
 * Initialize a changeset structure
 */
void of_changeset_init(struct of_changeset *ocs)
{
	memset(ocs, 0, sizeof(*ocs));
	INIT_LIST_HEAD(&ocs->entries);
}
EXPORT_SYMBOL_GPL(of_changeset_init);

/**
 * of_changeset_destroy - Destroy a changeset
 *
 * @ocs:	changeset pointer
 *
 * Destroys a changeset. Note that if a changeset is applied,
 * its changes to the tree cannot be reverted.
 */
void of_changeset_destroy(struct of_changeset *ocs)
{
	struct of_changeset_entry *ce, *cen;

	list_for_each_entry_safe_reverse(ce, cen, &ocs->entries, node)
		__of_changeset_entry_destroy(ce);
}
EXPORT_SYMBOL_GPL(of_changeset_destroy);

int __of_changeset_apply(struct of_changeset *ocs)
{
	struct of_changeset_entry *ce;
	int ret;

	/* perform the rest of the work */
	pr_debug("changeset: applying...\n");
	list_for_each_entry(ce, &ocs->entries, node) {
		ret = __of_changeset_entry_apply(ce);
		if (ret) {
			pr_err("Error applying changeset (%d)\n", ret);
			list_for_each_entry_continue_reverse(ce, &ocs->entries, node)
				__of_changeset_entry_revert(ce);
			return ret;
		}
	}
	pr_debug("changeset: applied, emitting notifiers.\n");

	/* drop the global lock while emitting notifiers */
	mutex_unlock(&of_mutex);
	list_for_each_entry(ce, &ocs->entries, node)
		__of_changeset_entry_notify(ce, 0);
	mutex_lock(&of_mutex);
	pr_debug("changeset: notifiers sent.\n");

	return 0;
}

/**
 * of_changeset_apply - Applies a changeset
 *
 * @ocs:	changeset pointer
 *
 * Applies a changeset to the live tree.
 * Any side-effects of live tree state changes are applied here on
 * success, like creation/destruction of devices and side-effects
 * like creation of sysfs properties and directories.
 * Returns 0 on success, a negative error value in case of an error.
 * On error the partially applied effects are reverted.
 */
int of_changeset_apply(struct of_changeset *ocs)
{
	int ret;

	mutex_lock(&of_mutex);
	ret = __of_changeset_apply(ocs);
	mutex_unlock(&of_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(of_changeset_apply);

int __of_changeset_revert(struct of_changeset *ocs)
{
	struct of_changeset_entry *ce;
	int ret;

	pr_debug("changeset: reverting...\n");
	list_for_each_entry_reverse(ce, &ocs->entries, node) {
		ret = __of_changeset_entry_revert(ce);
		if (ret) {
			pr_err("Error reverting changeset (%d)\n", ret);
			list_for_each_entry_continue(ce, &ocs->entries, node)
				__of_changeset_entry_apply(ce);
			return ret;
		}
	}
	pr_debug("changeset: reverted, emitting notifiers.\n");

	/* drop the global lock while emitting notifiers */
	mutex_unlock(&of_mutex);
	list_for_each_entry_reverse(ce, &ocs->entries, node)
		__of_changeset_entry_notify(ce, 1);
	mutex_lock(&of_mutex);
	pr_debug("changeset: notifiers sent.\n");

	return 0;
}

/**
 * of_changeset_revert - Reverts an applied changeset
 *
 * @ocs:	changeset pointer
 *
 * Reverts a changeset returning the state of the tree to what it
 * was before the application.
 * Any side-effects like creation/destruction of devices and
 * removal of sysfs properties and directories are applied.
 * Returns 0 on success, a negative error value in case of an error.
 */
int of_changeset_revert(struct of_changeset *ocs)
{
	int ret;

	mutex_lock(&of_mutex);
	ret = __of_changeset_revert(ocs);
	mutex_unlock(&of_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(of_changeset_revert);

/**
 * of_changeset_action - Perform a changeset action
 *
 * @ocs:	changeset pointer
 * @action:	action to perform
 * @np:		Pointer to device node
 * @prop:	Pointer to property
 *
 * On action being one of:
 * + OF_RECONFIG_ATTACH_NODE
 * + OF_RECONFIG_DETACH_NODE,
 * + OF_RECONFIG_ADD_PROPERTY
 * + OF_RECONFIG_REMOVE_PROPERTY,
 * + OF_RECONFIG_UPDATE_PROPERTY
 * Returns 0 on success, a negative error value in case of an error.
 */
int of_changeset_action(struct of_changeset *ocs, unsigned long action,
		struct device_node *np, struct property *prop)
{
	struct of_changeset_entry *ce;

	ce = kzalloc(sizeof(*ce), GFP_KERNEL);
	if (!ce)
		return -ENOMEM;

	/* get a reference to the node */
	ce->action = action;
	ce->np = of_node_get(np);
	ce->prop = prop;

	if (action == OF_RECONFIG_UPDATE_PROPERTY && prop)
		ce->old_prop = of_find_property(np, prop->name, NULL);

	/* add it to the list */
	list_add_tail(&ce->node, &ocs->entries);
	return 0;
}
EXPORT_SYMBOL_GPL(of_changeset_action);

/* changeset helpers */

/**
 * of_changeset_create_device_node - Create an empty device node
 *
 * @ocs:	changeset pointer
 * @parent:	parent device node
 * @fmt:	format string for the node's full_name
 * @args:	argument list for the format string
 *
 * Create an empty device node, marking it as detached and allocated.
 *
 * Returns a device node on success, an error encoded pointer otherwise
 */
struct device_node *of_changeset_create_device_nodev(
	struct of_changeset *ocs, struct device_node *parent,
	const char *fmt, va_list vargs)
{
	struct device_node *node;

	node = __of_node_dupv(NULL, fmt, vargs);
	if (!node)
		return ERR_PTR(-ENOMEM);

	node->parent = parent;
	return node;
}

/**
 * of_changeset_create_device_node - Create an empty device node
 *
 * @ocs:	changeset pointer
 * @parent:	parent device node
 * @fmt:	Format string for the node's full_name
 * ...		Arguments
 *
 * Create an empty device node, marking it as detached and allocated.
 *
 * Returns a device node on success, an error encoded pointer otherwise
 */
struct device_node *of_changeset_create_device_node(
	struct of_changeset *ocs, struct device_node *parent,
	const char *fmt, ...)
{
	va_list vargs;
	struct device_node *node;

	va_start(vargs, fmt);
	node = of_changeset_create_device_nodev(ocs, parent, fmt, vargs);
	va_end(vargs);
	return node;
}

/**
 * of_changeset_add_property_copy - Create a new property copying name & value
 *
 * @ocs:	changeset pointer
 * @np:		device node pointer
 * @name:	name of the property
 * @value:	pointer to the value data
 * @length:	length of the value in bytes
 *
 * Adds a property to the changeset by making copies of the name & value
 * entries.
 *
 * Returns zero on success, a negative error value otherwise.
 */
int of_changeset_add_property_copy(struct of_changeset *ocs,
		struct device_node *np, const char *name, const void *value,
		int length)
{
	struct property *prop;
	char *new_name;
	void *new_value;
	int ret = -ENOMEM;

	prop = kzalloc(sizeof(*prop), GFP_KERNEL);
	if (!prop)
		goto out_no_prop;

	new_name = kstrdup(name, GFP_KERNEL);
	if (!new_name)
		goto out_no_name;

	/*
	 * NOTE: There is no check for zero length value.
	 * In case of a boolean property, this will allocate a value
	 * of zero bytes. We do this to work around the use
	 * of of_get_property() calls on boolean values.
	 */
	new_value = kmemdup(value, length, GFP_KERNEL);
	if (!new_value)
		goto out_no_value;

	of_property_set_flag(prop, OF_DYNAMIC);

	prop->name = new_name;
	prop->value = new_value;
	prop->length = length;

	ret = of_changeset_add_property(ocs, np, prop);
	if (ret != 0)
		goto out_no_add;

	return 0;

out_no_add:
	kfree(prop->value);
out_no_value:
	kfree(prop->name);
out_no_name:
	kfree(prop);
out_no_prop:
	return ret;
}

/**
 * of_changeset_add_property_string - Create a new string property
 *
 * @ocs:	changeset pointer
 * @np:		device node pointer
 * @name:	name of the property
 * @str:	string property
 *
 * Adds a string property to the changeset by making copies of the name
 * and the string value.
 *
 * Returns zero on success, a negative error value otherwise.
 */
int of_changeset_add_property_string(struct of_changeset *ocs,
		struct device_node *np, const char *name, const char *str)
{
	return of_changeset_add_property_copy(ocs, np, name, str,
			strlen(str) + 1);
}

/**
 * of_changeset_add_property_stringf - Create a new formatted string property
 *
 * @ocs:	changeset pointer
 * @np:		device node pointer
 * @name:	name of the property
 * @fmt:	format of string property
 * ...		arguments of the format string
 *
 * Adds a string property to the changeset by making copies of the name
 * and the formatted value.
 *
 * Returns zero on success, a negative error value otherwise.
 */
int of_changeset_add_property_stringf(struct of_changeset *ocs,
		struct device_node *np, const char *name, const char *fmt, ...)
{
	va_list vargs;
	char *str;
	int ret;

	va_start(vargs, fmt);
	str = kvasprintf(GFP_KERNEL, fmt, vargs);
	va_end(vargs);

	ret = of_changeset_add_property_string(ocs, np, name, str);

	kfree(str);
	return ret;
}

/**
 * of_changeset_add_property_string_list - Create a new string list property
 *
 * @ocs:	changeset pointer
 * @np:		device node pointer
 * @name:	name of the property
 * @strs:	pointer to the string list
 * @count:	string count
 *
 * Adds a string list property to the changeset.
 *
 * Returns zero on success, a negative error value otherwise.
 */
int of_changeset_add_property_string_list(struct of_changeset *ocs,
		struct device_node *np, const char *name, const char **strs,
		int count)
{
	int total = 0, i, ret;
	char *value, *s;

	for (i = 0; i < count; i++) {
		/* check if  it's NULL */
		if (!strs[i])
			return -EINVAL;
		total += strlen(strs[i]) + 1;
	}

	value = kmalloc(total, GFP_KERNEL);
	if (!value)
		return -ENOMEM;

	for (i = 0, s = value; i < count; i++) {
		/* no need to check for NULL, check above */
		strcpy(s, strs[i]);
		s += strlen(strs[i]) + 1;
	}

	ret = of_changeset_add_property_copy(ocs, np, name, value, total);

	kfree(value);

	return ret;
}

/**
 * of_changeset_add_property_u32 - Create a new u32 property
 *
 * @ocs:	changeset pointer
 * @np:		device node pointer
 * @name:	name of the property
 * @val:	value in host endian format
 *
 * Adds a u32 property to the changeset.
 *
 * Returns zero on success, a negative error value otherwise.
 */
int of_changeset_add_property_u32(struct of_changeset *ocs,
		struct device_node *np, const char *name, u32 val)
{
	/* in place */
	val = cpu_to_be32(val);
	return of_changeset_add_property_copy(ocs, np, name, &val, sizeof(val));
}

/**
 * of_changeset_add_property_bool - Create a new u32 property
 *
 * @ocs:	changeset pointer
 * @np:		device node pointer
 * @name:	name of the property
 *
 * Adds a bool property to the changeset. Note that there is
 * no option to set the value to false, since the property
 * existing sets it to true.
 *
 * Returns zero on success, a negative error value otherwise.
 */
int of_changeset_add_property_bool(struct of_changeset *ocs,
		struct device_node *np, const char *name)
{
	return of_changeset_add_property_copy(ocs, np, name, "", 0);
}
