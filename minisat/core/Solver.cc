/***************************************************************************************[Solver.cc]
Copyright (c) 2003-2006, Niklas Een, Niklas Sorensson
Copyright (c) 2007-2010, Niklas Sorensson

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
**************************************************************************************************/

#include <math.h>
#include <time.h>

#include "minisat/mtl/Alg.h"
#include "minisat/mtl/Sort.h"
#include "minisat/utils/Options.h"
#include "minisat/utils/System.h"
#include "minisat/core/Solver.h"

using namespace Minisat;

//=================================================================================================
// Options:


static const char* _cat = "CORE";

static DoubleOption  opt_var_decay         (_cat, "var-decay",   "The variable activity decay factor",            0.95,     DoubleRange(0, false, 1, false));
static DoubleOption  opt_clause_decay      (_cat, "cla-decay",   "The clause activity decay factor",              0.999,    DoubleRange(0, false, 1, false));
static DoubleOption  opt_random_var_freq   (_cat, "rnd-freq",    "The frequency with which the decision heuristic tries to choose a random variable", 0, DoubleRange(0, true, 1, true));
static DoubleOption  opt_random_seed       (_cat, "rnd-seed",    "Used by the random variable selection",         91648253, DoubleRange(0, false, HUGE_VAL, false));
static IntOption     opt_ccmin_mode        (_cat, "ccmin-mode",  "Controls conflict clause minimization (0=none, 1=basic, 2=deep)", 2, IntRange(0, 2));
static IntOption     opt_phase_saving      (_cat, "phase-saving", "Controls the level of phase saving (0=none, 1=limited, 2=full)", 2, IntRange(0, 2));
static BoolOption    opt_rnd_init_act      (_cat, "rnd-init",    "Randomize the initial activity", false);
static BoolOption    opt_luby_restart      (_cat, "luby",        "Use the Luby restart sequence", true);
static IntOption     opt_restart_first     (_cat, "rfirst",      "The base restart interval", 100, IntRange(1, INT32_MAX));
static DoubleOption  opt_restart_inc       (_cat, "rinc",        "Restart interval increase factor", 2, DoubleRange(1, false, HUGE_VAL, false));
static DoubleOption  opt_garbage_frac      (_cat, "gc-frac",     "The fraction of wasted memory allowed before a garbage collection is triggered",  0.20, DoubleRange(0, false, HUGE_VAL, false));
static IntOption     opt_min_learnts_lim   (_cat, "min-learnts", "Minimum learnt clause limit",  0, IntRange(0, INT32_MAX));


//=================================================================================================
// Constructor/Destructor:

Solver::Solver(int vertices, int cutoff, int frequency, long assignmentCutoffPrerunTime, int assignmentCutoff) :
    // SMS
    sms              (this, vertices, cutoff, frequency, assignmentCutoffPrerunTime, assignmentCutoff)

    // Parameters (user settable):
    //
  , verbosity        (0)
  , var_decay        (opt_var_decay)
  , clause_decay     (opt_clause_decay)
  , random_var_freq  (opt_random_var_freq)
  , random_seed      (opt_random_seed)
  , luby_restart     (opt_luby_restart)
  , ccmin_mode       (opt_ccmin_mode)
  , phase_saving     (opt_phase_saving)
  , rnd_pol          (false)
  , rnd_init_act     (opt_rnd_init_act)
  , garbage_frac     (opt_garbage_frac)
  , min_learnts_lim  (opt_min_learnts_lim)
  , restart_first    (opt_restart_first)
  , restart_inc      (opt_restart_inc)

    // Parameters (the rest):
    //
  , learntsize_factor((double)1/(double)3), learntsize_inc(1.1)

    // Parameters (experimental):
    //
  , learntsize_adjust_start_confl (100)
  , learntsize_adjust_inc         (1.5)

    // Statistics: (formerly in 'SolverStats')
    //
  , solves(0), starts(0), decisions(0), rnd_decisions(0), propagations(0), conflicts(0)
  , dec_vars(0), num_clauses(0), num_learnts(0), clauses_literals(0), learnts_literals(0), max_literals(0), tot_literals(0)

  , watches            (WatcherDeleted(ca))
  , order_heap         (VarOrderLt(activity))
  , ok                 (true)
  , cla_inc            (1)
  , var_inc            (1)
  , qhead              (0)
  , simpDB_assigns     (-1)
  , simpDB_props       (0)
  , progress_estimate  (0)
  , remove_satisfied   (true)
  , next_var           (0)

    // Resource constraints:
    //
  , conflict_budget    (-1)
  , propagation_budget (-1)
  , time_budget        (-1)
  , asynch_interrupt   (false)
{}


Solver::~Solver()
{
}


//=================================================================================================
// Minor methods:


// Creates a new SAT variable in the solver. If 'decision' is cleared, variable will not be
// used as a decision variable (NOTE! This has effects on the meaning of a SATISFIABLE result).
//
Var Solver::newVar(lbool upol, bool dvar)
{
    Var v;
    if (free_vars.size() > 0){
        v = free_vars.last();
        free_vars.pop();
    }else
        v = next_var++;

    watches  .init(mkLit(v, false));
    watches  .init(mkLit(v, true ));
    assigns  .insert(v, l_Undef);
    vardata  .insert(v, mkVarData(CRef_Undef, 0));
    activity .insert(v, rnd_init_act ? drand(random_seed) * 0.00001 : 0);
    seen     .insert(v, 0);
    polarity .insert(v, true);
    user_pol .insert(v, upol);
    decision .reserve(v);
    trail    .capacity(v+1);
    setDecisionVar(v, dvar);
    return v;
}


// Note: at the moment, only unassigned variable will be released (this is to avoid duplicate
// releases of the same variable).
void Solver::releaseVar(Lit l)
{
    if (value(l) == l_Undef){
        addClause(l);
        released_vars.push(var(l));
    }
}


bool Solver::addClause_(vec<Lit>& ps)
{
    assert(decisionLevel() == 0);
    if (!ok) return false;

    // Check if clause is satisfied and remove false/duplicate literals:
    sort(ps);
    Lit p; int i, j;
    for (i = j = 0, p = lit_Undef; i < ps.size(); i++)
        if (value(ps[i]) == l_True || ps[i] == ~p)
            return true;
        else if (value(ps[i]) != l_False && ps[i] != p)
            ps[j++] = p = ps[i];
    ps.shrink(i - j);

    if (ps.size() == 0)
        return ok = false;
    else if (ps.size() == 1){
        uncheckedEnqueue(ps[0]);
        return ok = (propagate() == CRef_Undef);
    }else{
        CRef cr = ca.alloc(ps, false);
        clauses.push(cr);
        attachClause(cr);
    }

    return true;
}


