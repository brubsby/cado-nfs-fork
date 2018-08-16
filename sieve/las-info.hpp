#ifndef LAS_INFO_HPP_
#define LAS_INFO_HPP_

#include <stdint.h>
#include "las-config.h"
#include "las-base.hpp"
#include "cado_poly.h"
#include "las-todo-entry.hpp"
#include "las-siever-config.hpp"
#ifdef DLP_DESCENT
#include "las-dlog-base.hpp"
#endif
#include "sieve/bucket.hpp"     // bkmult
#include "ecm/batch.h"          // cofac_list
#include "las-forwardtypes.hpp"
#include "las-sieve-shared-data.hpp"
#include "las-todo-list.hpp"
#include "las-cofactor.hpp"     // cofactorization_statistics
#include <list>
#include <vector>
#include <stack>
#include <mutex>
#include "cxx_mpz.hpp"

#include <memory>
#ifdef HAVE_BOOST_SHARED_PTR
#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>
namespace std { using boost::shared_ptr; using boost::make_shared; }
#endif

/* This one wants to have siever_config defined */
#include "las-descent-trees.hpp"

// #define HILIGHT_START   "\e[01;31m"
// #define HILIGHT_END   "\e[00;30m"
#define HILIGHT_START   ""
#define HILIGHT_END   ""


/* {{{ las_info
 *
 * las_info holds general data, mostly unrelated to what is actually
 * computed within a sieve. las_info also contains outer data, which
 * lives outside the choice of one particular way to configure the siever
 * versus another.
 */
struct las_info : private NonCopyable {
    // ----- general operational flags
    int nb_threads;
    const char * galois; /* a string to indicate which galois to use in las */
    int suppress_duplicates;
    int adjust_strategy = 0;

    /* It's not ``general operational'', but global enough to be here */
    cxx_cado_poly cpoly;
    gmp_randstate_t rstate;

    // ----- default config and adaptive configs
    siever_config_pool config_pool;

    private:
    /* It's slightly unfortunate to have "mutable" here, obviously. The
     * root cause is the fetching of strategies for cofactoring in
     * duplicate suppression mode. As the call to relation_is_duplicate
     * happens really deep in the call chain, we have a const ref to las
     * at this point, and this is generally a good thing ! So the
     * get_strategies() method must be const.
     *
     * Other cache access calls, on the other hand, are all relatively
     * shallow and can access a non-const ref to las very close by. So
     * there is no compelling need to have the mutable keyword here, as
     * we can afford to call them with the non-const ref (see
     * nfs_work::prepare_for_new_q and nfs_work_cofac::nfs_work_cofac).
     */
    mutable sieve_shared_data shared_structure_cache;

    public:
    /* These accessors are for everyone to use. */
    fb_factorbase::slicing const * get_factorbase_slicing(int side, fb_factorbase::key_type fbK) {
        return shared_structure_cache.sides[side].get_factorbase_slicing(fbK);
    }
    trialdiv_data const * get_trialdiv_data(int side, fb_factorbase::key_type fbK, fb_factorbase::slicing const * fbs) {
        return shared_structure_cache.sides[side].get_trialdiv_data(fbK, fbs);
    }
    unsieve_data const * get_unsieve_data(siever_config const & conf) {
        return shared_structure_cache.get_unsieve_data(conf);
    }
    j_divisibility_helper const * get_j_divisibility_helper(int J) {
        return shared_structure_cache.get_j_divisibility_helper(J);
    }
    facul_strategies_t const * get_strategies(siever_config const & conf) const {
        return shared_structure_cache.get_strategies(conf);
    }
    bool no_fb(int side) const {
        return shared_structure_cache.sides[side].no_fb();
    }

    private:
    bkmult_specifier bk_multiplier { 1.0 };
    std::mutex mm;

    public:
    void grow_bk_multiplier(bkmult_specifier::key_type const& key, double d) {
        std::lock_guard<std::mutex> foo(mm);
        bk_multiplier.grow(key, d);
    }
    bkmult_specifier get_bk_multiplier() {
        std::lock_guard<std::mutex> foo(mm);
        return bk_multiplier;
    }

    /* For composite special-q: note present both in las_info and
     * las_todo_list */
    bool allow_composite_q = false;
    uint64_t qfac_min = 1024;
    uint64_t qfac_max = UINT64_MAX;
    inline bool is_in_qfac_range(uint64_t p) const {
        return (p >= qfac_min) && (p >= qfac_max);
    }

    std::array<unsigned long, 2> dupqmin;   /* smallest q sieved, for dupsup */
    std::array<unsigned long, 2> dupqmax;   /* largest q sieved, for dupsup */
 
    // ----- stuff roughly related to the descent
    unsigned int max_hint_bitsize[2];
    int * hint_lookups[2]; /* quick access indices into hint_table */
    /* This is an opaque pointer to C++ code. */
    void * descent_helper;
#ifdef  DLP_DESCENT
    las_dlog_base dlog_base;
#endif
    mutable descent_tree tree;
    void init_hint_table(param_list_ptr);
    void clear_hint_table();

    // ----- batch mode
    int batch; /* batch mode for cofactorization */
    FILE * batch_print_survivors;
    const char *batch_file[2];
    int batchlpb[2];
    int batchmfb[2];

    /* Would this rather go somewhere else ? In a global (not per-sq)
     * version of nfs_work_cofac perhaps ?
     * 
     * We're really over-using mutable modifiers with this struct. It's
     * annoying me, and more than a hint at the fact that we should think
     * the design a bit differently.
     */
    mutable cofac_list L; /* store (a,b) and corresponding cofactors in batch mode */

    /* ----- cofactorization statistics for the default config */
    mutable cofactorization_statistics cofac_stats;

    const char *dump_filename;

    las_info(cxx_param_list &);
    ~las_info();

    static void declare_usage(cxx_param_list & pl);
};
/* }}} */

enum {
  OUTPUT_CHANNEL,
  ERROR_CHANNEL,
  STATS_CHANNEL,
  TRACE_CHANNEL,
  NR_CHANNELS /* This must be the last element of the enum */
};


#endif	/* LAS_INFO_HPP_ */
