// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#pragma once

#include "lmem.h"
#include "lobject.h"

/**
 * Allocate and return a new class object.
 * @param name The name of this class. This does not have to be unique within a program.
 * @param memberstooffset A table mapping member names to their offset within the class
 * @param offsettomember An array of length `numberofinstancemembers + numberofstaticmembers` where
 * each entry is the name of the member at the specified offset.
 * @param numberofinstancemembers The number of instance members (fields) this class has.
 * @param numberofstaticmembers The number of static members (only methods today) this class has.
 */
LUAI_FUNC LuauClass* luaR_newclass(
    lua_State* L,
    TString* name,
    LuaTable* memberstooffset,
    TString** offsettomember,
    uint32_t numberofinstancemembers,
    uint32_t numberofstaticmembers,
    LuaTable* envt
);

/**
 * Allocates and returns a new class object with the same members as `classobject`.
 * @param classobject The class object to clone.
 */
LUAI_FUNC LuauClass* luaR_cloneclass(lua_State* L, LuauClass* classobject);

/**
 * Returns a new LuauClass object containing `child`'s members extended with `parent`'s.
 */
LUAI_FUNC void luaR_inheritclass(lua_State* L, LuauClass* child, LuauClass* parent);

/**
 * Add a new class member to `classobject` named `name` and with value `value`.
 */
LUAI_FUNC void luaR_addclassmember(lua_State* L, LuauClass* classobject, TString* name, TValue* value);

LUAI_FUNC void luaR_freeclass(lua_State* L, LuauClass* classobject, lua_Page* page);

/**
 * The default constructor for class instances. This is written as a Lua API
 * function and expects the stack to have a single optional indexable value.
 *
 * This function will allocate a new class instance, iterate over the instance
 * members of the class object, initialize each class instance member with the
 * result of indexing into the parameter, and then assign the value to the top
 * of the stack. Must be called with two arguments: the class instance to
 * populate and the table to populate it from.
 */
LUAI_FUNC int luaR_defaultcreateobject(lua_State* L);

/**
 * Construct an object, pass any stack arguments on to its __init() method, and
 * assign the new object to the top of the stack.
 */
LUAI_FUNC int luaR_constructobject(lua_State* L);

LUAI_FUNC void luaR_tryconstructobject(lua_State* L, StkId target, StkId maybeclass, uint32_t slotid);

LUAI_FUNC void luaR_freeobject(lua_State* L, LuauObject* classinstance, lua_Page* page);

#define luaR_checkoffsetinbounds(inst, offset) (offset < (inst)->lclass->numberofallmembers)

#define luaR_lookupmemberatoffset(inst, offset) \
    (LUAU_ASSERT(luaR_checkoffsetinbounds(inst, offset)), \
     offset < (inst)->lclass->numberofinstancemembers ? &(inst)->members[offset] \
                                                      : &(inst)->lclass->staticmembers[offset - inst->lclass->numberofinstancemembers])
