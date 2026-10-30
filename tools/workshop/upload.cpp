// Updates a Civilization V Steam Workshop item with a new mod package, as ModBuddy's Publish to Steam Workshop does.
// Civ 5's items are Steam's old kind, one file each, which only the old ISteamRemoteStorage functions update; this
// calls them through the steam_api.dll that comes with ModBuddy, signed in as the Civilization V SDK.
//
// usage: upload <steam_api.dll> <item id> <package.civ5mod> <change note file> <description file>
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

typedef unsigned __int64 uint64;
typedef __int64 int64;
typedef unsigned int uint32;
typedef int int32;
typedef uint64 SteamAPICall_t;
typedef uint64 PublishedFileId_t;
typedef uint64 UpdateHandle_t;
typedef uint64 UGCHandle_t;

// The order of these functions must match STEAMREMOTESTORAGE_INTERFACE_VERSION006, which ModBuddy's steam_api.dll
// returns, up to the last one called.
class ISteamRemoteStorage006
{
public:
	virtual bool FileWrite(const char* file, const void* data, int32 size) = 0;
	virtual int32 FileRead(const char* file, void* data, int32 size) = 0;
	virtual bool FileForget(const char* file) = 0;
	virtual bool FileDelete(const char* file) = 0;
	virtual SteamAPICall_t FileShare(const char* file) = 0;
	virtual bool SetSyncPlatforms(const char* file, int platforms) = 0;
	virtual bool FileExists(const char* file) = 0;
	virtual bool FilePersisted(const char* file) = 0;
	virtual int32 GetFileSize(const char* file) = 0;
	virtual int64 GetFileTimestamp(const char* file) = 0;
	virtual int GetSyncPlatforms(const char* file) = 0;
	virtual int32 GetFileCount() = 0;
	virtual const char* GetFileNameAndSize(int file, int32* size) = 0;
	virtual bool GetQuota(int32* total, int32* available) = 0;
	virtual bool IsCloudEnabledForAccount() = 0;
	virtual bool IsCloudEnabledForApp() = 0;
	virtual void SetCloudEnabledForApp(bool enabled) = 0;
	virtual SteamAPICall_t UGCDownload(UGCHandle_t content) = 0;
	virtual bool GetUGCDownloadProgress(UGCHandle_t content, uint32* downloaded, uint32* total) = 0;
	virtual bool GetUGCDetails(UGCHandle_t content, uint32* app, char** name, int32* size, void* owner) = 0;
	virtual int32 UGCRead(UGCHandle_t content, void* data, int32 size) = 0;
	virtual int32 GetCachedUGCCount() = 0;
	virtual UGCHandle_t GetCachedUGCHandle(int32 index) = 0;
	virtual SteamAPICall_t PublishWorkshopFile(const char* file, const char* preview, uint32 consumerApp, const char* title,
		const char* description, int visibility, void* tags, int fileType) = 0;
	virtual UpdateHandle_t CreatePublishedFileUpdateRequest(PublishedFileId_t item) = 0;
	virtual bool UpdatePublishedFileFile(UpdateHandle_t update, const char* file) = 0;
	virtual bool UpdatePublishedFilePreviewFile(UpdateHandle_t update, const char* preview) = 0;
	virtual bool UpdatePublishedFileTitle(UpdateHandle_t update, const char* title) = 0;
	virtual bool UpdatePublishedFileDescription(UpdateHandle_t update, const char* description) = 0;
	virtual bool UpdatePublishedFileVisibility(UpdateHandle_t update, int visibility) = 0;
	virtual bool UpdatePublishedFileTags(UpdateHandle_t update, void* tags) = 0;
	virtual SteamAPICall_t CommitPublishedFileUpdate(UpdateHandle_t update) = 0;
	virtual SteamAPICall_t GetPublishedFileDetails(PublishedFileId_t item) = 0;
	virtual SteamAPICall_t DeletePublishedFile(PublishedFileId_t item) = 0;
	virtual SteamAPICall_t EnumerateUserPublishedFiles(uint32 start) = 0;
	virtual SteamAPICall_t SubscribePublishedFile(PublishedFileId_t item) = 0;
	virtual SteamAPICall_t EnumerateUserSubscribedFiles(uint32 start) = 0;
	virtual SteamAPICall_t UnsubscribePublishedFile(PublishedFileId_t item) = 0;
	virtual bool UpdatePublishedFileSetChangeDescription(UpdateHandle_t update, const char* note) = 0;
};

