# M0: preserve the MACA mother before tuning LUCA

Mother: Mooncake `3aa0f716008c0cac25ffc82ef234d6b0a59e9c2e`,
`mooncake-transfer-engine/src/transport/maca_transport/maca_transport.cpp`.
Base: the original LUCA experiment at `95d8604aea2417a0930763543fa5226c32714ed3`.
Branch: `feat/luca-maca-minimal`. The earlier `feat/luca-maca-derived` and its
completed functional evidence remain separate comparison material.

## Rules for this attempt

Keep the mother's class layout, thread-local per-device stream/event pools,
caller `host`/`wait` modes, stream-query completion, persistent IPC mapping
cache, synchronous direct submit, asynchronous task submit, copy selection and
first-error behavior. No Pending/PImpl, per-submit completion event,
device-wide producer barrier, idle mapping close, new registration transaction
or drain-before-terminal state machine is introduced.

Any later change must identify the observed problem, its evidence, the smallest
change and the validation to perform. A passing functional sample does not
justify unrelated restructuring. Do not publish or deploy this experiment as
an already validated replacement.

## Deviations from the mother, stated before node testing

| Deviation | Reason and scope |
| --- | --- |
| Class, protocol and `MC_MACA_*` names become LUCA | Keep a distinct transport and its configuration |
| `luca_compat.h` includes the installed LUCA CUDA compatibility headers | Retain the mother's CUDA-spelled stream/copy/event code; no reconstructed vendor API header |
| Native `lcCopyFlag_t` and `lcExtBatchCopyFlagAndWaitV2` | Use the actual SDK batch descriptor; native return codes avoid cross-enum casts |
| Ordinary IPC replaced by native Cross v2 Get/Open/Close | Ordinary IPC failed in the already tested isolated-container setup; Cross v2 native primitives passed |
| Existing `luca-cross-v2` envelope and logical buffer bounds retained | Preserve this base's registered-range contract and checked allocation-base relocation; no new Pending/refcount design |
| Registration deduplicates logical addresses, while imports remain cached by peer/allocation base | Support multiple registered subranges without losing logical bounds; exports are not given the earlier derived branch's allocation cache |
| Existing metadata update flag is honored | Allow this base's batch register API to publish once |
| Old `Slice.local` gains `cuda_stream`; task gains `transport_`; framework delegates status | The mother needs stream-query completion, and this older framework lacks those fields and delegation |
| Explicit standard includes and verbatim upstream PairHash helper | Replace missing transitive/header dependencies in the older base |

The framework retains its original batch failure/timeout policy. No opt-in
`requiresStatusPolling()` or wait-all-before-failure change is carried over.
The direct entry remains synchronous; only task submission uses copy modes.
Single-visible-GPU containers are the initial test scope, even though the
mother's device enumeration code remains present.

## Local checks and their limits

Run from the repository root:

```shell
python3 mooncake-transfer-engine/tests/luca_maca_minimal/run_tests.py
```

Six independent CPU-double processes cover default/batchflag/auto combined with
host/wait caller sync. Checks include READ/WRITE and guards, offset4096, two
logical registrations sharing an allocation, old-framework polling, persistent
mapping reuse, synchronous direct submit and stream-level completion behavior.
They do not replace the earlier branch's lifecycle tests, whose expectations
describe a different implementation.

The batch SDK double assumes descriptors are consumed during the enqueue call,
matching a possible contract for the mother's local descriptor vectors. This
assumption is NOT established SDK behavior. Native TE batch tests must confirm
it before model testing; do not introduce descriptor-retention restructuring
before observing and explaining such a need.

Windows host tests disable TLS and use a test-only single-thread mutex shim for
the available MinGW runtime. Production TLS/pools remain unchanged. These tests
do not establish vendor ABI, real producer ordering, concurrency or performance.

## Incremental node sequence

1. Build/import against the existing SDK in a new private directory of the
   running build container. Do not initialize an engine or alter the Store.
2. With fresh GPU6/7 resource checks, run small real TE tests for READ/WRITE,
   suballocation and guards, all copy modes and host/wait. First inspect actual
   native symbols and verify descriptor lifetime after submit returns.
3. Test caller synchronization with the real producer dependency. Stop at any
   mismatch and record a focused follow-up change; do not add a device-wide
   barrier preemptively.
4. Only after TE checks pass, test pure PD and then PD plus LMCache cold/warm,
   using the same new engine hash. Keep the independent CPU Store on TCP.

Real SDK compilation, native GPU tests and model tests are pending for M0.
The older derived branch's successful engine hash is not an M0 result. The
mother's error/timeout and persistent-cache limitations remain; this attempt
does not claim cancellation, fault recovery or peer-restart support.
