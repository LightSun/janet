/*
* Copyright (c) 2017 Calvin Rose
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to
* deal in the Software without restriction, including without limitation the
* rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
* sell copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*/

#include "internal.h"

#define DST_LOCAL_FLAG_MUTABLE 1

/* Compiler typedefs */
typedef struct DstCompiler DstCompiler;
typedef struct FormOptions FormOptions;
typedef struct SlotTracker SlotTracker;
typedef struct DstScope DstScope;

/* Compilation state */
struct DstCompiler {
    Dst *vm;
    DstValue error;
    jmp_buf onError;
    DstScope *tail;
    DstBuffer *buffer;
    DstTable *env;
    int recursionGuard;
};

/* During compilation, FormOptions are passed to ASTs
 * as configuration options to allow for some optimizations. */
struct FormOptions {
    /* The location the returned Slot must be in. Can be ignored
     * if either canDrop or canChoose is true */
    uint16_t target;
    /* If the result of the value being compiled is not going to
     * be used, some forms can simply return a nil slot and save
     * co,putation */
    uint16_t resultUnused : 1;
    /* Allows the sub expression to evaluate into a
     * temporary slot of it's choice. A temporary Slot
     * can be allocated with DstCompilerGetLocal. */
    uint16_t canChoose : 1;
    /* True if the form is in the tail position. This allows
     * for tail call optimization. If a helper receives this
     * flag, it is free to return a returned slot and generate bytecode
     * for a return, including tail calls. */
    uint16_t isTail : 1;
};

/* A Slot represent a location of a local variable
 * on the stack. Also contains some meta information. */
typedef struct Slot Slot;
struct Slot {
    /* The index of the Slot on the stack. */
    uint16_t index;
    /* A nil Slot should not be expected to contain real data. (ignore index).
     * Forms that have side effects but don't evaulate to
     * anything will try to return nil slots. */
    uint16_t isNil : 1;
    /* A temp Slot is a Slot on the stack that does not
     * belong to a named local. They can be freed whenever,
     * and so are used in intermediate calculations. */
    uint16_t isTemp : 1;
    /* Flag indicating if byteCode for returning this slot
     * has been written to the buffer. Should only ever be true
     * when the isTail option is passed */
    uint16_t hasReturned : 1;
};

/* A SlotTracker provides a handy way to keep track of
 * Slots on the stack and free them in bulk. */
struct SlotTracker {
    Slot *slots;
    uint32_t count;
    uint32_t capacity;
};

/* A DstScope is a lexical scope in the program. It is
 * responsible for aliasing programmer facing names to
 * Slots and for keeping track of literals. It also
 * points to the parent DstScope, and its current child
 * DstScope. */
struct DstScope {
    uint32_t level;
    uint16_t nextLocal;
    uint16_t frameSize;
    uint32_t heapCapacity;
    uint32_t heapSize;
    uint16_t touchParent;
    uint16_t touchEnv;
    uint16_t *freeHeap;
    DstTable *literals;
    DstArray *literalsArray;
    DstTable *locals;
    DstScope *parent;
};

/* Provides default FormOptions */
static FormOptions form_options_default() {
    FormOptions opts;
    opts.canChoose = 1;
    opts.isTail = 0;
    opts.resultUnused = 0;
    opts.target = 0;
    return opts;
}

/* Create some helpers that allows us to push more than just raw bytes
 * to the byte buffer. This helps us create the byte code for the compiled
 * functions. */
BUFFER_DEFINE(i32, int32_t)
BUFFER_DEFINE(i64, int64_t)
BUFFER_DEFINE(real, DstReal)
BUFFER_DEFINE(u16, uint16_t)
BUFFER_DEFINE(i16, int16_t)

/* If there is an error during compilation,
 * jump back to start */
static void c_error(DstCompiler *c, const char *e) {
    c->error = dst_string_cv(c->vm, e);
    longjmp(c->onError, 1);
}

static void c_error1(DstCompiler *c, DstValue e) {
    c->error = e;
    longjmp(c->onError, 1);
}

/* Push a new scope in the compiler and return
 * a pointer to it for configuration. There is
 * more configuration that needs to be done if
 * the new scope is a function declaration. */
static DstScope *compiler_push_scope(DstCompiler *c, int sameFunction) {
    DstScope *scope = dst_alloc(c->vm, sizeof(DstScope));
    scope->locals = dst_table(c->vm, 4);
    scope->freeHeap = dst_alloc(c->vm, 4 * sizeof(uint16_t));
    scope->heapSize = 0;
    scope->heapCapacity = 4;
    scope->parent = c->tail;
    scope->frameSize = 0;
    scope->touchParent = 0;
    scope->touchEnv = 0;
    if (c->tail) {
        scope->level = c->tail->level + (sameFunction ? 0 : 1);
    } else {
        scope->level = 0;
    }
    if (sameFunction) {
        if (!c->tail) {
            c_error(c, "cannot inherit scope when root scope");
        }
        scope->nextLocal = c->tail->nextLocal;
        scope->literals = c->tail->literals;
        scope->literalsArray = c->tail->literalsArray;
    } else {
        scope->nextLocal = 0;
        scope->literals = dst_table(c->vm, 4);
        scope->literalsArray = dst_array(c->vm, 4);
    }
    c->tail = scope;
    return scope;
}

/* Remove the inner most scope from the compiler stack */
static void compiler_pop_scope(DstCompiler *c) {
    DstScope *last = c->tail;
    if (last == NULL) {
        c_error(c, "no scope to pop");
    } else {
        if (last->nextLocal > last->frameSize) {
            last->frameSize = last->nextLocal;
        }
        c->tail = last->parent;
        if (c->tail) {
            if (last->frameSize > c->tail->frameSize) {
                c->tail->frameSize = last->frameSize;
            }
        }
    }
}

/* Get the next stack position that is open for
 * a variable */
static uint16_t compiler_get_local(DstCompiler *c, DstScope *scope) {
    if (scope->heapSize == 0) {
        if (scope->nextLocal + 1 == 0) {
            c_error(c, "too many local variables");
        }
        return scope->nextLocal++;
    } else {
        return scope->freeHeap[--scope->heapSize];
    }
}

/* Free a slot on the stack for other locals and/or
 * intermediate values */
