// ****************************************************************
// PacDrive.h header file.
// Author: Ben Baker [headsoft.com.au]
// ****************************************************************
#ifdef PACDRIVE_EXPORTS
#define PACDRIVE_API __declspec(dllexport)
#else
#define PACDRIVE_API __declspec(dllimport)
#endif

// ****************************************************************
// PacDrive / U-HID / Blue-HID
// ****************************************************************
// - 16 LED channels

// ****************************************************************
// Nano-LED
// ****************************************************************
// - 60 LED channels
// - 256 brightness levels
// - 20 RGB LEDs

// ****************************************************************
// PacLED64
// ****************************************************************
// - 64 LED channels
// - 256 brightness levels
// - 21 RGB LEDs

// ****************************************************************
// I-Pac Ultimate I/O
// ****************************************************************
// - 96 LED channels
// - 256 brightness levels
// - 32 RGB LEDs

#define VID_ULTIMARC				0xd209	// Ultimarc Vendor Id

#define PID_PACDRIVE				0x1500	// PacDrive PID (PacDrive uses it's Version Number to id it's different devices)
#define PID_UHID_LO					0x1501	// Start of U-HID PID
#define PID_UHID_HI					0x1508  // End of U-HID PID
#define PID_NANOLED_LO				0x1481	// Start of NanoLED PID
#define PID_NANOLED_HI				0x1484	// End of NanoLED PID
#define PID_PACLED64_LO				0x1401	// Start of PacLED64 PID
#define PID_PACLED64_HI				0x1408	// End of PacLED64 PID
#define PID_IPACULTIMATEIO_LO		0x0410	// Start I-Pac Ultimate I/O PID
#define PID_IPACULTIMATEIO_HI		0x0413	// End I-Pac Ultimate I/O PID
#define PID_SERVOSTIK				0x1700  // ServoStik PID
#define PID_USBBUTTON				0x1200	// USB Button PID

#define IS_TYPE_PACDRIVE(type)		(type == DEVICETYPE_PACDRIVE || type == DEVICETYPE_UHID || type == DEVICETYPE_BLUEHID)
#define IS_TYPE_PAC64(type)			(type == DEVICETYPE_NANOLED || type == DEVICETYPE_PACLED64 || type == DEVICETYPE_IPACULTIMATEIO)

#define IS_PACDRIVE(pid)			(pid == PID_PACDRIVE)
#define IS_UHID(pid)				(pid >= PID_UHID_LO && pid <= PID_UHID_HI)
#define IS_BLUEHID(usage, usagePage, inputReportLen, outputReportLen)		(usage = 10 && usagePage == 1 && inputReportLen == 9 && outputReportLen == 9)
#define IS_NANOLED(pid)				(pid >= PID_NANOLED_LO && pid <= PID_NANOLED_HI)
#define IS_PACLED64(pid)			(pid >= PID_PACLED64_LO && pid <= PID_PACLED64_HI)
#define IS_IPACULTIMATEIO(pid)		(pid >= PID_IPACULTIMATEIO_LO && pid <= PID_IPACULTIMATEIO_HI)
#define IS_SERVOSTIK(pid)			(pid == PID_SERVOSTIK)
#define IS_USBBUTTON(pid)			(pid == PID_USBBUTTON)

#define IS_ULTIMARC(vid)			(vid == VID_ULTIMARC)

#define IS_OLDDRIVER(usagePage, usage, outputReportLen)		(usagePage == 1 && usage == 0 && outputReportLen != 0)
#define IS_NEWDRIVER(usagePage, usage, outputReportLen)		(usagePage == 6 && usage == 0 && outputReportLen == 97)

#define WINDOW_NAME		L"PacDrive Event Window"
#define WINDOW_CLASS	L"PacDrive Event Class"

static GUID GUID_DEVINTERFACE_HID =
{ 0x4D1E55B2L, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };

typedef struct
{
	UCHAR ReportID;
	UCHAR ReportBuffer[96];
} REPORT_BUF, *PREPORT_BUF;

enum DeviceType
{
	DEVICETYPE_UNKNOWN,
	DEVICETYPE_PACDRIVE,
	DEVICETYPE_UHID,
	DEVICETYPE_BLUEHID,
	DEVICETYPE_NANOLED,
	DEVICETYPE_PACLED64,
	DEVICETYPE_IPACULTIMATEIO,
	DEVICETYPE_SERVOSTIK,
	DEVICETYPE_USBBUTTON
};

typedef struct HID_DEVICE_DATA
{
	INT Type;
	HANDLE hDevice;
	USHORT VendorID;
	USHORT ProductID;
	USHORT VersionNumber;
	WCHAR VendorName[256];
	WCHAR ProductName[256];
	WCHAR SerialNumber[256];
	WCHAR DevicePath[256];
	USHORT InputReportLen;
	USHORT OutputReportLen;
	USHORT UsagePage;
	USHORT Usage;
	HID_DEVICE_DATA *pHidDeviceData;
} *PHID_DEVICE_DATA;

