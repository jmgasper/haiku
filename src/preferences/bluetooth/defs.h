#ifndef DEFS_H_
#define DEFS_H_

#include <bluetooth/LocalDevice.h>

#include <bluetoothserver_p.h>


#define SET_VISIBLE 		'sVis'
#define SET_DISCOVERABLE 	'sDis'
#define SET_AUTHENTICATION 	'sAth'

// Main window
static const uint32 kMsgStartServices = 'SrSR';
static const uint32 kMsgStopServices = 'StST';
static const uint32 kMsgRestartServices = 'SrRS';
static const uint32 kMsgRefresh = 'rFLd';
static const uint32 kMsgShowAdvanced = 'sAdv';
static const uint32 kMsgAdapterSelected = 'adSl';
static const uint32 kMsgPairedSelected = 'pdSl';
static const uint32 kMsgNearbySelected = 'nbSl';
static const uint32 kMsgNearbyInvoked = 'nbIv';
static const uint32 kMsgConnectPaired = 'pdCn';
static const uint32 kMsgDisconnectPaired = 'pdDc';
static const uint32 kMsgRemovePaired = 'pdRm';
static const uint32 kMsgConnectNearby = 'nbCn';
static const uint32 kMsgShowUnnamed = 'shUn';
static const uint32 kMsgShowLog = 'shLg';
static const uint32 kMsgDismissStatus = 'dsSt';
static const uint32 kMsgTick = 'tick';
static const uint32 kMsgServerLaunched = 'svLn';
static const uint32 kMsgLocalNameChanged = 'lnCh';
static const uint32 kMsgServiceHookDone = 'hkDn';

// Worker results
static const uint32 kMsgAdaptersProbed = 'adPr';
static const uint32 kMsgPairedLoaded = 'pdLd';
static const uint32 kMsgNameResult = 'nmRs';
static const uint32 kMsgClassicConnectSent = 'ccSt';
static const uint32 kMsgLEPairBusy = 'lePB';
static const uint32 kMsgLEPairDone = 'lePD';

// Advanced settings
static const int32 kMsgSetConnectionPolicy = 'sCpo';
static const int32 kMsgSetDeviceClass = 'sDC0';
static const int32 kMsgSetFriendlyName = 'sFnm';
static const uint32 kMsgSetLogLevel = 'sLgL';
static const uint32 kMsgLoadLocalDevice = 'ldLD';

// The adapter the main window works with; the advanced settings use it too.
extern LocalDevice* ActiveLocalDevice;

#endif
