/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/vm/unwind.h"

#include <boost/implicit_cast.hpp>

#include <folly/ScopeGuard.h>

#include "hphp/util/trace.h"

#include "hphp/runtime/base/tv-refcount.h"
#include "hphp/runtime/ext/asio/ext_async-function-wait-handle.h"
#include "hphp/runtime/ext/asio/ext_async-generator-wait-handle.h"
#include "hphp/runtime/ext/asio/ext_async-generator.h"
#include "hphp/runtime/ext/asio/ext_static-wait-handle.h"
#include "hphp/runtime/ext/generator/ext_generator.h"
#include "hphp/runtime/vm/bytecode.h"
#include "hphp/runtime/vm/debugger-hook.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/hhbc.h"
#include "hphp/runtime/vm/hhbc-codec.h"
#include "hphp/runtime/vm/resumable.h"
#include "hphp/runtime/vm/runtime.h"
#include "hphp/runtime/vm/unit.h"
#include "hphp/runtime/vm/vm-regs.h"

namespace HPHP {

TRACE_SET_MOD(unwind);
using boost::implicit_cast;

namespace {

//////////////////////////////////////////////////////////////////////

#if (!defined(NDEBUG) || defined(USE_TRACE))
std::string describeEx(Either<ObjectData*, Exception*> exception) {
  if (exception.left()) {
    return folly::format("[user exception] {}",
                         implicit_cast<void*>(exception.left())).str();
  }
  return folly::format("[C++ exception] {}",
                       implicit_cast<void*>(exception.right())).str();
}
#endif

void discardStackTemps(const ActRec* const fp, Stack& stack) {
  ITRACE(2, "discardStackTemps with fp {} sp {}\n",
         implicit_cast<const void*>(fp),
         implicit_cast<void*>(stack.top()));

  visitStackElems(
    fp, stack.top(),
    [&] (TypedValue* tv) {
      assertx(tv == stack.top());
      ITRACE(2, "  unwind pop TV : {}\n",
             implicit_cast<void*>(stack.top()));
      stack.popTV();
    }
  );

  ITRACE(2, "discardStackTemps ends with sp = {}\n",
         implicit_cast<void*>(stack.top()));
}

void discardMemberTVRefs(PC pc) {
  auto const throwOp = peek_op(pc);

  /*
   * If the opcode that threw was a member instruction, we have to decref tvRef
   * and tvRef2. AssertRAT* instructions can appear while these values are live
   * but they will never throw.
   */
  if (UNLIKELY(isMemberDimOp(throwOp) || isMemberFinalOp(throwOp))) {
    auto& mstate = vmMInstrState();
    tvDecRefGen(mstate.tvRef);
    tvWriteUninit(mstate.tvRef);
    tvDecRefGen(mstate.tvRef2);
    tvWriteUninit(mstate.tvRef2);
  }
}

/**
 * Discard the current frame, assuming that a PHP exception given in
 * phpException argument, or C++ exception (phpException == nullptr)
 * is being thrown. Returns an exception to propagate, or nulltpr
 * if the VM execution should be resumed.
 */
ObjectData* tearDownFrame(ActRec*& fp, Stack& stack, PC& pc,
                          ObjectData* phpException) {
  auto const func = fp->func();
  auto const prevFp = fp->sfp();
  auto const callOff = fp->callOffset();

  ITRACE(1, "tearDownFrame: {} ({})\n",
         func->fullName()->data(),
         func->unit()->filepath()->data());
  ITRACE(1, "  fp {} prevFp {}\n",
         implicit_cast<void*>(fp),
         implicit_cast<void*>(prevFp));

  auto const decRefLocals = [&] {
    /*
     * It is possible that locals have already been decref'd.
     *
     * Here's why:
     *
     *   - If a destructor for any of these things throws a php
     *     exception, it's swallowed at the dtor boundary and we keep
     *     running php.
     *
     *   - If the destructor for any of these things throws a fatal,
     *     it's swallowed, and we set surprise flags to throw a fatal
     *     from now on.
     *
     *   - If the second case happened and we have to run another
     *     destructor, its enter hook will throw, but it will be
     *     swallowed again.
     *
     *   - Finally, the exit hook for the returning function can
     *     throw, but this happens last so everything is destructed.
     *
     *   - When that happens, exit hook sets localsDecRefd flag.
     */
    if (!fp->localsDecRefd()) {
      fp->setLocalsDecRefd();
      try {
        frame_free_locals_unwind(fp, func->numLocals(), phpException);
      } catch (...) {}
    }
  };

  if (LIKELY(!isResumed(fp))) {
    decRefLocals();
    if (UNLIKELY(func->isAsyncFunction()) &&
        phpException &&
        (!fp->isAsyncEagerReturn() || func->isMemoizeImpl())) {
      // If in an eagerly executed async function without request for async
      // eager return, wrap the user exception into a failed StaticWaitHandle
      // and return it to the caller.
      auto const waitHandle = c_StaticWaitHandle::CreateFailed(phpException);
      phpException = nullptr;
      stack.ndiscard(func->numSlotsInFrame());
      stack.ret();
      assertx(stack.topTV() == fp->retSlot());
      tvCopy(make_tv<KindOfObject>(waitHandle), *fp->retSlot());
      fp->retSlot()->m_aux.u_asyncEagerReturnFlag = 0;
    } else {
      // We need to discard the NullUninits from inout on the stack but if the
      // function was called with the wrong arity (resulting in an excpetion),
      // then there may be missing inout arguments.
      auto const numInOut = [&] () -> uint32_t {
        if (!func->takesInOutParams()) return 0;
        uint32_t i = 0;
        for (int p = 0; p < fp->numArgs(); ++p) i += func->isInOut(p);
        return i;
      }();
      // Free ActRec.
      stack.ndiscard(func->numSlotsInFrame());
      stack.discardAR();

      // JIT may have optimized away NullUninit writes over the space reserved
      // for inout outputs.
      stack.ndiscard(numInOut);
    }
  } else if (func->isAsyncFunction()) {
    auto const waitHandle = frame_afwh(fp);
    if (phpException) {
      // Handle exception thrown by async function.
      decRefLocals();
      waitHandle->fail(phpException);
      decRefObj(waitHandle);
      phpException = nullptr;
    } else if (waitHandle->isRunning()) {
      // Let the C++ exception propagate. If the current frame represents async
      // function that is running, mark it as abruptly interrupted. Some opcodes
      // like Await may change state of the async function just before exit hook
      // decides to throw C++ exception.
      decRefLocals();
      waitHandle->failCpp();
      decRefObj(waitHandle);
    }
  } else if (func->isAsyncGenerator()) {
    auto const gen = frame_async_generator(fp);
    if (phpException) {
      // Handle exception thrown by async generator.
      decRefLocals();
      auto eagerResult = gen->fail(phpException);
      phpException = nullptr;
      if (eagerResult) {
        stack.pushObjectNoRc(eagerResult);
      }
    } else if (gen->isEagerlyExecuted() || gen->getWaitHandle()->isRunning()) {
      // Fail the async generator and let the C++ exception propagate.
      decRefLocals();
      gen->failCpp();
    }
  } else if (func->isNonAsyncGenerator()) {
    // Mark the generator as finished.
    decRefLocals();
    frame_generator(fp)->fail();
  } else {
    not_reached();
  }

  /*
   * At the final ActRec in this nesting level.
   */
  if (UNLIKELY(!prevFp)) {
    pc = nullptr;
    fp = nullptr;
    return phpException;
  }

  assertx(stack.isValidAddress(reinterpret_cast<uintptr_t>(prevFp)) ||
          isResumed(prevFp));
  pc = prevFp->func()->unit()->at(callOff + prevFp->func()->base());
  assertx(prevFp->func()->contains(pc));
  fp = prevFp;
  return phpException;
}

const StaticString s_previous("previous");
const Slot s_previousIdx{6};

DEBUG_ONLY bool is_throwable(ObjectData* throwable) {
  auto const erCls = SystemLib::s_ErrorClass;
  auto const exCls = SystemLib::s_ExceptionClass;
  return throwable->instanceof(erCls) || throwable->instanceof(exCls);
}

DEBUG_ONLY bool throwable_has_expected_props() {
  auto const erCls = SystemLib::s_ErrorClass;
  auto const exCls = SystemLib::s_ExceptionClass;
  if (erCls->lookupDeclProp(s_previous.get()) != s_previousIdx ||
      exCls->lookupDeclProp(s_previous.get()) != s_previousIdx) {
    return false;
  }

  // Check that we have the expected type-hints on these props so we don't need
  // to verify anything when setting. If someone changes the type-hint we want
  // to know.
  auto const isException = [&](const TypeConstraint& tc) {
    if (!tc.isObject()) return false;
    auto const cls = Unit::lookupClass(tc.namedEntity());
    return cls && cls == SystemLib::s_ExceptionClass;
  };

  return
    isException(erCls->declPropTypeConstraint(s_previousIdx)) &&
    isException(exCls->declPropTypeConstraint(s_previousIdx));
}

const StaticString s_hphpd_break("hphpd_break");

//////////////////////////////////////////////////////////////////////

}

Offset findCatchHandler(const Func* func, Offset raiseOffset) {
  auto const eh = func->findEH(raiseOffset);
  if (eh == nullptr) return kInvalidOffset;
  return eh->m_handler;
}

void chainFaultObjects(ObjectData* top, ObjectData* prev) {
  assertx(throwable_has_expected_props());

  // We don't chain the fault objects if there is a cycle in top, prev, or the
  // resulting chained fault object.
  std::unordered_set<uintptr_t> seen;

  // Walk head's previous pointers untill we find an unset one, or determine
  // they form a cycle.
  auto findAcyclicPrev = [&](ObjectData* head) {
    tv_lval foundLval;
    do {
      assertx(is_throwable(head));

      if (!seen.emplace((uintptr_t)head).second) return tv_lval();

      foundLval = head->propLvalAtOffset(s_previousIdx);
      assertx(foundLval.type() != KindOfUninit);
      head = foundLval.val().pobj;
    } while (foundLval.type() == KindOfObject &&
             foundLval.val().pobj->instanceof(SystemLib::s_ThrowableClass));
    return foundLval;
  };

  auto const prevLval = findAcyclicPrev(top);
  if (!prevLval || !findAcyclicPrev(prev)) {
    decRefObj(prev);
    return;
  }

  // Found an unset previous pointer, and result will not have a cycle so chain
  // the fault objects.
  tvMove(make_tv<KindOfObject>(prev), prevLval);
}

void lockObjectWhileUnwinding(PC pc, Stack& stack) {
  auto const op = decode_op(pc);
  if (LIKELY(op != OpFCallCtor)) return;
  auto fca = decodeFCallArgs(op, pc);
  if (!fca.lockWhileUnwinding) return;

  // We just unwound from a constructor that was called from a new expression
  // (as opposed to via e.g. parent::__construct()). The object being
  // constructed is on the top of the stack, and needs to be locked.
  auto const obj = stack.top();
  assertx(tvIsObject(obj));
  ITRACE(2, "Locking object {}\n", obj);
  obj->m_data.pobj->lockObject();
}

/*
 * Unwinding proceeds as follows:
 *
 *   - Discard all evaluation stack temporaries.
 *
 *   - Check if we are handling user exception in an eagerly executed
 *     async function. If so, pop its frame, wrap the exception into
 *     failed StaticWaitHandle object, leave it on the stack as
 *     a return value from the async function and resume VM.
 *
 *   - Failing any of the above, pop the frame for the current
 *     function.  If the current function was the last frame in the
 *     current VM nesting level, rethrow the exception, otherwise go
 *     to the first step and repeat this process in the caller's
 *     frame.
 *
 * If a non nullptr fpToUnwind is given, the unwinder will not unwind past
 * fpToUnwind, instead return when vmfp() is equal to fpToUnwind.
 *
 * The return value UnwinderResult indicates whether we ended unwinding due to
 * reaching fpToUnwind as well as whether we ended with putting a failed
 * static wait handle on the stack.
 */
UnwinderResult unwindVM(Either<ObjectData*, Exception*> exception,
                        const ActRec* fpToUnwind /* = nullptr */) {
  assertx(!exception.isNull());
  auto phpException = exception.left();
  if (phpException) phpException->incRefCount();

  auto& fp = vmfp();
  auto& stack = vmStack();
  auto& pc = vmpc();

  ITRACE(1, "entering unwinder for exception: {}\n", describeEx(exception));
  SCOPE_EXIT {
    ITRACE(1, "leaving unwinder for exception: {}\n", describeEx(exception));
  };

  discardMemberTVRefs(pc);

  while (true) {
    auto const func = fp->func();

    ITRACE(1, "unwind: func {}, raiseOffset {} fp {}\n",
           func->name()->data(),
           func->unit()->offsetOf(pc),
           implicit_cast<void*>(fp));

    discardStackTemps(fp, stack);

    // Note: we skip catch/finally clauses if we have a pending C++
    // exception as part of our efforts to avoid running more PHP
    // code in the face of such exceptions. Similarly, if the frame
    // has already been torn down (eg an exception thrown by a user
    // profiler on function exit), we can't execute any handlers in
    // *this* frame.
    if (RequestInfo::s_requestInfo->m_pendingException == nullptr &&
        phpException && !UNLIKELY(fp->localsDecRefd())) {

      const EHEnt* eh = func->findEH(func->unit()->offsetOf(pc));
      if (eh != nullptr) {
        // Found exception handler. Push the exception on top of the
        // stack and resume VM.
        ITRACE(1, "unwind: entering catch at {} func {} ({})\n",
               eh->m_handler,
               func->fullName()->data(),
               func->unit()->filepath()->data());

        vmStack().pushObjectNoRc(phpException);
        pc = func->unit()->at(eh->m_handler);
        DEBUGGER_ATTACHED_ONLY(phpDebuggerExceptionHandlerHook());
        return UnwindNone;
      }
    }

    // We found no more handlers in this frame.
    phpException = tearDownFrame(fp, stack, pc, phpException);

    // If we entered from the JIT and this is the last iteration, we can't
    // trust the PC since catch traces for inlined frames may add more
    // frames on vmfp()'s rbp chain which might have resulted in us incorrectly
    // calculating the PC.

    if (exception.left() != phpException) {
      assertx(phpException == nullptr);
      auto retCode = UnwindNone;
      if (fp) {
        if (!fpToUnwind) pc = skipCall(pc);
        retCode = UnwindSkipCall;
      }
      ITRACE(1, "Returning with exception == null\n");
      return retCode | UnwindFSWH;
    }

    if (!fp || (fpToUnwind && fp == fpToUnwind)) break;
    lockObjectWhileUnwinding(pc, stack);
  }

  if (fp) {
    assertx(fpToUnwind && phpException);
    ITRACE(1, "Reached {}\n", fpToUnwind);
    phpException->decRefCount();
    return UnwindReachedGoal;
  }

  ITRACE(1, "unwind: reached the end of this nesting's ActRec chain\n");
  if (exception.right()) {
    exception.right()->throwException();
  }
  assertx(phpException);
  throw_object(Object::attach(phpException));
}

//////////////////////////////////////////////////////////////////////

}
