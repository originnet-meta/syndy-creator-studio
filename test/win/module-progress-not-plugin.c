#ifdef _WIN32
#define MODULE_EXPORT __declspec(dllexport)
#else
#define MODULE_EXPORT __attribute__((visibility("default")))
#endif

MODULE_EXPORT int module_progress_not_plugin_symbol(void)
{
	return 1;
}