static void compiler_free_local(DstCompiler *c, DstScope *scope, uint16_t slot) {
    /* Ensure heap has space */
    if (scope->heapSize >= scope->heapCapacity) {
        uint32_t newCap = 2 * scope->heapSize;
        uint16_t *newData = dst_alloc(c->vm, newCap * sizeof(uint16_t));
        dst_memcpy(newData, scope->freeHeap, scope->heapSize * sizeof(uint16_t));
        scope->freeHeap = newData;
        scope->heapCapacity = newCap;
    }
    scope->freeHeap[scope->heapSize++] = slot;
}

/* Initializes a SlotTracker. SlotTrackers
 * are used during compilation to free up slots on the stack
 * after they are no longer needed. */
static void tracker_init(DstCompiler *c, SlotTracker *tracker) {
    tracker->slots = dst_alloc(c->vm, 10 * sizeof(Slot));
    tracker->count = 0;
    tracker->capacity = 10;
}

/* Free up a slot if it is a temporary slot (does not
 * belong to a named local). If the slot does belong
 * to a named variable, does nothing. */
static void compiler_drop_slot(DstCompiler *c, DstScope *scope, Slot slot) {
    if (!slot.isNil && slot.isTemp) {
        compiler_free_local(c, scope, slot.index);
    }
}

/* Helper function to return a slot. Useful for compiling things that return
 * nil. (set, while, etc.). Use this to wrap compilation calls that need
 * to return things. */
static Slot compiler_return(DstCompiler *c, Slot slot) {
    Slot ret;
    ret.hasReturned = 1;
    ret.isNil = 1;
    if (slot.hasReturned) {
        /* Do nothing */
    } else if (slot.isNil) {
        /* Return nil */
        dst_buffer_push_u16(c->vm, c->buffer, DST_OP_RTN);
    } else {
        /* Return normal value */
        dst_buffer_push_u16(c->vm, c->buffer, DST_OP_RET);
        dst_buffer_push_u16(c->vm, c->buffer, slot.index);
    }
    return ret;
}

/* Gets a temporary slot for the bottom-most scope. */
static Slot compiler_get_temp(DstCompiler *c) {
    DstScope *scope = c->tail;
    Slot ret;
    ret.isTemp = 1;
    ret.isNil = 0;
    ret.hasReturned = 0;
    ret.index = compiler_get_local(c, scope);
    return ret;
}

/* Return a slot that is the target Slot given some FormOptions. Will
 * Create a temporary slot if needed, so be sure to drop the slot after use. */
static Slot compiler_get_target(DstCompiler *c, FormOptions opts) {
    if (opts.canChoose) {
        return compiler_get_temp(c);
    } else {
        Slot ret;
        ret.isTemp = 0;
        ret.isNil = 0;
        ret.hasReturned = 0;
        ret.index = opts.target;
        return ret;
    }
}

/* If a slot is a nil slot, create a slot that has
 * an actual location on the stack. */
static Slot compiler_realize_slot(DstCompiler *c, Slot slot) {
    if (slot.isNil) {
        slot = compiler_get_temp(c);
        dst_buffer_push_u16(c->vm, c->buffer, DST_OP_NIL);
        dst_buffer_push_u16(c->vm, c->buffer, slot.index);
    }
    return slot;
}

/* Coerce a slot to match form options. Can write to buffer. */
static Slot compiler_coerce_slot(DstCompiler *c, FormOptions opts, Slot slot) {
    DstScope *scope = c->tail;
    if (opts.resultUnused) {
        compiler_drop_slot(c, scope, slot);
        slot.isNil = 1;
        return slot;
    } else {
        slot = compiler_realize_slot(c, slot);
    }
    if (opts.canChoose) {

    } else {
        if (slot.index != opts.target) {
             /* We need to move the variable. This
             * would occur in a simple assignment like a = b. */
            DstBuffer *buffer = c->buffer;
            dst_buffer_push_u16(c->vm, buffer, DST_OP_MOV);
            dst_buffer_push_u16(c->vm, buffer, opts.target);
            dst_buffer_push_u16(c->vm, buffer, slot.index);
            slot.index = opts.target;
            slot.isTemp = 0; /* We don't own the slot anymore */
        }
    }
    return slot;
}

/* Helper to get a nil slot */
static Slot nil_slot() { Slot ret; ret.isNil = 1; ret.hasReturned = 0; return ret; }

/* Writes all of the slots in the tracker to the compiler */
static void compiler_tracker_write(DstCompiler *c, SlotTracker *tracker, int reverse) {
    uint32_t i;
    DstBuffer *buffer = c->buffer;
    for (i = 0; i < tracker->count; ++i) {
        Slot s;
        if (reverse)
            s = tracker->slots[tracker->count - 1 - i];
        else
            s = tracker->slots[i];
        if (s.isNil)
            c_error(c, "trying to write nil slot");
        dst_buffer_push_u16(c->vm, buffer, s.index);
    }
}

/* Free the tracker after creation. This unlocks the memory
 * that was allocated by the GC an allows it to be collected. Also
 * frees slots that were tracked by this tracker in the given scope. */
static void compiler_tracker_free(DstCompiler *c, DstScope *scope, SlotTracker *tracker) {
    uint32_t i;
    /* Free in reverse order */
    for (i = tracker->count - 1; i < tracker->count; --i) {
        compiler_drop_slot(c, scope, tracker->slots[i]);
    }
}

/* Add a new Slot to a slot tracker. */
static void compiler_tracker_push(DstCompiler *c, SlotTracker *tracker, Slot slot) {
    if (tracker->count >= tracker->capacity) {
        uint32_t newCap = 2 * tracker->count;
        Slot *newData = dst_alloc(c->vm, newCap * sizeof(Slot));
        dst_memcpy(newData, tracker->slots, tracker->count * sizeof(Slot));
        tracker->slots = newData;
        tracker->capacity = newCap;
    }
    tracker->slots[tracker->count++] = slot;
}

/* Registers a literal in the given scope. If an equal literal is found, uses
 * that one instead of creating a new literal. This allows for some reuse
 * of things like string constants.*/
static uint16_t compiler_add_literal(DstCompiler *c, DstScope *scope, DstValue x) {
    DstValue checkDup = dst_table_get(scope->literals, x);
    uint16_t literalIndex = 0;
    if (checkDup.type != DST_NIL) {
        /* An equal literal is already registered in the current scope */
        return (uint16_t) checkDup.as.integer;
    } else {
        /* Add our literal for tracking */
        DstValue valIndex;
        valIndex.type = DST_INTEGER;
        literalIndex = scope->literalsArray->count;
        valIndex.as.integer = literalIndex;
        dst_table_put(c->vm, scope->literals, x, valIndex);
        dst_array_push(c->vm, scope->literalsArray, x);
    }
    return literalIndex;
}

