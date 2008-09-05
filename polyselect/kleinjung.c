/* polynomial selection with Kleinjung's algorithm

   Reference:
   "On polynomial selection for the general number field sieve",
   Thorsten Kleinjung, Mathematics of Computation 75 (2006), p. 2037-2047.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h> /* for log, pow, fabs */
#include "cado.h"
#include "utils/utils.h"
#include "aux.h" /* for common routines with polyselect.c */

#define QUICK_SEARCH

#define P0_MAX 1000

/* if NEAREST is defined, round m0 and the x[i][j] to nearest, instead of
   towards +infinity as in the original Algorithm 3.6 */
#define NEAREST

int verbose = 0;
double search_time = 0.0;
m_logmu_t *Mt; /* stores values of m, b and log(mu) found in 1st phase */
unsigned long Malloc, Msize, Malloc2, Msize2;

/* Implements Lemma 2.1 from Kleinjung's paper.
   If a[d] is non-zero, it is assumed it is already set, otherwise it is
   determined as a[d] = N/m^d (mod p).
*/
void
Lemma21 (mpz_t *a, mpz_t N, int d, mpz_t p, mpz_t m)
{
  mpz_t r, mi, invp;
  int i;

  mpz_init (r);
  mpz_init (mi);
  mpz_init (invp);
  mpz_set (r, N);
  mpz_pow_ui (mi, m, d);
  if (mpz_cmp_ui (a[d], 0) == 0)
    {
      mpz_invert (a[d], mi, p); /* 1/m^d mod p */
      mpz_mul (a[d], a[d], N);
      mpz_mod (a[d], a[d], p);
    }
  for (i = d - 1; i >= 0; i--)
    {
      /* invariant: mi = m^(i+1) */
      mpz_mul (a[i], a[i+1], mi);
      mpz_sub (r, r, a[i]);
      ASSERT (mpz_divisible_p (r, p));
      mpz_divexact (r, r, p);
      mpz_divexact (mi, mi, m); /* now mi = m^i */
      if (i == d - 1)
        {
          mpz_invert (invp, p, mi); /* 1/p mod m^i */
          mpz_sub (invp, mi, invp); /* -1/p mod m^i */
        }
      else
        mpz_mod (invp, invp, mi);
      mpz_mul (a[i], invp, r);
      mpz_mod (a[i], a[i], mi); /* -r/p mod m^i */
      /* round to nearest in [-m^i/2, m^i/2] */
      mpz_mul_2exp (a[i], a[i], 1);
      if (mpz_cmp (a[i], mi) >= 0)
        {
          mpz_div_2exp (a[i], a[i], 1);
          mpz_sub (a[i], a[i], mi);
        }
      else
        mpz_div_2exp (a[i], a[i], 1);
      mpz_mul (a[i], a[i], p);
      mpz_add (a[i], a[i], r);
      ASSERT (mpz_divisible_p (a[i], mi));
      mpz_divexact (a[i], a[i], mi);
    }
  mpz_clear (r);
  mpz_clear (mi);
  mpz_clear (invp);
}

/* utility stuff for the searching algorithm */

typedef int (*sortfunc_t) (const void *a, const void * b);

int cmp(uint64_t * a, uint64_t * b)
{
    if (*a < *b) return -1;
    if (*b < *a) return 1;
    return 0;
}

void save_all_sums(uint64_t * dst, uint64_t ** g, int d, int l)
{
    int *mu, i;
    uint64_t *s;
    mu = (int*) malloc (l * sizeof (int));
    s = (uint64_t*) malloc ((l + 1) * sizeof (uint64_t));
    uint64_t * d0 = dst;

    for (i = 0; i < l; i++)
        mu[i] = 0;
    s[0] = 0;
    for (i = 1; i <= l; i++)
        s[i] = s[i-1] + g[i-1][mu[i-1]];

    while (1)
    {
        *dst++ = s[l];

        /* go to next combination */
        for (i = l - 1; i >= 0 && mu[i] == d - 1; i--);
        /* now either i < 0 and we are done, or mu[i] < d-1 */
        if (UNLIKELY(i < 0))
            break;
        mu[i] ++;
        s[i+1] = s[i] + g[i][mu[i]];
        while (++i < l)
        {
            mu[i] = 0;
            s[i+1] = s[i] + g[i][mu[i]];
        }
    }

    qsort(d0, dst-d0, sizeof(uint64_t), (sortfunc_t) cmp);

    free(s);
    free(mu);
}

/* Our second pass will be given a list of target sums to be matched.
 * We'll first do a dumb hash to make the tight loop just as fast as
 * above, and then search (later bsearch) through the list of targets.
 *
 * Results are given per target as a list (possibly with several
 * elements) of vectors mu. Those vectors are to be properly freed by the
 * caller.
 */

int * mu_dup(int * mu, int l)
{
    int * nmu = malloc(l * sizeof(int));
    int i;
    for(i = 0 ; i < l ; i++) nmu[i]=mu[i];
    return nmu;
}

struct mu_llist;
struct mu_llist {
    int * mu;
    struct mu_llist * next;
};

/* TODO: as we do the stuff, duplicates in the list of targets would have
 * the corresponding results list populated twice.
 */
