#ifndef _SMS_PROPAGATOR_
#define _SMS_PROPAGATOR_

#include "minisat/core/SolverTypes.h"
#include "sms.hpp"

namespace Minisat {
 
class Solver;

class SMSPropagator {
  public:
    SMSPropagator(Solver* solver, int vertices, int cutoff);
    int checkAssignment(bool);
    void printStats() { checker.printStats(); };
    Solver* solver;

    SolverConfig config;
    MinimalityChecker checker;

    adjacency_matrix_t getAdjMatrix();
    vec<Lit> blockingClause(const forbidden_graph_t& fg);
};
}

#endif /* ifndef _SMS_PROPAGATOR_ */