void Solver::attachClause(CRef cr){
    const Clause& c = ca[cr];
    assert(c.size() > 1);
    watches[~c[0]].push(Watcher(cr, c[1]));
    watches[~c[1]].push(Watcher(cr, c[0]));
    if (c.learnt()) num_learnts++, learnts_literals += c.size();
    else            num_clauses++, clauses_literals += c.size();
}


void Solver::detachClause(CRef cr, bool strict){
    const Clause& c = ca[cr];
    assert(c.size() > 1);
    
    // Strict or lazy detaching:
    if (strict){
        remove(watches[~c[0]], Watcher(cr, c[1]));
        remove(watches[~c[1]], Watcher(cr, c[0]));
    }else{
        watches.smudge(~c[0]);
        watches.smudge(~c[1]);
    }

    if (c.learnt()) num_learnts--, learnts_literals -= c.size();
    else            num_clauses--, clauses_literals -= c.size();
}


void Solver::removeClause(CRef cr) {
    Clause& c = ca[cr];
    detachClause(cr);
    // Don't leave pointers to free'd memory!
    if (locked(c)) vardata[var(c[0])].reason = CRef_Undef;
    c.mark(1); 
    ca.free(cr);
}


bool Solver::satisfied(const Clause& c) const {
    for (int i = 0; i < c.size(); i++)
        if (value(c[i]) == l_True)
            return true;
    return false; }


// Revert to the state at given level (keeping all assignment at 'level' but not beyond).
//
void Solver::cancelUntil(int level) {
    if (decisionLevel() > level){
        cflr = CRef_Undef;
        for (int c = trail.size()-1; c >= trail_lim[level]; c--){
            Var      x  = var(trail[c]);
            assigns [x] = l_Undef;
            if (phase_saving > 1 || (phase_saving == 1 && c > trail_lim.last()))
                polarity[x] = sign(trail[c]);
            insertVarOrder(x); }
        qhead = trail_lim[level];
        trail.shrink(trail.size() - trail_lim[level]);
        trail_lim.shrink(trail_lim.size() - level);
    } }


//=================================================================================================
// Major methods:


Lit Solver::pickBranchLit()
{
    Var next = var_Undef;

    // Random decision:
    if (drand(random_seed) < random_var_freq && !order_heap.empty()){
        next = order_heap[irand(random_seed,order_heap.size())];
        if (value(next) == l_Undef && decision[next])
            rnd_decisions++; }

    // Activity based decision:
    while (next == var_Undef || value(next) != l_Undef || !decision[next])
        if (order_heap.empty()){
            next = var_Undef;
            break;
        }else
            next = order_heap.removeMin();

    // Choose polarity based on different polarity modes (global or per-variable):
    if (next == var_Undef)
        return lit_Undef;
    else if (user_pol[next] != l_Undef)
        return mkLit(next, user_pol[next] == l_True);
    else if (rnd_pol)
        return mkLit(next, drand(random_seed) < 0.5);
    else
        return mkLit(next, polarity[next]);
}


/*_________________________________________________________________________________________________
|
|  analyze : (confl : Clause*) (out_learnt : vec<Lit>&) (out_btlevel : int&)  ->  [void]
|  
|  Description:
|    Analyze conflict and produce a reason clause.
|  
|    Pre-conditions:
|      * 'out_learnt' is assumed to be cleared.
|      * Current decision level must be greater than root level.
|  
|    Post-conditions:
|      * 'out_learnt[0]' is the asserting literal at level 'out_btlevel'.
|      * If out_learnt.size() > 1 then 'out_learnt[1]' has the greatest decision level of the 
|        rest of literals. There may be others from the same level though.
|  
|________________________________________________________________________________________________@*/
void Solver::analyze(CRef confl, vec<Lit>& out_learnt, int& out_btlevel)
{
    int pathC = 0;
    Lit p     = lit_Undef;

    // Generate conflict clause:
    //
    out_learnt.push();      // (leave room for the asserting literal)
    int index   = trail.size() - 1;

    do{
        assert(confl != CRef_Undef); // (otherwise should be UIP)
        Clause& c = ca[confl];

        if (c.learnt())
            claBumpActivity(c);

        for (int j = (p == lit_Undef) ? 0 : 1; j < c.size(); j++){
            Lit q = c[j];

            if (!seen[var(q)] && level(var(q)) > 0){
                varBumpActivity(var(q));
                seen[var(q)] = 1;
                if (level(var(q)) >= decisionLevel())
                    pathC++;
                else
                    out_learnt.push(q);
            }
        }
        
        // Select next clause to look at:
        while (!seen[var(trail[index--])]);
        p     = trail[index+1];
        confl = reason(var(p));
        seen[var(p)] = 0;
        pathC--;

    }while (pathC > 0);
    out_learnt[0] = ~p;

    // Simplify conflict clause:
    //
    int i, j;
    out_learnt.copyTo(analyze_toclear);
    if (ccmin_mode == 2){
        for (i = j = 1; i < out_learnt.size(); i++)
            if (reason(var(out_learnt[i])) == CRef_Undef || !litRedundant(out_learnt[i]))
                out_learnt[j++] = out_learnt[i];
        
    }else if (ccmin_mode == 1){
        for (i = j = 1; i < out_learnt.size(); i++){
            Var x = var(out_learnt[i]);

            if (reason(x) == CRef_Undef)
                out_learnt[j++] = out_learnt[i];
            else{
                Clause& c = ca[reason(var(out_learnt[i]))];
                for (int k = 1; k < c.size(); k++)
                    if (!seen[var(c[k])] && level(var(c[k])) > 0){
                        out_learnt[j++] = out_learnt[i];
                        break; }
            }
        }
    }else
        i = j = out_learnt.size();

    max_literals += out_learnt.size();
    out_learnt.shrink(i - j);
    tot_literals += out_learnt.size();

    // Find correct backtrack level:
    //
    if (out_learnt.size() == 1)
        out_btlevel = 0;
    else{
        int max_i = 1;
        // Find the first literal assigned at the next-highest level:
        for (int i = 2; i < out_learnt.size(); i++)
            if (level(var(out_learnt[i])) > level(var(out_learnt[max_i])))
                max_i = i;
        // Swap-in this literal at index 1:
        Lit p             = out_learnt[max_i];
        out_learnt[max_i] = out_learnt[1];
        out_learnt[1]     = p;
        out_btlevel       = level(var(p));
    }

    for (int j = 0; j < analyze_toclear.size(); j++) seen[var(analyze_toclear[j])] = 0;    // ('seen[]' is now cleared)
}


