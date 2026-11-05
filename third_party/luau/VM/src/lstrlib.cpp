// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#include "lualib.h"

#include "lstring.h"

#include <ctype.h>
#include <string.h>
#include <stdio.h>

LUAU_DYNAMIC_FASTFLAGVARIABLE(LuauOptimizeStringSplit, false)
LUAU_DYNAMIC_FASTFLAGVARIABLE(LuauOptimizeStringGsub, false)

// macro to `unsign' a character
#define uchar(c) ((unsigned char)(c))

static int str_len(lua_State* L)
{
    size_t l;
    luaL_checklstring(L, 1, &l);
    lua_pushinteger(L, (int)l);
    return 1;
}

static int posrelat(int pos, size_t len)
{
    // relative string position: negative means back from end
    if (pos < 0)
        pos += (int)len + 1;
    return (pos >= 0) ? pos : 0;
}

static int str_sub(lua_State* L)
{
    size_t l;
    const char* s = luaL_checklstring(L, 1, &l);
    int start = posrelat(luaL_checkinteger(L, 2), l);
    int end = posrelat(luaL_optinteger(L, 3, -1), l);
    if (start < 1)
        start = 1;
    if (end > (int)l)
        end = (int)l;
    if (start <= end)
        lua_pushlstring(L, s + start - 1, end - start + 1);
    else
        lua_pushliteral(L, "");
    return 1;
}

static int str_reverse(lua_State* L)
{
    size_t l;
    const char* s = luaL_checklstring(L, 1, &l);
    luaL_Strbuf b;
    char* ptr = luaL_buffinitsize(L, &b, l);
    while (l--)
        *ptr++ = s[l];
    luaL_pushresultsize(&b, ptr - b.p);
    return 1;
}

static int str_lower(lua_State* L)
{
    size_t l;
    const char* s = luaL_checklstring(L, 1, &l);
    luaL_Strbuf b;
    char* ptr = luaL_buffinitsize(L, &b, l);
    for (size_t i = 0; i < l; i++)
        *ptr++ = tolower(uchar(s[i]));
    luaL_pushresultsize(&b, l);
    return 1;
}

static int str_upper(lua_State* L)
{
    size_t l;
    const char* s = luaL_checklstring(L, 1, &l);
    luaL_Strbuf b;
    char* ptr = luaL_buffinitsize(L, &b, l);
    for (size_t i = 0; i < l; i++)
        *ptr++ = toupper(uchar(s[i]));
    luaL_pushresultsize(&b, l);
    return 1;
}

static int str_rep(lua_State* L)
{
    size_t l;
    const char* s = luaL_checklstring(L, 1, &l);
    int n = luaL_checkinteger(L, 2);

    if (n <= 0)
    {
        lua_pushliteral(L, "");
        return 1;
    }

    if (l > MAXSSIZE / (size_t)n) // may overflow?
        luaL_error(L, "resulting string too large");

    luaL_Strbuf b;
    char* ptr = luaL_buffinitsize(L, &b, l * n);

    const char* start = ptr;

    size_t left = l * n;
    size_t step = l;

    memcpy(ptr, s, l);
    ptr += l;
    left -= l;

    // use the increasing 'pattern' inside our target buffer to fill the next part
    while (step < left)
    {
        memcpy(ptr, start, step);
        ptr += step;
        left -= step;
        step <<= 1;
    }

    // fill tail
    memcpy(ptr, start, left);
    ptr += left;

    luaL_pushresultsize(&b, l * n);

    return 1;
}

static int str_byte(lua_State* L)
{
    size_t l;
    const char* s = luaL_checklstring(L, 1, &l);
    int posi = posrelat(luaL_optinteger(L, 2, 1), l);
    int pose = posrelat(luaL_optinteger(L, 3, posi), l);
    int n, i;
    if (posi <= 0)
        posi = 1;
    if ((size_t)pose > l)
        pose = (int)l;
    if (posi > pose)
        return 0; // empty interval; return no values
    n = (int)(pose - posi + 1);
    if (posi + n <= pose) // overflow?
        luaL_error(L, "string slice too long");
    luaL_checkstack(L, n, "string slice too long");
    for (i = 0; i < n; i++)
        lua_pushinteger(L, uchar(s[posi + i - 1]));
    return n;
}

static int str_char(lua_State* L)
{
    int n = lua_gettop(L); // number of arguments

    luaL_Strbuf b;
    char* ptr = luaL_buffinitsize(L, &b, n);

    for (int i = 1; i <= n; i++)
    {
        int c = luaL_checkinteger(L, i);
        luaL_argcheck(L, uchar(c) == c, i, "invalid value");

        *ptr++ = uchar(c);
    }
    luaL_pushresultsize(&b, n);
    return 1;
}

/*
** {======================================================
** PATTERN MATCHING
** =======================================================
*/

#define CAP_UNFINISHED (-1)
#define CAP_POSITION (-2)

typedef struct MatchState
{
    int matchdepth;       // control for recursive depth (to avoid C stack overflow)
    const char* src_init; // init of source string
    const char* src_end;  // end ('\0') of source string
    const char* p_end;    // end ('\0') of pattern
    lua_State* L;
    int level; // total number of captures (finished or unfinished)
    struct
    {
        const char* init;
        ptrdiff_t len;
    } capture[LUA_MAXCAPTURES];
} MatchState;

// recursive function
static const char* match(MatchState* ms, const char* s, const char* p);

#define L_ESC '%'
#define SPECIALS "^$*+?.([%-"

static int check_capture(MatchState* ms, int l)
{
    l -= '1';
    if (l < 0 || l >= ms->level || ms->capture[l].len == CAP_UNFINISHED)
        luaL_error(ms->L, "invalid capture index %%%d", l + 1);
    return l;
}

static int capture_to_close(MatchState* ms)
{
    int level = ms->level;
    for (level--; level >= 0; level--)
        if (ms->capture[level].len == CAP_UNFINISHED)
            return level;
    luaL_error(ms->L, "invalid pattern capture");
}

static const char* classend(MatchState* ms, const char* p)
{
    switch (*p++)
    {
    case L_ESC:
    {
        if (p == ms->p_end)
            luaL_error(ms->L, "malformed pattern (ends with '%%')");
        return p + 1;
    }
    case '[':
    {
        if (*p == '^')
            p++;
        do
        { // look for a `]'
            if (p == ms->p_end)
                luaL_error(ms->L, "malformed pattern (missing ']')");
            if (*(p++) == L_ESC && p < ms->p_end)
                p++; // skip escapes (e.g. `%]')
        } while (*p != ']');
        return p + 1;
    }
    default:
    {
        return p;
    }
    }
}

