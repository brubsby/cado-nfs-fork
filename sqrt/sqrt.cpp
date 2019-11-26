/* square root, it can be used in two ways:

 * either do everything in one call:

   sqrt -poly cxxx.poly -prefix cxxx.dep.gz -purged cxxx.purged.gz -index cxxx.index.gz -ker cxxx.kernel

 * or in two steps:

   sqrt -poly cxxx.poly -prefix cxxx.dep.gz -purged cxxx.purged.gz -index cxxx.index.gz -ker cxxx.kernel -ab
   sqrt -poly cxxx.poly -prefix cxxx.dep.gz -sqrt0 -sqrt1 -gcd
 */

#include "cado.h"
/* the following avoids the warnings "Unknown pragma" if OpenMP is not
   available, and should come after cado.h, which sets -Werror=all */
#ifdef  __GNUC__
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif
#include <stdint.h>     /* AIX wants it first (it's a bug) */
#include <stdio.h>
#include <stdlib.h>
#include <gmp.h>
#include <string.h>
#include <inttypes.h>
#include <math.h> /* for log */
#ifdef HAVE_RESOURCE_H
#include <sys/resource.h>
#endif
#include <sys/stat.h>
#include <errno.h>
#include <pthread.h>
#include "cxx_mpz.hpp"
#include <vector>
#include "fmt/format.h"
#include "fmt/printf.h"
#ifdef HAVE_OPENMP
#include <omp.h>
#endif

#include "utils_with_io.h"
#include "portability.h"

static int verbose = 0;

struct cxx_mpz_polymod_scaled {
  cxx_mpz_poly p;
  int v;
  cxx_mpz_polymod_scaled(int deg) : p(deg), v(0) {}
  cxx_mpz_polymod_scaled() = default;
  cxx_mpz_polymod_scaled(cxx_mpz_polymod_scaled const &) = delete;
  cxx_mpz_polymod_scaled(cxx_mpz_polymod_scaled &&) = default;
  cxx_mpz_polymod_scaled& operator=(cxx_mpz_polymod_scaled const &) = delete;
  cxx_mpz_polymod_scaled& operator=(cxx_mpz_polymod_scaled &&) = default;
};

/* Pseudo-reduce a plain polynomial p modulo a non-monic polynomial F.
   The result is of type cxx_mpz_polymod_scaled P, and satisfies:
   P->p = lc(F)^P->v * p mod F.
   WARNING: this function destroys its input p !!! */
static void
cxx_mpz_polymod_scaled_reduce(cxx_mpz_polymod_scaled & P, cxx_mpz_poly & p, cxx_mpz_poly const & F) {
  int v = 0;

  if (p->deg < F->deg) {
    mpz_poly_set(P.p, p);
    P.v = 0;
    return;
  }

  const int d = F->deg;

  while (p->deg >= d) {
    const int k = p->deg;
    int i;

    /* We compute F[d]*p - p[k]*F. In case F[d] divides p[k], we can simply
       compute p - p[k]/F[d]*F. However this will happen rarely with
       Kleinjung's polynomial selection, since lc(F) is large. */

    /* FIXME: in msieve, Jason Papadopoulos reduces by F[d]^d*F(x/F[d])
       instead of F(x). This might avoid one of the for-loops below. */

    // temporary hack: account for the possibility that we're indeed
    // using f_hat instead of f.
    if (mpz_cmp_ui(F->coeff[d], 1) != 0) {
      v++; /* we consider p/F[d]^v */
      for (i = 0; i < k; ++i)
        mpz_mul (p->coeff[i], p->coeff[i], F->coeff[d]);
    }

    for (i = 0; i < d; ++i)
      mpz_submul (p->coeff[k-d+i], p->coeff[k], F->coeff[i]);

    mpz_poly_cleandeg (p, k-1);
  }

  mpz_poly_set(P.p, p);
  P.v = v;
}

/* Set Q=P1*P2 (mod F). Warning: Q might equal P1 (or P2). */
void
cxx_mpz_polymod_scaled_mul (cxx_mpz_polymod_scaled & Q, cxx_mpz_polymod_scaled const & P1, cxx_mpz_polymod_scaled const & P2,
                   cxx_mpz_poly const & F) {
  int v;

  /* beware: if P1 and P2 are zero, P1.p->deg + P2.p->deg = -2 */
  cxx_mpz_poly prd ((P1.p->deg == -1) ? -1 : P1.p->deg + P2.p->deg);

  ASSERT_ALWAYS(mpz_poly_normalized_p (P1.p));
  ASSERT_ALWAYS(mpz_poly_normalized_p (P2.p));

  mpz_poly_mul (prd, P1.p, P2.p);
  v = P1.v + P2.v;

  cxx_mpz_polymod_scaled_reduce (Q, prd, F);
  Q.v += v;
}

/********** RATSQRT **********/

#define THRESHOLD 2 /* must be >= 2 */

static char*
get_depname (const char *prefix, const char *algrat, int numdep)
{
  char *depname;
  const char* suffixes[] = {".gz", ".bz2", ".lzma", ""};
  const char *suffix;
  char *prefix_base;
  int ret;

  for (int i = 0; strlen (suffix = suffixes[i]) != 0; i++)
    if (strcmp (prefix + strlen (prefix) - strlen (suffix), suffix) == 0)
      break;
  prefix_base = strndup(prefix, strlen (prefix) - strlen (suffix));
  ASSERT_ALWAYS(prefix_base != NULL);
  ret = asprintf (&depname, "%s.%s%03d%s", prefix_base, algrat, numdep, suffix);
  ASSERT_ALWAYS(ret > 0);
  free (prefix_base);
  return depname;
}

static char*
get_depsidename (const char *prefix, int numdep, int side)
{
  char S[10];
  snprintf(S, 10, "side%d.", side);
  return get_depname (prefix, S, numdep);
}

static FILE*
fopen_maybe_compressed_lock (const char * name, const char * mode)
{
  FILE *fp;

#pragma omp critical
  fp = fopen_maybe_compressed (name, mode);
  return fp;
}

static int
fclose_maybe_compressed_lock (FILE * f, const char * name)
{
  int ret;

#pragma omp critical
  ret = fclose_maybe_compressed (f, name);
  return ret;
}

/* this function is run sequentially, thus no need to be thread-safe */
static int
check_dep (const char *prefix, int numdep)
{
  char *depname;
  FILE *depfile;

  depname = get_depname (prefix, "", numdep);
  depfile = fopen (depname, "r");
  free (depname);
  if (depfile == NULL)
    return 0;
  fclose (depfile);
  return 1;
}

/* replace the vector of elements of type T by
 * the product of elements in the range. It's fairly trivial to do this
 * in a DFS manner, but it's slightly less so for a BFS algorithm. The
 * latter is more amenable to parallelization.
 *
 * The most balanced split is of course when the size of the vector v is a
 * power of two. When the size of the vector v is N=2^k+r, the optimal way
 * to fall back to the balanced case is as follows. Form a vector w of
 * length 2^k by moving all elements of the vector v to the vector w, one
 * by one, except that the i-th element of w is created from *two*
 * consecutive elements of v whenever the bit-reversal of i is less than
 * r.
 *
 * Of course we prefer to create w from v in place, which is done easily
 * by having two pointers go through v.
 */
