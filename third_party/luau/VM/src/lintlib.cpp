// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "lualib.h"

#include "lcommon.h"
#include "lnumutils.h"
#include "lobject.h"

#include <climits>
#include <cmath>

#ifdef _MSC_VER
#include <intrin.h>
#endif

LUAU_FASTFLAGVARIABLE(LuauIntegerLibrary)

#define mask64(w) (0xFFFFFFFFFFFFFFFFULL >> (64 - (w)))

static int int64_create(lua_State* L)
{
    double x = luaL_checknumber(L, 1);
    if (x >= -9223372036854775808.0 && x < 9223372036854775808.0)
    {
        int64_t l = (int64_t)x;
        if (((double)l) == x)
        {
            lua_pushinteger64(L, l);
            return 1;
        }
    }

    lua_pushnil(L);

    return 1;
}

static int int64_fromstring(lua_State* L)
{
    const char* s = luaL_checkstring(L, 1);
    int base = luaL_optinteger(L, 2, 10);
    luaL_argcheck(L, 2 <= base && base <= 36, 2, "base out of range");

    int64_t result;
    if (luaO_str2l(s, &result, base))
        lua_pushinteger64(L, result);
    else
        lua_pushnil(L);

    return 1;
}

static int int64_tonumber(lua_State* L)
{
    int64_t x = luaL_checkinteger64(L, 1);

    lua_pushnumber(L, (double)x);

    return 1;
}

static int int64_neg(lua_State* L)
{
    int64_t x = luaL_checkinteger64(L, 1);

    lua_pushinteger64(L, (int64_t)(~(uint64_t)x + 1));

    return 1;
}

static int int64_add(lua_State* L)
{
    int64_t x = luaL_checkinteger64(L, 1);
    int64_t y = luaL_checkinteger64(L, 2);

    lua_pushinteger64(L, (int64_t)((uint64_t)x + (uint64_t)y));

    return 1;
}

static int int64_sub(lua_State* L)
{
    int64_t x = luaL_checkinteger64(L, 1);
    int64_t y = luaL_checkinteger64(L, 2);

    lua_pushinteger64(L, (int64_t)((uint64_t)x - (uint64_t)y));

    return 1;
}

static int int64_mul(lua_State* L)
{
    int64_t x = luaL_checkinteger64(L, 1);
    int64_t y = luaL_checkinteger64(L, 2);

    lua_pushinteger64(L, (int64_t)((uint64_t)x * (uint64_t)y));

    return 1;
}

static int int64_div(lua_State* L)
{
    int64_t a = luaL_checkinteger64(L, 1);
    int64_t b = luaL_checkinteger64(L, 2);

    if (b == 0)
        luaL_error(L, "division by zero");
    if ((a == LLONG_MIN) && (b == -1))
        luaL_error(L, "integer overflow");

    lua_pushinteger64(L, a / b);

    return 1;
}

static int int64_idiv(lua_State* L)
{
    int64_t a = luaL_checkinteger64(L, 1);
    int64_t b = luaL_checkinteger64(L, 2);

    if (b == 0)
        luaL_error(L, "division by zero");
    if ((a == LLONG_MIN) && (b == -1))
        luaL_error(L, "integer overflow");

    int64_t result = a / b;
    // Floored division rounds toward -inf: adjust the truncated quotient down by 1
    // when the operands have opposite signs and the division is inexact.
    if (((a ^ b) < 0) && (a % b))
        result -= 1;
    lua_pushinteger64(L, result);

    return 1;
}

static int int64_rem(lua_State* L)
{
    int64_t a = luaL_checkinteger64(L, 1);
    int64_t b = luaL_checkinteger64(L, 2);

    if (b == 0)
        luaL_error(L, "division by zero");

    if ((a == LLONG_MIN) && (b == -1))
    {
        lua_pushinteger64(L, 0);
        return 1;
    }

    lua_pushinteger64(L, a % b);

    return 1;
}

static int int64_mod(lua_State* L)
{
    int64_t a = luaL_checkinteger64(L, 1);
    int64_t b = luaL_checkinteger64(L, 2);

    if (b == 0)
        luaL_error(L, "division by zero");

    int64_t remainder = 0;
    if ((a != LLONG_MIN) || (b != -1))
    {
        remainder = a % b;
        if (remainder && ((a < 0) != (b < 0)))
            remainder += b;
    }

    lua_pushinteger64(L, remainder);

    return 1;
}

