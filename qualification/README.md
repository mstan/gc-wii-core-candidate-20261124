# Native/HLE component build

Use `qualification/attach.cmake` as CMAKE_PROJECT_TOP_LEVEL_INCLUDES in the pinned BlueWake Windows dependency graph. Supply SHARED_GC_WII_CORE_DIR, SHARED_GC_DSP_CORE_DIR, GC_DSP_DONOR_HOST_DIR and BLUEWAKE_RECOMPCORE. Build `gcnllr-native-gx` and `shared_core_dsp_mail`. Consumers supply dependencies; no donor game executable is built. Native GX has synthetic FIFO/bank fixtures in native/gx/test_memory_gpu.py. Neither firmware boot nor console integration is claimed.
