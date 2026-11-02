// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#include "lgc.h"

#include "lobject.h"
#include "lstate.h"
#include "ltable.h"
#include "lfunc.h"
#include "lstring.h"
#include "ldo.h"
#include "lmem.h"
#include "ludata.h"
#include "lbuffer.h"
#include "lclass.h"
#include "lvector.h"

#include <string.h>

#include <cstddef>

LUAU_FASTFLAGVARIABLE(LuauGcTraceUdata)
LUAU_FLAGVERSION(LuauGcTraceUdata, 4)
LUAU_FASTFLAG(LuauBackedgeHeapCheck)
LUAU_FASTFLAG(LuauFastpcall)
LUAU_FASTFLAG(LuauCoroutineFinally)
LUAU_FASTFLAG(LuauFrozenMetaButterfly)
LUAU_DYNAMIC_FASTFLAGVARIABLE(LuauGcHeapShrinkFix, false)

/*
 * Luau uses an incremental non-generational non-moving mark&sweep garbage collector.
 *
 * The collector runs in three stages: mark, atomic and sweep. Mark and sweep are incremental and try to do a limited amount
 * of work every GC step; atomic is ran once per the GC cycle and is indivisible. In either case, the work happens during GC
 * steps that are "scheduled" by the GC pacing algorithm - the steps happen either from explicit calls to lua_gc, or after
 * the mutator (aka application) allocates some amount of memory, which is known as "GC assist". In either case, GC steps
 * can't happen concurrently with other access to VM state.
 *
 * Current GC stage is stored in global_State::gcstate, and has two additional stages for pause and second-phase mark, explained below.
 *
 * GC pacer is an algorithm that tries to ensure that GC can always catch up to the application allocating garbage, but do this
 * with minimal amount of effort. To configure the pacer Luau provides control over three variables: GC goal, defined as the
 * target heap size during atomic phase in relation to live heap size (e.g. 200% goal means the heap's worst case size is double
 * the total size of alive objects), step size (how many kilobytes should the application allocate for GC step to trigger), and
 * GC multiplier (how much should the GC try to mark relative to how much the application allocated). It's critical that step
 * multiplier is significantly above 1, as this is what allows the GC to catch up to the application's allocation rate, and
 * GC goal and GC multiplier are linked in subtle ways, described in lua.h comments for LUA_GCSETGOAL.
 *
 * During mark, GC tries to identify all reachable objects and mark them as reachable, while keeping unreachable objects unmarked.
 * During sweep, GC tries to sweep all objects that were not reachable at the end of mark. The atomic phase is needed to ensure
 * that all pending marking has completed and all objects that are still marked as unreachable are, in fact, unreachable.
 *
 * Notably, during mark GC doesn't free any objects, and so the heap size constantly grows; during sweep, GC doesn't do any marking
 * work, so it can't immediately free objects that became unreachable after sweeping started.
 *
 * Every collectable object has one of three colors at any given point in time: white, gray or black. This coloring scheme
 * is necessary to implement incremental marking: white objects have not been marked and may be unreachable, black objects
 * have been marked and will not be marked again if they stay black, and gray objects have been marked but may contain unmarked
 * references.
 *
 * Objects are allocated as white; however, during sweep, we need to differentiate between objects that remained white in the mark
 * phase (these are not reachable and can be freed) and objects that were allocated after the mark phase ended. Because of this, the
 * colors are encoded using three bits inside GCheader::marked: white0, white1 and black (so technically we use a four-color scheme:
 * any object can be white0, white1, gray or black). All bits are exclusive, and gray objects have all three bits unset. This allows
 * us to have the "current" white bit, which is flipped during atomic stage - during sweeping, objects that have the white color from
 * the previous mark may be deleted, and all other objects may or may not be reachable, and will be changed to the current white color,
 * so that the next mark can start coloring objects from scratch again.
 *
 * Crucially, the coloring scheme comes with what's known as a tri-color invariant: a black object may never point to a white object.
 *
 * At the end of atomic stage, the expectation is that there are no gray objects anymore, which means all objects are either black
 * (reachable) or white (unreachable = dead). Tri-color invariant is maintained throughout mark and atomic phase. To uphold this
 * invariant, every modification of an object needs to check if the object is black and the new referent is white; if so, we
 * need to either mark the referent, making it non-white (known as a forward barrier), or mark the object as gray and queue it
 * for additional marking (known as a backward barrier).
 *
 * Luau uses both types of barriers. Forward barriers advance GC progress, since they don't create new outstanding work for GC,
 * but they may be expensive when an object is modified many times in succession. Backward barriers are cheaper, as they defer
 * most of the work until "later", but they require queueing the object for a rescan which isn't always possible. Table writes usually
 * use backward barriers (but switch to forward barriers during second-phase mark), whereas upvalue writes and setmetatable use forward
 * barriers.
 *
 * Since marking is incremental, it needs a way to track progress, which is implemented as a gray set: at any point, objects that
 * are gray need to mark their white references, objects that are black have no pending work, and objects that are white have not yet
 * been reached. Once the gray set is empty, the work completes; as such, incremental marking is as simple as removing an object from
 * the gray set, and turning it to black (which requires turning all its white references to gray). The gray set is implemented as
 * an intrusive singly linked list, using `gclist` field in multiple objects (functions, tables, threads and protos). When an object
 * doesn't have gclist field, the marking of that object needs to be "immediate", changing the colors of all references in one go.
 *
 * When a black object is modified, it needs to become gray again. Objects like this are placed on a separate `grayagain` list by a
 * barrier - this is important because it allows us to have a mark stage that terminates when the gray set is empty even if the mutator
 * is constantly changing existing objects to gray. After mark stage finishes traversing `gray` list, we copy `grayagain` list to `gray`
 * once and incrementally mark it again. During this phase of marking, we may get more objects marked as `grayagain`, so after we finish
 * emptying out the `gray` list the second time, we finish the mark stage and do final marking of `grayagain` during atomic phase.
 * GC works correctly without this second-phase mark (called GCSpropagateagain), but it reduces the time spent during atomic phase.
 *
 * Sweeping is also incremental, but instead of working at a granularity of an object, it works at a granularity of a page: all GC
 * objects are allocated in special pages (see lmem.cpp for details), and sweeper traverses all objects in one page in one incremental
 * step, freeing objects that aren't reachable (old white), and recoloring all other objects with the new white to prepare them for next
 * mark. During sweeping we don't need to maintain the GC invariant, because our goal is to paint all objects with current white -
 * however, some barriers will still trigger (because some reachable objects are still black as sweeping didn't get to them yet), and
 * some barriers will proactively mark black objects as white to avoid extra barriers from triggering excessively.
 *
 * Most references that GC deals with are strong, and as such they fit neatly into the incremental marking scheme. Some, however, are
 * weak - notably, tables can be marked as having weak keys/values (using __mode metafield). During incremental marking, we don't know
 * for certain if a given object is alive - if it's marked as black, it definitely was reachable during marking, but if it's marked as
 * white, we don't know if it's actually unreachable. Because of this, we need to defer weak table handling to the atomic phase; after
 * all objects are marked, we traverse all weak tables (that are linked into special weak table lists using `gclist` during marking),
 * and remove all entries that have white keys or values. If keys or values are strong, they are marked normally.
 *
 * The simplified scheme described above isn't fully accurate because of threads and upvalues.
 *
 * Threads are hard to deal with because for them to fit into the white-gray-black scheme, writes to thread stacks need to have barriers
 * that turn the thread from black (already scanned) to gray - but this is very expensive because stack writes are very common. To
 * get around this problem, threads have an "active" state which means that a thread is actively executing code. When GC reaches an active
 * thread, it keeps it as gray, and rescans it during atomic phase. When a thread is inactive, GC instead paints the thread black. All
 * API calls that can write to thread stacks outside of execution (which implies active) uses a thread barrier that checks if the thread is
 * black, and if it is it marks it as gray and puts it on a gray list to be rescanned during atomic phase.
 *
 * Upvalues are special objects that can be closed, in which case they contain the value (acting as a reference cell) and can be dealt
 * with using the regular algorithm, or open, in which case they refer to a stack slot in some other thread. These are difficult to deal
 * with because the stack writes are not monitored. Because of this open upvalues are treated in a somewhat special way: they are never marked
 * as black (doing so would violate the GC invariant), and they are kept in a special global list (global_State::uvhead) which is traversed
 * during atomic phase. This is needed because an open upvalue might point to a stack location in a dead thread that never marked the stack
 * slot - upvalues like this are identified since they don't have `markedopen` bit set during thread traversal and closed in `clearupvals`.
 */