/* Declare a symbol in a given scope. */
static uint16_t compiler_declare_symbol(DstCompiler *c, DstScope *scope, DstValue sym, uint16_t flags) {
    DstValue x;
    uint16_t target;
    if (sym.type != DST_SYMBOL)
        c_error(c, "expected symbol");
    target = compiler_get_local(c, scope);
    x.type = DST_INTEGER;
    x.as.integer = target + (flags << 16);
    dst_table_put(c->vm, scope->locals, sym, x);
    return target;
}

/* Try to resolve a symbol. If the symbol can be resolved, return true and
 * pass back the level and index by reference. */
static int symbol_resolve(DstCompiler *c, DstValue x, uint16_t *level, uint16_t *index, uint16_t *flags, DstValue *out) {
    DstScope *scope = c->tail;
    DstValue check;
    uint32_t currentLevel = scope->level;
    while (scope) {
        check = dst_table_get(scope->locals, x);
        if (check.type != DST_NIL) {
            *level = currentLevel - scope->level;
            *index = (uint16_t) (check.as.integer & 0xFFFF);
            if (flags) *flags = check.as.integer >> 16;
            return 1;
        }
        scope = scope->parent;
    }
    /* Check for named literals */
    check = dst_table_get(c->env, x);
    if (check.type != DST_NIL) {
        /* Check metadata for var (mutable) */
        DstTable *metas = dst_env_meta(c->vm, c->env);
        DstValue maybeMeta = dst_table_get(metas, x);
        if (maybeMeta.type == DST_TABLE) {
            DstValue isMutable = dst_table_get(maybeMeta.as.table, dst_string_cv(c->vm, "mutable"));
            if (dst_truthy(isMutable)) {
                if (flags) *flags = DST_LOCAL_FLAG_MUTABLE;
                *out = check;
                return 3;
            }
        }
        if (flags) *flags = 0;
        *out = check;
        return 2;
    }
    /* Check for nil named literal */
    check = dst_table_get(dst_env_nils(c->vm, c->env), x);
    if (check.type != DST_NIL) {
        if (flags) *flags = 0;
        *out = dst_wrap_nil();
        return 2;
    }
    return 0;
}

/* Forward declaration */
/* Compile a value and return it stack location after loading.
 * If a target > 0 is passed, the returned value must be equal
 * to the targtet. If target < 0, the DstCompiler can choose whatever
 * slot location it likes. If, for example, a symbol resolves to
 * whatever is in a given slot, it makes sense to use that location
 * to 'return' the value. For other expressions, like function
 * calls, the compiler will just pick the lowest free slot
 * as the location on the stack. */
static Slot compile_value(DstCompiler *c, FormOptions opts, DstValue x);

/* Compile boolean, nil, and number values. */
static Slot compile_nonref_type(DstCompiler *c, FormOptions opts, DstValue x) {
    DstBuffer *buffer = c->buffer;
    Slot ret;
    if (opts.resultUnused) return nil_slot();
    ret = compiler_get_target(c, opts);
    if (x.type == DST_NIL) {
        dst_buffer_push_u16(c->vm, buffer, DST_OP_NIL);
        dst_buffer_push_u16(c->vm, buffer, ret.index);
    } else if (x.type == DST_BOOLEAN) {
        dst_buffer_push_u16(c->vm, buffer, x.as.boolean ? DST_OP_TRU : DST_OP_FLS);
        dst_buffer_push_u16(c->vm, buffer, ret.index);
    } else if (x.type == DST_REAL) {
        dst_buffer_push_u16(c->vm, buffer, DST_OP_F64);
        dst_buffer_push_u16(c->vm, buffer, ret.index);
        dst_buffer_push_real(c->vm, buffer, x.as.real);
    } else if (x.type == DST_INTEGER) {
        if (x.as.integer <= 32767 && x.as.integer >= -32768) {
            dst_buffer_push_u16(c->vm, buffer, DST_OP_I16);
            dst_buffer_push_u16(c->vm, buffer, ret.index);
            dst_buffer_push_i16(c->vm, buffer, x.as.integer);
        } else if (x.as.integer <= 2147483647 && x.as.integer >= -2147483648) {
            dst_buffer_push_u16(c->vm, buffer, DST_OP_I32);
            dst_buffer_push_u16(c->vm, buffer, ret.index);
            dst_buffer_push_i32(c->vm, buffer, x.as.integer);
        } else {
            dst_buffer_push_u16(c->vm, buffer, DST_OP_I64);
            dst_buffer_push_u16(c->vm, buffer, ret.index);
            dst_buffer_push_i64(c->vm, buffer, x.as.integer);
        }
    } else {
        c_error(c, "expected boolean, nil, or number type");
    }
    return ret;
}

/* Compile a structure that evaluates to a literal value. Useful
 * for objects like strings, or anything else that cannot be instantiated
 * from bytecode and doesn't do anything in the AST. */
static Slot compile_literal(DstCompiler *c, FormOptions opts, DstValue x) {
    DstScope *scope = c->tail;
    DstBuffer *buffer = c->buffer;
    Slot ret;
    uint16_t literalIndex;
    if (opts.resultUnused) return nil_slot();
    switch (x.type) {
        case DST_INTEGER:
        case DST_REAL:
        case DST_BOOLEAN:
        case DST_NIL:
            return compile_nonref_type(c, opts, x);
        default:
            break;
    }
    ret = compiler_get_target(c, opts);
    literalIndex = compiler_add_literal(c, scope, x);
    dst_buffer_push_u16(c->vm, buffer, DST_OP_CST);
    dst_buffer_push_u16(c->vm, buffer, ret.index);
    dst_buffer_push_u16(c->vm, buffer, literalIndex);
    return ret;
}

/* Quote a value */
static DstValue quote(Dst *vm, DstValue x) {
    DstValue *tuple = dst_tuple_begin(vm, 2);
    tuple[0] = dst_string_cvs(vm, "quote");
    tuple[1] = x;
    return dst_wrap_tuple(dst_tuple_end(vm, tuple));
}