static int int64_udiv(lua_State* L)
{
    uint64_t a = luaL_checkinteger64(L, 1);
    uint64_t b = luaL_checkinteger64(L, 2);

    if (b == 0)
        luaL_error(L, "division by zero");

    lua_pushinteger64(L, a / b);

    return 1;
}

static int int64_urem(lua_State* L)
{
    uint64_t a = luaL_checkinteger64(L, 1);
    uint64_t b = luaL_checkinteger64(L, 2);

    if (b == 0)
        luaL_error(L, "division by zero");

    lua_pushinteger64(L, a % b);

    return 1;
}

static int int64_min(lua_State* L)
{
    int64_t tmin = luaL_checkinteger64(L, 1);
    int n = lua_gettop(L);
    for (int i = 2; i <= n; i++)
    {
        int64_t x = luaL_checkinteger64(L, i);
        if (x < tmin)
            tmin = x;
    }

    lua_pushinteger64(L, tmin);

    return 1;
}

static int int64_max(lua_State* L)
{
    int64_t tmax = luaL_checkinteger64(L, 1);
    int n = lua_gettop(L);
    for (int i = 2; i <= n; i++)
    {
        int64_t x = luaL_checkinteger64(L, i);
        if (x > tmax)
            tmax = x;
    }

    lua_pushinteger64(L, tmax);

    return 1;
}

static int int64_band(lua_State* L)
{
    uint64_t tres = ULLONG_MAX;
    int n = lua_gettop(L);

    for (int i = 1; i <= n; i++)
    {
        uint64_t x = (uint64_t)luaL_checkinteger64(L, i);
        tres &= x;
    }

    lua_pushinteger64(L, tres);

    return 1;
}

static int int64_bor(lua_State* L)
{
    uint64_t tres = 0;
    int n = lua_gettop(L);

    for (int i = 1; i <= n; i++)
    {
        uint64_t x = (uint64_t)luaL_checkinteger64(L, i);
        tres |= x;
    }

    lua_pushinteger64(L, tres);

    return 1;
}

static int int64_bnot(lua_State* L)
{
    uint64_t a = luaL_checkinteger64(L, 1);

    lua_pushinteger64(L, ~a);

    return 1;
}

static int int64_bxor(lua_State* L)
{
    uint64_t tres = 0;
    int n = lua_gettop(L);

    for (int i = 1; i <= n; i++)
    {
        uint64_t x = (uint64_t)luaL_checkinteger64(L, i);
        tres ^= x;
    }

    lua_pushinteger64(L, tres);

    return 1;
}

static int int64_lt(lua_State* L)
{
    int64_t a = luaL_checkinteger64(L, 1);
    int64_t b = luaL_checkinteger64(L, 2);

    lua_pushboolean(L, a < b);

    return 1;
}

static int int64_le(lua_State* L)
{
    int64_t a = luaL_checkinteger64(L, 1);
    int64_t b = luaL_checkinteger64(L, 2);

    lua_pushboolean(L, a <= b);

    return 1;
}

static int int64_ult(lua_State* L)
{
    uint64_t a = luaL_checkinteger64(L, 1);
    uint64_t b = luaL_checkinteger64(L, 2);

    lua_pushboolean(L, a < b);

    return 1;
}

static int int64_ule(lua_State* L)
{
    uint64_t a = luaL_checkinteger64(L, 1);
    uint64_t b = luaL_checkinteger64(L, 2);

    lua_pushboolean(L, a <= b);

    return 1;
}

static int int64_gt(lua_State* L)
{
    int64_t a = luaL_checkinteger64(L, 1);
    int64_t b = luaL_checkinteger64(L, 2);

    lua_pushboolean(L, a > b);

    return 1;
}

static int int64_ge(lua_State* L)
{
    int64_t a = luaL_checkinteger64(L, 1);
    int64_t b = luaL_checkinteger64(L, 2);

    lua_pushboolean(L, a >= b);

    return 1;
}