// Likewise for SteamUtils005.
class ISteamUtils005
{
public:
	virtual uint32 GetSecondsSinceAppActive() = 0;
	virtual uint32 GetSecondsSinceComputerActive() = 0;
	virtual int GetConnectedUniverse() = 0;
	virtual uint32 GetServerRealTime() = 0;
	virtual const char* GetIPCountry() = 0;
	virtual bool GetImageSize(int image, uint32* width, uint32* height) = 0;
	virtual bool GetImageRGBA(int image, unsigned char* dest, int size) = 0;
	virtual bool GetCSERIPPort(uint32* ip, unsigned short* port) = 0;
	virtual unsigned char GetCurrentBatteryPower() = 0;
	virtual uint32 GetAppID() = 0;
	virtual void SetOverlayNotificationPosition(int position) = 0;
	virtual bool IsAPICallCompleted(SteamAPICall_t call, bool* failed) = 0;
	virtual int GetAPICallFailureReason(SteamAPICall_t call) = 0;
	virtual bool GetAPICallResult(SteamAPICall_t call, void* result, int size, int expected, bool* failed) = 0;
};

namespace
{
	const char* const kSdkApp = "16830";
	const int kUpdateResult = 1316;
	const int kResultOk = 1;
	const int kResultFileNotFound = 9;
	const int kResultAccessDenied = 15;
	const int kWaitMinutes = 5;

	#pragma pack(push, 8)
	struct UpdateResult
	{
		int result;
		PublishedFileId_t item;
	};
	#pragma pack(pop)

	typedef bool (__cdecl *InitFn)();
	typedef void (__cdecl *VoidFn)();
	typedef void* (__cdecl *InterfaceFn)();

	int Fail(const char* format, ...)
	{
		va_list args;
		va_start(args, format);
		printf("upload: ");
		vprintf(format, args);
		printf("\n");
		va_end(args);
		return 1;
	}

	bool ReadBytes(const char* path, std::vector<char>* data)
	{
		FILE* f = NULL;
		if (fopen_s(&f, path, "rb") != 0 || !f)
			return false;
		fseek(f, 0, SEEK_END);
		const long size = ftell(f);
		fseek(f, 0, SEEK_SET);
		data->resize(size);
		const bool read = size == 0 || fread(&(*data)[0], 1, size, f) == (size_t)size;
		fclose(f);
		return read;
	}

	bool ReadText(const char* path, std::string* text)
	{
		std::vector<char> data;
		if (!ReadBytes(path, &data))
			return false;
		text->assign(data.begin(), data.end());
		return true;
	}

	std::string FileName(const char* path)
	{
		const char* name = path;
		for (const char* c = path; *c; ++c)
			if (*c == '\\' || *c == '/')
				name = c + 1;
		return name;
	}

	SteamAPICall_t StartUpdate(ISteamRemoteStorage006* storage, PublishedFileId_t item, const std::string& file,
		const std::string& note, const std::string& description)
	{
		const UpdateHandle_t update = storage->CreatePublishedFileUpdateRequest(item);
		if (!storage->UpdatePublishedFileFile(update, file.c_str()) || !storage->UpdatePublishedFileSetChangeDescription(update, note.c_str())
			|| !storage->UpdatePublishedFileDescription(update, description.c_str()))
			return 0;
		return storage->CommitPublishedFileUpdate(update);
	}

	bool WaitForCall(ISteamUtils005* utils, VoidFn runCallbacks, SteamAPICall_t call)
	{
		bool failed = false;
		for (int i = 0; i < kWaitMinutes * 60 * 10; ++i)
		{
			runCallbacks();
			if (utils->IsAPICallCompleted(call, &failed))
				return true;
			Sleep(100);
		}
		return false;
	}