static int match_class(int c, int cl)
{
    int res;
    switch (tolower(cl))
    {
    case 'a':
        res = isalpha(c);
        break;
    case 'c':
        res = iscntrl(c);
        break;
    case 'd':
        res = isdigit(c);
        break;
    case 'g':
        res = isgraph(c);
        break;
    case 'l':
        res = islower(c);
        break;
    case 'p':
        res = ispunct(c);
        break;
    case 's':
        res = isspace(c);
        break;
    case 'u':
        res = isupper(c);
        break;
    case 'w':
        res = isalnum(c);
        break;
    case 'x':
        res = isxdigit(c);
        break;
    case 'z':
        res = (c == 0);
        break; // deprecated option
    default:
        return (cl == c);
    }
    return (islower(cl) ? res : !res);
}

static int matchbracketclass(int c, const char* p, const char* ec)
{
    int sig = 1;
    if (*(p + 1) == '^')
    {
        sig = 0;
        p++; // skip the `^'
    }
    while (++p < ec)
    {
        if (*p == L_ESC)
        {
            p++;
            if (match_class(c, uchar(*p)))
                return sig;
        }
        else if ((*(p + 1) == '-') && (p + 2 < ec))
        {
            p += 2;
            if (uchar(*(p - 2)) <= c && c <= uchar(*p))
                return sig;
        }
        else if (uchar(*p) == c)
            return sig;
    }
    return !sig;
}

static int singlematch(MatchState* ms, const char* s, const char* p, const char* ep)
{
    if (s >= ms->src_end)
        return 0;
    else
    {
        int c = uchar(*s);
        switch (*p)
        {
        case '.':
            return 1; // matches any char
        case L_ESC:
            return match_class(c, uchar(*(p + 1)));
        case '[':
            return matchbracketclass(c, p, ep - 1);
        default:
            return (uchar(*p) == c);
        }
    }
}

static const char* matchbalance(MatchState* ms, const char* s, const char* p)
{
    if (p >= ms->p_end - 1)
        luaL_error(ms->L, "malformed pattern (missing arguments to '%%b')");
    if (*s != *p)
        return NULL;
    else
    {
        int b = *p;
        int e = *(p + 1);
        int cont = 1;
        while (++s < ms->src_end)
        {
            if (*s == e)
            {
                if (--cont == 0)
                    return s + 1;
            }
            else if (*s == b)
                cont++;
        }
    }
    return NULL; // string ends out of balance
}

static const char* max_expand(MatchState* ms, const char* s, const char* p, const char* ep)
{
    ptrdiff_t i = 0; // counts maximum expand for item
    while (singlematch(ms, s + i, p, ep))
        i++;
    // keeps trying to match with the maximum repetitions
    while (i >= 0)
    {
        const char* res = match(ms, (s + i), ep + 1);
        if (res)
            return res;
        i--; // else didn't match; reduce 1 repetition to try again
    }
    return NULL;
}

static const char* min_expand(MatchState* ms, const char* s, const char* p, const char* ep)
{
    for (;;)
    {
        const char* res = match(ms, s, ep + 1);
        if (res != NULL)
            return res;
        else if (singlematch(ms, s, p, ep))
            s++; // try with one more repetition
        else
            return NULL;
    }
}

static const char* start_capture(MatchState* ms, const char* s, const char* p, int what)
{
    const char* res;
    int level = ms->level;
    if (level >= LUA_MAXCAPTURES)
        luaL_error(ms->L, "too many captures");
    ms->capture[level].init = s;
    ms->capture[level].len = what;
    ms->level = level + 1;
    if ((res = match(ms, s, p)) == NULL) // match failed?
        ms->level--;                     // undo capture
    return res;
}

static const char* end_capture(MatchState* ms, const char* s, const char* p)
{
    int l = capture_to_close(ms);
    const char* res;
    ms->capture[l].len = s - ms->capture[l].init; // close capture
    if ((res = match(ms, s, p)) == NULL)          // match failed?
        ms->capture[l].len = CAP_UNFINISHED;      // undo capture
    return res;
}

static const char* match_capture(MatchState* ms, const char* s, int l)
{
    size_t len;
    l = check_capture(ms, l);
    len = ms->capture[l].len;
    if ((size_t)(ms->src_end - s) >= len && memcmp(ms->capture[l].init, s, len) == 0)
        return s + len;
    else
        return NULL;
}

static const char* match(MatchState* ms, const char* s, const char* p)
{
    if (ms->matchdepth-- == 0)
        luaL_error(ms->L, "pattern too complex");

    lua_State* L = ms->L;
    void (*interrupt)(lua_State*, int) = L->global->cb.interrupt;

    if (LUAU_UNLIKELY(!!interrupt))
    {
        // this interrupt is not yieldable
        L->nCcalls++;
        interrupt(L, -1);
        L->nCcalls--;
    }

init: // using goto's to optimize tail recursion
    if (p != ms->p_end)
    { // end of pattern?
        switch (*p)
        {
        case '(':
        {                        // start capture
            if (*(p + 1) == ')') // position capture?
                s = start_capture(ms, s, p + 2, CAP_POSITION);
            else
                s = start_capture(ms, s, p + 1, CAP_UNFINISHED);
            break;
        }
        case ')':
        { // end capture
            s = end_capture(ms, s, p + 1);
            break;
        }
        case '$':
        {
            if ((p + 1) != ms->p_end)          // is the `$' the last char in pattern?
                goto dflt;                     // no; go to default
            s = (s == ms->src_end) ? s : NULL; // check end of string
            break;
        }
        case L_ESC:
        { // escaped sequences not in the format class[*+?-]?
            switch (*(p + 1))
            {
            case 'b':
            { // balanced string?
                s = matchbalance(ms, s, p + 2);
                if (s != NULL)
                {
                    p += 4;
                    goto init; // return match(ms, s, p + 4);
                } // else fail (s == NULL)
                break;
            }
            case 'f':
            { // frontier?
                const char* ep;
                char previous;
                p += 2;
                if (*p != '[')
                    luaL_error(ms->L, "missing '[' after '%%f' in pattern");
                ep = classend(ms, p); // points to what is next
                previous = (s == ms->src_init) ? '\0' : *(s - 1);
                if (!matchbracketclass(uchar(previous), p, ep - 1) && matchbracketclass(uchar(*s), p, ep - 1))
                {
                    p = ep;
                    goto init; // return match(ms, s, ep);
                }
                s = NULL; // match failed
                break;
            }
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
            { // capture results (%0-%9)?
                s = match_capture(ms, s, uchar(*(p + 1)));
                if (s != NULL)
                {
                    p += 2;
                    goto init; // return match(ms, s, p + 2)
                }
                break;
            }
            default:
                goto dflt;
            }
            break;
        }
        default:
        dflt:
        {                                     // pattern class plus optional suffix
            const char* ep = classend(ms, p); // points to optional suffix
            // does not match at least once?
            if (!singlematch(ms, s, p, ep))
            {
                if (*ep == '*' || *ep == '?' || *ep == '-')
                { // accept empty?
                    p = ep + 1;
                    goto init; // return match(ms, s, ep + 1);
                }
                else          // '+' or no suffix
                    s = NULL; // fail
            }
            else
            { // matched once
                switch (*ep)
                { // handle optional suffix
                case '?':
                { // optional
                    const char* res;
                    if ((res = match(ms, s + 1, ep + 1)) != NULL)
                        s = res;
                    else
                    {
                        p = ep + 1;
                        goto init; // else return match(ms, s, ep + 1);
                    }
                    break;
                }
                case '+':             // 1 or more repetitions
                    s++;              // 1 match already done
                    LUAU_FALLTHROUGH; // go through
                case '*':             // 0 or more repetitions
                    s = max_expand(ms, s, p, ep);
                    break;
                case '-': // 0 or more repetitions (minimum)
                    s = min_expand(ms, s, p, ep);
                    break;
                default: // no suffix
                    s++;
                    p = ep;
                    goto init; // return match(ms, s + 1, ep);
                }
            }
            break;
        }
        }
    }
    ms->matchdepth++;
    return s;
}