#define GC_SWEEPPAGESTEPCOST 16

#define GC_INTERRUPT(state) \
    { \
        void (*interrupt)(lua_State*, int) = g->cb.interrupt; \
        if (LUAU_UNLIKELY(!!interrupt)) \
            interrupt(L, state); \
    }

#define maskmarks cast_byte(~(bitmask(BLACKBIT) | WHITEBITS))

#define makewhite(g, x) ((x)->gch.marked = cast_byte(((x)->gch.marked & maskmarks) | luaC_white(g)))

#define white2gray(x) reset2bits((x)->gch.marked, WHITE0BIT, WHITE1BIT)
#define black2gray(x) resetbit((x)->gch.marked, BLACKBIT)

#define stringmark(s) (reset2bits((s)->marked, WHITE0BIT, WHITE1BIT), l_setbit((s)->marked, BLACKBIT))

#define markvalue(g, o) \
    { \
        checkconsistency(o); \
        if (iscollectable(o) && iswhite(gcvalue(o))) \
            reallymarkobject(g, gcvalue(o)); \
    }

#define markobject(g, t) \
    { \
        if (iswhite(obj2gco(t))) \
            reallymarkobject(g, obj2gco(t)); \
    }

#ifdef LUAI_GCMETRICS
static void recordGcStateStep(global_State* g, int startgcstate, double seconds, bool assist, size_t work)
{
    switch (startgcstate)
    {
    case GCSpause:
        // record root mark time if we have switched to next state
        if (g->gcstate == GCSpropagate)
        {
            g->gcmetrics.currcycle.marktime += seconds;

            if (assist)
                g->gcmetrics.currcycle.markassisttime += seconds;
        }
        break;
    case GCSpropagate:
    case GCSpropagateagain:
        g->gcmetrics.currcycle.marktime += seconds;
        g->gcmetrics.currcycle.markwork += work;

        if (assist)
            g->gcmetrics.currcycle.markassisttime += seconds;
        break;
    case GCSatomic:
        g->gcmetrics.currcycle.atomictime += seconds;
        break;
    case GCSsweep:
        g->gcmetrics.currcycle.sweeptime += seconds;
        g->gcmetrics.currcycle.sweepwork += work;

        if (assist)
            g->gcmetrics.currcycle.sweepassisttime += seconds;
        break;
    default:
        LUAU_ASSERT(!"Unexpected GC state");
    }

    if (assist)
    {
        g->gcmetrics.stepassisttimeacc += seconds;
        g->gcmetrics.currcycle.assistwork += work;
    }
    else
    {
        g->gcmetrics.stepexplicittimeacc += seconds;
        g->gcmetrics.currcycle.explicitwork += work;
    }
}

static double recordGcDeltaTime(double& timer)
{
    double now = lua_clock();
    double delta = now - timer;
    timer = now;
    return delta;
}

static void startGcCycleMetrics(global_State* g)
{
    g->gcmetrics.currcycle.starttimestamp = lua_clock();
    g->gcmetrics.currcycle.pausetime = g->gcmetrics.currcycle.starttimestamp - g->gcmetrics.lastcycle.endtimestamp;
}

static void finishGcCycleMetrics(global_State* g)
{
    g->gcmetrics.currcycle.endtimestamp = lua_clock();
    g->gcmetrics.currcycle.endtotalsizebytes = g->totalbytes;

    g->gcmetrics.completedcycles++;
    g->gcmetrics.lastcycle = g->gcmetrics.currcycle;
    g->gcmetrics.currcycle = GCCycleMetrics();

    g->gcmetrics.currcycle.starttotalsizebytes = g->totalbytes;
    g->gcmetrics.currcycle.heaptriggersizebytes = g->GCthreshold;
}
#endif

static void removeentry(LuaNode* n)
{
    LUAU_ASSERT(ttisnil(gval(n)));
    if (iscollectable(gkey(n)))
        setttype(gkey(n), LUA_TDEADKEY); // dead key; remove it
}

static void reallymarkobject(global_State* g, GCObject* o)
{
    LUAU_ASSERT(iswhite(o) && !isdead(g, o));
    white2gray(o);
    switch (o->gch.tt)
    {
    case LUA_TSTRING:
    {
        gray2black(o); // strings are never gray
        return;
    }
    case LUA_TUSERDATA:
    {
        if (FFlag::LuauGcTraceUdata)
        {
            gray2black(o); // udata are never gray
            Udata* u = gco2u(o);
            if (u->tag < LUA_UTAG_LIMIT)
                if (lua_UserdataMark markfn = g->udatamark[u->tag])
                    markfn(g->mainthread, u->data);
            if (LuaTable* mt = u->metatable)
                markobject(g, mt);
            return;
        }
        else
        {
            LuaTable* mt = gco2u(o)->metatable;
            gray2black(o); // udata are never gray
            if (mt)
                markobject(g, mt);
            return;
        }
    }
    case LUA_TUPVAL:
    {
        UpVal* uv = gco2uv(o);
        markvalue(g, uv->v);
        if (!upisopen(uv)) // closed?
            gray2black(o); // open upvalues are never black
        return;
    }
    case LUA_TFUNCTION:
    {
        gco2cl(o)->gclist = g->gray;
        g->gray = o;
        break;
    }
    case LUA_TTABLE:
    {
        gco2h(o)->gclist = g->gray;
        g->gray = o;
        break;
    }
    case LUA_TTHREAD:
    {
        gco2th(o)->gclist = g->gray;
        g->gray = o;
        break;
    }
    case LUA_TVECTOR:
    {
        gray2black(o); // vectors are never gray
        return;
    }
    case LUA_TBUFFER:
    {
        gray2black(o); // buffers are never gray
        return;
    }
    case LUA_TPROTO:
    {
        gco2p(o)->gclist = g->gray;
        g->gray = o;
        break;
    }
    case LUA_TCLASS:
    {
        gco2class(o)->gclist = g->gray;
        g->gray = o;
        break;
    }
    case LUA_TOBJECT:
    {
        gco2object(o)->gclist = g->gray;
        g->gray = o;
        break;
    }
    default:
        LUAU_ASSERT(0);
    }
}

