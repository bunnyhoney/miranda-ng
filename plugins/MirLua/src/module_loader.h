#pragma once

class CMLuaModuleLoader
{
private:
	lua_State *L;

	CMLuaModuleLoader(lua_State *L);

	void Load(const char *name, lua_CFunction loader);
	void Preload(const char *name, lua_CFunction loader);

	void LoadModules();

public:
	static void Load(lua_State *L);
};
