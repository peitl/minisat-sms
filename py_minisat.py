"""Wrapper class for the C++ based SMS solver
"""

import ctypes as ct
import sys
#import pysms

smslib = ct.CDLL("libminisat.so")

class PropLits(ct.Structure):
    _fields_ = [('result', ct.c_int),
                ('num_prop_lits', ct.c_int)]

# load functions into aliases
sms_create_solver = smslib.create_solver
sms_add = smslib.add
sms_destroy_solver = smslib.destroy_solver
sms_assign_literal = smslib.assign_literal
sms_backtrack = smslib.backtrack
sms_get_propagated_literal = smslib.get_propagated_literal
sms_learn_clause = smslib.learn_clause

# specify function signatures
sms_create_solver.argtypes = []
sms_create_solver.restype = ct.c_void_p
#sms_next_solution.argtypes = [ct.c_void_p]
#sms_next_solution.restype = ct.POINTER(ct.c_int)
sms_add.argtypes = [ct.c_void_p, ct.c_int]
sms_add.restype = None
sms_destroy_solver.argtypes = [ct.c_void_p]
sms_destroy_solver.restype = None
sms_assign_literal.argtypes = [ct.c_void_p]
sms_assign_literal.restype = PropLits
sms_backtrack.argtypes = [ct.c_void_p, ct.c_int]
sms_backtrack.restype = ct.c_int
sms_get_propagated_literal.argtypes = [ct.c_void_p]
sms_get_propagated_literal.restype = ct.c_int
sms_learn_clause.argtypes = [ct.c_void_p]
sms_learn_clause.restype = PropLits


class Solver:
    def __init__(self, vertices=2, clauses=[]):
        self.sms_solver = sms_create_solver() # <2 vertices is an error
        for clause in clauses:
            self.addClause(clause)

    def __del__(self):
        sms_destroy_solver(self.sms_solver)

    #@classmethod
    #def fromBuilder(cls, builder : pysms.graph_builder.GraphEncodingBuilder):
    #    return cls(builder.n, builder)

    def addClause(self, clause):
        for lit in clause:
            sms_add(self.sms_solver, lit)
        sms_add(self.sms_solver, 0)

    def assignLiteral(self, lit : int):
        pl = sms_assign_literal(self.sms_solver, lit)
        return pl.result, pl.num_prop_lits

    def getPropagatedLiterals(self):
        prop_lits = []
        while True:
            lit = sms_get_propagated_literal(self.sms_solver)
            if lit != 0:
                yield lit
            else:
                break

    def backtrack(self, levels:int = 1):
        sms_backtrack(self.sms_solver, levels)

    def learnClause(self):
        pl = sms_learn_clause(self.sms_solver)
        return pl.result, pl.num_prop_lits


if __name__ == "__main__":
    #n = int(sys.argv[1])
    t = 0
    solver = Solver()
    solver.addClause([-1, 2])
    solver.addClause([-2, 3])
    solver.addClause([-3, 4])
    solver.addClause([ 3, 4])
    print(solver.assignLiteral(1))
    for lit in solver.getPropagatedLiterals():
        print(lit)
    solver.backtrack(1)
    print("---")
    print(solver.assignLiteral(-1))
    for lit in solver.getPropagatedLiterals():
        print(lit)
    solver.backtrack(1)
    print("---")
    print(solver.assignLiteral(-4))
    for lit in solver.getPropagatedLiterals():
        print(lit)
    print(solver.learnClause());
    if not solver.backtrack(1):
        print("Backtracking 1 level failed (most likely because current decision level is 0)")
    print("---")
    del solver
