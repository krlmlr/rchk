
#include "callocators.h"
#include "errors.h"
#include "guards.h"
#include "symbols.h"
#include "linemsg.h"
#include "state.h"
#include "table.h"

#include <map>
#include <stack>
#include <unordered_set>

#include <llvm/IR/CallSite.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;

  // FIXME: could reduce copy-paste vs. bcheck?
const bool DEBUG = false;
const bool TRACE = false;
const bool UNIQUE_MSG = true;
const int MAX_STATES = CALLOCATORS_MAX_STATES;
const bool VERBOSE_DUMP = false;

const bool DUMP_STATES = false;
const std::string DUMP_STATES_FUNCTION = "bcEval"; // only dump states in this function
const bool ONLY_CHECK_ONLY_FUNCTION = false; // only check one function (named ONLY_FUNCTION_NAME)
const std::string ONLY_FUNCTION_NAME = "bcEval";
const bool ONLY_DEBUG_ONLY_FUNCTION = true;
const bool ONLY_TRACE_ONLY_FUNCTION = true;

const bool KEEP_CALLED_IN_STATE = false;

std::string CalledFunctionTy::getNameSuffix() const {
  std::string suff;
  unsigned nKnown = 0;

  for(ArgInfosVectorTy::const_iterator ai = argInfo->begin(), ae = argInfo->end(); ai != ae; ++ai) {
    const ArgInfoTy *a = *ai;
    if (ai != argInfo->begin()) {
      suff += ",";
    }
    if (a && a->isSymbol()) {
      suff += "S:" + cast<SymbolArgInfoTy>(a)->symbolName;
      nKnown++;
    } else {
      suff += "?";
    }
  }
  
  if (nKnown > 0) {
    return "(" + suff + ")";
  }
  return std::string();
}


std::string CalledFunctionTy::getName() const {
  return fun->getName().str() + getNameSuffix();
}

size_t CalledFunctionTy_hash::operator()(const CalledFunctionTy& t) const {
  size_t res = 0;
  hash_combine(res, t.fun);
  hash_combine(res, t.argInfo); // argInfos are interned
  return res;
}

bool CalledFunctionTy_equal::operator() (const CalledFunctionTy& lhs, const CalledFunctionTy& rhs) const {
  return lhs.fun == rhs.fun && lhs.argInfo == rhs.argInfo && lhs.module == rhs.module;  // argInfos are interned
}

SymbolArgInfoTy::SymbolArgInfoTableTy SymbolArgInfoTy::table;

size_t ArgInfosVectorTy_hash::operator()(const ArgInfosVectorTy& t) const {
  size_t res = 0;
  hash_combine(res, t.size());
  
  size_t cnt = 0;
  for(ArgInfosVectorTy::const_iterator ai = t.begin(), ae = t.end(); ai != ae; ++ai) {
    const ArgInfoTy *a = *ai;
    if (a && a->isSymbol()) {
      hash_combine(res, cast<SymbolArgInfoTy>(a)->symbolName);
      cnt++;
    }
  }
  hash_combine(res, cnt);
  return res;
}

const CalledFunctionTy* CalledModuleTy::getCalledFunction(Function *f) {
  size_t nargs = f->arg_size();
  ArgInfosVectorTy argInfos(nargs, NULL);
  CalledFunctionTy calledFunction(f, intern(argInfos), this);
  return intern(calledFunction);
}

const CalledFunctionTy* CalledModuleTy::getCalledFunction(Value *inst, bool registerCallSite) {
  return getCalledFunction(inst, NULL, registerCallSite);
}