static const char* gettablemode(global_State* g, LuaTable* h)
{
    const TValue* mode = gfasttm(g, h->metatable, TM_MODE);

    if (mode && ttisstring(mode))
        return svalue(mode);

    return NULL;
}

static int traversetable(global_State* g, LuaTable* h)
{
    int i;
    int weakkey = 0;
    int weakvalue = 0;
    if (h->metatable)
        markobject(g, cast_to(LuaTable*, h->metatable));

    // is there a weak mode?
    if (const char* modev = gettablemode(g, h))
    {
        weakkey = (strchr(modev, 'k') != NULL);
        weakvalue = (strchr(modev, 'v') != NULL);
        if (weakkey || weakvalue)
        {                         // is really weak?
            h->gclist = g->weak;  // must be cleared after GC, ...
            g->weak = obj2gco(h); // ... so put in the appropriate list
        }
    }

    if (weakkey && weakvalue)
        return 1;
    if (!weakvalue)
    {
        i = h->sizearray;
        while (i--)
            markvalue(g, &h->array[i]);
    }
    i = sizenode(h);
    while (i--)
    {
        LuaNode* n = gnode(h, i);
        LUAU_ASSERT(ttype(gkey(n)) != LUA_TDEADKEY || ttisnil(gval(n)));
        if (ttisnil(gval(n)))
            removeentry(n); // remove empty entries
        else
        {
            LUAU_ASSERT(!ttisnil(gkey(n)));
            if (!weakkey)
                markvalue(g, gkey(n));
            if (!weakvalue)
                markvalue(g, gval(n));
        }
    }
    return weakkey || weakvalue;
}

/*
** All marks are conditional because a GC may happen while the
** prototype is still being created
*/
static void traverseproto(global_State* g, Proto* f)
{
    int i;
    if (f->source)
        stringmark(f->source);
    if (f->debugname)
        stringmark(f->debugname);
    for (i = 0; i < f->sizek; i++) // mark literals
        markvalue(g, &f->k[i]);
    for (i = 0; i < f->sizeupvalues; i++)
    { // mark upvalue names
        if (f->upvalues[i])
            stringmark(f->upvalues[i]);
    }
    for (i = 0; i < f->sizep; i++)
    { // mark nested protos
        if (f->p[i])
            markobject(g, f->p[i]);
    }
    for (i = 0; i < f->sizelocvars; i++)
    { // mark local-variable names
        if (f->locvars[i].varname)
            stringmark(f->locvars[i].varname);
    }
    if (f->optimized)
        markobject(g, f->optimized);

    if (f->deoptimized)
        markobject(g, f->deoptimized);
}

static void traverseclosure(global_State* g, Closure* cl)
{
    markobject(g, cl->env);
    if (cl->isC)
    {
        if (TString* str = cl->c.debugname)
            stringmark(str);

        int i;
        for (i = 0; i < cl->nupvalues; i++) // mark its upvalues
            markvalue(g, &cl->c.upvals[i]);
    }
    else
    {
        int i;
        LUAU_ASSERT(cl->nupvalues == cl->l.p->nups);
        markobject(g, cast_to(Proto*, cl->l.p));
        for (i = 0; i < cl->nupvalues; i++) // mark its upvalues
            markvalue(g, &cl->l.uprefs[i]);
    }
}

static void traversestack(global_State* g, lua_State* l)
{
    markobject(g, l->gt);

    if (l->namecall)
        stringmark(l->namecall);

    if (FFlag::LuauCoroutineFinally && l->finalizers)
        markobject(g, l->finalizers);

    for (StkId o = l->stack; o < l->top; o++)
        markvalue(g, o);

    for (UpVal* uv = l->openupval; uv; uv = uv->u.open.threadnext)
    {
        LUAU_ASSERT(upisopen(uv));
        uv->markedopen = 1;
        markobject(g, uv);
    }
}

static void traverseclass(global_State* g, LuauClass* classobject)
{
    markobject(g, classobject->name);
    if (classobject->super)
        markobject(g, classobject->super);
    markobject(g, classobject->memberstooffset);
    for (uint32_t i = 0; i < classobject->numberofallmembers; i++)
        markobject(g, classobject->offsettomember[i]);
    for (uint32_t i = 0; i < classobject->numberofallmembers - classobject->numberofinstancemembers; i++)
        markvalue(g, &classobject->staticmembers[i]);
    if (classobject->metatable)
        markobject(g, classobject->metatable);
    if (classobject->instancemetatable)
        markobject(g, classobject->instancemetatable);
}

static void traverseobject(global_State* g, LuauObject* classinst)
{
    markobject(g, classinst->lclass);
    for (uint32_t i = 0; i < classinst->numberofmembers; i++)
        markvalue(g, &classinst->members[i]);
}

static void clearstack(lua_State* l)
{
    StkId stack_end = l->stack + l->stacksize;
    for (StkId o = l->top; o < stack_end; o++) // clear not-marked stack slice
        setnilvalue(o);
}

static void shrinkstack(lua_State* L)
{
    // compute used stack - note that we can't use th->top if we're in the middle of vararg call
    StkId lim = L->top;
    for (CallInfo* ci = L->base_ci; ci <= L->ci; ci++)
    {
        LUAU_ASSERT(ci->top <= L->stack_last);
        if (lim < ci->top)
            lim = ci->top;
    }

    // shrink stack and callinfo arrays if we aren't using most of the space
    int ci_used = cast_int(L->ci - L->base_ci); // number of `ci' in use
    int s_used = cast_int(lim - L->stack);      // part of stack in use
    if (L->size_ci > LUAI_MAXCALLS)             // handling overflow?
        return;                                 // do not touch the stacks

    if (3 * size_t(ci_used) < size_t(L->size_ci) && 2 * BASIC_CI_SIZE < L->size_ci)
        luaD_reallocCI(L, L->size_ci / 2); // still big enough...
    condhardstacktests(luaD_reallocCI(L, ci_used + 1));

    if (3 * size_t(s_used) < size_t(L->stacksize) && 2 * (BASIC_STACK_SIZE + EXTRA_STACK) < L->stacksize)
        luaD_reallocstack(L, L->stacksize / 2, 0); // still big enough...
    condhardstacktests(luaD_reallocstack(L, s_used, 0));
}

