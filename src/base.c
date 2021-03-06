#define	JEMALLOC_BASE_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

static malloc_mutex_t	base_mtx;
static extent_tree_t	base_avail_szad;
static extent_node_t	*base_nodes;
static size_t		base_allocated;

/******************************************************************************/

/* base_mtx must be held. */
static extent_node_t *
base_node_try_alloc(void)
{
	extent_node_t *node;

	if (base_nodes == NULL)
		return (NULL);
	node = base_nodes;
	base_nodes = *(extent_node_t **)node;
	JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED(node, sizeof(extent_node_t));
	return (node);
}

/* base_mtx must be held. */
static void
base_node_dalloc(extent_node_t *node)
{

	JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED(node, sizeof(extent_node_t));
	*(extent_node_t **)node = base_nodes;
	base_nodes = node;
}

/* base_mtx must be held. */
static extent_node_t *
base_chunk_alloc(size_t minsize)
{
	extent_node_t *node;
	size_t csize, nsize;
	void *addr;

	assert(minsize != 0);
	node = base_node_try_alloc();
	/* Allocate enough space to also carve a node out if necessary. */
	nsize = (node == NULL) ? CACHELINE_CEILING(sizeof(extent_node_t)) : 0;
	csize = CHUNK_CEILING(minsize + nsize);
	addr = chunk_alloc_base(csize);
	if (addr == NULL) {
		if (node != NULL)
			base_node_dalloc(node);
		return (NULL);
	}
	if (node == NULL) {
		csize -= nsize;
		node = (extent_node_t *)((uintptr_t)addr + csize);
		if (config_stats)
			base_allocated += nsize;
	}
	extent_node_init(node, NULL, addr, csize, true);
	return (node);
}

/*
 * base_alloc() guarantees demand-zeroed memory, in order to make multi-page
 * sparse data structures such as radix tree nodes efficient with respect to
 * physical memory usage.
 */
void *
base_alloc(size_t size)
{
	void *ret;
	size_t csize;
	extent_node_t *node;
	extent_node_t key;

	/*
	 * Round size up to nearest multiple of the cacheline size, so that
	 * there is no chance of false cache line sharing.
	 */
	csize = CACHELINE_CEILING(size);

	extent_node_init(&key, NULL, NULL, csize, false);
	malloc_mutex_lock(&base_mtx);
	node = extent_tree_szad_nsearch(&base_avail_szad, &key);
	if (node != NULL) {
		/* Use existing space. */
		extent_tree_szad_remove(&base_avail_szad, node);
	} else {
		/* Try to allocate more space. */
		node = base_chunk_alloc(csize);
	}
	if (node == NULL) {
		ret = NULL;
		goto label_return;
	}

	ret = extent_node_addr_get(node);
	if (extent_node_size_get(node) > csize) {
		extent_node_addr_set(node, (void *)((uintptr_t)ret + csize));
		extent_node_size_set(node, extent_node_size_get(node) - csize);
		extent_tree_szad_insert(&base_avail_szad, node);
	} else
		base_node_dalloc(node);
	if (config_stats)
		base_allocated += csize;
	JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED(ret, csize);
label_return:
	malloc_mutex_unlock(&base_mtx);
	return (ret);
}

size_t
base_allocated_get(void)
{
	size_t ret;

	malloc_mutex_lock(&base_mtx);
	ret = base_allocated;
	malloc_mutex_unlock(&base_mtx);
	return (ret);
}

bool
base_boot(void)
{

	if (malloc_mutex_init(&base_mtx))
		return (true);
	extent_tree_szad_new(&base_avail_szad);
	base_nodes = NULL;

	return (false);
}

void
base_prefork(void)
{

	malloc_mutex_prefork(&base_mtx);
}

void
base_postfork_parent(void)
{

	malloc_mutex_postfork_parent(&base_mtx);
}

void
base_postfork_child(void)
{

	malloc_mutex_postfork_child(&base_mtx);
}