const CalledFunctionTy* CalledModuleTy::getCalledFunction(Value *inst, SEXPGuardsTy *sexpGuards, bool registerCallSite) {
  // FIXME: this is quite inefficient, does a lot of allocation
  
  CallSite cs (inst);
  if (!cs) {
    return NULL;
  }
  Function *fun = cs.getCalledFunction();
  if (!fun) {
    return NULL;
  }
      
  // build arginfo
      
  unsigned nargs = cs.arg_size();
  ArgInfosVectorTy argInfo(nargs, NULL);

  for(unsigned i = 0; i < nargs; i++) {
    Value *arg = cs.getArgument(i);
    if (LoadInst::classof(arg)) { // R_XSymbol
      Value *src = cast<LoadInst>(arg)->getPointerOperand();
      if (GlobalVariable::classof(src)) {
        auto ssearch = symbolsMap->find(cast<GlobalVariable>(src));
        if (ssearch != symbolsMap->end()) {
          argInfo[i] = SymbolArgInfoTy::create(ssearch->second);
          continue;
        }
      }
      if (sexpGuards && AllocaInst::classof(src)) {
        AllocaInst *var = cast<AllocaInst>(src);
        auto gsearch = sexpGuards->find(var);
        if (gsearch != sexpGuards->end()) {
          std::string symbolName;
          SEXPGuardState gs = getSEXPGuardState(*sexpGuards, var, symbolName);
          if (gs == SGS_SYMBOL) {
            argInfo[i] = SymbolArgInfoTy::create(symbolName);
            continue;
          }
        }
      }
    }
    std::string symbolName;  // install("X")
    if (isInstallConstantCall(arg, symbolName)) {
      argInfo[i] = SymbolArgInfoTy::create(symbolName);
      continue;
    }
    // not a symbol, leave argInfo as NULL
  }
      
  CalledFunctionTy calledFunction(fun, intern(argInfo), this);
  const CalledFunctionTy* cf = intern(calledFunction);
  
  if (registerCallSite) {
    auto csearch = callSiteTargets.find(inst);
    if (csearch == callSiteTargets.end()) {
      CalledFunctionsSetTy newSet;
      newSet.insert(cf);
      callSiteTargets.insert({inst, newSet});
    } else {
      CalledFunctionsSetTy& existingSet = csearch->second;
      existingSet.insert(cf);
    }
  }
  
  return cf;
}

CalledModuleTy::CalledModuleTy(Module *m, SymbolsMapTy *symbolsMap, FunctionsSetTy* errorFunctions, GlobalsTy* globals, 
  FunctionsSetTy* possibleAllocators, FunctionsSetTy* allocatingFunctions):
  
  m(m), symbolsMap(symbolsMap), errorFunctions(errorFunctions), globals(globals), possibleAllocators(possibleAllocators), allocatingFunctions(allocatingFunctions),
  callSiteTargets(), gcFunction(getCalledFunction(getGCFunction(m))) {

  for(Module::iterator fi = m->begin(), fe = m->end(); fi != fe; ++fi) {
    Function *fun = fi;

    assert(fun);
    getCalledFunction(fun); // make sure each function has a called function counterpart
    for(Value::user_iterator ui = fun->user_begin(), ue = fun->user_end(); ui != ue; ++ui) {
      User *u = *ui;
      getCalledFunction(cast<Value>(u)); // NOTE: this only gets contexts that are constant, more are gotten during allocators computation
    }
  }  
    // only compute on demand - it takes a bit of time
  possibleCAllocators = NULL;
  allocatingCFunctions = NULL;
}

CalledModuleTy::~CalledModuleTy() {

  if (possibleCAllocators) {
    delete possibleCAllocators;
  }
  if (allocatingCFunctions) {
    delete allocatingCFunctions;
  }
}

CalledModuleTy* CalledModuleTy::create(Module *m) {
  SymbolsMapTy *symbolsMap = new SymbolsMapTy();
  findSymbols(m, symbolsMap);

  FunctionsSetTy *errorFunctions = new FunctionsSetTy();
  findErrorFunctions(m, *errorFunctions);
  
  GlobalsTy *globals = new GlobalsTy(m);
  
  FunctionsSetTy *possibleAllocators = new FunctionsSetTy();
  findPossibleAllocators(m, *possibleAllocators);

  FunctionsSetTy *allocatingFunctions = new FunctionsSetTy();
  findAllocatingFunctions(m, *allocatingFunctions);
      
  return new CalledModuleTy(m, symbolsMap, errorFunctions, globals, possibleAllocators, allocatingFunctions);
}