/* Compile a symbol. Resolves any kind of symbol. */
static Slot compile_symbol(DstCompiler *c, FormOptions opts, DstValue sym) {
    DstValue lit = dst_wrap_nil();
    DstBuffer * buffer = c->buffer;
    uint16_t index = 0;
    uint16_t level = 0;
    Slot ret;
    int status = symbol_resolve(c, sym, &level, &index, NULL, &lit);
    if (!status) {
        c_error1(c, sym);
    }
    if (opts.resultUnused) return nil_slot();
    if (status == 2) {
        /* We have a named literal */
        return compile_literal(c, opts, lit);
    } else if (status == 3) {
        /* We have a global variable */
        const DstValue *tup;
        Dst *vm= c->vm;
        DstValue *t = dst_tuple_begin(vm, 3);
        t[0] = dst_string_cvs(vm, "get"); /* Todo - replace with actual cfunc or bytecode */
        t[1] = quote(vm, lit);
        t[2] = dst_wrap_integer(0);
        tup = dst_tuple_end(vm, t);
        return compile_value(c, opts, dst_wrap_tuple(tup));
    } else if (level > 0) {
        /* We have an upvalue from a parent function. Make
         * sure that the chain of functions up to the upvalue keep
         * their parent references */
        uint32_t i = level;
        DstScope *scope = c->tail;
        for (i = level; i > 1; --i) {
            scope->touchParent = 1;
            scope = scope->parent;
        }
        scope->touchEnv = 1;
        ret = compiler_get_target(c, opts);
        dst_buffer_push_u16(c->vm, buffer, DST_OP_UPV);
        dst_buffer_push_u16(c->vm, buffer, ret.index);
        dst_buffer_push_u16(c->vm, buffer, level);
        dst_buffer_push_u16(c->vm, buffer, index);
    } else {
        /* Local variable on stack */
        ret.isTemp = 0;
        ret.isNil = 0;
        ret.hasReturned = 0;
        if (opts.canChoose) {
            ret.index = index;
        } else {
            /* We need to move the variable. This
             * would occur in a simple assignment like a = b. */
            ret.index = opts.target;
            dst_buffer_push_u16(c->vm, buffer, DST_OP_MOV);
            dst_buffer_push_u16(c->vm, buffer, ret.index);
            dst_buffer_push_u16(c->vm, buffer, index);
        }
    }
    return ret;
}

/* Compile an assignment operation */
static Slot compile_assign(DstCompiler *c, FormOptions opts, DstValue left, DstValue right) {
    DstValue lit = dst_wrap_nil();
    DstScope *scope = c->tail;
    DstBuffer *buffer = c->buffer;
    FormOptions subOpts = form_options_default();
    uint16_t target = 0;
    uint16_t level = 0;
    uint16_t flags = 0;
    Slot slot;
    int status;
    subOpts.isTail = 0;
    subOpts.resultUnused = 0;
    status = symbol_resolve(c, left, &level, &target, &flags, &lit);
    if (status == 1) {
        if (!(flags & DST_LOCAL_FLAG_MUTABLE))
            c_error(c, "cannot varset immutable binding");
        /* Check if we have an up value. Otherwise, it's just a normal
         * local variable */
        if (level != 0) {
            subOpts.canChoose = 1;
            /* Evaluate the right hand side */
            slot = compiler_realize_slot(c, compile_value(c, subOpts, right));
            /* Set the up value */
            dst_buffer_push_u16(c->vm, buffer, DST_OP_SUV);
            dst_buffer_push_u16(c->vm, buffer, slot.index);
            dst_buffer_push_u16(c->vm, buffer, level);
            dst_buffer_push_u16(c->vm, buffer, target);
        } else {
            /* Local variable */
            subOpts.canChoose = 0;
            subOpts.target = target;
            slot = compile_value(c, subOpts, right);
        }
    } else if (status == 3) {
        /* Global var */
        const DstValue *tup;
        Dst *vm= c->vm;
        DstValue *t = dst_tuple_begin(vm, 4);
        t[0] = dst_string_cvs(vm, "set!"); /* Todo - replace with ref ro actual cfunc */
        t[1] = quote(c->vm, lit);
        t[2] = dst_wrap_integer(0);
        t[3] = right;
        tup = dst_tuple_end(vm, t);
        subOpts.resultUnused = 1;
        compile_value(c, subOpts, dst_wrap_tuple(tup));
        return compile_value(c, opts, left);
    } else {
        c_error(c, "cannot varset immutable binding");
    }
    if (opts.resultUnused) {
        compiler_drop_slot(c, scope, slot);
        return nil_slot();
    } else {
        return slot;
    }
}

/* Set a var */
static Slot compile_varset(DstCompiler *c, FormOptions opts, const DstValue *form) {
    if (dst_tuple_length(form) != 3)
        c_error(c, "expected 2 arguments to varset");
    if (DST_SYMBOL != form[1].type)
        c_error(c, "expected symbol as first argument");
    return compile_assign(c, opts, form[1], form[2]);
}

/* Global var */
static Slot compile_global_var(DstCompiler *c, FormOptions opts, const DstValue *form) {
    const DstValue *tup;
    Dst *vm= c->vm;
    DstValue *t = dst_tuple_begin(vm, 3);
    t[0] = dst_string_cvs(vm, "global-var"); /* Todo - replace with ref ro actual cfunc */
    t[1] = form[1];
    t[1].type = DST_STRING;
    t[2] = form[2];
    tup = dst_tuple_end(vm, t);
    return compile_value(c, opts, dst_wrap_tuple(tup));
}

/* Global define */
static Slot compile_global_def(DstCompiler *c, FormOptions opts, const DstValue *form) {
    const DstValue *tup;
    Dst *vm= c->vm;
    DstValue *t = dst_tuple_begin(vm, 3);
    t[0] = dst_string_cvs(vm, "global-def"); /* Todo - replace with ref ro actual cfunc */
    t[1] = form[1];
    t[1].type = DST_STRING;
    t[2] = form[2];
    tup = dst_tuple_end(vm, t);
    return compile_value(c, opts, dst_wrap_tuple(tup));
}