typedef VOID (__stdcall *USBDEVICE_ATTACHED_CALLBACK)(INT id);
typedef VOID (__stdcall *USBDEVICE_REMOVED_CALLBACK)(INT id);

PACDRIVE_API VOID __stdcall PacSetCallbacks(USBDEVICE_ATTACHED_CALLBACK usbAttachedCallback, USBDEVICE_REMOVED_CALLBACK usbRemovedCallback);

PACDRIVE_API INT __stdcall PacInitialize();
PACDRIVE_API VOID __stdcall PacShutdown();

PACDRIVE_API BOOL __stdcall PacSetLEDStates(INT id, USHORT data);
PACDRIVE_API BOOL __stdcall PacSetLEDState(INT id, INT port, BOOL state);

PACDRIVE_API BOOL __stdcall Pac64SetLEDStates(INT id, INT group, BYTE data);
PACDRIVE_API BOOL __stdcall Pac64SetLEDState(INT id, INT group, INT port, BOOL state);
PACDRIVE_API BOOL __stdcall Pac64SetLEDStatesRandom(INT id);
PACDRIVE_API BOOL __stdcall Pac64SetLEDIntensities(INT id, PBYTE data);
PACDRIVE_API BOOL __stdcall Pac64SetLEDIntensity(INT id, INT port, BYTE intensity);
PACDRIVE_API BOOL __stdcall Pac64SetLEDFadeTime(INT id, BYTE fadeTime);
PACDRIVE_API BOOL __stdcall Pac64SetLEDFlashSpeeds(INT id, BYTE flashSpeed);
PACDRIVE_API BOOL __stdcall Pac64SetLEDFlashSpeed(INT id, INT port, BYTE flashSpeed);
PACDRIVE_API BOOL __stdcall Pac64StartScriptRecording(INT id);
PACDRIVE_API BOOL __stdcall Pac64StopScriptRecording(INT id);
PACDRIVE_API BOOL __stdcall Pac64SetScriptStepDelay(INT id, BYTE stepDelay);
PACDRIVE_API BOOL __stdcall Pac64RunScript(INT id);
PACDRIVE_API BOOL __stdcall Pac64ClearFlash(INT id);
PACDRIVE_API BOOL __stdcall Pac64SetDeviceId(INT id, INT newId);
PACDRIVE_API BOOL __stdcall Pac64UpdateFirmware(INT id);

PACDRIVE_API INT __stdcall PacGetDeviceType(INT id);
PACDRIVE_API INT __stdcall PacGetVendorId(INT id);
PACDRIVE_API INT __stdcall PacGetProductId(INT id);
PACDRIVE_API INT __stdcall PacGetVersionNumber(INT id);
PACDRIVE_API VOID __stdcall PacGetVendorName(INT id, PWCHAR sVendorName);
PACDRIVE_API VOID __stdcall PacGetProductName(INT id, PWCHAR sProductName);
PACDRIVE_API VOID __stdcall PacGetSerialNumber(INT id, PWCHAR sSerialNumber);
PACDRIVE_API VOID __stdcall PacGetDevicePath(INT id, PWCHAR sDevicePath);

PACDRIVE_API BOOL __stdcall PacProgramUHid(INT id, PCHAR sFilePath);

PACDRIVE_API BOOL __stdcall PacSetServoStik4Way();
PACDRIVE_API BOOL __stdcall PacSetServoStik8Way();

PACDRIVE_API BOOL __stdcall USBButtonConfigurePermanent(INT id, PBYTE data);
PACDRIVE_API BOOL __stdcall USBButtonConfigureTemporary(INT id, PBYTE data);
PACDRIVE_API BOOL __stdcall USBButtonConfigureColor(INT id, BYTE red, BYTE green, BYTE blue);
PACDRIVE_API BOOL __stdcall USBButtonGetState(INT id, PBOOL state);

BOOL UsbOpen(LPCWSTR devicePath, HID_DEVICE_DATA *pDeviceData);
BOOL UsbRead(PHID_DEVICE_DATA pHidDeviceData, PREPORT_BUF pInputReport);
BOOL UsbRead(PHID_DEVICE_DATA pHidDeviceData, PREPORT_BUF pInputReport, DWORD timeOut);
BOOL UsbWrite(PHID_DEVICE_DATA pHidDeviceData, PREPORT_BUF pOutputReport);
BOOL UsbWrite(PHID_DEVICE_DATA pHidDeviceData, PREPORT_BUF pInputReport, DWORD timeOut);

DWORD WINAPI EventWindowThread(LPVOID lpParam);
BOOL RegisterDeviceInterface(HWND hWnd);
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

BOOL GetDeviceInfo(HANDLE hDevice, USHORT& vendorID, USHORT& productID, USHORT& versionNumber, USHORT& usage, USHORT& usagePage, USHORT& inputReportLen, USHORT& outputReportLen);

void DebugOutput(LPCTSTR lpszFormat, ...);

void strlow(wchar_t *src);
void strlow(char *s);