void CalledModuleTy::release(CalledModuleTy *cm) {
  delete cm->getAllocatingFunctions();
  delete cm->getPossibleAllocators();
  delete cm->getGlobals();
  delete cm->getErrorFunctions();
  delete cm->getSymbolsMap();
  delete cm;
}

typedef std::map<AllocaInst*,const CalledFunctionsOrderedSetTy*> InternedVarOriginsTy;
typedef std::map<AllocaInst*,CalledFunctionsOrderedSetTy> VarOriginsTy; // uninterned

  // for a local variable, a list of functions whose return values may have
  // been assigned, possibly indirectly, to that variable

struct CalledFunctionsOSTableTy_hash {
  size_t operator()(const CalledFunctionsOrderedSetTy& t) const {
    size_t res = 0;
    hash_combine(res, t.size());
        
    for(CalledFunctionsOrderedSetTy::const_iterator fi = t.begin(), fe = t.end(); fi != fe; ++fi) {
      const CalledFunctionTy *f = *fi;
      hash_combine(res, (const void *) f);
    } // ordered set
    return res;
  }
};

typedef InterningTable<CalledFunctionsOrderedSetTy, CalledFunctionsOSTableTy_hash> CalledFunctionsOSTableTy;

struct CAllocStateTy;

struct CAllocPackedStateTy : public PackedStateWithGuardsTy {
  const size_t hashcode;
  const CalledFunctionsOrderedSetTy *called;
  const InternedVarOriginsTy varOrigins;
  
  
  CAllocPackedStateTy(size_t hashcode, BasicBlock* bb, const PackedIntGuardsTy& intGuards, const PackedSEXPGuardsTy& sexpGuards,
    const InternedVarOriginsTy& varOrigins, const CalledFunctionsOrderedSetTy *called):
    
    hashcode(hashcode), PackedStateBaseTy(bb), PackedStateWithGuardsTy(bb, intGuards, sexpGuards), varOrigins(varOrigins), called(called) {};
    
  static CAllocPackedStateTy create(CAllocStateTy& us, IntGuardsCheckerTy& intGuardsChecker, SEXPGuardsCheckerTy& sexpGuardsChecker);
};

static VarOriginsTy unpackVarOrigins(const InternedVarOriginsTy& internedOrigins) {

  VarOriginsTy varOrigins;

  for(InternedVarOriginsTy::const_iterator oi = internedOrigins.begin(), oe = internedOrigins.end(); oi != oe; ++oi) {
    AllocaInst* var = oi->first;
    const CalledFunctionsOrderedSetTy* srcs = oi->second;
    varOrigins.insert({var, *srcs});
  }
  
  return varOrigins;
}

static CalledFunctionsOSTableTy osTable; // interned ordered sets

static InternedVarOriginsTy packVarOrigins(const VarOriginsTy& varOrigins) {

  InternedVarOriginsTy internedOrigins;

  for(VarOriginsTy::const_iterator oi = varOrigins.begin(), oe = varOrigins.end(); oi != oe; ++oi) {
    AllocaInst* var = oi->first;
    const CalledFunctionsOrderedSetTy& srcs = oi->second;
    internedOrigins.insert({var, osTable.intern(srcs)});
  }
  
  return internedOrigins;
}

struct CAllocStateTy : public StateWithGuardsTy {
  CalledFunctionsOrderedSetTy called;
  VarOriginsTy varOrigins;
  
  CAllocStateTy(const CAllocPackedStateTy& ps, IntGuardsCheckerTy& intGuardsChecker, SEXPGuardsCheckerTy& sexpGuardsChecker):
    CAllocStateTy(ps.bb, intGuardsChecker.unpack(ps.intGuards), sexpGuardsChecker.unpack(ps.sexpGuards), *ps.called, unpackVarOrigins(ps.varOrigins)) {};

  CAllocStateTy(BasicBlock *bb): StateBaseTy(bb), StateWithGuardsTy(bb), called(), varOrigins() {};

