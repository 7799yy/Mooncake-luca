# MACA-derived LUCA Cross v2 transport

This branch replaces the original per-request synchronous LUCA transport with
an asynchronous implementation derived from Mooncake's
[MACA transport at 3aa0f716008c0cac25ffc82ef234d6b0a59e9c2e](https://github.com/kvcache-ai/Mooncake/blob/3aa0f716008c0cac25ffc82ef234d6b0a59e9c2e/mooncake-transfer-engine/src/transport/maca_transport/maca_transport.cpp).
It keeps the existing `protocol="luca"` interface and `luca-cross-v2` metadata
envelope. The supported scope remains one visible GPU per process, two processes
on the same host, and TP=1.

## What is reused and adapted

| MACA mechanism | This port |
| --- | --- |
| Task/slice submission and READ/WRITE endpoints | Retained; both submit entry points use one implementation |
| Dedicated nonblocking streams | One transport-owned stream for the single visible GPU |
| `auto` / `default` / `batchflag` copy selection | Retained, using native LUCA APIs and options below |
| `mcCopyFlag_t` and batch copy | `lcCopyFlag_t` / `lcExtBatchCopyFlagAndWaitV2` |
| Completion polling | Per-submission completion event, dispatched through `MultiTransport` |
| Shared allocation export and mapping reuse | One export per allocation; imports shared by logical buffers and overlapping in-flight submissions |
| Ordinary GPU IPC | Replaced by the already verified LUCA Cross v2 protocol |

Per-submission events prevent later work on a reused stream from delaying an
earlier task's completion. Imported mappings are closed after their last
in-flight user completes; they are not kept across idle periods. Export records
retain reference counts for distinct registered subranges of one allocation.
Logical registration boundaries remain separate from allocation base/size.

`MultiTransport` now stores the selected transport on each task and dispatches
status queries only for backends opting into `requiresStatusPolling()`. Other
backends retain their counter-based status path. A batch containing a polling
backend waits for all tasks to reach a terminal state before reporting failure,
so the synchronous wrapper cannot free a failed batch while another copy is
still in flight. Route selection is validated before appending new tasks.

## Copy options

Set these before installing the transport:

| Variable | Values / default |
| --- | --- |
| `MC_LUCA_COPY_API` | `auto` (default), `default`, `batchflag` |
| `MC_LUCA_BATCHFLAG_MIN_BYTES` | Nonnegative integer, default `1048576`; used by `auto` |
| `MC_LUCA_HOST_ID` | Optional existing host-identity override; otherwise the host boot ID |

`default` uses `lcMemcpyAsync(..., lcMemcpyDefault, stream)`. `batchflag` uses
`lcExtBatchCopyFlagAndWaitV2`. Auto mode preserves request order by batching
contiguous runs rather than moving every batchflag request after smaller copies.
Copies within one native batch run in parallel; callers must use independent
source/destination regions within that batch and synchronize conflicting work.
The SDK descriptor arrays stay alive until GPU completion. The threshold is a
selection policy, not an SDK minimum transfer size.

One local device synchronization remains before each nonempty submission as a
producer-ready barrier. The existing vLLM sender may run on an executor thread;
an event recorded on that thread's default stream cannot be assumed to cover
the actual model producer. Copies are then enqueued asynchronously and the
submission returns before completion. Integrating the actual producer event is
a separate optimization; this change makes no performance claim.

## Ownership and failures

- Requests are resolved into slices before enqueue; polling does not dereference
  the caller's request vector after submit returns.
- GPU buffers must remain allocated and unchanged as required by the operation
  until the batch is terminal. Batch submission, polling and freeing must be
  serialized for each BatchID, as required by the existing batch container.
- Peer allocations must remain valid until the operation finishes. Local
  unregister drains this transport's outstanding work; it does not coordinate
  other processes that may still be using an exported allocation.
- Copy errors, event errors and timeouts drain work before publishing failure.
  A failed drain retains contexts, descriptor arrays and imports, and leaves
  tasks unfinished so `freeBatchID` refuses them. A timeout is not cancellation.
- Failed native mapping closes are retained for another close attempt at
  transport teardown. Destroying a transport after an unrecoverable GPU drain
  failure quarantines native resources until process exit rather than unmapping
  memory potentially still in use.
- Duplicate registration and publication failures preserve prior registrations.
  Batch registration/unregistration failures roll back local metadata changes.
  Remote storage publication follows the existing metadata plugin's semantics.

The public batch and memory APIs have not changed. `TransferTask` and the
transport vtable have changed internally: rebuild the engine and its transfer
engine library together; do not combine these headers with an old prebuilt
transfer-engine library.

## Validation status

Native LUCA 3.7 tests preceding this port found ordinary IPC import returning 17
in the tested private-IPC, one-visible-GPU containers. Cross v2 with both async
and batchflag passed READ/WRITE, offset4096 and whole-allocation guards on GPU6/7.
Those tests establish the native capabilities, not this new backend's success.

Run the CPU-only lifecycle suite from the repository root:

```shell
python3 mooncake-transfer-engine/tests/luca_mock/run_tests.py
```

It builds the production LUCA transport, MultiTransport, slice/batch lifecycle
and Status sources against SDK and metadata dependency doubles. It requires a
host C++17 compiler, no GPU SDK or Python packages. The test suite covers delayed
completion, repeated polling, retained descriptor storage, overlapping batches,
READ/WRITE and guards, partial enqueue, failed drain, event errors, timeout,
registration rollback, direct submission and counter-only backend dispatch.
It is also registered as `luca_transport_mock_test` when unit tests and LUCA
are enabled.

The metadata double is a typed in-memory value store, not a JSON wire parser.
On Windows, the single-thread suite uses a test-only mutex fallback for MinGW
without gthreads and a process-lifetime slice cache to avoid that runtime's
reproduced TLS-destructor crash. Linux uses the standard mutex and production
thread-local cache unchanged. This suite does not validate native ABI, real
metadata publication, GPU behavior, thread isolation or performance.

The new backend still needs a complete private-overlay build against the real
SDK, TE GPU READ/WRITE and failure tests, and real vLLM PD plus LMCache cold/warm
regression with the newly built engine identity verified. Keep CPU Store traffic
on TCP. Existing experimental tracers expecting synchronous `lcMemcpy` counts
must be adapted to `lcMemcpyAsync` and batchflag descriptors; do not reuse an old
engine hash or treat enqueue counts as completed bytes.
