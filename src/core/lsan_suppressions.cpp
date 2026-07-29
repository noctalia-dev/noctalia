// __lsan_default_suppressions() is called automatically by LSAN at startup.
extern "C" const char* __lsan_default_suppressions() {
  // PipeWire unloads SPA modules through atexit after LSAN checks for leaks.
  // ROCm SMI retains its logger singleton after shutdown and is already unloaded when LSAN reports it.
  return "leak:pw_context_load_module\n"
         "leak:pw_context_new\n"
         "leak:SystemMonitorService::AmdRsmiReader::ensureReady\n";
}