static const char* lmemfind(const char* s1, size_t l1, const char* s2, size_t l2)
{
    if (l2 == 0)
        return s1; // empty strings are everywhere
    else if (l2 > l1)
        return NULL; // avoids a negative `l1'
    else
    {
        const char* init; // to search for a `*s2' inside `s1'
        l2--;             // 1st char will be checked by `memchr'
        l1 = l1 - l2;     // `s2' cannot be found after that
        while (l1 > 0 && (init = (const char*)memchr(s1, *s2, l1)) != NULL)
        {
            init++; // 1st char is already checked
            if (memcmp(init, s2 + 1, l2) == 0)
                return init - 1;
            else
            { // correct `l1' and `s1' to try again
                l1 -= init - s1;
                s1 = init;
            }
        }
        return NULL; // not found
    }
}

static void push_onecapture(MatchState* ms, int i, const char* s, const char* e)
{
    if (i >= ms->level)
    {
        if (i == 0)                           // ms->level == 0, too
            lua_pushlstring(ms->L, s, e - s); // add whole match
        else
            luaL_error(ms->L, "invalid capture index");
    }
    else
    {
        ptrdiff_t l = ms->capture[i].len;
        if (l == CAP_UNFINISHED)
            luaL_error(ms->L, "unfinished capture");
        if (l == CAP_POSITION)
            lua_pushinteger(ms->L, (int)(ms->capture[i].init - ms->src_init) + 1);
        else
            lua_pushlstring(ms->L, ms->capture[i].init, l);
    }
}

static int push_captures(MatchState* ms, const char* s, const char* e)
{
    int i;
    int nlevels = (ms->level == 0 && s) ? 1 : ms->level;
    luaL_checkstack(ms->L, nlevels, "too many captures");
    for (i = 0; i < nlevels; i++)
        push_onecapture(ms, i, s, e);
    return nlevels; // number of strings pushed
}

// check whether pattern has no special characters
static int nospecials(const char* p, size_t l)
{
    size_t upto = 0;
    do
    {
        if (strpbrk(p + upto, SPECIALS))
            return 0;                 // pattern has a special character
        upto += strlen(p + upto) + 1; // may have more after \0
    } while (upto <= l);
    return 1; // no special chars found
}

static void prepstate(MatchState* ms, lua_State* L, const char* s, size_t ls, const char* p, size_t lp)
{
    ms->L = L;
    ms->matchdepth = LUAI_MAXCCALLS;
    ms->src_init = s;
    ms->src_end = s + ls;
    ms->p_end = p + lp;
}

static void reprepstate(MatchState* ms)
{
    ms->level = 0;
    LUAU_ASSERT(ms->matchdepth == LUAI_MAXCCALLS);
}

static int str_find_aux(lua_State* L, int find)
{
    size_t ls, lp;
    const char* s = luaL_checklstring(L, 1, &ls);
    const char* p = luaL_checklstring(L, 2, &lp);
    int init = posrelat(luaL_optinteger(L, 3, 1), ls);
    if (init < 1)
        init = 1;
    else if (init > (int)ls + 1)
    {                   // start after string's end?
        lua_pushnil(L); // cannot find anything
        return 1;
    }
    // explicit request or no special characters?
    if (find && (lua_toboolean(L, 4) || nospecials(p, lp)))
    {
        // do a plain search
        const char* s2 = lmemfind(s + init - 1, ls - init + 1, p, lp);
        if (s2)
        {
            lua_pushinteger(L, (int)(s2 - s + 1));
            lua_pushinteger(L, (int)(s2 - s + lp));
            return 2;
        }
    }
    else
    {
        MatchState ms;
        const char* s1 = s + init - 1;
        int anchor = (*p == '^');
        if (anchor)
        {
            p++;
            lp--; // skip anchor character
        }
        prepstate(&ms, L, s, ls, p, lp);
        do
        {
            const char* res;
            reprepstate(&ms);
            if ((res = match(&ms, s1, p)) != NULL)
            {
                if (find)
                {
                    lua_pushinteger(L, (int)(s1 - s + 1)); // start
                    lua_pushinteger(L, (int)(res - s));    // end
                    return push_captures(&ms, NULL, 0) + 2;
                }
                else
                    return push_captures(&ms, s1, res);
            }
        } while (s1++ < ms.src_end && !anchor);
    }
    lua_pushnil(L); // not found
    return 1;
}

static int str_find(lua_State* L)
{
    return str_find_aux(L, 1);
}

static int str_match(lua_State* L)
{
    return str_find_aux(L, 0);
}

static int gmatch_aux(lua_State* L)
{
    MatchState ms;
    size_t ls, lp;
    const char* s = lua_tolstring(L, lua_upvalueindex(1), &ls);
    const char* p = lua_tolstring(L, lua_upvalueindex(2), &lp);
    const char* src;
    prepstate(&ms, L, s, ls, p, lp);
    for (src = s + (size_t)lua_tointeger(L, lua_upvalueindex(3)); src <= ms.src_end; src++)
    {
        const char* e;
        reprepstate(&ms);
        if ((e = match(&ms, src, p)) != NULL)
        {
            int newstart = (int)(e - s);
            if (e == src)
                newstart++; // empty match? go at least one position
            lua_pushinteger(L, newstart);
            lua_replace(L, lua_upvalueindex(3));
            return push_captures(&ms, src, e);
        }
    }
    return 0; // not found
}

