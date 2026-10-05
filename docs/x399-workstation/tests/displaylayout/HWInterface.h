/*
 * Stand-in for app_server's HWInterface in displaylayouttest: DisplayLayout
 * only asks it for the outputs and their modes, so that is all there is.
 */
#ifndef HW_INTERFACE_H
#define HW_INTERFACE_H


#include <Accelerant.h>


class HWInterface {
public:
	virtual						~HWInterface() {}
	virtual	status_t			GetDisplayOutputs(display_output** _outputs,
									uint32* _count) = 0;
	virtual	status_t			GetDisplayOutputModes(uint32 id,
									display_mode** _modes, uint32* _count) = 0;
};


#endif	// HW_INTERFACE_H