/* Compile def */
static Slot compile_def(DstCompiler *c, FormOptions opts, const DstValue *form) {
    DstScope *scope = c->tail;
    if (dst_tuple_length(form) != 3)
        c_error(c, "expected 2 arguments to def");
    if (DST_SYMBOL != form[1].type)
        c_error(c, "expected symbol as first argument");
    if (scope->parent) {
        FormOptions subOpts;
        Slot slot;
        subOpts.isTail = opts.isTail;
        subOpts.resultUnused = 0;
        subOpts.canChoose = 0;
        subOpts.target = compiler_declare_symbol(c, scope, form[1], 0);
        slot = compile_value(c, subOpts, form[2]);
        return compiler_coerce_slot(c, opts, slot);
    } else {
        return compile_global_def(c, opts, form);
    }
}

/* Compile var */
static Slot compile_var(DstCompiler *c, FormOptions opts, const DstValue *form) {
    DstScope *scope = c->tail;
    if (dst_tuple_length(form) != 3)
        c_error(c, "expected 2 arguments to var");
    if (DST_SYMBOL != form[1].type)
        c_error(c, "expected symbol as first argument");
    if (scope->parent) {
        FormOptions subOpts;
        Slot slot;
        subOpts.isTail = opts.isTail;
        subOpts.resultUnused = 0;
        subOpts.canChoose = 0;
        subOpts.target = compiler_declare_symbol(c, scope, form[1], DST_LOCAL_FLAG_MUTABLE);
        slot = compile_value(c, subOpts, form[2]);
        return compiler_coerce_slot(c, opts, slot);
    } else {
        return compile_global_var(c, opts, form);
    }
}

/* Compile series of expressions. This compiles the meat of
 * function definitions and the inside of do forms. */
static Slot compile_block(DstCompiler *c, FormOptions opts, const DstValue *form, uint32_t startIndex) {
    DstScope *scope = c->tail;
    FormOptions subOpts = form_options_default();
    uint32_t current = startIndex;
    /* Check for empty body */
    if (dst_tuple_length(form) <= startIndex) return nil_slot();
    /* Compile the body */
    subOpts.resultUnused = 1;
    subOpts.isTail = 0;
    subOpts.canChoose = 1;
    while (current < dst_tuple_length(form) - 1) {
        compiler_drop_slot(c, scope, compile_value(c, subOpts, form[current]));
        ++current;
    }
    /* Compile the last expression in the body */
    return compile_value(c, opts, form[dst_tuple_length(form) - 1]);
}

/* Extract the last n bytes from the buffer and use them to construct
 * a function definition. */
static DstFuncDef *compiler_gen_funcdef(DstCompiler *c, uint32_t lastNBytes, uint32_t arity, int varargs) {
    DstScope *scope = c->tail;
    DstBuffer *buffer = c->buffer;
    DstFuncDef *def = dst_alloc(c->vm, sizeof(DstFuncDef));
    /* Create enough space for the new byteCode */
    if (lastNBytes > buffer->count)
        c_error(c, "trying to extract more bytes from buffer than in buffer");
    uint8_t * byteCode = dst_alloc(c->vm, lastNBytes);
    def->byteCode = (uint16_t *)byteCode;
    def->byteCodeLen = lastNBytes / 2;
    /* Copy the last chunk of bytes in the buffer into the new
     * memory for the function's byteCOde */
    dst_memcpy(byteCode, buffer->data + buffer->count - lastNBytes, lastNBytes);
    /* Remove the byteCode from the end of the buffer */
    buffer->count -= lastNBytes;
    /* Create the literals used by this function */
    if (scope->literalsArray->count) {
        def->literals = dst_alloc(c->vm, scope->literalsArray->count * sizeof(DstValue));
        dst_memcpy(def->literals, scope->literalsArray->data,
                scope->literalsArray->count * sizeof(DstValue));
    } else {
        def->literals = NULL;
    }
    def->literalsLen = scope->literalsArray->count;
    /* Delete the sub scope */
    compiler_pop_scope(c);
    /* Initialize the new FuncDef */
    def->locals = scope->frameSize;
    def->arity = arity;
    def->flags = (varargs ? DST_FUNCDEF_FLAG_VARARG : 0) |
        (scope->touchParent ? DST_FUNCDEF_FLAG_NEEDSPARENT : 0) |
        (scope->touchEnv ? DST_FUNCDEF_FLAG_NEEDSENV : 0);
    return def;
}

/* Check if a string a cstring are equal */
static int equal_cstr(const uint8_t *str, const char *cstr) {
    uint32_t i;
    for (i = 0; i < dst_string_length(str); ++i) {
        if (cstr[i] == 0) return 0;
        if (str[i] != ((const uint8_t *)cstr)[i]) return 0;
    }
    return cstr[i] == 0;
}

/* Compile a function from a function literal source form */
static Slot compile_function(DstCompiler *c, FormOptions opts, const DstValue *form) {
    DstScope *scope = c->tail;
    DstBuffer *buffer = c->buffer;
    uint32_t current = 1;
    uint32_t i;
    uint32_t sizeBefore; /* Size of buffer before compiling function */
    DstScope *subDstScope;
    DstArray *params;
    FormOptions subOpts = form_options_default();
    Slot ret;
    int varargs = 0;
    uint32_t arity;
    if (opts.resultUnused) return nil_slot();
    ret = compiler_get_target(c, opts);
    subDstScope = compiler_push_scope(c, 0);
    /* Define the function parameters */
    if (form[current].type != DST_ARRAY)
        c_error(c, "expected function arguments array");
    params = form[current++].as.array;
    arity = params->count;
    for (i = 0; i < params->count; ++i) {
        DstValue param = params->data[i];
        if (param.type != DST_SYMBOL)
            c_error(c, "function parameters should be symbols");
        /* Check for varargs */
        if (equal_cstr(param.as.string, "&")) {
            if (i != params->count - 1) {
                c_error(c, "& is reserved for vararg argument in function");
            }
            varargs = 1;
            arity--;
        }
        /* The compiler puts the parameter locals
         * in the right place by default - at the beginning
         * of the stack frame. */
        compiler_declare_symbol(c, subDstScope, param, 0);
    }
    /* Mark where we are on the stack so we can
     * return to it later. */
    sizeBefore = buffer->count;
    /* Compile the body in the subscope */
    subOpts.isTail = 1;
    compiler_return(c, compile_block(c, subOpts, form, current));
    /* Create a new FuncDef as a constant in original scope by splicing
     * out the relevant code from the buffer. */
    {
        DstValue newVal;
        uint16_t literalIndex;
        DstFuncDef *def = compiler_gen_funcdef(c, buffer->count - sizeBefore, arity, varargs);
        /* Add this FuncDef as a literal in the outer scope */
        newVal.type = DST_FUNCDEF;
        newVal.as.def = def;
        literalIndex = compiler_add_literal(c, scope, newVal);
        dst_buffer_push_u16(c->vm, buffer, DST_OP_CLN);
        dst_buffer_push_u16(c->vm, buffer, ret.index);
        dst_buffer_push_u16(c->vm, buffer, literalIndex);
    }
    return ret;
}