static int gmatch(lua_State* L)
{
    luaL_checkstring(L, 1);
    luaL_checkstring(L, 2);
    lua_settop(L, 2);
    lua_pushinteger(L, 0);
    lua_pushcclosure(L, gmatch_aux, NULL, 3);
    return 1;
}

static void add_s(MatchState* ms, luaL_Strbuf* b, const char* s, const char* e)
{
    size_t l, i;
    const char* news = lua_tolstring(ms->L, 3, &l);

    luaL_prepbuffsize(b, l);

    for (i = 0; i < l; i++)
    {
        if (news[i] != L_ESC)
            luaL_addchar(b, news[i]);
        else
        {
            i++; // skip ESC
            if (!isdigit(uchar(news[i])))
            {
                if (news[i] != L_ESC)
                    luaL_error(ms->L, "invalid use of '%c' in replacement string", L_ESC);
                luaL_addchar(b, news[i]);
            }
            else if (news[i] == '0')
                luaL_addlstring(b, s, e - s);
            else
            {
                push_onecapture(ms, news[i] - '1', s, e);
                luaL_addvalue(b); // add capture to accumulated result
            }
        }
    }
}

static void add_value(MatchState* ms, luaL_Strbuf* b, const char* s, const char* e, int tr)
{
    lua_State* L = ms->L;
    switch (tr)
    {
    case LUA_TFUNCTION:
    {
        int n;
        lua_pushvalue(L, 3);
        n = push_captures(ms, s, e);
        lua_call(L, n, 1);
        break;
    }
    case LUA_TTABLE:
    {
        push_onecapture(ms, 0, s, e);
        lua_gettable(L, 3);
        break;
    }
    default:
    { // LUA_TNUMBER or LUA_TSTRING
        add_s(ms, b, s, e);
        return;
    }
    }
    if (!lua_toboolean(L, -1))
    { // nil or false?
        lua_pop(L, 1);
        lua_pushlstring(L, s, e - s); // keep original text
    }
    else if (!lua_isstring(L, -1))
        luaL_error(L, "invalid replacement value (a %s)", luaL_typename(L, -1));
    luaL_addvalue(b); // add result to accumulator
}

// checks if a pattern only matches its own text
// ')' is not in SPECIALS, but it is still an error in a pattern, so it has to take the regular path
// ']' is intentionally left out: it is only special inside a set, and a set always starts with '[' which is in SPECIALS
static bool isliteralpattern(const char* p, size_t lp)
{
    // lookup table built from SPECIALS and ')', so that each character is checked without a strchr call
    struct SpecialChars
    {
        bool chars[256] = {};

        SpecialChars()
        {
            for (const char* c = SPECIALS ")"; *c; c++)
                chars[uchar(*c)] = true;
        }
    };

    static const SpecialChars specials;

    for (size_t i = 0; i < lp; i++)
    {
        if (specials.chars[uchar(p[i])])
            return false;
    }

    return true;
}

// finds the first occurrence of a non-empty literal in [s, end)
static const char* findliteral(const char* s, const char* end, const char* p, size_t lp)
{
    if (size_t(end - s) < lp)
        return NULL;

    const char* last = end - lp;

    if (lp == 1)
    {
        // matches that immediately follow each other don't need a memchr call
        if (*s == p[0])
            return s;

        return (const char*)memchr(s, p[0], last - s + 1);
    }

    // the first and the last characters are checked inline to avoid a memcmp call at most positions
    for (; s <= last; s++)
    {
        if (s[0] == p[0] && s[lp - 1] == p[lp - 1] && memcmp(s, p, lp) == 0)
            return s;
    }

    return NULL;
}

static int str_gsub(lua_State* L)
{
    size_t srcl, lp;
    const char* src = luaL_checklstring(L, 1, &srcl);
    const char* p = luaL_checklstring(L, 2, &lp);
    int tr = lua_type(L, 3);
    int max_s = luaL_optinteger(L, 4, (int)srcl + 1);
    int anchor = (*p == '^');
    int n = 0;
    MatchState ms;
    luaL_Strbuf b;
    luaL_argexpected(L, tr == LUA_TNUMBER || tr == LUA_TSTRING || tr == LUA_TFUNCTION || tr == LUA_TTABLE, 3, "string/function/table");
    luaL_buffinit(L, &b);
    if (anchor)
    {
        p++;
        lp--; // skip anchor character
    }
    prepstate(&ms, L, src, srcl, p, lp);

    // a non-empty pattern without special characters can only match its own text, so we can search for it directly
    if (DFFlag::LuauOptimizeStringGsub && !anchor && lp > 0 && isliteralpattern(p, lp))
    {
        // replacement strings without escapes can be appended as is; this is determined on the first match
        int plainrepl = -1;
        const char* repl = NULL;
        size_t repll = 0;

        while (n < max_s)
        {
            const char* e = findliteral(src, ms.src_end, p, lp);
            if (!e)
                break;

            n++;

            if (e != src)
                luaL_addlstring(&b, src, e - src);

            if (plainrepl < 0)
            {
                repl = (tr == LUA_TSTRING || tr == LUA_TNUMBER) ? lua_tolstring(L, 3, &repll) : NULL;
                plainrepl = repl && !memchr(repl, L_ESC, repll);
            }

            if (plainrepl)
            {
                if (repll != 0)
                    luaL_addlstring(&b, repl, repll);
            }
            else
            {
                reprepstate(&ms);
                add_value(&ms, &b, e, e + lp, tr);
            }

            src = e + lp;
        }
    }
    else
    {
        while (n < max_s)
        {
            const char* e;
            reprepstate(&ms);
            e = match(&ms, src, p);
            if (e)
            {
                n++;
                add_value(&ms, &b, src, e, tr);
            }
            if (e && e > src) // non empty match?
                src = e;      // skip it
            else if (src < ms.src_end)
                luaL_addchar(&b, *src++);
            else
                break;
            if (anchor)
                break;
        }
    }
    luaL_addlstring(&b, src, ms.src_end - src);
    luaL_pushresult(&b);
    lua_pushinteger(L, n); // number of substitutions
    return 2;
}

// }======================================================

