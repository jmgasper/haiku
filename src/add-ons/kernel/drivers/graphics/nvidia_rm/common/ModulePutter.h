// TODO

#pragma once

#include <module.h>


template<typename Type, const char *name>
class ModulePutter {
private:
	Type *fModule;

public:
	ModulePutter();
	ModulePutter(Type *module);
	~ModulePutter();

	ModulePutter &operator=(const ModulePutter &other);

	bool IsSet();
	Type *Get();
}
