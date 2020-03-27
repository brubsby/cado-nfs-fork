#ifndef TEST_BBLAS_LEVEL4_HPP_
#define TEST_BBLAS_LEVEL4_HPP_

#include "test_bblas_base.hpp"

struct test_bblas_level4 : public test_bblas_base
{
    test_bblas_level4(unsigned int n) : test_bblas_base(n) {}

    /* in test_bblas_level4_pluq.cpp */
    static tags_t pluq_tags;
    void pluq();

    /* in test_bblas_level4_gauss.cpp */
    static tags_t gauss_tags;
    void gauss();

#ifdef  HAVE_M4RI
    void m4ri_plu_tests(int n);
#endif

    void operator()(std::vector<std::string> const & tests, std::set<std::string> & seen)
    {
        printf("-- level-4 tests (reductions / factorizations of matrices) --\n");
        for(auto const & s : tests) {
            bool match = false;
            if (matches(s, pluq_tags, match)) pluq();
            if (matches(s, gauss_tags, match)) gauss();
            if (match) seen.insert(s);
        }
    }
};

#endif	/* TEST_BBLAS_LEVEL4_HPP_ */