// valid flags in a format specification
#define FLAGS "-+ #0"
// maximum size of each formatted item (> len(format('%99.99f', -1e308)))
#define MAX_ITEM 512
// maximum size of each format specification (such as '%-099.99d')
#define MAX_FORMAT 32

static void addquoted(lua_State* L, luaL_Strbuf* b, int arg)
{
    size_t l;
    const char* s = luaL_checklstring(L, arg, &l);

    luaL_prepbuffsize(b, l + 2);

    luaL_addchar(b, '"');
    while (l--)
    {
        switch (*s)
        {
        case '"':
        case '\\':
        case '\n':
        {
            luaL_addchar(b, '\\');
            luaL_addchar(b, *s);
            break;
        }
        case '\r':
        {
            luaL_addlstring(b, "\\r", 2);
            break;
        }
        case '\0':
        {
            luaL_addlstring(b, "\\000", 4);
            break;
        }
        default:
        {
            luaL_addchar(b, *s);
            break;
        }
        }
        s++;
    }
    luaL_addchar(b, '"');
}

static const char* scanformat(lua_State* L, const char* strfrmt, char* form, size_t* size)
{
    const char* p = strfrmt;
    while (*p != '\0' && strchr(FLAGS, *p) != NULL)
        p++; // skip flags
    if ((size_t)(p - strfrmt) >= sizeof(FLAGS))
        luaL_error(L, "invalid format (repeated flags)");
    if (isdigit(uchar(*p)))
        p++; // skip width
    if (isdigit(uchar(*p)))
        p++; // (2 digits at most)
    if (*p == '.')
    {
        p++;
        if (isdigit(uchar(*p)))
            p++; // skip precision
        if (isdigit(uchar(*p)))
            p++; // (2 digits at most)
    }
    if (isdigit(uchar(*p)))
        luaL_error(L, "invalid format (width or precision too long)");
    *(form++) = '%';
    *size = p - strfrmt + 1;
    strncpy(form, strfrmt, *size);
    form += *size;
    *form = '\0';
    return p;
}

static void addInt64Format(char form[MAX_FORMAT], char formatIndicator, size_t formatItemSize)
{
    LUAU_ASSERT((formatItemSize + 3) <= MAX_FORMAT);
    LUAU_ASSERT(form[0] == '%');
    LUAU_ASSERT(form[formatItemSize] != 0);
    LUAU_ASSERT(form[formatItemSize + 1] == 0);
    form[formatItemSize + 0] = 'l';
    form[formatItemSize + 1] = 'l';
    form[formatItemSize + 2] = formatIndicator;
    form[formatItemSize + 3] = 0;
}

static int str_format(lua_State* L)
{
    int top = lua_gettop(L);
    int arg = 1;
    size_t sfl;
    const char* strfrmt = luaL_checklstring(L, arg, &sfl);
    const char* strfrmt_end = strfrmt + sfl;
    luaL_Strbuf b;
    luaL_buffinit(L, &b);
    while (strfrmt < strfrmt_end)
    {
        if (*strfrmt != L_ESC)
            luaL_addchar(&b, *strfrmt++);
        else if (*++strfrmt == L_ESC)
            luaL_addchar(&b, *strfrmt++); // %%
        else if (*strfrmt == '*')
        {
            strfrmt++;
            if (++arg > top)
                luaL_error(L, "missing argument #%d", arg);

            luaL_addvalueany(&b, arg);
        }
        else
        {                          // format item
            char form[MAX_FORMAT]; // to store the format (`%...')
            char buff[MAX_ITEM];   // to store the formatted item
            if (++arg > top)
                luaL_error(L, "missing argument #%d", arg);
            size_t formatItemSize = 0;
            strfrmt = scanformat(L, strfrmt, form, &formatItemSize);
            char formatIndicator = *strfrmt++;
            switch (formatIndicator)
            {
            case 'c':
            {
                int count = snprintf(buff, sizeof(buff), form, (int)luaL_checknumber(L, arg));
                luaL_addlstring(&b, buff, count);
                continue; // skip the 'luaL_addlstring' at the end
            }
            case 'd':
            case 'i':
            {
                long long value = lua_isinteger64(L, arg) ? luaL_checkinteger64(L, arg) : (int64_t)luaL_checknumber(L, arg);
                addInt64Format(form, formatIndicator, formatItemSize);
                snprintf(buff, sizeof(buff), form, value);
                break;
            }
            case 'o':
            case 'u':
            case 'x':
            case 'X':
            {
                uint64_t v;
                if (lua_isinteger64(L, arg))
                {
                    v = luaL_checkinteger64(L, arg);
                }
                else
                {
                    double argValue = luaL_checknumber(L, arg);
                    v = (argValue < 0) ? (unsigned long long)(long long)argValue : (unsigned long long)argValue;
                }
                addInt64Format(form, formatIndicator, formatItemSize);
                snprintf(buff, sizeof(buff), form, v);
                break;
            }
            case 'e':
            case 'E':
            case 'f':
            case 'g':
            case 'G':
            {
                snprintf(buff, sizeof(buff), form, (double)luaL_checknumber(L, arg));
                break;
            }
            case 'q':
            {
                addquoted(L, &b, arg);
                continue; // skip the 'luaL_addlstring' at the end
            }
            case 's':
            {
                size_t l;
                const char* s = luaL_checklstring(L, arg, &l);
                // no precision and string is too long to be formatted, or no format necessary to begin with
                if (form[2] == '\0' || (!strchr(form, '.') && l >= 100))
                {
                    luaL_addlstring(&b, s, l);
                    continue; // skip the `luaL_addlstring' at the end
                }
                else
                {
                    snprintf(buff, sizeof(buff), form, s);
                    break;
                }
            }
            case '*':
            {
                // %* is parsed above, so if we got here we must have %...*
                luaL_error(L, "'%%*' does not take a form");
            }
            default:
            { // also treat cases `pnLlh'
                luaL_error(L, "invalid option '%%%c' to 'format'", *(strfrmt - 1));
            }
            }
            luaL_addlstring(&b, buff, strlen(buff));
        }
    }
    luaL_pushresult(&b);
    return 1;
}