static inline uint64_t bitrev(uint64_t a)
{
    a = (a >> 32) ^ (a << 32);
    uint64_t m;
    m = UINT64_C(0x0000ffff0000ffff);
    a = ((a >> 16) & m) ^ ((a << 16) & ~m);
    m = UINT64_C(0x00ff00ff00ff00ff);
    a = ((a >> 8) & m) ^ ((a << 8) & ~m);
    m = UINT64_C(0x0f0f0f0f0f0f0f0f);
    a = ((a >> 4) & m) ^ ((a << 4) & ~m);
    m = UINT64_C(0x3333333333333333);
    a = ((a >> 2) & m) ^ ((a << 2) & ~m);
    m = UINT64_C(0x5555555555555555);
    a = ((a >> 1) & m) ^ ((a << 1) & ~m);
    return a;
}

cxx_mpz& operator*=(cxx_mpz & a, cxx_mpz & b)
{
    mpz_mul(a, a, b);
    return a;
}

template<typename T, typename M>
T accumulate(std::vector<T> & v, M const & m, std::string const & message)
{
    size_t vs = v.size();
    unsigned int nthr;

    #pragma omp parallel
    nthr = omp_get_num_threads (); /* total number of threads */

    if (v.size() < 16) {
        for(size_t j = 1 ; j < v.size() ; j++) {
	  m(v[0], v[0], v[j], nthr);
        }
        v.erase(v.begin() + 1, v.end());
    } else if ((vs & (vs - 1))) {
        size_t n = 0;
        for( ; vs >= 1UL << (n+1) ; n++) ;
        uint64_t r = vs - (1UL << n);
#pragma omp critical
	{
	  fmt::fprintf (stderr, "%s: doing level 00, %zu -> 2^%zu+%zu\n",
			message, vs, n, r);
	  fflush (stderr);
	}
        /* Need to make v of size a power of two */
        typename std::vector<T>::iterator read = v.begin(), write = v.begin();
        std::vector<uint64_t> incrs;
        for(uint64_t i = 0 ; i < 16 ; ++i) {
            incrs.push_back(bitrev(i) >> (64-n));
        }
        size_t nvs = 1UL << n;
        for(uint64_t i = 0 ; i < nvs ; i += 16) {
            uint64_t ir = bitrev(i) >> (64 - n);
            for(uint64_t j = 0 ; j < 16 && (i + j < nvs) ; j++) {
                uint64_t jr = ir + incrs[j];
                if (jr < r) {
		    m(*write, read[0], read[1], nthr);
                    write++;
                    read++;
                    read++;
                } else {
                    std::swap(*read, *write);
                    write++;
                    read++;
                }
            }
        }
        v.erase(write, v.end());
    }
    vs = v.size();
    ASSERT_ALWAYS(!(vs & (vs - 1)));

  for(int level = 0 ; v.size() > 1 ; level++) {
#pragma omp critical
      {
	fmt::fprintf (stderr, "%s: doing level %d, %zu values to multiply\n",
		      message, level, v.size());
	fflush (stderr);
      }

      size_t N = v.size() - 1;
      int local_nthreads;
      /* the loop below will compute N/2 products */
      if (nthr < (N / 2))
	local_nthreads = 1;
      else
	local_nthreads = nthr / (N / 2);
#pragma omp parallel for
      for(size_t j = 0 ; j < N ; j += 2) {
	  m(v[j], v[j], v[j+1], local_nthreads);
          v[j+1] = T();
      }

      /* shrink (not parallel) */
#pragma omp critical
      {
	fmt::fprintf (stderr, "%s: shrinking level %d\n", message, level);
	fflush (stderr);
      }
      for(size_t j = 2 ; j < v.size() ; j += 2) {
          std::swap(v[j], v[j/2]);
      }
      v.erase(v.begin() + (v.size() + 1) / 2, v.end());
  }
  return std::move(v.front());
}

int
calculateSqrtRat (const char *prefix, int numdep, cado_poly pol,
        int side, mpz_t Np)
{
  char *depname, *sidename;
  depname = get_depname (prefix, "", numdep);
  sidename = get_depsidename (prefix, numdep, side);
  FILE *depfile = NULL;
  FILE *resfile;
  int ret;
  unsigned long ab_pairs = 0, line_number, freerels = 0;

  ASSERT_ALWAYS (pol->pols[side]->deg == 1);

#pragma omp critical
  {
#ifdef __MPIR_VERSION
    fprintf (stderr, "Using MPIR %s\n", mpir_version);
#else
    fprintf (stderr, "Using GMP %s\n", gmp_version);
#endif
    fflush (stderr);
  }
  depfile = fopen_maybe_compressed_lock (depname, "rb");
  ASSERT_ALWAYS(depfile != NULL);

  line_number = 2;
  cxx_mpz a, b, v;
  std::vector<cxx_mpz> prd;

  for (;;)
    {
      ret = gmp_fscanf (depfile, "%Zd %Zd\n", (mpz_ptr) a, (mpz_ptr) b);

      if (ret != 2)
        {
          fprintf (stderr, "Invalid line %lu in file %s\n", line_number,
                   depname);
          fflush (stderr);
          break;
        }

      ab_pairs ++;
      line_number ++;

      if (ab_pairs % 1000000 == 0)
#pragma omp critical
        {
          fprintf (stderr, "Rat(%d): read %lu pairs in %1.2fs, (peak %luM)\n",
                   numdep, ab_pairs, seconds (),
                   PeakMemusage () >> 10);
	  fflush (stderr);
        }

      if (mpz_cmp_ui (b, 0) == 0)
        freerels ++;

      /* accumulate g1*a+g0*b */
      mpz_mul (v, pol->pols[side]->coeff[1], a);
      mpz_addmul (v, pol->pols[side]->coeff[0], b);

      prd.emplace_back(std::move(v));

      if (feof (depfile))
        break;
    }
  fclose_maybe_compressed_lock (depfile, depname);
  free (depname);

#pragma omp critical
  {
    fprintf (stderr, "Rat(%d): read %lu (a,b) pairs, including %lu free\n",
	     numdep, ab_pairs, freerels);
    fflush (stderr);
  }

  struct multiplier {
    void operator()(cxx_mpz & res, cxx_mpz const & a, cxx_mpz const & b, int MAYBE_UNUSED nthreads) const {
          mpz_mul(res, a, b);
      }
  };

  cxx_mpz prod = accumulate(prd, multiplier(), fmt::format("Rat({})", numdep));

  /* we must divide by g1^ab_pairs: if the number of (a,b) pairs is odd, we
     multiply by g1, and divide by g1^(ab_pairs+1) */
  if (ab_pairs & 1)
    mpz_mul (prod, prod, pol->pols[side]->coeff[1]);

#pragma omp critical
  {
    fprintf (stderr, "Rat(%d): size of product = %zu bits (peak %luM)\n",
	     numdep, mpz_sizeinbase (prod, 2),
	     PeakMemusage () >> 10);
    fflush (stderr);
  }

  if (mpz_sgn (prod) < 0)
    {
      fprintf (stderr, "Error, product is negative: try another dependency\n");
      exit (1);
    }

#pragma omp critical
  {
    fprintf (stderr, "Rat(%d): starting rational square root at %.2lfs\n",
	     numdep, seconds ());
    fflush (stderr);
  }

  /* since we know we have a square, take the square root */
  mpz_sqrtrem (prod, v, prod);

#pragma omp critical
  {
    fprintf (stderr, "Rat(%d): computed square root at %.2lfs\n",
	     numdep, seconds ());
    fflush (stderr);
  }

  if (mpz_cmp_ui (v, 0) != 0)
    {
      unsigned long p = 2, e, errors = 0;
      mpz_t pp;

      mpz_init (pp);
      fprintf (stderr, "Error, rational square root remainder is not zero\n");
      /* reconstruct the initial value of prod to debug */
      mpz_mul (prod, prod, prod);
      mpz_add (prod, prod, v);
      prime_info pi;
      prime_info_init (pi);
      while (mpz_cmp_ui (prod, 1) > 0)
        {
          e = 0;
          if (verbose)
            printf ("Removing p=%lu:", p);
          mpz_set_ui (pp, p);
          e = mpz_remove (prod, prod, pp);
          if (verbose)
            printf (" exponent=%lu, remaining %zu bits\n", e,
                    mpz_sizeinbase (prod, 2));
          if ((e % 2) != 0)
            {
              errors ++;
              fprintf (stderr, "Prime %lu appears to odd power %lu\n", p, e);
              if (verbose || errors >= 10)
                break;
            }
          p = getprime_mt (pi);
        }
      mpz_clear (pp);
      prime_info_clear (pi);
      exit (EXIT_FAILURE);
    }

  mpz_mod (prod, prod, Np);

#pragma omp critical
  {
    fprintf (stderr, "Rat(%d): reduced mod n at %.2lfs\n",
	     numdep, seconds ());
    fflush (stderr);
  }

  /* now divide by g1^(ab_pairs/2) if ab_pairs is even, and g1^((ab_pairs+1)/2)
     if ab_pairs is odd */

  mpz_powm_ui (v, pol->pols[side]->coeff[1], (ab_pairs + 1) / 2, Np);
#pragma omp critical
  {
    fprintf (stderr, "Rat(%d): computed g1^(nab/2) mod n at %.2lfs\n",
	     numdep, seconds ());
    fflush (stderr);
  }
  resfile = fopen_maybe_compressed_lock (sidename, "wb");

  mpz_invert (v, v, Np);
  mpz_mul (prod, prod, v);
  mpz_mod (prod, prod, Np);

  gmp_fprintf (resfile, "%Zd\n", (mpz_srcptr) prod);
  fclose_maybe_compressed_lock (resfile, sidename);
  free (sidename);

#pragma omp critical
  {
    gmp_fprintf (stderr, "Rat(%d): square root is %Zd\n", numdep, (mpz_srcptr) prod);
    fprintf (stderr, "Rat(%d): square root time: %.2lfs\n", numdep, seconds ());
    fflush (stderr);
  }

  return 0;
}