static void shrinkstackprotected(lua_State* L)
{
    struct CallContext
    {
        static void run(lua_State* L, void* ud)
        {
            shrinkstack(L);
        }
    } ctx = {};

    // the resize call can fail on exception, in which case we will continue with original size
    int status = luaD_rawrunprotected(L, &CallContext::run, &ctx);
    LUAU_ASSERT(status == LUA_OK || status == LUA_ERRMEM);
}

/*
** traverse one gray object, turning it to black.
** Returns `quantity' traversed.
*/
static size_t propagatemark(global_State* g)
{
    GCObject* o = g->gray;
    LUAU_ASSERT(isgray(o));
    gray2black(o);
    switch (o->gch.tt)
    {
    case LUA_TTABLE:
    {
        LuaTable* h = gco2h(o);
        g->gray = h->gclist;
        if (traversetable(g, h)) // table is weak?
            black2gray(o);       // keep it gray

        return sizeof(LuaTable) + sizeof(TValue) * h->sizearray + sizeof(LuaNode) * (h->node == &luaH_dummynode ? 0 : sizenode(h));
    }
    case LUA_TFUNCTION:
    {
        Closure* cl = gco2cl(o);
        g->gray = cl->gclist;
        traverseclosure(g, cl);
        return cl->isC ? sizeCclosure(cl->nupvalues) : sizeLclosure(cl->nupvalues);
    }
    case LUA_TTHREAD:
    {
        lua_State* th = gco2th(o);
        g->gray = th->gclist;

        bool active = th->isactive || th == th->global->mainthread;

        traversestack(g, th);

        // active threads will need to be rescanned later to mark new stack writes so we mark them gray again
        if (active)
        {
            th->gclist = g->grayagain;
            g->grayagain = o;

            black2gray(o);
        }

        // the stack needs to be cleared after the last modification of the thread state before sweep begins
        // if the thread is inactive, we might not see the thread in this cycle so we must clear it now
        if (!active || g->gcstate == GCSatomic)
            clearstack(th);

        // we could shrink stack at any time but we opt to do it during initial mark to do that just once per cycle
        if (g->gcstate == GCSpropagate)
            shrinkstackprotected(th);

        return sizeof(lua_State) + sizeof(TValue) * th->stacksize + sizeof(CallInfo) * th->size_ci;
    }
    case LUA_TPROTO:
    {
        Proto* p = gco2p(o);
        g->gray = p->gclist;
        traverseproto(g, p);

        return sizeof(Proto) + sizeof(Instruction) * p->sizecode + sizeof(Proto*) * p->sizep + sizeof(TValue) * p->sizek + p->sizelineinfo +
               sizeof(LocVar) * p->sizelocvars + sizeof(TString*) * p->sizeupvalues + p->sizetypeinfo;
    }
    case LUA_TCLASS:
    {
        LuauClass* classobject = gco2class(o);
        g->gray = classobject->gclist;
        traverseclass(g, classobject);
        // We've traversed the "object" itself ...
        return sizeof(LuauClass) +
               // ... plus the method closures, each a `TValue` wide ...
               ((classobject->numberofallmembers - classobject->numberofinstancemembers) * sizeof(TValue)) +
               // ... plus a string pointer for each method or property, each a pointer wide.
               (classobject->numberofallmembers * sizeof(TString*));
    }
    case LUA_TOBJECT:
    {
        LuauObject* classinst = gco2object(o);
        g->gray = classinst->gclist;
        traverseobject(g, classinst);
        // We've traversed the instance ...
        return sizeof(LuauObject) +
               // ... plus all of the instance fields.
               classinst->numberofmembers * sizeof(TValue);
    }
    default:
        LUAU_ASSERT(0);
        return 0;
    }
}

static void embeddermarkref(lua_State* L, int ref)
{
    LUAU_ASSERT(FFlag::LuauGcTraceUdata);
    if (ref <= LUA_REFNIL)
        return;
    LuaTable* wt = hvalue(&L->global->weakregistry);
    const TValue* slot = luaH_getnum(wt, ref);
    if (iscollectable(slot) && iswhite(gcvalue(slot)))
        reallymarkobject(L->global, gcvalue(slot));
}

static size_t propagateall(global_State* g)
{
    size_t work = 0;
    while (g->gray)
    {
        work += propagatemark(g);
    }
    return work;
}

/*
** The next function tells whether a key or value can be cleared from
** a weak table. Non-collectable objects are never removed from weak
** tables. Strings behave as `values', so are never removed too. for
** other objects: if really collected, cannot keep them.
*/
static int isobjcleared(GCObject* o)
{
    if (o->gch.tt == LUA_TSTRING)
    {
        stringmark(&o->ts); // strings are `values', so are never weak
        return 0;
    }

    return iswhite(o);
}

#define iscleared(o) (iscollectable(o) && isobjcleared(gcvalue(o)))

static void tableresizeprotected(lua_State* L, LuaTable* t, int nhsize)
{
    struct CallContext
    {
        LuaTable* t;
        int nhsize;

        static void run(lua_State* L, void* ud)
        {
            CallContext* ctx = (CallContext*)ud;

            luaH_resizehash(L, ctx->t, ctx->nhsize);
        }
    } ctx = {t, nhsize};

    // the resize call can fail on exception, in which case we will continue with original size
    int status = luaD_rawrunprotected(L, &CallContext::run, &ctx);
    LUAU_ASSERT(status == LUA_OK || status == LUA_ERRMEM);
}

static void rebuildmetacache(lua_State* L, LuaTable* h)
{
    uint8_t tmcache = 0;

    for (int event = 0; event < TM_N; ++event)
    {
        const TValue* value = luaH_getstr(h, L->global->tmname[event]);
        setobj(L, getmetacache(h, event), value);

        if (event <= TM_EQ && ttisnil(value))
            tmcache |= cast_byte(1u << event);
    }

    h->tmcache = tmcache;
}