static int str_split(lua_State* L)
{
    size_t haystackLen;
    const char* haystack = luaL_checklstring(L, 1, &haystackLen);
    size_t needleLen;
    const char* needle = luaL_optlstring(L, 2, ",", &needleLen);

    if (DFFlag::LuauOptimizeStringSplit)
    {
        const char* end = haystack + haystackLen;
        const char* spanStart = haystack;
        int numMatches = 0;

        // Use of memchr/memcmp here instead of strchr/strncmp is so that we allow
        // embedded nulls to be used in either of the haystack or the needle
        // strings. Most Lua string APIs allow embedded nulls, and this should be
        // no exception.
        if (needleLen == 0)
        {
            // empty separator splits the string into individual characters, so the result size is known up front
            lua_createtable(L, int(haystackLen), 0);

            for (const char* iter = haystack; iter < end; iter++)
            {
                lua_pushlstring(L, iter, 1);
                lua_rawseti(L, -2, ++numMatches);
            }

            return 1;
        }
        else if (needleLen == 1)
        {
            // every occurrence of a single character separator is a split, so we can cheaply count them up front
            // and allocate the result table at its final size
            char sep = needle[0];

            int count = 1;
            for (const char* iter = haystack; (iter = (const char*)memchr(iter, sep, end - iter)) != NULL; iter++)
                count++;

            lua_createtable(L, count, 0);

            for (const char* found; (found = (const char*)memchr(spanStart, sep, end - spanStart)) != NULL; spanStart = found + 1)
            {
                lua_pushlstring(L, spanStart, found - spanStart);
                lua_rawseti(L, -2, ++numMatches);
            }
        }
        else
        {
            lua_createtable(L, 0, 0);

            if (needleLen <= haystackLen)
            {
                // Don't iterate the last needleLen - 1 bytes of the string - they are
                // impossible to be splits and would let us memcmp past the end of the
                // buffer.
                const char* last = end - needleLen;

                for (const char* iter = haystack; iter <= last;)
                {
                    // the first and the last characters are checked inline to avoid a memcmp call at most positions
                    if (iter[0] == needle[0] && iter[needleLen - 1] == needle[needleLen - 1] && memcmp(iter, needle, needleLen) == 0)
                    {
                        lua_pushlstring(L, spanStart, iter - spanStart);
                        lua_rawseti(L, -2, ++numMatches);

                        spanStart = iter + needleLen;
                        iter = spanStart;
                    }
                    else
                    {
                        iter++;
                    }
                }
            }
        }

        lua_pushlstring(L, spanStart, end - spanStart);
        lua_rawseti(L, -2, ++numMatches);
    }
    else
    {
        const char* begin = haystack;
        const char* end = haystack + haystackLen;
        const char* spanStart = begin;
        int numMatches = 0;

        lua_createtable(L, 0, 0);

        if (needleLen == 0)
            begin++;

        // Don't iterate the last needleLen - 1 bytes of the string - they are
        // impossible to be splits and would let us memcmp past the end of the
        // buffer.
        for (const char* iter = begin; iter <= end - needleLen; iter++)
        {
            // Use of memcmp here instead of strncmp is so that we allow embedded
            // nulls to be used in either of the haystack or the needle strings.
            // Most Lua string APIs allow embedded nulls, and this should be no
            // exception.
            if (memcmp(iter, needle, needleLen) == 0)
            {
                lua_pushinteger(L, ++numMatches);
                lua_pushlstring(L, spanStart, iter - spanStart);
                lua_settable(L, -3);

                spanStart = iter + needleLen;
                if (needleLen > 0)
                    iter += needleLen - 1;
            }
        }

        if (needleLen > 0)
        {
            lua_pushinteger(L, ++numMatches);
            lua_pushlstring(L, spanStart, end - spanStart);
            lua_settable(L, -3);
        }
    }

    return 1;
}

/*
** {======================================================
** PACK/UNPACK
** =======================================================
*/

// value used for padding
#if !defined(LUAL_PACKPADBYTE)
#define LUAL_PACKPADBYTE 0x00
#endif

// maximum size for the binary representation of an integer
#define MAXINTSIZE 16

// number of bits in a character
#define NB CHAR_BIT

// mask for one character (NB 1's)
#define MC ((1 << NB) - 1)

// internal size of integers used for pack/unpack
#define SZINT (int)sizeof(long long)

// dummy union to get native endianness
static const union
{
    int dummy;
    char little; // true iff machine is little endian
} nativeendian = {1};

// assume we need to align for double & pointers
#define MAXALIGN 8

/*
** Union for serializing floats
*/
typedef union Ftypes
{
    float f;
    double d;
    double n;
    char buff[5 * sizeof(double)]; // enough for any float type
} Ftypes;

/*
** information to pack/unpack stuff
*/
typedef struct Header
{
    lua_State* L;
    int islittle;
    int maxalign;
} Header;

/*
** options for pack/unpack
*/
typedef enum KOption
{
    Kint,       // signed integers
    Kuint,      // unsigned integers
    Kfloat,     // floating-point numbers
    Kchar,      // fixed-length strings
    Kstring,    // strings with prefixed length
    Kzstr,      // zero-terminated strings
    Kpadding,   // padding
    Kpaddalign, // padding for alignment
    Knop        // no-op (configuration or spaces)
} KOption;

/*
** Read an integer numeral from string 'fmt' or return 'df' if
** there is no numeral
*/
static int digit(int c)
{
    return '0' <= c && c <= '9';
}

static int getnum(Header* h, const char** fmt, int df)
{
    if (!digit(**fmt)) // no number?
        return df;     // return default value
    else
    {
        int a = 0;
        do
        {
            a = a * 10 + (*((*fmt)++) - '0');
        } while (digit(**fmt) && a <= (INT_MAX - 9) / 10);
        if (a > MAXSSIZE || digit(**fmt))
            luaL_error(h->L, "size specifier is too large");
        return a;
    }
}

/*
** Read an integer numeral and raises an error if it is larger
** than the maximum size for integers.
*/
static int getnumlimit(Header* h, const char** fmt, int df)
{
    int sz = getnum(h, fmt, df);
    if (sz > MAXINTSIZE || sz <= 0)
        luaL_error(h->L, "integral size (%d) out of limits [1,%d]", sz, MAXINTSIZE);
    return sz;
}

/*
** Initialize Header
*/
static void initheader(lua_State* L, Header* h)
{
    h->L = L;
    h->islittle = nativeendian.little;
    h->maxalign = 1;
}