void retrieve_sums(uint64_t * targets, struct mu_llist ** res, int ntargets,
        uint64_t ** g, int d, int l)
{
    /* we could even do a bitmap, but then we would have to do shifts in
     * the tight loop, which is probably not worth it. */
    unsigned char hash[256] = {0,};
    int i;
    for(i = 0 ; i < ntargets ; i++) {
        hash[targets[i] & 0xff]=1;
        res[i]=(struct mu_llist *) NULL;
    }
    int *mu;
    uint64_t *s;
    mu = (int*) malloc (l * sizeof (int));
    s = (uint64_t*) malloc ((l + 1) * sizeof (uint64_t));

    for (i = 0; i < l; i++)
        mu[i] = 0;
    s[0] = 0;
    for (i = 1; i <= l; i++)
        s[i] = s[i-1] + g[i-1][mu[i-1]];

    while (1) {
        uint64_t v = s[l];
        if (UNLIKELY(hash[v&0xff])) {
            int j;
            for(j = 0 ; j < ntargets ; j++) {
                if (v == targets[j]) {
                    /* fond someone to populate ! */
                    struct mu_llist * kid;
                    kid = malloc(sizeof(struct mu_llist));
                    kid->next = res[j];
                    kid->mu = mu_dup(mu,l);
                    res[j] = kid;
                    /* loop again because we're not sorting targets yet
                     */
                }
            }
            /* if we arrive here without finding anybody, too bad ; it's
             * a normal condition anyway since our only guard is the
             * one-byte hash above.
             */
        }

        /* go to next combination */
        for (i = l - 1; i >= 0 && mu[i] == d - 1; i--);
        /* now either i < 0 and we are done, or mu[i] < d-1 */
        if (UNLIKELY(i < 0))
            break;
        mu[i] ++;
        s[i+1] = s[i] + g[i][mu[i]];
        while (++i < l)
        {
            mu[i] = 0;
            s[i+1] = s[i] + g[i][mu[i]];
        }
    }

    free(s);
    free(mu);
}

/* checks a possible candidate */
static void
possible_candidate (int * mu, int l, int d, mpz_t *a, mpz_t P, mpz_t N,
                    double M, mpz_t **x, mpz_t m0)
{
    mpz_t t;
    int i;
    double lognorm, logM = log (M);

    mpz_init (t);
    mpz_add (t, m0, x[0][mu[0]]);
    for (i = 1; i < l; i++)
        mpz_add (t, t, x[i][mu[i]]);

    Lemma21 (a, N, d, P, t);
    // norm = sup_norm (a, d);
    lognorm = LOGNORM (a, d, SKEWNESS (a, d, SKEWNESS_DEFAULT_PREC));

    if (lognorm <= logM) {
        if (verbose)
          {
            gmp_printf ("ad=%Zd p=%Zd m=%Zd norm=%1.2e (log %1.2f)\n",
                        a[d], P, t, exp (lognorm), lognorm);
            /* terse output does not need the polynomial */
            if (verbose > 1)
              {
                fprint_polynomial (stdout, a, d);
                printf ("\n");
              }
            fflush (stdout);
        }
        m_logmu_insert (Mt, Malloc, &Msize, P, t, lognorm, "lognorm=", verbose);
    }
    mpz_clear (t);
}

struct expanding_list_s {
    uint64_t * v;
    size_t alloc;
    size_t size;
};
typedef struct expanding_list_s expanding_list[1];

void expanding_list_init(expanding_list l)
{
    l->alloc=16;
    l->size=0;
    l->v = malloc(l->alloc * sizeof(uint64_t));
}
void expanding_list_clear(expanding_list l)
{
    free(l->v);
    l->v=NULL;
    l->alloc=0;
    l->size=0;
}
void expanding_list_push(expanding_list l, uint64_t v)
{
    if (LIKELY(l->size < l->alloc)) {
        l->v[l->size++]=v;
    } else {
        l->alloc += l->alloc >> 1;
        l->v = realloc(l->v, l->alloc * sizeof(uint64_t));
    }
}