// Check if 'p' can be removed from a conflict clause.
bool Solver::litRedundant(Lit p)
{
    enum { seen_undef = 0, seen_source = 1, seen_removable = 2, seen_failed = 3 };
    assert(seen[var(p)] == seen_undef || seen[var(p)] == seen_source);
    assert(reason(var(p)) != CRef_Undef);

    Clause*               c     = &ca[reason(var(p))];
    vec<ShrinkStackElem>& stack = analyze_stack;
    stack.clear();

    for (uint32_t i = 1; ; i++){
        if (i < (uint32_t)c->size()){
            // Checking 'p'-parents 'l':
            Lit l = (*c)[i];
            
            // Variable at level 0 or previously removable:
            if (level(var(l)) == 0 || seen[var(l)] == seen_source || seen[var(l)] == seen_removable){
                continue; }
            
            // Check variable can not be removed for some local reason:
            if (reason(var(l)) == CRef_Undef || seen[var(l)] == seen_failed){
                stack.push(ShrinkStackElem(0, p));
                for (int i = 0; i < stack.size(); i++)
                    if (seen[var(stack[i].l)] == seen_undef){
                        seen[var(stack[i].l)] = seen_failed;
                        analyze_toclear.push(stack[i].l);
                    }
                    
                return false;
            }

            // Recursively check 'l':
            stack.push(ShrinkStackElem(i, p));
            i  = 0;
            p  = l;
            c  = &ca[reason(var(p))];
        }else{
            // Finished with current element 'p' and reason 'c':
            if (seen[var(p)] == seen_undef){
                seen[var(p)] = seen_removable;
                analyze_toclear.push(p);
            }

            // Terminate with success if stack is empty:
            if (stack.size() == 0) break;
            
            // Continue with top element on stack:
            i  = stack.last().i;
            p  = stack.last().l;
            c  = &ca[reason(var(p))];

            stack.pop();
        }
    }

    return true;
}


/*_________________________________________________________________________________________________
|
|  analyzeFinal : (p : Lit)  ->  [void]
|  
|  Description:
|    Specialized analysis procedure to express the final conflict in terms of assumptions.
|    Calculates the (possibly empty) set of assumptions that led to the assignment of 'p', and
|    stores the result in 'out_conflict'.
|________________________________________________________________________________________________@*/
void Solver::analyzeFinal(Lit p, LSet& out_conflict)
{
    out_conflict.clear();
    out_conflict.insert(p);

    if (decisionLevel() == 0)
        return;

    seen[var(p)] = 1;

    for (int i = trail.size()-1; i >= trail_lim[0]; i--){
        Var x = var(trail[i]);
        if (seen[x]){
            if (reason(x) == CRef_Undef){
                assert(level(x) > 0);
                out_conflict.insert(~trail[i]);
            }else{
                Clause& c = ca[reason(x)];
                for (int j = 1; j < c.size(); j++)
                    if (level(var(c[j])) > 0)
                        seen[var(c[j])] = 1;
            }
            seen[x] = 0;
        }
    }

    seen[var(p)] = 0;
}


void Solver::uncheckedEnqueue(Lit p, CRef from)
{
    assert(value(p) == l_Undef);
    assigns[var(p)] = lbool(!sign(p));
    vardata[var(p)] = mkVarData(from, decisionLevel());
    trail.push_(p);
}


/*_________________________________________________________________________________________________
|
|  propagate : [void]  ->  [Clause*]
|  
|  Description:
|    Propagates all enqueued facts. If a conflict arises, the conflicting clause is returned,
|    otherwise CRef_Undef.
|  
|    Post-conditions:
|      * the propagation queue is empty, even if there was a conflict.
|________________________________________________________________________________________________@*/
CRef Solver::propagate()
{
    CRef    confl     = CRef_Undef;
    int     num_props = 0;

    while (qhead < trail.size()){
        Lit            p   = trail[qhead++];     // 'p' is enqueued fact to propagate.
        vec<Watcher>&  ws  = watches.lookup(p);
        Watcher        *i, *j, *end;
        num_props++;

        for (i = j = (Watcher*)ws, end = i + ws.size();  i != end;){
            // Try to avoid inspecting the clause:
            Lit blocker = i->blocker;
            if (value(blocker) == l_True){
                *j++ = *i++; continue; }

            // Make sure the false literal is data[1]:
            CRef     cr        = i->cref;
            Clause&  c         = ca[cr];
            Lit      false_lit = ~p;
            if (c[0] == false_lit)
                c[0] = c[1], c[1] = false_lit;
            assert(c[1] == false_lit);
            i++;

            // If 0th watch is true, then clause is already satisfied.
            Lit     first = c[0];
            Watcher w     = Watcher(cr, first);
            if (first != blocker && value(first) == l_True){
                *j++ = w; continue; }

            // Look for new watch:
            for (int k = 2; k < c.size(); k++)
                if (value(c[k]) != l_False){
                    c[1] = c[k]; c[k] = false_lit;
                    watches[~c[1]].push(w);
                    goto NextClause; }

            // Did not find watch -- clause is unit under assignment:
            *j++ = w;
            if (value(first) == l_False){
                confl = cr;
                qhead = trail.size();
                // Copy the remaining watches:
                while (i < end)
                    *j++ = *i++;
            }else
                uncheckedEnqueue(first, cr);

        NextClause:;
        }
        ws.shrink(i - j);
    }
    propagations += num_props;
    simpDB_props -= num_props;

    return confl;
}


/*_________________________________________________________________________________________________
|
|  reduceDB : ()  ->  [void]
|  
|  Description:
|    Remove half of the learnt clauses, minus the clauses locked by the current assignment. Locked
|    clauses are clauses that are reason to some assignment. Binary clauses are never removed.
|________________________________________________________________________________________________@*/
struct reduceDB_lt { 
    ClauseAllocator& ca;
    reduceDB_lt(ClauseAllocator& ca_) : ca(ca_) {}
    bool operator () (CRef x, CRef y) { 
        return ca[x].size() > 2 && (ca[y].size() == 2 || ca[x].activity() < ca[y].activity()); } 
};
void Solver::reduceDB()
{
    int     i, j;
    double  extra_lim = cla_inc / learnts.size();    // Remove any clause below this activity

    sort(learnts, reduceDB_lt(ca));
    // Don't delete binary or locked clauses. From the rest, delete clauses from the first half
    // and clauses with activity smaller than 'extra_lim':
    for (i = j = 0; i < learnts.size(); i++){
        Clause& c = ca[learnts[i]];
        if (c.size() > 2 && !locked(c) && (i < learnts.size() / 2 || c.activity() < extra_lim))
            removeClause(learnts[i]);
        else
            learnts[j++] = learnts[i];
    }
    learnts.shrink(i - j);
    checkGarbage();
}


