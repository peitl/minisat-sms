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
  checker(30, config.initialPartition, makeDefaultOrderingVector(vertices), config.cutoff, NULL),
  checker010(config.triangles, config.edges, &triangle_stats, &edge_stats)
{}

//  0 means non-minimal, clause added successfully
//  1 means     minimal
// -1 means non-minimal, clause violated at decision level 0, formula UNSAT
int SMSPropagator::checkAssignment(bool is_full_assignment) {
  adjacency_matrix_t matrix = getAdjMatrix();
  try {
    checker.check(matrix, is_full_assignment);
  } catch (forbidden_graph_t fg) {
    if (!solver->addClauseDuringSearch(blockingClause(fg)))
      return -1; // UNSAT
    return 0; // symmetry clause learned
  }
  if (is_full_assignment && prop010) {
    try {
      checker010.check(matrix, vector<int>(), config.nextFreeVariable);
    } catch (vector<clause_t> clauses) {
      /*if (general_purpose_counter % 10 == 0) {
        if (general_purpose_counter % 100 == 0)
          printf("\n## %d ##", general_purpose_counter);
        printf("\n010 clause hashes: ");
      }
      general_purpose_counter++;*/
      for (clause_t& clause : clauses) {
        /*int hash = 0;
        for (int lit : clause) {
          hash += lit;
        }
        printf("% 7d ", hash);*/
        if (!solver->addClauseDuringSearch(blockingClause(clause))) {
          return -1;
        }
        return 0;
      }
    }
  }
  if (config.assignmentCutoffPrerunTime && solver->solve_time > config.assignmentCutoffPrerunTime) {
    int m = config.vertices * (config.vertices - 1) / 2;
    int num_assigned_edge_variables = 0;
    vec<Lit> blocking_clause;
    for (Var v = 0; v < m; v++) {
      if (solver->value(v) != l_Undef) {
        num_assigned_edge_variables++;
        blocking_clause.push(mkLit(v, solver->value(v) == l_True));
      }
    }
    if (num_assigned_edge_variables >= config.assignmentCutoff) {
      //general_purpose_counter++;
      printf("a");
      for (int i = 0; i < blocking_clause.size(); i++) {
        printf(" %d", solver->l2i(~blocking_clause[i]));
      }
      printf("\n");
      //printf("adding cube clause\n");
      if (!solver->addClauseDuringSearch(std::move(blocking_clause)))
        return -1; // UNSAT
      return 0; // cube blocker added
    }
  }
  //printf("SMS check passed\n");
  return 1;
}


vec<Lit> SMSPropagator::blockingClause(const clause_t& clause) {
    vec<Lit> lcls(clause.size());
    for (int i = 0; i < clause.size(); i++) {
      //lcls.push(mkLit(abs(lit), lit < 0));
      lcls[i] = solver->i2l(clause[i]);
    }
    return lcls;
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