/* Same as naive_search, but in O~(d^(l/2)) instead of O(d^l).
   Returns the number of polynomials checked, i.e., d^l.
*/
double
quick_search (double f0, double **f, int l, int d, double eps, mpz_t *a,
              mpz_t P, mpz_t N, double M, mpz_t **x, mpz_t m0)
{
    uint64_t ** g;
    double two_64 = pow(2,64);
    uint64_t lim = 2 * eps * two_64;
    int found = 0;
    int i,j;
    unsigned int dlr, dll;
    unsigned int u;
    //    double t0=clock();

    // ASSERT_ALWAYS(0 <= eps && eps < 1);

    // double delta;

    /* We do integer arithmetic, scaled by 2^64 so that the wraparound of
     * the ALU exactly matches the integer wraparound we're looking for.
     */
    g = malloc(l * sizeof(uint64_t*));
    for(i = 0 ; i < l ; i++) {
        g[i] = malloc(d * sizeof(uint64_t));
    }

    /* Offset by epsilon, and add f0. */
    for(j = 0 ; j < d ; j++) {
        /* Beware -- conversion from double can yield _MIN or _MAX ! */
        double t;
        for(t = f0 + f[0][j] + eps ; t < 0 ; t += 1);
        for( ; t >= 1 ; t -= 1);
        g[0][j] = t * two_64;
    }
    for(i = 1 ; i < l ; i++) {
        for(j = 0 ; j < d ; j++) {
            double t;
            for(t = f[i][j] ; t < 0 ; t += 1);
            for( ; t >= 1 ; t -= 1);
            g[i][j] = t * two_64;
        }
    }

    int lcut = l - l/2;
    int rcut = l/2;

    /* l == lcut + rcut */

    dll = (long) pow(d, lcut);
    uint64_t * all_l = malloc(dll * sizeof(uint64_t));
    FATAL_ERROR_CHECK(all_l == NULL, "Not enough memory");

    dlr = (long) pow(d, rcut);


    /* This ``extra'' value is some replicated data after the all_r
     * array, so that we can avoid having to wrap around in the tight
     * pointer loop. We want to have all_l[0] matched with values which,
     * besides the positions all_r[0..x[, are also found in
     * all_r[dlr..dlr+x[ for some x. Afterwards, the pointers within
     * all_r merely have to go downwards while the pointer within all_l
     * goes upward.
     */
    /* We need to look ahead to see how far we will have to go to find
     * some value such that all_r[x]+all_l[0] > lim */
    unsigned int extra0 = 1 + 5.0 * eps * dlr;
    uint64_t * all_r = malloc((dlr + extra0) * sizeof(uint64_t));
    FATAL_ERROR_CHECK(all_r == NULL, "Not enough memory");

    // delta = -clock();

    save_all_sums(all_l, g, d, lcut);
    save_all_sums(all_r, g + lcut, d, rcut);

    unsigned int extra;
    unsigned int badluck = 1000 + 100 * eps * dlr;
    for(extra = eps * dlr ; extra < badluck && extra < dlr; extra++) {
        if (all_l[0] + all_r[extra] >= lim) {
            extra++;
            break;
        }
    }
    ASSERT_ALWAYS(!(extra == badluck || extra == dlr));
    ASSERT_ALWAYS(all_l[0] + all_r[extra-1] >= lim);

    if (extra > extra0) {
        /* unlikely to occur. */
        all_r = realloc(all_r, (dlr + extra) * sizeof(uint64_t));
    }

    ASSERT_ALWAYS(all_l[0] + all_r[extra-1] >= lim);
    /* wrap around, so that we can simplify the
     * inner loop */
    for(u = 0 ; u < extra ; u++) {
        all_r[dlr+u]=all_r[u];
    }

    /* This integer stores the difference between the two pointers within
     * all_r
     */
    unsigned int pd = 0;
    unsigned int rx = dlr + extra - 1;

    ASSERT_ALWAYS(all_l[0] + all_r[rx] >= lim);


    expanding_list ltargets;
    expanding_list rtargets;

    expanding_list_init(ltargets);
    expanding_list_init(rtargets);

    for(unsigned int lx = 0 ; lx < dll ; lx++) {
        unsigned int k = 0;
        /* arrange so that *rv is the furthermost value with sum < 0 */
        for( ; rx ; rx--, pd++) {
            if (((int64_t) (all_l[lx] + all_r[rx])) < 0)
                break;
        }
        /* arrange so that rv[pd] is the furthermost value with sum >=0
         * and < lim, but no further than rv */
        for( ; pd && all_l[lx] + all_r[rx+pd] >= lim ; pd--);
        /* The difference between the two pointers is exactly the number
         * of solutions. The exact solutions are those whose right part
         * is at [1]...[pd]
         */
        for(k = 0 ; k < pd ; k++) {
            /* l[lx] and r[rx+k+1] match together ! */
            expanding_list_push(ltargets, all_l[lx]);
            expanding_list_push(rtargets, all_r[rx+k+1]);
        }

        found += pd;
    }

    // delta += clock();

    if (verbose >= 3) {
        printf("# Found %d matches\n", found);
    }

    struct mu_llist ** lres;
    struct mu_llist ** rres;

    lres = malloc(ltargets->size  * sizeof(struct mu_llist));
    rres = malloc(rtargets->size  * sizeof(struct mu_llist));
    FATAL_ERROR_CHECK(lres == NULL, "Not enough memory");
    FATAL_ERROR_CHECK(rres == NULL, "Not enough memory");
    memset(lres, 0, ltargets->size  * sizeof(struct mu_llist));
    memset(rres, 0, rtargets->size  * sizeof(struct mu_llist));

    ASSERT(ltargets->size == rtargets->size);

    retrieve_sums(ltargets->v, lres, ltargets->size, g,d,lcut);
    retrieve_sums(rtargets->v, rres, rtargets->size, g+lcut,d,rcut);

    for(i = 0 ; i < l ; i++) {
        free(g[i]);
    }
    free(g);

    for(u = 0 ; u < ltargets->size ; u++) {
        struct mu_llist * ll;
        struct mu_llist * rr;
        int * mu = malloc(l*sizeof(int));
        for(ll = lres[u] ; ll ; ll = ll->next) {
            for(i = 0 ; i < lcut ; i++) {
                mu[i]=ll->mu[i];
            }
            for(rr = rres[u] ; rr ; rr = rr->next) {
                for(i = 0 ; i < rcut ; i++) {
                    mu[lcut+i]=rr->mu[i];
                }
                possible_candidate(mu,l,d,a,P,N,M,x,m0);
            }
        }
        struct mu_llist * q;
        for(ll = lres[u] ; ll ; ll = q) {
            free(ll->mu);
            q=ll->next;
            free(ll);
        }
        for(rr = rres[u] ; rr ; rr = q) {
            free(rr->mu);
            q=rr->next;
            free(rr);
        }
        free(mu);

    }

    expanding_list_clear(ltargets);
    expanding_list_clear(rtargets);
    free(lres);
    free(rres);
    free (all_l);
    free (all_r);

    /* return the number of checked polynomials, i.e., d^l */
    return pow ((double) d, (double) l);
}