void Solver::removeSatisfied(vec<CRef>& cs)
{
    int i, j;
    for (i = j = 0; i < cs.size(); i++){
        Clause& c = ca[cs[i]];
        if (satisfied(c))
            removeClause(cs[i]);
        else{
            // Trim clause:
            assert(value(c[0]) == l_Undef && value(c[1]) == l_Undef);
            for (int k = 2; k < c.size(); k++)
                if (value(c[k]) == l_False){
                    c[k--] = c[c.size()-1];
                    c.pop();
                }
            cs[j++] = cs[i];
        }
    }
    cs.shrink(i - j);
}


void Solver::rebuildOrderHeap()
{
    vec<Var> vs;
    for (Var v = 0; v < nVars(); v++)
        if (decision[v] && value(v) == l_Undef)
            vs.push(v);
    order_heap.build(vs);
}


/*_________________________________________________________________________________________________
|
|  simplify : [void]  ->  [bool]
|  
|  Description:
|    Simplify the clause database according to the current top-level assigment. Currently, the only
|    thing done here is the removal of satisfied clauses, but more things can be put here.
|________________________________________________________________________________________________@*/
bool Solver::simplify()
{
    assert(decisionLevel() == 0);

    if (!ok || propagate() != CRef_Undef)
        return ok = false;

    if (nAssigns() == simpDB_assigns || (simpDB_props > 0))
        return true;

    // Remove satisfied clauses:
    removeSatisfied(learnts);
    if (remove_satisfied){       // Can be turned off.
        removeSatisfied(clauses);

        // TODO: what todo in if 'remove_satisfied' is false?

        // Remove all released variables from the trail:
        for (int i = 0; i < released_vars.size(); i++){
            assert(seen[released_vars[i]] == 0);
            seen[released_vars[i]] = 1;
        }

        int i, j;
        for (i = j = 0; i < trail.size(); i++)
            if (seen[var(trail[i])] == 0)
                trail[j++] = trail[i];
        trail.shrink(i - j);
        //printf("trail.size()= %d, qhead = %d\n", trail.size(), qhead);
        qhead = trail.size();

        for (int i = 0; i < released_vars.size(); i++)
            seen[released_vars[i]] = 0;

        // Released variables are now ready to be reused:
        append(released_vars, free_vars);
        released_vars.clear();
    }
    checkGarbage();
    rebuildOrderHeap();

    simpDB_assigns = nAssigns();
    simpDB_props   = clauses_literals + learnts_literals;   // (shouldn't depend on stats really, but it will do for now)

    return true;
}

void Solver::assertClauseSatisfiedOrNotFalsifiedAndStable(const Clause& clause) {
    int num_satisfied = 0;
    int num_falsified = 0;
    int highest_dl = -1;
    int num_highest_dl = 0;
    for (int i = 0; i < clause.size(); i++) {
      if (value(clause[i]) != l_Undef) {
        if (level(var(clause[i])) > highest_dl) {
          highest_dl = level(var(clause[i]));
          num_highest_dl = 1;
        } else if (level(var(clause[i])) == highest_dl) {
          num_highest_dl++;
        }
        if (value(clause[i]) == l_True) {
          num_satisfied++;
        } else if (value(clause[i]) == l_False) {
          num_falsified++;
        }
      }
    }

    // assert that a propagated clause was propagated at earliest possible time
    if (!(num_satisfied != 1 || num_falsified != clause.size() - 1 || level(var(clause[0])) != highest_dl || num_highest_dl > 1 || value(clause[0]) == l_False)) {
      printf("#sat = %d, #fal = %d, sz = %d, lvl0 = %d, lvl1 = %d, c[0] = %d, c[1] = %d, hdl = %d, #hdl = %d\n",
          num_satisfied, num_falsified, clause.size(), level(var(clause[0])), level(var(clause[1])), l2i(clause[0]), l2i(clause[1]), highest_dl, num_highest_dl);
      fflush(stdout);
    }
    assert (num_satisfied != 1 || num_falsified != clause.size() - 1 || level(var(clause[0])) != highest_dl || num_highest_dl > 1 || value(clause[0]) == l_False);
    assert (num_satisfied > 0 || num_falsified + 2 <= clause.size());

    //printf("clause size = %d\n", clause.size());
    //printf("num_falsified = %d\n", num_falsified);
    /*if (num_satisfied == 0 && num_falsified + 2 > clause.size()) {
      printf("call no %d\n", sms.general_purpose_counter);
      for (int i = 0; i < clause.size(); i++) {
        char val = value(clause[i]) == l_Undef ? 'u' :
          value(clause[i]) == l_True ? 't' : 'f';
        int lev = value(clause[i]) == l_Undef ? -1 : level(var(clause[i]));
        printf("%d[%c:%d]  ", l2i(clause[i]), val, lev);
      }
      printf("\n");
      fflush(stdout);
    }
    assert(num_falsified + 2 <= clause.size());*/
}

void Solver::solverInStableState() {
  sms.general_purpose_counter++;
  for (int i = 0; i < clauses.size(); i++) {
    assertClauseSatisfiedOrNotFalsifiedAndStable(ca[clauses[i]]);
  }
  for (int i = 0; i < learnts.size(); i++) {
    assertClauseSatisfiedOrNotFalsifiedAndStable(ca[learnts[i]]);
  }
}