  CAllocStateTy(BasicBlock *bb, const IntGuardsTy& intGuards, const SEXPGuardsTy& sexpGuards, const CalledFunctionsOrderedSetTy& called, const VarOriginsTy& varOrigins):
    StateBaseTy(bb), StateWithGuardsTy(bb, intGuards, sexpGuards), called(called), varOrigins(varOrigins) {};
      
  virtual CAllocStateTy* clone(BasicBlock *newBB) {
    return new CAllocStateTy(newBB, intGuards, sexpGuards, called, varOrigins);
  }
    
  void dump(std::string dumpMsg) {
    StateBaseTy::dump(VERBOSE_DUMP);
    StateWithGuardsTy::dump(VERBOSE_DUMP);

    if (KEEP_CALLED_IN_STATE) {
      errs() << "=== called (allocating):\n";
      for(CalledFunctionsOrderedSetTy::iterator fi = called.begin(), fe = called.end(); fi != fe; *fi++) {
        const CalledFunctionTy* f = *fi;
        errs() << "   " << funName(f) << "\n";
      }
    }
    errs() << "=== origins (allocators):\n";
    for(VarOriginsTy::const_iterator oi = varOrigins.begin(), oe = varOrigins.end(); oi != oe; ++oi) {
      AllocaInst* var = oi->first;
      const CalledFunctionsOrderedSetTy& srcs = oi->second;

      errs() << "   " << varName(var) << ":";
        
      for(CalledFunctionsOrderedSetTy::const_iterator fi = srcs.begin(), fe = srcs.end(); fi != fe; ++fi) {
        const CalledFunctionTy *f = *fi;
        errs() << " " << funName(f);
      }
      errs() << "\n";
    }
    errs() << " ######################" << dumpMsg << "######################\n";
  }
    
  virtual bool add();
};


CAllocPackedStateTy CAllocPackedStateTy::create(CAllocStateTy& us, IntGuardsCheckerTy& intGuardsChecker, SEXPGuardsCheckerTy& sexpGuardsChecker) {

  InternedVarOriginsTy internedOrigins = packVarOrigins(us.varOrigins);
   
  size_t res = 0;
  hash_combine(res, us.bb);
  intGuardsChecker.hash(res, us.intGuards);
  sexpGuardsChecker.hash(res, us.sexpGuards);
    
  hash_combine(res, internedOrigins.size());
  for(InternedVarOriginsTy::const_iterator oi = internedOrigins.begin(), oe = internedOrigins.end(); oi != oe; ++oi) {
    AllocaInst* var = oi->first;
    const CalledFunctionsOrderedSetTy* srcs = oi->second;
    hash_combine(res, (const void *)srcs); // interned
  } // ordered map
    
  return CAllocPackedStateTy(res, us.bb, intGuardsChecker.pack(us.intGuards), sexpGuardsChecker.pack(us.sexpGuards), internedOrigins, osTable.intern(us.called));
}
  
// the hashcode is cached at the time of first hashing
//   (and indeed is not copied)

struct CAllocPackedStateTy_hash {
  size_t operator()(const CAllocPackedStateTy& t) const {
    return t.hashcode;
  }
};

struct CAllocPackedStateTy_equal {
  bool operator() (const CAllocPackedStateTy& lhs, const CAllocPackedStateTy& rhs) const {
    return lhs.bb == rhs.bb && lhs.intGuards == rhs.intGuards && lhs.sexpGuards == rhs.sexpGuards && 
      lhs.varOrigins == rhs.varOrigins;
  }
};

typedef std::stack<const CAllocPackedStateTy*> WorkListTy;
typedef std::unordered_set<CAllocPackedStateTy, CAllocPackedStateTy_hash, CAllocPackedStateTy_equal> DoneSetTy;

static WorkListTy workList; // FIXME: avoid these "globals"
static DoneSetTy doneSet;   // FIXME: avoid these "globals"

