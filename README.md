# gc-wii-core

Current licensing and the grant for existing owner contributions are documented in [LICENSE-SCOPE.md](LICENSE-SCOPE.md).

Shared low-level machine/device source package extracted from `mstan/gcnllr`
commit `f07bfb9515082a964c36a4bbafb095dbb2d584b7`, directory
`third_party/lle/runtime`. Original runtime sources are unchanged. The initial
package retains the GameCube executable and reference GX renderer as compatible
consumers; it does not establish a working Wii machine.

Consumers add this directory through CMake. Supply `GC_WII_POWERPC_CORE_DIR`,
`GC_WII_IPL_TOOLS_DIR` and `GC_WII_TRACE_INCLUDE_DIR` when using a layout other
than gcnllr's. The IPL descrambler and trace-format header are included with
their original provenance. DSP instruction execution comes from the pinned `gc-dsp-core`
submodule; initialize submodules recursively.

The reusable low-level implementations cover memory/CPU glue, native REL
binding/invalidation, portable allocation/write epochs, interrupts,
GX/FIFO/VI, DSP registers, AI, disc, EXI, SI and persistence. Console composition,
memory-map extensions, firmware policy and host integration remain consumers'
responsibility. Wii-specific IOS, NAND, IPC and input integration is pending.

The package retains its GPL license and imported file notices. DSP ancestry is
documented in `dsp_lle/UPSTREAM.md`. Firmware, games, generated guest code and
native saves must never be committed.

The native Aurora GX service is in `native/gx`, with its consumer memory and
completion ABI in `include/gcnllr/native_gx.h`. Its donor dependency graph is
supplied by the consumer. Wii address fields are a separate build selection;
GameCube defaults and the software GX reference remain available. See the
service README for the explicit bank callbacks and focused GPU check.

CP initialization can select Wii FIFO address masks while the existing entry
point retains GameCube masks. Gather bursts resolve main-memory banks through
the shared bus, and consumers may supply an ordered drain hook. The extracted
audio-DMA register/pump functions serve both the legacy DSP wrapper and native
consumers, with explicit transfer periods and optional multi-bank CPU memory.

CPU DMAU/DMAL transfers use the locked-L1 backing and the ordinary memory bus,
including MEM2 when supplied by a Wii consumer. Split block lengths, the
zero-length 128-line encoding, direction and the self-clearing trigger are
modeled. Transfers complete synchronously; hardware queue latency is not
modeled. Writes retain native-code invalidation and reservation handling.

Private candidate default: native/HLE services. Root CMake requires the declared pinned native dependency targets; it never silently selects the reference provider. Existing accuracy sources remain behind the explicit `lle` build selection. See the paired qualification attachment for a real standalone component build.
