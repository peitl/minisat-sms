"""Wrapper class for the C++ based SMS solver
"""

import ctypes as ct
import sys
import itertools
#import pysms

smslib = ct.CDLL("libminisat.so")

STATUS = {
        -2 : "INCONSISTENT_ASSUMPTIONS",
        -1 : "CONFLICT",
         0 : "OPEN",
         1 : "SAT",
        }

SOLUTION_RESULT = {
        0 : "TIME",
        1 : "DONE",
        2 : "LIMIT",
        }

class PropLits(ct.Structure):
    _fields_ = [('result', ct.c_int),
                ('num_prop_lits', ct.c_int)]

class AssignmentSwitchResult(ct.Structure):
    _fields_ = [('result', ct.c_int),
                ('num_decisions_executed', ct.c_int),
                ('num_prop_lits', ct.c_int)]

class Lit(ct.Structure):
    _fields_ = [('x', ct.c_int)]

class EnumerationResult(ct.Structure):
    _fields_ = [('num_sol', ct.c_int),
                ('status', ct.c_int)]

# load functions into aliases
sms_create_solver = smslib.create_solver
sms_add = smslib.add
sms_destroy_solver = smslib.destroy_solver
sms_assign_literal = smslib.assign_literal
sms_backtrack = smslib.backtrack
sms_next_prop_lit = smslib.next_prop_lit
sms_request_propagation_scope = smslib.request_propagation_scope
sms_fast_switch_assignment = smslib.fast_switch_assignment
sms_learn_clause = smslib.learn_clause
sms_run_solver = smslib.run_solver
sms_run_solver_enumerate = smslib.run_solver_enumerate
sms_model_value = smslib.model_value
sms_block_model = smslib.block_model
sms_n_vars = smslib.n_vars
sms_trail_location = smslib.trail_location
sms_attach_010_propagator = smslib.attach_010_propagator

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
sms_fast_switch_assignment.argtypes = [ct.c_void_p, ct.c_int, ct.Array]
sms_fast_switch_assignment.restype = AssignmentSwitchResult
sms_backtrack.argtypes = [ct.c_void_p, ct.c_int]
sms_backtrack.restype = ct.c_int
sms_request_propagation_scope.argtypes = [ct.c_void_p, ct.c_int]
sms_request_propagation_scope.restype = ct.c_int
sms_next_prop_lit.argtypes = [ct.c_void_p]
sms_next_prop_lit.restype = ct.c_int
sms_learn_clause.argtypes = [ct.c_void_p]
sms_learn_clause.restype = PropLits
sms_run_solver.argtypes = [ct.c_void_p, ct.c_double]
sms_run_solver.restype = ct.c_int
sms_run_solver_enumerate.argtypes = [ct.c_void_p, ct.c_double, ct.c_bool, ct.c_int]
sms_run_solver_enumerate.restype = EnumerationResult
sms_model_value.argtypes = [ct.c_void_p, ct.c_int]
sms_model_value.restype = ct.c_int
sms_block_model.argtypes = [ct.c_void_p]
sms_block_model.restype = None
sms_n_vars.argtypes = [ct.c_void_p]
sms_n_vars.restype = ct.c_int
sms_trail_location.argtypes = [ct.c_void_p, ct.c_int]
sms_trail_location.restype = ct.POINTER(Lit)
sms_attach_010_propagator.argtypes = [ct.c_void_p, ct.c_int]
sms_attach_010_propagator.restype = None

def l2i(x : int):
    s = 1 - (x % 2) * 2;
    v = x // 2 + 1;
    return v * s