/*
** clear collected entries from weaktables
*/
static size_t cleartable(lua_State* L, GCObject* l)
{
    size_t work = 0;
    while (l)
    {
        LuaTable* h = gco2h(l);

        work += sizeof(LuaTable) + sizeof(TValue) * h->sizearray + sizeof(LuaNode) * (h->node == &luaH_dummynode ? 0 : sizenode(h));

        int i = h->sizearray;
        while (i--)
        {
            TValue* o = &h->array[i];
            if (iscleared(o))   // value was collected?
                setnilvalue(o); // remove value
        }
        i = sizenode(h);
        int activevalues = 0;
        while (i--)
        {
            LuaNode* n = gnode(h, i);

            // non-empty entry?
            if (!ttisnil(gval(n)))
            {
                // can we clear key or value?
                if (iscleared(gkey(n)) || iscleared(gval(n)))
                {
                    setnilvalue(gval(n)); // remove value ...
                    removeentry(n);       // remove entry from table
                }
                else
                {
                    activevalues++;
                }
            }
        }

        if (FFlag::LuauFrozenMetaButterfly && hasmetacache(h))
            rebuildmetacache(L, h);

        if (!FFlag::LuauFrozenMetaButterfly || !h->readonly) // frozen tables do not shrink
        {
            if (const char* modev = gettablemode(L->global, h))
            {
                // are we allowed to shrink this weak table?
                if (strchr(modev, 's'))
                {
                    // shrink at 37.5% occupancy
                    if (activevalues < sizenode(h) * 3 / 8)
                        tableresizeprotected(L, h, activevalues);
                }
            }
        }

        l = h->gclist;
    }
    return work;
}

static void freeobj(lua_State* L, GCObject* o, lua_Page* page)
{
    switch (o->gch.tt)
    {
    case LUA_TPROTO:
        luaF_freeproto(L, gco2p(o), page);
        break;
    case LUA_TFUNCTION:
        luaF_freeclosure(L, gco2cl(o), page);
        break;
    case LUA_TUPVAL:
        luaF_freeupval(L, gco2uv(o), page);
        break;
    case LUA_TTABLE:
        luaH_free(L, gco2h(o), page);
        break;
    case LUA_TTHREAD:
        LUAU_ASSERT(gco2th(o) != L && gco2th(o) != L->global->mainthread);
        luaE_freethread(L, gco2th(o), page);
        break;
    case LUA_TSTRING:
        luaS_free(L, gco2ts(o), page);
        break;
    case LUA_TUSERDATA:
        luaU_freeudata(L, gco2u(o), page);
        break;
    case LUA_TVECTOR:
        luaVec_freevector(L, gco2vec(o), page);
        break;
    case LUA_TBUFFER:
        luaB_freebuffer(L, gco2buf(o), page);
        break;
    case LUA_TCLASS:
        luaR_freeclass(L, gco2class(o), page);
        break;
    case LUA_TOBJECT:
        luaR_freeobject(L, gco2object(o), page);
        break;
    default:
        LUAU_ASSERT(0);
    }
}

static void stringresizeprotected(lua_State* L, int newsize)
{
    struct CallContext
    {
        int newsize;

        static void run(lua_State* L, void* ud)
        {
            CallContext* ctx = (CallContext*)ud;

            luaS_resize(L, ctx->newsize);
        }
    } ctx = {newsize};

    // the resize call can fail on exception, in which case we will continue with original size
    int status = luaD_rawrunprotected(L, &CallContext::run, &ctx);
    LUAU_ASSERT(status == LUA_OK || status == LUA_ERRMEM);
}

static void shrinkbuffers(lua_State* L)
{
    global_State* g = L->global;
    // check size of string hash
    if (g->strt.nuse < cast_to(uint32_t, g->strt.size / 4) && g->strt.size > LUA_MINSTRTABSIZE * 2)
        stringresizeprotected(L, g->strt.size / 2); // table is too big
}

static void shrinkbuffersfull(lua_State* L)
{
    global_State* g = L->global;
    // check size of string hash
    int hashsize = g->strt.size;
    while (g->strt.nuse < cast_to(uint32_t, hashsize / 4) && hashsize > LUA_MINSTRTABSIZE * 2)
        hashsize /= 2;
    if (hashsize != g->strt.size)
        stringresizeprotected(L, hashsize); // table is too big
}

static bool deletegco(void* context, lua_Page* page, GCObject* gco)
{
    lua_State* L = (lua_State*)context;
    freeobj(L, gco, page);
    return true;
}

void luaC_freeall(lua_State* L)
{
    global_State* g = L->global;

    LUAU_ASSERT(L == g->mainthread);

    luaM_visitgco(L, L, deletegco);

    for (int i = 0; i < g->strt.size; i++) // free all string lists
        LUAU_ASSERT(g->strt.hash[i] == NULL);

    LUAU_ASSERT(L->global->strt.nuse == 0);
}

static void markudatadirectaccess(global_State* g)
{
    for (int i = 0; i < UTAG_INTERNAL_LIMIT; i++)
    {
        lua_UdataDirectAccessData& udatadirect = g->udatadirect[i];

        markvalue(g, &udatadirect.indextm);
        markvalue(g, &udatadirect.newindextm);
        markvalue(g, &udatadirect.namecalltm);
    }
}

static void markudatadirectfields(global_State* g)
{
    for (int i = 0; i < UTAG_INTERNAL_LIMIT; i++)
        if (g->udatadirectfields[i])
            markobject(g, g->udatadirectfields[i]);
}

static void markmt(global_State* g)
{
    int i;
    for (i = 0; i < LUA_T_COUNT; i++)
        if (g->mt[i])
            markobject(g, g->mt[i]);
}

static void marktaggetmt(global_State* g)
{
    for (int i = 0; i < LUA_UTAG_LIMIT; i++)
    {
        if (g->udatamt[i])
            markobject(g, g->udatamt[i]);
    }
}

static void markfastpcalls(global_State* g)
{
    if (g->builtinPcall)
        markobject(g, g->builtinPcall);

    if (g->builtinXpcall)
        markobject(g, g->builtinXpcall);
}

// mark root set
static void markroot(lua_State* L)
{
    global_State* g = L->global;
    g->gray = NULL;
    g->grayagain = NULL;
    g->weak = NULL;
    markobject(g, g->mainthread);
    // make global table be traversed before main stack
    markobject(g, g->mainthread->gt);
    markvalue(g, registry(L));
    if (FFlag::LuauGcTraceUdata)
    {
        markvalue(g, &g->weakregistry);
        if (g->embeddergc)
            g->embeddergc(g->mainthread, nullptr);
    }

    markudatadirectaccess(g);

    markudatadirectfields(g);

    if (FFlag::LuauFastpcall)
        markfastpcalls(g);

    markmt(g);

    marktaggetmt(g);

    g->gcstate = GCSpropagate;
}

static size_t remarkupvals(global_State* g)
{
    size_t work = 0;

    for (UpVal* uv = g->uvhead.u.open.next; uv != &g->uvhead; uv = uv->u.open.next)
    {
        work += sizeof(UpVal);

        LUAU_ASSERT(upisopen(uv));
        LUAU_ASSERT(uv->u.open.next->u.open.prev == uv && uv->u.open.prev->u.open.next == uv);
        LUAU_ASSERT(!isblack(obj2gco(uv))); // open upvalues are never black

        if (isgray(obj2gco(uv)))
            markvalue(g, uv->v);
    }

    return work;
}

