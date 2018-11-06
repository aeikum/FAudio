/* map xaudio2 types to faudio types */
typedef uint32_t HRESULT;
typedef uint32_t UINT32;
typedef uint8_t BOOL;

typedef FAudio IXAudio27;
typedef FAudioDeviceDetails XAUDIO2_DEVICE_DETAILS;

#define S_OK 0

#define IXAudio27_Initialize FAudio_Initialize
#define IXAudio27_Release FAudio_Release
#define IXAudio27_GetDeviceCount FAudio_GetDeviceCount
#define IXAudio27_GetDeviceDetails FAudio_GetDeviceDetails
#define XAUDIO2_ANY_PROCESSOR FAUDIO_DEFAULT_PROCESSOR
#define GlobalDefaultDevice FAudioGlobalDefaultDevice
#define NotDefaultDevice FAudioNotDefaultDevice
