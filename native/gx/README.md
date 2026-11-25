# Native GX service

Extracted from `mstan/gcnllr` commit `c05553224c95ccffcab3336da14d9652592ddf08`,
`src/native_services` and `tools/aurora_retail.py`. Original MIT notices remain.
This library adapts the pinned donor Aurora dependencies; it does not link a
donor game executable. The package's software GX implementation remains the
low-level reference. Machine/FIFO policy and host loading remain consumers.

Include `attach.cmake` in the donor dependency build, with `GXRUNTIME_DIR`
and its Aurora/GX targets provided by that build. The graphics ABI is declared
in `include/gcnllr/native_gx.h`. ABI11 adds paired consumer memory resolution
and reverse-address callbacks, keeping physical identities in copy/epoch
callbacks. Consumers own memory maps, coherence, completion, scanout selection,
audio and persistent paths. The old single-MEM1 setup is retained for GameCube.

ABI12 appends focused, image-relative mouse state to the status result. Pointer
coordinates are normalized to 0..65535 inside the aspect-fitted image, accounting
for logical window units and physical pixel dimensions. Positions outside the
image or an unfocused/hidden window are invalid. Button bits are left/right/middle;
consumers map them to their own input devices. Rebuild consumers for the new ABI.

`GC_WII_GX_WII=ON` selects Wii texture/TLUT/copy address fields when building
the service. The default retains GameCube masks. `test_memory_gpu.py` exercises
real FIFO draws from MEM2 textures, MEM2 XFB writes, CPU rewrites and bank
isolation against that build. It requires `--library` and `--work-root` paths.

Extraction and multi-bank support require consumer build/runtime qualification;
the service test does not establish a working Wii machine or menu rendering.

Windows D3D12 consumers supply `GC_WII_DXC_RUNTIME_DIR` from their pinned Dawn package. Both DXC DLLs are materialized beside the service because Dawn loads them lazily; ordinary imported-DLL discovery does not include them.