/*_________________________________________________________________________________________________
|
|  search : (nof_conflicts : int) (params : const SearchParams&)  ->  [lbool]
|  
|  Description:
|    Search for a model the specified number of conflicts. 
|    NOTE! Use negative value for 'nof_conflicts' indicate infinity.
|  
|  Output:
|    'l_True' if a partial assigment that is consistent with respect to the clauseset is found. If
|    all variables are decision variables, this means that the clause set is satisfiable. 'l_False'
|    if the clause set is unsatisfiable. 'l_Undef' if the bound on number of conflicts is reached.
|________________________________________________________________________________________________@*/
lbool Solver::search(int nof_conflicts)
{
    assert(ok);
    clock_t search_start = clock(), search_end;
    int         backtrack_level;
    int         conflictC = 0;
    vec<Lit>    learnt_clause;
    starts++;

    for (;;){
        search_end = clock();
        solve_time += double(search_end - search_start) / CLOCKS_PER_SEC;
        search_start = search_end;
        CRef confl = propagate();
        if (confl != CRef_Undef){
            // CONFLICT
            conflicts++; conflictC++;
            if (decisionLevel() == 0) return l_False;

            learnt_clause.clear();
            analyze(confl, learnt_clause, backtrack_level);
            cancelUntil(backtrack_level);

            if (learnt_clause.size() == 1){
                uncheckedEnqueue(learnt_clause[0]);
            }else{
                CRef cr = ca.alloc(learnt_clause, true);
                learnts.push(cr);
                attachClause(cr);
                claBumpActivity(ca[cr]);
                uncheckedEnqueue(learnt_clause[0], cr);
            }

            varDecayActivity();
            claDecayActivity();

            if (--learntsize_adjust_cnt == 0){
                learntsize_adjust_confl *= learntsize_adjust_inc;
                learntsize_adjust_cnt    = (int)learntsize_adjust_confl;
                max_learnts             *= learntsize_inc;

                if (verbosity >= 1)
                    printf("| %9d | %7d %8d %8d | %8d %8d %6.0f | %6.3f %% |\n", 
                           (int)conflicts, 
                           (int)dec_vars - (trail_lim.size() == 0 ? trail.size() : trail_lim[0]), nClauses(), (int)clauses_literals, 
                           (int)max_learnts, nLearnts(), (double)learnts_literals/nLearnts(), progressEstimate()*100);
            }

        }else{

            //solverInStableState();

            // NO CONFLICT
            if ((nof_conflicts >= 0 && conflictC >= nof_conflicts) || !withinBudget()){
                // Reached bound on number of conflicts:
                progress_estimate = progressEstimate();
                cancelUntil(0);
                return l_Undef; }

            // Simplify the set of problem clauses:
            if (decisionLevel() == 0 && !simplify())
                return l_False;

            if (learnts.size()-nAssigns() >= max_learnts)
                // Reduce the set of learnt clauses:
                reduceDB();

            // run SMS mincheck
            int m = sms.config.vertices * (sms.config.vertices-1) / 2;
            bool is_full_graph = true;
            for (Var v = 0; v < m; v++) {
              if (value(v) == l_Undef) {
                is_full_graph = false;
                break;
              }
            }
            int sms_min_status = sms.checkAssignment(is_full_graph);
            if (sms_min_status == 0)
              continue;
            if (sms_min_status == -1)
              return l_False;

            Lit next = lit_Undef;
            while (decisionLevel() < assumptions.size()){
                // Perform user provided assumption:
                Lit p = assumptions[decisionLevel()];
                if (value(p) == l_True){
                    // Dummy decision level:
                    newDecisionLevel();
                }else if (value(p) == l_False){
                    analyzeFinal(~p, conflict);
                    return l_False;
                }else{
                    next = p;
                    break;
                }
            }

            if (next == lit_Undef){
                // New variable decision:
                decisions++;
                next = pickBranchLit();

                if (next == lit_Undef) {
                    // Model found:
                    /*//
                    // run SMS mincheck (should be unnecessary, if we correctly identify full graphs above)
                    sms_min_status = sms.checkAssignment(true);
                    if (sms_min_status == 0)
                      continue;
                    if (sms_min_status == -1)
                      return l_False;*/

                    return l_True;
                }
            }

            /* TODO call mincheck here
             * 1. construct an adjacency matrix from the partial assignment
             * 2. call mincheck
             * 3. if not minimal:
             *      figure out whether unit or conflict
             *      if unit:
             *        add symmetry clause
             *        enqueue unit literal with the symmetry clause as reason
             *      else:
             *        figure out backtrack level and whether asserting
             *        backtrack
             *        add symmetry clause
             *        if asserting:
             *          enqueue unit literal with the symmetry clause as reason
             */

            // Increase decision level and enqueue 'next'
            newDecisionLevel();
            uncheckedEnqueue(next);
        }
    }
}


double Solver::progressEstimate() const
{
    double  progress = 0;
    double  F = 1.0 / nVars();

    for (int i = 0; i <= decisionLevel(); i++){
        int beg = i == 0 ? 0 : trail_lim[i - 1];
        int end = i == decisionLevel() ? trail.size() : trail_lim[i];
        progress += pow(F, i) * (end - beg);
    }

    return progress / nVars();
}

/*
  Finite subsequences of the Luby-sequence:

  0: 1
  1: 1 1 2
  2: 1 1 2 1 1 2 4
  3: 1 1 2 1 1 2 4 1 1 2 1 1 2 4 8
  ...


 */

static double luby(double y, int x){

    // Find the finite subsequence that contains index 'x', and the
    // size of that subsequence:
    int size, seq;
    for (size = 1, seq = 0; size < x+1; seq++, size = 2*size+1);

    while (size-1 != x){
        size = (size-1)>>1;
        seq--;
        x = x % size;
    }

    return pow(y, seq);
}

// NOTE: assumptions passed in member-variable 'assumptions'.
lbool Solver::solve_()
{
    model.clear();
    conflict.clear();
    if (!ok) return l_False;

    solves++;
    solve_time = 0.0; // we reset the solve_time record, so that successive calls to time-limited solve with the same budget will each get to use that budget, but note that we do not reset the conflict counter or any such

    max_learnts = nClauses() * learntsize_factor;
    if (max_learnts < min_learnts_lim)
        max_learnts = min_learnts_lim;

    learntsize_adjust_confl   = learntsize_adjust_start_confl;
    learntsize_adjust_cnt     = (int)learntsize_adjust_confl;
    lbool   status            = l_Undef;

    if (verbosity >= 1){
        printf("============================[ Search Statistics ]==============================\n");
        printf("| Conflicts |          ORIGINAL         |          LEARNT          | Progress |\n");
        printf("|           |    Vars  Clauses Literals |    Limit  Clauses Lit/Cl |          |\n");
        printf("===============================================================================\n");
    }

    // Search:
    int curr_restarts = 0;
    while (status == l_Undef){
        double rest_base = luby_restart ? luby(restart_inc, curr_restarts) : pow(restart_inc, curr_restarts);
        status = search(rest_base * restart_first);
        if (!withinBudget()) break;
        curr_restarts++;
    }

    if (verbosity >= 1)
        printf("===============================================================================\n");


    if (status == l_True){
        // Extend & copy model:
        model.growTo(nVars());
        for (int i = 0; i < nVars(); i++) model[i] = value(i);
    }else if (status == l_False && conflict.size() == 0)
        ok = false;

    cancelUntil(0);
    fflush(stdout);
    return status;
}


bool Solver::implies(const vec<Lit>& assumps, vec<Lit>& out)
{
    trail_lim.push(trail.size());
    for (int i = 0; i < assumps.size(); i++){
        Lit a = assumps[i];

        if (value(a) == l_False){
            cancelUntil(0);
            return false;
        }else if (value(a) == l_Undef)
            uncheckedEnqueue(a);
    }

    unsigned trail_before = trail.size();
    bool     ret          = true;
    if (propagate() == CRef_Undef){
        out.clear();
        for (int j = trail_before; j < trail.size(); j++)
            out.push(trail[j]);
    }else
        ret = false;
    
    cancelUntil(0);
    return ret;
}