typedef struct
{
  const char *prefix;
  int task;            /* 0:ratsqrt 1:algsqrt 2:gcd */
  int numdep;
  cado_poly_ptr pol;
  int side;
  mpz_ptr Np;
} __tab_struct;
typedef __tab_struct tab_t[1];

/********** ALGSQRT **********/
static cxx_mpz_polymod_scaled
cxx_mpz_polymod_scaled_from_ab (cxx_mpz const & a, cxx_mpz const & b)
{
    if (mpz_cmp_ui (b, 0) == 0) {
        cxx_mpz_polymod_scaled tmp(0);
        mpz_set (tmp.p->coeff[0], a);
        mpz_poly_cleandeg(tmp.p, 0);
        return tmp;
    } else {
        cxx_mpz_polymod_scaled tmp(1);
        mpz_neg (tmp.p->coeff[1], b);
        mpz_set (tmp.p->coeff[0], a);
        mpz_poly_cleandeg(tmp.p, 1);
        return tmp;
    }
}

/* Reduce the coefficients of R in [-m/2, m/2) */
static void
mpz_poly_mod_center (mpz_poly R, const mpz_t m)
{
  int i;

  for (i=0; i <= R->deg; i++)
    mpz_ndiv_r (R->coeff[i], R->coeff[i], m);
}

#if 0
/* Check whether the coefficients of R (that are given modulo m) are in
   fact genuine integers. We assume that x mod m is a genuine integer if
   x or |x-m| is less than m/10^6, i.e., the bit size of x or |x-m| is
   less than that of m minus 20.
   Assumes the coefficients x satisfy 0 <= x < m.
*/
static int
mpz_poly_integer_reconstruction (mpz_poly R, const mpz_t m)
{
  int i;
  size_t sizem = mpz_sizeinbase (m, 2), sizer;

  for (i=0; i <= R->deg; ++i)
    {
      sizer = mpz_sizeinbase (R->coeff[i], 2);
      if (sizer + 20 > sizem)
        {
          mpz_sub (R->coeff[i], R->coeff[i], m);
          sizer = mpz_sizeinbase (R->coeff[i], 2);
          if (sizer + 20 > sizem)
            return 0;
        }
    }
  return 1;
}
#endif

// compute res := sqrt(a) in Fp[x]/f(x)
static void
TonelliShanks (mpz_poly res, const mpz_poly a, const mpz_poly F, unsigned long p)
{
  int d = F->deg;
  mpz_t q;
  mpz_poly delta;  // a non quadratic residue
  mpz_poly auxpol;
  mpz_t aux;
  mpz_t t;
  int s;
  mpz_t myp;

  mpz_init_set_ui(myp, p);

  mpz_init(aux);
  mpz_init(q);
  mpz_poly_init(auxpol, d);
  mpz_ui_pow_ui(q, p, (unsigned long)d);

  // compute aux = (q-1)/2
  // and (s,t) s.t.  q-1 = 2^s*t
  mpz_sub_ui(aux, q, 1);
  mpz_divexact_ui(aux, aux, 2);
  mpz_init_set(t, aux);
  s = 1;
  while (mpz_divisible_2exp_p(t, 1)) {
    s++;
    mpz_divexact_ui(t, t, 2);
  }
  // find a non quadratic residue delta
  {
    mpz_poly_init(delta, d);
    gmp_randstate_t state;
    gmp_randinit_default(state);
    do {
      int i;
      // pick a random delta
      for (i = 0; i < d; ++i)
	mpz_urandomm(delta->coeff[i], state, myp);
      mpz_poly_cleandeg(delta, d-1);
      // raise it to power (q-1)/2
      mpz_poly_pow_mod_f_mod_ui(auxpol, delta, F, aux, p);
      /* Warning: the coefficients of auxpol might either be reduced in
	 [0, p) or in [-p/2, p/2). This code should work in both cases. */
    } while (auxpol->deg != 0 || (mpz_cmp_ui (auxpol->coeff[0], p-1) != 0 &&
				  mpz_cmp_si (auxpol->coeff[0], -1) != 0));
    gmp_randclear (state);
  }

  // follow the description of Crandall-Pomerance, page 94
  {
    mpz_poly A, D;
    mpz_t m;
    int i;
    mpz_poly_init(A, d);
    mpz_poly_init(D, d);
    mpz_init_set_ui(m, 0);
    mpz_poly_pow_mod_f_mod_ui(A, a, F, t, p);
    mpz_poly_pow_mod_f_mod_ui(D, delta, F, t, p);
    for (i = 0; i <= s-1; ++i) {
      mpz_poly_pow_mod_f_mod_ui(auxpol, D, F, m, p);
      mpz_poly_mul_mod_f_mod_mpz(auxpol, auxpol, A, F, myp, NULL);
      mpz_ui_pow_ui(aux, 2, (s-1-i));
      mpz_poly_pow_mod_f_mod_ui(auxpol, auxpol, F, aux, p);
      if ((auxpol->deg == 0) && (mpz_cmp_ui(auxpol->coeff[0], p-1)== 0))
    mpz_add_ui(m, m, 1UL<<i);
    }
    mpz_add_ui(t, t, 1);
    mpz_divexact_ui(t, t, 2);
    mpz_poly_pow_mod_f_mod_ui(res, a, F, t, p);
    mpz_divexact_ui(m, m, 2);
    mpz_poly_pow_mod_f_mod_ui(auxpol, D, F, m, p);

    mpz_poly_mul_mod_f_mod_mpz(res, res, auxpol, F, myp, NULL);
    mpz_poly_clear(D);
    mpz_poly_clear(A);
    mpz_clear(m);
  }

  mpz_poly_clear(auxpol);
  mpz_poly_clear(delta);
  mpz_clear(q);
  mpz_clear(aux);
  mpz_clear(myp);
  mpz_clear (t);
}