static int int64_ugt(lua_State* L)
{
    uint64_t a = luaL_checkinteger64(L, 1);
    uint64_t b = luaL_checkinteger64(L, 2);

    lua_pushboolean(L, a > b);

    return 1;
}

static int int64_uge(lua_State* L)
{
    uint64_t a = luaL_checkinteger64(L, 1);
    uint64_t b = luaL_checkinteger64(L, 2);

    lua_pushboolean(L, a >= b);

    return 1;
}

static int int64_lshift(lua_State* L)
{
    uint64_t n = luaL_checkinteger64(L, 1);
    int64_t i = luaL_checkinteger64(L, 2);

    if ((i >= -63) && (i <= 63))
        lua_pushinteger64(L, (i < 0) ? (n >> (-i)) : (n << i));
    else
        lua_pushinteger64(L, 0);

    return 1;
}

static int int64_rshift(lua_State* L)
{
    uint64_t n = luaL_checkinteger64(L, 1);
    int64_t i = luaL_checkinteger64(L, 2);

    if ((i >= -63) && (i <= 63))
        lua_pushinteger64(L, (i < 0) ? (n << (-i)) : (n >> i));
    else
        lua_pushinteger64(L, 0);

    return 1;
}

static int int64_arshift(lua_State* L)
{
    int64_t n = luaL_checkinteger64(L, 1);
    int64_t i = luaL_checkinteger64(L, 2);

    if ((i >= -63) && (i <= 63))
        lua_pushinteger64(L, (i < 0) ? (int64_t)((uint64_t)n << (-i)) : (n >> i));
    else if (i < -63)
        lua_pushinteger64(L, 0);
    else
        lua_pushinteger64(L, (n < 0) ? -1 : 0);

    return 1;
}

static int int64_lrotate(lua_State* L)
{
    uint64_t n = (uint64_t)luaL_checkinteger64(L, 1);
    unsigned s = (unsigned)((uint64_t)luaL_checkinteger64(L, 2) % 64);

    lua_pushinteger64(L, (int64_t)(s != 0 ? (n << s) | (n >> (64 - s)) : n));

    return 1;
}

static int int64_rrotate(lua_State* L)
{
    uint64_t n = (uint64_t)luaL_checkinteger64(L, 1);
    unsigned s = (unsigned)((uint64_t)luaL_checkinteger64(L, 2) % 64);

    lua_pushinteger64(L, (int64_t)(s != 0 ? (n >> s) | (n << (64 - s)) : n));

    return 1;
}

static int int64_extract(lua_State* L)
{
    int64_t n = luaL_checkinteger64(L, 1);
    int64_t f = luaL_checkinteger64(L, 2);
    int64_t w = luaL_optinteger64(L, 3, 1);

    luaL_argcheck(L, 0 <= f && f <= 63, 2, "field cannot be negative");
    luaL_argcheck(L, 0 < w, 3, "width must be positive");
    if (f + w > 64)
        luaL_error(L, "trying to access non-existent bits");

    lua_pushinteger64(L, ((uint64_t)n >> f) & mask64(w));

    return 1;
}

static int int64_replace(lua_State* L)
{
    int64_t n = luaL_checkinteger64(L, 1);
    int64_t r = luaL_checkinteger64(L, 2);
    int64_t f = luaL_checkinteger64(L, 3);
    int64_t w = luaL_optinteger64(L, 4, 1);

    luaL_argcheck(L, 0 <= f && f <= 63, 3, "field cannot be negative");
    luaL_argcheck(L, 0 < w, 4, "width must be positive");
    if (f + w > 64)
        luaL_error(L, "trying to access non-existent bits");

    uint64_t baseMask = ((0xFFFFFFFFFFFFFFFFULL) >> (64 - w));
    uint64_t replacement = (((uint64_t)r) & baseMask) << f;
    uint64_t mask = 0xFFFFFFFFFFFFFFFFULL ^ (baseMask << f);
    lua_pushinteger64(L, (((uint64_t)n) & mask) | replacement);

    return 1;
}

static int int64_clamp(lua_State* L)
{
    int64_t a = luaL_checkinteger64(L, 1);
    int64_t mi = luaL_checkinteger64(L, 2);
    int64_t mx = luaL_checkinteger64(L, 3);

    luaL_argcheck(L, mi <= mx, 3, "max must be greater than or equal to min");

    if (a < mi)
        lua_pushinteger64(L, mi);
    else if (a > mx)
        lua_pushinteger64(L, mx);
    else
        lua_pushinteger64(L, a);

    return 1;
}