/* Outputs all (mu[0], ..., mu[l-1]), 0 <= mu_i < d, such that S is at distance
   less than eps from an integer, with
   S = f0 + f[0][mu[0]] + ... + f[l-1][mu[l-1]].
   Assumes a[d] is set to the current search value.
   Returns then number of polynomials checked.
*/
double
naive_search (double f0, double **f, int l, int d, double eps, mpz_t *a,
	      mpz_t P, mpz_t N, double M, mpz_t **x, mpz_t m0)
{
  int *mu, i;
  double *s, fr;
  // double norm;
  mpz_t t;

    // ASSERT_ALWAYS(0 <= eps && eps < 1);

  if (verbose >= 3) printf("In naive_search()\n");
  mu = (int*) malloc (l * sizeof (int));
  s = (double*) malloc ((l + 1) * sizeof (double));
  /* s[i] contains f0 + f[0][mu[0]] + ... + f[i-1][mu[i-1]] */

  /* initializes mu[] to (0,...,0) */
  for (i = 0; i < l; i++)
    mu[i] = 0;
  s[0] = f0;
  for (i = 1; i <= l; i++)
    s[i] = s[i-1] + f[i-1][mu[i-1]];
  mpz_init (t);

  while (1)
    {
      /* check current sum, the following is a trick to avoid a call to
         the round() function, which is slow */
      fr = s[l] + 6755399441055744.0;
      fr = fr - 6755399441055744.0; /* fr = round(s[l]) */
      fr = fabs (fr - s[l]);
      if (UNLIKELY(fr <= eps)) /* Prob ~ 4e-7 on RSA155 with l=7, degree 5,
                                  M=5e24, pb=256, even less for smaller M */
        possible_candidate (mu, l, d, a, P, N, M, x, m0);
      
      /* go to next combination */
      for (i = l - 1; i >= 0 && mu[i] == d - 1; i--);
      /* now either i < 0 and we are done, or mu[i] < d-1 */
      if (UNLIKELY(i < 0))
	break;
      mu[i] ++;
      s[i+1] = s[i] + f[i][mu[i]];
      while (++i < l)
	{
	  mu[i] = 0;
	  s[i+1] = s[i] + f[i][mu[i]];
	}
    }

  mpz_clear (t);
  free (mu);
  free (s);

  return pow ((double) d, (double) l);
}

/* return rho if ad*x^d = N has exactly one root rho mod p0,
   otherwise returns 0. */
static unsigned int
has_one_root (mpz_t ad, int d, mpz_t N, unsigned int p0)
{
  unsigned int rho = 0, x, nroots = 0;
  residueul_t adr, t, xr, Nr;
  int i;
  modulusul_t p0r;

  modul_initmod_ul (p0r, p0);
  modul_set_ul (adr, mpz_fdiv_ui (ad, p0), p0r);
  modul_set_ul (Nr,  mpz_fdiv_ui (N,  p0), p0r);
  for (x = 1; nroots < 2 && x < p0; x ++)
    {
      modul_set_ul (xr, x, p0r);
      modul_mul (t, adr, xr, p0r);
      for (i = 1; i < d; i++)
        modul_mul (t, t, xr, p0r);
      modul_sub (t, t, Nr, p0r);
      if (modul_is0 (t, p0r))
        {
          nroots ++;
          rho = (nroots == 1) ? x : 0;
        }
    }
  modul_clearmod (p0r);
  return rho;
}