// res <- Sqrt(AA) mod F, using p-adic lifting, at prime p.
void
cxx_mpz_polymod_scaled_sqrt (cxx_mpz_polymod_scaled & res, cxx_mpz_polymod_scaled & AA, cxx_mpz_poly const & F, unsigned long p,
	       int numdep)
{
  mpz_poly A, *P;
  int v;
  int d = F->deg;
  unsigned long k, target_k;
  unsigned long K[65];
  int lk, logk, logk0;
  size_t target_size; /* target bit size for Hensel lifting */

  /* The size of the coefficients of the square root of A should be about half
     the size of the coefficients of A. Here is an heuristic argument: let
     K = Q[x]/(f(x)) where f(x) is the algebraic polynomial. The square root
     r(x) might be considered as a random element of K: it is smooth, not far
     from an integer, but except that has no relationship with the coefficients
     of f(x). When we square r(x), we obtain a polynomial with coefficients
     twice as large, before reduction by f(x). The reduction modulo f(x)
     produces A(x), however that reduction should not decrease the size of
     the coefficients. */
  target_size = mpz_poly_sizeinbase (AA.p, 2);
  target_size = target_size / 2;
  target_size += target_size / 10;
#pragma omp critical
  {
    fprintf (stderr, "Alg(%d): target_size=%lu\n", numdep,
	     (unsigned long int) target_size);
    fflush (stderr);
  }

  mpz_poly_init(A, d-1);
  // Clean up the mess with denominator: if it is an odd power of fd,
  // then multiply num and denom by fd to make it even.
  mpz_poly_swap(A, AA.p);
  if (((AA.v)&1) == 0) {
    v = AA.v / 2;
  } else {
    v = (1+AA.v) / 2;
    mpz_poly_mul_mpz(A, A, F->coeff[d]);
  }

  // Now, we just have to take the square root of A (without denom) and
  // divide by fd^v.

  // Variables for the lifted values
  mpz_poly invsqrtA;
  // variables for A and F modulo pk
  mpz_poly a;
  mpz_poly_init(invsqrtA, d-1);
  mpz_poly_init(a, d-1);
  // variable for the current pk
  mpz_t pk;
  mpz_init (pk);

  /* Jason Papadopoulos's trick: since we will lift the square root of A to at
     most target_size bits, we can reduce A accordingly */
  double st = seconds ();
  target_k = (unsigned long) ((double) target_size * log ((double) 2) / log((double) p));
  mpz_ui_pow_ui (pk, p, target_k);
  while (mpz_sizeinbase (pk, 2) <= target_size)
    {
      mpz_mul_ui (pk, pk, p);
      target_k ++;
    }
  mpz_poly_mod_mpz (A, A, pk);
  for (k = target_k, logk = 0; k > 1; k = (k + 1) / 2, logk ++)
    K[logk] = k;
  K[logk] = 1;
#pragma omp critical
  {
    fprintf (stderr, "Alg(%d): reducing A mod p^%lu took %.2lfs\n", numdep,
	     target_k, seconds () - st);
    fflush (stderr);
  }

  // Initialize things modulo p:
  mpz_set_ui (pk, p);
  k = 1; /* invariant: pk = p^k */
  lk = 0; /* k = 2^lk */
  st = seconds ();
  P = mpz_poly_base_modp_init (A, p, K, logk0 = logk);
#pragma omp critical
  {
    fprintf (stderr, "Alg(%d): mpz_poly_base_modp_init took %.2lfs\n",
	     numdep, seconds () - st);
    if (verbose)
      {
	int i;
	size_t s = 0;
	for (i = 0; i <= logk; i++)
	  s += mpz_poly_totalsize (P[i]);
	fprintf (stderr, "Alg(%d): P takes %zuMb\n", numdep, s >> 20);
      }
    fflush (stderr);
  }

  /* A is not needed anymore, thus we can clear it now */
  mpz_poly_clear (A);

  mpz_poly_set (a, P[0]);

  // First compute the inverse square root modulo p
  {
    mpz_t q, aux;
    mpz_init(q);
    mpz_init(aux);
    mpz_ui_pow_ui(q, p, (unsigned long)d);

#if 0
    // compute (q-2)(q+1)/4   (assume q == 3 mod 4, here !!!!!)
    // where q = p^d, the size of the finite field extension.
    // since we work mod q-1, this gives (3*q-5)/4
    mpz_mul_ui(aux, q, 3);
    mpz_sub_ui(aux, aux, 5);
    mpz_divexact_ui(aux, aux, 4);               // aux := (3q-5)/4
    mpz_poly_pow_mod_f_mod_ui(invsqrtA, a, F, aux, p);
#else
    TonelliShanks(invsqrtA, a, F, p);
    mpz_sub_ui(aux, q, 2);
    mpz_poly_pow_mod_f_mod_ui(invsqrtA, invsqrtA, F, aux, p);
#endif

    mpz_clear(aux);
    mpz_clear(q);
  }

  // Now, the lift begins
  // When entering the loop, invsqrtA contains the inverse square root
  // of A computed modulo p.

  mpz_poly tmp;
  mpz_poly_init (tmp, 2*d-1);
  do {
    double st;

    if (mpz_sizeinbase (pk, 2) > target_size)
      {
        fprintf (stderr, "Failed to reconstruct an integer polynomial\n");
        printf ("Failed\n");
        exit (1);
      }

    /* invariant: invsqrtA = 1/sqrt(A) bmod p^k */

    lk += 1;
    st = seconds ();
    /* a <- a + pk*P[lk] */
    mpz_poly_base_modp_lift (a, P, lk, pk);
    /* free P[lk] which is no longer needed */
    mpz_poly_clear (P[lk]);
    st = seconds () - st;
    if (verbose)
#pragma omp critical
      {
	fprintf (stderr, "Alg(%d):    mpz_poly_base_modp_lift took %.2lfs (peak %luM)\n", numdep, st, PeakMemusage () >> 10);
	fprintf (stderr, "Alg(%d):    a takes %zuMb\n", numdep,
		 mpz_poly_totalsize (a) >> 20);
	fflush (stderr);
      }

    /* invariant: k = K[logk] */
    ASSERT_ALWAYS(k == K[logk]);

    mpz_mul (pk, pk, pk);   // double the current precision
    logk --;
    if (K[logk] & 1)
      {
        mpz_div_ui (pk, pk, p);
        k --;
      }
    k = K[logk];
#pragma omp critical
    {
      fprintf (stderr, "Alg(%d): start lifting mod p^%lu (%lu bits) at %.2lfs\n",
	       numdep, k, (unsigned long int) mpz_sizeinbase (pk, 2),
	       seconds ());
      fflush (stderr);
    }

    // now, do the Newton operation x <- 1/2(3*x-a*x^3)
    st = seconds ();
    mpz_poly_sqr_mod_f_mod_mpz (tmp, invsqrtA, F, pk, NULL); /* tmp = invsqrtA^2 */
    if (verbose)
#pragma omp critical
      {
        fprintf (stderr, "Alg(%d):    mpz_poly_sqr_mod_f_mod_mpz took %.2lfs (peak %luM)\n", numdep, seconds () - st, PeakMemusage () >> 10);
	fprintf (stderr, "Alg(%d):    tmp takes %zuMb\n", numdep,
		 mpz_poly_totalsize (tmp) >> 20);
        fflush (stderr);
      }

    /* Faster version which computes x <- x + x/2*(1-a*x^2).
       However I don't see how to use the fact that the coefficients
       if 1-a*x^2 are divisible by p^(k/2). */
    st = seconds ();
    mpz_poly_mul_mod_f_mod_mpz (tmp, tmp, a, F, pk, NULL); /* tmp=a*invsqrtA^2 */
    if (verbose)
#pragma omp critical
      {
        fprintf (stderr, "Alg(%d):    mpz_poly_mul_mod_f_mod_mpz took %.2lfs (peak %luM)\n", numdep, seconds () - st, PeakMemusage () >> 10);
	fprintf (stderr, "Alg(%d):    tmp takes %zuMb\n", numdep,
		 mpz_poly_totalsize (tmp) >> 20);
        fflush (stderr);
      }
    mpz_poly_sub_ui (tmp, tmp, 1); /* a*invsqrtA^2-1 */
    mpz_poly_div_2_mod_mpz (tmp, tmp, pk); /* (a*invsqrtA^2-1)/2 */
    st = seconds ();
    mpz_poly_mul_mod_f_mod_mpz (tmp, tmp, invsqrtA, F, pk, NULL);
    if (verbose)
#pragma omp critical
      {
        fprintf (stderr, "Alg(%d):    mpz_poly_mul_mod_f_mod_mpz took %.2lfs (peak %luM)\n", numdep, seconds () - st, PeakMemusage () >> 10);
	fprintf (stderr, "Alg(%d):    tmp takes %zuMb\n", numdep,
		 mpz_poly_totalsize (tmp) >> 20);
        fflush (stderr);
      }
    /* tmp = invsqrtA/2 * (a*invsqrtA^2-1) */
    mpz_poly_sub_mod_mpz (invsqrtA, invsqrtA, tmp, pk);
    if (verbose)
#pragma omp critical
      {
	fprintf (stderr, "Alg(%d):    invsqrtA takes %zuMb\n", numdep,
		 mpz_poly_totalsize (invsqrtA) >> 20);
        fflush (stderr);
      }

  } while (k < target_k);

  /* multiply by a to get an approximation of the square root */
  st = seconds ();
  mpz_poly_mul_mod_f_mod_mpz (tmp, invsqrtA, a, F, pk, NULL);
  if (verbose)
#pragma omp critical
    {
      fprintf (stderr, "Alg(%d):    final mpz_poly_mul_mod_f_mod_mpz took %.2lfs (peak %luM)\n", numdep, seconds () - st, PeakMemusage () >> 10);
      fprintf (stderr, "Alg(%d):    tmp takes %zuMb\n", numdep,
	       mpz_poly_totalsize (tmp) >> 20);
      fflush (stderr);
    }
  mpz_poly_mod_center (tmp, pk);

  mpz_poly_base_modp_clear (P, logk0);

  mpz_poly_set(res.p, tmp);
  res.v = v;

  mpz_clear (pk);
  mpz_poly_clear(tmp);
  mpz_poly_clear (invsqrtA);
  mpz_poly_clear (a);

  size_t sqrt_size = mpz_poly_sizeinbase (res.p, 2);
#pragma omp critical
  {
    fprintf (stderr, "Alg(%d): maximal sqrt bit-size = %zu (%.0f%% of target size)\n",
	     numdep, sqrt_size, 100.0 * (double) sqrt_size / target_size);
    fflush (stderr);
  }
}

