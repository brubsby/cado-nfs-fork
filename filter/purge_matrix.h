#ifndef PURGE_MATRICE_H_
#define PURGE_MATRICE_H_

struct purge_matrix_s
{
  uint64_t nrows_init;  /* number of initial rows */
  uint64_t nrows; /* number of active rows */
  uint64_t ncols; /* number of active columns */
  uint64_t col_max_index; /* Maximum possible value for indexes of columns*/
  uint64_t col_min_index; /* Only columns with indexes >= col_min_index are
                             considered */
  index_t **row_compact; /* rows as lists of indexes of columns */
  weight_t *cols_weight; /* weights of columns */
  bit_vector row_used; /* row_used[i] = 0 iff row i was deleted */
  uint64_t *sum2_row; /* sum of 2 row indexes for columns of weight 2 */
  size_t tot_alloc_bytes; /* To keep track of allocated memory */
};
typedef struct purge_matrix_s purge_matrix_t[1];
typedef struct purge_matrix_s * purge_matrix_ptr;
typedef const struct purge_matrix_s * purge_matrix_srcptr;


void purge_matrix_init (purge_matrix_ptr, uint64_t, uint64_t, uint64_t);
void purge_matrix_clear_row_compact (purge_matrix_ptr);
void purge_matrix_clear (purge_matrix_ptr);
void purge_matrix_clear_row_compact_update_mem_usage (purge_matrix_ptr);
void* purge_matrix_set_row_from_rel (purge_matrix_t, earlyparsed_relation_ptr);
void purge_matrix_delete_row (purge_matrix_ptr mat, uint64_t i);
void purge_matrix_compute_sum2_row (purge_matrix_ptr, unsigned int);

#define purge_matrix_compute_excess(m) (((int64_t)m->nrows)-((int64_t)m->ncols))

/* These 3 functions compute and print stats on rows weight, columns weight and
 * cliques (connected components) length. The stats can be expensive to
 * compute, so these functions should not be called by default. */
void purge_matrix_print_stats_columns_weight (FILE *, purge_matrix_srcptr, int);
void purge_matrix_print_stats_rows_weight (FILE *, purge_matrix_srcptr, int);
void purge_matrix_print_stats_on_cliques (FILE *, purge_matrix_srcptr, int);

#endif /* PURGE_MATRICE_H_ */