static IntGuardsCheckerTy intGuardsChecker; // FIXME: avoid these "globals"
static SEXPGuardsCheckerTy sexpGuardsChecker; // FIXME: avoid these "globals"


bool CAllocStateTy::add() {

  CAllocPackedStateTy ps = CAllocPackedStateTy::create(*this, intGuardsChecker, sexpGuardsChecker);
  delete this; // NOTE: state suicide
  auto sinsert = doneSet.insert(ps);
  if (sinsert.second) {
    const CAllocPackedStateTy* insertedState = &*sinsert.first;
    workList.push(insertedState); // make the worklist point to the doneset
    return true;
  } else {
    return false;
  }
}

static void clearStates() { // FIXME: avoid copy paste (vs. bcheck)
  // clear the worklist and the doneset
  doneSet.clear();
  WorkListTy empty;
  std::swap(workList, empty);
  osTable.clear();
  sexpGuardsChecker.clear();
  intGuardsChecker.clear();
}

static void getCalledAndWrappedFunctions(const CalledFunctionTy *f, LineMessenger& msg, 
  CalledFunctionsOrderedSetTy& called, CalledFunctionsOrderedSetTy& wrapped) {

  if (!f->fun || !f->fun->size()) {
    return;
  }
  CalledModuleTy *cm = f->module;
    
  VarBoolCacheTy intGuardVarsCache;
  VarBoolCacheTy sexpGuardVarsCache;

  BasicBlocksSetTy errorBasicBlocks;
  findErrorBasicBlocks(f->fun, cm->getErrorFunctions(), errorBasicBlocks); // FIXME: this could be remembered in CalledFunction
    
  VarsSetTy possiblyReturnedVars; 
  findPossiblyReturnedVariables(f->fun, possiblyReturnedVars); // to restrict origin tracking
    
  bool trackOrigins = isSEXP(f->fun->getReturnType());
    
  if (DEBUG && ONLY_DEBUG_ONLY_FUNCTION) {
    if (ONLY_FUNCTION_NAME == funName(f)) {
      msg.debug(true);
    } else {
      msg.debug(false);
    }
  }

  if (TRACE && ONLY_TRACE_ONLY_FUNCTION) {
    if (ONLY_FUNCTION_NAME == funName(f)) {
      msg.trace(true);
    } else {
      msg.trace(false);
    }
  }
    
  msg.newFunction(f->fun, " - " + funName(f));
  sexpGuardsChecker.reset(f->fun);
  intGuardsChecker.reset(f->fun);

  clearStates();
  {
    CAllocStateTy* initState = new CAllocStateTy(&f->fun->getEntryBlock());
    initState->add();
  }
  while(!workList.empty()) {
    CAllocStateTy s(*workList.top(), intGuardsChecker, sexpGuardsChecker); // unpacks the state
    workList.pop();    

    if (DUMP_STATES && (DUMP_STATES_FUNCTION.empty() || DUMP_STATES_FUNCTION == f->getName())) {
      msg.trace("going to work on this state:", s.bb->begin());
      s.dump("worklist top");
    }    

    if (ONLY_CHECK_ONLY_FUNCTION && ONLY_FUNCTION_NAME != f->getName()) {
      continue;
    }      

    if (errorBasicBlocks.find(s.bb) != errorBasicBlocks.end()) {
      msg.debug("ignoring basic block on error path", s.bb->begin());
      continue;
    }
      
    if (doneSet.size() > MAX_STATES) {
      errs() << "ERROR: too many states (abstraction error?) in function " << funName(f) << "\n";
      clearStates();
        
      // NOTE: some callsites may have already been registered to more specific called functions
      bool originAllocating = cm->isAllocating(f->fun);
      bool originAllocator = cm->isPossibleAllocator(f->fun);
        
      if (!originAllocating && !originAllocator) {
        return;
      }
      for(inst_iterator ini = inst_begin(*f->fun), ine = inst_end(*f->fun); ini != ine; ++ini) {
        Instruction *in = &*ini;
          
        if (errorBasicBlocks.find(in->getParent()) != errorBasicBlocks.end()) {
          continue;
        }
        CallSite cs(in);
        const CalledFunctionTy *ct = cm->getCalledFunction(in, true);
        if (cs) {
          assert(ct);
          Function *t = cs.getCalledFunction();
            // note that this is a heuristic, best-effort approach that is not equivalent to what allocators.cpp do
            //   this heuristic may treat a function as wrapped even when allocators.cpp will not
            //
            // on the other hand, we may discover that a call is in a context that makes it non-allocating/non-allocator
            // it would perhaps be cleaner to re-use the context-insensitive algorithm here
            // or just improve performance so that we don't run out of states in the first place
          if (originAllocating && cm->isAllocating(t)) {
            called.insert(ct);
          }
          if (originAllocator && cm->isPossibleAllocator(t)) {
            wrapped.insert(ct);
          }
        }
      }
      return;
    }
      
    // process a single basic block
    // FIXME: phi nodes
      
    for(BasicBlock::iterator in = s.bb->begin(), ine = s.bb->end(); in != ine; ++in) {
      msg.trace("visiting", in);
   
      handleIntGuardsForNonTerminator(in, intGuardVarsCache, s.intGuards, msg);
      handleSEXPGuardsForNonTerminator(in, sexpGuardVarsCache, s.sexpGuards, cm->getGlobals(), f->argInfo, cm->getSymbolsMap(), msg, NULL);
        
      // handle stores
      if (trackOrigins && StoreInst::classof(in)) {
        StoreInst *st = cast<StoreInst>(in);
          
        if (AllocaInst::classof(st->getPointerOperand())) {
          AllocaInst *dst = cast<AllocaInst>(st->getPointerOperand());
          if (possiblyReturnedVars.find(dst) != possiblyReturnedVars.end()) {
            
            // FIXME: should also handle phi nodes here, currently we may miss some allocators
            if (msg.debug()) msg.debug("dropping origins of " + varName(dst) + " at variable overwrite", in);
            s.varOrigins.erase(dst);
            
            // dst is a variable to be tracked
            if (LoadInst::classof(st->getValueOperand())) {
              Value *src = cast<LoadInst>(st->getValueOperand())->getPointerOperand();
              if (AllocaInst::classof(src)) {
                // copy all var origins of src into dst
                if (msg.debug()) msg.debug("propagating origins on assignment of " + varName(cast<AllocaInst>(src)) + " to " + varName(dst), in); 
                auto sorig = s.varOrigins.find(cast<AllocaInst>(src));
                if (sorig != s.varOrigins.end()) {
                  CalledFunctionsOrderedSetTy& srcOrigs = sorig->second;
                  s.varOrigins.insert({dst, srcOrigs}); // set (copy) origins
                }
              }
              continue;
            }
            const CalledFunctionTy *tgt = cm->getCalledFunction(st->getValueOperand(), &s.sexpGuards, true);
            if (tgt && cm->isPossibleAllocator(tgt->fun)) {
              // storing a value gotten from a (possibly allocator) function
              if (msg.debug()) msg.debug("setting origin " + funName(tgt) + " of " + varName(dst), in); 
              CalledFunctionsOrderedSetTy newOrigins;
              newOrigins.insert(tgt);
              s.varOrigins.insert({dst, newOrigins});
              continue;
            }
          }
        }
      }
        
      // handle calls
      const CalledFunctionTy *tgt = cm->getCalledFunction(in, &s.sexpGuards, true);
      if (tgt && cm->isAllocating(tgt->fun)) {
        if (msg.debug()) msg.debug("recording call to " + funName(tgt), in);
          
        if (KEEP_CALLED_IN_STATE) {  
          if (called.find(tgt) == called.end()) { // if we already know the function is called, don't add, save memory
            s.called.insert(tgt);
          }
        } else {
          called.insert(tgt);
        }
      }
    }
      
    TerminatorInst *t = s.bb->getTerminator();
      
    if (ReturnInst::classof(t)) { // handle return statement

      if (KEEP_CALLED_IN_STATE) {
        if (msg.debug()) msg.debug("collecting " + std::to_string(s.called.size()) + " calls at function return", t);
        called.insert(s.called.begin(), s.called.end());
      }

      if (trackOrigins) {
        Value *returnOperand = cast<ReturnInst>(t)->getReturnValue();
        if (LoadInst::classof(returnOperand)) { // return(var)
          Value *src = cast<LoadInst>(returnOperand)->getPointerOperand();
          if (AllocaInst::classof(src)) {
              
            auto origins = s.varOrigins.find(cast<AllocaInst>(src));
            size_t nOrigins = 0;
            if (origins != s.varOrigins.end()) {
              CalledFunctionsOrderedSetTy& knownOrigins = origins->second;
              wrapped.insert(knownOrigins.begin(), knownOrigins.end()); // copy origins as result
              nOrigins = knownOrigins.size();
            }
            if (msg.debug()) msg.debug("collecting " + std::to_string(nOrigins) + " at function return, variable " + varName(cast<AllocaInst>(src)), t); 
          }
        }
        const CalledFunctionTy *tgt = cm->getCalledFunction(returnOperand, &s.sexpGuards, true);
        if (tgt && cm->isPossibleAllocator(tgt->fun)) { // return(foo())
          if (msg.debug()) msg.debug("collecting immediate origin " + funName(tgt) + " at function return", t); 
          wrapped.insert(tgt);
        }   
      }
    }

    if (handleSEXPGuardsForTerminator(t, sexpGuardVarsCache, s, cm->getGlobals(), f->argInfo, cm->getSymbolsMap(), msg)) {
      continue;
    }

    if (handleIntGuardsForTerminator(t, intGuardVarsCache, s, msg)) {
      continue;
    }
      
    // add conservatively all cfg successors
    for(int i = 0, nsucc = t->getNumSuccessors(); i < nsucc; i++) {
      BasicBlock *succ = t->getSuccessor(i);
      {
        CAllocStateTy* state = s.clone(succ);
        if (state->add()) {
          msg.trace("added successor of", t);
        }
      }
    }
  }
  clearStates();
  if (trackOrigins && called.find(cm->getCalledGCFunction()) != called.end()) {
    // the GC function is an exception
    //   even though it does not return SEXP, any function that calls it and returns an SEXP is regarded as wrapping it
    //   (this is a heuristic)
    wrapped.insert(cm->getCalledGCFunction());
  }
}