static size_t clearupvals(lua_State* L)
{
    global_State* g = L->global;

    size_t work = 0;

    for (UpVal* uv = g->uvhead.u.open.next; uv != &g->uvhead;)
    {
        work += sizeof(UpVal);

        LUAU_ASSERT(upisopen(uv));
        LUAU_ASSERT(uv->u.open.next->u.open.prev == uv && uv->u.open.prev->u.open.next == uv);
        LUAU_ASSERT(!isblack(obj2gco(uv))); // open upvalues are never black
        LUAU_ASSERT(iswhite(obj2gco(uv)) || !iscollectable(uv->v) || !iswhite(gcvalue(uv->v)));

        if (uv->markedopen)
        {
            // upvalue is still open (belongs to alive thread)
            LUAU_ASSERT(isgray(obj2gco(uv)));
            uv->markedopen = 0; // for next cycle
            uv = uv->u.open.next;
        }
        else
        {
            // upvalue is either dead, or alive but the thread is dead; unlink and close
            UpVal* next = uv->u.open.next;
            luaF_closeupval(L, uv, /* dead= */ iswhite(obj2gco(uv)));
            uv = next;
        }
    }

    return work;
}

static size_t atomic(lua_State* L)
{
    global_State* g = L->global;
    LUAU_ASSERT(g->gcstate == GCSatomic);

    size_t work = 0;

#ifdef LUAI_GCMETRICS
    double currts = lua_clock();
#endif

    // remark occasional upvalues of (maybe) dead threads
    work += remarkupvals(g);
    // traverse objects caught by write barrier and by 'remarkupvals'
    work += propagateall(g);

#ifdef LUAI_GCMETRICS
    g->gcmetrics.currcycle.atomictimeupval += recordGcDeltaTime(currts);
#endif

    // remark weak tables
    g->gray = g->weak;
    g->weak = NULL;
    LUAU_ASSERT(!iswhite(obj2gco(g->mainthread)));

    markobject(g, L); // mark running thread
    markmt(g);        // mark basic metatables (again)

    marktaggetmt(g); // mark tagged userdata metatables (again)

    markudatadirectaccess(g); // mark tagged userdata direct access functions (again)

    markudatadirectfields(g); // mark direct field dispatch tables (again)

    if (FFlag::LuauFastpcall)
        markfastpcalls(g);

    work += propagateall(g);

#ifdef LUAI_GCMETRICS
    g->gcmetrics.currcycle.atomictimeweak += recordGcDeltaTime(currts);
#endif

    // remark gray again
    g->gray = g->grayagain;
    g->grayagain = NULL;
    work += propagateall(g);

#ifdef LUAI_GCMETRICS
    g->gcmetrics.currcycle.atomictimegray += recordGcDeltaTime(currts);
#endif

    // fixed-point algorithm for embedder references
    if (FFlag::LuauGcTraceUdata && g->embeddergc)
    {
        g->embeddergc(g->mainthread, embeddermarkref);
        while (g->gray)
        {
            work += propagateall(g);
            g->embeddergc(g->mainthread, embeddermarkref);
        }
    }

#ifdef LUAI_GCMETRICS
    if (FFlag::LuauGcTraceUdata)
        g->gcmetrics.currcycle.atomictimeembedder += recordGcDeltaTime(currts);
#endif

    // remove collected objects from weak tables
    work += cleartable(L, g->weak);
    g->weak = NULL;

#ifdef LUAI_GCMETRICS
    g->gcmetrics.currcycle.atomictimeclear += recordGcDeltaTime(currts);
#endif

    // close orphaned live upvalues of dead threads and clear dead upvalues
    work += clearupvals(L);

#ifdef LUAI_GCMETRICS
    g->gcmetrics.currcycle.atomictimeupval += recordGcDeltaTime(currts);
#endif

    // flip current white
    g->currentwhite = cast_byte(otherwhite(g));
    g->sweepgcopage = g->allgcopages;
    g->sweepgcopage_cage = g->allgcopages_cage;
    g->gcstate = GCSsweep;

    return work;
}

// a version of generic luaM_visitpage specialized for the main sweep stage
static int sweepgcopage(lua_State* L, lua_Page* page)
{
    char* start;
    char* end;
    int busyBlocks;
    int blockSize;
    luaM_getpagewalkinfo(page, &start, &end, &busyBlocks, &blockSize);

    LUAU_ASSERT(busyBlocks > 0);

    global_State* g = L->global;

    int deadmask = otherwhite(g);
    LUAU_ASSERT(testbit(deadmask, FIXEDBIT)); // make sure we never sweep fixed objects

    int newwhite = luaC_white(g);

    for (char* pos = start; pos != end; pos += blockSize)
    {
        GCObject* gco = (GCObject*)pos;

        // skip memory blocks that are already freed
        if (gco->gch.tt == LUA_TNIL)
            continue;

        // is the object alive?
        if ((gco->gch.marked ^ WHITEBITS) & deadmask)
        {
            LUAU_ASSERT(!isdead(g, gco));
            // make it white (for next cycle)
            gco->gch.marked = cast_byte((gco->gch.marked & maskmarks) | newwhite);
        }
        else
        {
            LUAU_ASSERT(isdead(g, gco));
            freeobj(L, gco, page);

            // if the last block was removed, page would be removed as well
            if (--busyBlocks == 0)
                return int(pos - start) / blockSize + 1;
        }
    }

    return int(end - start) / blockSize;
}