/* Enumerates all subsets of exactly l elements of Q, where Q has lQ elements,
   such that the product does not exceed max_adm1.
   Assumes a[d] is set to the current search value.
   Returns the number of polynomials checked.
*/
static double
enumerate (unsigned int *Q, int lQ, int l, double max_adm1, double max_adm2,
           mpz_t *a, mpz_t N, int d, mpz_t *g, mpz_t mtilde, double M)
{
  int *p, k, i, j;
  mpz_t **x, t, u, dad, e, e00, m0, invN, M0, x0;
  mpz_t P, P_over_pi, P_over_p0, P_over_2;
  mpz_t **x1; /* values of the x[][] for p0=1 */
  unsigned int pi, r0;
  unsigned long *roots;
  double eps, f0, **f, one_over_P2, checked = 0.0, Pd;

  p = (int*) malloc (l * sizeof (int));
  for (k = 0; k < l; k++)
    p[k] = k; /* p[] stores the current indices in [0, lQ-1] we consider */
  x = (mpz_t**) malloc (l * sizeof(mpz_t*));
  x1 = (mpz_t**) malloc (l * sizeof(mpz_t*));
  f = (double**) malloc (l * sizeof(double*));
  for (i = 0; i < l; i++)
    {
      x[i] = (mpz_t*) malloc (d * sizeof(mpz_t));
      x1[i] = (mpz_t*) malloc (d * sizeof(mpz_t));
      f[i] = (double*) malloc (d * sizeof(double));
      for (j = 0; j < d; j++)
        {
          mpz_init (x[i][j]);
          mpz_init (x1[i][j]);
        }
    }
  mpz_init (t);
  mpz_init (u);
  mpz_init (dad);
  mpz_init (e);
  mpz_init (e00);
  mpz_init (P_over_pi);
  mpz_init (P_over_p0);
  mpz_init (P_over_2);
  mpz_init (P);
  mpz_init (m0);
  mpz_init (x0);
  mpz_init (M0);
  mpz_init (invN);
  roots = (unsigned long*) malloc (d * sizeof(unsigned long));
  do
    {
      /* compute product of current subset */
      mpz_set_ui (P, Q[p[0]]);
      for (k = 1; k < l; k++)
        mpz_mul_ui (P, P, Q[p[k]]);

      Pd = mpz_get_d (P);
      if (Pd <= max_adm1)
        {
          unsigned int p0_max, p0;

          if (verbose >= 3)
            {
              printf("# subset");
              for (k = 0; k < l; k++)
                printf(" %u",Q[p[k]]);
              printf("\n");
            }

          /* p_0 idea */
          p0_max = (unsigned int) (max_adm1 / Pd);
          if (p0_max > P0_MAX)
            p0_max = P0_MAX;

          mpz_set (P_over_p0, P);
          for (p0 = 1; p0 <= p0_max; p0++)
            {
              if (p0 > 1 && p0 % d == 1) /* such primes are already taken */
                continue;

              if (p0 > 1 && (r0 = has_one_root (a[d], d, N, p0)) == 0)
                continue;
              
              mpz_mul_ui (P, P_over_p0, p0);

              if (verbose >= 2)
                gmp_printf ("trying P=%Zd\n", P);

          /* compute 1/N mod P */
          mpz_invert (invN, N, P);

          mpz_div_2exp (P_over_2, P, 1); /* floor(P/2) */
#ifndef NEAREST
          /* m0 is the smallest integer bigger than mtilde and divisible by P:
             m0 = s*P, where s = ceil(mtilde/P) = floor((mtilde + P - 1)/P) */
          mpz_add (t, mtilde, P);
          mpz_sub_ui (t, t, 1);
#else /* we round to nearest instead */
          mpz_add (t, mtilde, P_over_2);
#endif
          mpz_tdiv_q (t, t, P);
          mpz_mul (m0, t, P);
          eps = max_adm2 / mpz_get_d (m0);
          if (eps >= 1.0) {
              fprintf(stderr, "Warning, epsilon > 1, restricting to 1."
                      " M should be below %e\n",
                      pow(pow(mpz_get_d(mtilde),d-4)*pow(mpz_get_d(m0),d-2),1.0/(double)(2*d-6)));
              eps = 1.0;
          }

          /* compute f0 */
          mpz_pow_ui (t, m0, d);
          mpz_mul (t, t, a[d]);
          mpz_sub (t, N, t); /* N - a[d]*m0^d */
          f0 = mpz_get_d (t);
          mpz_pow_ui (t, m0, d - 1);
          mpz_mul (t, t, P);
          mpz_mul (t, t, P);
          f0 = f0 / mpz_get_d (t);
          
          if (verbose >= 3) printf("# xij...");
          /* compute the x[i][j] from (3.2) */
          for (i = 0; i < l; i++)
            {
              pi = Q[p[i]];
              if (p0 == 1)
                {
                  /* put in x[i][] the d roots of x^d = N/a[d] mod Q[p[i]] */
                  mpz_set_ui (u, pi);
                  mpz_invert (t, a[d], u);            /* 1/a[d] mod pi */
                  mpz_mul (t, t, N);
                  mpz_mod_ui (t, t, pi);
                  mpz_ui_sub (g[0], pi, t);
                  if (poly_roots_ulong(roots, g, d, pi) != d)
                    {
                      fprintf (stderr, "Error, d roots expected\n");
                      exit (1);
                    }
                  mpz_divexact_ui (P_over_pi, P, pi);
                  mpz_invert (t, P_over_pi, u);       /* 1 / (P/pi) mod pi */
                  for (j = 0; j < d; j++)
                    {
                      /* we want x[i][j] = c*(P/pi) and x[i][j] = roots[j] mod
                         pi, thus c = roots[j] / (P/pi) mod pi */
                      mpz_mul_ui (x[i][j], t, roots[j]);
                      mpz_mod_ui (x[i][j], x[i][j], pi);
                      mpz_mul (x[i][j], x[i][j], P_over_pi);
#ifdef NEAREST
                      /* round the x[i][j] to nearest */
                      if (mpz_cmp (x[i][j], P_over_2) > 0)
                        mpz_sub (x[i][j], x[i][j], P);
#endif
                      mpz_set (x1[i][j], x[i][j]);
                    }
                }
              else /* case p0 > 1 */
                {
                  unsigned long inv;

                  if (i == 0)
                    {
                      mpz_set_ui (u, p0);
                      mpz_invert (t, P_over_p0, u); /* 1/(P/p0) (mod p0) */
                      mpz_ui_sub (t, p0, t); /* -1/(P/p0) (mod p0) */
                      inv = mpz_get_ui (t);
                    }
                  for (j = 0; j < d; j++)
                    {
                      /* x1[i][j] is the value corresponding to p0=1, i.e.,
                         0 <= x1[i][j] < P/p0 and (P/p0)/pi divides x1[i][j];
                         we want x[i][j] = x1[i][j] + s*(P/p0) (which implies
                         (P/p0)/pi divides x[i][j]), and x[i][j] = 0 (mod p0).
                         This yields s = -x1[i][j]/(P/p0) (mod p0). */
                      unsigned long s;
                      s = mpz_fdiv_ui (x1[i][j], p0); /* x1[i][j] mod p0 */
                      mpz_mul_ui (x[i][j], P_over_p0, s * inv);
                      mpz_add (x[i][j], x[i][j], x1[i][j]);
                    }
                }
            } /* end of for-loop on i */
          if (verbose >= 3) printf("done\n");
          if (verbose >= 4) {
              for (i = 0; i < l; i++) {
                printf ("%u: ", Q[p[i]]);
                  for (j = 0; j < d; j++) {
                      gmp_printf("%Zd, ",x[i][j]);
                  }
                  gmp_printf("\n");
              }
          }

          /* The m[i][j], cf (3.3) are not needed, since m[i][j] = x[i][j]
             for i >= 1, and m[0][j] = m0 + x[0][j] */

          mpz_mul_ui (dad, a[d], d);
          Pd = mpz_get_d (P);
          one_over_P2 = -1.0 / (Pd * Pd);

          /* compute the e[i][j] from (3.6) */
          /* first compute e[0][j] = a_{d-1, (j,...,1)} */
          if (p0 > 1)
            {
              /* We need to add x0 to m0 + x[0][] + ... + x[d-1][],
                 such that x0 = r0 (mod p0) and x0 is divisible by P/p0;
                 Let x0 = t * (P/p0), then t = r0/(P/p0) mod (p0). */
              mpz_set_ui (u, p0);
              mpz_invert (t, P_over_p0, u);
              mpz_mul_ui (t, t, r0);
              mpz_fdiv_r_ui (t, t, p0);
#ifdef NEAREST
              if (mpz_cmp_ui (t, p0 >> 1) > 0)
                mpz_sub_ui (t, t, p0);
#endif
              mpz_mul (x0, t, P_over_p0);
              mpz_add (m0, m0, x0);
            }
          mpz_set (M0, m0);
          for (i = 0; i < l; i++)
            mpz_add (M0, M0, x[i][0]);
          mpz_set (t, M0);
          /* t = m0 + x_{(1,...,1)} = m_{(1,...,1)} */
          for (j = 0; j < d; j++)
            {
              if (j > 0)
                {
                  /* m_{(j,1,...,1)} = m_{(j-1,1,...,1)} + x_{1,j}-x_{1,j-1} */
                  mpz_sub (t, t, x[0][j - 1]);
                  mpz_add (t, t, x[0][j]);
                }
              mpz_pow_ui (u, t, d);
              mpz_mul (u, u, a[d]);
              mpz_sub (u, N, u);
              ASSERT (mpz_divisible_p (u, P));
              mpz_divexact (u, u, P);
              mpz_mul (u, u, invN);
              mpz_mul (u, u, a[d]);
              mpz_mul (u, u, t);
              mpz_mod (e, u, P);
              if (j == 0)
                mpz_set (e00, e);
              /* compute f[0][j] from x[0][j] and e[0][j] */
              mpz_mul (u, dad, x[0][j]);
              mpz_addmul (u, e, P);
              f[0][j] = mpz_get_d (u) * one_over_P2;
              ASSERT_ALWAYS(-2.0 < f[0][j] && f[0][j] < 1.0);
            }
          /* now compute e[i][j] and deduce f[i][j] for i > 0 */
          for (i = 1; i < l; i++)
            {
              /* since e[i][0] = e_{i,1} = 0, f[i][0] = -d a[d] x[i][0]/p^2 */
              mpz_mul (u, dad, x[i][0]);
              f[i][0] = mpz_get_d (u) * one_over_P2;
              ASSERT_ALWAYS(-2.0 < f[i][0] && f[i][0] < 1.0);
              mpz_set (t, M0);         /* m_{(1,...,1)} */
              for (j = 1; j < d; j++)
                {
                  /* since i >= 1, m[i][.] = x[i][.] */
                  mpz_sub (t, t, x[i][j - 1]);
                  mpz_add (t, t, x[i][j]);
                  /* now t = m_{(1,...,j,...,1)} where the j is at the
                     ith place */
                  mpz_pow_ui (u, t, d);
                  mpz_mul (u, u, a[d]);
                  mpz_sub (u, N, u);
                  ASSERT (mpz_divisible_p (u, P));
                  mpz_divexact (u, u, P);
                  mpz_mul (u, u, invN);
                  mpz_mul (u, u, a[d]);
                  mpz_mul (e, u, t);
                  mpz_sub (e, e, e00);
                  mpz_mod (e, e, P);
                  /* compute f[i][j] from x[i][j] and e[i][j] */
                  mpz_mul (u, dad, x[i][j]);
                  mpz_addmul (u, e, P);
                  f[i][j] = mpz_get_d (u) * one_over_P2;
                  ASSERT_ALWAYS(-2.0 < f[i][j] && f[i][j] < 1.0);
                }
            }

          /* now search for a small combination */
          search_time -= seconds ();
#ifdef QUICK_SEARCH
          checked += quick_search (f0, f, l, d, eps, a, P, N, M, x, m0);
#else
          checked += naive_search (f0, f, l, d, eps, a, P, N, M, x, m0);
#endif
          search_time += seconds ();
            } /* end of p0 loop */
        }
      
      /* go to next subset */
      for (k = l - 1; k >= 0 && p[k] == lQ - l + k; k--);
      if (k < 0)
        break;
      p[k] ++;
      while (++k < l)
        p[k] = p[k - 1] + 1;
    }
  while (1);
  for (i = 0; i < l; i++)
    {
      for (j = 0; j < d; j++)
        {
          mpz_clear (x[i][j]);
          mpz_clear (x1[i][j]);
        }
      free (x[i]);
      free (x1[i]);
      free (f[i]);
    }
  free (x);
  free (x1);
  free (f);
  mpz_clear (t);
  mpz_clear (u);
  mpz_clear (dad);
  mpz_clear (e);
  mpz_clear (e00);
  mpz_clear (P_over_pi);
  mpz_clear (P_over_p0);
  mpz_clear (P_over_2);
  mpz_clear (P);
  mpz_clear (m0);
  mpz_clear (x0);
  mpz_clear (M0);
  mpz_clear (invN);
  free (roots);
  free (p);

  return checked;
}