//=================================================================================================
// Writing CNF to DIMACS:
// 
// FIXME: this needs to be rewritten completely.

static Var mapVar(Var x, vec<Var>& map, Var& max)
{
    if (map.size() <= x || map[x] == -1){
        map.growTo(x+1, -1);
        map[x] = max++;
    }
    return map[x];
}


void Solver::toDimacs(FILE* f, Clause& c, vec<Var>& map, Var& max)
{
    if (satisfied(c)) return;

    for (int i = 0; i < c.size(); i++)
        if (value(c[i]) != l_False)
            fprintf(f, "%s%d ", sign(c[i]) ? "-" : "", mapVar(var(c[i]), map, max)+1);
    fprintf(f, "0\n");
}


void Solver::toDimacs(const char *file, const vec<Lit>& assumps)
{
    FILE* f = fopen(file, "wr");
    if (f == NULL)
        fprintf(stderr, "could not open file %s\n", file), exit(1);
    toDimacs(f, assumps);
    fclose(f);
}


void Solver::toDimacs(FILE* f, const vec<Lit>& assumps)
{
    // Handle case when solver is in contradictory state:
    if (!ok){
        fprintf(f, "p cnf 1 2\n1 0\n-1 0\n");
        return; }

    vec<Var> map; Var max = 0;

    // Cannot use removeClauses here because it is not safe
    // to deallocate them at this point. Could be improved.
    int cnt = 0;
    for (int i = 0; i < clauses.size(); i++)
        if (!satisfied(ca[clauses[i]]))
            cnt++;
        
    for (int i = 0; i < clauses.size(); i++)
        if (!satisfied(ca[clauses[i]])){
            Clause& c = ca[clauses[i]];
            for (int j = 0; j < c.size(); j++)
                if (value(c[j]) != l_False)
                    mapVar(var(c[j]), map, max);
        }

    // Assumptions are added as unit clauses:
    cnt += assumps.size();

    fprintf(f, "p cnf %d %d\n", max, cnt);

    for (int i = 0; i < assumps.size(); i++){
        assert(value(assumps[i]) != l_False);
        fprintf(f, "%s%d 0\n", sign(assumps[i]) ? "-" : "", mapVar(var(assumps[i]), map, max)+1);
    }

    for (int i = 0; i < clauses.size(); i++)
        toDimacs(f, ca[clauses[i]], map, max);

    if (verbosity > 0)
        printf("Wrote DIMACS with %d variables and %d clauses.\n", max, cnt);
}


void Solver::printStats() const
{
    double cpu_time = cpuTime();
    double mem_used = memUsedPeak();
    printf("restarts              : %" PRIu64"\n", starts);
    printf("conflicts             : %-12" PRIu64"   (%.0f /sec)\n", conflicts   , conflicts   /cpu_time);
    printf("decisions             : %-12" PRIu64"   (%4.2f %% random) (%.0f /sec)\n", decisions, (float)rnd_decisions*100 / (float)decisions, decisions   /cpu_time);
    printf("propagations          : %-12" PRIu64"   (%.0f /sec)\n", propagations, propagations/cpu_time);
    printf("conflict literals     : %-12" PRIu64"   (%4.2f %% deleted)\n", tot_literals, (max_literals - tot_literals)*100 / (double)max_literals);
    if (mem_used != 0) printf("Memory used           : %.2f MB\n", mem_used);
    printf("CPU time              : %g s\n", cpu_time);
}


//=================================================================================================
// Garbage Collection methods:

void Solver::relocAll(ClauseAllocator& to)
{
    // All watchers:
    //
    watches.cleanAll();
    for (int v = 0; v < nVars(); v++)
        for (int s = 0; s < 2; s++){
            Lit p = mkLit(v, s);
            vec<Watcher>& ws = watches[p];
            for (int j = 0; j < ws.size(); j++)
                ca.reloc(ws[j].cref, to);
        }

    // All reasons:
    //
    for (int i = 0; i < trail.size(); i++){
        Var v = var(trail[i]);

        // Note: it is not safe to call 'locked()' on a relocated clause. This is why we keep
        // 'dangling' reasons here. It is safe and does not hurt.
        if (reason(v) != CRef_Undef && (ca[reason(v)].reloced() || locked(ca[reason(v)]))){
            assert(!isRemoved(reason(v)));
            ca.reloc(vardata[v].reason, to);
        }
    }

    // All learnt:
    //
    int i, j;
    for (i = j = 0; i < learnts.size(); i++)
        if (!isRemoved(learnts[i])){
            ca.reloc(learnts[i], to);
            learnts[j++] = learnts[i];
        }
    learnts.shrink(i - j);

    // All original:
    //
    for (i = j = 0; i < clauses.size(); i++)
        if (!isRemoved(clauses[i])){
            ca.reloc(clauses[i], to);
            clauses[j++] = clauses[i];
        }
    clauses.shrink(i - j);
}


void Solver::garbageCollect()
{
    // Initialize the next region to a size corresponding to the estimated utilization degree. This
    // is not precise but should avoid some unnecessary reallocations for the new region:
    ClauseAllocator to(ca.size() - ca.wasted()); 

    relocAll(to);
    if (verbosity >= 2)
        printf("|  Garbage collection:   %12d bytes => %12d bytes             |\n", 
               ca.size()*ClauseAllocator::Unit_Size, to.size()*ClauseAllocator::Unit_Size);
    to.moveTo(ca);
}

// ----- SMS + step-wise controls -----

int compare( const void* a, const void* b) {
   int int_a = * ( (int*) a );
   int int_b = * ( (int*) b );

   return (int_a > int_b) - (int_a < int_b);
}

int in(int* begin, int* end, int val) {
	if (end - begin < 1) {
		return 0;
	}
	if (end - begin == 1) {
		return *begin == val;
	}
	int* m = begin + (end - begin) / 2;
	if (*m <= val) {
		begin = m;
	} else {
		end = m;
	}
	return in(begin, end, val);
}


