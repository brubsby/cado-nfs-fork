#include "cado.h"
#include <stdio.h>
#include "portability.h"
#include "utils.h"
#include "filter_utils.h"
#include "filter_memalloc.h"

/* index_plist is a list of blocks, each one of BLOCK_SIZE index_ts */
static index_t **index_plist = NULL;
static index_t *index_p;
static unsigned int index_used = BLOCK_SIZE; /* usage of current block */
static unsigned int index_psize = 0;  /* minimal size of relcompact_list */
static int index_pcurrent = -1; /* index of current block */

/* same thing but for ideal_merge_t */
static ideal_merge_t **idealmerge_plist = NULL;
static ideal_merge_t *idealmerge_p;
static unsigned int idealmerge_used = BLOCK_SIZE; /* usage of current block */
static unsigned int idealmerge_psize = 0;  /* minimal size of relcompact_list */
static int idealmerge_pcurrent = -1; /* index of current block */

static size_t my_malloc_bytes = 0;

/* return a pointer to an array of n (index_t) */
index_t *
index_my_malloc (unsigned int n)
{
  index_t *ptr;

  if (index_used + n > BLOCK_SIZE)
  {
    index_used = 0;
    if (((unsigned int) (++index_pcurrent)) == index_psize)
    {
      my_malloc_bytes -= index_psize * sizeof (index_t *);
      index_psize = index_psize ? (index_psize << 1) : INIT_NB_BLOCK;
      size_t tmp_size = index_psize * sizeof(index_t *);
      index_plist = (index_t **) realloc(index_plist, tmp_size);
      FATAL_ERROR_CHECK((index_plist == NULL), "realloc error");
      my_malloc_bytes += tmp_size;
    }
    index_plist[index_pcurrent] = (index_t*) malloc (BLOCK_INDEX_BYTES);
    FATAL_ERROR_CHECK((index_plist[index_pcurrent] == NULL), "malloc error");
    index_p = index_plist[index_pcurrent];
    my_malloc_bytes += BLOCK_INDEX_BYTES;
  }
  ptr = &(index_p[index_used]);
  index_used += n;
  return ptr;
}

/* return a pointer to an array of n (ideal_merge_t) */
ideal_merge_t *
idealmerge_my_malloc (unsigned int n)
{
  ideal_merge_t *ptr;

  if (idealmerge_used + n > BLOCK_SIZE)
  {
    idealmerge_used = 0;
    if (((unsigned int) (++idealmerge_pcurrent)) == idealmerge_psize)
    {
      my_malloc_bytes -= idealmerge_psize * sizeof (ideal_merge_t *);
      idealmerge_psize =
                     idealmerge_psize ? (idealmerge_psize << 1) : INIT_NB_BLOCK;
      size_t tmp_size = idealmerge_psize * sizeof(ideal_merge_t *);
      idealmerge_plist = (ideal_merge_t **) realloc(idealmerge_plist, tmp_size);
      FATAL_ERROR_CHECK((idealmerge_plist == NULL), "realloc error");
      my_malloc_bytes += tmp_size;
    }
    idealmerge_plist[idealmerge_pcurrent] =
                              (ideal_merge_t*) malloc (BLOCK_IDEALMERGE_BYTES);
    FATAL_ERROR_CHECK((idealmerge_plist[idealmerge_pcurrent] == NULL),
                                                                "malloc error");
    idealmerge_p = idealmerge_plist[idealmerge_pcurrent];
    my_malloc_bytes += BLOCK_INDEX_BYTES;
  }
  ptr = &(idealmerge_p[idealmerge_used]);
  idealmerge_used += n;
  return ptr;
}

void
my_malloc_free_all (void)
{
  for ( ; index_pcurrent >= 0; index_pcurrent--)
  {
    free(index_plist[index_pcurrent]);
    index_plist[index_pcurrent] = NULL;
  }
  for ( ; idealmerge_pcurrent >= 0; idealmerge_pcurrent--)
  {
    free(idealmerge_plist[idealmerge_pcurrent]);
    idealmerge_plist[idealmerge_pcurrent] = NULL;
  }
  free(index_plist);
  free(idealmerge_plist);
  index_plist = NULL;
  idealmerge_plist = NULL;
  index_used = idealmerge_used = BLOCK_SIZE;
  index_psize = idealmerge_psize = 0;
  my_malloc_bytes = 0;
}

inline size_t
get_my_malloc_bytes ()
{
  return my_malloc_bytes;
}
