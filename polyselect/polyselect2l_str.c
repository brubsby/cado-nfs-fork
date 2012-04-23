/* Data struct used for polyselect2l */
#include "polyselect2l_str.h"

/* LEN_SPECIAL_Q in the header */
const unsigned int SPECIAL_Q[LEN_SPECIAL_Q] = {
  2, 3, 5, 7, 11, 13, 17, 19, 23, 29,
  31, 37, 41, 43, 47, 53, 59, 61, 67, 71,
  73, 79, 83, 89, 97, 101, 103, 107, 109, 113,
  127, 131, 137, 139, 149, 151, 157, 163, 167, 173,
  179, 181, 191, 193, 197, 199, 211, 223, 227, 229,
  233, 239, 241, 251, 0 };

//#define LESS_P

/* init prime array */
unsigned long
initPrimes ( unsigned long P,
             uint32_t **primes )
{
  unsigned long p, nprimes = 0;

#ifdef LESS_P // if impatient for root finding
  unsigned long maxprimes = (unsigned long) (1.2 * (double) P) /
    log (1.2 * (double) P) - (double) P / log ((double) P);
#else
  unsigned long maxprimes = (unsigned long) 2.0 * (double) P /
    log (2.0 * (double) P) - (double) P / log ((double) P);
#endif

  *primes = (uint32_t*) malloc (maxprimes * sizeof (uint32_t));
  if ( (*primes) == NULL) {
    fprintf (stderr, "Error, cannot allocate memory in initPrimes\n");
    exit (1);
  }

  for (p = 2; p < P; p = getprime (p));

#ifdef LESS_P
  while (p <= (P + P/5)) {
#else
  while (p <= 2 * P) {
#endif
    if (nprimes + 1 >= maxprimes) {
      maxprimes += maxprimes / 10;
      *primes = (uint32_t*) realloc (*primes, maxprimes * sizeof (uint32_t));
      if ( (*primes) == NULL) {
        fprintf (stderr, "Error, cannot reallocate memory in initPrimes\n");
        exit (1);
      }
    }
    (*primes)[nprimes++] = p;
    p = getprime (p);
  }

  getprime (0); /* free the memory used by getprime */

  *primes = (uint32_t*) realloc (*primes, (nprimes) * sizeof (uint32_t));
  if ( (*primes) == NULL) {
    fprintf (stderr, "Error, cannot allocate memory in initPrimes\n");
    exit (1);
  }

  return nprimes;
}


/* clear prime array */
void
printPrimes ( uint32_t *primes,
              unsigned long size )
{
  unsigned long i;
  for (i = 0; i < size; i++) {
    fprintf (stderr, "(%lu, %"PRIu32") ", i, primes[i]);
    if ((i+1) % 5 == 0)
      fprintf (stderr, "\n");
  }
  fprintf (stderr, "\n");
}


/* clear prime array */
void
clearPrimes ( uint32_t **primes )
{
  free (*primes);
}


/* init the header struct */
void
header_init ( header_t header,
              mpz_t N,
              unsigned long d,
              unsigned long ad )
{
  /* compute Ntilde, m0 */
  mpz_init_set (header->N, N);
  mpz_init (header->Ntilde);
  mpz_init (header->m0);
  header->d = d;
  header->ad = ad;

  /* compute Ntilde, ... from N, ... */
  mpz_set_ui (header->Ntilde, header->ad);
  mpz_mul_ui (header->Ntilde, header->Ntilde, header->d);
  mpz_pow_ui (header->Ntilde, header->Ntilde, header->d - 1);
  mpz_mul_ui (header->Ntilde, header->Ntilde, header->d);
  mpz_mul (header->Ntilde, header->Ntilde, header->N); /* d^d * ad^(d-1) * N */
  mpz_root (header->m0, header->Ntilde, header->d);
}


/* clear header struct */
void
header_clear ( header_t header )
{
  mpz_clear (header->m0);
  mpz_clear (header->Ntilde);
  mpz_clear (header->N);
}


/* init proots_t */
void
proots_init ( proots_t R,
              unsigned long size )
{
  R->size = size;

  /* length of nr&roots is known now. lengths of roots[i] are TBD. */
  R->nr = malloc (size * sizeof (unsigned int));
  R->roots = malloc (size * sizeof (uint64_t*));

  if (R->nr == NULL || R->roots == NULL) {
    fprintf (stderr, "Error, cannot allocate memory in proots_init().\n");
    exit (1);
  }
}


/* add a root to proots_t */
void
proots_add ( proots_t R,
             unsigned long nr,
             unsigned long *roots,
             unsigned long index )
{
  unsigned int i;
  R->nr[index] = nr;

  if (nr != 0) {
    R->roots[index] = malloc (nr * sizeof (uint64_t));

    if (R->roots[index] == NULL) {
      fprintf (stderr, "Error, cannot allocate memory in proots_add\n");
      exit (1);
    }

    for (i = 0; i < nr; i++)
      R->roots[index][i] = roots[i];
  }
  else
    R->roots[index] = NULL;
}


/* print roots */
void
proots_print ( proots_t R,
               unsigned long size )
{
  unsigned int i, j;
  for (i = 0; i < size; i++) {
    if (R->nr[i] == 0) {
      fprintf (stderr, "NULL\n");
    }
    else {
      for (j = 0; j < R->nr[i]; j ++)
        fprintf (stderr, "%lu ", R->roots[i][j]);
      fprintf (stderr, "\n");
    }
  }
}


/* clear roots */
void
proots_clear ( proots_t R,
               unsigned long size )
{
  unsigned int i;

  free (R->nr);
  for (i = 0; i < size; i++)
    free (R->roots[i]);
  free (R->roots);
}


void
qroots_init (qroots_t R)
{
  R->alloc = 0;
  R->size = 0;
  R->q = NULL;
  R->nr = NULL;
  R->roots = NULL;
}

void
qroots_realloc (qroots_t R, unsigned long newalloc)
{
  assert (newalloc >= R->size);
  R->alloc = newalloc;
  R->q = realloc (R->q, newalloc * sizeof (unsigned int));
  if (R->q == NULL)
  {
    fprintf (stderr, "Error, cannot reallocate memory in roots_realloc\n");
    exit (1);
  }
  R->nr = realloc (R->nr, newalloc * sizeof (unsigned int));
  if (R->nr == NULL)
  {
    fprintf (stderr, "Error, cannot reallocate memory in roots_realloc\n");
    exit (1);
  }
  R->roots = realloc (R->roots, newalloc * sizeof (unsigned long*));
  if (R->roots == NULL)
  {
    fprintf (stderr, "Error, cannot reallocate memory in roots_realloc\n");
    exit (1);
  }
}

void
qroots_add (qroots_t R, unsigned int q, unsigned int nr, unsigned long *roots)
{
  unsigned int i;

  if (nr == 0)
    return;
  if (R->size == R->alloc)
    qroots_realloc (R, R->alloc + R->alloc / 2 + 1);
  R->q[R->size] = q;
  R->nr[R->size] = nr;
  R->roots[R->size] = malloc (nr * sizeof (unsigned long));
  if (R->roots[R->size] == NULL)
  {
    fprintf (stderr, "Error, cannot allocate memory in roots_add\n");
    exit (1);
  }
  for (i = 0; i < nr; i++)
    R->roots[R->size][i] = roots[i];
  R->size ++;
}

void
qroots_print (qroots_t R)
{
  unsigned int i, j;
  for (i = 0; i < R->size; i++) {
    fprintf (stderr, "p: %u, r: ", R->q[i]);
    for (j = 0; j < R->nr[i]; j ++)
      fprintf (stderr, "%lu ", R->roots[i][j]);
    fprintf (stderr, "\n");
  }
}

void
qroots_clear (qroots_t R)
{
  unsigned int i;

  free (R->q);
  free (R->nr);
  for (i = 0; i < R->size; i++)
    free (R->roots[i]);
  free (R->roots);
}


/* init hash table */
void
hash_init (hash_t H, unsigned long init_size)
{
  unsigned long j;

  H->alloc = init_size;
  H->p = (uint32_t*) malloc (H->alloc * sizeof (uint32_t));
  if (H->p == NULL)
  {
    fprintf (stderr, "Error, cannot allocate memory in hash_init\n");
    exit (1);
  }
  H->i = (int64_t*) malloc (H->alloc * sizeof (int64_t));
  if (H->i == NULL)
  {
    fprintf (stderr, "Error, cannot allocate memory in hash_init\n");
    exit (1);
  }
  for (j = 0; j < H->alloc; j++)
    H->p[j] = 0;
  H->size = 0;
}


/* rq is a root of N = (m0 + rq)^d mod (q^2) */
void
hash_add (hash_t H, unsigned long p, int64_t i, mpz_t m0, unsigned long ad,
          unsigned int d, mpz_t N, unsigned long q, mpz_t rq)
{
  unsigned long h;

  if (H->size >= H->alloc)
    hash_grow (H);
  if (i >= 0)
    // h = i % H->alloc;
    h = ((int)i) % H->alloc;
  else
  {
    // h = H->alloc - ( (-i) % H->alloc );
    h = H->alloc - ( ((int)(-i)) % H->alloc);
    if (h == H->alloc)
      h = 0;
  }

  while (H->p[h] != 0)
  {
    if (m0 != NULL && H->i[h] == i && H->p[h] != p)
      match (H->p[h], p, i, m0, ad, d, N, q, rq);
    if (++h == H->alloc)
      h = 0;
  }
  H->p[h] = p;
  H->i[h] = i;
  H->size ++;
}

void
hash_clear (hash_t H)
{
  free (H->p);
  free (H->i);
}

void
hash_grow (hash_t H)
{
  unsigned long j, old_alloc;
  uint32_t *old_p;
  int64_t *old_i;
  mpz_t tmp;
  mpz_init (tmp);
  mpz_set_ui (tmp, 0);

  old_alloc = H->alloc;
  old_p = H->p;
  old_i = H->i;

  H->alloc = 2 * old_alloc;
  H->p = (uint32_t*) malloc (H->alloc * sizeof (uint32_t));
  if (H->p == NULL)
  {
    fprintf (stderr, "Error, cannot allocate memory in hash_grow\n");
    exit (1);
  }
  for (j = 0; j < H->alloc; j++)
    H->p[j] = 0;
  H->i = (int64_t*) malloc (H->alloc * sizeof (int64_t));
  if (H->i == NULL)
  {
    fprintf (stderr, "Error, cannot allocate memory in hash_grow\n");
    exit (1);
  }
  H->size = 0;
  for (j = 0; j < old_alloc; j++)
    if (old_p[j] != 0)
      hash_add (H, old_p[j], old_i[j], NULL, 0, 0, NULL, 0, tmp);
  free (old_p);
  free (old_i);
  mpz_clear (tmp);

}


#if 0
double
hash_mean_value (hash_t H)
{
  double s = 0;
  unsigned long j;

  for (j = 0; j < H->alloc; j++)
    if (H->p[j] != 0)
      s += fabs ((double) H->i[j]);
  return s / (double) H->size;
}
#endif