/* N is the number to factor
   d is the wanted degree
   M is the sup-norm bound
   l is the number of primes = 1 mod d in p
   pb is the prime bound for those primes
   incr is the increment for a[d]
   Returns the number of polynomials checked.
*/
static double
Algo36 (mpz_t N, unsigned int d, double M, unsigned int l, unsigned int pb,
        mpz_t incr, unsigned int keep)
{
  unsigned int *P = NULL, lP = 0;
  unsigned int *Q = NULL, lQ = 0;
  unsigned int r, i;
  mpz_t *a, *g, t, mtilde;
  double Nd, max_ad, max_adm1, max_adm2, checked = 0.0;

  ASSERT_ALWAYS (d >= 4);

  if (verbose) printf("# Step 1\n");
  /* step 1 */
  for (r = 1; r < pb; r += d)
    if (isprime (r))
      {
        if (mpz_divisible_ui_p (N, r))
          fprintf (stderr, "Warning, N is divisible by %u\n", r);
        else /* add r to P */
          {
            lP ++;
            P = (unsigned int*) realloc (P, lP * sizeof (unsigned int));
            P[lP - 1] = r;
          }
      }
  //  fprintf (stderr, "P has %u primes\n", lP);

  a = alloc_mpz_array (d + 1);
  g = alloc_mpz_array (d + 1);
  /* g will store the polynomial x^d - t */
  mpz_set_ui (g[d], 1);
  for (i = 1; i < d; i++)
    mpz_set_ui (g[i], 0);

  Nd = mpz_get_d (N);

  max_ad = pow (pow (M, (double) (2 * d - 2)) / Nd, 1.0 / (double) (d - 3));
  fprintf (stderr, "# max ad=%1.2e\n", max_ad);
  fflush (stderr);

  mpz_set (a[d], incr);

  Q = (unsigned int*) malloc (lP * sizeof (unsigned int));
  mpz_init (t);
  mpz_init (mtilde);

  /* step 2 */
  Msize = 0;
  while (Msize < keep && mpz_cmp_d (a[d], max_ad) <= 0)
    {
      for (i = lQ = 0; i < lP; i++)
        {
          /* add r = P[i] to Q if a[d]/N <> 0 and is a dth power modulo r */
          r = P[i];
          mpz_set_ui (t, r);
          mpz_invert (t, N, t);
          mpz_mul (t, t, a[d]);
          mpz_mod_ui (t, t, r);
          if (mpz_cmp_ui (t, 0) != 0)
            {
              mpz_set_si (g[0], - mpz_get_si (t));
              if (poly_roots_ulong (NULL, g, d, r) > 0)
                Q[lQ++] = r;
            }
        }
      if (lQ < l)
        goto next_ad;

      mpz_tdiv_q (mtilde, N, a[d]);
      mpz_root (mtilde, mtilde, d);
      max_adm1 = M * M / mpz_get_d (mtilde);
      max_adm2 = pow ((pow (M, (double) (2 * d - 6)) /
                      pow (mpz_get_d (mtilde), (double) (d - 4))),
                      1.0 / (double) (d - 2));

      if (verbose >= 2)
        gmp_printf ("# try ad=%Zd max_adm1=%e max_adm2=%e\n",
                    a[d], max_adm1, max_adm2);

      /* enumerate all subsets Pprime of at least l elements of Q such that
         prod(r, r in Pprime) <= max_adm1 */
      for (i = l; i <= lQ; i++)
        checked +=
          enumerate (Q, lQ, i, max_adm1, max_adm2, a, N, d, g, mtilde, M);

    next_ad:
      mpz_add (a[d], a[d], incr);
    }

  gmp_fprintf (stderr, "# stopped at ad=%Zd\n", a[d]);

  free (P);
  free (Q);
  clear_mpz_array (a, d + 1);
  clear_mpz_array (g, d + 1);
  mpz_clear (t);
  mpz_clear (mtilde);

  return checked;
}