static unsigned long
FindSuitableModP (mpz_poly F, mpz_t N)
{
  unsigned long p = 2;
  int dF = F->deg;

  modul_poly_t fp;

  modul_poly_init (fp, dF);
  prime_info pi;
  prime_info_init (pi);
  while (1)
    {
    int d;

    p = getprime_mt (pi);
    if (mpz_gcd_ui(NULL, N, p) != 1)
      continue;

    d = modul_poly_set_mod (fp, F, &p);
    if (d != dF)
      continue;
    if (modul_poly_is_irreducible (fp, &p))
      break;
    }
  modul_poly_clear (fp);
  prime_info_clear (pi);

  return p;
}

/*
   Process dependencies numdep to numdep + nthreads - 1.
*/
int
calculateSqrtAlg (const char *prefix, int numdep,
                  cado_poly_ptr pol, int side, mpz_t Np)
{
  char *depname, *sidename;
  FILE *depfile = NULL;
  FILE *resfile;
  unsigned long p;
  double t0 = seconds ();
  int nab = 0, nfree = 0;

  ASSERT_ALWAYS(side == 0 || side == 1);

  depname = get_depname (prefix, "", numdep);
  sidename = get_depsidename (prefix, numdep, side);
  depfile = fopen_maybe_compressed_lock (depname, "rb");
  ASSERT_ALWAYS(depfile != NULL);

  // Init F to be the corresponding polynomial
  cxx_mpz_poly F(pol->pols[side]);
  cxx_mpz_polymod_scaled prod;

  // Accumulate product with a subproduct tree
  {
      cxx_mpz a, b;
      std::vector<cxx_mpz_polymod_scaled> prd;
      while (gmp_fscanf(depfile, "%Zd %Zd", (mpz_ptr) a, (mpz_ptr) b) != EOF)
        {
          if(!(nab % 1000000))
#pragma omp critical
            {
              fprintf(stderr, "Alg(%d): reading ab pair #%d at %.2lfs (peak %luM)\n",
                      numdep, nab, seconds (), PeakMemusage () >> 10);
              fflush (stderr);
            }
          if (mpz_cmp_ui (a, 0) == 0 && mpz_cmp_ui (b, 0) == 0)
            break;
          prd.emplace_back(cxx_mpz_polymod_scaled_from_ab(a, b));
          nab++;
          if (mpz_cmp_ui (b, 0) == 0)
            nfree++;
        }
#pragma omp critical
      {
	fprintf (stderr, "Alg(%d): read %d including %d free relations\n",
		 numdep, nab, nfree);
	fflush (stderr);
      }
      ASSERT_ALWAYS ((nab & 1) == 0);
      ASSERT_ALWAYS ((nfree & 1) == 0);
      /* nfree being even is forced by a specific character column added
       * by character.c. The correspond assert should not fail.
       *
       * nab being even is kind of a mystery. There is a character column
       * that gives the sign of the rational side. It could well be that
       * with the parameters we usually use, it is negative for all
       * relations; that would force the overall number of relations to
       * be even. Another possibility is when f_d contains a big prime
       * number that does not occur anywhere else, so that the power of
       * this prime is controlled by the usual characters, and since f_d
       * is always present...
       *
       * But: I wouldn't be surprised if the assert(even(nab)) fails.
       * Then, a patch would be:
       *    - remove the assert (!)
       *    - in the numerator of the big product, eliminate powers of
       *       f_d that divides all coefficients.
       *    - this should finally give an even power of f_d in the
       *      denominator, and the algorithm can continue.
       */
      fclose_maybe_compressed_lock (depfile, depname);
      free (depname);

      struct multiplier {
          cxx_mpz_poly const & F;
          multiplier(cxx_mpz_poly & F) : F(F) {}
	void operator()(cxx_mpz_polymod_scaled &res, cxx_mpz_polymod_scaled const & a, cxx_mpz_polymod_scaled const & b, int nthreads) const {
	  omp_set_num_threads (nthreads);
	  cxx_mpz_polymod_scaled_mul(res, a, b, F);
          }
      };

      prod = accumulate(prd, multiplier(F), fmt::format("Alg({})", numdep));
    }

    p = FindSuitableModP(F, Np);
#pragma omp critical
    {
      fprintf (stderr, "Alg(%d): finished accumulating product at %.2lfs\n",
	       numdep, seconds());
      fprintf (stderr, "Alg(%d): nab = %d, nfree = %d, v = %d\n", numdep,
	       nab, nfree, prod.v);
      fprintf (stderr, "Alg(%d): maximal polynomial bit-size = %lu\n", numdep,
	       (unsigned long) mpz_poly_sizeinbase (prod.p, 2));
      fprintf (stderr, "Alg(%d): using p=%lu for lifting\n", numdep, p);
      fflush (stderr);
    }

    double tm = seconds();
    cxx_mpz_polymod_scaled_sqrt (prod, prod, F, p, numdep);
#pragma omp critical
    {
      fprintf (stderr, "Alg(%d): square root lifted in %.2lfs\n",
	       numdep, seconds() - tm);
      fflush (stderr);
    }

    mpz_t algsqrt, aux;
    mpz_init(algsqrt);
    mpz_init(aux);
    mpz_t m;
    int ret;
    mpz_init(m);
    do {
        ret = cado_poly_getm(m, pol, Np);
        if (!ret) {
            gmp_fprintf(stderr, "When trying to compute m, got the factor %Zd\n", m);
            mpz_divexact(Np, Np, m);
        }
    } while (!ret);
    mpz_poly_eval_mod_mpz(algsqrt, prod.p, m, Np);
    mpz_clear(m);
    mpz_invert(aux, F->coeff[F->deg], Np);  // 1/fd mod n
    mpz_powm_ui(aux, aux, prod.v, Np);      // 1/fd^v mod n
    mpz_mul(algsqrt, algsqrt, aux);
    mpz_mod(algsqrt, algsqrt, Np);

    resfile = fopen_maybe_compressed_lock (sidename, "wb");
    gmp_fprintf (resfile, "%Zd\n", algsqrt);
    fclose_maybe_compressed_lock (resfile, sidename);

#pragma omp critical
    {
      gmp_fprintf (stderr, "Alg(%d): square root is: %Zd\n",
		   numdep, algsqrt);
      fprintf (stderr, "Alg(%d): square root time is %.2lfs\n",
	       numdep, seconds() - t0);
      fflush (stderr);
    }
    free (sidename);
    mpz_clear(aux);
    mpz_clear(algsqrt);
    return 0;
}