static size_t gcstep(lua_State* L, size_t limit)
{
    size_t cost = 0;
    global_State* g = L->global;
    switch (g->gcstate)
    {
    case GCSpause:
    {
        markroot(L); // start a new collection
        LUAU_ASSERT(g->gcstate == GCSpropagate);
        break;
    }
    case GCSpropagate:
    {
        while (g->gray && cost < limit)
        {
            cost += propagatemark(g);
        }

        if (!g->gray)
        {
#ifdef LUAI_GCMETRICS
            g->gcmetrics.currcycle.propagatework = g->gcmetrics.currcycle.explicitwork + g->gcmetrics.currcycle.assistwork;
#endif

            // perform one iteration over 'gray again' list
            g->gray = g->grayagain;
            g->grayagain = NULL;

            g->gcstate = GCSpropagateagain;
        }
        break;
    }
    case GCSpropagateagain:
    {
        while (g->gray && cost < limit)
        {
            cost += propagatemark(g);
        }

        if (!g->gray) // no more `gray' objects
        {
#ifdef LUAI_GCMETRICS
            g->gcmetrics.currcycle.propagateagainwork =
                g->gcmetrics.currcycle.explicitwork + g->gcmetrics.currcycle.assistwork - g->gcmetrics.currcycle.propagatework;
#endif

            g->gcstate = GCSatomic;
        }
        break;
    }
    case GCSatomic:
    {
#ifdef LUAI_GCMETRICS
        g->gcmetrics.currcycle.atomicstarttimestamp = lua_clock();
        g->gcmetrics.currcycle.atomicstarttotalsizebytes = g->totalbytes;
#endif

        g->gcstats.atomicstarttimestamp = lua_clock();
        g->gcstats.atomicstarttotalsizebytes = g->totalbytes;

        cost = atomic(L); // finish mark phase

        LUAU_ASSERT(g->gcstate == GCSsweep);
        break;
    }
    case GCSsweep:
    {
        while (g->sweepgcopage && cost < limit)
        {
            lua_Page* next = luaM_getnextpage(g->sweepgcopage); // page sweep might destroy the page

            int steps = sweepgcopage(L, g->sweepgcopage);

            g->sweepgcopage = next;
            cost += static_cast<size_t>(steps * GC_SWEEPPAGESTEPCOST);
        }

        while (g->sweepgcopage_cage && cost < limit)
        {
            lua_Page* next = luaM_getnextpage(g->sweepgcopage_cage); // page sweep might destroy the page

            int steps = sweepgcopage(L, g->sweepgcopage_cage);

            g->sweepgcopage_cage = next;
            cost += static_cast<size_t>(steps * GC_SWEEPPAGESTEPCOST);
        }

        // nothing more to sweep?
        if (g->sweepgcopage == NULL && g->sweepgcopage_cage == NULL)
        {
            // don't forget to visit main thread, it's the only object not allocated in GCO pages
            LUAU_ASSERT(!isdead(g, obj2gco(g->mainthread)));
            makewhite(g, obj2gco(g->mainthread)); // make it white (for next cycle)

            shrinkbuffers(L);

            g->gcstate = GCSpause; // end collection
        }
        break;
    }
    default:
        LUAU_ASSERT(!"Unexpected GC state");
    }
    return cost;
}

static int64_t getheaptriggererroroffset(global_State* g)
{
    // adjust for error using Proportional-Integral controller
    // https://en.wikipedia.org/wiki/PID_controller
    int32_t errorKb = int32_t((g->gcstats.atomicstarttotalsizebytes - g->gcstats.heapgoalsizebytes) / 1024);

    // we use sliding window for the error integral to avoid error sum 'windup' when the desired target cannot be reached
    const size_t triggertermcount = sizeof(g->gcstats.triggerterms) / sizeof(g->gcstats.triggerterms[0]);

    int32_t* slot = &g->gcstats.triggerterms[g->gcstats.triggertermpos % triggertermcount];
    int32_t prev = *slot;
    *slot = errorKb;
    g->gcstats.triggerintegral += errorKb - prev;
    g->gcstats.triggertermpos++;

    // controller tuning
    // https://en.wikipedia.org/wiki/Ziegler%E2%80%93Nichols_method
    const double Ku = 0.9; // ultimate gain (measured)
    const double Tu = 2.5; // oscillation period (measured)

    const double Kp = 0.45 * Ku; // proportional gain
    const double Ti = 0.8 * Tu;
    const double Ki = 0.54 * Ku / Ti; // integral gain

    double proportionalTerm = Kp * errorKb;
    double integralTerm = Ki * g->gcstats.triggerintegral;

    double totalTerm = proportionalTerm + integralTerm;

    return int64_t(totalTerm * 1024);
}

// the heap can shrink between measurements (after a full collection, or when thread stacks are shrunk during marking), which counts as no growth
static size_t getheapgrowth(size_t current, size_t previous)
{
    return current > previous ? current - previous : 0;
}

static size_t getheaptrigger(global_State* g, size_t heapgoal)
{
    // adjust threshold based on a guess of how many bytes will be allocated between the cycle start and sweep phase
    // our goal is to begin the sweep when used memory has reached the heap goal
    const double durationthreshold = 1e-3;
    double allocationduration = g->gcstats.atomicstarttimestamp - g->gcstats.endtimestamp;

    // avoid measuring intervals smaller than 1ms
    if (allocationduration < durationthreshold)
        return heapgoal;

    double allocationrate;
    if (DFFlag::LuauGcHeapShrinkFix)
        allocationrate = getheapgrowth(g->gcstats.atomicstarttotalsizebytes, g->gcstats.endtotalsizebytes) / allocationduration;
    else
        allocationrate = (g->gcstats.atomicstarttotalsizebytes - g->gcstats.endtotalsizebytes) / allocationduration;
    double markduration = g->gcstats.atomicstarttimestamp - g->gcstats.starttimestamp;

    int64_t expectedgrowth = int64_t(markduration * allocationrate);
    int64_t offset = getheaptriggererroroffset(g);
    int64_t heaptrigger = heapgoal - (expectedgrowth + offset);

    // clamp the trigger between memory use at the end of the cycle and the heap goal
    return heaptrigger < int64_t(g->totalbytes) ? g->totalbytes : (heaptrigger > int64_t(heapgoal) ? heapgoal : size_t(heaptrigger));
}

size_t luaC_step(lua_State* L, bool assist)
{
    global_State* g = L->global;

    size_t lim = g->gcstepsize * g->gcstepmul / 100; // how much to work
    LUAU_ASSERT(g->totalbytes >= g->GCthreshold);
    size_t debt = g->totalbytes - g->GCthreshold;

    GC_INTERRUPT(0);

    // at the start of the new cycle
    if (g->gcstate == GCSpause)
        g->gcstats.starttimestamp = lua_clock();

#ifdef LUAI_GCMETRICS
    if (g->gcstate == GCSpause)
        startGcCycleMetrics(g);

    double lasttimestamp = lua_clock();
#endif

    // if allocations outpace the GC to require an assist, adjust step size to cover the difference
    if ((FFlag::LuauBackedgeHeapCheck || LUA_VECTOR_DOUBLE == 1) && assist)
    {
        size_t need = debt * g->gcstepmul / 100;

        if (need > lim)
            lim = need;
    }

    int lastgcstate = g->gcstate;

    size_t work = gcstep(L, lim);

#ifdef LUAI_GCMETRICS
    recordGcStateStep(g, lastgcstate, lua_clock() - lasttimestamp, assist, work);
#endif

    size_t actualstepsize = work * 100 / g->gcstepmul;

    // at the end of the last cycle
    if (g->gcstate == GCSpause)
    {
        // at the end of a collection cycle, set goal based on gcgoal setting
        size_t heapgoal = (g->totalbytes / 100) * g->gcgoal;
        size_t heaptrigger = getheaptrigger(g, heapgoal);

        g->GCthreshold = heaptrigger;

        g->gcstats.heapgoalsizebytes = heapgoal;
        g->gcstats.endtimestamp = lua_clock();
        g->gcstats.endtotalsizebytes = g->totalbytes;

#ifdef LUAI_GCMETRICS
        finishGcCycleMetrics(g);
#endif
    }
    else
    {
        g->GCthreshold = g->totalbytes + actualstepsize;

        // compensate if GC is "behind schedule" (has some debt to pay)
        if (g->GCthreshold >= debt)
            g->GCthreshold -= debt;
    }

    GC_INTERRUPT(lastgcstate);

    return actualstepsize;
}