static void
usage ()
{
  fprintf (stderr, "Usage: kleinjung [-v] [-degree d] [-keep k] [-incr i] [-l l] [-M M] [-pb p] < in\n\n");
  fprintf (stderr, "       -v        - verbose\n");
  fprintf (stderr, "       -full     - also output factor base parameters\n");
  fprintf (stderr, "       -degree d - use algebraic polynomial of degree d (default 5)\n");
  fprintf (stderr, "       -keep k   - keep k smallest polynomials (default 100)\n");
  fprintf (stderr, "       -incr i   - ad is incremented by i (default 60)\n");
  fprintf (stderr, "       -l l      - leading coefficient of g(x) has l prime factors (default 7)\n");
  fprintf (stderr, "       -M M      - keep polynomials with sup-norm <= M (default 1e25)\n");
  fprintf (stderr, "       -pb p   - prime factors are bounded by p (default 256)\n");
  fprintf (stderr, "       in        - input file (n:...)\n");
  exit (1);
}

int
main (int argc, char *argv[])
{
  int argc0 = argc;
  char **argv0 = argv;
  int degree = 5;
  unsigned int keep = 100;
  double M = 1e25;
  int l = 7;
  int pb = 256;
  mpz_t incr; /* Implements remark (1) following Algorithm 3.6:
                 try a[d]=incr, then 2*incr, 3*incr, ... */
  unsigned int i, best_i = -1;
  double checked, st0 = seconds (), st = st0;
  FILE * f = NULL;
  cado_poly poly;
  double E, best_E;
  int raw = 1;
  long jmin, kmin, bestj = 0, bestk = 0;

  /* print command line */
  fprintf (stderr, "# %s.r%s", argv[0], REV);
  for (i = 1; i < (unsigned int) argc; i++)
    fprintf (stderr, " %s", argv[i]);
  fprintf (stderr, "\n");

  param_list pl;
  mpz_t n;

  param_list_init(pl);
  mpz_init_set_ui(n,0);
  mpz_init_set_ui (incr, 60);

  argv++, argc--;
  for( ; argc ; ) {
      /* knobs first */
      if (strcmp(argv[0], "-v") == 0) { verbose++; argv++,argc--; continue; }
      if (strcmp(argv[0], "-full") == 0) { raw=0; argv++,argc--; continue; }
      /* Then aliases */
      if (param_list_update_cmdline_alias(pl, "degree", "-d", &argc, &argv))
          continue;
      if (param_list_update_cmdline_alias(pl, "degree", "d=", &argc, &argv))
          continue;
      if (param_list_update_cmdline_alias(pl, "incr", "-i", &argc, &argv))
          continue;
      /* Pick just everything from the rest that looks like a parameter */
      if (param_list_update_cmdline(pl, NULL, &argc, &argv)) { continue; }

      /* Now last resort measures */
      if (strspn(argv[0], "0123456789") == strlen(argv[0])) {
          param_list_add_key(pl, "n", argv[0], PARAMETER_FROM_CMDLINE);
          argv++,argc--;
          continue;
      }
      /* If something remains, then it could be an input file */
      if ((f = fopen(argv[0], "r")) != NULL) {
          param_list_read_stream(pl, f);
          fclose(f);
          argv++,argc--;
          continue;
      }
      /* bail out */
      fprintf(stderr, "Unhandled parameter %s\n", argv[0]);
      usage();
  }
  int have_n = param_list_parse_mpz(pl, "n", n);

  if (!have_n) {
      if (verbose) {
          fprintf(stderr, "Reading n from stdin\n");
      }
      param_list_read_stream(pl, stdin);
      have_n = param_list_parse_mpz(pl, "n", n);
  }

  if (!have_n) {
      fprintf(stderr, "No n defined ; sorry.\n");
      exit(1);
  }

  param_list_parse_uint(pl, "keep", &keep);
  param_list_parse_mpz(pl, "incr", incr);
  param_list_parse_int(pl, "l", &l);
  param_list_parse_int(pl, "pb", &pb);
  param_list_parse_double(pl, "M", &M);
  param_list_parse_int(pl, "degree", &degree);

  if (verbose) {
      param_list_display(pl, stdout);
  }

  if (param_list_warn_unused(pl)) {
      usage();
  }
  param_list_clear(pl);

  Malloc = 100; /* for Kleinjung's algorithm, keeping the 100 polynomials
                   of smallest norm is enough for the first phase */
  Mt = m_logmu_init (Malloc);
  cado_poly_init (poly);

  checked = Algo36 (n, degree, M, l, pb, incr, keep);

  fprintf (stderr, "# First phase took %.2fs, checked %1.0f and kept %lu polynomial(s)\n",
           seconds () - st, checked, Msize);
  fflush (stderr);

  /* Second/third phases: loop over entries in M database, and try to find the
     best rotation for each one. In principle we should compute the
     contribution to alpha(F) for primes up to the algebraic factor base bound,
     but since it is too expensive, we use a fixed bound.
     To speed up things we first compute an approximation of alpha(F) by
     considering small primes only, and keep only the best polynomials, for
     which we compute an approximation of alpha(F) up to a larger prime bound.
  */
  Malloc2 = 10; /* we keep the best 10 polynomials only */
  Msize2 = 0;
  st = seconds ();
  for (i = 0, best_E = DBL_MAX; i < Msize; i++)
    {
      if (Msize <= Malloc2) /* skip second phase */
        {
          Msize2 = Msize;
          break;
        }
      mpz_set_ui (poly->f[degree], 0);
      Lemma21 (poly->f, n, degree, Mt[i].b, Mt[i].m);
      /* we do not use translation here, since it has little effect on the
         norm, and moreover it does not permute with the base-m generation,
         thus we would need to save Mt[i].m, and redo all steps in the same
         order below */
      E = rotate (poly->f, degree, ALPHA_BOUND_SMALL, Mt[i].m, Mt[i].b, &jmin,
                  &kmin, 0);
      if (E < best_E)
        {
          best_E = E;
          gmp_fprintf (stderr, "# p=%Zd m=%Zd E~%1.2f\n", Mt[i].b, Mt[i].m, E);
          best_i = i;
        }
      m_logmu_insert (Mt, Malloc2, &Msize2, Mt[i].b, Mt[i].m, E, "E~", verbose);
    }
  fprintf (stderr, "# Second phase took %.2fs and kept %lu polynomial(s)\n",
           seconds () - st, Msize2);
  fflush (stderr);

  st = seconds ();
  for (i = 0, best_E = DBL_MAX; i < Msize2; i++)
    {
      mpz_set_ui (poly->f[degree], 0);
      Lemma21 (poly->f, n, degree, Mt[i].b, Mt[i].m);
      /* optimize norm before root properties */
      mpz_set (poly->g[1], Mt[i].b);
      mpz_neg (poly->g[0], Mt[i].m);
      optimize (poly->f, degree, poly->g, verbose);
      E = rotate (poly->f, degree, ALPHA_BOUND, Mt[i].m, Mt[i].b, &jmin,
                  &kmin, 1);
      if (E < best_E)
        {
          best_E = E;
          gmp_fprintf (stderr, "# p=%Zd m=%Zd E=%1.2f\n", Mt[i].b, Mt[i].m, E);
          best_i = i;
          bestj = jmin;
          bestk = kmin;
        }
    }
  fprintf (stderr, "# Third phase took %.2fs\n", seconds () - st);
  fflush (stderr);

  if (best_E == DBL_MAX)
    {
      fprintf (stderr, "No polynomial found, please increase M\n");
      exit (1);
    }

  /* regenerate best polynomial */
  mpz_set_ui (poly->f[degree], 0);
  i = best_i;
  Lemma21 (poly->f, n, degree, Mt[i].b, Mt[i].m);
  /* optimize again norm, to start from same polynomial before rotation */
  mpz_set (poly->g[1], Mt[i].b);
  mpz_neg (poly->g[0], Mt[i].m);
  optimize (poly->f, degree, poly->g, 0);
  rotate_aux (poly->f, Mt[i].b, Mt[i].m, 0, bestk);
  rotate_aux1 (poly->f, Mt[i].b, Mt[i].m, 0, bestj);
  translate (poly->f, degree, poly->g, Mt[i].m, Mt[i].b, verbose);

  mpz_set (poly->n, n);
  poly->degree = degree;
  mpz_set (poly->g[1], Mt[i].b);
  mpz_neg (poly->g[0], Mt[i].m);
  poly->skew = SKEWNESS (poly->f, degree, 2 * SKEWNESS_DEFAULT_PREC);
  strncpy (poly->type, "gnfs", sizeof (poly->type));
  print_poly (stdout, poly, argc0, argv0, st0, raw);

  m_logmu_clear (Mt, Malloc);
  cado_poly_clear (poly);
  mpz_clear (n);
  mpz_clear (incr);

  return 0;
}