/*
 * Try to factor input using trial division up to bound B.
 * Found factors are printed (one per line).
 * Returns 1 if input is completely factored, otherwise, returns
 * remaining factor.
 */
unsigned long
trialdivide_print(unsigned long N, unsigned long B)
{
    ASSERT(N != 0);
    if (N == 1) return 1;
    unsigned long p;
    prime_info pi;
    prime_info_init (pi);
    for (p = 2; p <= B; p = getprime_mt (pi)) {
        while ((N%p) == 0) {
            N /= p;
            printf("%ld\n", p);
            if (N == 1) {
                prime_info_clear (pi);
                return N;
            }
        }
    }
    prime_info_clear (pi);
    return N;
}

void print_nonsmall(mpz_t zx)
{
    if (mpz_probab_prime_p(zx, 10))
        gmp_printf("%Zd\n", zx);
    else {
        int pp = mpz_perfect_power_p(zx);
        if (pp) {
            pp = mpz_sizeinbase(zx, 2);
            mpz_t roo;
            mpz_init(roo);
            while (!mpz_root(roo, zx, pp))
                pp--;
            int i;
            for (i = 0; i < pp; ++i)
                gmp_printf("%Zd\n", roo);
            mpz_clear(roo);
        } else
            gmp_printf("%Zd\n", zx);
    }
    fflush (stdout);
}

void print_factor(mpz_t N)
{
    unsigned long xx = mpz_get_ui(N);
#pragma omp critical
    if (mpz_cmp_ui(N, xx) == 0) {
        xx = trialdivide_print(xx, 1000000);
        if (xx != 1) {
            mpz_t zx;
            mpz_init(zx);
            mpz_set_ui(zx, xx);
            print_nonsmall(zx);
            mpz_clear(zx);
        }
    } else
        print_nonsmall(N);
}


/********** GCD **********/
int
calculateGcd (const char *prefix, int numdep, mpz_t Np)
{
    char *sidename[2];
    FILE *sidefile[2] = {NULL, NULL};
    mpz_t sidesqrt[2];
    int found = 0;

    for (int side = 0; side < 2; ++side) {
        sidename[side] = get_depsidename (prefix, numdep, side);
        sidefile[side] = fopen_maybe_compressed_lock (sidename[side], "rb");
        mpz_init(sidesqrt[side]);
        if (sidefile[side] == NULL) {
            fprintf(stderr, "Error, cannot open file %s for reading\n",
                    sidename[side]);
            exit(EXIT_FAILURE);
        }
        gmp_fscanf (sidefile[side], "%Zd", sidesqrt[side]);
        fclose_maybe_compressed_lock (sidefile[side], sidename[side]);
        free(sidename[side]);
    }

    mpz_t g1, g2;
    mpz_init(g1);
    mpz_init(g2);

    // reduce mod Np
    mpz_mod(sidesqrt[0], sidesqrt[0], Np);
    mpz_mod(sidesqrt[1], sidesqrt[1], Np);

    // First check that the squares agree
    mpz_mul(g1, sidesqrt[0], sidesqrt[0]);
    mpz_mod(g1, g1, Np);

    mpz_mul(g2, sidesqrt[1], sidesqrt[1]);
    mpz_mod(g2, g2, Np);

    if (mpz_cmp(g1, g2)!=0) {
      fprintf(stderr, "Bug: the squares do not agree modulo n!\n");
      ASSERT_ALWAYS(0);
      //      gmp_printf("g1:=%Zd;\ng2:=%Zd;\n", g1, g2);
    }

    mpz_sub(g1, sidesqrt[0], sidesqrt[1]);
    mpz_gcd(g1, g1, Np);
    if (mpz_cmp(g1,Np)) {
      if (mpz_cmp_ui(g1,1)) {
        found = 1;
        print_factor(g1);
      }
    }

    mpz_add(g2, sidesqrt[0], sidesqrt[1]);
    mpz_gcd(g2, g2, Np);
    if (mpz_cmp(g2,Np)) {
      if (mpz_cmp_ui(g2,1)) {
        found = 1;
        print_factor(g2);
      }
    }
    mpz_clear(g1);
    mpz_clear(g2);

    if (!found)
#pragma omp critical
      printf ("Failed\n");

    mpz_clear(sidesqrt[0]);
    mpz_clear(sidesqrt[1]);

    return 0;
}

typedef struct
{
  uint64_t *abs;
  uint64_t *dep_masks;
  unsigned int *dep_counts;
  unsigned int nonzero_deps;
  FILE **dep_files;
} sqrt_data_t;

void *
thread_sqrt (void * context_data, earlyparsed_relation_ptr rel)
{
  sqrt_data_t *data = (sqrt_data_t *) context_data;
  for(unsigned int j = 0 ; j < data->nonzero_deps ; j++)
  {
    if (data->abs[rel->num] & data->dep_masks[j])
    {
      fprintf(data->dep_files[j], "%" PRId64 " %" PRIu64 "\n", rel->a, rel->b);
      data->dep_counts[j]++;
    }
  }
  return NULL;
}

