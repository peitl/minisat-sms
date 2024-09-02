#include "SMSPropagator.h"
#include "Solver.h"

namespace Minisat {

std::vector<std::vector<int>> makeDefaultOrderingVector(int vertices) {
  std::vector<std::vector<int>> initialVertexOrderings = {std::vector<int>(vertices)};
  for (int i = 0; i < vertices; i++) {
    initialVertexOrderings.back()[i] = i;
  }
  return initialVertexOrderings;
}

SMSPropagator::SMSPropagator(Solver* solver, int vertices, int cutoff) :
  solver(solver),
  config(vertices, cutoff),
  checker(30, config.initialPartition, makeDefaultOrderingVector(vertices), config.cutoff, NULL)
{}

//  0 means non-minimal, clause added successfully
//  1 means     minimal
// -1 means non-minimal, clause violated at decision level 0, formula UNSAT
int SMSPropagator::checkAssignment(bool is_full_assignment) {
  try {
    checker.check(getAdjMatrix(), is_full_assignment);
  } catch (forbidden_graph_t fg) {
    if (!solver->addClauseDuringSearch(blockingClause(fg)))
      return -1;
    return 0;
  }
  //printf("SMS check passed\n");
  return 1;
}


vec<Lit> SMSPropagator::blockingClause(const forbidden_graph_t& fg) {
  vec<Lit> clause;
  //printf("symmetry clause: ");
  for (auto signedEdge : fg) {
    auto edge = signedEdge.second;
    Lit el = mkLit(config.edges[edge.first][edge.second]-1, signedEdge.first == truth_value_true);
    //printf("%d ", solver->l2i(el));
    clause.push(el);
  }
  //printf("\n");
  return std::move(clause);
}

adjacency_matrix_t SMSPropagator::getAdjMatrix() {
  adjacency_matrix_t matrix(config.vertices, vector<truth_value_t>(config.vertices, truth_value_unknown));
  //bool isFullyDefined = true;
  for (int i = 0; i < config.vertices; i++) {
    /*for (int j = 0; j <= i; j++) {
      printf(" ");
    }*/
    for (int j = i + 1; j < config.vertices; j++) {
      Lit eij = solver->i2l(config.edges[i][j]);
      if (solver->value(eij) == l_True) {
        matrix[i][j] = matrix[j][i] = truth_value_true;
        //printf("1");
      } else if (solver->value(eij) == l_False) {
        matrix[i][j] = matrix[j][i] = truth_value_false;
        //printf("0");
      } else {
        matrix[i][j] = matrix[j][i] = truth_value_unknown;
        //printf("*");
      }
    }
    //printf("\n");
  }
  return matrix;
}


}