/* Branching special */
static Slot compile_if(DstCompiler *c, FormOptions opts, const DstValue *form) {
    DstScope *scope = c->tail;
    DstBuffer *buffer = c->buffer;
    FormOptions condOpts = opts;
    FormOptions branchOpts = opts;
    Slot left, right, condition;
    uint32_t countAtJumpIf = 0;
    uint32_t countAtJump = 0;
    uint32_t countAfterFirstBranch = 0;
    /* Check argument count */
    if (dst_tuple_length(form) < 3 || dst_tuple_length(form) > 4)
        c_error(c, "if takes either 2 or 3 arguments");
    /* Compile the condition */
    condOpts.isTail = 0;
    condOpts.resultUnused = 0;
    condition = compile_value(c, condOpts, form[1]);
    /* If the condition is nil, just compile false path */
    if (condition.isNil) {
        if (dst_tuple_length(form) == 4) {
            return compile_value(c, opts, form[3]);
        }
        return condition;
    }
    /* Mark where the buffer is now so we can write the jump
     * length later */
    countAtJumpIf = buffer->count;
    buffer->count += sizeof(int32_t) + 2 * sizeof(uint16_t);
    /* Configure branch form options */
    branchOpts.canChoose = 0;
    branchOpts.target = condition.index;
    /* Compile true path */
    left = compile_value(c, branchOpts, form[2]);
    if (opts.isTail) {
        compiler_return(c, left);
    } else {
        /* If we need to jump again, do so */
        if (dst_tuple_length(form) == 4) {
            countAtJump = buffer->count;
            buffer->count += sizeof(int32_t) + sizeof(uint16_t);
        }
    }
    compiler_drop_slot(c, scope, left);
    /* Reinsert jump with correct index */
    countAfterFirstBranch = buffer->count;
    buffer->count = countAtJumpIf;
    dst_buffer_push_u16(c->vm, buffer, DST_OP_JIF);
    dst_buffer_push_u16(c->vm, buffer, condition.index);
    dst_buffer_push_i32(c->vm, buffer, (countAfterFirstBranch - countAtJumpIf) / 2);
    buffer->count = countAfterFirstBranch;
    /* Compile false path */
    if (dst_tuple_length(form) == 4) {
        right = compile_value(c, branchOpts, form[3]);
        if (opts.isTail) compiler_return(c, right);
        compiler_drop_slot(c, scope, right);
    } else if (opts.isTail) {
        compiler_return(c, condition);
    }
    /* Reset the second jump length */
    if (!opts.isTail && dst_tuple_length(form) == 4) {
        countAfterFirstBranch = buffer->count;
        buffer->count = countAtJump;
        dst_buffer_push_u16(c->vm, buffer, DST_OP_JMP);
        dst_buffer_push_i32(c->vm, buffer, (countAfterFirstBranch - countAtJump) / 2);
        buffer->count = countAfterFirstBranch;
    }
    if (opts.isTail)
        condition.hasReturned = 1;
    return condition;
}

/* While special */
static Slot compile_while(DstCompiler *c, FormOptions opts, const DstValue *form) {
    Slot cond;
    uint32_t countAtStart = c->buffer->count;
    uint32_t countAtJumpDelta;
    uint32_t countAtFinish;
    FormOptions defaultOpts = form_options_default();
    compiler_push_scope(c, 1);
    /* Compile condition */
    cond = compile_value(c, defaultOpts, form[1]);
    /* Assert that cond is a real value - otherwise do nothing (nil is false,
     * so loop never runs.) */
    if (cond.isNil) return cond;
    /* Leave space for jump later */
    countAtJumpDelta = c->buffer->count;
    c->buffer->count += sizeof(uint16_t) * 2 + sizeof(int32_t);
    /* Compile loop body */
    defaultOpts.resultUnused = 1;
    compiler_drop_slot(c, c->tail, compile_block(c, defaultOpts, form, 2));
    /* Jump back to the loop start */
    countAtFinish = c->buffer->count;
    dst_buffer_push_u16(c->vm, c->buffer, DST_OP_JMP);
    dst_buffer_push_i32(c->vm, c->buffer, (int32_t)(countAtFinish - countAtStart) / -2);
    countAtFinish = c->buffer->count;
    /* Set the jump to the correct length */
    c->buffer->count = countAtJumpDelta;
    dst_buffer_push_u16(c->vm, c->buffer, DST_OP_JIF);
    dst_buffer_push_u16(c->vm, c->buffer, cond.index);
    dst_buffer_push_i32(c->vm, c->buffer, (int32_t)(countAtFinish - countAtJumpDelta) / 2);
    /* Pop scope */
    c->buffer->count = countAtFinish;
    compiler_pop_scope(c);
    /* Return nil */
    if (opts.resultUnused)
        return nil_slot();
    else
        return cond;
}

/* Do special */
static Slot compile_do(DstCompiler *c, FormOptions opts, const DstValue *form) {
    Slot ret;
    compiler_push_scope(c, 1);
    ret = compile_block(c, opts, form, 1);
    compiler_pop_scope(c);
    return ret;
}

/* Quote special - returns its argument as is. */
static Slot compile_quote(DstCompiler *c, FormOptions opts, const DstValue *form) {
    DstScope *scope = c->tail;
    DstBuffer *buffer = c->buffer;
    Slot ret;
    uint16_t literalIndex;
    if (dst_tuple_length(form) != 2)
        c_error(c, "quote takes exactly 1 argument");
    DstValue x = form[1];
    if (x.type == DST_NIL ||
            x.type == DST_BOOLEAN ||
            x.type == DST_REAL ||
            x.type == DST_INTEGER) {
        return compile_nonref_type(c, opts, x);
    }
    if (opts.resultUnused) return nil_slot();
    ret = compiler_get_target(c, opts);
    literalIndex = compiler_add_literal(c, scope, x);
    dst_buffer_push_u16(c->vm, buffer, DST_OP_CST);
    dst_buffer_push_u16(c->vm, buffer, ret.index);
    dst_buffer_push_u16(c->vm, buffer, literalIndex);
    return ret;
}