void create_dependencies(const char * prefix, const char * indexname, const char * purgedname, const char * kername)
{
    FILE * ix = fopen_maybe_compressed(indexname, "r");
    uint64_t small_nrows;
    int ret;

    ret = fscanf(ix, "%" SCNu64 "\n", &small_nrows);
    ASSERT(ret == 1);

    FILE * ker;
    size_t ker_stride;
    /* Check that kername has consistent size */
    {
        ker = fopen(kername, "rb");
        if (ker == NULL) { perror(kername); exit(errno); }
        struct stat sbuf[1];
        ret = fstat(fileno(ker), sbuf);
        if (ret < 0) { perror(kername); exit(errno); }
        ASSERT_ALWAYS(sbuf->st_size % small_nrows == 0);
        unsigned int ndepbytes = sbuf->st_size / small_nrows;
        fprintf(stderr, "%s contains %u dependencies (including padding)\n",
                kername, 8 * ndepbytes);
        ker_stride = ndepbytes - sizeof(uint64_t);
        if (ker_stride)
            fprintf(stderr, "Considering only the first 64 dependencies\n");
    }

    /* Read the number of (a,b) pairs */
    uint64_t nrows, ncols;
    purgedfile_read_firstline (purgedname, &nrows, &ncols);

    uint64_t * abs = (uint64_t *) malloc(nrows * sizeof(uint64_t));
    ASSERT_ALWAYS(abs != NULL);
    memset(abs, 0, nrows * sizeof(uint64_t));

    for(uint64_t i = 0 ; i < small_nrows ; i++) {
        uint64_t v;
        ret = fread(&v, sizeof(uint64_t), 1, ker);
        if (ker_stride) fseek(ker, ker_stride, SEEK_CUR);

        /* read the index row */
        int nc;
        ret = fscanf(ix, "%d", &nc); ASSERT_ALWAYS(ret == 1);
        for(int k = 0 ; k < nc ; k++) {
            uint64_t col;
            ret = fscanf(ix, "%" SCNx64 "", &col); ASSERT_ALWAYS(ret == 1);
            ASSERT_ALWAYS(col < nrows);
            abs[col] ^= v;
        }
    }
    fclose_maybe_compressed(ix, indexname);
    fclose(ker);

    unsigned int nonzero_deps = 0;
    uint64_t sanity = 0;
    for(uint64_t i = 0 ; i < nrows ; i++) {
        sanity |= abs[i];
    }
    uint64_t dep_masks[64]={0,};
    char * dep_names[64];
    FILE * dep_files[64];
    unsigned int dep_counts[64]={0,};

    for(int i = 0 ; i < 64 ; i++) {
        uint64_t m = UINT64_C(1) << i;
        if (sanity & m)
            dep_masks[nonzero_deps++] = m;
    }
    fprintf(stderr, "Total: %u non-zero dependencies\n", nonzero_deps);
    for(unsigned int i = 0 ; i < nonzero_deps ; i++) {
        dep_names[i] = get_depname (prefix, "", i);
        dep_files[i] = fopen_maybe_compressed (dep_names[i], "wb");
        ASSERT_ALWAYS(dep_files[i] != NULL);
    }

    sqrt_data_t data = {.abs = abs, .dep_masks = dep_masks,
                        .dep_counts = dep_counts, .nonzero_deps = nonzero_deps,
                        .dep_files = dep_files};
    char *fic[2] = {(char *) purgedname, NULL};
    filter_rels (fic, (filter_rels_callback_t) thread_sqrt, &data,
          EARLYPARSE_NEED_AB_HEXA, NULL, NULL);


    fprintf(stderr, "Written %u dependencies files\n", nonzero_deps);
    for(unsigned int i = 0 ; i < nonzero_deps ; i++) {
        fprintf(stderr, "%s : %u (a,b) pairs\n", dep_names[i], dep_counts[i]);
        fclose_maybe_compressed (dep_files[i], dep_names[i]);
        free (dep_names[i]);
    }
    free (abs);
}

#define TASK_SQRT 0
#define TASK_GCD  2
/* perform one task (rat or alg or gcd) on one dependency */
void*
one_thread (void* args)
{
  tab_t *tab = (tab_t*) args;
  if (tab[0]->task == TASK_SQRT) {
      if (tab[0]->pol->pols[tab[0]->side]->deg == 1) {
          calculateSqrtRat (tab[0]->prefix, tab[0]->numdep, tab[0]->pol,
                  tab[0]->side, tab[0]->Np);
      } else {
          calculateSqrtAlg (tab[0]->prefix, tab[0]->numdep, tab[0]->pol,
                  tab[0]->side, tab[0]->Np);
      }
  } else /* gcd */
    calculateGcd (tab[0]->prefix, tab[0]->numdep, tab[0]->Np);
  return NULL;
}

/* process task (0=sqrt, 2=gcd) in parallel for
   dependencies numdep to numdep + nthreads - 1 */
void
calculateTaskN (int task, const char *prefix, int numdep, int nthreads,
                cado_poly pol, int side, mpz_t Np)
{
  pthread_t *tid;
  tab_t *T;
  int j;

  tid = (pthread_t*) malloc (nthreads * sizeof (pthread_t));
  ASSERT_ALWAYS(tid != NULL);
  T = (tab_t*) malloc (nthreads * sizeof (tab_t));
  ASSERT_ALWAYS(T != NULL);
  for (j = 0; j < nthreads; j++)
    {
      T[j]->prefix = prefix;
      T[j]->task = task;
      T[j]->numdep = numdep + j;
      T[j]->pol = pol;
      T[j]->side = side;
      T[j]->Np = Np;
    }
#ifdef __OpenBSD__
  /* On openbsd, we have obscure failures that seem to be triggered
   * by multithreading. So let's play it simple.
   */
  for (j = 0; j < nthreads; j++)
      (*one_thread)((void*)(T+j));
#else
  for (j = 0; j < nthreads; j++)
      pthread_create (&tid[j], NULL, one_thread, (void *) (T+j));
  while (j > 0)
      pthread_join (tid[--j], NULL);
#endif
  free (tid);
  free (T);
}

void declare_usage(param_list pl)
{
    param_list_decl_usage(pl, "poly", "Polynomial file");
    param_list_decl_usage(pl, "purged", "Purged relations file, as produced by 'purge'");
    param_list_decl_usage(pl, "index", "Index file, as produced by 'merge'");
    param_list_decl_usage(pl, "ker", "Kernel file, as produced by 'characters'");
    param_list_decl_usage(pl, "prefix", "File name prefix used for output files");
    param_list_decl_usage(pl, "ab", "For each dependency, create file with the a,b-values of the relations used in that dependency");
    param_list_decl_usage(pl, "side0", "Compute square root for side 0 and store in file");
    param_list_decl_usage(pl, "side1", "Compute square root for side 1 and store in file");
    param_list_decl_usage(pl, "gcd", "Compute gcd of the two square roots. Requires square roots on both sides");
    param_list_decl_usage(pl, "dep", "The initial dependency for which to compute square roots");
    param_list_decl_usage(pl, "t",   "The number of dependencies to process (default 1)");
    param_list_decl_usage(pl, "v", "More verbose output");
    param_list_decl_usage(pl, "force-posix-threads", "force the use of posix threads, do not rely on platform memory semantics");
}

