#ifndef PLINGEN_TUNING_HPP_
#define PLINGEN_TUNING_HPP_

#include "params.h"
#include "plingen.hpp"
#include "select_mpi.h"
#include "timing.h"     /* for weighted_double */
#include "lingen-substep-schedule.h"
#include "tree_stats.hpp"
#include <map>

/* This object is passed as a companion info to a call
 * of bw_biglingen_recursive ; it is computed by the code in
 * plingen-tuning.cpp
 * but once tuning is over, it is essentially fixed.
 */

struct lingen_call_companion {
    bool recurse;
    bool go_mpi;
    double ttb;
    /* total_ncalls is a priori a power of two, but not always.
     * It is the number of calls that correspond to identical
     * lingen_call_companion::key keys.  In particular, since comparison
     * of ::key types is coarse, this means that total_ncalls is the
     * addition of the number of calls for two possibly different input
     * lengths.
     */
    size_t total_ncalls;
    struct mul_or_mp_times {
        /* XXX This must be trivially copyable because we share it via
         * MPI... ! */
        lingen_substep_schedule S;
        weighted_double
            tt,         /* 1, time per call to the mul operation */
            /* For the following, we have both the number of times the
             * operation is done within 1 call of the mul (or mp)
             * operation, plus the time of _each individual call_.
             */
            t_dft_A,    /* time per dft of the first operand, and so on */
            t_dft_A_comm,
            t_dft_B,
            t_dft_B_comm,
            t_conv,
            t_ift_C;
        size_t reserved_ram;
        size_t ram;
    };
    mul_or_mp_times mp, mul;
    struct key {
        int depth;
        size_t L;
        bool operator<(key const& a) const {
            if (depth < a.depth) return true;
            if (depth > a.depth) return false;
            return lingen_round_operand_size(L) < lingen_round_operand_size(a.L);
        }
    };
};

struct lingen_hints_t : public std::map<lingen_call_companion::key, lingen_call_companion> {
    typedef std::map<lingen_call_companion::key, lingen_call_companion> super;
    double tt_scatter_per_unit;
    double tt_gather_per_unit;
    int ipeak;
    size_t peak;
    void share(int root, MPI_Comm comm);
};

void plingen_tuning_decl_usage(cxx_param_list & pl);
void plingen_tuning_lookup_parameters(cxx_param_list & pl);
lingen_hints_t plingen_tuning(bw_dimensions & d, size_t, MPI_Comm comm, cxx_param_list & pl);

#endif	/* PLINGEN_TUNING_HPP_ */