// returns false if clause violated at decision level 0
bool Solver::addClauseDuringSearch(vec<Lit> &&clause) {
  /* 1. cancelUntil(highest dec level in clause)
   * 2. learnts.push(...)
   * 3. if unit, then enqueue at this dec level with the new CRef
   * 4. if conflict, then
   * 4a.  if only one assignment at highest dec level, then backtrack before this declevel and enqueue
   * 4b.  if two assignments at highest dec level, then analyze at that level
   */

  // TODO this first sorts the clause so that unassigned literals come first, then by decision level
  // this is potentially expensive and unnecessary; all that we need is that the first two places
  // contain unassigned literals if possible, and highest decision levels otherwise, which can be computed in linear time
  if (clause.size() == 0)
    return false;

  std::sort(&clause[0], &clause.last()+1, [this](Lit la, Lit lb){
        if (this->value(la) == l_Undef)
          return true;
        if (this->value(lb) == l_Undef)
          return false;
        return this->level(var(la)) > this->level(var(lb));
      });

  // Warning: Code below is incomplete, doesn't look for second highest decision level
  /*int maxdl = -1;
  if (clause.size() > 1) {
    int i = 0, j = 0;
    while (i < clause.size()) {
      if (value(clause[i]) == l_Undef) {
        Lit l = clause[j];
        clause[j++] = clause[i];
        clause[i++] = l;
      } else {
        if (level(var(clause[i])) > maxdl) {
          maxdl = level(var(clause[i]));
        }
        i++;
      }
    }
    if (j < 2) {
      for (i = j; i < clause.size(); i++) {
        if (level(var(clause[i])) == maxdl) {
          Lit l = clause[j];
          clause[j++] = clause[i];
          clause[i] = l;
          if (j == 2) {
            break;
          }
        }
      }
    }
  }*/

  // Debug code to check that clause is sorted
  /*for (int i = 1; i < clause.size(); i++) {
    if (value(clause[i-1]) != l_Undef &&
          (value(clause[i]) == l_Undef || level(var(clause[i])) > level(var(clause[i-1])))
        ) {
      printf("ERROR at pos %d: %d has level %d, predecessor %d has level %d\n", i, l2i(clause[i]), level(var(clause[i])), l2i(clause[i-1]), level(var(clause[i-1])));
    }
  }*/

  //printf("  sorted clause:");
  /*for (int i = 0; i < clause.size(); i++) {
    printf(" %d", l2i(clause[i]));
  }
  printf("\n");*/

  int num_unassigned = 0;
  while (num_unassigned < clause.size() && value(clause[num_unassigned]) == l_Undef) {
    num_unassigned++;
  }

  assert(num_unassigned <= 1);

  if (num_unassigned == clause.size()) {
    assert(num_unassigned == 1);
    cancelUntil(0);
    uncheckedEnqueue(clause[0]);
    return true;
  }

  int highest_dl = level(var(clause[num_unassigned]));

  if (highest_dl == 0 && num_unassigned == 0)
    return false;

  int num_highest_dl = 1;
  while (num_unassigned + num_highest_dl < clause.size() && level(var(clause[num_unassigned + num_highest_dl])) == highest_dl) num_highest_dl++;

  if (num_unassigned == 1) {
    cancelUntil(highest_dl);
    CRef cr = ca.alloc(clause, false);
    for (int i = 0; i < clause.size(); i++) {
      varBumpActivity(var(clause[i]));
    }
    clauses.push(cr);
    attachClause(cr);
    uncheckedEnqueue(clause[0], cr);
  } else {
    assert(num_unassigned == 0);
    if (num_highest_dl > 1) {
      cancelUntil(highest_dl);
      CRef cr = ca.alloc(clause, false);
      clauses.push(cr);
      attachClause(cr);

      vec<Lit> learnt_clause;
      int backtrack_level;
      analyze(cr, learnt_clause, backtrack_level);
      cancelUntil(backtrack_level);
      if (learnt_clause.size() == 1){
        uncheckedEnqueue(learnt_clause[0]);
      } else{
        CRef cr = ca.alloc(learnt_clause, true);
        learnts.push(cr);
        attachClause(cr);
        claBumpActivity(ca[cr]);
        uncheckedEnqueue(learnt_clause[0], cr);
      }
    } else {
      // clause to be added is already asserting (after appropriate backtrack)
      if (clause.size() > 1) {
        int second_highest_dl = level(var(clause[1]));
        cancelUntil(second_highest_dl);
        CRef cr = ca.alloc(clause, false);
        for (int i = 0; i < clause.size(); i++) {
          varBumpActivity(var(clause[i]));
        }
        clauses.push(cr);
        attachClause(cr);
        uncheckedEnqueue(clause[0], cr);
      } else {
        cancelUntil(0);
        uncheckedEnqueue(clause[0]);
      }
    }
  }
  return true;
}

// ------ CTYPES API ---------