typedef std::vector<std::vector<bool>> BoolMatrixTy;
typedef std::vector<unsigned> AdjacencyListRow;
typedef std::vector<AdjacencyListRow> AdjacencyListTy;

static void resize(AdjacencyListTy& list, unsigned n) {
  list.resize(n);
}

static void resize(BoolMatrixTy& matrix, unsigned n) {
  unsigned oldn = matrix.size();
  if (n <= oldn) {
    return;
  }
  matrix.resize(n);
  for (int i = 0; i < n; i++) {
    matrix[i].resize(n);
  }
}

static void buildClosure(BoolMatrixTy& mat, AdjacencyListTy& list, unsigned n) {

  bool added = true;
  while(added) {
    added = false;
    
    for(unsigned i = 0; i < n; i++) {
      for(unsigned jidx = 0; jidx < list[i].size(); jidx++) {
        unsigned j = list[i][jidx];
        if (i == j) {
          continue;
        }
        for(unsigned kidx = 0; kidx < list[j].size(); kidx++) {
          unsigned k = list[j][kidx];
          if (j == k) {
            continue;
          }
          if (!mat[i][k]) {
            mat[i][k] = true;
            list[i].push_back(k);
            added = true;
          }
        }
      }
    }
  }
}

void CalledModuleTy::computeCalledAllocators() {

  // find calls and variable origins for each called function
  // then create a "callgraph" out of these
  // and then compute call graph closure
  //
  // for performance, restrict variable origins to possible allocators
  // and restrict calls to possibly allocating functions
  
  if (possibleCAllocators && allocatingCFunctions) {
    return;
  }
  
  possibleCAllocators = new CalledFunctionsSetTy();
  allocatingCFunctions = new CalledFunctionsSetTy();
  
  LineMessenger msg(m->getContext(), DEBUG, TRACE, UNIQUE_MSG);
  
  unsigned nfuncs = getNumberOfCalledFunctions(); // NOTE: nfuncs can increase during the checking

  BoolMatrixTy callsMat(nfuncs, std::vector<bool>(nfuncs));  // calls[i][j] - function i calls function j
  AdjacencyListTy callsList(nfuncs, AdjacencyListRow()); // calls[i] - list of functions called by i
  BoolMatrixTy wrapsMat(nfuncs, std::vector<bool>(nfuncs));  // wraps[i][j] - function i wraps function j
  AdjacencyListTy wrapsList(nfuncs, AdjacencyListRow()); // wraps[i] - list of functions wrapped by i
  
  for(unsigned i = 0; i < getNumberOfCalledFunctions(); i++) {

    const CalledFunctionTy *f = getCalledFunction(i);
    if (!f->fun || !f->fun->size() || !isAllocating(f->fun)) {
      continue;
    }
    
    CalledFunctionsOrderedSetTy called;
    CalledFunctionsOrderedSetTy wrapped;
    getCalledAndWrappedFunctions(f, msg, called, wrapped);
    
    if (DEBUG && called.size()) {
      errs() << "\nDetected (possible allocators) called by function " << funName(f) << ":\n";
      for(CalledFunctionsOrderedSetTy::const_iterator cfi = called.begin(), cfe = called.end(); cfi != cfe; ++cfi) {
        const CalledFunctionTy *cf = *cfi;
        errs() << "   " << funName(cf) << "\n";
      }
    }
    if (DEBUG && wrapped.size()) {
      errs() << "\nDetected (possible allocators) wrapped by function " << funName(f) << ":\n";
      for(CalledFunctionsOrderedSetTy::const_iterator cfi = wrapped.begin(), cfe = wrapped.end(); cfi != cfe; ++cfi) {
        const CalledFunctionTy *cf = *cfi;
        errs() << "   " << funName(cf) << "\n";
      }
    }
    
    nfuncs = getNumberOfCalledFunctions(); // get the current size
    resize(callsList, nfuncs);
    resize(wrapsList, nfuncs);
    resize(callsMat, nfuncs);
    resize(wrapsMat, nfuncs);
    
    for(CalledFunctionsOrderedSetTy::const_iterator cfi = called.begin(), cfe = called.end(); cfi != cfe; ++cfi) {
      const CalledFunctionTy *cf = *cfi;
      callsMat[f->idx][cf->idx] = true;
      callsList[f->idx].push_back(cf->idx);
    }

    for(CalledFunctionsOrderedSetTy::const_iterator wfi = wrapped.begin(), wfe = wrapped.end(); wfi != wfe; ++wfi) {
      const CalledFunctionTy *wf = *wfi;
      wrapsMat[f->idx][wf->idx] = true;
      wrapsList[f->idx].push_back(wf->idx);
    }    
  }
  
  // calculate transitive closure

  buildClosure(callsMat, callsList, nfuncs);
  buildClosure(wrapsMat, wrapsList, nfuncs);
  
  // fill in results
  
  unsigned gcidx = gcFunction->idx;
  for(unsigned i = 0; i < nfuncs; i++) {
    if (callsMat[i][gcidx]) {
      allocatingCFunctions->insert(getCalledFunction(i));
    }
    if (wrapsMat[i][gcidx]) {
      const CalledFunctionTy *tgt = getCalledFunction(i);
      if (!isKnownNonAllocator(tgt->fun)) {
        possibleCAllocators->insert(tgt);
      }
    }    
  }
  allocatingCFunctions->insert(gcFunction);
  possibleCAllocators->insert(gcFunction);
}

std::string funName(const CalledFunctionTy *cf) {
  return funName(cf->fun) + cf->getNameSuffix();  
}