/*
** Read and classify next option. 'size' is filled with option's size.
*/
static KOption getoption(Header* h, const char** fmt, int* size)
{
    int opt = *((*fmt)++);
    *size = 0; // default
    switch (opt)
    {
    case 'b':
        *size = 1;
        return Kint;
    case 'B':
        *size = 1;
        return Kuint;
    case 'h':
        *size = 2;
        return Kint;
    case 'H':
        *size = 2;
        return Kuint;
    case 'l':
        *size = 8;
        return Kint;
    case 'L':
        *size = 8;
        return Kuint;
    case 'j':
        *size = 4;
        return Kint;
    case 'J':
        *size = 4;
        return Kuint;
    case 'T':
        *size = 4;
        return Kuint;
    case 'f':
        *size = 4;
        return Kfloat;
    case 'd':
        *size = 8;
        return Kfloat;
    case 'n':
        *size = 8;
        return Kfloat;
    case 'i':
        *size = getnumlimit(h, fmt, 4);
        return Kint;
    case 'I':
        *size = getnumlimit(h, fmt, 4);
        return Kuint;
    case 's':
        *size = getnumlimit(h, fmt, 4);
        return Kstring;
    case 'c':
        *size = getnum(h, fmt, -1);
        if (*size == -1)
            luaL_error(h->L, "missing size for format option 'c'");
        return Kchar;
    case 'z':
        return Kzstr;
    case 'x':
        *size = 1;
        return Kpadding;
    case 'X':
        return Kpaddalign;
    case ' ':
        break;
    case '<':
        h->islittle = 1;
        break;
    case '>':
        h->islittle = 0;
        break;
    case '=':
        h->islittle = nativeendian.little;
        break;
    case '!':
        h->maxalign = getnumlimit(h, fmt, MAXALIGN);
        break;
    default:
        luaL_error(h->L, "invalid format option '%c'", opt);
    }
    return Knop;
}

/*
** Read, classify, and fill other details about the next option.
** 'psize' is filled with option's size, 'notoalign' with its
** alignment requirements.
** Local variable 'size' gets the size to be aligned. (Kpaddalign option
** always gets its full alignment, other options are limited by
** the maximum alignment ('maxalign'). Kchar option needs no alignment
** despite its size.
*/
static KOption getdetails(Header* h, size_t totalsize, const char** fmt, int* psize, int* ntoalign)
{
    KOption opt = getoption(h, fmt, psize);
    int align = *psize; // usually, alignment follows size
    if (opt == Kpaddalign)
    { // 'X' gets alignment from following option
        if (**fmt == '\0' || getoption(h, fmt, &align) == Kchar || align == 0)
            luaL_argerror(h->L, 1, "invalid next option for option 'X'");
    }
    if (align <= 1 || opt == Kchar) // need no alignment?
        *ntoalign = 0;
    else
    {
        if (align > h->maxalign) // enforce maximum alignment
            align = h->maxalign;
        if ((align & (align - 1)) != 0) // is 'align' not a power of 2?
            luaL_argerror(h->L, 1, "format asks for alignment not power of 2");
        *ntoalign = (align - (int)(totalsize & (align - 1))) & (align - 1);
    }
    return opt;
}

/*
** Pack integer 'n' with 'size' bytes and 'islittle' endianness.
** The final 'if' handles the case when 'size' is larger than
** the size of a Lua integer, correcting the extra sign-extension
** bytes if necessary (by default they would be zeros).
*/
static void packint(luaL_Strbuf* b, unsigned long long n, int islittle, int size, int neg)
{
    LUAU_ASSERT(size <= MAXINTSIZE);
    char buff[MAXINTSIZE];
    int i;
    buff[islittle ? 0 : size - 1] = (char)(n & MC); // first byte
    for (i = 1; i < size; i++)
    {
        n >>= NB;
        buff[islittle ? i : size - 1 - i] = (char)(n & MC);
    }
    if (neg && size > SZINT)
    {                                  // negative number need sign extension?
        for (i = SZINT; i < size; i++) // correct extra bytes
            buff[islittle ? i : size - 1 - i] = (char)MC;
    }
    luaL_addlstring(b, buff, size); // add result to buffer
}

/*
** Copy 'size' bytes from 'src' to 'dest', correcting endianness if
** given 'islittle' is different from native endianness.
*/
static void copywithendian(volatile char* dest, volatile const char* src, int size, int islittle)
{
    if (islittle == nativeendian.little)
    {
        while (size-- != 0)
            *(dest++) = *(src++);
    }
    else
    {
        dest += size - 1;
        while (size-- != 0)
            *(dest--) = *(src++);
    }
}

static int str_pack(lua_State* L)
{
    luaL_Strbuf b;
    Header h;
    const char* fmt = luaL_checkstring(L, 1); // format string
    int arg = 1;                              // current argument to pack
    size_t totalsize = 0;                     // accumulate total size of result
    initheader(L, &h);
    lua_pushnil(L); // mark to separate arguments from string buffer
    luaL_buffinit(L, &b);
    while (*fmt != '\0')
    {
        int size, ntoalign;
        KOption opt = getdetails(&h, totalsize, &fmt, &size, &ntoalign);
        totalsize += ntoalign + size;
        while (ntoalign-- > 0)
            luaL_addchar(&b, LUAL_PACKPADBYTE); // fill alignment
        arg++;
        switch (opt)
        {
        case Kint:
        { // signed integers
            long long n = (long long)luaL_checknumber(L, arg);
            if (size < SZINT)
            { // need overflow check?
                long long lim = (long long)1 << ((size * NB) - 1);
                luaL_argcheck(L, -lim <= n && n < lim, arg, "integer overflow");
            }
            packint(&b, n, h.islittle, size, (n < 0));
            break;
        }
        case Kuint:
        { // unsigned integers
            long long n = (long long)luaL_checknumber(L, arg);
            if (size < SZINT) // need overflow check?
                luaL_argcheck(L, (unsigned long long)n < ((unsigned long long)1 << (size * NB)), arg, "unsigned overflow");
            packint(&b, (unsigned long long)n, h.islittle, size, 0);
            break;
        }
        case Kfloat:
        { // floating-point options
            volatile Ftypes u;
            char buff[MAXINTSIZE];
            double n = luaL_checknumber(L, arg); // get argument
            if (size == sizeof(u.f))
                u.f = (float)n; // copy it into 'u'
            else if (size == sizeof(u.d))
                u.d = (double)n;
            else
                u.n = n;
            // move 'u' to final result, correcting endianness if needed
            copywithendian(buff, u.buff, size, h.islittle);
            luaL_addlstring(&b, buff, size);
            break;
        }
        case Kchar:
        { // fixed-size string
            size_t len;
            const char* s = luaL_checklstring(L, arg, &len);
            luaL_argcheck(L, len <= (size_t)size, arg, "string longer than given size");
            luaL_addlstring(&b, s, len); // add string
            while (len++ < (size_t)size) // pad extra space
                luaL_addchar(&b, LUAL_PACKPADBYTE);
            break;
        }
        case Kstring:
        { // strings with length count
            size_t len;
            const char* s = luaL_checklstring(L, arg, &len);
            luaL_argcheck(L, size >= (int)sizeof(size_t) || len < ((size_t)1 << (size * NB)), arg, "string length does not fit in given size");
            packint(&b, len, h.islittle, size, 0); // pack length
            luaL_addlstring(&b, s, len);
            totalsize += len;
            break;
        }
        case Kzstr:
        { // zero-terminated string
            size_t len;
            const char* s = luaL_checklstring(L, arg, &len);
            luaL_argcheck(L, strlen(s) == len, arg, "string contains zeros");
            luaL_addlstring(&b, s, len);
            luaL_addchar(&b, '\0'); // add zero at the end
            totalsize += len + 1;
            break;
        }
        case Kpadding:
            luaL_addchar(&b, LUAL_PACKPADBYTE);
            LUAU_FALLTHROUGH;
        case Kpaddalign:
        case Knop:
            arg--; // undo increment
            break;
        }
    }
    luaL_pushresult(&b);
    return 1;
}