extern "C" {

  void* create_solver(int vertices, int cutoff, int frequency, long assignmentCutoffPrerunTime, int assignmentCutoff) {
    return new(std::nothrow) Solver(vertices, cutoff, frequency, assignmentCutoffPrerunTime, assignmentCutoff);
  }

  void attach_010_propagator(void* sms_solver, int triangleVars) {
    Solver* s = (Solver*) sms_solver;
    s->sms.prepare010(triangleVars);
  }

  void destroy_solver(void* sms_solver) {
    delete (Solver*) sms_solver;
  }
 
  void add(void* sms_solver, int lit) {
    Solver* s = (Solver*) sms_solver;
    if (lit != 0) {
	  Var v = abs(lit) - 1;
	  while (v >= s->nVars())
		s->newVar();
      s->tmp_clause.push(s->i2l(lit));
    } else {
      s->addTmpClause();
    }
  }

  // call after all clauses have been added
  // performs unit propagation and returns -1, 0, 1 if the formula is
  // falsified, undecided, satisfied

  PropLits propagate(void* sms_solver) {
    Solver* s = (Solver*) sms_solver;
    s->cflr = s->propagate();
    int num_prop_lits = s->nAssigns() - s->trail_lim.last();
    if (s->cflr != CRef_Undef) {
      return {CONFLICT, num_prop_lits};
    } else if (s->nAssigns() == s->nVars()) {
      return {SAT, num_prop_lits};
    } else {
      return {OPEN, num_prop_lits};
    }
  }

  // always call this before a sequence of next_prop_lit
  // will yield all assigned literals starting with some decision level
  int request_propagation_scope(void* sms_solver, int level) {
    Solver* s = (Solver*) sms_solver;
    if (level == 0) {
      s->literator = 0;
    } else if (level < 0 || level > s->decisionLevel()) {
      return 0;
    } else {
      s->literator = s->trail_lim[level-1];
    }
    return 1;
  }

  Lit* trail_location(void* sms_solver, int level) {
    Solver* s = (Solver*) sms_solver;
    if (level == 0) {
      return &s->trail[0];
    } else if (level < 0 || level > s->decisionLevel()) {
      return NULL;
    } else {
      return &s->trail[s->trail_lim[level-1]];
    }
  }

  int next_prop_lit(void* sms_solver) {
    Solver* s = (Solver*) sms_solver;
    if (s->literator >= 0 && s->literator < s->trail.size()) {
      return s->l2i(s->trail[s->literator++]);
    } else {
      s->literator = -1; // iterator exhausted
      return 0;
    }
  }

  // TODO
  /*int next_conflict_lit(void* sms_solver) {
    Solver* s = (Solver*) sms_solver;
    if (s->cflterator > s->conflict.size()) {
      s->cflterator = 0;
      return 0;
    } else {
    }  
  }*/

  PropLits assign_literal(void* sms_solver, int literal) {
    Solver* s = (Solver*) sms_solver;
    if (s->cflr != CRef_Undef) {
      return {CONFLICT, s->nAssigns() - s->trail_lim.last()};
    }
    s->newDecisionLevel();
    s->uncheckedEnqueue(s->i2l(literal));
    return propagate(sms_solver);
  }

  int backtrack(void* sms_solver, int num_dec_levels) {
    Solver* s = (Solver*) sms_solver;
    int target_dec_lev = s->decisionLevel() - num_dec_levels;
    if (target_dec_lev >= 0) {
      s->cancelUntil(target_dec_lev);
      return 1;
    }
    return 0;
  }

  PropLits learn_clause(void* sms_solver) {
    Solver* s = (Solver*) sms_solver;
    if (s->cflr == CRef_Undef) {
      return {OPEN, 0};
    }
    s->lrncls.clear();
    s->analyze(s->cflr, s->lrncls, s->btlev);
    s->cancelUntil(s->btlev);

    if (s->lrncls.size() == 1) {
      s->uncheckedEnqueue(s->lrncls[0]);
      return propagate(sms_solver);
    } else {
      CRef cr = s->ca.alloc(s->lrncls, true);
      s->learnts.push(cr);
      s->attachClause(cr);
      s->claBumpActivity(s->ca[cr]);
      s->uncheckedEnqueue(s->lrncls[0], cr);
      return propagate(sms_solver);
    }
  }

  AssignmentSwitchResult fast_switch_assignment(void* sms_solver, int length, int* literals) {
	  Solver* s = (Solver*) sms_solver;
	  int n_unassigned = 0;
	  for (int i = 0; i < length; i++) {
		  Lit l = s->i2l(literals[i]);
		  if (s->value(var(l)) == l_Undef) {
			  int t = literals[n_unassigned];
			  literals[n_unassigned++] = literals[i];
			  literals[i] = t;
		  }
	  }
	  qsort (literals + n_unassigned, length - n_unassigned, sizeof(int), compare);

	  int btlev;
	  for (btlev = 0; btlev < s->decisionLevel(); btlev++) {
		  Lit dec = s->trail[s->trail_lim[btlev]]; // trail_lim[0] should be the first decision
		  if (!in(literals + n_unassigned, literals + length, s->l2i(dec))) {
			  break;
		  }
	  }
	  s->cancelUntil(btlev);

    int num_prop_lits = s->nAssigns();
    int num_decisions_executed = 0;

    if (btlev == s->decisionLevel()) {
      if (s->cflr != CRef_Undef) {
        return {CONFLICT, num_decisions_executed, num_prop_lits};
      }
    }
    // this function will report the total number of propagated literals including propagations that had already been in place
	  for (int i = 0; i < length; i++) {
		  Lit l = s->i2l(literals[i]);
		  if (s->value(l) == l_Undef) {
        num_decisions_executed++;
			  s->newDecisionLevel();
			  s->uncheckedEnqueue(l);
			  s->cflr = s->propagate();
			  if (s->cflr != CRef_Undef) {
			    return {CONFLICT, num_decisions_executed, num_prop_lits};
			  }
		  } else if (s->value(l) == l_False) {
        // conflict (attempt to assign counter to current value)
        // WARNING: this returns a conflict state, but the solver is actually not in conflict, it is only about to enter one
        // a clause cannot be learned the usual way, must call analyzeFinal
        s->analyzeFinal(l, s->conflict);
        s->cflterator = 0;
        return {INCONSISTENT_ASSUMPTIONS, num_decisions_executed + 1, num_prop_lits + 1};
      } else {
        // attempt to assign in a way that was already propagated, just ignore
      }
	  }

    if (s->nAssigns() == s->nVars()) {
      return {SAT, num_decisions_executed, num_prop_lits};
    } else { 
      return {OPEN, num_decisions_executed, num_prop_lits};
    }
  }

  int run_solver(void* sms_solver, double secs) {
	  Solver* s = (Solver*) sms_solver;
    s->time_budget = secs;
    lbool result = s->solve_();
    if (result == l_True) {
      return 10;
    } else if (result == l_False) {
      return 20;
    } else {
      return 0;
    }
  }

  EnumerationResult run_solver_enumerate(void* sms_solver, double secs, bool store_solutions = true, int max_sol = INT32_MAX) {
	  Solver* s = (Solver*) sms_solver;
    s->time_budget = secs;
    lbool result = l_Undef;
    while (result != l_False) {
      result = s->solve_();
      if (result == l_True) {
        int n = s->sms.config.vertices;
        int m = n*(n-1)/2;
        s->num_sol++;

        vec<Lit> blocking_clause(m);
        for (Var v = 0; v < m; v++) {
          blocking_clause.push(mkLit(v, s->modelValue(v) == l_True));
        }
        if (store_solutions) {
          s->solution_store.push_back(vector<Lit>(m));
          for (int i = 0; i < blocking_clause.size(); i++) {
            s->solution_store.back().push_back(~blocking_clause[i]);
          }
        }

        s->addClause(blocking_clause);
        if (s->num_sol >= max_sol) {
          return {s->num_sol, LIMIT};
        }
      } else if (result == l_Undef) {
        // time's up
        return {s->num_sol, TIME};
      }
    }

    return {s->num_sol, DONE};
  }

  int model_value(void* sms_solver, int literal) {
	  Solver* s = (Solver*) sms_solver;
    return s->modelValue(s->i2l(literal)) == l_True;
  }

  void block_model(void* sms_solver) {
	  Solver* s = (Solver*) sms_solver;
    //printf("model block ");
    for (Var v = 0; v < s->nVars(); v++) {
      Lit l = mkLit(v, s->modelValue(v) == l_True);
      //printf("%d ", s->l2i(l));
      s->tmp_clause.push(mkLit(v, s->modelValue(v) == l_True));
    }
    //printf("\n");
    s->addTmpClause();
  }

  int n_vars(void* sms_solver) {
	  Solver* s = (Solver*) sms_solver;
    return s->nVars();
  }

  void print_stats(void* sms_solver) {
	  Solver* s = (Solver*) sms_solver;
    s->sms.checker.printStats();
    if (s->sms.prop010) {
      s->sms.checker010.printStats();
    }
  }

}