/* Apply special */
static Slot compile_apply(DstCompiler *c, FormOptions opts, const DstValue *form) {
    DstScope *scope = c->tail;
    DstBuffer *buffer = c->buffer;
    /* Empty forms evaluate to nil. */
    if (dst_tuple_length(form) < 3)
        c_error(c, "apply expects at least 2 arguments");
    {
        Slot ret, callee;
        SlotTracker tracker;
        FormOptions subOpts = form_options_default();
        uint32_t i;
        tracker_init(c, &tracker);
        /* Compile function to be called */
        callee = compiler_realize_slot(c, compile_value(c, subOpts, form[1]));
        /* Compile all of the arguments */
        for (i = 2; i < dst_tuple_length(form) - 1; ++i) {
            Slot slot = compile_value(c, subOpts, form[i]);
            compiler_tracker_push(c, &tracker, slot);
        }
        /* Write last item */
        {
            Slot slot = compile_value(c, subOpts, form[dst_tuple_length(form) - 1]);
            slot = compiler_realize_slot(c, slot);
            /* Free up some slots */
            compiler_drop_slot(c, scope, callee);
            compiler_drop_slot(c, scope, slot);
            compiler_tracker_free(c, scope, &tracker);
            /* Write first arguments */
            dst_buffer_push_u16(c->vm, buffer, DST_OP_PSK);
            dst_buffer_push_u16(c->vm, buffer, tracker.count);
            /* Write the location of all of the arguments */
            compiler_tracker_write(c, &tracker, 0);
            /* Write last arguments */
            dst_buffer_push_u16(c->vm, buffer, DST_OP_PAR);
            dst_buffer_push_u16(c->vm, buffer, slot.index);
        }
        /* If this is in tail position do a tail call. */
        if (opts.isTail) {
            dst_buffer_push_u16(c->vm, buffer, DST_OP_TCL);
            dst_buffer_push_u16(c->vm, buffer, callee.index);
            ret.hasReturned = 1;
            ret.isNil = 1;
        } else {
            ret = compiler_get_target(c, opts);
            dst_buffer_push_u16(c->vm, buffer, DST_OP_CAL);
            dst_buffer_push_u16(c->vm, buffer, callee.index);
            dst_buffer_push_u16(c->vm, buffer, ret.index);
        }
        return ret;
    }
}

/* Transfer special */
static Slot compile_tran(DstCompiler *c, FormOptions opts, const DstValue *form) {
    DstBuffer *buffer = c->buffer;
    Slot t, v, r;
    if (dst_tuple_length(form) != 3 && dst_tuple_length(form) != 2)
        c_error(c, "tran expects 2 or 3 arguments");
    t = compiler_realize_slot(c, compile_value(c, form_options_default(), form[1]));
    if (dst_tuple_length(form) == 3)
        v = compiler_realize_slot(c, compile_value(c, form_options_default(), form[2]));
    else
        v = compile_value(c, form_options_default(), dst_wrap_nil());
    r = compiler_get_target(c, opts);
    dst_buffer_push_u16(c->vm, buffer, DST_OP_TRN);
    dst_buffer_push_u16(c->vm, buffer, r.index);
    dst_buffer_push_u16(c->vm, buffer, t.index);
    dst_buffer_push_u16(c->vm, buffer, v.index);
    return r;
}

/* Define a function type for Special Form helpers */
typedef Slot (*SpecialFormHelper) (DstCompiler *c, FormOptions opts, const DstValue *form);

/* Dispatch to a special form */
static SpecialFormHelper get_special(const DstValue *form) {
    const uint8_t *name;
    if (dst_tuple_length(form) < 1 || form[0].type != DST_SYMBOL)
        return NULL;
    name = form[0].as.string;
    /* If we have a symbol with a zero length name, we have other
     * problems. */
    if (dst_string_length(name) == 0)
        return NULL;
    /* Specials */
    switch (name[0]) {
        case 'a':
            {
                if (dst_string_length(name) == 5 &&
                        name[1] == 'p' &&
                        name[2] == 'p' &&
                        name[3] == 'l' &&
                        name[4] == 'y') {
                    return compile_apply;
                }
            }
            break;
        case 'd':
            {
                if (dst_string_length(name) == 2 &&
                        name[1] == 'o') {
                    return compile_do;
                } else if (dst_string_length(name) == 3 &&
                        name[1] == 'e' &&
                        name[2] == 'f') {
                    return compile_def;
                }
            }
            break;
        case 'i':
            {
                if (dst_string_length(name) == 2 &&
                        name[1] == 'f') {
                    return compile_if;
                }
            }
            break;
        case 'f':
            {
                if (dst_string_length(name) == 2 &&
                        name[1] == 'n') {
                    return compile_function;
                }
            }
            break;
        case 'q':
            {
                if (dst_string_length(name) == 5 &&
                        name[1] == 'u' &&
                        name[2] == 'o' &&
                        name[3] == 't' &&
                        name[4] == 'e') {
                    return compile_quote;
                }
            }
            break;
        case 't':
            {
                if (dst_string_length(name) == 4 &&
                        name[1] == 'r' &&
                        name[2] == 'a' &&
                        name[3] == 'n') {
                    return compile_tran;
                }
            }
            break;
        case 'v':
            {
                if (dst_string_length(name) == 3 &&
                        name[1] == 'a' &&
                        name[2] == 'r') {
                    return compile_var;
                }
                if (dst_string_length(name) == 7 &&
                        name[1] == 'a' &&
                        name[2] == 'r' &&
                        name[3] == 's' &&
                        name[4] == 'e' &&
                        name[5] == 't' &&
                        name[6] == '!') {
                    return compile_varset;
                }
            }
            break;
        case 'w':
            {
                if (dst_string_length(name) == 5 &&
                        name[1] == 'h' &&
                        name[2] == 'i' &&
                        name[3] == 'l' &&
                        name[4] == 'e') {
                    return compile_while;
                }
            }
            break;
        default:
            break;
    }
    return NULL;
}