class Solver:
    def __init__(self, vertices=2, cutoff=20000, frequency=30, assignmentCutoffPrerunTime=0, assignmentCutoff=0, non010=False, triangleVars=None, clauses=[]):
        self.sms_solver = sms_create_solver(vertices, cutoff, frequency, assignmentCutoffPrerunTime, assignmentCutoff) # <2 vertices is an error
        if non010:
            if triangleVars == None:
                triangleVars = vertices*(vertices-1)//2+1
            sms_attach_010_propagator(self.sms_solver, triangleVars)
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

    def switchAssignment(self, new_ass : list[int]):
        lits = (ct.c_int * len(new_ass))(*new_ass)
        asr = sms_fast_switch_assignment(self.sms_solver, len(new_ass), lits)
        return asr.result, asr.num_decisions_executed, asr.num_prop_lits

    # get all propagations starting from decision level sinceLevel (0 means all)
    def getPropagatedLiterals(self, sinceLevel=0):
        if sms_request_propagation_scope(self.sms_solver, sinceLevel) == 0:
            raise IndexError(sinceLevel)

        while True:
            lit = sms_next_prop_lit(self.sms_solver)
            if lit != 0:
                yield lit
            else:
                break

    # collect num propagated literals from the solver (caller needs to know how many)
    def getPropagatedLiteralsFast(self, num : int, sinceLevel=0):
        tloc : ct.POINTER(Lit) = sms_trail_location(self.sms_solver, sinceLevel)
        if not tloc:
            raise IndexError(sinceLevel)

        for i in range(num):
            x = tloc[i].x # dereference tloc, then take .x member
            v = x // 2 + 1
            yield v if x % 2 == 0 else -v
            #yield l2i(x)


    def backtrack(self, levels:int = 1):
        return sms_backtrack(self.sms_solver, levels)

    def learnClause(self):
        pl = sms_learn_clause(self.sms_solver)
        return pl.result, pl.num_prop_lits

    def solve(self, time : float = -1): #time in seconds, -1 is without limit
        """
        runs the solver for time seconds, can be called repeatedly
        """
        return sms_run_solver(self.sms_solver, time)

    def enumerate(self, time : float = -1, max_sol = None):
        """
        runs the solver for time seconds,
        enumerates all solutions
        returns solution count or -1 if timeout
        """
        if max_sol == None:
            max_sol = 2**31-1
        result = sms_run_solver_enumerate(self.sms_solver, time, True, max_sol)
        return result.num_sol, result.status


    def getModel(self):
        model = []
        for v in range(1, sms_n_vars(self.sms_solver)+1):
            if sms_model_value(self.sms_solver, v) == 0:
                model.append(-v)               
            else:
                model.append(v)               
        return model

    def blockModel(self):
        sms_block_model(self.sms_solver)



def genPHP(n : int):
    PHP = []
    phvar = lambda p, h: 1 + p*n + h

    for p in range(n+1):
        PHP.append([phvar(p, h) for h in range(n)])

    for h in range(n):
        for p1 in range(n+1):
            for p2 in range(p1+1, n+1):
                PHP.append([-phvar(p1, h), -phvar(p2, h)])

    return PHP

def edge(u, v, n):
    return u*(2*n-1-u)//2 + v-u

#triangle-free
def genTF(n : int):
    tfn = []
    for u, v, w in itertools.combinations(range(n), 3):
        tfn.append([-edge(u,v,n), -edge(u,w,n), -edge(v,w,n)])
    return tfn


if __name__ == "__main__":

    test_formulas = []
    test_formulas.append([
        [-1, 2],
        [-2, 3],
        [-3, 4],
        [ 3, 4]
        ])
    test_formulas.append(genPHP(3))
    test_formulas.append(genPHP(11))
    for n in range(3,11):
        test_formulas.append(genTF(n))
    #n = int(sys.argv[1])
    t = 0
    for F in test_formulas[:1]:
        solver = Solver()
        for C in F:
            solver.addClause(C)

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

        print(solver.switchAssignment([2, -3]))
        for lit in solver.getPropagatedLiterals():
            print(lit)
        del solver

    for F in test_formulas[1:3]:
        solver = Solver()
        for C in F:
            solver.addClause(C)
        print(f"Solve result: {solver.solve(0.1)}")
        del solver

    for n in range(3,11):
        #print(test_formulas[n])
        solver = Solver(n, 100000, test_formulas[n])
        m = 0
        while solver.solve() == 10:
            m += 1
            #print(solver.getModel())
            solver.blockModel()
        print (f"#triangle-free graphs on {n} vertices = {m}")
        del solver