	int CheckResult(ISteamUtils005* utils, SteamAPICall_t call, PublishedFileId_t item, const std::string& file)
	{
		UpdateResult result;
		memset(&result, 0, sizeof(result));
		bool failed = false;
		if (!utils->GetAPICallResult(call, &result, sizeof(result), kUpdateResult, &failed) || failed)
			return Fail("Steam's answer to the update could not be read.");
		// Steam answers an item of another account as not found.
		if (result.result == kResultFileNotFound || result.result == kResultAccessDenied)
			return Fail("Steam refused the update with result %d: is Steam signed in to the account that owns the item?",
				result.result);
		if (result.result != kResultOk)
			return Fail("Steam refused the update with result %d.", result.result);
		printf("upload: item %I64u now holds %s.\n", item, file.c_str());
		return 0;
	}

	int Upload(ISteamRemoteStorage006* storage, ISteamUtils005* utils, VoidFn runCallbacks, PublishedFileId_t item,
		const std::string& file, const std::vector<char>& package, const std::string& note, const std::string& description)
	{
		if (!storage->FileWrite(file.c_str(), &package[0], (int32)package.size()))
			return Fail("The package could not be written to Steam Cloud.");
		const SteamAPICall_t call = StartUpdate(storage, item, file, note, description);
		if (call == 0)
		{
			storage->FileDelete(file.c_str());
			return Fail("Steam refused to make the update.");
		}
		// Steam may still be reading the cloud's copy, so it stays.
		if (!WaitForCall(utils, runCallbacks, call))
			return Fail("Steam did not finish the update in %d minutes: check the item's page.", kWaitMinutes);
		// The item keeps its own copy, so the cloud's would only use up space.
		storage->FileDelete(file.c_str());
		return CheckResult(utils, call, item, file);
	}
}

int main(int argc, char** argv)
{
	if (argc != 6)
	{
		printf("usage: upload <steam_api.dll> <item id> <package.civ5mod> <change note file> <description file>\n");
		return 2;
	}
	const PublishedFileId_t item = _strtoui64(argv[2], NULL, 10);
	std::vector<char> package;
	std::string note, description;
	if (!ReadBytes(argv[3], &package) || package.empty())
		return Fail("The package could not be read.");
	if (!ReadText(argv[4], &note) || !ReadText(argv[5], &description))
		return Fail("The change note or description could not be read.");

	// steam_api.dll reads the app it signs in as from here.
	SetEnvironmentVariableA("SteamAppId", kSdkApp);
	HMODULE api = LoadLibraryA(argv[1]);
	if (!api)
		return Fail("ModBuddy's steam_api.dll could not be loaded.");
	InitFn init = (InitFn)GetProcAddress(api, "SteamAPI_Init");
	VoidFn shutdown = (VoidFn)GetProcAddress(api, "SteamAPI_Shutdown");
	VoidFn runCallbacks = (VoidFn)GetProcAddress(api, "SteamAPI_RunCallbacks");
	InterfaceFn storageFn = (InterfaceFn)GetProcAddress(api, "SteamRemoteStorage");
	InterfaceFn utilsFn = (InterfaceFn)GetProcAddress(api, "SteamUtils");
	if (!init || !shutdown || !runCallbacks || !storageFn || !utilsFn)
		return Fail("steam_api.dll is not the one that comes with ModBuddy.");
	if (!init())
		return Fail("Steam could not be reached: start Steam and sign in to the account that owns the item.");
	ISteamRemoteStorage006* storage = (ISteamRemoteStorage006*)storageFn();
	ISteamUtils005* utils = (ISteamUtils005*)utilsFn();
	const int code = storage && utils
		? Upload(storage, utils, runCallbacks, item, FileName(argv[3]), package, note, description)
		: Fail("Steam gave no Workshop functions.");
	shutdown();
	return code;
}
