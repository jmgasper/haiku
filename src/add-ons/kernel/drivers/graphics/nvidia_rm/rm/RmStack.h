#pragma once

#include <stdlib.h>

#include "nv-include.h"


/*
 * The RM core of NVIDIA's proprietary driver runs on an alternate stack
 * supplied by the caller of each RM entry point. The open RM ignores it.
 */
class RmStack {
public:
	RmStack()
	{
		if (!rm_is_altstack_in_use())
			return;

		fStack = (nvidia_stack_t*)malloc(sizeof(nvidia_stack_t));
		if (fStack != NULL) {
			fStack->size = sizeof(fStack->stack);
			fStack->top = fStack->stack + fStack->size;
		}
	}

	~RmStack()
	{
		free(fStack);
	}

	RmStack(const RmStack&) = delete;
	RmStack& operator=(const RmStack&) = delete;

	bool IsValid() const
	{
		return fStack != NULL || !rm_is_altstack_in_use();
	}

	nvidia_stack_t* Get() const
	{
		return fStack;
	}

private:
	nvidia_stack_t* fStack = NULL;
};
