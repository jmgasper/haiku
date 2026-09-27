/*
 * displaylayouttest - app_server's DisplayLayout against a pretend
 * accelerant, through what happens to monitors between boots: arranged by
 * the user, one of them asleep or late at boot, and back again.
 *
 * The first case is the one that lost the arrangement on the workstation:
 * two monitors swapped, the left one missing for a moment. Everything an
 * automatic reconfiguration decided used to be stored, so the right monitor's
 * stand-in place at x = 0 was remembered, and once the left one came back
 * both claimed x = 0 and connector order won. The same case run the old way
 * is kept as a check that the test still sees that failure.
 *
 * Build and run: tools/build-displaylayout-test.sh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include <Message.h>
#include <edid.h>

#include "DisplayLayout.h"
#include "HWInterface.h"


extern "C" void
edid_decode(edid1_info* edid, const edid1_raw* raw)
{
	// the pretend monitors have no EDID
	memset(edid, 0, sizeof(*edid));
}


struct FakeOutput {
	uint32	id;
	char	name[16];
	bool	connected;
	bool	enabled;
	int32	x;
	int32	y;
};


static display_timing
uhd_timing()
{
	display_timing timing = {};
	timing.pixel_clock = 533250;
	timing.h_display = 3840;
	timing.h_total = 4000;
	timing.v_display = 2160;
	timing.v_total = 2222;
	return timing;
}


// Everything at 100 percent, so logical and frame buffer pixels agree.
class FakeAccelerant : public HWInterface {
public:
	void Add(uint32 id, const char* name)
	{
		FakeOutput output = {};
		output.id = id;
		strlcpy(output.name, name, sizeof(output.name));
		output.connected = true;
		fOutputs.push_back(output);
	}

	void SetConnected(uint32 id, bool connected)
	{
		for (FakeOutput& output : fOutputs) {
			if (output.id == id) {
				output.connected = connected;
				if (!connected)
					output.enabled = false;
			}
		}
	}

	status_t GetDisplayOutputs(display_output** _outputs, uint32* _count)
		override
	{
		display_output* outputs = (display_output*)calloc(fOutputs.size(),
			sizeof(display_output));
		for (size_t i = 0; i < fOutputs.size(); i++) {
			const FakeOutput& fake = fOutputs[i];
			display_output& output = outputs[i];
			output.id = fake.id;
			strlcpy(output.name, fake.name, sizeof(output.name));
			output.flags = (fake.connected ? B_DISPLAY_OUTPUT_CONNECTED : 0)
				| (fake.enabled ? B_DISPLAY_OUTPUT_ENABLED : 0);
			output.x = fake.x;
			output.y = fake.y;
			output.width = 3840;
			output.height = 2160;
			output.scale = 100;
			output.render_scale = 100;
			output.native_timing = uhd_timing();
			output.timing = uhd_timing();
		}
		*_outputs = outputs;
		*_count = fOutputs.size();
		return B_OK;
	}

	status_t GetDisplayOutputModes(uint32, display_mode** _modes,
		uint32* _count) override
	{
		*_modes = NULL;
		*_count = 0;
		return B_OK;
	}

	void Apply(const std::vector<display_output_config>& configs)
	{
		for (FakeOutput& output : fOutputs) {
			output.enabled = false;
			for (const display_output_config& config : configs) {
				if (config.id != output.id || !output.connected)
					continue;
				output.enabled = (config.flags & B_DISPLAY_OUTPUT_ENABLED) != 0;
				output.x = config.x;
				output.y = config.y;
			}
		}
	}

private:
	std::vector<FakeOutput> fOutputs;
};


// What Desktop::_ConfigureDisplayLayout() does at boot, on a hot plug and
// after a resume; the old version stored the result as well.
static void
configure(DisplayLayout& layout, FakeAccelerant& hardware, BMessage& saved,
	bool storeLikeBefore)
{
	layout.ReadOutputs(&hardware);
	layout.Configure(saved, false);
	std::vector<display_output_config> configs;
	layout.GetConfigs(configs);
	hardware.Apply(configs);
	layout.ReadOutputs(&hardware);
	if (storeLikeBefore)
		layout.Store(saved);
}


// What Desktop::SetDisplayLayout() does for the Screen preferences.
static void
request(DisplayLayout& layout, FakeAccelerant& hardware, BMessage& saved,
	uint32 id, float x, float y)
{
	BMessage message;
	BMessage display;
	display.AddInt32("id", id);
	display.AddPoint("origin", BPoint(x, y));
	message.AddMessage("display", &display);
	if (layout.ApplyRequest(message) != B_OK) {
		printf("  request refused\n");
		return;
	}
	std::vector<display_output_config> configs;
	layout.GetConfigs(configs);
	hardware.Apply(configs);
	layout.ReadOutputs(&hardware);
	layout.Store(saved);
}


static int sFailures = 0;


static void
expect(const DisplayLayout& layout, uint32 id, float x, float y,
	const char* what)
{
	const DisplayInfo* display = layout.DisplayByID(id);
	if (display == NULL || !display->IsEnabled()) {
		printf("  FAIL %s: display %" B_PRIu32 " is not on\n", what, id);
		sFailures++;
		return;
	}
	bool ok = display->frame.left == x && display->frame.top == y;
	printf("  %s %s: %s at %g,%g (want %g,%g)\n", ok ? "ok  " : "FAIL", what,
		display->name.String(), display->frame.left, display->frame.top, x, y);
	if (!ok)
		sFailures++;
}


static void
swapped_pair(bool storeLikeBefore)
{
	printf("%s: two monitors swapped, the left one missing for a while\n",
		storeLikeBefore ? "old behaviour" : "swap");
	FakeAccelerant hardware;
	hardware.Add(1, "DP-2");
	hardware.Add(2, "DP-4");
	DisplayLayout layout;
	BMessage saved;

	configure(layout, hardware, saved, storeLikeBefore);
	expect(layout, 1, 0, 0, "first boot, connector order");
	expect(layout, 2, 3840, 0, "first boot, connector order");

	// dragging DP-4 onto DP-2 pushes DP-2 aside
	request(layout, hardware, saved, 2, 0, 0);
	expect(layout, 2, 0, 0, "swapped");
	expect(layout, 1, 3840, 0, "swapped");

	// DP-4 asleep, or not answering yet at boot
	hardware.SetConnected(2, false);
	configure(layout, hardware, saved, storeLikeBefore);
	expect(layout, 1, 0, 0, "left monitor gone");

	hardware.SetConnected(2, true);
	configure(layout, hardware, saved, storeLikeBefore);
	if (storeLikeBefore) {
		const DisplayInfo* left = layout.DisplayByID(1);
		bool lost = left != NULL && left->frame.left == 0;
		printf("  %s old behaviour loses the swap\n", lost ? "ok  " : "FAIL");
		if (!lost)
			sFailures++;
		return;
	}
	expect(layout, 2, 0, 0, "left monitor back");
	expect(layout, 1, 3840, 0, "left monitor back");

	// and a boot where the file is read afresh
	DisplayLayout booted;
	configure(booted, hardware, saved, false);
	expect(booted, 2, 0, 0, "after a reboot");
	expect(booted, 1, 3840, 0, "after a reboot");
}


static void
row_of_three()
{
	printf("row: three monitors side by side, the middle one missing\n");
	FakeAccelerant hardware;
	hardware.Add(1, "DP-0");
	hardware.Add(2, "DP-2");
	hardware.Add(3, "DP-4");
	DisplayLayout layout;
	BMessage saved;
	configure(layout, hardware, saved, false);
	// store the arrangement the way the user would
	request(layout, hardware, saved, 1, 0, 0);
	expect(layout, 3, 7680, 0, "all three");

	hardware.SetConnected(2, false);
	configure(layout, hardware, saved, false);
	expect(layout, 1, 0, 0, "middle missing");
	expect(layout, 3, 3840, 0, "middle missing, closed up");

	hardware.SetConnected(2, true);
	configure(layout, hardware, saved, false);
	expect(layout, 2, 3840, 0, "middle back");
	expect(layout, 3, 7680, 0, "middle back");
}


static void
column_of_three()
{
	printf("column: three monitors stacked, the middle one missing\n");
	FakeAccelerant hardware;
	hardware.Add(1, "DP-0");
	hardware.Add(2, "DP-2");
	hardware.Add(3, "DP-4");
	DisplayLayout layout;
	BMessage saved;
	configure(layout, hardware, saved, false);
	request(layout, hardware, saved, 2, 0, 2160);
	request(layout, hardware, saved, 3, 0, 4320);
	expect(layout, 1, 0, 0, "stacked");
	expect(layout, 2, 0, 2160, "stacked");
	expect(layout, 3, 0, 4320, "stacked");

	hardware.SetConnected(2, false);
	configure(layout, hardware, saved, false);
	expect(layout, 1, 0, 0, "middle missing");
	expect(layout, 3, 0, 2160, "middle missing, closed up");

	hardware.SetConnected(2, true);
	configure(layout, hardware, saved, false);
	expect(layout, 2, 0, 2160, "middle back");
	expect(layout, 3, 0, 4320, "middle back");
}


int
main()
{
	swapped_pair(false);
	swapped_pair(true);
	row_of_three();
	column_of_three();
	printf(sFailures == 0 ? "all passed\n" : "%d FAILED\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
