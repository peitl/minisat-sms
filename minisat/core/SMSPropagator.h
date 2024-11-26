#ifndef _SMS_PROPAGATOR_
#define _SMS_PROPAGATOR_

#include "minisat/core/SolverTypes.h"
#include "sms.hpp"
#include "coloringCheck.hpp"

namespace Minisat {
 
class Solver;

class SMSPropagator {
  public:
    SMSPropagator(Solver* solver, int vertices, int cutoff, int frequency, long assignmentCutoffPrerunTime, int assignmentCutoff);
    int checkAssignment(bool);
    void printStats() { checker.printStats(); checker010.printStats(); };
    Solver* solver;

    SolverConfig config;
    MinimalityChecker checker;

    adjacency_matrix_t getAdjMatrix();
    vec<Lit> blockingClause(const forbidden_graph_t& fg);


    //data structures and setup for 010-colorability
    Non010colorableChecker checker010;
    bool prop010 = false;
    vector<vector<int>> edge_stats;
    vector<vector<vector<int>>> triangle_stats;

    int general_purpose_counter = 0;

    inline void initEdgeMemory() { int n = config.vertices;
      edge_stats.resize(n);
      for (int v = 0; v < n; v++)
        edge_stats[v].resize(n, 0);
    }

    inline void initTriangleMemory() { int n = config.vertices;
      triangle_stats.resize(n);
      for (int v = 0; v < n; v++) {
        triangle_stats[v].resize(n);
        for (int w = 0; w < n; w++)
          triangle_stats[v][w].resize(n, 0);
      }
    }

    inline void prepare010(int triangleVarsBegin) {
      config.init_triangle_vars(triangleVarsBegin);
      initEdgeMemory();
      initTriangleMemory();
      prop010 = true;
    }

    vec<Lit> blockingClause(const clause_t& clause);
};
}

#endif /* ifndef _SMS_PROPAGATOR_ */