void usage(param_list pl, const char * argv0, FILE *f)
{
    param_list_print_usage(pl, argv0, f);
    fprintf(f, "Usage: %s [-ab || -side0 || -side1 || -gcd] -poly polyname -prefix prefix -dep numdep -t ndep", argv0);
    fprintf(f, " -purged purgedname -index indexname -ker kername\n");
    fprintf(f, "or %s (-side0 || -side1 || -gcd) -poly polyname -prefix prefix -dep numdep -t ndep\n\n", argv0);
    fprintf(f, "(a,b) pairs of dependency relation 'numdep' will be r/w in file 'prefix.numdep',");
    fprintf(f, " side0 sqrt in 'prefix.side0.numdep' ...\n");
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
    cado_poly pol;
    int numdep = -1, nthreads = 1, ret MAYBE_UNUSED, i;

    char * me = *argv;
    /* print the command line */
    fprintf (stderr, "%s.r%s", argv[0], cado_revision_string);
    for (i = 1; i < argc; i++)
      fprintf (stderr, " %s", argv[i]);
    fprintf (stderr, "\n");

    param_list pl;
    param_list_init(pl);
    declare_usage(pl);

    int opt_ab = 0;    /* create dependency files */
    int opt_side0 = 0; /* compute square root on side 0 */
    int opt_side1 = 0; /* compute square root on side 1 */
    int opt_gcd = 0;   /* compute gcd */
    param_list_configure_switch(pl, "ab", &opt_ab);
    param_list_configure_switch(pl, "side0", &opt_side0);
    param_list_configure_switch(pl, "side1", &opt_side1);
    param_list_configure_switch(pl, "gcd", &opt_gcd);
    param_list_configure_switch(pl, "-v", &verbose);
    param_list_configure_switch(pl, "force-posix-threads", &filter_rels_force_posix_threads);
    argc--,argv++;
    for( ; argc ; ) {
        if (param_list_update_cmdline(pl, &argc, &argv)) continue;
        if (strcmp(*argv, "--help") == 0) {
            usage(pl, me, stderr);
            exit(0);
        } else {
            fprintf(stderr, "unexpected argument: %s\n", *argv);
            usage(pl, me, stderr);
            exit(1);
        }
    }
    const char * tmp;
    if((tmp = param_list_lookup_string(pl, "poly")) == NULL) {
        fprintf(stderr, "Parameter -poly is missing\n");
        usage(pl, me, stderr);
        exit(1);
    }
    cado_poly_init(pol);
    ret = cado_poly_read(pol, tmp);
    if (ret == 0) {
        fprintf(stderr, "Could not read polynomial file\n");
        exit(1);
    }

    param_list_parse_int (pl, "dep", &numdep);
    param_list_parse_int (pl, "t", &nthreads);
    const char * purgedname = param_list_lookup_string(pl, "purged");
    const char * indexname = param_list_lookup_string(pl, "index");
    const char * kername = param_list_lookup_string(pl, "ker");
    const char * prefix = param_list_lookup_string(pl, "prefix");
    if (prefix == NULL) {
        fprintf(stderr, "Parameter -prefix is missing\n");
        exit(1);
    }
    if (param_list_warn_unused(pl))
        exit(1);

    /* if no options then -ab -side0 -side1 -gcd */
    if (!(opt_ab || opt_side0 || opt_side1 || opt_gcd))
        opt_ab = opt_side0 = opt_side1 = opt_gcd = 1;

#ifdef HAVE_OPENMP
    /* when computing the product tree on the algebraic side, we want
       nested parallelism: a first level of parallelism makes N/2 product
       when the tree has width N, and a second level computes the polynomial
       products in parallel */
    omp_set_nested (1);
#endif

    double cpu0 = seconds ();
    double wct0 = wct_seconds();

    /*
     * In the case where the number N to factor has a prime factor that
     * divides the leading coefficient of f or g, the reduction modulo N
     * will fail. Let's compute N', the factor of N that is coprime to
     * those leading coefficients.
     */
    mpz_t Np;
    mpz_init(Np);
    {
        mpz_t gg;
        mpz_init(gg);
        mpz_set(Np, pol->n);
        for (int side = 0; side < 2; ++side) {
            do {
                mpz_gcd(gg, Np, pol->pols[side]->coeff[pol->pols[side]->deg]);
                if (mpz_cmp_ui(gg, 1) != 0) {
                    gmp_fprintf(stderr, "Warning: found the following factor of N as a factor of g: %Zd\n", gg);
                    print_factor(gg);
                    mpz_divexact(Np, Np, gg);
                }
            } while (mpz_cmp_ui(gg, 1) != 0);
        }
        mpz_clear(gg);
        /* Trial divide Np, to avoid bug if a stupid input is given */
        {
            unsigned long p;
            prime_info pi;
            prime_info_init (pi);
            for (p = 2; p <= 1000000; p = getprime_mt (pi)) {
                while (mpz_tdiv_ui(Np, p) == 0) {
                    printf("%lu\n", p);
                    mpz_divexact_ui(Np, Np, p);
                }
            }
            prime_info_clear (pi);
        }
        if (mpz_cmp(pol->n, Np) != 0)
            gmp_fprintf(stderr, "Now factoring N' = %Zd\n", Np);
        if (mpz_cmp_ui(Np, 1) == 0) {
            gmp_fprintf(stderr, "Hey N' is 1! Stopping\n");
            cado_poly_clear (pol);
            param_list_clear(pl);
            mpz_clear(Np);
            return 0;
        }
        if (mpz_probab_prime_p(Np, 10) || mpz_perfect_power_p(Np)) {
            gmp_fprintf(stderr, "Hey N' is (power of) prime! Stopping\n");
            print_factor(Np);
            cado_poly_clear (pol);
            param_list_clear(pl);
            mpz_clear(Np);
            return 0;
        }
    }

    if (opt_ab) {
        /* Computing (a,b) pairs is now done in batch for 64 dependencies
         * together -- should be enough for our purposes, even if we do
         * have more dependencies !
         */
        if (indexname == NULL) {
            fprintf(stderr, "Parameter -index is missing\n");
            exit(1);
        }
        if (purgedname == NULL) {
            fprintf(stderr, "Parameter -purged is missing\n");
            exit(1);
        }
        if (kername == NULL) {
            fprintf(stderr, "Parameter -ker is missing\n");
            exit(1);
        }
        create_dependencies(prefix, indexname, purgedname, kername);
    }

#ifdef __OpenBSD__
    if (nthreads > 1) {
        fprintf(stderr, "Warning: reducing number of threads to 1 for openbsd ; unexplained failure https://ci.inria.fr/cado/job/compile-openbsd-59-amd64-random-integer/2775/console\n");
        /* We'll still process everything we've been told to. But in a
         * single-threaded fashion */
    }
#endif

    if (opt_side0 || opt_side1 || opt_gcd)
      {
        int i;

        for (i = 0; i < nthreads; i++)
          if (check_dep (prefix, numdep + i) == 0)
            {
              fprintf (stderr, "Warning: dependency %d does not exist, reducing the number of threads to %d\n",
                       numdep + i, i);
              nthreads = i;
              break;
            }
      }

    if (nthreads == 0)
      {
        fprintf (stderr, "Error, no more dependency\n");
        cado_poly_clear (pol);
        param_list_clear (pl);
        mpz_clear (Np);
        return 1;
      }

    if (opt_side0) {
        ASSERT_ALWAYS(numdep != -1);
        calculateTaskN(TASK_SQRT, prefix, numdep, nthreads, pol, 0, Np);
    }

    if (opt_side1) {
        ASSERT_ALWAYS(numdep != -1);
        calculateTaskN(TASK_SQRT, prefix, numdep, nthreads, pol, 1, Np);
    }

    if (opt_gcd) {
        ASSERT_ALWAYS(numdep != -1);
        calculateTaskN(TASK_GCD, prefix, numdep, nthreads, pol, 0, Np);
    }

    cado_poly_clear (pol);
    param_list_clear (pl);
    mpz_clear (Np);
    print_timing_and_memory (stderr, cpu0, wct0);
    return 0;
}
