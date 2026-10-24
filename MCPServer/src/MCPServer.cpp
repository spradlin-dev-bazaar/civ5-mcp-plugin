#include "CvGameCoreDLLPCH.h"

#include "CivHooks.h"
#include "CivPluginApi.h"

#include <stdarg.h>
#include <stdio.h>

#define PLUGIN_EXPORT extern "C" __declspec(dllexport)

CIV_PLUGIN_EXPORT_API_VERSION;

static const CivHostInfo* s_host = 0;

// Writes a line to Logs\Bazaar.log, headed with this plugin's name.
static void Log(const char* format, ...)
{
	char line[512];
	va_list args;
	va_start(args, format);
	_vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
	va_end(args);
	s_host->Log(s_host->pluginIndex, line);
}

static void Check(int result, const char* hook)
{
	if (result != CIV_HOOK_OK)
		Log("could not register %s (result %d)", hook, result);
}

PLUGIN_EXPORT bool CivPluginEntry(const CivHostInfo* host, CivPluginInfo* plugin)
{
	s_host = host;
	plugin->name = "MCP Server for Bazaar";
	plugin->version = "1";
	return true;
}

PLUGIN_EXPORT void CivPluginShutdown()
{
	Log("shutting down");
}