void luaC_fullgc(lua_State* L)
{
    global_State* g = L->global;

#ifdef LUAI_GCMETRICS
    if (g->gcstate == GCSpause)
        startGcCycleMetrics(g);
#endif

    if (keepinvariant(g))
    {
        // reset sweep marks to sweep all elements (returning them to white)
        g->sweepgcopage = g->allgcopages;
        g->sweepgcopage_cage = g->allgcopages_cage;
        // reset other collector lists
        g->gray = NULL;
        g->grayagain = NULL;
        g->weak = NULL;
        g->gcstate = GCSsweep;
    }
    LUAU_ASSERT(g->gcstate == GCSpause || g->gcstate == GCSsweep);
    // finish any pending sweep phase
    while (g->gcstate != GCSpause)
    {
        LUAU_ASSERT(g->gcstate == GCSsweep);
        gcstep(L, SIZE_MAX);
    }

    // clear markedopen bits for all open upvalues; these might be stuck from half-finished mark prior to full gc
    for (UpVal* uv = g->uvhead.u.open.next; uv != &g->uvhead; uv = uv->u.open.next)
    {
        LUAU_ASSERT(upisopen(uv));
        uv->markedopen = 0;
    }

#ifdef LUAI_GCMETRICS
    finishGcCycleMetrics(g);
    startGcCycleMetrics(g);
#endif

    // run a full collection cycle
    markroot(L);
    while (g->gcstate != GCSpause)
    {
        gcstep(L, SIZE_MAX);
    }
    // reclaim as much buffer memory as possible (shrinkbuffers() called during sweep is incremental)
    shrinkbuffersfull(L);

    size_t heapgoalsizebytes = (g->totalbytes / 100) * g->gcgoal;

    // trigger cannot be correctly adjusted after a forced full GC.
    // we will try to place it so that we can reach the goal based on
    // the rate at which we run the GC relative to allocation rate
    // and on amount of bytes we need to traverse in propagation stage.
    // goal and stepmul are defined in percents
    g->GCthreshold = g->totalbytes * (g->gcgoal * g->gcstepmul / 100 - 100) / g->gcstepmul;

    // but it might be impossible to satisfy that directly
    if (g->GCthreshold < g->totalbytes)
        g->GCthreshold = g->totalbytes;

    g->gcstats.heapgoalsizebytes = heapgoalsizebytes;

    if (DFFlag::LuauGcHeapShrinkFix)
    {
        // full collection ends a cycle, so the heap growth is measured from this point
        g->gcstats.endtimestamp = lua_clock();
        g->gcstats.endtotalsizebytes = g->totalbytes;
    }

#ifdef LUAI_GCMETRICS
    finishGcCycleMetrics(g);
#endif
}

void luaC_barrierf(lua_State* L, GCObject* o, GCObject* v)
{
    global_State* g = L->global;
    LUAU_ASSERT(isblack(o) && iswhite(v) && !isdead(g, v) && !isdead(g, o));
    LUAU_ASSERT(g->gcstate != GCSpause);
    // must keep invariant?
    if (keepinvariant(g))
        reallymarkobject(g, v); // restore invariant
    else                        // don't mind
        makewhite(g, o);        // mark as white just to avoid other barriers
}

void luaC_barriertable(lua_State* L, LuaTable* t, GCObject* v)
{
    global_State* g = L->global;
    GCObject* o = obj2gco(t);

    // in the second propagation stage, table assignment barrier works as a forward barrier
    if (g->gcstate == GCSpropagateagain)
    {
        LUAU_ASSERT(isblack(o) && iswhite(v) && !isdead(g, v) && !isdead(g, o));
        reallymarkobject(g, v);
        return;
    }

    LUAU_ASSERT(isblack(o) && !isdead(g, o));
    LUAU_ASSERT(g->gcstate != GCSpause);
    black2gray(o); // make table gray (again)
    t->gclist = g->grayagain;
    g->grayagain = o;
}

void luaC_barrierback(lua_State* L, GCObject* o, GCObject** gclist)
{
    global_State* g = L->global;
    LUAU_ASSERT(isblack(o) && !isdead(g, o));
    LUAU_ASSERT(g->gcstate != GCSpause);

    black2gray(o); // make object gray (again)
    *gclist = g->grayagain;
    g->grayagain = o;
}

void luaC_upvalclosed(lua_State* L, UpVal* uv)
{
    global_State* g = L->global;
    GCObject* o = obj2gco(uv);

    LUAU_ASSERT(!upisopen(uv)); // upvalue was closed but needs GC state fixup

    if (isgray(o))
    {
        if (keepinvariant(g))
        {
            gray2black(o); // closed upvalues need barrier
            luaC_barrier(L, uv, uv->v);
        }
        else
        { // sweep phase: sweep it (turning it into white)
            makewhite(g, o);
            LUAU_ASSERT(g->gcstate != GCSpause);
        }
    }
}

// measure the allocation rate in bytes/sec
// returns -1 if allocation rate cannot be measured
int64_t luaC_allocationrate(lua_State* L)
{
    global_State* g = L->global;
    const double durationthreshold = 1e-3; // avoid measuring intervals smaller than 1ms

    if (g->gcstate <= GCSatomic)
    {
        double duration = lua_clock() - g->gcstats.endtimestamp;

        if (duration < durationthreshold)
            return -1;

        if (DFFlag::LuauGcHeapShrinkFix)
            return int64_t(getheapgrowth(g->totalbytes, g->gcstats.endtotalsizebytes) / duration);

        return int64_t((g->totalbytes - g->gcstats.endtotalsizebytes) / duration);
    }

    // totalbytes is unstable during the sweep, use the rate measured at the end of mark phase
    double duration = g->gcstats.atomicstarttimestamp - g->gcstats.endtimestamp;

    if (duration < durationthreshold)
        return -1;

    if (DFFlag::LuauGcHeapShrinkFix)
        return int64_t(getheapgrowth(g->gcstats.atomicstarttotalsizebytes, g->gcstats.endtotalsizebytes) / duration);

    return int64_t((g->gcstats.atomicstarttotalsizebytes - g->gcstats.endtotalsizebytes) / duration);
}

const char* luaC_statename(int state)
{
    switch (state)
    {
    case GCSpause:
        return "pause";

    case GCSpropagate:
        return "mark";

    case GCSpropagateagain:
        return "remark";

    case GCSatomic:
        return "atomic";

    case GCSsweep:
        return "sweep";

    default:
        return NULL;
    }
}