/* Compile an array */
static Slot compile_array(DstCompiler *c, FormOptions opts, DstArray *array) {
    DstScope *scope = c->tail;
    FormOptions subOpts = form_options_default();
    DstBuffer *buffer = c->buffer;
    Slot ret;
    SlotTracker tracker;
    uint32_t i, count;
    count = array->count;
    ret = compiler_get_target(c, opts);
    tracker_init(c, &tracker);
    for (i = 0; i < count; ++i) {
        Slot slot = compile_value(c, subOpts, array->data[i]);
        compiler_tracker_push(c, &tracker, compiler_realize_slot(c, slot));
    }
    compiler_tracker_free(c, scope, &tracker);
    dst_buffer_push_u16(c->vm, buffer, DST_OP_ARR);
    dst_buffer_push_u16(c->vm, buffer, ret.index);
    dst_buffer_push_u16(c->vm, buffer, count);
    compiler_tracker_write(c, &tracker, 0);
    return ret;
}

/* Compile an object literal */
static Slot compile_table(DstCompiler *c, FormOptions opts, DstTable *tab) {
    DstScope *scope = c->tail;
    FormOptions subOpts = form_options_default();
    DstBuffer *buffer = c->buffer;
    Slot ret;
    SlotTracker tracker;
    uint32_t i, cap;
    cap = tab->capacity;
    ret = compiler_get_target(c, opts);
    tracker_init(c, &tracker);
    for (i = 0; i < cap; i += 2) {
        if (tab->data[i].type != DST_NIL) {
            Slot slot = compile_value(c, subOpts, tab->data[i]);
            compiler_tracker_push(c, &tracker, compiler_realize_slot(c, slot));
            slot = compile_value(c, subOpts, tab->data[i + 1]);
            compiler_tracker_push(c, &tracker, compiler_realize_slot(c, slot));
        }
    }
    compiler_tracker_free(c, scope, &tracker);
    dst_buffer_push_u16(c->vm, buffer, DST_OP_DIC);
    dst_buffer_push_u16(c->vm, buffer, ret.index);
    dst_buffer_push_u16(c->vm, buffer, tab->count * 2);
    compiler_tracker_write(c, &tracker, 0);
    return ret;
}

/* Compile a form. Checks for special forms. */
static Slot compile_form(DstCompiler *c, FormOptions opts, const DstValue *form) {
    DstScope *scope = c->tail;
    DstBuffer *buffer = c->buffer;
    SpecialFormHelper helper;
    /* Empty forms evaluate to nil. */
    if (dst_tuple_length(form) == 0) {
        DstValue temp;
        temp.type = DST_NIL;
        return compile_nonref_type(c, opts, temp);
    }
    /* Check and handle special forms */
    helper = get_special(form);
    if (helper != NULL) {
        return helper(c, opts, form);
    } else {
        Slot ret, callee;
        SlotTracker tracker;
        FormOptions subOpts = form_options_default();
        uint32_t i;
        tracker_init(c, &tracker);
        /* Compile function to be called */
        callee = compiler_realize_slot(c, compile_value(c, subOpts, form[0]));
        /* Compile all of the arguments */
        for (i = 1; i < dst_tuple_length(form); ++i) {
            Slot slot = compile_value(c, subOpts, form[i]);
            compiler_tracker_push(c, &tracker, slot);
        }
        /* Free up some slots */
        compiler_drop_slot(c, scope, callee);
        compiler_tracker_free(c, scope, &tracker);
        /* Prepare next stack frame */
        dst_buffer_push_u16(c->vm, buffer, DST_OP_PSK);
        dst_buffer_push_u16(c->vm, buffer, dst_tuple_length(form) - 1);
        /* Write the location of all of the arguments */
        compiler_tracker_write(c, &tracker, 0);
        /* If this is in tail position do a tail call. */
        if (opts.isTail) {
            dst_buffer_push_u16(c->vm, buffer, DST_OP_TCL);
            dst_buffer_push_u16(c->vm, buffer, callee.index);
            ret.hasReturned = 1;
            ret.isNil = 1;
        } else {
            ret = compiler_get_target(c, opts);
            dst_buffer_push_u16(c->vm, buffer, DST_OP_CAL);
            dst_buffer_push_u16(c->vm, buffer, callee.index);
            dst_buffer_push_u16(c->vm, buffer, ret.index);
        }
        return ret;
    }
}

/* Recursively compile any value or form */
static Slot compile_value(DstCompiler *c, FormOptions opts, DstValue x) {
    Slot ret;
    /* Check if recursion is too deep */
    if (--c->recursionGuard == 0) {
        c_error(c, "recursed too deeply");
    }
    switch (x.type) {
        case DST_NIL:
        case DST_BOOLEAN:
        case DST_REAL:
        case DST_INTEGER:
            ret = compile_nonref_type(c, opts, x);
            break;
        case DST_SYMBOL:
            ret = compile_symbol(c, opts, x);
            break;
        case DST_TUPLE:
            ret = compile_form(c, opts, x.as.tuple);
            break;
        case DST_ARRAY:
            ret = compile_array(c, opts, x.as.array);
            break;
        case DST_TABLE:
            ret = compile_table(c, opts, x.as.table);
            break;
        default:
            ret = compile_literal(c, opts, x);
            break;
    }
    c->recursionGuard++;
    return ret;
}

/* Compile interface. Returns a function that evaluates the
 * given AST. Returns NULL if there was an error during compilation. */
DstValue dst_compile(Dst *vm, DstTable *env, DstValue form) {
    DstCompiler c;
    FormOptions opts = form_options_default();
    DstFuncDef *def;
    if (setjmp(c.onError)) {
        if (c.error.type == DST_NIL)
            return dst_string_cv(vm, "unknown error");
        return c.error;
    }
    c.error.type = DST_NIL;
    c.env = env;
    c.vm = vm;
    c.tail = NULL;
    c.buffer = dst_buffer(vm, 24);
    c.recursionGuard = DST_RECURSION_GUARD;
    compiler_push_scope(&c, 0);
    opts.isTail = 1;
    compiler_return(&c, compile_value(&c, opts, form));
    def = compiler_gen_funcdef(&c, c.buffer->count, 0, 0);
    {
        DstFuncEnv *env = dst_alloc(vm, sizeof(DstFuncEnv));
        DstFunction *func = dst_alloc(vm, sizeof(DstFunction));
        env->values = NULL;
        env->stackOffset = 0;
        env->thread = NULL;
        func->parent = NULL;
        func->def = def;
        func->env = env;
        return dst_wrap_function(func);
    }
}