static int int64_btest(lua_State* L)
{
    uint64_t tres = ULLONG_MAX;
    int n = lua_gettop(L);

    for (int i = 1; i <= n; i++)
    {
        uint64_t x = (uint64_t)luaL_checkinteger64(L, i);
        tres &= x;
    }

    lua_pushboolean(L, (tres != 0));

    return 1;
}

static int int64_countrz(lua_State* L)
{
    uint64_t n = luaL_checkinteger64(L, 1);
    int result;
#ifdef _MSC_VER
#ifdef _WIN64
    unsigned long rl;
    result = _BitScanForward64(&rl, n) ? int(rl) : 64;
#else
    unsigned long rl;
    if (_BitScanForward(&rl, uint32_t(n)))
        result = int(rl);
    else
        result = _BitScanForward(&rl, uint32_t(n >> 32)) ? int(rl) + 32 : 64;
#endif
#else
    result = (n == 0) ? 64 : __builtin_ctzll(n);
#endif

    lua_pushinteger64(L, result);

    return 1;
}

static int int64_countlz(lua_State* L)
{
    uint64_t n = luaL_checkinteger64(L, 1);
    int result;
#ifdef _MSC_VER
#ifdef _WIN64
    unsigned long rl;
    result = _BitScanReverse64(&rl, n) ? 63 - int(rl) : 64;
#else
    unsigned long rl;
    if (_BitScanReverse(&rl, uint32_t(n >> 32)))
        result = 31 - int(rl);
    else
        result = _BitScanReverse(&rl, uint32_t(n)) ? 63 - int(rl) : 64;
#endif
#else
    result = (n == 0) ? 64 : __builtin_clzll(n);
#endif
    lua_pushinteger64(L, result);

    return 1;
}

static int int64_bswap(lua_State* L)
{
    uint64_t a = luaL_checkinteger64(L, 1);

    lua_pushinteger64(
        L,
        (a >> 56) | ((a & 0xFF000000000000) >> 40) | ((a & 0xFF0000000000) >> 24) | ((a & 0xFF00000000) >> 8) | ((a & 0xFF000000) << 8) |
            ((a & 0xFF0000) << 24) | ((a & 0xFF00) << 40) | ((a & 0xFF) << 56)
    );

    return 1;
}

static const luaL_Reg int64lib[] = {
    {"create", int64_create},
    {"tonumber", int64_tonumber},
    {"neg", int64_neg},
    {"add", int64_add},
    {"sub", int64_sub},
    {"mul", int64_mul},
    {"div", int64_div},
    {"min", int64_min},
    {"max", int64_max},
    {"rem", int64_rem},
    {"idiv", int64_idiv},
    {"udiv", int64_udiv},
    {"urem", int64_urem},
    {"mod", int64_mod},
    {"clamp", int64_clamp},
    {"band", int64_band},
    {"bor", int64_bor},
    {"bnot", int64_bnot},
    {"bxor", int64_bxor},
    {"lt", int64_lt},
    {"le", int64_le},
    {"ult", int64_ult},
    {"ule", int64_ule},
    {"gt", int64_gt},
    {"ge", int64_ge},
    {"ugt", int64_ugt},
    {"uge", int64_uge},
    {"lshift", int64_lshift},
    {"rshift", int64_rshift},
    {"arshift", int64_arshift},
    {"lrotate", int64_lrotate},
    {"rrotate", int64_rrotate},
    {"extract", int64_extract},
    {"replace", int64_replace},
    {"btest", int64_btest},
    {"countrz", int64_countrz},
    {"countlz", int64_countlz},
    {"bswap", int64_bswap},
    {"fromstring", int64_fromstring},
    {NULL, NULL},
};

int luaopen_integer(lua_State* L)
{
    luaL_register(L, LUA_INTLIBNAME, int64lib);

    lua_pushinteger64(L, LLONG_MAX);
    lua_setfield(L, -2, "maxsigned");
    lua_pushinteger64(L, LLONG_MIN);
    lua_setfield(L, -2, "minsigned");

    return 1;
}