static int str_packsize(lua_State* L)
{
    Header h;
    const char* fmt = luaL_checkstring(L, 1); // format string
    int totalsize = 0;                        // accumulate total size of result
    initheader(L, &h);
    while (*fmt != '\0')
    {
        int size, ntoalign;
        KOption opt = getdetails(&h, totalsize, &fmt, &size, &ntoalign);
        luaL_argcheck(L, opt != Kstring && opt != Kzstr, 1, "variable-length format");
        size += ntoalign; // total space used by option
        luaL_argcheck(L, totalsize <= MAXSSIZE - size, 1, "format result too large");
        totalsize += size;
    }
    lua_pushinteger(L, totalsize);
    return 1;
}

/*
** Unpack an integer with 'size' bytes and 'islittle' endianness.
** If size is smaller than the size of a Lua integer and integer
** is signed, must do sign extension (propagating the sign to the
** higher bits); if size is larger than the size of a Lua integer,
** it must check the unread bytes to see whether they do not cause an
** overflow.
*/
static long long unpackint(lua_State* L, const char* str, int islittle, int size, int issigned)
{
    unsigned long long res = 0;
    int i;
    int limit = (size <= SZINT) ? size : SZINT;
    for (i = limit - 1; i >= 0; i--)
    {
        res <<= NB;
        res |= (unsigned char)str[islittle ? i : size - 1 - i];
    }
    if (size < SZINT)
    { // real size smaller than int?
        if (issigned)
        { // needs sign extension?
            unsigned long long mask = (unsigned long long)1 << (size * NB - 1);
            res = ((res ^ mask) - mask); // do sign extension
        }
    }
    else if (size > SZINT)
    { // must check unread bytes
        int mask = (!issigned || (long long)res >= 0) ? 0 : MC;
        for (i = limit; i < size; i++)
        {
            if ((unsigned char)str[islittle ? i : size - 1 - i] != mask)
                luaL_error(L, "%d-byte integer does not fit into Lua Integer", size);
        }
    }
    return (long long)res;
}

static int str_unpack(lua_State* L)
{
    Header h;
    const char* fmt = luaL_checkstring(L, 1);
    size_t ld;
    const char* data = luaL_checklstring(L, 2, &ld);
    int pos = posrelat(luaL_optinteger(L, 3, 1), ld) - 1;
    if (pos < 0)
        pos = 0;
    int n = 0; // number of results
    luaL_argcheck(L, size_t(pos) <= ld, 3, "initial position out of string");
    initheader(L, &h);
    while (*fmt != '\0')
    {
        int size, ntoalign;
        KOption opt = getdetails(&h, pos, &fmt, &size, &ntoalign);
        luaL_argcheck(L, (size_t)ntoalign + size <= ld - pos, 2, "data string too short");
        pos += ntoalign; // skip alignment
        // stack space for item + next position
        luaL_checkstack(L, 2, "too many results");
        n++;
        switch (opt)
        {
        case Kint:
        {
            long long res = unpackint(L, data + pos, h.islittle, size, true);
            lua_pushnumber(L, (double)res);
            break;
        }
        case Kuint:
        {
            unsigned long long res = unpackint(L, data + pos, h.islittle, size, false);
            lua_pushnumber(L, (double)res);
            break;
        }
        case Kfloat:
        {
            volatile Ftypes u;
            double num;
            copywithendian(u.buff, data + pos, size, h.islittle);
            if (size == sizeof(u.f))
                num = (double)u.f;
            else if (size == sizeof(u.d))
                num = (double)u.d;
            else
                num = u.n;
            lua_pushnumber(L, num);
            break;
        }
        case Kchar:
        {
            lua_pushlstring(L, data + pos, size);
            break;
        }
        case Kstring:
        {
            size_t len = (size_t)unpackint(L, data + pos, h.islittle, size, 0);
            luaL_argcheck(L, len <= ld - pos - size, 2, "data string too short");
            lua_pushlstring(L, data + pos + size, len);
            pos += (int)len; // skip string
            break;
        }
        case Kzstr:
        {
            size_t len = strlen(data + pos);
            luaL_argcheck(L, pos + len < ld, 2, "unfinished string for format 'z'");
            lua_pushlstring(L, data + pos, len);
            pos += (int)len + 1; // skip string plus final '\0'
            break;
        }
        case Kpaddalign:
        case Kpadding:
        case Knop:
            n--; // undo increment
            break;
        }
        pos += size;
    }
    lua_pushinteger(L, pos + 1); // next position
    return n + 1;
}

// }======================================================

static const luaL_Reg strlib[] = {
    {"byte", str_byte},
    {"char", str_char},
    {"find", str_find},
    {"format", str_format},
    {"gmatch", gmatch},
    {"gsub", str_gsub},
    {"len", str_len},
    {"lower", str_lower},
    {"match", str_match},
    {"rep", str_rep},
    {"reverse", str_reverse},
    {"sub", str_sub},
    {"upper", str_upper},
    {"split", str_split},
    {"pack", str_pack},
    {"packsize", str_packsize},
    {"unpack", str_unpack},
    {NULL, NULL},
};

static void createmetatable(lua_State* L)
{
    lua_createtable(L, 0, 1); // create metatable for strings
    lua_pushliteral(L, "");   // dummy string
    lua_pushvalue(L, -2);
    lua_setmetatable(L, -2);        // set string metatable
    lua_pop(L, 1);                  // pop dummy string
    lua_pushvalue(L, -2);           // string library...
    lua_setfield(L, -2, "__index"); // ...is the __index metamethod
    lua_pop(L, 1);                  // pop metatable
}

/*
** Open string library
*/
int luaopen_string(lua_State* L)
{
    luaL_register(L, LUA_STRLIBNAME, strlib);
    createmetatable(L);

    return 1;
}
